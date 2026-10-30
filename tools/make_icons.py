"""Builds every app icon from assets/logo.png (blue wind swirl on white). Needs Pillow.

    python tools/make_icons.py

Windows: windows/res/app.ico (swirl on a white rounded square, like the phone launcher) and
app_off.ico (greyed, tray when switched off). installer/wizard*.bmp for the setup wizard.
Android: launcher PNGs per density (adaptive foreground + legacy), and a white alpha-mask
glyph for the Quick Settings tile and notifications.
"""
from pathlib import Path

from PIL import Image, ImageDraw, ImageFilter, ImageOps

ROOT = Path(__file__).resolve().parent.parent
SRC = ROOT / "assets/logo.png"
RES = ROOT / "android/app/src/main/res"


def swirl() -> Image.Image:
    """The swirl on a transparent background, cropped square around it."""
    im = Image.open(SRC).convert("RGB")
    # Alpha from "distance to white": the logo is a single blue on white, so the darkest channel
    # tells coverage, which keeps the anti-aliased edges smooth.
    gray = ImageOps.grayscale(im)
    alpha = ImageOps.invert(gray).point(lambda v: 0 if v < 12 else min(255, v * 255 // 120))  # drop off-white noise
    rgba = Image.new("RGBA", im.size, (0x0E, 0x7C, 0xD4, 0))
    rgba.putalpha(alpha)
    box = alpha.getbbox()
    w, h = box[2] - box[0], box[3] - box[1]
    side = max(w, h)
    cx, cy = (box[0] + box[2]) // 2, (box[1] + box[3]) // 2
    return rgba.crop((cx - side // 2, cy - side // 2, cx + side // 2, cy + side // 2))


def padded(img: Image.Image, size: int, fill: float) -> Image.Image:
    """img scaled so its side is `fill` of a size x size transparent canvas, centred."""
    inner = max(1, round(size * fill))
    out = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    out.alpha_composite(img.resize((inner, inner), Image.LANCZOS), ((size - inner) // 2, (size - inner) // 2))
    return out


def tile(logo: Image.Image, size: int, bg, fg=None) -> Image.Image:
    """Logo on a rounded square filling the whole icon. Drawn at 4x and scaled down for clean edges."""
    big = size * 4
    out = Image.new("RGBA", (big, big), (0, 0, 0, 0))
    mask = Image.new("L", (big, big), 0)
    ImageDraw.Draw(mask).rounded_rectangle((0, 0, big - 1, big - 1), radius=round(big * 0.22), fill=255)
    out.paste(Image.new("RGBA", (big, big), bg + (255,)), (0, 0), mask)
    # Small icons: thin strokes vanish, so the swirl gets more of the square and a touch of weight.
    fill = 0.90 if size <= 24 else 0.84 if size <= 48 else 0.78
    mark = padded(logo, big, fill)
    if size <= 32:
        mark = bolder(mark, max(1, big // 100))
    if fg:
        mark = tinted(mark, fg)
    out.alpha_composite(mark)
    return out.resize((size, size), Image.LANCZOS)


def bolder(img: Image.Image, px: int) -> Image.Image:
    """Thickens the strokes by about px pixels (alpha dilation)."""
    out = img.copy()
    out.putalpha(img.getchannel("A").filter(ImageFilter.MaxFilter(px * 2 + 1)))
    return out


def tinted(img: Image.Image, rgb) -> Image.Image:
    out = Image.new("RGBA", img.size, rgb + (0,))
    out.putalpha(img.getchannel("A"))
    return out


def main():
    logo = swirl()

    # Windows: icon sizes Explorer, the Start menu and the tray ask for.
    sizes = [16, 20, 24, 32, 40, 48, 64, 128, 256]
    on = [tile(logo, s, (255, 255, 255)) for s in sizes]
    off = [tile(logo, s, (0xD6, 0xD6, 0xD6), (0x86, 0x86, 0x86)) for s in sizes]
    on[-1].save(ROOT / "windows/res/app.ico", sizes=[(s, s) for s in sizes], append_images=on[:-1])
    off[-1].save(ROOT / "windows/res/app_off.ico", sizes=[(s, s) for s in sizes], append_images=off[:-1])

    # Setup wizard images (Inno Setup): the large one on the welcome page, the small one top-right.
    inst = ROOT / "installer"
    inst.mkdir(exist_ok=True)
    big = Image.new("RGB", (164, 314), (255, 255, 255))
    big.paste(padded(logo, 140, 1.0), (12, 87), padded(logo, 140, 1.0))
    big.save(inst / "wizard.bmp")
    small = Image.new("RGB", (55, 55), (255, 255, 255))
    small.paste(padded(logo, 51, 1.0), (2, 2), padded(logo, 51, 1.0))
    small.save(inst / "wizard-small.bmp")

    # Android launcher. Adaptive foreground is 108dp with a 66dp safe zone; legacy is 48dp.
    dens = {"mdpi": 1, "hdpi": 1.5, "xhdpi": 2, "xxhdpi": 3, "xxxhdpi": 4}
    for name, k in dens.items():
        d = RES / f"mipmap-{name}"
        d.mkdir(parents=True, exist_ok=True)
        padded(logo, round(108 * k), 0.64).save(d / "ic_launcher_fg.png")
        legacy = Image.new("RGBA", (round(48 * k),) * 2, (255, 255, 255, 0))
        circle = Image.new("L", legacy.size, 0)
        ImageDraw.Draw(circle).ellipse((0, 0, legacy.size[0] - 1, legacy.size[1] - 1), fill=255)
        white = Image.new("RGBA", legacy.size, (255, 255, 255, 255))
        legacy.paste(white, (0, 0), circle)
        legacy.alpha_composite(padded(logo, legacy.size[0], 0.8))
        legacy.save(d / "ic_launcher.png")

    # Tile + notification glyph: white silhouette, the system tints it.
    for name, k in dens.items():
        d = RES / f"drawable-{name}"
        d.mkdir(parents=True, exist_ok=True)
        # The swirl is wide and short, so it spans the full width to look as big as other tiles' icons.
        side = round(24 * k)
        glyph = bolder(padded(logo, side * 4, 1.0), max(1, side * 4 // 160))
        tinted(glyph.resize((side, side), Image.LANCZOS), (255, 255, 255)).save(d / "ic_windrop_glyph.png")
    print("icons written")


if __name__ == "__main__":
    main()
