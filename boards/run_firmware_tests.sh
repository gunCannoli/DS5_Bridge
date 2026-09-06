#!/usr/bin/env bash
# Build and run the host-side firmware logic tests (tests/firmware/*).
#
# These are plain host executables (no Pico SDK, no board) -- the firmware
# equivalent of the companion app's vitest suite. Kept in build/waveshare-tests/
# so the only build tree pattern in this fork is build/wave* (see AGENTS.md).
#
# Usage:
#   ./boards/run_firmware_tests.sh
#
# Note: the MinGW/UCRT toolchain's runtime DLLs must be on PATH for the test
# exes to load. ctest spawns them in a context where that sometimes fails
# (exit 0xc0000139 STATUS_ENTRYPOINT_NOT_FOUND) even though the build is fine;
# this script runs the exes directly with the compiler's bin dir prepended to
# PATH, which resolves it.

set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="${PROJECT_ROOT}/build/waveshare-tests"

cd "${PROJECT_ROOT}"
cmake -S tests/firmware -B "${BUILD_DIR}" -G Ninja
cmake --build "${BUILD_DIR}"

# Prepend the C++ compiler's own bin dir so the runtime DLLs resolve when the
# test exes are launched directly. Read it from the CMake cache and convert the
# Windows path to a POSIX one so it slots into PATH cleanly (a bare "C:/..."
# entry would be split on the colon).
CXX_PATH="$(sed -n 's#^CMAKE_CXX_COMPILER:[^=]*=##p' "${BUILD_DIR}/CMakeCache.txt")"
if [[ -n "${CXX_PATH}" ]]; then
    CXX_BIN="$(cygpath -u "$(dirname "${CXX_PATH}")" 2>/dev/null || dirname "${CXX_PATH}")"
    if [[ -d "${CXX_BIN}" ]]; then
        export PATH="${CXX_BIN}:${PATH}"
    fi
fi

fail=0
for exe in firmware_logic_tests usb_descriptor_migration_test diagnostics_config_test; do
    echo "=== ${exe} ==="
    if ! "${BUILD_DIR}/${exe}.exe"; then
        echo "!!! ${exe} FAILED"
        fail=1
    fi
    echo
done

if [[ "${fail}" -ne 0 ]]; then
    echo "One or more firmware test suites failed." >&2
    exit 1
fi
echo "All firmware host-side test suites passed."
