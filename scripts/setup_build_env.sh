#!/usr/bin/env bash
# Build environment helper for ac_matter (ESP-IDF + ESP-Matter + China registry).
#
# ONE-TIME install (run / execute — installs `gn` etc.):
#   bash scripts/setup_build_env.sh --install
#
# EVERY new terminal before build (must SOURCE):
#   . scripts/setup_build_env.sh
#
# Path overrides if your trees are elsewhere:
#   export IDF_PATH=~/esp-adf/esp-idf
#   export ESP_MATTER_PATH=~/esp-adf/esp-idf/esp-matter

_AC_MATTER_SETUP_INSTALL=0
for _ac_arg in "$@"; do
  case "$_ac_arg" in
    --install) _AC_MATTER_SETUP_INSTALL=1 ;;
  esac
done
unset _ac_arg

: "${IDF_PATH:=$HOME/esp-adf/esp-idf}"
: "${ESP_MATTER_PATH:=$HOME/esp-adf/esp-idf/esp-matter}"
: "${IDF_COMPONENT_REGISTRY_URL:=https://components-file.espressif.cn}"
export IDF_PATH ESP_MATTER_PATH IDF_COMPONENT_REGISTRY_URL

_ac_matter_setup_fail() {
  echo "ERROR: $*" >&2
  return 1
}

if [[ ! -f "$IDF_PATH/export.sh" ]]; then
  _ac_matter_setup_fail "IDF_PATH invalid: $IDF_PATH"
  return 1 2>/dev/null || exit 1
fi
if [[ ! -f "$ESP_MATTER_PATH/export.sh" ]]; then
  _ac_matter_setup_fail "ESP_MATTER_PATH invalid: $ESP_MATTER_PATH"
  return 1 2>/dev/null || exit 1
fi

echo "==> IDF_PATH=$IDF_PATH"
echo "==> ESP_MATTER_PATH=$ESP_MATTER_PATH"
echo "==> IDF_COMPONENT_STORAGE_URL=$IDF_COMPONENT_STORAGE_URL"
echo "==> IDF_COMPONENT_REGISTRY_URL=${IDF_COMPONENT_REGISTRY_URL:-<default components.espressif.com>}"

# shellcheck disable=SC1091
. "$IDF_PATH/export.sh" || {
  _ac_matter_setup_fail "failed to source IDF export.sh"
  return 1 2>/dev/null || exit 1
}

if [[ "$_AC_MATTER_SETUP_INSTALL" -eq 1 ]]; then
  echo "==> Running esp-matter ./install.sh (needs network; installs gn / CHIP deps)"
  (
    cd "$ESP_MATTER_PATH" && ./install.sh
  ) || {
    _ac_matter_setup_fail "esp-matter install.sh failed"
    return 1 2>/dev/null || exit 1
  }
fi
unset _AC_MATTER_SETUP_INSTALL

# shellcheck disable=SC1091
. "$ESP_MATTER_PATH/export.sh" || {
  _ac_matter_setup_fail "failed to source esp-matter export.sh"
  return 1 2>/dev/null || exit 1
}

if ! command -v idf.py >/dev/null 2>&1; then
  _ac_matter_setup_fail "idf.py not found after export"
  return 1 2>/dev/null || exit 1
fi
if ! command -v gn >/dev/null 2>&1; then
  _ac_matter_setup_fail "gn not found. Run once: bash scripts/setup_build_env.sh --install"
  return 1 2>/dev/null || exit 1
fi

echo "==> OK idf.py: $(command -v idf.py)"
echo "==> OK gn:     $(command -v gn)"
echo
echo "Same shell next:"
echo "  cd \"$ESP_MATTER_PATH/examples/ac_matter\"   # or your clone path"
echo "  idf.py set-target esp32s3 && idf.py build"
