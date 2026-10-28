#!/usr/bin/env bash
#
# SRAM interface test for virtualjaguar-libretro
#
# Generates a test ROM that writes known EEPROM values, then verifies
# the libretro SRAM interface works correctly (pack/unpack/round-trip).
#
# Usage: ./test/sram_test.sh <core_path>
# Example: ./test/sram_test.sh ./virtualjaguar_libretro.so
#
set -euo pipefail

CORE="${1:?Usage: $0 <core_path>}"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
WORK_DIR="$(mktemp -d)"

trap 'rm -rf "${WORK_DIR}"' EXIT

# --- Detect platform ---
LDFLAGS=""
BUILD_CC="${CC:-cc}"
case "$(uname -s)" in
    Linux)
        LDFLAGS="-ldl"
        ;;
esac

# --- Resolve core to absolute path ---
CORE="$(cd "$(dirname "${CORE}")" && pwd)/$(basename "${CORE}")"

# --- Build ROM generator ---
echo "==> Building EEPROM test ROM generator..."
${BUILD_CC} -O2 -Wall -o "${WORK_DIR}/gen_eeprom_test_rom" \
    "${SCRIPT_DIR}/tools/gen_eeprom_test_rom.c"

# --- Generate test ROM ---
echo "==> Generating EEPROM test ROM..."
"${WORK_DIR}/gen_eeprom_test_rom" "${WORK_DIR}/eeprom_test.j64"

# --- Build SRAM test harness ---
echo "==> Building SRAM test harness..."
${BUILD_CC} -O2 -Wall -o "${WORK_DIR}/sram_test" \
    "${SCRIPT_DIR}/tools/sram_test.c" ${LDFLAGS}

# --- Run tests ---
echo "==> Running SRAM tests..."
"${WORK_DIR}/sram_test" "${CORE}" "${WORK_DIR}/eeprom_test.j64"
