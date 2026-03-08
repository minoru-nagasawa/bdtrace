#!/bin/bash
#
# Build transparency test: verify bdtrace does not alter build output.
# Downloads Lua 5.4.7, builds it with and without bdtrace, and compares
# every object file and final binary byte-for-byte.
#
# Usage: scripts/test_build_transparency.sh [path-to-bdtrace]
#        Default: ./bdtrace

set -euo pipefail

BDTRACE="${1:-./bdtrace}"
LUA_VERSION="5.4.7"
LUA_TAR="lua-${LUA_VERSION}.tar.gz"
LUA_URL="https://www.lua.org/ftp/${LUA_TAR}"
LUA_DIR="lua-${LUA_VERSION}"
WORKDIR="$(mktemp -d /tmp/bdtrace-transparency.XXXXXX)"

cleanup() {
    if [ "${KEEP_WORKDIR:-0}" = "1" ]; then
        echo "Work directory kept: ${WORKDIR}"
    else
        rm -rf "${WORKDIR}"
    fi
}
trap cleanup EXIT

echo "=== Build Transparency Test ==="
echo "bdtrace: ${BDTRACE}"
echo "workdir: ${WORKDIR}"

# Resolve bdtrace to absolute path
BDTRACE="$(cd "$(dirname "${BDTRACE}")" && pwd)/$(basename "${BDTRACE}")"
if [ ! -x "${BDTRACE}" ]; then
    echo "ERROR: ${BDTRACE} not found or not executable"
    exit 1
fi

# Download Lua source
echo ""
echo "--- Downloading Lua ${LUA_VERSION} ---"
cd "${WORKDIR}"
if command -v curl >/dev/null 2>&1; then
    curl -sS -L -o "${LUA_TAR}" "${LUA_URL}"
elif command -v wget >/dev/null 2>&1; then
    wget -q -O "${LUA_TAR}" "${LUA_URL}"
else
    echo "ERROR: neither curl nor wget found"
    exit 1
fi
tar xzf "${LUA_TAR}"

# Sanity check: no __DATE__/__TIME__ in source
echo ""
echo "--- Checking for __DATE__/__TIME__ usage ---"
if grep -rn '__DATE__\|__TIME__' "${LUA_DIR}/src/" 2>/dev/null; then
    echo "WARNING: __DATE__/__TIME__ found; results may be non-deterministic"
else
    echo "OK: no __DATE__/__TIME__ usage found"
fi

# Build 1: baseline (no tracing)
echo ""
echo "--- Build 1: baseline (no tracing) ---"
cp -a "${LUA_DIR}" build-baseline
cd build-baseline
T1_START=$(date +%s%N)
make -j1 linux 2>&1 | tail -3
T1_END=$(date +%s%N)
cd "${WORKDIR}"

# Build 2: traced with bdtrace
echo ""
echo "--- Build 2: traced (with bdtrace) ---"
cp -a "${LUA_DIR}" build-traced
cd build-traced
T2_START=$(date +%s%N)
"${BDTRACE}" -o "${WORKDIR}/trace.db" make -j1 linux 2>&1 | tail -3
T2_END=$(date +%s%N)
cd "${WORKDIR}"

# Compare artifacts
echo ""
echo "--- Comparing build artifacts ---"
PASS=0
FAIL=0

compare_file() {
    local rel="$1"
    local f1="build-baseline/${rel}"
    local f2="build-traced/${rel}"
    if [ ! -f "${f1}" ]; then
        echo "  SKIP ${rel} (not in baseline)"
        return
    fi
    if [ ! -f "${f2}" ]; then
        echo "  FAIL ${rel} (not in traced build)"
        FAIL=$((FAIL + 1))
        return
    fi
    if cmp -s "${f1}" "${f2}"; then
        echo "  PASS ${rel}"
        PASS=$((PASS + 1))
    else
        echo "  FAIL ${rel} (differs)"
        FAIL=$((FAIL + 1))
    fi
}

# Compare all .o files
for obj in build-baseline/src/*.o; do
    rel="${obj#build-baseline/}"
    compare_file "${rel}"
done

# Compare library and executables
compare_file "src/liblua.a"
compare_file "src/lua"
compare_file "src/luac"

# Check trace.db validity
echo ""
echo "--- Checking trace.db ---"
TRACE_DB="${WORKDIR}/trace.db"
if [ ! -f "${TRACE_DB}" ]; then
    echo "  FAIL trace.db not found"
    FAIL=$((FAIL + 1))
else
    TRACE_SIZE=$(stat -c%s "${TRACE_DB}" 2>/dev/null || stat -f%z "${TRACE_DB}" 2>/dev/null)
    if [ "${TRACE_SIZE}" -gt 4096 ]; then
        echo "  PASS trace.db exists (${TRACE_SIZE} bytes)"
        PASS=$((PASS + 1))
    else
        echo "  FAIL trace.db too small (${TRACE_SIZE} bytes)"
        FAIL=$((FAIL + 1))
    fi
fi

# Timing summary
T1_MS=$(( (T1_END - T1_START) / 1000000 ))
T2_MS=$(( (T2_END - T2_START) / 1000000 ))
if [ "${T1_MS}" -gt 0 ]; then
    OVERHEAD=$(( (T2_MS - T1_MS) * 100 / T1_MS ))
else
    OVERHEAD="N/A"
fi

# Summary
echo ""
echo "=== Results ==="
echo "PASS: ${PASS}"
echo "FAIL: ${FAIL}"
echo ""
echo "=== Timing ==="
echo "Baseline:  ${T1_MS} ms"
echo "Traced:    ${T2_MS} ms"
echo "Overhead:  ${OVERHEAD}%"

if [ "${FAIL}" -gt 0 ]; then
    echo ""
    echo "FAILED: build artifacts differ under tracing"
    KEEP_WORKDIR=1
    exit 1
fi

echo ""
echo "SUCCESS: bdtrace does not alter build output"
exit 0
