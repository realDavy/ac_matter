#!/usr/bin/env bash
# Regenerate LVGL Chinese+ASCII fonts used by the round UI.
# Requires: npm package lv_font_conv, DejaVuSans, DroidSansFallbackFull.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$ROOT/main/ui"
LATIN="${LATIN_FONT:-/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf}"
CJK="${CJK_FONT:-/usr/share/fonts/truetype/droid/DroidSansFallbackFull.ttf}"

# CJK subset for ui_i18n.h (+ common extras). Keep ASCII/° on the Latin face.
SYMBOLS='上中习亮件任光入关准分切加动升合后启吸呼围在外夜始学完对左并应度开式彩意感成或手扫按换控文新方显机正氛添温滑灯用白码示空红纯组网置色虹设请调输遥配重键闭间降需正在'

gen() {
  local size="$1" name="$2"
  npx --yes lv_font_conv \
    --font "$LATIN" -r 0x20-0x7E -r 0xB0-0xB0 \
    --font "$CJK" --symbols "$SYMBOLS" \
    --size "$size" --bpp 4 --format lvgl \
    --lv-font-name "$name" --lv-include lvgl.h --no-compress \
    -o "$OUT/${name}.c"
  echo "wrote $OUT/${name}.c"
}

gen 16 ui_font_cn_16
gen 20 ui_font_cn_20
gen 28 ui_font_cn_28
