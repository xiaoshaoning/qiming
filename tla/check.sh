#!/bin/bash
# Setup TLA+ model checker for the Qiming scheduler reference model.
# Requires Java. Uses the vendored tla2tools.jar (tla/vendor/) if present,
# otherwise downloads it from GitHub releases.
set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
VENDOR_JAR="$SCRIPT_DIR/vendor/tla2tools.jar"
TLA_JAR="$SCRIPT_DIR/tla2tools.jar"     # legacy local location (gitignored)
TLA_VERSION="v1.7.4"
TLA_URL="https://github.com/tlaplus/tlaplus/releases/download/$TLA_VERSION/tla2tools.jar"

if [ ! -f "$VENDOR_JAR" ] && [ ! -f "$TLA_JAR" ]; then
    echo "Downloading TLA+ tools from $TLA_URL ..."
    # Prefer curl/wget over gh: gh needs auth and can prompt interactively,
    # which hangs on CI runners.
    if command -v curl >/dev/null 2>&1; then
        if ! curl -fsSL --connect-timeout 30 --max-time 300 -o "$TLA_JAR" "$TLA_URL"; then
            echo "ERROR: failed to download tla2tools.jar (curl exit $?)"
            rm -f "$TLA_JAR"
            exit 1
        fi
    elif command -v wget >/dev/null 2>&1; then
        if ! wget -q -O "$TLA_JAR" "$TLA_URL"; then
            echo "ERROR: failed to download tla2tools.jar (wget exit $?)"
            rm -f "$TLA_JAR"
            exit 1
        fi
    else
        echo "ERROR: need curl or wget (or commit vendor/tla2tools.jar)"
        exit 1
    fi
    if ! java -cp "$TLA_JAR" tlc2.TLC -version >/dev/null 2>&1; then
        echo "ERROR: downloaded file invalid (network issue?)"
        rm -f "$TLA_JAR"
        exit 1
    fi
fi

JAR="$VENDOR_JAR"
[ -f "$JAR" ] || JAR="$TLA_JAR"
echo "Using TLC jar: $JAR ($(wc -c < "$JAR") bytes)"

echo "Running TLC model checker..."
cd "$SCRIPT_DIR"
java -cp "$JAR" tlc2.TLC -modelcheck -config MC.cfg scheduler.tla
status=$?
if [ $status -ne 0 ]; then
    echo "ERROR: TLC model check failed with exit code $status"
fi
exit $status
