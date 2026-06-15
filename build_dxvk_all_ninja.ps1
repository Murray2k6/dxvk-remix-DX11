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
  [string]$BuildTarget,
  # Build both architectures when requested. build.bat defaults to x64 release to avoid looking hung.
  [ValidateSet('both', 'x64', 'x86')]
  [string]$Architecture = 'x64',
  # Build debug + debugoptimized + release. Without this, release is the default.
  [switch]$AllConfigs,
  # Do not run packman. Use only when external\ folders are already populated.
  [switch]$NoDepsFetch,
  # Timeout for scripts-common\update-deps.cmd / packman dependency fetch.
  [int]$DepsTimeoutSeconds = 900,
  # Timeout for Meson setup/reconfigure. Prevents Meson/dependency setup from hanging forever.
  [int]$MesonSetupTimeoutSeconds = 900,
  # 0 disables compile timeout because a full Remix compile can legitimately be long.
  [int]$BuildTimeoutSeconds = 0,
  [int]$InstallTimeoutSeconds = 600
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
try {
$ScriptRoot = if ($PSScriptRoot) {
  [IO.Path]::GetFullPath($PSScriptRoot)
} else {
  [IO.Path]::GetFullPath((Get-Location).Path)
}
if (-not (Test-Path (Join-Path $ScriptRoot 'meson.build'))) {
  throw "meson.build was not found next to this script. Copy build_dxvk_all_ninja.ps1, build_common.ps1, meson.build, and meson_options.txt into the dxvk-remix-DX11 repo root."
}
. (Join-Path $ScriptRoot 'build_common.ps1')

# DX11 x86 in this fork is bridge-based, not a local 32-bit USD runtime build.
# When x86 or both is requested, delegate to the dual-output builder so x64 and
# x86 are staged into output\x64 and output\x86 with their correct layouts.
$DualOutputScript = Join-Path $ScriptRoot 'Build-DX11-DualOutput-V51.ps1'
if (($Architecture -eq 'both' -or $Architecture -eq 'x86') -and (Test-Path -LiteralPath $DualOutputScript -PathType Leaf)) {
  Write-Host '[build] DX11 x86 requested; using bridge-based dual-output build instead of local x86 USD build.' -ForegroundColor Cyan
  $dualArgs = @()
  if ($Clean) { $dualArgs += '-Clean' }
  if ($NoDepsFetch) { $dualArgs += '-NoDepsFetch' }
  & $DualOutputScript @dualArgs
  exit $LASTEXITCODE
}
# Do not do a second full-tree timestamp scan here. PerformBuild repairs timestamps
# once per active build directory with output folders excluded.
if ($AllConfigs -and -not $ReleaseOnly) {
  [string[]]$BuildFlavours = @('debug', 'debugoptimized', 'release')
} else {
  [string[]]$BuildFlavours = @('release')
}

# PowerShell 5 strict mode can collapse a single switch result into a scalar string.
# Force a real array so .Count and nested foreach are safe for x64-only/release-only builds.
[string[]]$BuildArchitectures = @(
  switch ($Architecture) {
    'x64'  { 'x64' }
    'x86'  { 'x86' }
    default { 'x64'; 'x86' }
  }
)
$BuildFlavours = @($BuildFlavours)
$BuildArchitectures = @($BuildArchitectures)

function Get-DxvkBuildSubDir {
  param(
    [Parameter(Mandatory)]
    [ValidateSet('x64', 'x86')]
    [string]$Arch,
    [Parameter(Mandatory)]
    [ValidateSet('debug', 'debugoptimized', 'release')]
    [string]$Flavour
  )
  $prefix = if ($Arch -eq 'x86') { '_Comp32' } else { '_Comp64' }
  switch ($Flavour) {
    'debug'          { return ($prefix + 'Debug') }
    'debugoptimized' { return ($prefix + 'DebugOptimized') }
    'release'        { return ($prefix + 'Release') }
  }
}

if ($Clean) {
  foreach ($dirName in @('_Comp64Debug', '_Comp64DebugOptimized', '_Comp64Release', '_Comp32Debug', '_Comp32DebugOptimized', '_Comp32Release')) {
    $dir = Join-Path $ScriptRoot $dirName
    if (Test-Path $dir) {
      Write-Host "[build] Removing clean build directory: $dir" -ForegroundColor Yellow
      Remove-Item -LiteralPath $dir -Recurse -Force
    }
  }
}
$EnableTracyValue = if ($Tracy) { 'true' } else { 'false' }
$totalBuilds = @($BuildArchitectures).Count * @($BuildFlavours).Count
$currentBuild = 0
Write-Host ("[build] Build plan: architecture(s)={0}; config(s)={1}; jobs={2}" -f ($BuildArchitectures -join ','), ($BuildFlavours -join ','), $totalBuilds) -ForegroundColor Cyan
foreach ($arch in @($BuildArchitectures)) {
  foreach ($flavour in @($BuildFlavours)) {
    $currentBuild++
    Write-Host ("[build] ===== Job {0}/{1}: {2} {3} =====" -f $currentBuild, $totalBuilds, $arch, $flavour) -ForegroundColor Cyan
    $performArgs = @{
      Architecture = $arch
      BuildFlavour = $flavour
      BuildSubDir = Get-DxvkBuildSubDir -Arch $arch -Flavour $flavour
      Backend = 'ninja'
      EnableTracy = $EnableTracyValue
      ConfigureOnly = [bool]$ConfigureOnly
      ShadersOnly = [bool]$ShadersOnly
      BuildTarget = $BuildTarget
      FetchDependencies = -not [bool]$NoDepsFetch
      DepsTimeoutSeconds = $DepsTimeoutSeconds
      MesonSetupTimeoutSeconds = $MesonSetupTimeoutSeconds
      BuildTimeoutSeconds = $BuildTimeoutSeconds
      InstallTimeoutSeconds = $InstallTimeoutSeconds
    }
    PerformBuild @performArgs
  }
}

} catch {
  Write-Host ('[build] ERROR: ' + $_.Exception.Message) -ForegroundColor Red
  if ($_.ScriptStackTrace) {
    Write-Host '[build] PowerShell stack:' -ForegroundColor DarkYellow
    Write-Host $_.ScriptStackTrace -ForegroundColor DarkYellow
  }
  exit 1
}
