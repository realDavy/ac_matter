#!/usr/bin/env python3
"""Generate docs/aidaegis_ac_remote_user_manual.pdf — commercial Chinese user guide."""

from __future__ import annotations

from pathlib import Path

from fpdf import FPDF
from fontTools.ttLib import TTCollection

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "docs" / "aidaegis_ac_remote_user_manual.pdf"
IMG = ROOT / "img" / "manual"
WQY_TTC = Path("/usr/share/fonts/truetype/wqy/wqy-microhei.ttc")


def resolve_font() -> Path:
    """WenQuanYi Micro Hei covers CJK + Latin."""
    cached = ROOT / "docs" / "fonts" / "wqy-microhei.ttf"
    if cached.is_file():
        return cached
    if WQY_TTC.is_file():
        cached.parent.mkdir(parents=True, exist_ok=True)
        TTCollection(str(WQY_TTC)).fonts[0].save(str(cached))
        return cached
    raise SystemExit("Need WenQuanYi Micro Hei (fonts-wqy-microhei) to build the PDF")


FONT = resolve_font()

# Brand palette
C_NAVY = (24, 48, 72)
C_NAVY2 = (36, 64, 96)
C_TEXT = (35, 38, 42)
C_MUTED = (110, 116, 124)
C_LINE = (210, 216, 222)
C_SOFT = (245, 248, 252)
C_ACCENT = (180, 200, 220)


def img(name: str) -> Path:
    p = IMG / name
    if not p.is_file():
        raise SystemExit(f"Missing image: {p}")
    return p


class ManualPDF(FPDF):
    def __init__(self) -> None:
        super().__init__(format="A4", unit="mm")
        self.set_auto_page_break(auto=True, margin=18)
        self.set_margins(18, 18, 18)
        self.add_font("cn", "", str(FONT))
        self.add_font("cn", "B", str(FONT))

    def header(self) -> None:
        if self.page_no() <= 1:
            return
        self.set_font("cn", "", 8.5)
        self.set_text_color(*C_MUTED)
        self.cell(0, 7, "aidaegis AC Remote  ·  用户说明书", align="L")
        self.ln(2)
        self.set_draw_color(*C_LINE)
        self.set_line_width(0.25)
        self.line(self.l_margin, 14, self.w - self.r_margin, 14)
        self.ln(6)
        self.set_text_color(*C_TEXT)

    def footer(self) -> None:
        if self.page_no() <= 1:
            return
        self.set_y(-14)
        self.set_draw_color(*C_LINE)
        self.line(self.l_margin, self.get_y(), self.w - self.r_margin, self.get_y())
        self.set_y(-12)
        self.set_font("cn", "", 8)
        self.set_text_color(*C_MUTED)
        self.cell(0, 8, f"— {self.page_no()} —", align="C")

    def _reset_x(self) -> None:
        self.set_x(self.l_margin)

    def cover(self) -> None:
        self.add_page()
        # Hero product image
        hero = img("product_hero_desk_pdf.jpg")
        iw = self.epw
        ih = iw * 1024 / 1536
        self.image(str(hero), x=self.l_margin, y=22, w=iw, h=ih)

        y = 22 + ih + 10
        self.set_y(y)
        self.set_font("cn", "B", 26)
        self.set_text_color(*C_NAVY)
        self.set_x(self.l_margin)
        self.multi_cell(self.epw, 12, "aidaegis", align="C")
        self.set_font("cn", "B", 20)
        self.set_x(self.l_margin)
        self.multi_cell(self.epw, 10, "AC Remote", align="C")
        self.ln(2)
        self.set_draw_color(*C_NAVY)
        self.set_line_width(0.45)
        cy = self.get_y()
        self.line(78, cy, 132, cy)
        self.ln(7)
        self.set_font("cn", "", 14)
        self.set_text_color(*C_TEXT)
        self.set_x(self.l_margin)
        self.multi_cell(self.epw, 8, "智能空调控制器  ·  用户说明书", align="C")
        self.ln(4)
        self.set_font("cn", "", 10)
        self.set_text_color(*C_MUTED)
        self.set_x(self.l_margin)
        self.multi_cell(
            self.epw,
            6,
            "支持 Apple 家庭 / Google Home / Home Assistant\n"
            "圆屏触控 · Matter 本地控制 · 氛围光环",
            align="C",
        )
        self.set_y(-28)
        self.set_font("cn", "", 9)
        self.set_text_color(*C_MUTED)
        self.set_x(self.l_margin)
        self.multi_cell(self.epw, 5, "请妥善保管本说明书，以便日后查阅。", align="C")

    def h1(self, title: str) -> None:
        if self.get_y() > self.h - 40:
            self.add_page()
        self.ln(3)
        self._reset_x()
        self.set_font("cn", "B", 15)
        self.set_text_color(*C_NAVY)
        # Left-align CJK body text — fpdf2 default JUSTIFY opens huge gaps with mixed CN/EN.
        self.multi_cell(0, 9, title, align="L")
        self._reset_x()
        self.set_draw_color(*C_NAVY)
        self.set_line_width(0.45)
        y = self.get_y()
        self.line(self.l_margin, y, self.l_margin + 36, y)
        self.ln(4)
        self.set_text_color(*C_TEXT)

    def h2(self, title: str) -> None:
        self.ln(2)
        self._reset_x()
        self.set_font("cn", "B", 11.5)
        self.set_text_color(*C_NAVY2)
        self.multi_cell(0, 7, title, align="L")
        self._reset_x()
        self.set_text_color(*C_TEXT)

    def p(self, text: str) -> None:
        self._reset_x()
        self.set_font("cn", "", 10)
        self.set_text_color(*C_TEXT)
        self.multi_cell(0, 6, text, align="L")
        self._reset_x()
        self.ln(1)

    def bullets(self, items: list[str]) -> None:
        self.set_font("cn", "", 10)
        self.set_text_color(*C_TEXT)
        for item in items:
            self._reset_x()
            self.multi_cell(0, 6, f"•  {item}", align="L")
        self._reset_x()
        self.ln(1)

    def numbered(self, items: list[str]) -> None:
        self.set_font("cn", "", 10)
        self.set_text_color(*C_TEXT)
        for i, item in enumerate(items, 1):
            self._reset_x()
            self.multi_cell(0, 6, f"{i}.  {item}", align="L")
        self._reset_x()
        self.ln(1)

    def note(self, text: str) -> None:
        self._reset_x()
        self.set_fill_color(*C_SOFT)
        self.set_draw_color(*C_ACCENT)
        self.set_font("cn", "", 9)
        self.set_text_color(*C_TEXT)
        start = self.get_y()
        self.set_x(self.l_margin + 2)
        self.multi_cell(self.epw - 4, 5.5, f"提示：{text}", align="L")
        end = self.get_y()
        self.rect(self.l_margin, start - 1.5, self.epw, end - start + 3)
        self.set_y(end + 3)
        self._reset_x()
        self.set_text_color(*C_TEXT)

    def caption(self, text: str) -> None:
        self._reset_x()
        self.set_font("cn", "", 8.5)
        self.set_text_color(*C_MUTED)
        self.multi_cell(0, 5, text, align="C")
        self._reset_x()
        self.set_text_color(*C_TEXT)
        self.ln(2)

    def fig(self, path: Path, width: float | None = None, caption: str = "") -> None:
        """Place a centered figure; page-break if needed."""
        w = width if width is not None else self.epw * 0.72
        # Estimate height from image aspect
        from PIL import Image as PILImage

        with PILImage.open(path) as im:
            iw, ih = im.size
        h = w * ih / iw
        need = h + (8 if caption else 2)
        if self.get_y() + need > self.page_break_trigger:
            self.add_page()
        x = self.l_margin + (self.epw - w) / 2
        self.image(str(path), x=x, y=self.get_y(), w=w, h=h)
        self.set_y(self.get_y() + h + 1)
        if caption:
            self.caption(caption)

    def figs_row(
        self,
        paths: list[Path],
        captions: list[str],
        gap: float = 6,
        width: float | None = None,
    ) -> None:
        n = len(paths)
        assert n == len(captions) and n >= 2
        w = width if width is not None else (self.epw - gap * (n - 1)) / n
        from PIL import Image as PILImage

        heights = []
        for p in paths:
            with PILImage.open(p) as im:
                iw, ih = im.size
            heights.append(w * ih / iw)
        h = max(heights)
        need = h + 12
        if self.get_y() + need > self.page_break_trigger:
            self.add_page()
        y0 = self.get_y()
        x0 = self.l_margin
        for i, (p, cap) in enumerate(zip(paths, captions)):
            x = x0 + i * (w + gap)
            self.image(str(p), x=x, y=y0, w=w, h=heights[i])
        self.set_y(y0 + h + 1)
        self.set_font("cn", "", 8)
        self.set_text_color(*C_MUTED)
        for i, cap in enumerate(captions):
            x = x0 + i * (w + gap)
            self.set_xy(x, self.get_y())
            self.multi_cell(w, 4.5, cap, align="C")
        self._reset_x()
        self.set_text_color(*C_TEXT)
        self.ln(3)

    def table(self, headers: list[str], rows: list[list[str]], col_w: list[float]) -> None:
        self._reset_x()
        self.set_font("cn", "B", 9)
        self.set_fill_color(*C_NAVY)
        self.set_text_color(255, 255, 255)
        for h, w in zip(headers, col_w):
            self.cell(w, 7, h, border=1, fill=True, align="C")
        self.ln()
        self._reset_x()
        self.set_font("cn", "", 9)
        self.set_text_color(*C_TEXT)
        fill = False
        for row in rows:
            self.set_fill_color(248, 250, 252)
            heights = [max(7, self._estimate_lines(text, w) * 5 + 2) for text, w in zip(row, col_w)]
            row_h = max(heights)
            if self.get_y() + row_h > self.page_break_trigger:
                self.add_page()
                self._reset_x()
                self.set_font("cn", "B", 9)
                self.set_fill_color(*C_NAVY)
                self.set_text_color(255, 255, 255)
                for h, w in zip(headers, col_w):
                    self.cell(w, 7, h, border=1, fill=True, align="C")
                self.ln()
                self._reset_x()
                self.set_font("cn", "", 9)
                self.set_text_color(*C_TEXT)
            y0 = self.get_y()
            x0 = self.l_margin
            self.set_x(x0)
            for text, w in zip(row, col_w):
                x = self.get_x()
                self.rect(x, y0, w, row_h, style="DF" if fill else "D")
                self.set_xy(x + 1, y0 + 1)
                self.multi_cell(w - 2, 5, text, border=0, align="L")
                self.set_xy(x + w, y0)
            self.set_xy(x0, y0 + row_h)
            fill = not fill
        self._reset_x()
        self.ln(3)

    def _estimate_lines(self, text: str, width: float) -> int:
        if not text:
            return 1
        avg = max(self.font_size * 0.45, 1.0)
        chars_per_line = max(int(width / avg), 1)
        lines = 0
        for para in text.split("\n"):
            lines += max(1, (len(para) + chars_per_line - 1) // chars_per_line)
        return lines

    def toc_line(self, num: str, title: str) -> None:
        self._reset_x()
        self.set_font("cn", "", 11)
        self.set_text_color(*C_TEXT)
        self.cell(12, 8, num)
        self.cell(0, 8, title)
        self.ln(8)


def build() -> Path:
    pdf = ManualPDF()
    pdf.set_title("aidaegis AC Remote 用户说明书")
    pdf.set_author("aidaegis")
    pdf.set_creator("aidaegis AC Remote")
    pdf.set_keywords("aidaegis, AC Remote, Matter, 空调, 用户说明书")

    # ——— Cover ———
    pdf.cover()

    # ——— TOC ———
    pdf.add_page()
    pdf.h1("目录")
    toc = [
        ("01", "产品简介"),
        ("02", "外观与界面总览"),
        ("03", "放置建议"),
        ("04", "首次使用：智能家居配网"),
        ("05", "绑定空调（红外学习）"),
        ("06", "圆屏操作说明"),
        ("07", "手机 App 控制"),
        ("08", "氛围灯光"),
        ("09", "恢复出厂设置"),
        ("10", "常见问题"),
        ("11", "产品规格"),
        ("12", "安全与注意事项"),
    ]
    for num, title in toc:
        pdf.toc_line(num, title)

    # ——— 1 产品简介 ———
    pdf.add_page()
    pdf.h1("1. 产品简介")
    pdf.p(
        "感谢您选择 aidaegis AC Remote。本产品是一款面向家庭使用的智能空调控制器，"
        "通过红外方式控制家中空调，并以 Matter 标准接入 Apple「家庭」、Google Home "
        "或 Home Assistant 等智能家居平台，实现本地控制，无需依赖空调厂商云服务。"
    )
    pdf.p(
        "设备采用光环造型设计：外圈为氛围灯环，内嵌 1.28 英寸圆形触摸屏。"
        "您可在屏上完成配网、空调绑定、日常控制与灯光设置，也可通过手机 App 远程操作。"
    )

    pdf.h2("主要功能")
    pdf.bullets(
        [
            "智能家居配网：圆屏显示动态二维码与数字配对码，一键添加至家庭平台",
            "空调绑定：使用原装遥控器完成红外学习，快速对接您的空调",
            "本地控制：开关、制冷/制热、设定温度、风扇档位；屏上与手机状态双向同步",
            "氛围灯环：多种灯光模式与亮度调节，可在手机端开关与调光",
            "环境感知（选配）：支持室温与湿度显示",
            "中英文界面：出厂默认中文，可一键切换英文",
        ]
    )
    pdf.fig(
        img("product_studio_front_pdf.jpg"),
        width=95,
        caption="图 1-1  产品正面（氛围灯环点亮示意）",
    )
    pdf.note(
        "本产品通过红外协议控制空调。若您的空调遥控协议不受支持，绑定可能失败。"
        "届时请参考「常见问题」或联系售后支持。"
    )

    # ——— 2 外观与界面总览 ———
    pdf.h1("2. 外观与界面总览")
    pdf.p(
        "请先熟悉产品各部分名称，以便后续阅读操作说明。"
    )
    pdf.h2("2.1 外观结构")
    pdf.table(
        ["名称", "说明"],
        [
            ["氛围灯环", "外圈环形灯带，提供环境照明与状态氛围"],
            ["圆形触摸屏", "1.28 英寸触控显示屏，用于配网、学习与日常控制"],
            ["底座", "稳定支撑整机，便于桌面放置"],
            ["电源接口", "位于底座后方，连接附赠电源线供电"],
        ],
        [40, 134],
    )
    pdf.figs_row(
        [img("product_studio_front_pdf.jpg"), img("product_side_power_pdf.jpg")],
        ["图 2-1  正面外观", "图 2-2  侧面与电源连接"],
        gap=8,
    )

    pdf.h2("2.2 圆屏界面一览")
    pdf.p(
        "设备按使用阶段自动切换以下四个界面（界面文案与布局以设备实际显示为准，"
        "下图为与产品一致的示意）："
    )
    pdf.table(
        ["界面标题", "何时出现", "主要操作"],
        [
            ["Matter 配网", "首次上电或尚未完成智能家居配网", "扫码 / 输入配对码"],
            ["红外学习", "已配网但尚未绑定空调", "点按「开始学习」并用原装遥控学码"],
            ["空调", "绑定完成后（日常使用）", "开启/关闭、降温/升温；左滑进入灯光"],
            ["氛围灯光", "在空调页向左滑动进入", "选择灯光模式、拖动亮度滑条；右滑返回"],
        ],
        [36, 58, 80],
    )
    pdf.figs_row(
        [
            img("ui_pairing_on_device_pdf.jpg"),
            img("ui_learn_on_device_pdf.jpg"),
        ],
        ["图 2-3  Matter 配网页", "图 2-4  红外学习页"],
        gap=8,
    )
    pdf.figs_row(
        [
            img("ui_ac_on_device_pdf.jpg"),
            img("ui_light_on_device_pdf.jpg"),
        ],
        ["图 2-5  空调控制页", "图 2-6  氛围灯光页"],
        gap=8,
    )
    pdf.note(
        "各页右上角均有「EN」按钮，可切换中文 / 英文界面（英文界面显示为「中文」）。"
        "出厂默认中文。"
    )

    # ——— 3 放置建议 ———
    pdf.h1("3. 放置建议")
    pdf.p(
        "正确放置有助于红外控制更稳定，也能让灯环氛围效果更舒适。"
    )
    pdf.numbered(
        [
            "将设备放置在空调附近的桌面或台面，保证前方开阔、无遮挡。",
            "使红外发射方向大致朝向空调室内机的接收窗口（通常位于出风口附近）。",
            "使设备能够接收到您日常使用原装遥控器时的红外信号（便于状态同步）。",
            "避免强阳光直射屏幕与接收区域；远离金属大面积遮挡。",
            "使用稳定电源供电；手机与设备须连接同一家庭网络中的 2.4 GHz Wi-Fi。",
        ]
    )
    pdf.fig(
        img("product_hero_desk_pdf.jpg"),
        width=pdf.epw * 0.88,
        caption="图 3-1  建议放置于床头柜、书桌等稳定平面",
    )
    pdf.note(
        "Matter 配网依赖 2.4 GHz Wi-Fi。若家中路由器为双频合一，请确认手机当前已连接 2.4 GHz 频段。"
    )

    # ——— 4 配网 ———
    pdf.h1("4. 首次使用：智能家居配网")
    pdf.p(
        "首次上电或尚未完成配网时，圆屏显示「Matter 配网」页。"
        "请使用您常用的智能家居 App 完成添加。"
    )
    pdf.fig(
        img("ui_pairing_on_device_pdf.jpg"),
        width=78,
        caption="图 4-1  Matter 配网页（示意；请以屏上实时内容为准）",
    )
    pdf.h2("界面说明")
    pdf.table(
        ["屏幕元素", "功能说明"],
        [
            ["标题「Matter 配网」", "当前处于智能家居配网阶段"],
            ["提示「请扫码或输入配对码」", "引导使用 App 扫码或手输配对码"],
            ["中央二维码", "由设备实时生成的 Matter 配网码，供 App 扫描"],
            ["「配对码」+ 数字", "无法扫码时，在 App 中手动输入该数字码"],
            ["右上角「EN」", "切换为英文界面"],
        ],
        [50, 124],
    )

    pdf.h2("操作步骤")
    pdf.numbered(
        [
            "为设备接通电源，等待圆屏显示「Matter 配网」页面。",
            "确认手机已连接家庭 2.4 GHz Wi-Fi。",
            "打开 Apple「家庭」、Google Home 或 Home Assistant Companion，选择添加 Matter 配件。",
            "扫描屏上二维码；若不便扫码，可手动输入屏上「配对码」下方的数字。",
            "若系统提示“未认证 / 未经验证的配件”，按提示继续即可。",
            "配网成功后，手机中将出现本设备（默认名称：AC Remote；品牌：aidaegis）。",
        ]
    )
    pdf.note(
        "屏上二维码与配对码由设备实时生成。请务必使用当前屏幕显示的内容，"
        "勿扫描说明书中的示例图。"
    )

    # ——— 5 红外学习 ———
    pdf.h1("5. 绑定空调（红外学习）")
    pdf.p(
        "完成智能家居配网后，若尚未绑定空调，屏幕进入「红外学习」页。"
        "请准备好空调原装遥控器。"
    )
    pdf.fig(
        img("ui_learn_on_device_pdf.jpg"),
        width=78,
        caption="图 5-1  红外学习页",
    )
    pdf.h2("界面说明")
    pdf.table(
        ["屏幕元素", "功能说明"],
        [
            ["标题「红外学习」", "当前处于空调绑定阶段"],
            ["提示「对准遥控按任意键」", "学习开始后，请用原装遥控对准本机按键"],
            ["「开始学习」按钮", "点按后开始等待红外信号；等待中按钮显示为「...」"],
            ["右上角「EN」", "切换为英文界面"],
        ],
        [50, 124],
    )

    pdf.h2("操作步骤")
    pdf.numbered(
        [
            "在圆屏上点按「开始学习」。此时氛围灯环呈黄色柔和呼吸，表示正在等待遥控信号。",
            "将原装遥控器对准本设备，按下任意键（建议：制冷模式、25℃、任意风速）。",
            "学习成功后，屏幕自动进入「空调」控制页，即可开始使用。",
            "若多次失败，请确认遥控器电量充足、对准方向正确，并参阅第 10 章「常见问题」。",
        ]
    )
    pdf.note(
        "学习入口仅在触摸屏「开始学习」。绑定完成后，日常使用原装遥控时，"
        "设备可接收信号并尽量与手机端状态保持同步。"
    )

    # ——— 6 圆屏操作 ———
    pdf.h1("6. 圆屏操作说明")
    pdf.p(
        "绑定完成后，日常控制在「空调」页完成；向左滑动进入「氛围灯光」页。"
        "各页右上角「EN / 中文」可切换界面语言。"
    )

    pdf.h2("6.1 空调控制页")
    pdf.fig(
        img("ui_ac_on_device_pdf.jpg"),
        width=78,
        caption="图 6-1  空调控制页（关机状态示例：按钮显示「开启」）",
    )
    pdf.table(
        ["屏幕元素", "功能说明"],
        [
            ["标题「空调」", "当前为空调控制界面"],
            ["副标题 / 底部「左滑灯光」", "提示可向左滑动进入氛围灯光页"],
            ["设定温度（如 25°）", "显示目标设定温度，单位为整度 ℃"],
            ["绿色「开启」/「关闭」", "空调关闭时显示「开启」，开启时显示「关闭」；点按切换开关"],
            ["「降温」", "设定温度降低 1℃，并向空调发送红外指令"],
            ["「升温」", "设定温度升高 1℃，并向空调发送红外指令"],
            ["右上角「EN」", "切换为英文界面"],
        ],
        [50, 124],
    )
    pdf.p(
        "屏上开关与设定温度与手机智能家居 App 双向同步；任一侧更改都会反映到另一侧。"
    )

    pdf.h2("6.2 氛围灯光页")
    pdf.p("在空调控制页向左滑动进入本页；向右滑动返回「空调」页。")
    pdf.fig(
        img("ui_light_on_device_pdf.jpg"),
        width=78,
        caption="图 6-2  氛围灯光页",
    )
    pdf.table(
        ["屏幕元素", "功能说明"],
        [
            ["标题「氛围灯光」", "当前为灯环模式与亮度设置界面"],
            ["副标题「亮度」", "提示下方滑条用于调节亮度"],
            ["六种模式按钮", "点按切换灯环效果（见下表）"],
            ["底部亮度滑条", "调节灯环明暗；与手机端亮度控制对应"],
            ["右上角「EN」", "切换为英文界面"],
            ["向右滑动", "返回空调控制页"],
        ],
        [45, 129],
    )
    pdf.table(
        ["灯光模式", "效果说明"],
        [
            ["夜间关闭", "关闭灯环，适合睡眠环境"],
            ["手动亮度", "白光常亮，亮度跟随滑条或手机调节"],
            ["温感呼吸", "出厂默认；按室温在绿色至橙色间柔和呼吸"],
            ["纯色", "按当前温感色固定点亮"],
            ["彩虹", "色彩循环变化"],
            ["呼吸白", "白光呼吸效果"],
        ],
        [40, 134],
    )
    pdf.p(
        "手机 App 可控制灯环开关与亮度；彩虹、呼吸等氛围模式仅在本机圆屏选择。"
        "红外学习进行中时，灯环会临时变为黄色呼吸，结束后自动恢复原模式。"
    )

    pdf.h2("6.3 语言切换")
    pdf.p(
        "在任意主要界面，点按屏幕右上角「EN」切换为英文界面；"
        "在英文界面点按「中文」切回中文。出厂默认中文。"
    )

    # ——— 7 手机 App ———
    pdf.h1("7. 手机 App 控制")
    pdf.p(
        "完成配网并绑定空调后，即可在手机智能家居应用中控制本设备。"
        "不同平台界面略有差异，可控制内容如下："
    )
    pdf.table(
        ["控制项", "可操作内容"],
        [
            ["空调", "开关、制冷/制热、目标温度（整度 ℃）"],
            ["风扇", "低 / 中 / 高风速（部分手机界面会单独显示）"],
            ["湿度", "相对湿度显示（需选配温湿度传感）"],
            ["氛围灯", "开关与亮度调节"],
        ],
        [40, 134],
    )
    pdf.note(
        "半度温度、扫风摆叶、除湿/仅通风等高级功能，可能受智能家居标准与空调协议限制，"
        "表现可能与原装遥控器不完全一致。设备会尽量将手机指令映射为可用的红外控制。"
    )

    # ——— 8 氛围灯光 ———
    pdf.h1("8. 氛围灯光")
    pdf.p(
        "灯环既是产品外观的一部分，也可作为柔和的环境照明。"
        "您可在圆屏灯光页选择模式，或在手机 App 中开关灯环并调节亮度。"
    )
    pdf.figs_row(
        [img("product_studio_front_pdf.jpg"), img("ui_light_on_device_pdf.jpg")],
        ["图 8-1  灯环点亮效果", "图 8-2  灯光设置界面"],
        gap=8,
    )
    pdf.bullets(
        [
            "睡眠场景建议选择「夜间关闭」，避免灯光影响休息。",
            "「温感呼吸」适合日常使用，色调随室内温度氛围变化。",
            "学习空调时灯环会临时变为黄色呼吸，学习结束后自动恢复原模式。",
        ]
    )

    # ——— 9 恢复出厂 ———
    pdf.h1("9. 恢复出厂设置")
    pdf.p(
        "当需要更换家庭网络、重新绑定其他空调，或设备状态异常时，可恢复出厂设置。"
        "恢复后，智能家居配网信息与空调绑定记录将被清除。"
    )
    pdf.numbered(
        [
            "找到机身复位键，长按约 5 秒。",
            "松开后，设备将清除配网与空调绑定数据并重启。",
            "重新上电后，请按第 4 章重新配网，并按第 5 章重新绑定空调。",
        ]
    )
    pdf.note("恢复出厂不会损坏硬件。完成后需重新完成配网与空调绑定，方可继续使用手机控制。")

    # ——— 10 常见问题 ———
    pdf.h1("10. 常见问题")
    pdf.table(
        ["现象", "处理建议"],
        [
            ["手机找不到设备", "确认手机连接 2.4 GHz Wi-Fi；靠近路由器后重试；必要时恢复出厂后重新配网"],
            ["屏上二维码扫不进", "确认仍处于配网页；重新上电刷新二维码；或改用数字配对码"],
            ["学习时无反应", "先点「开始学习」再按遥控；对准设备；检查遥控器电池"],
            ["已学习但空调不动", "调整设备朝向空调接收窗；缩短距离、排除遮挡后重试"],
            ["触摸无反应", "使用手指直接触控；避免过厚贴膜；尝试重新上电"],
            ["氛围灯不亮", "确认未选择「夜间关闭」；检查手机端灯是否关闭；适当提高亮度"],
            ["温度/湿度不准或无显示", "温湿度为选配功能；未安装传感器时仍可正常控制空调"],
            ["想换绑另一台空调", "恢复出厂设置后，重新配网并对新空调执行红外学习"],
        ],
        [48, 126],
    )

    # ——— 11 规格 ———
    pdf.h1("11. 产品规格")
    pdf.table(
        ["项目", "说明"],
        [
            ["产品名称", "aidaegis AC Remote"],
            ["显示", "1.28 英寸圆形触摸屏"],
            ["网络", "2.4 GHz Wi-Fi，Matter 本地智能家居控制"],
            ["控制方式", "红外控制空调；圆屏触控；手机 App"],
            ["氛围照明", "环形氛围灯（开关、亮度与多种模式）"],
            ["选配传感", "温湿度传感（如已配置）"],
            ["界面语言", "中文（默认）/ 英文"],
            ["供电", "底座后方电源接口（请使用合格电源适配器）"],
            ["适用环境", "干燥室内桌面放置"],
        ],
        [40, 134],
    )

    # ——— 12 安全 ———
    pdf.h1("12. 安全与注意事项")
    pdf.bullets(
        [
            "仅在干燥的室内环境使用，避免进水、凝露与高湿环境。",
            "请勿遮挡灯环、屏幕及红外收发区域。",
            "请使用符合当地安全标准的电源适配器与线缆。",
            "儿童进行配网或恢复出厂等操作时，请在成人指导下进行。",
            "请勿自行拆解设备；非专业维修可能导致损坏或安全风险。",
            "本说明书仅描述用户日常操作；功能以设备实际表现为准。",
        ]
    )

    pdf.ln(10)
    pdf.set_draw_color(*C_LINE)
    pdf.set_line_width(0.3)
    pdf.line(pdf.l_margin + 40, pdf.get_y(), pdf.w - pdf.r_margin - 40, pdf.get_y())
    pdf.ln(8)
    pdf.set_font("cn", "", 9)
    pdf.set_text_color(*C_MUTED)
    pdf.set_x(pdf.l_margin)
    pdf.multi_cell(
        pdf.epw,
        5,
        "© aidaegis\n"
        "本说明书内容如有更新，以产品随附或官网最新版本为准。\n"
        "品牌与产品名称：aidaegis AC Remote",
        align="C",
    )

    OUT.parent.mkdir(parents=True, exist_ok=True)
    pdf.output(OUT)
    return OUT


if __name__ == "__main__":
    path = build()
    print(f"Wrote {path} ({path.stat().st_size} bytes)")
