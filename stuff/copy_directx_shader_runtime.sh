#!/bin/bash
set -euo pipefail

DIST_DIR="${1:?usage: copy_directx_shader_runtime.sh DIST_DIR}"
DIST_WIN="$(cygpath -w "$DIST_DIR")"

# Qt treats DXC as optional, but uxplay-windows exposes a D3D12 renderer. Use
# the supported compiler and validator shipped with the installed Windows SDK.
powershell.exe -NoProfile -NonInteractive -Command "
  \$ErrorActionPreference = 'Stop'
  \$d3dRedist = Join-Path \${env:ProgramFiles(x86)} 'Windows Kits\10\Redist\D3D\x64'
  foreach (\$name in @('dxcompiler.dll', 'dxil.dll')) {
    \$source = Join-Path \$d3dRedist \$name
    if (-not (Test-Path \$source)) {
      throw \"Required DirectX Shader Compiler runtime not found: \$source\"
    }
    Copy-Item -Force \$source '$DIST_WIN'
  }
"

echo "Copied DirectX Shader Compiler runtime from the Windows SDK"
