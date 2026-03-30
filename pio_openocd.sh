#!/bin/bash
# PlatformIO OpenOCD Wrapper for STM32C0
# This script ensures that the correct interface and target config files are always used
# regardless of how PlatformIO's VS Code extension or core CLI parses arguments.

OPENOCD_BIN="/home/andre/.platformio/packages/tool-openocd/bin/openocd"
SCRIPTS_DIR="/home/andre/.platformio/packages/tool-openocd/openocd/scripts"

# Check if the binary exists
if [ ! -f "$OPENOCD_BIN" ]; then
    echo "Error: OpenOCD binary not found at $OPENOCD_BIN"
    exit 1
fi

# Execute OpenOCD with the hardcoded STM32C0 configuration
# Any additional arguments passed by PlatformIO (like -c "program ...") will be appended.
exec "$OPENOCD_BIN" \
    -s "$SCRIPTS_DIR" \
    -f interface/stlink.cfg \
    -f target/stm32c0x.cfg \
    "$@"
