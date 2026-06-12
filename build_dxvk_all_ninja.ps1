<#
  Copyright (c) 2021-2023, NVIDIA CORPORATION. All rights reserved.
  Permission is hereby granted, free of charge, to any person obtaining a
  copy of this software and associated documentation files (the "Software"),
  to deal in the Software without restriction, including without limitation
  the rights to use, copy, modify, merge, publish, distribute, sublicense,
  and/or sell copies of the Software, and to permit persons to whom the
  Software is furnished to do so, subject to the following conditions:
  The above copyright notice and this permission notice shall be included in
  all copies or substantial portions of the Software.
  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
  THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
  FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
  DEALINGS IN THE SOFTWARE.
#>
param(
  # Build with the Tracy profiler compiled in.
  [switch]$Tracy,
  # Only configure Meson; do not compile or install.
  [switch]$ConfigureOnly,
  # Build only release instead of debug, debugoptimized, and release.
  [switch]$ReleaseOnly,
  # Remove Meson build folders before configuring.
  [switch]$Clean,
  # Compile only the rtx_shaders Meson target and skip APIC download.
  [switch]$ShadersOnly,
  # Compile one Meson target instead of the default full build.
  [string]$BuildTarget
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$ScriptRoot = if ($PSScriptRoot) {
  [IO.Path]::GetFullPath($PSScriptRoot)
} else {
  [IO.Path]::GetFullPath((Get-Location).Path)
}
if (-not (Test-Path (Join-Path $ScriptRoot 'meson.build'))) {
  throw "meson.build was not found next to this script. Copy build_dxvk_all_ninja.ps1, build_common.ps1, meson.build, and meson_options.txt into the dxvk-remix-DX11 repo root."
}
. (Join-Path $ScriptRoot 'build_common.ps1')
# Repair timestamps before clean/setup too. Meson and Ninja can stop on files that
# were extracted from ZIPs with clocks ahead of the local Windows clock.
Repair-FutureFileTimestamps -SourceDir $ScriptRoot
if ($ReleaseOnly) {
  $BuildFlavours = @('release')
  $BuildSubDirs  = @('_Comp64Release')
} else {
  $BuildFlavours = @('debug', 'debugoptimized', 'release')
  $BuildSubDirs  = @('_Comp64Debug', '_Comp64DebugOptimized', '_Comp64Release')
}
if ($Clean) {
  foreach ($dirName in @('_Comp64Debug', '_Comp64DebugOptimized', '_Comp64Release')) {
    $dir = Join-Path $ScriptRoot $dirName
    if (Test-Path $dir) {
      Write-Host "[build] Removing clean build directory: $dir" -ForegroundColor Yellow
      Remove-Item -LiteralPath $dir -Recurse -Force
    }
  }
}
$EnableTracyValue = if ($Tracy) { 'true' } else { 'false' }
for ($i = 0; $i -lt $BuildFlavours.Length; $i++) {
  $performArgs = @{
    BuildFlavour = $BuildFlavours[$i]
    BuildSubDir = $BuildSubDirs[$i]
    Backend = 'ninja'
    EnableTracy = $EnableTracyValue
    ConfigureOnly = [bool]$ConfigureOnly
    ShadersOnly = [bool]$ShadersOnly
    BuildTarget = $BuildTarget
  }
  PerformBuild @performArgs
}
