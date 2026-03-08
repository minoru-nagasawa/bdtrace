#!/bin/bash
#
# Build transparency test: verify bdtrace does not alter OpenSSL build output.
# Downloads OpenSSL 1.1.1w, builds it with and without bdtrace, and compares
# every object file, static library, and final binary byte-for-byte.
#
# OpenSSL's Configure and code generation use Perl extensively,
# making this a Perl process tracking stress test.
#
# Usage: scripts/test_openssl_transparency.sh [path-to-bdtrace]
#        Default: ./bdtrace

set -euo pipefail

BDTRACE="${1:-./bdtrace}"
OPENSSL_VERSION="1.1.1w"
OPENSSL_TAR="openssl-${OPENSSL_VERSION}.tar.gz"
OPENSSL_URL="https://www.openssl.org/source/${OPENSSL_TAR}"
OPENSSL_DIR="openssl-${OPENSSL_VERSION}"
WORKDIR="$(mktemp -d /tmp/bdtrace-openssl-transparency.XXXXXX)"

cleanup() {
    if [ "${KEEP_WORKDIR:-0}" = "1" ]; then
        echo "Work directory kept: ${WORKDIR}"
    else
        rm -rf "${WORKDIR}"
    fi
}
trap cleanup EXIT

echo "=== OpenSSL Build Transparency Test ==="
echo "bdtrace: ${BDTRACE}"
echo "workdir: ${WORKDIR}"

# Check Perl version (>= 5.10 required for OpenSSL 1.1.1)
if ! command -v perl >/dev/null 2>&1; then
    echo "SKIP: perl not found"
    exit 0
fi
PERL_VERSION=$(perl -e 'printf "%d.%03d", $], ($] - int($]))*1000' 2>/dev/null || echo "0")
PERL_MAJOR=$(perl -e 'print int($])' 2>/dev/null || echo "0")
PERL_MINOR=$(perl -e 'printf "%d", ($] - int($])) * 1000' 2>/dev/null || echo "0")
if [ "${PERL_MAJOR}" -lt 5 ] || { [ "${PERL_MAJOR}" -eq 5 ] && [ "${PERL_MINOR}" -lt 10 ]; }; then
    echo "SKIP: perl >= 5.10 required (found ${PERL_MAJOR}.${PERL_MINOR})"
    exit 0
fi
echo "Perl version: $(perl -v 2>/dev/null | head -2 | tail -1)"

# Resolve bdtrace to absolute path
BDTRACE="$(cd "$(dirname "${BDTRACE}")" && pwd)/$(basename "${BDTRACE}")"
if [ ! -x "${BDTRACE}" ]; then
    echo "ERROR: ${BDTRACE} not found or not executable"
    exit 1
fi

# Download OpenSSL source
echo ""
echo "--- Downloading OpenSSL ${OPENSSL_VERSION} ---"
cd "${WORKDIR}"
if command -v curl >/dev/null 2>&1; then
    curl -sS -L -o "${OPENSSL_TAR}" "${OPENSSL_URL}"
elif command -v wget >/dev/null 2>&1; then
    wget -q -O "${OPENSSL_TAR}" "${OPENSSL_URL}"
else
    echo "ERROR: neither curl nor wget found"
    exit 1
fi
tar xzf "${OPENSSL_TAR}"

# Sanity check: __DATE__/__TIME__ usage
echo ""
echo "--- Checking for __DATE__/__TIME__ usage ---"
if grep -rn '__DATE__\|__TIME__' "${OPENSSL_DIR}/crypto/" "${OPENSSL_DIR}/ssl/" "${OPENSSL_DIR}/apps/" 2>/dev/null | grep -v 'buildinf.h'; then
    echo "WARNING: __DATE__/__TIME__ found outside buildinf.h; results may be non-deterministic"
else
    echo "OK: no __DATE__/__TIME__ usage outside buildinf.h"
fi

# Use a single directory name for both builds so that absolute paths
# embedded in debug info and Makefiles are identical.
BUILDDIR="${WORKDIR}/build"

# Build 1: baseline (no tracing)
# config + make both run without bdtrace
echo ""
echo "--- Build 1: baseline (no tracing) ---"
cp -a "${OPENSSL_DIR}" "${BUILDDIR}"
cd "${BUILDDIR}"
./config no-async no-shared >/dev/null 2>&1
T1_START=$(date +%s%N)
make -j1 2>&1 | tail -3
T1_END=$(date +%s%N)
cd "${WORKDIR}"
mv "${BUILDDIR}" build-baseline

# Build 2: traced with bdtrace
# config runs without bdtrace; only make is traced
echo ""
echo "--- Build 2: traced (with bdtrace) ---"
cp -a "${OPENSSL_DIR}" "${BUILDDIR}"
cd "${BUILDDIR}"
./config no-async no-shared >/dev/null 2>&1
# Copy buildinf.h from baseline to ensure identical timestamps
# (mkbuildinf.pl does not respect SOURCE_DATE_EPOCH in OpenSSL 1.1.1w)
cp "${WORKDIR}/build-baseline/crypto/buildinf.h" crypto/buildinf.h
touch crypto/buildinf.h
T2_START=$(date +%s%N)
"${BDTRACE}" -o "${WORKDIR}/trace.db" make -j1 2>&1 | tail -3
T2_END=$(date +%s%N)
cd "${WORKDIR}"
mv "${BUILDDIR}" build-traced

# Verify buildinf.h stayed identical
echo ""
echo "--- Checking buildinf.h determinism ---"
if cmp -s build-baseline/crypto/buildinf.h build-traced/crypto/buildinf.h; then
    echo "  PASS buildinf.h identical (copied from baseline)"
else
    echo "  FAIL buildinf.h differs (make regenerated it)"
fi

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
OBJ_COUNT=0
for obj in $(find build-baseline -name '*.o' | sort); do
    rel="${obj#build-baseline/}"
    compare_file "${rel}"
    OBJ_COUNT=$((OBJ_COUNT + 1))
done
echo "  (compared ${OBJ_COUNT} object files)"

# Compare static libraries and binary
compare_file "libcrypto.a"
compare_file "libssl.a"
compare_file "apps/openssl"

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
echo "SUCCESS: bdtrace does not alter OpenSSL build output"
exit 0
