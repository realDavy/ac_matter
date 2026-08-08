#!/usr/bin/env python3
"""Generate round-screen UI mockups that match main/ui (LVGL) for the user manual."""

from __future__ import annotations

from pathlib import Path

import qrcode
from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "img" / "manual"
FONT_PATH = Path("/usr/share/fonts/truetype/wqy/wqy-microhei.ttc")

SIZE = 720
CX = CY = SIZE // 2


def font(size: int) -> ImageFont.FreeTypeFont:
    return ImageFont.truetype(str(FONT_PATH), size=size, index=0)


def make_base() -> Image.Image:
    im = Image.new("RGBA", (SIZE, SIZE), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    d.ellipse([0, 0, SIZE - 1, SIZE - 1], fill=(245, 245, 243, 255))
    for i, a in enumerate([90, 60, 35]):
        pad = 10 + i * 3
        d.ellipse(
            [pad, pad, SIZE - 1 - pad, SIZE - 1 - pad],
            outline=(255, 160, 60, a),
            width=6 - i,
        )
    pad = 28
    screen = Image.new("RGB", (SIZE - 2 * pad, SIZE - 2 * pad), (16, 24, 32))
    sd = ImageDraw.Draw(screen)
    h = screen.height
    for y in range(h):
        t = y / max(h - 1, 1)
        sd.line(
            [(0, y), (screen.width, y)],
            fill=(
                int(16 + (28 - 16) * t),
                int(24 + (46 - 24) * t),
                int(32 + (58 - 32) * t),
            ),
        )
    mask = Image.new("L", screen.size, 0)
    ImageDraw.Draw(mask).ellipse([0, 0, screen.width - 1, screen.height - 1], fill=255)
    im.paste(screen, (pad, pad), mask)
    return im


def text_center(d: ImageDraw.ImageDraw, xy, text: str, f, fill) -> None:
    bbox = d.textbbox((0, 0), text, font=f)
    tw, th = bbox[2] - bbox[0], bbox[3] - bbox[1]
    d.text((xy[0] - tw / 2, xy[1] - th / 2), text, font=f, fill=fill)


def rounded_rect(d: ImageDraw.ImageDraw, box, radius: int, fill) -> None:
    d.rounded_rectangle(box, radius=radius, fill=fill)


def draw_lang(d: ImageDraw.ImageDraw, label: str = "EN") -> None:
    x1, y1 = SIZE - 28 - 70, 28 + 18
    x2, y2 = SIZE - 28 - 14, 28 + 18 + 42
    rounded_rect(d, [x1, y1, x2, y2], 12, (58, 74, 88))
    text_center(d, ((x1 + x2) / 2, (y1 + y2) / 2), label, font(22), (255, 255, 255))


def save(im: Image.Image, name: str) -> Path:
    bg = Image.new("RGB", (SIZE + 80, SIZE + 80), (232, 234, 236))
    glow = Image.new("RGBA", bg.size, (0, 0, 0, 0))
    gd = ImageDraw.Draw(glow)
    for i in range(40, 0, -1):
        a = int(8 * (i / 40))
        gd.ellipse(
            [40 - i, 40 - i, SIZE + 40 + i, SIZE + 40 + i],
            fill=(255, 150, 50, a),
        )
    bg = Image.alpha_composite(bg.convert("RGBA"), glow).convert("RGB")
    bg.paste(im, (40, 40), im)
    OUT.mkdir(parents=True, exist_ok=True)
    path = OUT / name
    bg.save(path, "JPEG", quality=88, optimize=True)
    return path


def make_pairing() -> Path:
    im = make_base()
    d = ImageDraw.Draw(im)
    draw_lang(d)
    text_center(d, (CX, 28 + 48), "Matter 配网", font(34), (232, 241, 248))
    text_center(d, (CX, 28 + 90), "请扫码或输入配对码", font(24), (155, 180, 196))
    qr = qrcode.QRCode(
        version=4,
        error_correction=qrcode.constants.ERROR_CORRECT_L,
        box_size=5,
        border=2,
    )
    # Sample payload only for illustration; device shows a live MT: payload.
    qr.add_data("MT:Y.K9042C00KA0648G00")
    qr.make(fit=True)
    qrim = (
        qr.make_image(fill_color="black", back_color="white")
        .convert("RGB")
        .resize((200, 200), Image.Resampling.NEAREST)
    )
    im.paste(qrim, (CX - 100, CY - 115))
    text_center(d, (CX, SIZE - 28 - 95), "配对码", font(24), (208, 228, 240))
    text_center(d, (CX, SIZE - 28 - 60), "34970112345", font(24), (208, 228, 240))
    return save(im, "ui_pairing_actual.jpg")


def make_learn() -> Path:
    im = make_base()
    d = ImageDraw.Draw(im)
    draw_lang(d)
    text_center(d, (CX, 28 + 48), "红外学习", font(34), (232, 241, 248))
    text_center(d, (CX, 28 + 90), "对准遥控按任意键", font(24), (155, 180, 196))
    bx1, by1, bx2, by2 = CX - 105, CY + 20, CX + 105, CY + 20 + 66
    rounded_rect(d, [bx1, by1, bx2, by2], 22, (47, 111, 237))
    text_center(d, (CX, (by1 + by2) / 2), "开始学习", font(26), (255, 255, 255))
    return save(im, "ui_learn_actual.jpg")


def make_ac(power_on: bool = False, temp: int = 25, name: str = "ui_ac_actual.jpg") -> Path:
    im = make_base()
    d = ImageDraw.Draw(im)
    draw_lang(d)
    text_center(d, (CX, 28 + 48), "空调", font(34), (232, 241, 248))
    text_center(d, (CX, 28 + 90), "左滑灯光", font(24), (155, 180, 196))
    text_center(d, (CX, CY - 70), f"{temp}°", font(48), (242, 247, 250))
    bx1, by1, bx2, by2 = CX - 75, CY - 20, CX + 75, CY - 20 + 63
    rounded_rect(d, [bx1, by1, bx2, by2], 22, (31, 138, 95))
    # Button label is the action: Off device shows 开启; On device shows 关闭.
    label = "关闭" if power_on else "开启"
    text_center(d, (CX, (by1 + by2) / 2), label, font(26), (255, 255, 255))
    rounded_rect(d, [CX - 55 - 88, CY + 55, CX - 55, CY + 55 + 60], 20, (43, 76, 126))
    text_center(d, (CX - 55 - 44, CY + 55 + 30), "降温", font(24), (255, 255, 255))
    rounded_rect(d, [CX + 55, CY + 55, CX + 55 + 88, CY + 55 + 60], 20, (139, 58, 58))
    text_center(d, (CX + 55 + 44, CY + 55 + 30), "升温", font(24), (255, 255, 255))
    text_center(d, (CX, SIZE - 28 - 40), "左滑灯光", font(22), (127, 151, 168))
    return save(im, name)


def make_light() -> Path:
    im = make_base()
    d = ImageDraw.Draw(im)
    draw_lang(d)
    text_center(d, (CX, 28 + 48), "氛围灯光", font(34), (232, 241, 248))
    text_center(d, (CX, 28 + 90), "亮度", font(24), (155, 180, 196))
    modes = ["夜间关闭", "手动亮度", "温感呼吸", "纯色", "彩虹", "呼吸白"]
    bw, bh = 147, 51
    gapx, gapy = 10, 10
    grid_w = bw * 2 + gapx
    grid_h = bh * 3 + gapy * 2
    x0 = CX - grid_w / 2
    y0 = CY - 30 - grid_h / 2
    for i, mode in enumerate(modes):
        col, row = i % 2, i // 2
        x1 = x0 + col * (bw + gapx)
        y1 = y0 + row * (bh + gapy)
        rounded_rect(d, [x1, y1, x1 + bw, y1 + bh], 20, (36, 52, 71))
        text_center(d, (x1 + bw / 2, y1 + bh / 2), mode, font(22), (230, 238, 245))
    sy = SIZE - 28 - 70
    sx1, sx2 = CX - 120, CX + 120
    d.rounded_rectangle([sx1, sy - 5, sx2, sy + 5], radius=5, fill=(60, 80, 95))
    d.rounded_rectangle([sx1, sy - 5, CX + 40, sy + 5], radius=5, fill=(80, 160, 200))
    d.ellipse([CX + 40 - 12, sy - 12, CX + 40 + 12, sy + 12], fill=(220, 230, 240))
    return save(im, "ui_light_actual.jpg")


def main() -> None:
    paths = [
        make_pairing(),
        make_learn(),
        make_ac(power_on=False, temp=25, name="ui_ac_actual.jpg"),
        make_ac(power_on=True, temp=24, name="ui_ac_on_actual.jpg"),
        make_light(),
    ]
    for p in paths:
        print(f"Wrote {p}")


if __name__ == "__main__":
    main()
