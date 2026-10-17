#!/usr/bin/env bash
# Run from any directory: scripts/idf.sh menuconfig | build | -p PORT flash monitor
set -e
# The ESP-IDF component manager keeps bare git caches; a global safe.bareRepository=explicit breaks it.
# Override for this process only, preserving any GIT_CONFIG_COUNT entries already set.
count="${GIT_CONFIG_COUNT:-0}"
export "GIT_CONFIG_KEY_${count}=safe.bareRepository" "GIT_CONFIG_VALUE_${count}=all" GIT_CONFIG_COUNT=$((count + 1))
project_dir="$(cd "$(dirname "$0")/.." && pwd)"
idf_root="${IDF_PATH:-$HOME/esp/v6.0.1/esp-idf}"
if [[ ! -f "$idf_root/export.sh" ]]; then
    echo "Set IDF_PATH to an ESP-IDF 6.0.1 checkout." >&2
    exit 1
fi
source "$idf_root/export.sh" >/dev/null
# Some C6-only installations have the Xtensa compiler installed but omit it from PATH.
if ! command -v xtensa-esp32s3-elf-gcc >/dev/null; then
    tool_version="$(python - "$idf_root/tools/tools.json" <<'PY'
import json, sys
with open(sys.argv[1]) as source:
    tools = json.load(source)['tools']
print(next(v['name'] for t in tools if t['name'] == 'xtensa-esp-elf'
           for v in t['versions'] if v['status'] == 'recommended'))
PY
)"
    for tools_root in "${IDF_TOOLS_PATH:-$HOME/.espressif}" "$HOME/esp/tools"; do
        for bin_dir in "$tools_root/xtensa-esp-elf/$tool_version/xtensa-esp-elf/bin" \
                       "$tools_root/tools/xtensa-esp-elf/$tool_version/xtensa-esp-elf/bin"; do
            if [[ -x "$bin_dir/xtensa-esp32s3-elf-gcc" ]]; then
                export PATH="$bin_dir:$PATH"
                break 2
            fi
        done
    done
fi
if ! command -v xtensa-esp32s3-elf-gcc >/dev/null; then
    echo "Install the ESP32-S3 tools with: $idf_root/install.sh esp32s3" >&2
    exit 1
fi
cd "$project_dir"
exec idf.py "$@"
