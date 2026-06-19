#!/bin/bash
# Setup TLA+ model checker for the Qiming scheduler.
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
TLA_JAR="$SCRIPT_DIR/tla2tools.jar"
TLA_VERSION="v1.7.4"
TLA_URL="https://github.com/tlaplus/tlaplus/releases/download/$TLA_VERSION/tla2tools.jar"

if [ ! -f "$TLA_JAR" ]; then
    echo "Downloading TLA+ tools from $TLA_URL ..."
    if command -v gh &>/dev/null; then
        gh release download "$TLA_VERSION" --repo tlaplus/tlaplus -p "tla2tools.jar" -D "$SCRIPT_DIR"
    elif command -v curl &>/dev/null; then
        curl -sL -o "$TLA_JAR" "$TLA_URL"
    elif command -v wget &>/dev/null; then
        wget -q -O "$TLA_JAR" "$TLA_URL"
    else
        echo "ERROR: need curl, wget, or gh"
        echo "Download manually from: $TLA_URL"
        exit 1
    fi
    if ! java -jar "$TLA_JAR" -version &>/dev/null; then
        echo "ERROR: downloaded file invalid (network issue?)"
        rm -f "$TLA_JAR"
        exit 1
    fi
fi

echo "Running TLC model checker..."
cd "$SCRIPT_DIR"
java -cp "$TLA_JAR" tlc2.TLC -modelcheck -config MC.cfg scheduler.tla "$@"
echo "Done."
