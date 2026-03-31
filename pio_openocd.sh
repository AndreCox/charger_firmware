#!/usr/bin/env bash
set -euo pipefail

# PlatformIO OpenOCD wrapper for STM32C0.
# If OpenOCD in PlatformIO/system is missing STM32C0 support, this script
# downloads a known-good xPack build into the project and uses it automatically.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LOCAL_OPENOCD_DIR="$SCRIPT_DIR/openocd"
TOOLS_DIR="$SCRIPT_DIR/.tools"
BOOTSTRAP_DIR="$TOOLS_DIR/openocd-bootstrap"
BOOTSTRAP_BIN="$BOOTSTRAP_DIR/bin/openocd"
BOOTSTRAP_SCRIPTS_DIR="$BOOTSTRAP_DIR/openocd/scripts"
PIO_CORE_DIR="${PLATFORMIO_CORE_DIR:-$HOME/.platformio}"

XPACK_VERSION="${OPENOCD_XPACK_VERSION:-0.12.0-4}"

log() {
    echo "[pio_openocd] $*" >&2
}

detect_triplet() {
    local os arch
    os="$(uname -s)"
    arch="$(uname -m)"

    case "$os" in
        Darwin)
            case "$arch" in
                arm64|aarch64) echo "darwin-arm64" ;;
                x86_64) echo "darwin-x64" ;;
                *) return 1 ;;
            esac
            ;;
        Linux)
            case "$arch" in
                x86_64) echo "linux-x64" ;;
                aarch64|arm64) echo "linux-arm64" ;;
                *) return 1 ;;
            esac
            ;;
        *)
            return 1
            ;;
    esac
}

has_stm32c0_support() {
    local bin="$1"
    local scripts_dir="$2"

    [ -x "$bin" ] || return 1
    [ -f "$scripts_dir/target/swj-dp.tcl" ] || return 1
    [ -f "$scripts_dir/mem_helper.tcl" ] || return 1

    if strings "$bin" | grep -Eq 'STM32C0|STM32C09|STM32C01|STM32C03|STM32C05|STM32C071'; then
        return 0
    fi

    return 1
}

download_bootstrap_openocd() {
    mkdir -p "$TOOLS_DIR"

    local triplet archive_name url tmp_archive extracted_dir
    triplet="$(detect_triplet)" || {
        log "Unsupported host OS/arch for auto-download ($(uname -s)/$(uname -m))."
        return 1
    }

    archive_name="xpack-openocd-${XPACK_VERSION}-${triplet}.tar.gz"
    url="https://github.com/xpack-dev-tools/openocd-xpack/releases/download/v${XPACK_VERSION}/${archive_name}"
    tmp_archive="$TOOLS_DIR/${archive_name}"

    log "Downloading OpenOCD ${XPACK_VERSION} for ${triplet} ..."
    if command -v curl >/dev/null 2>&1; then
        curl -fL "$url" -o "$tmp_archive"
    elif command -v wget >/dev/null 2>&1; then
        wget -O "$tmp_archive" "$url"
    else
        log "Neither curl nor wget is installed; cannot auto-download OpenOCD."
        return 1
    fi

    rm -rf "$BOOTSTRAP_DIR"
    tar -xzf "$tmp_archive" -C "$TOOLS_DIR"

    extracted_dir="$TOOLS_DIR/xpack-openocd-${XPACK_VERSION}"
    if [ ! -d "$extracted_dir" ]; then
        # Fallback: infer first extracted directory.
        extracted_dir="$(tar -tzf "$tmp_archive" | head -n1 | cut -d/ -f1)"
        extracted_dir="$TOOLS_DIR/$extracted_dir"
    fi

    if [ ! -x "$extracted_dir/bin/openocd" ]; then
        log "Downloaded archive does not contain bin/openocd as expected."
        return 1
    fi

    mv "$extracted_dir" "$BOOTSTRAP_DIR"
    rm -f "$tmp_archive"

    log "Installed bootstrap OpenOCD at $BOOTSTRAP_BIN"
}

pick_openocd() {
    local candidates_bin=() candidates_scripts=()
    local pio_bin pio_scripts sys_bin

    pio_bin="$PIO_CORE_DIR/packages/tool-openocd/bin/openocd"
    pio_scripts="$PIO_CORE_DIR/packages/tool-openocd/openocd/scripts"

    if [ -x "$BOOTSTRAP_BIN" ]; then
        candidates_bin+=("$BOOTSTRAP_BIN")
        candidates_scripts+=("$BOOTSTRAP_SCRIPTS_DIR")
    fi

    if [ -x "$pio_bin" ]; then
        candidates_bin+=("$pio_bin")
        candidates_scripts+=("$pio_scripts")
    fi

    if sys_bin="$(command -v openocd 2>/dev/null || true)"; then
        if [ -n "$sys_bin" ]; then
            candidates_bin+=("$sys_bin")
            candidates_scripts+=("$(dirname "$sys_bin")/../share/openocd/scripts")
        fi
    fi

    local i
    for i in "${!candidates_bin[@]}"; do
        if has_stm32c0_support "${candidates_bin[$i]}" "${candidates_scripts[$i]}"; then
            OPENOCD_BIN="${candidates_bin[$i]}"
            SCRIPTS_DIR="${candidates_scripts[$i]}"
            return 0
        fi
    done

    log "No compatible OpenOCD found locally; trying auto-download."
    download_bootstrap_openocd || return 1

    if has_stm32c0_support "$BOOTSTRAP_BIN" "$BOOTSTRAP_SCRIPTS_DIR"; then
        OPENOCD_BIN="$BOOTSTRAP_BIN"
        SCRIPTS_DIR="$BOOTSTRAP_SCRIPTS_DIR"
        return 0
    fi

    return 1
}

OPENOCD_BIN=""
SCRIPTS_DIR=""

if ! pick_openocd; then
    log "Could not locate or install a STM32C0-capable OpenOCD build."
    log "Set OPENOCD_XPACK_VERSION to another release if needed."
    exit 1
fi

log "Using OpenOCD: $OPENOCD_BIN"

exec "$OPENOCD_BIN" \
    -s "$LOCAL_OPENOCD_DIR" \
    -s "$SCRIPTS_DIR" \
    -f interface/stlink.cfg \
    -f target/stm32c0x.cfg \
    "$@"
