"""Gera a arte do PKG (icon0.png, pic0.png, pic1.png) a partir da cópia do jogo de quem builda, seguindo a receita do
jogo em jogos/<title id>/arte.json. Nada aqui conhece um jogo: caminhos de asset e parâmetros vêm da receita, e as
imagens, do RomFS. As imagens geradas nunca vão para o repositório.

    python -I arte_pkg.py --romfs <dir com romfs.bin e index.json> --receita <arte.json> --out <dir>

Receita: icon0 e pic, cada um com "tamanho", "fundo" (um quadro de vídeo do RomFS: "video", "ponto" em fração da
duração, "escurecer" opcional) e "logo" (uma textura BNTX: "bntx", "textura", "recortar" a borda transparente),
posicionado por "posicao": {"modo": "centro", "escala", "brilho"} ou {"modo": "canto", "largura", "direita", "topo"}.
Precisa de ffmpeg com AV1 e dos módulos bntx.py e romfs_read.py (mesma pasta), com numpy, Pillow e texture2ddecoder.
"""
import argparse
import json
import subprocess
import sys
import tempfile
from pathlib import Path

from PIL import Image, ImageFilter

sys.path.insert(0, str(Path(__file__).resolve().parent))
import bntx         # noqa: E402
import romfs_read   # noqa: E402


def video_frame(rom, romfs_dir, path, frac):
    off, size = rom.span(path)
    src = f"subfile,,start,{off},end,{off + size},,:{Path(romfs_dir) / 'romfs.bin'}"
    dur = float(subprocess.check_output(["ffprobe", "-v", "error", "-show_entries", "format=duration", "-of", "csv=p=0",
                                         src], text=True).strip())
    with tempfile.TemporaryDirectory() as td:
        out = Path(td) / "f.png"
        subprocess.check_call(["nice", "-n", "10", "ffmpeg", "-nostdin", "-v", "error", "-ss", f"{dur * frac:.3f}",
                               "-i", src, "-frames:v", "1", "-y", str(out)])
        return Image.open(out).convert("RGB")


def texture(rom, path, name):
    b = rom.read(path)
    t = next(t for t in bntx.textures(b) if t["name"] == name)
    return bntx.decode(b, t).convert("RGBA")


def cover(im, size):
    w, h = size
    k = max(w / im.width, h / im.height)
    r = im.resize((round(im.width * k), round(im.height * k)), Image.LANCZOS)
    x, y = (r.width - w) // 2, (r.height - h) // 2
    return r.crop((x, y, x + w, y + h))


def compose(rom, romfs_dir, spec):
    size = tuple(spec["tamanho"])
    f = spec["fundo"]
    bg = cover(video_frame(rom, romfs_dir, f["video"], f["ponto"]), size)
    if f.get("escurecer"):
        k = f["escurecer"]
        bg = Image.eval(bg, lambda v: int(v * k))
    canvas = bg.convert("RGBA")
    lg = spec["logo"]
    logo = texture(rom, lg["bntx"], lg["textura"])
    if lg.get("recortar"):
        logo = logo.crop(logo.getbbox())
    p = spec["posicao"]
    if p["modo"] == "centro":
        s = p.get("escala", 0.9)
        k = min(size[0] * s / logo.width, size[1] * s / logo.height)
        logo = logo.resize((int(logo.width * k), int(logo.height * k)), Image.LANCZOS)
        pos = ((size[0] - logo.width) // 2, (size[1] - logo.height) // 2)
        if p.get("brilho"):
            a = logo.getchannel("A").filter(ImageFilter.GaussianBlur(max(4, size[0] // 60)))
            shadow = Image.new("RGBA", logo.size, (0, 0, 0, 0))
            shadow.putalpha(a.point(lambda v: v * 0.7))
            canvas.alpha_composite(shadow, pos)
    elif p["modo"] == "canto":
        w = p["largura"]
        logo = logo.resize((w, round(logo.height * w / logo.width)), Image.LANCZOS)
        pos = (size[0] - w - p["direita"], p["topo"])
    else:
        sys.exit(f"arte_pkg: posição desconhecida {p['modo']}")
    canvas.alpha_composite(logo, pos)
    return canvas.convert("RGB")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--romfs", required=True)
    ap.add_argument("--receita", required=True, type=Path)
    ap.add_argument("--out", required=True, type=Path)
    a = ap.parse_args()
    receita = json.loads(a.receita.read_text())
    a.out.mkdir(parents=True, exist_ok=True)
    rom = romfs_read.Romfs(a.romfs)
    compose(rom, a.romfs, receita["icon0"]).save(a.out / "icon0.png", optimize=True)
    pic = compose(rom, a.romfs, receita["pic"])
    pic.save(a.out / "pic0.png", optimize=True)
    pic.save(a.out / "pic1.png", optimize=True)
    print(f"arte do PKG em {a.out}: icon0.png, pic0.png, pic1.png")


if __name__ == "__main__":
    main()
