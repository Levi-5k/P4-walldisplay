#!/usr/bin/env sh
set -eu

overlay_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
upstream_dir=${1:-"$overlay_dir/upstream"}

if [ ! -d "$upstream_dir" ]; then
    echo "WLED upstream checkout not found: $upstream_dir" >&2
    echo "Clone WLED-MM/MoonModules there first, or pass the checkout path as the first argument." >&2
    exit 1
fi

mkdir -p "$upstream_dir/usermods"
mkdir -p "$upstream_dir/pio-scripts"
mkdir -p "$upstream_dir/boards"
rm -rf "$upstream_dir/usermods/86box_rs485_bridge"
cp -R "$overlay_dir/usermods/86box_rs485_bridge" "$upstream_dir/usermods/"
cp "$overlay_dir/pio-scripts/86box_esptool_compat.py" "$upstream_dir/pio-scripts/86box_esptool_compat.py"
cp "$overlay_dir/boards/waveshare_esp32s3_relay_1ch.json" "$upstream_dir/boards/waveshare_esp32s3_relay_1ch.json"
cp "$overlay_dir/platformio_override.sample.ini" "$upstream_dir/platformio_override.ini"

cat > "$upstream_dir/esptool.py" <<'PY'
#!/usr/bin/env python3
import os
import sys

tool_dirs = [
  os.path.expanduser("~/.platformio/packages/tool-esptoolpy"),
  os.path.expanduser("~/.platformio/tools/tool-esptoolpy"),
]
tool_dir = next((path for path in tool_dirs if os.path.isdir(os.path.join(path, "esptool"))), None)
if not tool_dir:
  sys.stderr.write("PlatformIO tool-esptoolpy package not found\n")
  sys.exit(1)

script_dir = os.path.dirname(os.path.abspath(__file__))
sys.path = [tool_dir] + [path for path in sys.path if os.path.abspath(path or ".") != script_dir]

import esptool
esptool._main()
PY
chmod +x "$upstream_dir/esptool.py"

cat <<'MSG'
Overlay files copied.

This WLED 16 non-audio overlay uses custom_usermods and REGISTER_USERMOD, so no
manual wled00/usermods_list.cpp edit is needed.

Then build from the upstream checkout:

  pio run -e S3_WLED_Host -t clean
  pio run -e S3_WLED_Host
MSG
