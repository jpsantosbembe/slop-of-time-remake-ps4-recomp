"""Decodificador mínimo de BNTX (Switch 1 e Switch 2 "Ounc"): lista texturas e decodifica o mip 0, camada 0.

Layout block-linear (GOB 64 B x 8 linhas, blocos de 2^bhl GOBs empilhados). Dentro do GOB, a ordem dos bits do
endereço no BNTX do Switch 2 ("Ounc") é x[0:4], y0, y1, x4, y2, x5 (o Switch 1 usa x[0:4], y0, x4, y1, y2, x5);
achado medindo a continuidade dos blocos de várias texturas; confere em ASTC (16 B) e BC4 (8 B). Formatos: RGBA8/BGRA8, BC1-BC5, BC7, ASTC.
"""
import struct
import numpy as np
import texture2ddecoder as t2d
from PIL import Image

# tipo do formato -> (bloco w, bloco h, bytes por bloco, decodificador)
ASTC = {0x2d: (4, 4), 0x2e: (5, 4), 0x2f: (5, 5), 0x30: (6, 5), 0x31: (6, 6), 0x32: (8, 5), 0x33: (8, 6),
        0x34: (8, 8), 0x35: (10, 5), 0x36: (10, 6), 0x37: (10, 8), 0x38: (10, 10), 0x39: (12, 10), 0x3a: (12, 12)}
FMT = {0x02: (1, 1, 1, "r8"), 0x09: (1, 1, 2, "rg8"), 0x0b: (1, 1, 4, "rgba"), 0x0c: (1, 1, 4, "bgra"),
       0x1a: (4, 4, 8, t2d.decode_bc1), 0x1c: (4, 4, 16, t2d.decode_bc3),
       0x1d: (4, 4, 8, t2d.decode_bc4), 0x1e: (4, 4, 16, t2d.decode_bc5), 0x20: (4, 4, 16, t2d.decode_bc7)}
for k, (bw, bh) in ASTC.items():
    FMT[k] = (bw, bh, 16, "astc")


def _name(b, off):
    n = struct.unpack_from("<H", b, off)[0]
    return b[off + 2:off + 2 + n].decode("utf-8", "replace")


def textures(b):
    """Lista de dicts com nome, w, h, formato, block height e offset dos dados do mip 0."""
    assert b[:4] == b"BNTX", "não é BNTX"
    count, info_arr = struct.unpack_from("<IQ", b, 0x24)
    out = []
    for i in range(count):
        o = struct.unpack_from("<Q", b, info_arr + 8 * i)[0]
        assert b[o:o + 4] == b"BRTI"
        tile = struct.unpack_from("<H", b, o + 0x12)[0]
        fmt = struct.unpack_from("<I", b, o + 0x1c)[0]
        w, h, d, layers, bhl = struct.unpack_from("<IIIII", b, o + 0x24)
        size = struct.unpack_from("<I", b, o + 0x50)[0]
        name_off, _, mips_ptr = struct.unpack_from("<QQQ", b, o + 0x60)
        data = struct.unpack_from("<Q", b, mips_ptr)[0]
        out.append(dict(name=_name(b, name_off), w=w, h=h, fmt=fmt, tile=tile, bhl=bhl, layers=layers,
                        size=size, data=data))
    return out


def _deswizzle(src, wb, hb, bpb, bhl):
    """Block-linear -> linear. wb/hb em blocos de formato, bpb bytes por bloco."""
    gob_h = 1 << bhl
    row_bytes = wb * bpb
    gobs_x = (row_bytes + 63) // 64
    x = np.arange(gobs_x * 64)[None, :]
    y = np.arange(hb)[:, None]
    addr = ((y // (8 * gob_h)) * 512 * gob_h * gobs_x + (x // 64) * 512 * gob_h + ((y % (8 * gob_h)) // 8) * 512
            + ((x % 64) // 32) * 256 + ((y % 8) // 4) * 128 + ((x % 32) // 16) * 64 + ((y % 4) // 2) * 32
            + (y % 2) * 16 + (x % 16))
    addr = addr[:, :row_bytes]
    s = np.frombuffer(src, np.uint8)
    need = int(addr.max()) + 1
    if len(s) < need:
        s = np.concatenate([s, np.zeros(need - len(s), np.uint8)])
    return s[addr].tobytes()


def decode(b, t):
    """PIL RGBA do mip 0 da textura t (dict de textures())."""
    ftype = t["fmt"] >> 8
    if ftype not in FMT:
        raise ValueError(f"formato 0x{t[fmt]:04x} não suportado")
    bw, bh, bpb, dec = FMT[ftype]
    w, h = t["w"], t["h"]
    wb, hb = (w + bw - 1) // bw, (h + bh - 1) // bh
    src = b[t["data"]:t["data"] + t["size"]]
    lin = src[:wb * hb * bpb] if t["tile"] == 1 else _deswizzle(src, wb, hb, bpb, t["bhl"])
    if dec == "rgba":
        return Image.frombytes("RGBA", (w, h), lin)
    if dec == "bgra":
        r, g, bb, a = Image.frombytes("RGBA", (w, h), lin).split()
        return Image.merge("RGBA", (bb, g, r, a))
    if dec == "r8":
        return Image.frombytes("L", (w, h), lin).convert("RGBA")
    if dec == "rg8":
        a = np.frombuffer(lin, np.uint8).reshape(h, w, 2)
        rgba = np.dstack([a[..., 0], a[..., 1], np.zeros_like(a[..., 0]), np.full_like(a[..., 0], 255)])
        return Image.fromarray(rgba, "RGBA")
    pw, ph = wb * bw, hb * bh
    if dec == "astc":
        raw = t2d.decode_astc(lin, pw, ph, bw, bh)
    else:
        raw = dec(lin, pw, ph)
    im = Image.frombytes("RGBA", (pw, ph), raw, "raw", "BGRA")
    return im.crop((0, 0, w, h))
