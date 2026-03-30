#!/bin/bash
# PlatformIO OpenOCD Wrapper for STM32C0
# This script is now platform-independent and uses local configuration files.

# Determine the directory where this script is located
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LOCAL_OPENOCD_DIR="$SCRIPT_DIR/openocd"

# Try to find the OpenOCD binary:
# 1. Check if it's already in the PATH
# 2. Check the standard PlatformIO location
# 3. Check for the PLATFORMIO_CORE_DIR environment variable
OPENOCD_BIN="openocd"

if ! command -v "$OPENOCD_BIN" &> /dev/null; then
    # Check common PlatformIO locations
    PIO_CORE_DIR="${PLATFORMIO_CORE_DIR:-$HOME/.platformio}"
    if [[ "$OSTYPE" == "msys" || "$OSTYPE" == "cygwin" ]]; then
        # Windows-style path handling if needed, though this is a bash script
        POTENTIAL_BIN="$PIO_CORE_DIR/packages/tool-openocd/bin/openocd.exe"
    else
        POTENTIAL_BIN="$PIO_CORE_DIR/packages/tool-openocd/bin/openocd"
    fi
    
    if [ -f "$POTENTIAL_BIN" ]; then
        OPENOCD_BIN="$POTENTIAL_BIN"
    else
        echo "Error: openocd not found in PATH or at $POTENTIAL_BIN"
        exit 1
    fi
fi

# Execute OpenOCD
# We prepend the local openocd directory to the search path (-s)
# This ensures our local interface/stlink.cfg and target/stm32c0x.cfg are found first.
exec "$OPENOCD_BIN" \
    -s "$LOCAL_OPENOCD_DIR" \
    -f interface/stlink.cfg \
    -f target/stm32c0x.cfg \
    "$@"
