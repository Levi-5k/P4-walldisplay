#!/usr/bin/env bash
set -euo pipefail

# Build the esp_hosted slave firmware for ESP32-C6 using PlatformIO's ESP-IDF.
# Produces: tools/slave_build/build/network_adapter.bin (+ bootloader, partitions)

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
SLAVE_SRC="$PROJECT_ROOT/managed_components/espressif__esp_hosted/slave"
BUILD_DIR="$SCRIPT_DIR/slave_build"

# PlatformIO-installed tools
PIO_HOME="$HOME/.platformio"
export IDF_PATH="$PIO_HOME/packages/framework-espidf"
TOOLCHAIN_DIR="$PIO_HOME/packages/toolchain-riscv32-esp/bin"
CMAKE_BIN="$PIO_HOME/packages/tool-cmake/bin"
NINJA_BIN="$PIO_HOME/packages/tool-ninja"
PYTHON="$PIO_HOME/penv/bin/python3"

export PATH="$TOOLCHAIN_DIR:$CMAKE_BIN:$NINJA_BIN:$PATH"

IDF_TARGET=esp32c6

echo "=== Building esp_hosted slave FW for $IDF_TARGET ==="
echo "IDF_PATH=$IDF_PATH"
echo "Toolchain: $TOOLCHAIN_DIR"
echo "Source: $SLAVE_SRC"
echo "Build: $BUILD_DIR"

mkdir -p "$BUILD_DIR"

# Merge sdkconfig defaults: base + target + SDIO transport
SDKCONFIG="$BUILD_DIR/sdkconfig"
if [ ! -f "$SDKCONFIG" ]; then
    cat "$SLAVE_SRC/sdkconfig.defaults" \
        "$SLAVE_SRC/sdkconfig.defaults.$IDF_TARGET" \
        "$SLAVE_SRC/sdkconfig.ci.sdio" \
        > "$BUILD_DIR/sdkconfig.combined"
    echo "# Generated merged sdkconfig defaults" > "$SDKCONFIG"
fi

# Run idf.py via Python (avoids needing install.sh / export.sh)
cd "$SLAVE_SRC"

$PYTHON "$IDF_PATH/tools/idf.py" \
    -B "$BUILD_DIR" \
    -DIDF_TARGET="$IDF_TARGET" \
    -DSDKCONFIG_DEFAULTS="$BUILD_DIR/sdkconfig.combined" \
    set-target "$IDF_TARGET"

$PYTHON "$IDF_PATH/tools/idf.py" \
    -B "$BUILD_DIR" \
    build 2>&1

echo ""
echo "=== Build complete ==="
ls -lh "$BUILD_DIR/network_adapter.bin" 2>/dev/null || echo "Binary not found at expected path"
ls -lh "$BUILD_DIR/bootloader/bootloader.bin" 2>/dev/null || true
ls -lh "$BUILD_DIR/partition_table/partition-table.bin" 2>/dev/null || true
