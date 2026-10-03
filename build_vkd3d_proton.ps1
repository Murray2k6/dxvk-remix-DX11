[CmdletBinding()]
param(
  # vkd3d-proton commit the DX12 front end is designed against
  # (documentation/engine_knowledge/vkd3d_proton_capture.md).
  [string]$Commit = '31d1f89ca5b3f2fd3b6f025c8e6f73ed3eaa852b',
  [ValidateRange(1, 256)][int]$Jobs = 6,
  [string]$MsysRoot = 'C:\msys64'
)

# DX12 front end (documentation/engine_knowledge/DX12_PLAN.md): build vkd3d-proton's
# d3d12.dll / d3d12core.dll with Remix's patch, using the MSYS2 UCRT64 toolchain, its
# supported Windows route. Source and build live under _vkd3d\ (git-ignored); output
# is staged into _output\x64\ beside the runtime.
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$repoRoot = $PSScriptRoot
$work = Join-Path $repoRoot '_vkd3d'
$src = Join-Path $work 'src'
$bash = Join-Path $MsysRoot 'usr\bin\bash.exe'
if (-not (Test-Path -LiteralPath $bash)) {
  throw "MSYS2 not found at $MsysRoot. Install it, then in UCRT64: pacman -S git mingw-w64-ucrt-x86_64-{gcc,meson,ninja,glslang,tools}"
}

function Invoke-Ucrt64([string]$Script) {
  $env:MSYSTEM = 'UCRT64'
  $env:CHERE_INVOKING = '1'
  # Windows PowerShell 5.1 makes native stderr fatal under Stop; the exit code decides.
  $previous = $ErrorActionPreference
  $ErrorActionPreference = 'Continue'
  try {
    & $bash -lc $Script 2>&1 | ForEach-Object { Write-Host "$_" }
    $code = $LASTEXITCODE
  } finally { $ErrorActionPreference = $previous }
  if ($code -ne 0) { throw "UCRT64 step failed ($code): $Script" }
}

function ConvertTo-MsysPath([string]$Path) {
  $full = [IO.Path]::GetFullPath($Path)
  return '/' + $full.Substring(0, 1).ToLowerInvariant() + $full.Substring(2).Replace('\', '/')
}

New-Item -ItemType Directory -Force -Path $work | Out-Null
$srcMsys = ConvertTo-MsysPath $src
if (-not (Test-Path -LiteralPath (Join-Path $src '.git'))) {
  Invoke-Ucrt64 "git clone https://github.com/HansKristian-Work/vkd3d-proton.git '$srcMsys'"
}
Invoke-Ucrt64 "cd '$srcMsys' && git fetch --quiet origin && git checkout --quiet $Commit && git submodule update --init --recursive --quiet"

# Remix's vkd3d-proton changes (DX12 front end, documentation/engine_knowledge/DX12_PLAN.md):
# root CBV addresses, VA -> CPU pointer, SRV layouts and the per-draw D3D12 binding
# annotation for the Remix Vulkan layer. Applied once; a re-run finds it applied.
$patch = ConvertTo-MsysPath (Join-Path $repoRoot 'patches\vkd3d-proton-remix.patch')
Invoke-Ucrt64 "cd '$srcMsys' && (git apply --ignore-whitespace --reverse --check '$patch' 2>/dev/null && echo '[vkd3d] Remix patch already applied' || git apply --ignore-whitespace '$patch')"

# The annotation ABI is shared with remix_vk_layer.dll: build against the same header.
$remixInclude = Join-Path $src 'include\remix'
New-Item -ItemType Directory -Force -Path $remixInclude | Out-Null
Copy-Item -LiteralPath (Join-Path $repoRoot 'public\include\remix\remix_vk_frontend.h') -Destination $remixInclude -Force

$buildMsys = ConvertTo-MsysPath (Join-Path $work 'build64')
$setup = if (Test-Path -LiteralPath (Join-Path $work 'build64\build.ninja')) { '--reconfigure' } else { '' }
Invoke-Ucrt64 "cd '$srcMsys' && meson setup $setup --buildtype release -Denable_tests=false '$buildMsys' && ninja -C '$buildMsys' -j $Jobs"

# Staged beside the runtime: everything a game needs is the one x64 folder
# copied next to its executable (DX12 games load these instead of Windows'
# D3D12; DX11 games never load them).
$stage = Join-Path $repoRoot '_output\x64'
New-Item -ItemType Directory -Force -Path $stage | Out-Null
# Earlier builds staged into a vkd3d\ subfolder.
$oldStage = Join-Path $stage 'vkd3d'
if (Test-Path -LiteralPath $oldStage) { Remove-Item -LiteralPath $oldStage -Recurse -Force }
$built = @(Get-ChildItem -LiteralPath (Join-Path $work 'build64\libs') -Recurse -File |
  Where-Object { $_.Name -in @('d3d12.dll', 'd3d12core.dll') })
if ($built.Count -eq 0) { throw 'vkd3d-proton built no d3d12.dll' }
$built | Copy-Item -Destination $stage -Force
Write-Host "[vkd3d] staged: $(($built | ForEach-Object Name) -join ', ') -> $stage"
