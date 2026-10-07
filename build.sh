#!/usr/bin/env bash
set -euo pipefail

# Ensure ~/.local/bin is in PATH (common location for pipx / pio)
export PATH="$HOME/.local/bin:$PATH"

# ---------------------------------------------------------------------------
# Version argument (required)
# ---------------------------------------------------------------------------
if [ $# -lt 1 ]; then
    echo "Usage: $0 <version>  (e.g. $0 1.0.3)" >&2
    exit 1
fi
VERSION="$1"

# Resolve directories
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

if [ -f "$SCRIPT_DIR/platformio.ini" ]; then
    FIRMWARE_DIR="$SCRIPT_DIR"
    ROOT_DIR="$(dirname "$SCRIPT_DIR")"
elif [ -d "$SCRIPT_DIR/firmware_source" ] && [ -f "$SCRIPT_DIR/firmware_source/platformio.ini" ]; then
    FIRMWARE_DIR="$SCRIPT_DIR/firmware_source"
    ROOT_DIR="$SCRIPT_DIR"
else
    echo "Error: Cannot locate firmware_source with platformio.ini." >&2
    exit 1
fi

BUILD_DIR="$FIRMWARE_DIR/.pio/build/esp32-s3-devkitm-1"
OUTPUT_DIR="$ROOT_DIR/dist"

echo "=========================================="
echo " Diptyx Firmware Build"
echo "=========================================="
echo "Firmware source: $FIRMWARE_DIR"
echo "Version:         $VERSION"
echo ""

# Check for PlatformIO
if ! command -v pio &> /dev/null && ! command -v platformio &> /dev/null; then
    echo "Error: PlatformIO Core ('pio') not found." >&2
    echo "Please install it using: pipx install platformio" >&2
    exit 1
fi

PIO_CMD="pio"
if ! command -v pio &> /dev/null; then
    PIO_CMD="platformio"
fi

cd "$FIRMWARE_DIR"

# Step 1: Build filesystem image (littlefs.bin) needed by merge_bin.py
echo "==> [1/2] Building LittleFS filesystem image (data/ -> littlefs.bin)..."
PLATFORMIO_BUILD_SRC_FLAGS="-DFIRMWARE_VERSION=\\\"${VERSION}\\\"" "$PIO_CMD" run -t buildfs

# Step 2: Build firmware, passing the version string as a compile definition
echo ""
echo "==> [2/2] Compiling firmware (version: $VERSION) and merging binaries..."
PLATFORMIO_BUILD_SRC_FLAGS="-DFIRMWARE_VERSION=\\\"${VERSION}\\\"" "$PIO_CMD" run

# Verify outputs
if [ ! -f "$BUILD_DIR/firmware.bin" ]; then
    echo "Error: Build succeeded but $BUILD_DIR/firmware.bin was not found." >&2
    exit 1
fi

mkdir -p "$OUTPUT_DIR"
cp "$BUILD_DIR/firmware.bin"  "$OUTPUT_DIR/firmware_${VERSION}.bin"
if [ -f "$BUILD_DIR/merged.bin" ]; then
    cp "$BUILD_DIR/merged.bin" "$OUTPUT_DIR/merged_${VERSION}.bin"
fi

echo ""
echo "=========================================="
echo " Build Completed Successfully! "
echo "=========================================="
echo "Binaries copied to: $OUTPUT_DIR"
echo ""
echo " 1. App Patch Binary: $OUTPUT_DIR/firmware_${VERSION}.bin"
echo "    Size: $(du -h "$OUTPUT_DIR/firmware_${VERSION}.bin" | cut -f1)"
echo "    Flash Address: 0x10000 (preserves settings/books)"
echo ""
if [ -f "$OUTPUT_DIR/merged_${VERSION}.bin" ]; then
    echo " 2. Full Merged Binary: $OUTPUT_DIR/merged_${VERSION}.bin"
    echo "    Size: $(du -h "$OUTPUT_DIR/merged_${VERSION}.bin" | cut -f1)"
    echo "    Flash Address: 0x0000 (factory reflash)"
    echo ""
fi
echo "To flash via USB (device held in joystick-boot mode):"
echo "  esptool.py --chip esp32s3 -p /dev/ttyUSB0 write_flash 0x10000 $OUTPUT_DIR/firmware_${VERSION}.bin"
echo "Or use a web flasher at https://www.espboards.dev/tools/program/"
echo "=========================================="
