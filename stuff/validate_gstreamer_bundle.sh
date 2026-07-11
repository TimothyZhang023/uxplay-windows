#!/bin/bash
set -euo pipefail

DIST_DIR="${1:?usage: validate_gstreamer_bundle.sh DIST_DIR WORK_DIR}"
WORK_DIR="${2:?usage: validate_gstreamer_bundle.sh DIST_DIR WORK_DIR}"
RUNTIME_PREFIX="${MSYSTEM_PREFIX:-/ucrt64}"
GST_INSPECT_SOURCE="$RUNTIME_PREFIX/bin/gst-inspect-1.0.exe"

if [ ! -s "$GST_INSPECT_SOURCE" ]; then
  echo "ERROR: gst-inspect-1.0.exe not found: $GST_INSPECT_SOURCE" >&2
  exit 1
fi

VALIDATOR_DIR="$WORK_DIR/gstreamer-bundle-validation"
rm -rf "$VALIDATOR_DIR"
mkdir -p "$VALIDATOR_DIR"
cp "$GST_INSPECT_SOURCE" "$VALIDATOR_DIR/"

DIST_WIN="$(cygpath -w "$DIST_DIR")"
PLUGIN_WIN="$(cygpath -w "$DIST_DIR/lib/gstreamer-1.0")"
SCANNER_WIN="$(cygpath -w "$DIST_DIR/libexec/gstreamer-1.0/gst-plugin-scanner.exe")"
VALIDATOR_WIN="$(cygpath -w "$VALIDATOR_DIR/gst-inspect-1.0.exe")"
REGISTRY_WIN="$(cygpath -w "$VALIDATOR_DIR/registry.bin")"

echo "Validating required GStreamer plugins using only packaged DLLs..."
for plugin in app libav playback autodetect videoparsersbad; do
  powershell.exe -NoProfile -NonInteractive -Command "
    \$ErrorActionPreference = 'Stop'
    \$env:PATH = '$DIST_WIN;' + \$env:SystemRoot + '\\System32;' + \$env:SystemRoot
    \$env:GST_PLUGIN_PATH = '$PLUGIN_WIN'
    \$env:GST_PLUGIN_PATH_1_0 = '$PLUGIN_WIN'
    \$env:GST_PLUGIN_SYSTEM_PATH_1_0 = '$PLUGIN_WIN'
    \$env:GST_PLUGIN_SCANNER = '$SCANNER_WIN'
    \$env:GST_PLUGIN_SCANNER_1_0 = '$SCANNER_WIN'
    \$env:GST_REGISTRY_1_0 = '$REGISTRY_WIN'
    & '$VALIDATOR_WIN' '$plugin'
    if (\$LASTEXITCODE -ne 0) { exit \$LASTEXITCODE }
  "
  echo "Validated GStreamer plugin: $plugin"
done

rm -rf "$VALIDATOR_DIR"
