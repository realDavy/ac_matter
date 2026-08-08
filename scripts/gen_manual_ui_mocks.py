#!/usr/bin/env python3
"""Composite firmware-accurate round-screen UI onto the product render.

Important: do NOT invent a different product chassis. Always reuse
img/manual/product_studio_front_pdf.jpg (Aura Ring form factor) and only
replace the nested circular display pixels.
"""

from __future__ import annotations

import math
from pathlib import Path

import qrcode
from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "img" / "manual"
PRODUCT = OUT / "product_studio_front_pdf.jpg"
FONT_PATH = Path("/usr/share/fonts/truetype/wqy/wqy-microhei.ttc")

# Nested circular display on product_studio_front_pdf.jpg (do not change housing)
SCX, SCY, SR = 508, 628, 125


def font(size: int) -> ImageFont.FreeTypeFont:
    return ImageFont.truetype(str(FONT_PATH), size=size, index=0)


def text_center(d, xy, text, f, fill) -> None:
    b = d.textbbox((0, 0), text, font=f)
    tw, th = b[2] - b[0], b[3] - b[1]
    d.text((xy[0] - tw / 2, xy[1] - th / 2), text, font=f, fill=fill)


def rounded(d, box, r, fill) -> None:
    d.rounded_rectangle(box, radius=r, fill=fill)


def make_screen(size: int, kind: str) -> Image.Image:
    im = Image.new("RGB", (size, size), (16, 24, 32))
    d = ImageDraw.Draw(im)
    for y in range(size):
        t = y / max(size - 1, 1)
        d.line(
            [(0, y), (size, y)],
            fill=(int(16 + 12 * t), int(24 + 22 * t), int(32 + 26 * t)),
        )
    scale = size / 240.0

    def S(v: float) -> int:
        return max(1, int(v * scale))

    cx = cy = size // 2
    rounded(d, [size - S(56), S(12), size - S(10), S(40)], S(10), (58, 74, 88))
    text_center(d, (size - S(33), S(26)), "EN", font(max(10, S(14))), (255, 255, 255))

    if kind == "pairing":
        text_center(d, (cx, S(28)), "Matter 配网", font(max(14, S(18))), (232, 241, 248))
        text_center(d, (cx, S(50)), "请扫码或输入配对码", font(max(10, S(12))), (155, 180, 196))
        qr = qrcode.QRCode(
            version=4, error_correction=qrcode.constants.ERROR_CORRECT_L, box_size=3, border=1
        )
        qr.add_data("MT:Y.K9042C00KA0648G00")
        qr.make(fit=True)
        q = qr.make_image(fill_color="black", back_color="white").convert("RGB")
        qsz = S(88)
        q = q.resize((qsz, qsz), Image.Resampling.NEAREST)
        im.paste(q, (cx - qsz // 2, cy - qsz // 2 - S(6)))
        text_center(d, (cx, size - S(52)), "配对码", font(max(10, S(12))), (208, 228, 240))
        text_center(d, (cx, size - S(32)), "34970112345", font(max(10, S(12))), (208, 228, 240))
    elif kind == "learn":
        text_center(d, (cx, S(28)), "红外学习", font(max(14, S(18))), (232, 241, 248))
        text_center(d, (cx, S(52)), "对准遥控按任意键", font(max(11, S(13))), (155, 180, 196))
        rounded(d, [cx - S(70), cy + S(8), cx + S(70), cy + S(44)], S(14), (47, 111, 237))
        text_center(d, (cx, cy + S(26)), "开始学习", font(max(12, S(14))), (255, 255, 255))
    elif kind == "ac":
        text_center(d, (cx, S(28)), "空调", font(max(14, S(18))), (232, 241, 248))
        text_center(d, (cx, S(50)), "左滑灯光", font(max(11, S(12))), (155, 180, 196))
        text_center(d, (cx, cy - S(42)), "25°", font(max(18, S(26))), (242, 247, 250))
        rounded(d, [cx - S(48), cy - S(6), cx + S(48), cy + S(26)], S(14), (31, 138, 95))
        text_center(d, (cx, cy + S(10)), "开启", font(max(12, S(14))), (255, 255, 255))
        rounded(d, [cx - S(92), cy + S(38), cx - S(12), cy + S(68)], S(12), (43, 76, 126))
        text_center(d, (cx - S(52), cy + S(53)), "降温", font(max(11, S(12))), (255, 255, 255))
        rounded(d, [cx + S(12), cy + S(38), cx + S(92), cy + S(68)], S(12), (139, 58, 58))
        text_center(d, (cx + S(52), cy + S(53)), "升温", font(max(11, S(12))), (255, 255, 255))
        text_center(d, (cx, size - S(26)), "左滑灯光", font(max(10, S(11))), (127, 151, 168))
    elif kind == "light":
        text_center(d, (cx, S(26)), "氛围灯光", font(max(13, S(16))), (232, 241, 248))
        text_center(d, (cx, S(46)), "亮度", font(max(10, S(12))), (155, 180, 196))
        modes = ["夜间关闭", "手动亮度", "温感呼吸", "纯色", "彩虹", "呼吸白"]
        bw, bh, gapx, gapy = S(78), S(26), S(5), S(5)
        x0 = cx - (bw * 2 + gapx) / 2
        y0 = cy - S(50)
        for i, mode in enumerate(modes):
            col, row = i % 2, i // 2
            x1 = x0 + col * (bw + gapx)
            y1 = y0 + row * (bh + gapy)
            rounded(d, [x1, y1, x1 + bw, y1 + bh], S(11), (36, 52, 71))
            text_center(d, (x1 + bw / 2, y1 + bh / 2), mode, font(max(9, S(10))), (230, 238, 245))
        sy = size - S(48)
        d.rounded_rectangle([cx - S(68), sy - S(3), cx + S(68), sy + S(3)], radius=3, fill=(60, 80, 95))
        d.rounded_rectangle([cx - S(68), sy - S(3), cx + S(18), sy + S(3)], radius=3, fill=(80, 160, 200))
        d.ellipse([cx + S(18) - S(7), sy - S(7), cx + S(18) + S(7), sy + S(7)], fill=(220, 230, 240))
    else:
        raise ValueError(kind)

    out = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    mask = Image.new("L", (size, size), 0)
    ImageDraw.Draw(mask).ellipse([0, 0, size - 1, size - 1], fill=255)
    out.paste(im, (0, 0))
    out.putalpha(mask)
    return out


def composite(kind: str, filename: str) -> Path:
    if not PRODUCT.is_file():
        raise SystemExit(f"Missing product render: {PRODUCT}")
    base = Image.open(PRODUCT).convert("RGBA")
    screen = make_screen(SR * 2, kind)
    base.paste(screen, (SCX - SR, SCY - SR), screen)
    path = OUT / filename
    base.convert("RGB").save(path, "JPEG", quality=90, optimize=True)
    return path


def main() -> None:
    outs = [
        composite("pairing", "ui_pairing_on_device_pdf.jpg"),
        composite("learn", "ui_learn_on_device_pdf.jpg"),
        composite("ac", "ui_ac_on_device_pdf.jpg"),
        composite("light", "ui_light_on_device_pdf.jpg"),
    ]
    # aliases
    for src, dst in [
        ("ui_pairing_on_device_pdf.jpg", "ui_pairing_actual.jpg"),
        ("ui_learn_on_device_pdf.jpg", "ui_learn_actual.jpg"),
        ("ui_ac_on_device_pdf.jpg", "ui_ac_actual.jpg"),
        ("ui_light_on_device_pdf.jpg", "ui_light_actual.jpg"),
        ("ui_ac_on_device_pdf.jpg", "ui_ac_on_actual.jpg"),
    ]:
        Image.open(OUT / src).save(OUT / dst, quality=90)
    for p in outs:
        print(f"Wrote {p}")


if __name__ == "__main__":
    main()
