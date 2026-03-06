#!/bin/bash
# Fetch SQLite amalgamation
set -e

SQLITE_VERSION="3450100"
SQLITE_YEAR="2024"
URL="https://www.sqlite.org/${SQLITE_YEAR}/sqlite-amalgamation-${SQLITE_VERSION}.zip"

VENDOR_DIR="$(cd "$(dirname "$0")/../vendor" && pwd)"

if [ -f "$VENDOR_DIR/sqlite3.c" ] && [ -f "$VENDOR_DIR/sqlite3.h" ]; then
    echo "SQLite already present in vendor/"
    exit 0
fi

echo "Downloading SQLite amalgamation..."
TMPDIR=$(mktemp -d)
cd "$TMPDIR"

if command -v wget >/dev/null 2>&1; then
    wget -q "$URL" -O sqlite.zip
elif command -v curl >/dev/null 2>&1; then
    curl -sL "$URL" -o sqlite.zip
else
    echo "Error: neither wget nor curl found" >&2
    exit 1
fi

unzip -q sqlite.zip
cp sqlite-amalgamation-${SQLITE_VERSION}/sqlite3.c "$VENDOR_DIR/"
cp sqlite-amalgamation-${SQLITE_VERSION}/sqlite3.h "$VENDOR_DIR/"

rm -rf "$TMPDIR"
echo "SQLite placed in vendor/"
