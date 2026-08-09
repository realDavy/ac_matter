#!/usr/bin/env python3
"""Composite firmware-accurate round-screen UI onto the official product render.

Pipeline:
  1. Load official studio photo (docs/product_studio_user.jpg)
  2. Replace ONLY nested circular display pixels (housing untouched)
  3. Square-pad to a common 900×900 plate so figure rows stay level

UI content is laid out inside a safe inset so controls do not crowd or clip the
circular bezel (fixes “画面跑出屏幕”).
"""

from __future__ import annotations

from pathlib import Path

import qrcode
from PIL import Image, ImageDraw, ImageFilter, ImageFont

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "img" / "manual"
DOCS = ROOT / "docs"
FONT_PATH = Path("/usr/share/fonts/truetype/wqy/wqy-microhei.ttc")

# Calibrated on docs/product_studio_user.jpg (790×753) — nested black aperture.
SCX, SCY, SR = 386, 540, 108

# Content must stay well inside the circular aperture (avoid edge clipping).
SAFE = 0.60

CANVAS = 900
BG = (236, 236, 236)


def font(size: int) -> ImageFont.FreeTypeFont:
    return ImageFont.truetype(str(FONT_PATH), size=size, index=0)


def text_center(d, xy, text, f, fill) -> None:
    b = d.textbbox((0, 0), text, font=f)
    tw, th = b[2] - b[0], b[3] - b[1]
    d.text((xy[0] - tw / 2, xy[1] - th / 2), text, font=f, fill=fill)


def rounded(d, box, r, fill) -> None:
    d.rounded_rectangle(box, radius=r, fill=fill)


def square_pad(
    img: Image.Image,
    size: int = CANVAS,
    bg=BG,
    fill: float = 0.86,
) -> Image.Image:
    """Fit image into a square canvas, centered — keeps figure rows level.

    `fill` is the fraction of the canvas the content may occupy; using the same
    fill for every plate keeps devices visually the same size in figure rows.
    """
    canvas = Image.new("RGB", (size, size), bg)
    scale = min((size * fill) / img.width, (size * fill) / img.height)
    nw, nh = max(1, int(img.width * scale)), max(1, int(img.height * scale))
    im = img.resize((nw, nh), Image.Resampling.LANCZOS)
    canvas.paste(im, ((size - nw) // 2, (size - nh) // 2))
    return canvas


def studio_source() -> Image.Image:
    src = DOCS / "product_studio_user.jpg"
    if not src.is_file():
        src = OUT / "product_studio_user.jpg"
    return Image.open(src).convert("RGB")


def hero_source() -> Image.Image:
    src = DOCS / "product_hero_user.jpg"
    if not src.is_file():
        src = OUT / "product_hero_user.jpg"
    return Image.open(src).convert("RGB")


def make_screen(size: int, kind: str) -> Image.Image:
    """Draw UI on a square, then circular-mask. Layout uses SAFE inset."""
    im = Image.new("RGB", (size, size), (10, 12, 16))
    d = ImageDraw.Draw(im)
    for y in range(size):
        t = y / max(size - 1, 1)
        d.line(
            [(0, y), (size, y)],
            fill=(int(10 + 8 * t), int(12 + 14 * t), int(16 + 18 * t)),
        )

    cx = cy = size // 2
    R = size // 2
    sR = int(R * SAFE)

    def F(frac: float) -> int:
        return max(10, int(sR * frac))

        )

    if kind == "pairing":
        text_center(d, (cx - int(sR * 0.12), cy - int(sR * 0.72)), "Matter 配网", font(F(0.20)), (232, 241, 248))
        text_center(
            d, (cx, cy - int(sR * 0.50)), "请扫码或输入配对码", font(F(0.12)), (155, 180, 196)
        )
        qr = qrcode.QRCode(
            version=4, error_correction=qrcode.constants.ERROR_CORRECT_L, box_size=3, border=1
        )
        qr.add_data("MT:Y.K9042C00KA0648G00")
        qr.make(fit=True)
        q = qr.make_image(fill_color="black", back_color="white").convert("RGB")
        qsz = int(sR * 0.82)
        q = q.resize((qsz, qsz), Image.Resampling.NEAREST)
        im.paste(q, (cx - qsz // 2, cy - qsz // 2 + int(sR * 0.06)))
        text_center(d, (cx, cy + int(sR * 0.55)), "配对码", font(F(0.12)), (208, 228, 240))
        text_center(d, (cx, cy + int(sR * 0.72)), "34970112345", font(F(0.12)), (208, 228, 240))

    elif kind == "learn":
        text_center(d, (cx, cy - int(sR * 0.45)), "红外学习", font(F(0.22)), (232, 241, 248))
        text_center(
            d, (cx, cy - int(sR * 0.10)), "对准遥控按任意键", font(F(0.14)), (155, 180, 196)
        )
        bw, bh = int(sR * 0.95), int(sR * 0.28)
        bx, by = cx - bw // 2, cy + int(sR * 0.28)
        rounded(d, [bx, by, bx + bw, by + bh], bh // 2, (47, 111, 237))
        text_center(d, (cx, by + bh / 2), "开始学习", font(F(0.16)), (255, 255, 255))

    elif kind == "ac":
        text_center(d, (cx, cy - int(sR * 0.68)), "空调", font(F(0.20)), (232, 241, 248))
        text_center(d, (cx, cy - int(sR * 0.44)), "左滑灯光", font(F(0.12)), (155, 180, 196))
        text_center(d, (cx, cy - int(sR * 0.04)), "25°", font(F(0.38)), (242, 247, 250))
        bw, bh = int(sR * 0.60), int(sR * 0.26)
        bx, by = cx - bw // 2, cy + int(sR * 0.26)
        rounded(d, [bx, by, bx + bw, by + bh], bh // 2, (31, 138, 95))
        text_center(d, (cx, by + bh / 2), "开启", font(F(0.16)), (255, 255, 255))
        # Keep cool/heat fully inside the inscribed safe circle.
        sbw, sbh = int(sR * 0.38), int(sR * 0.22)
        sby = cy + int(sR * 0.54)
        gap = int(sR * 0.06)
        rounded(d, [cx - gap - sbw, sby, cx - gap, sby + sbh], sbh // 2, (43, 76, 126))
        text_center(
            d, (cx - gap - sbw / 2, sby + sbh / 2), "降温", font(F(0.13)), (255, 255, 255)
        )
        rounded(d, [cx + gap, sby, cx + gap + sbw, sby + sbh], sbh // 2, (139, 58, 58))
        text_center(
            d, (cx + gap + sbw / 2, sby + sbh / 2), "升温", font(F(0.13)), (255, 255, 255)
        )

    elif kind == "ac_off":
        # Clean product appearance — no perimeter arc.
        text_center(d, (cx, cy - int(sR * 0.38)), "空调", font(F(0.18)), (232, 241, 248))
        text_center(d, (cx, cy - int(sR * 0.08)), "当前 24°", font(F(0.14)), (155, 180, 196))
        text_center(d, (cx, cy + int(sR * 0.26)), "关闭", font(F(0.26)), (242, 247, 250))

    elif kind == "light":
        text_center(d, (cx - int(sR * 0.10), cy - int(sR * 0.72)), "氛围灯光", font(F(0.18)), (232, 241, 248))
        text_center(d, (cx, cy - int(sR * 0.52)), "亮度", font(F(0.12)), (155, 180, 196))
        modes = ["夜间关闭", "手动亮度", "温感呼吸", "纯色", "彩虹", "呼吸白"]
        bw, bh = int(sR * 0.58), int(sR * 0.20)
        gapx, gapy = int(sR * 0.08), int(sR * 0.08)
        x0 = cx - bw - gapx / 2
        y0 = cy - int(sR * 0.30)
        for i, mode in enumerate(modes):
            col, row = i % 2, i // 2
            x1 = x0 + col * (bw + gapx)
            y1 = y0 + row * (bh + gapy)
            rounded(d, [x1, y1, x1 + bw, y1 + bh], bh // 2, (36, 52, 71))
            text_center(
                d, (x1 + bw / 2, y1 + bh / 2), mode, font(max(9, F(0.10))), (230, 238, 245)
            )
        sy = cy + int(sR * 0.62)
        sw = int(sR * 0.95)
        d.rounded_rectangle(
            [cx - sw // 2, sy - 3, cx + sw // 2, sy + 3], radius=3, fill=(60, 80, 95)
        )
        d.rounded_rectangle(
            [cx - sw // 2, sy - 3, cx - sw // 2 + int(sw * 0.65), sy + 3],
            radius=3,
            fill=(80, 160, 200),
        )
        knob = cx - sw // 2 + int(sw * 0.65)
        d.ellipse([knob - 5, sy - 5, knob + 5, sy + 5], fill=(220, 230, 240))
    else:
        raise ValueError(kind)

    out = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    mask = Image.new("L", (size, size), 0)
    ImageDraw.Draw(mask).ellipse([0, 0, size - 1, size - 1], fill=255)
    # Soften outer edge so composite doesn't hard-cut into bezel.
    mask = mask.filter(ImageFilter.GaussianBlur(0.6))
    out.paste(im, (0, 0))
    out.putalpha(mask)
    return out


def composite_on_studio(kind: str) -> Image.Image:
    """Replace nested screen on the raw studio photo; return RGB image."""
    base = studio_source().convert("RGBA")
    # Paste radius slightly inside aperture to protect white bezel.
    r = SR - 4
    screen = make_screen(r * 2, kind)
    overlay = Image.new("RGBA", base.size, (0, 0, 0, 0))
    od = ImageDraw.Draw(overlay)
    od.ellipse((SCX - SR + 3, SCY - SR + 3, SCX + SR - 3, SCY + SR - 3), fill=(0, 0, 0, 240))
    base = Image.alpha_composite(base, overlay)
    base.paste(screen, (SCX - r, SCY - r), screen)
    return base.convert("RGB")


def blank_screen_ellipse(plate: Image.Image, cx: int, cy: int, rx: int, ry: int) -> Image.Image:
    """Fully clear nested screen (undersized ellipse so white bezel is untouched)."""
    mask = Image.new("L", plate.size, 0)
    ImageDraw.Draw(mask).ellipse((cx - rx, cy - ry, cx + rx, cy + ry), fill=255)
    mask = mask.filter(ImageFilter.GaussianBlur(1.0))
    black = Image.new("RGB", plate.size, (8, 10, 14))
    out = plate.copy()
    out.paste(black, (0, 0), mask)
    return out


def prepare_side_plate() -> Image.Image:
    """Square lifestyle plate emphasizing power cable.

    Perspective makes a flat UI paste look distorted, so we do not overlay UI.
    Clear the nested screen on the crop (before pad) so pad scale cannot drift.
    """
    hero = hero_source()
    hw, hh = hero.size
    side = max(min(hw, hh) - 40, 600)
    left = max(0, (hw - side) // 2)
    top = max(0, min(hh - side, int(hh * 0.28)))
    crop = hero.crop((left, top, left + side, top + side))
    # Nested-screen ellipse in crop pixel space (hero square crop before pad).
    # Undersized so white bezel stays untouched.
    cw = crop.width
    # Approx: screen sits mid-lower in the square crop.
    cx = int(cw * 0.50)
    cy = int(cw * 0.54)
    rx = int(cw * 0.11)
    ry = int(cw * 0.10)
    crop = blank_screen_ellipse(crop, cx=cx, cy=cy, rx=rx, ry=ry)
    # Same fill/bg as studio plates so fig 2-1 / 2-2 stay visually level.
    return square_pad(crop, CANVAS, BG, fill=0.86)


def save_jpeg(img: Image.Image, path: Path, quality: int = 92) -> Path:
    img.save(path, "JPEG", quality=quality, optimize=True)
    return path


def main() -> None:
    OUT.mkdir(parents=True, exist_ok=True)
    art = Path("/opt/cursor/artifacts")
    art.mkdir(parents=True, exist_ok=True)

    # Appearance front: clean AC-off UI inside screen, then square pad.
    front = square_pad(composite_on_studio("ac_off"), CANVAS, BG)
    save_jpeg(front, OUT / "product_studio_front_pdf.jpg")
    save_jpeg(front, OUT / "product_studio_user.jpg")

    side = prepare_side_plate()
    save_jpeg(side, OUT / "product_side_power_pdf.jpg")

    hero = hero_source()
    save_jpeg(hero, OUT / "product_hero_user.jpg")
    hero_pdf = hero.copy()
    if hero_pdf.height > 1400:
        ratio = 1400 / hero_pdf.height
        hero_pdf = hero_pdf.resize(
            (int(hero_pdf.width * ratio), 1400), Image.Resampling.LANCZOS
        )
    save_jpeg(hero_pdf, OUT / "product_hero_desk_pdf.jpg", quality=90)

    kinds = [
        ("pairing", "ui_pairing_on_device_pdf.jpg"),
        ("learn", "ui_learn_on_device_pdf.jpg"),
        ("ac", "ui_ac_on_device_pdf.jpg"),
        ("light", "ui_light_on_device_pdf.jpg"),
    ]
    outs = []
    for kind, filename in kinds:
        plate = square_pad(composite_on_studio(kind), CANVAS, BG)
        outs.append(save_jpeg(plate, OUT / filename))

    for src, dst in [
        ("ui_pairing_on_device_pdf.jpg", "ui_pairing_actual.jpg"),
        ("ui_learn_on_device_pdf.jpg", "ui_learn_actual.jpg"),
        ("ui_ac_on_device_pdf.jpg", "ui_ac_actual.jpg"),
        ("ui_light_on_device_pdf.jpg", "ui_light_actual.jpg"),
        ("ui_ac_on_device_pdf.jpg", "ui_ac_on_actual.jpg"),
    ]:
        Image.open(OUT / src).save(OUT / dst, quality=90)

    # Verification crops on studio-space composites (before pad) + final plates.
    for kind, filename in kinds + [("ac_off", "product_studio_front_pdf.jpg")]:
        raw = composite_on_studio(kind if kind != "ac_off" else "ac_off")
        crop = raw.crop((SCX - SR - 20, SCY - SR - 20, SCX + SR + 20, SCY + SR + 20))
        crop.save(art / f"verify_raw_{kind}.png")
        Image.open(OUT / filename).resize((450, 450)).save(
            art / f"thumb_{Path(filename).stem}.png"
        )

    side.resize((450, 450)).save(art / "thumb_product_side_power_pdf.png")
    side.crop((450 - 140, 555 - 140, 450 + 140, 555 + 140)).save(art / "verify_side_screen.png")

    for p in outs:
        print(f"Wrote {p}")
    print(f"Front/side plates: {front.size} / {side.size}")


if __name__ == "__main__":
    main()
