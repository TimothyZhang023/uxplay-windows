#!/bin/bash
set -euo pipefail

DIST_DIR="${1:?usage: complete_runtime_bundle.sh DIST_DIR}"
RUNTIME_PREFIX="${MSYSTEM_PREFIX:-/ucrt64}"

if ! command -v objdump >/dev/null 2>&1; then
  echo "ERROR: objdump is required to inspect packaged PE dependencies" >&2
  exit 1
fi

declare -A bundled_files=()
declare -A runtime_files=()

while IFS= read -r -d '' candidate; do
  filename="${candidate##*/}"
  bundled_files["${filename,,}"]=1
done < <(find "$DIST_DIR" -type f -print0)

while IFS= read -r -d '' candidate; do
  filename="${candidate##*/}"
  runtime_files["${filename,,}"]="$candidate"
done < <(find "$RUNTIME_PREFIX/bin" -maxdepth 1 -type f -iname '*.dll' -print0)

echo "Completing recursive MSYS2 runtime dependency closure..."
for iteration in $(seq 1 20); do
  added=0
  while IFS= read -r -d '' binary; do
    while IFS= read -r imported; do
      imported="${imported%$'\r'}"
      [ -z "$imported" ] && continue
      key="${imported,,}"
      [ -n "${bundled_files[$key]:-}" ] && continue

      source="${runtime_files[$key]:-}"
      [ -z "$source" ] && continue

      cp "$source" "$DIST_DIR/$(basename "$source")"
      bundled_files[$key]=1
      echo "COPIED dependency: $(basename "$source") (required by $(basename "$binary"))"
      added=$((added + 1))
    done < <({ objdump -p "$binary" 2>/dev/null || true; } |
             sed -n 's/^[[:space:]]*DLL Name: //p')
  done < <(find "$DIST_DIR" -type f \
            \( -iname '*.exe' -o -iname '*.dll' \) -print0)

  if [ "$added" -eq 0 ]; then
    echo "Runtime dependency closure complete after $iteration pass(es)."
    exit 0
  fi
done

echo "ERROR: runtime dependency closure did not converge" >&2
exit 1
