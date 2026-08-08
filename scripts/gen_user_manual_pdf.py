#!/usr/bin/env python3
"""Generate docs/aidaegis_ac_remote_user_manual.pdf (Chinese user guide)."""

from __future__ import annotations

from pathlib import Path

from fpdf import FPDF
from fontTools.ttLib import TTCollection

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "docs" / "aidaegis_ac_remote_user_manual.pdf"
WQY_TTC = Path("/usr/share/fonts/truetype/wqy/wqy-microhei.ttc")


def resolve_font() -> Path:
    """WenQuanYi Micro Hei covers CJK + Latin; DroidSansFallback is CJK-only."""
    cached = ROOT / "docs" / "fonts" / "wqy-microhei.ttf"
    if cached.is_file():
        return cached
    if WQY_TTC.is_file():
        cached.parent.mkdir(parents=True, exist_ok=True)
        TTCollection(str(WQY_TTC)).fonts[0].save(str(cached))
        return cached
    raise SystemExit("Need WenQuanYi Micro Hei (fonts-wqy-microhei) to build the PDF")


FONT = resolve_font()


class ManualPDF(FPDF):
    def __init__(self) -> None:
        super().__init__(format="A4", unit="mm")
        self.set_auto_page_break(auto=True, margin=18)
        self.add_font("cn", "", str(FONT))
        self.add_font("cn", "B", str(FONT))

    def header(self) -> None:
        if self.page_no() == 1:
            return
        self.set_font("cn", "", 9)
        self.set_text_color(110, 110, 110)
        self.cell(0, 8, "aidaegis AC Remote · 用户使用说明", align="L")
        self.ln(4)
        self.set_draw_color(200, 200, 200)
        self.line(self.l_margin, 14, self.w - self.r_margin, 14)
        self.ln(6)
        self.set_text_color(20, 20, 20)

    def footer(self) -> None:
        self.set_y(-14)
        self.set_font("cn", "", 8)
        self.set_text_color(130, 130, 130)
        self.cell(0, 8, f"{self.page_no()}", align="C")

    def _reset_x(self) -> None:
        self.set_x(self.l_margin)

    def cover(self) -> None:
        self.add_page()
        self.ln(42)
        self.set_font("cn", "B", 28)
        self.set_text_color(24, 48, 72)
        self.set_x(self.l_margin)
        self.multi_cell(self.epw, 14, "aidaegis", align="C")
        self.set_font("cn", "B", 22)
        self.set_x(self.l_margin)
        self.multi_cell(self.epw, 12, "AC Remote", align="C")
        self.ln(6)
        self.set_draw_color(24, 48, 72)
        self.set_line_width(0.4)
        self.line(70, self.get_y(), 140, self.get_y())
        self.ln(10)
        self.set_font("cn", "", 16)
        self.set_text_color(40, 40, 40)
        self.set_x(self.l_margin)
        self.multi_cell(self.epw, 10, "Matter 空调红外控制器", align="C")
        self.set_font("cn", "", 12)
        self.set_text_color(90, 90, 90)
        self.set_x(self.l_margin)
        self.multi_cell(self.epw, 8, "用户使用说明", align="C")
        self.ln(18)
        self.set_font("cn", "", 10)
        self.set_x(self.l_margin)
        self.multi_cell(
            self.epw,
            6,
            "适用于 ESP32-S3 + 1.28″ 圆屏（GC9A01 / IT7259）固件\n"
            "支持 Apple Home / Google Home / Home Assistant（Matter）\n"
            "固件版本：3.0-s3-ui",
            align="C",
        )
        self.ln(30)
        self.set_font("cn", "", 9)
        self.set_text_color(120, 120, 120)
        self.set_x(self.l_margin)
        self.multi_cell(
            self.epw,
            5,
            "请先阅读「放置建议」与「首次使用」两节，再进行配网与红外学习。",
            align="C",
        )

    def h1(self, title: str) -> None:
        self.ln(4)
        self._reset_x()
        self.set_font("cn", "B", 16)
        self.set_text_color(24, 48, 72)
        self.multi_cell(0, 9, title)
        self._reset_x()
        self.set_draw_color(24, 48, 72)
        self.set_line_width(0.5)
        y = self.get_y()
        self.line(self.l_margin, y, self.l_margin + 50, y)
        self.ln(4)
        self.set_text_color(30, 30, 30)

    def h2(self, title: str) -> None:
        self.ln(2)
        self._reset_x()
        self.set_font("cn", "B", 12)
        self.set_text_color(36, 64, 96)
        self.multi_cell(0, 7, title)
        self._reset_x()
        self.set_text_color(30, 30, 30)

    def p(self, text: str) -> None:
        self._reset_x()
        self.set_font("cn", "", 10)
        self.multi_cell(0, 6, text)
        self._reset_x()
        self.ln(1)

    def bullets(self, items: list[str]) -> None:
        self.set_font("cn", "", 10)
        for item in items:
            self._reset_x()
            self.multi_cell(0, 6, f"•  {item}")
        self._reset_x()
        self.ln(1)

    def numbered(self, items: list[str]) -> None:
        self.set_font("cn", "", 10)
        for i, item in enumerate(items, 1):
            self._reset_x()
            self.multi_cell(0, 6, f"{i}.  {item}")
        self._reset_x()
        self.ln(1)

    def note(self, text: str) -> None:
        self._reset_x()
        self.set_fill_color(245, 248, 252)
        self.set_draw_color(180, 200, 220)
        self.set_font("cn", "", 9)
        start = self.get_y()
        self.set_x(self.l_margin + 2)
        self.multi_cell(self.epw - 4, 5.5, f"提示：{text}")
        end = self.get_y()
        self.rect(self.l_margin, start - 1.5, self.epw, end - start + 3)
        self.set_y(end + 3)
        self._reset_x()
        self.set_text_color(30, 30, 30)

    def table(self, headers: list[str], rows: list[list[str]], col_w: list[float]) -> None:
        self._reset_x()
        self.set_font("cn", "B", 9)
        self.set_fill_color(24, 48, 72)
        self.set_text_color(255, 255, 255)
        for h, w in zip(headers, col_w):
            self.cell(w, 7, h, border=1, fill=True, align="C")
        self.ln()
        self._reset_x()
        self.set_font("cn", "", 9)
        self.set_text_color(30, 30, 30)
        fill = False
        for row in rows:
            self.set_fill_color(248, 250, 252)
            heights = [max(7, self._estimate_lines(text, w) * 5 + 2) for text, w in zip(row, col_w)]
            row_h = max(heights)
            if self.get_y() + row_h > self.page_break_trigger:
                self.add_page()
                self._reset_x()
                self.set_font("cn", "B", 9)
                self.set_fill_color(24, 48, 72)
                self.set_text_color(255, 255, 255)
                for h, w in zip(headers, col_w):
                    self.cell(w, 7, h, border=1, fill=True, align="C")
                self.ln()
                self._reset_x()
                self.set_font("cn", "", 9)
                self.set_text_color(30, 30, 30)
            y0 = self.get_y()
            x0 = self.l_margin
            self.set_x(x0)
            for text, w in zip(row, col_w):
                x = self.get_x()
                self.rect(x, y0, w, row_h, style="DF" if fill else "D")
                self.set_xy(x + 1, y0 + 1)
                self.multi_cell(w - 2, 5, text, border=0)
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


def build() -> Path:
    pdf = ManualPDF()
    pdf.set_title("aidaegis AC Remote 用户使用说明")
    pdf.set_author("aidaegis")
    pdf.set_creator("bc7215_ac_matter")

    pdf.cover()

    pdf.add_page()
    pdf.h1("1. 产品简介")
    pdf.p(
        "AC Remote 是一款 Matter 本地智能空调控制器。它通过红外控制家中空调，"
        "并通过 Wi-Fi Matter 接入 Apple Home、Google Home 或 Home Assistant，"
        "无需依赖空调厂商云服务。"
    )
    pdf.p("本机配备 1.28″ 圆形触摸屏，可在屏上完成配网扫码、红外学习、空调控制与氛围灯设置。")
    pdf.h2("主要能力")
    pdf.bullets(
        [
            "Matter 配网：屏上动态二维码与数字配对码",
            "红外学习：用原装遥控器对机学习协议",
            "空调控制：开关、制冷/制热、设定温度、风扇档位（屏上与手机双向同步）",
            "氛围灯：Matter 开关与亮度；多种灯光模式仅在屏上选择",
            "可选 SHT30：上报真实室温与湿度",
            "中英文界面切换（默认中文）",
        ]
    )
    pdf.note(
        "本产品仅支持 IRremoteESP8266 库已实现的空调协议，不是“任意遥控万能学习”。"
        "若遥控协议不在支持列表中，自动学习会失败，可尝试 BOOT 双击协议遍历。"
    )

    pdf.h1("2. 放置建议")
    pdf.p("设备同时承担“对空调发红外”和“接收原装遥控红外”两项工作，放置位置很重要：")
    pdf.numbered(
        [
            "放在空调附近，红外发射管朝向空调出风口/机体接收窗方向。",
            "红外接收头大致朝向日常使用遥控器的人（客厅/床位方向）。",
            "避免金属遮挡与强阳光直射接收头；保持供电稳定。",
            "手机与设备须使用 2.4 GHz Wi-Fi（Matter over Wi-Fi 不支持 5 GHz）。",
        ]
    )

    pdf.h1("3. 首次使用：Matter 配网")
    pdf.numbered(
        [
            "给设备上电。未入网时，圆屏显示「Matter 配网」页、二维码与数字配对码。",
            "确认手机已连接 2.4 GHz 家庭 Wi-Fi。",
            "打开 Apple「家庭」/ Google Home / Home Assistant Companion，选择添加 Matter 配件。",
            "扫描屏上二维码，或手动输入屏上数字配对码。",
            "若提示“未认证 / 未经验证的配件”，按系统指引继续即可（DIY/自研固件常见提示）。",
            "配网成功后，手机中可见设备名默认为「AC Remote」，厂商为 aidaegis。",
        ]
    )
    pdf.note(
        "屏上二维码内容由固件实时生成，与串口日志中的 MT:... 一致。"
        "请扫当前屏上显示的码，勿使用说明书印刷样例。"
    )

    pdf.h1("4. 红外学习（绑定空调）")
    pdf.p("完成 Matter 配网后，若尚未学码，屏幕会进入「红外学习」页。")
    pdf.numbered(
        [
            "点按「开始学习」。此时 WS2812 氛围灯呈黄色呼吸，表示正在等待遥控信号。",
            "用原装遥控器对准本机红外接收头，按任意键（建议：制冷、25℃、任意风速）。",
            "学习成功后自动进入空调控制页。",
            "若失败，可能是协议不受支持；请参考第 7 节「BOOT 双击协议遍历」。",
        ]
    )
    pdf.note("BOOT 按键单击不再进入学习。学习入口仅在触摸屏「开始学习」。")

    pdf.h1("5. 圆屏操作")
    pdf.h2("5.1 空调页")
    pdf.bullets(
        [
            "开启 / 关闭空调",
            "降温 / 升温（整度调节）",
            "状态与手机 Matter 控制器双向同步",
            "提示「左滑灯光」可进入氛围灯页",
        ]
    )
    pdf.h2("5.2 灯光页")
    pdf.p("在空调页向左滑动进入灯光页；向右滑动返回。")
    pdf.table(
        ["屏上模式", "效果说明"],
        [
            ["夜间关闭", "灯灭"],
            ["手动亮度", "白光，跟随亮度滑条 / Matter 亮度"],
            ["温感呼吸（默认）", "按室温绿→橙呼吸（约 2.5 秒）"],
            ["纯色", "按温感色固定点亮"],
            ["彩虹", "色相循环"],
            ["呼吸白", "白光呼吸"],
            ["学习中（临时）", "强制黄色呼吸，学习结束后恢复"],
        ],
        [45, 145],
    )
    pdf.p(
        "亮度滑条对应 Matter LevelControl。手机 App 可开关灯与调节亮度；"
        "彩虹、呼吸等氛围模式仅本地屏上选择，一般不会出现在手机 Home 界面。"
    )
    pdf.h2("5.3 语言切换")
    pdf.p("点按右上角「EN / 中文」可在中文与英文界面间切换。出厂默认中文。")

    pdf.h1("6. 手机 App 控制")
    pdf.p("配网并完成红外学习后，可在手机中控制：")
    pdf.table(
        ["端点", "可控制内容"],
        [
            ["空调（Room AC）", "开关、制冷/制热、目标温度（整度 ℃）"],
            ["风扇（Fan）", "低 / 中 / 高风速（部分手机界面会单独显示）"],
            ["湿度传感器", "相对湿度（需安装 SHT30）"],
            ["氛围灯（Dimmable Light）", "开关 + 亮度"],
        ],
        [50, 140],
    )
    pdf.note(
        "半度温度、扫风摆叶、除湿/仅通风等，受 Matter HVAC 表达与红外协议能力限制，"
        "可能弱于原装遥控器。收到手机命令时，固件会尽量映射到可用红外模式。"
    )

    pdf.h1("7. BOOT 按键")
    pdf.table(
        ["操作", "作用"],
        [
            ["单击", "无功能（不进入学习）"],
            ["双击", "Alt 协议遍历：依次尝试库内候选协议"],
            ["长按约 5 秒", "恢复出厂（清除红外配对与 Matter 配网）"],
        ],
        [45, 145],
    )
    pdf.h2("双击协议遍历步骤")
    pdf.numbered(
        [
            "自动学习失败时，双击 BOOT。",
            "设备按列表发送“制冷 / 25℃”测试帧；观察空调是否有反应（如滴一声）。",
            "空调有反应后，在手机上发任意 Matter 命令（如改温度）以确认当前协议。",
            "状态灯闪烁次数对应协议序号；试完一轮仍无效则回到未配对状态。",
        ]
    )

    pdf.h1("8. 状态指示灯（GPIO11）")
    pdf.p("机身上的状态 LED（低电平点亮）与 WS2812 氛围灯相互独立。")
    pdf.table(
        ["灯效", "含义"],
        [
            ["常亮", "出厂 / 尚未配置完成"],
            ["常灭", "未上电，或 Alt 遍历进行中"],
            ["快闪", "等待红外学习信号，或恢复出厂提示"],
            ["慢闪", "正在建立网络连接"],
            ["每秒闪 1 次", "网络已配好，空调尚未红外配对"],
            ["每秒闪 2 次", "空调已配对，等待手机 Matter 连接/订阅"],
            ["约每 3 秒闪一下", "配对与连接完成，待机"],
        ],
        [45, 145],
    )

    pdf.h1("9. 恢复出厂")
    pdf.numbered(
        [
            "长按 BOOT 约 5 秒，状态灯快闪。",
            "松开按键后，设备清除红外配对与 Matter 配网数据。",
            "重新上电后，从第 3 节再次配网，并从第 4 节重新学习红外。",
        ]
    )
    pdf.note("若需重新生成设备序列号，需擦除 Flash / 清除 factory 分区后再烧录固件（面向开发者操作）。")

    pdf.h1("10. 常见问题")
    pdf.table(
        ["现象", "处理建议"],
        [
            ["手机搜不到设备", "确认 2.4 GHz Wi-Fi；恢复出厂后重配；靠近路由器"],
            ["屏上码扫不进", "确认未入网页；重新上电刷新二维码；或改输数字码"],
            ["学习无反应", "对准接收头；先点「开始学习」；检查遥控电池"],
            ["学到了但空调不动", "发射管朝向空调；检查距离/遮挡；试双击协议遍历"],
            ["触摸无反应", "确认手指触控；重启设备；检查是否贴膜过厚"],
            ["氛围灯不亮", "确认灯光页未选「夜间关闭」；手机端灯是否关闭；亮度是否过低"],
            ["温度不准/无湿度", "需安装 SHT30；无传感器时仍可控制空调"],
            ["想换一台空调", "恢复出厂或重新进入学习流程后，对新空调再学一次"],
        ],
        [50, 140],
    )

    pdf.h1("11. 规格摘要")
    pdf.table(
        ["项目", "说明"],
        [
            ["品牌 / 设备名", "aidaegis / AC Remote"],
            ["主控", "ESP32-S3-WROOM-1-N16（16 MB Flash）"],
            ["显示", "1.28″ 圆屏 240×240，GC9A01 + IT7259 触摸"],
            ["网络", "2.4 GHz Wi-Fi，Matter 本地控制"],
            ["红外", "外接发射管 + 38 kHz 接收头"],
            ["氛围灯", "WS2812（开关 + 亮度；模式本地）"],
            ["可选传感", "SHT30 温湿度"],
            ["界面语言", "中文（默认）/ 英文"],
        ],
        [45, 145],
    )

    pdf.h1("12. 安全与注意")
    pdf.bullets(
        [
            "仅在干燥室内使用，避免进水与凝露。",
            "勿遮挡散热与红外收发窗口。",
            "儿童使用时请在成人指导下操作配网与恢复出厂。",
            "本说明书描述的是用户操作；固件编译烧录请参阅仓库 README（开发者文档）。",
        ]
    )
    pdf.ln(6)
    pdf.set_font("cn", "", 9)
    pdf.set_text_color(100, 100, 100)
    pdf.set_x(pdf.l_margin)
    pdf.multi_cell(
        pdf.epw,
        5,
        "文档版本与固件 3.0-s3-ui 对应。功能以实际固件为准。\n"
        "项目主页：https://github.com/realDavy/bc7215_ac_matter",
        align="C",
    )

    OUT.parent.mkdir(parents=True, exist_ok=True)
    pdf.output(OUT)
    return OUT


if __name__ == "__main__":
    path = build()
    print(f"Wrote {path} ({path.stat().st_size} bytes)")
