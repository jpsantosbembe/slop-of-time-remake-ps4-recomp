"""Gera a arte do app de PS4 (candidatos a icon0 / pic0 / pic1) a partir da cópia do jogo do usuário.

Nada de arte do jogo vai para o repositório: este script lê o RomFS empacotado do dump local (romfs.bin + index.json,
o diretório que prep_romfs_linux.sh produz em ext/ocarina-recomp/build/romfs) e grava PNGs na pasta de saída.

    venv/bin/python -I tools/gerar_arte.py --romfs ~/ocarina-ps4/ext/ocarina-recomp/build/romfs \
        --out ~/ocarina-ps4/arte/candidatos [--folha folha.png]

Fontes usadas:
  - UI/Layout2Archive/Title.Product.100.Nin_Ounce_NVN2.bntx.zs: logos do título por região (ASTC 4x4, alfa).
  - UI/Tex/Icon/*.bntx.zs: ícones de item (ASTC 4x4, 341x341).
  - UI/Layout2Archive/Common.Product.100.Nin_Ounce_NVN2.bntx.zs: mapa-múndi (4608x3360).
  - Event/Movie/Dm_Title_*.webm: vídeos da tela de título (AV1 1920x1080), lidos direto do romfs.bin pelo
    protocolo subfile do ffmpeg (sem copiar). Precisa de ffmpeg/ffprobe com AV1 no PATH.

Dependências (venv): zstandard não é necessário (usa compression.zstd do Python 3.14), Pillow, numpy,
texture2ddecoder.
"""
import argparse
import subprocess
import sys
import tempfile
from pathlib import Path

from PIL import Image, ImageDraw, ImageFilter, ImageFont

sys.path.insert(0, str(Path(__file__).resolve().parent))
import bntx            # noqa: E402
import romfs_read      # noqa: E402

TITLE_BNTX = "UI/Layout2Archive/Title.Product.100.Nin_Ounce_NVN2.bntx.zs"
COMMON_BNTX = "UI/Layout2Archive/Common.Product.100.Nin_Ounce_NVN2.bntx.zs"
ICON = "UI/Tex/Icon/{}.bntx.zs"
MOVIE = "Event/Movie/Dm_Title_{}.webm"

BG = (18, 22, 40)

# (arquivo de saída, vídeo, fração da duração)
FRAMES = [
    ("07_splash_0005_link_epona_lua_1920x1080.png", "0005", 0.15),
    ("08_splash_0005_link_epona_castelo_1920x1080.png", "0005", 0.30),
    ("09_splash_0045_montanha_da_morte_1920x1080.png", "0045", 0.60),
    ("10_splash_0055_rancho_lon_lon_1920x1080.png", "0055", 0.30),
    ("11_splash_0065_ruinas_floresta_1920x1080.png", "0065", 0.60),
    ("12_splash_0075_deserto_gerudo_1920x1080.png", "0075", 0.45),
    ("13_splash_0095_lago_hylia_1920x1080.png", "0095", 0.30),
]


def movie_frame(rom, romfs_dir, code, frac):
    off, size = rom.span(MOVIE.format(code))
    src = f"subfile,,start,{off},end,{off + size},,:{Path(romfs_dir) / 'romfs.bin'}"
    dur = float(subprocess.check_output(["ffprobe", "-v", "error", "-show_entries", "format=duration",
                                         "-of", "csv=p=0", src], text=True).strip())
    with tempfile.TemporaryDirectory() as td:
        out = Path(td) / "f.png"
        subprocess.check_call(["nice", "-n", "10", "ffmpeg", "-nostdin", "-v", "error", "-ss", f"{dur * frac:.3f}",
                               "-i", src, "-frames:v", "1", "-y", str(out)])
        return Image.open(out).convert("RGB")


def texture(rom, path, name=None):
    b = rom.read(path)
    ts = bntx.textures(b)
    t = ts[0] if name is None else next(t for t in ts if t["name"] == name)
    return bntx.decode(b, t)


def fit(im, box):
    im = im.copy()
    im.thumbnail(box, Image.LANCZOS)
    return im


def on_background(fg, size, bg=None, scale=0.92, glow=True):
    """fg (RGBA) centralizado sobre bg (RGB) ou sobre um degradê escuro, no tamanho size."""
    w, h = size
    if bg is None:
        canvas = Image.new("RGBA", size, BG + (255,))
        grad = Image.radial_gradient("L").resize(size)
        light = Image.new("RGBA", size, (52, 64, 104, 255))
        canvas = Image.composite(canvas, light, grad)
    else:
        canvas = bg.convert("RGBA")
    f = fit(fg, (int(w * scale), int(h * scale)))
    if f.width < w * scale * 0.98 and f.height < h * scale * 0.98:   # textura menor que o alvo: amplia
        k = min(w * scale / f.width, h * scale / f.height)
        f = fg.resize((int(fg.width * k), int(fg.height * k)), Image.LANCZOS)
    pos = ((w - f.width) // 2, (h - f.height) // 2)
    if glow:
        a = f.getchannel("A").filter(ImageFilter.GaussianBlur(max(4, w // 60)))
        shadow = Image.new("RGBA", f.size, (0, 0, 0, 0))
        shadow.putalpha(a.point(lambda v: v * 0.7))
        canvas.alpha_composite(shadow, pos)
    canvas.alpha_composite(f, pos)
    return canvas.convert("RGB")


def cover(im, size):
    """Recorta e redimensiona im para preencher size (como CSS object-fit: cover)."""
    w, h = size
    k = max(w / im.width, h / im.height)
    r = im.resize((round(im.width * k), round(im.height * k)), Image.LANCZOS)
    x, y = (r.width - w) // 2, (r.height - h) // 2
    return r.crop((x, y, x + w, y + h))


def darken(im, f=0.65):
    return Image.eval(im, lambda v: int(v * f))


def folha(out_dir, dest):
    files = sorted(p for p in Path(out_dir).glob("[0-9][0-9]_*.png"))
    cell, cols = 360, 5
    rows = (len(files) + cols - 1) // cols
    sheet = Image.new("RGB", (cell * cols, (cell + 40) * rows), (24, 24, 32))
    d = ImageDraw.Draw(sheet)
    font = small = None
    for bold, reg in (("DejaVuSans-Bold.ttf", "DejaVuSans.ttf"),
                      ("/usr/share/fonts/google-droid-sans-fonts/DroidSans-Bold.ttf",
                       "/usr/share/fonts/google-droid-sans-fonts/DroidSans.ttf")):
        try:
            font, small = ImageFont.truetype(bold, 26), ImageFont.truetype(reg, 14)
            break
        except OSError:
            pass
    if font is None:
        font, small = ImageFont.load_default(26), ImageFont.load_default(14)
    for i, p in enumerate(files):
        im = Image.open(p)
        x, y = (i % cols) * cell, (i // cols) * (cell + 40)
        flat = Image.new("RGBA", im.size, (24, 24, 32, 255))
        flat.alpha_composite(im.convert("RGBA"))   # alfa sobre fundo escuro, para logos transparentes
        th = fit(flat.convert("RGB"), (cell - 16, cell - 16))
        sheet.paste(th, (x + (cell - th.width) // 2, y + 8 + (cell - 16 - th.height) // 2))
        num = p.name[:2].lstrip("0")
        d.rectangle((x + 6, y + 6, x + 52, y + 40), fill=(255, 200, 0))
        d.text((x + 12, y + 8), num, fill=(0, 0, 0), font=font)
        d.text((x + 8, y + cell + 4), f"{p.name[3:-4][:44]}", fill=(220, 220, 220), font=small)
        d.text((x + 8, y + cell + 20), f"{im.width}x{im.height}", fill=(160, 160, 160), font=small)
    sheet.save(dest)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--romfs", required=True, help="diretório com romfs.bin e index.json")
    ap.add_argument("--out", required=True, help="pasta de saída dos PNG")
    ap.add_argument("--folha", help="grava também uma folha de contato numerada neste caminho")
    a = ap.parse_args()
    out = Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    rom = romfs_read.Romfs(a.romfs)

    def save(im, name):
        im.save(out / name, optimize=True)
        print(f"{name}  {im.width}x{im.height}")

    # Logos do título (Title.bntx) e um quadro do vídeo de título, usado também como fundo.
    tb = rom.read(TITLE_BNTX)
    tex = {t["name"]: t for t in bntx.textures(tb)}
    logo_us = bntx.decode(tb, tex["Logo_USEU_00"])
    logo_jp = bntx.decode(tb, tex["Logo_JPja_00"])
    logo_us_bbox = logo_us.crop(logo_us.getbbox())
    logo_jp_bbox = logo_jp.crop(logo_jp.getbbox())
    field = movie_frame(rom, a.romfs, "0005", 0.30)

    save(on_background(logo_us_bbox, (512, 512)), "01_icon_logo_USEU_512.png")
    save(on_background(logo_us_bbox, (512, 512), bg=darken(cover(field, (512, 512)), 0.55)),
         "02_icon_logo_USEU_sobre_campo_512.png")
    save(on_background(logo_jp_bbox, (512, 512)), "03_icon_logo_JPja_512.png")
    save(on_background(texture(rom, ICON.format("Weapon_Shield_Hylian")), (512, 512), scale=0.88),
         "04_icon_escudo_hylian_512.png")
    save(on_background(texture(rom, ICON.format("Item_Ocarina_Time")), (512, 512), scale=0.9),
         "05_icon_ocarina_do_tempo_512.png")
    save(on_background(logo_us_bbox, (1920, 1080), bg=darken(field, 0.8), scale=0.62),
         "06_splash_logo_sobre_campo_1920x1080.png")

    for name, code, frac in FRAMES:
        save(movie_frame(rom, a.romfs, code, frac), name)

    save(cover(texture(rom, COMMON_BNTX, "MapTexWorldAll_00").convert("RGB"), (1920, 1080)),
         "14_fundo_mapa_hyrule_1920x1080.png")
    logo_us.save(out / "15_logo_USEU_original_1664x1280.png", optimize=True)
    print("15_logo_USEU_original_1664x1280.png  1664x1280")

    if a.folha:
        folha(out, a.folha)
        print("folha:", a.folha)


if __name__ == "__main__":
    main()
