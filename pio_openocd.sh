#!/bin/bash
# PlatformIO OpenOCD Wrapper for STM32C0
# This script is now platform-independent and uses local configuration files.
# It prioritizes the OpenOCD version provided by PlatformIO, which includes STM32C0 support.

# Determine the directory where this script is located
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LOCAL_OPENOCD_DIR="$SCRIPT_DIR/openocd"

# Determine PlatformIO Core directory
PIO_CORE_DIR="${PLATFORMIO_CORE_DIR:-$HOME/.platformio}"

# Find the OpenOCD binary:
# 1. Check the PlatformIO package location first (version with STM32C0 support)
# 2. Check for the PLATFORMIO_CORE_DIR environment variable
# 3. Fallback to system PATH
if [[ "$OSTYPE" == "msys" || "$OSTYPE" == "cygwin" ]]; then
    # Windows-style path handling for bash
    POTENTIAL_BIN="$PIO_CORE_DIR/packages/tool-openocd/bin/openocd.exe"
    SCRIPTS_DIR="$PIO_CORE_DIR/packages/tool-openocd/openocd/scripts"
else
    POTENTIAL_BIN="$PIO_CORE_DIR/packages/tool-openocd/bin/openocd"
    SCRIPTS_DIR="$PIO_CORE_DIR/packages/tool-openocd/openocd/scripts"
fi

if [ -f "$POTENTIAL_BIN" ]; then
    OPENOCD_BIN="$POTENTIAL_BIN"
else
    # Fallback to system openocd
    OPENOCD_BIN=$(command -v openocd)
    if [ -z "$OPENOCD_BIN" ]; then
        echo "Error: openocd not found in ~/.platformio or in PATH"
        exit 1
    fi
fi

# Execute OpenOCD
# -s "$LOCAL_OPENOCD_DIR" : Prepends the local config directory (searched first)
# -s "$SCRIPTS_DIR"       : Includes the standard OpenOCD scripts (for swj-dp.tcl, mem_helper.tcl etc.)
exec "$OPENOCD_BIN" \
    -s "$LOCAL_OPENOCD_DIR" \
    -s "$SCRIPTS_DIR" \
    -f interface/stlink.cfg \
    -f target/stm32c0x.cfg \
    "$@"
