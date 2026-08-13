#!/usr/bin/env bash
# Regenerate LVGL Chinese+ASCII fonts used by the round UI.
# Prefers WenQuanYi Micro Hei (finer at small sizes) over DroidSansFallback.
# Requires: npm lv_font_conv, DejaVuSans, and either WQY Micro Hei or DroidSans.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$ROOT/main/ui"
LATIN="${LATIN_FONT:-/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf}"

# Resolve CJK face (TTC → temp TTF when needed).
CJK_TTC="${CJK_TTC:-/usr/share/fonts/truetype/wqy/wqy-microhei.ttc}"
CJK_TTF="${CJK_FONT:-/usr/share/fonts/truetype/droid/DroidSansFallbackFull.ttf}"
CJK_TMP=""
cleanup() { [[ -n "${CJK_TMP}" && -f "${CJK_TMP}" ]] && rm -f "${CJK_TMP}"; }
trap cleanup EXIT

if [[ -f "${CJK_TTC}" ]]; then
  CJK_TMP="$(mktemp /tmp/wqy-microhei.XXXXXX.ttf)"
  python3 - <<PY
try:
    from fontTools.ttLib import TTCollection
except ImportError:
    import subprocess, sys
    subprocess.check_call([sys.executable, "-m", "pip", "install", "fonttools", "-q"])
    from fontTools.ttLib import TTCollection
TTCollection("${CJK_TTC}").fonts[0].save("${CJK_TMP}")
print("using WQY Micro Hei from ${CJK_TTC}")
PY
  CJK="${CJK_TMP}"
elif [[ -f "${CJK_TTF}" ]]; then
  CJK="${CJK_TTF}"
  echo "using CJK font ${CJK}"
else
  echo "No CJK font found (WQY Micro Hei or DroidSansFallback)" >&2
  exit 1
fi

# CJK subset for ui_i18n.h (+ AC dial labels 当前/至). Keep ASCII/° on Latin face.
SYMBOLS='上中习亮件任光入关准分切加动升合后启吸呼围在外夜始学完对左并应度开式彩意感成或手扫按换控文新方显机正氛添温滑灯用白码示空红纯组网置色虹设请调输遥配重键闭间降需正在当前至'

gen() {
  local size="$1" name="$2" bpp="$3"
  npx --yes lv_font_conv \
    --font "$LATIN" -r 0x20-0x7E -r 0xB0-0xB0 \
    --font "$CJK" --symbols "$SYMBOLS" \
    --size "$size" --bpp "$bpp" --format lvgl \
    --lv-font-name "$name" --lv-include lvgl.h --no-compress \
    --autohint-strong \
    -o "$OUT/${name}.c"
  echo "wrote $OUT/${name}.c (size=${size} bpp=${bpp})"
}

# bpp 8 + slightly larger px sizes for smoother strokes on the 1.28" panel.
gen 18 ui_font_cn_18 8
gen 22 ui_font_cn_22 8
gen 30 ui_font_cn_30 8

echo "Note: if you add a new size, also add the .c to main/CMakeLists.txt COMPONENT_SRCS,"
echo "then run: idf.py reconfigure && idf.py build"
echo "Also ensure CONFIG_LV_COLOR_16_SWAP=y (GC9A01 SPI) or AA fonts show rainbow bands."
