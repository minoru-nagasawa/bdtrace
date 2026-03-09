#!/bin/bash
# Fetch Mongoose embedded HTTP server
set -e

MONGOOSE_VERSION="6.18"
URL="https://raw.githubusercontent.com/cesanta/mongoose/${MONGOOSE_VERSION}/mongoose.c"
URL_H="https://raw.githubusercontent.com/cesanta/mongoose/${MONGOOSE_VERSION}/mongoose.h"

VENDOR_DIR="$(cd "$(dirname "$0")/../vendor" && pwd)"

if [ -f "$VENDOR_DIR/mongoose.c" ] && [ -f "$VENDOR_DIR/mongoose.h" ]; then
    echo "Mongoose already present in vendor/"
    exit 0
fi

echo "Downloading Mongoose ${MONGOOSE_VERSION}..."

if command -v curl >/dev/null 2>&1; then
    curl -sL "$URL" -o "$VENDOR_DIR/mongoose.c"
    curl -sL "$URL_H" -o "$VENDOR_DIR/mongoose.h"
elif command -v wget >/dev/null 2>&1; then
    wget -q "$URL" -O "$VENDOR_DIR/mongoose.c"
    wget -q "$URL_H" -O "$VENDOR_DIR/mongoose.h"
else
    echo "Error: neither curl nor wget found" >&2
    exit 1
fi

echo "Mongoose placed in vendor/"
