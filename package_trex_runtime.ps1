<#
  package_trex_runtime.ps1

  Assembles the NVIDIA RTX Remix runtime folder layout from this fork's build
  outputs, for injecting the DX11 Remix renderer into 32-bit DX11 games via the
  process-bitness split that NVIDIA's runtime uses:

      runtime/
      |--- d3d11.dll        <-- 32-bit client interposer (lives in the game process)
      |--- dxgi.dll         <-- 32-bit client interposer
      \--- .trex/
           |--- d3d11.dll   <-- 64-bit Remix renderer (this fork)
           |--- dxgi.dll    <-- 64-bit Remix renderer
           |--- nvngx_*.dll <-- 64-bit NGX (DLSS / RR / FG) runtime
           \--- (renderer support files)

  WHY THE SPLIT EXISTS (not a packaging preference - an OS hard limit):
  A 32-bit process can only load 32-bit DLLs. A 32-bit DX11 game therefore can
  never load the 64-bit Remix renderer into its own address space. The 32-bit
  interposer in the game folder is small; the 64-bit renderer runs with the
  large address space Vulkan path tracing needs, inside .trex/.

  This script does NOT build the bridge IPC transport (NvRemixBridge.exe and the
  client<->server marshalling). That is a separate component - NVIDIA's lives in
  the bridge-remix repo and marshals D3D9, not DX11. This script lays out the
  renderer + interposer halves into the exact directory shape so that, paired
  with a DX11 bridge transport, the install matches NVIDIA's runtime/ + .trex/
  convention and the same install steps apply. For 64-bit DX11 games, .trex/'s
  renderer is loaded directly and no bridge is needed.

  USAGE:
    # After building both architectures (build_dxvk_all_ninja.ps1 -Architecture both):
    powershell -ExecutionPolicy Bypass -File .\package_trex_runtime.ps1 -Flavour release

  PARAMETERS:
    -Flavour   debug | debugoptimized | release   (default: release)
    -OutDir    output folder for the assembled runtime (default: .\_trex_runtime)
    -RepoRoot  repo root (default: script folder)
#>
param(
  [ValidateSet('debug', 'debugoptimized', 'release')]
  [string]$Flavour = 'release',
  [string]$OutDir = '',
  [string]$RepoRoot = ''
)
$ErrorActionPreference = 'Stop'
function Log([string]$m) { Write-Host ("[trex] {0}" -f $m) }
function Warn([string]$m) { Write-Host ("[trex] WARNING: {0}" -f $m) -ForegroundColor Yellow }

# --- locate repo root ---
if ([string]::IsNullOrWhiteSpace($RepoRoot)) {
  if (-not [string]::IsNullOrWhiteSpace($PSScriptRoot)) { $RepoRoot = $PSScriptRoot }
  else { $RepoRoot = (Get-Location).Path }
}
if (-not (Test-Path (Join-Path $RepoRoot 'meson.build'))) {
  $sub = Join-Path $RepoRoot 'dxvk-remix-DX11'
  if (Test-Path (Join-Path $sub 'meson.build')) { $RepoRoot = $sub }
  else { Log "ERROR: meson.build not found in '$RepoRoot'. Pass -RepoRoot pointing at the repo root."; exit 1 }
}
Log "Repo root: $RepoRoot"

if ([string]::IsNullOrWhiteSpace($OutDir)) { $OutDir = Join-Path $RepoRoot '_trex_runtime' }

# --- map flavour to the build subdir suffix produced by build_dxvk_all_ninja.ps1 ---
$suffix = switch ($Flavour) {
  'debug'          { 'Debug' }
  'debugoptimized' { 'DebugOptimized' }
  'release'        { 'Release' }
}
$dir32Name = "_Comp32$suffix"
$dir64Name = "_Comp64$suffix"

# Candidate locations for each architecture's built DLLs, in priority order.
# Both the install dir (_output\<arch>) and the raw build dir are checked.
$roots32 = @(
  (Join-Path $RepoRoot ('_output\x86')),
  (Join-Path $RepoRoot $dir32Name)
)
$roots64 = @(
  (Join-Path $RepoRoot ('_output\x64')),
  (Join-Path $RepoRoot $dir64Name)
)

function Find-Dll {
  param([string[]]$Roots, [string]$Name)
  foreach ($r in $Roots) {
    if (-not (Test-Path $r)) { continue }
    $hit = Get-ChildItem -Path $r -Recurse -Filter $Name -File -ErrorAction SilentlyContinue |
           Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if ($hit) { return $hit.FullName }
  }
  return $null
}

# --- locate the four required DLLs ---
$client_d3d11 = Find-Dll -Roots $roots32 -Name 'd3d11.dll'
$client_dxgi  = Find-Dll -Roots $roots32 -Name 'dxgi.dll'
$render_d3d11 = Find-Dll -Roots $roots64 -Name 'd3d11.dll'
$render_dxgi  = Find-Dll -Roots $roots64 -Name 'dxgi.dll'

$fatal = $false
if (-not $render_d3d11) { Warn "64-bit renderer d3d11.dll not found in $($roots64 -join ' or '). Build x64 first: build_dxvk_all_ninja.ps1 -Architecture x64"; $fatal = $true }
if (-not $render_dxgi)  { Warn "64-bit renderer dxgi.dll not found."; $fatal = $true }
if ($fatal) { Log "ERROR: the 64-bit renderer is required for the .trex folder. Aborting."; exit 1 }

if (-not $client_d3d11 -or -not $client_dxgi) {
  Warn "32-bit interposer DLLs not found in $($roots32 -join ' or ')."
  Warn "Build x86 first:  build_dxvk_all_ninja.ps1 -Architecture x86"
  Warn "Continuing: the .trex/ renderer half will still be assembled, but the 32-bit game-folder half will be incomplete (64-bit DX11 games will work; 32-bit games need the interposer)."
}

# --- verify bitness of each DLL so a mistake can't ship silently ---
function Get-DllMachine {
  param([string]$Path)
  try {
    $fs = [System.IO.File]::OpenRead($Path)
    try {
      $br = New-Object System.IO.BinaryReader($fs)
      $fs.Seek(0x3C, 'Begin') | Out-Null
      $peOff = $br.ReadInt32()
      $fs.Seek($peOff + 4, 'Begin') | Out-Null  # skip "PE\0\0"
      return $br.ReadUInt16()                    # IMAGE_FILE_HEADER.Machine
    } finally { $fs.Close() }
  } catch { return 0 }
}
$IMAGE_FILE_MACHINE_I386  = 0x014c
$IMAGE_FILE_MACHINE_AMD64 = 0x8664

function Assert-Bitness {
  param([string]$Path, [int]$Expected, [string]$What)
  if (-not $Path) { return }
  $m = Get-DllMachine -Path $Path
  if ($m -eq 0) { Warn "Could not read PE header of $What ($Path); skipping bitness check."; return }
  if ($m -ne $Expected) {
    $got = if ($m -eq $IMAGE_FILE_MACHINE_AMD64) { '64-bit' } elseif ($m -eq $IMAGE_FILE_MACHINE_I386) { '32-bit' } else { ("0x{0:X}" -f $m) }
    $exp = if ($Expected -eq $IMAGE_FILE_MACHINE_AMD64) { '64-bit' } else { '32-bit' }
    Log "ERROR: $What is $got but must be $exp. Wrong build output picked up - check $Path."
    exit 1
  }
}
Assert-Bitness -Path $client_d3d11 -Expected $IMAGE_FILE_MACHINE_I386  -What 'client d3d11.dll'
Assert-Bitness -Path $client_dxgi  -Expected $IMAGE_FILE_MACHINE_I386  -What 'client dxgi.dll'
Assert-Bitness -Path $render_d3d11 -Expected $IMAGE_FILE_MACHINE_AMD64 -What 'renderer d3d11.dll (.trex)'
Assert-Bitness -Path $render_dxgi  -Expected $IMAGE_FILE_MACHINE_AMD64 -What 'renderer dxgi.dll (.trex)'

# --- assemble the layout ---
if (Test-Path $OutDir) { Remove-Item $OutDir -Recurse -Force }
$trex = Join-Path $OutDir '.trex'
New-Item -ItemType Directory -Path $trex -Force | Out-Null

# 64-bit renderer half: everything that sits beside the renderer d3d11.dll
# (support DLLs, the rtx config, shader blobs, NGX) goes into .trex/.
$renderDir = Split-Path $render_d3d11 -Parent
$copied = 0
foreach ($f in Get-ChildItem -Path $renderDir -File -ErrorAction SilentlyContinue) {
  Copy-Item -Path $f.FullName -Destination (Join-Path $trex $f.Name) -Force
  $copied++
}
Log ".trex/: copied $copied renderer file(s) from $renderDir"

# NGX (64-bit) into .trex/ if not already carried over above.
$ngxSource = Join-Path $RepoRoot 'nv-private\hdremix\bin\release'
if (Test-Path $ngxSource) {
  $n = 0
  foreach ($dll in Get-ChildItem -Path $ngxSource -Filter 'nvngx_*.dll' -File) {
    Copy-Item -Path $dll.FullName -Destination (Join-Path $trex $dll.Name) -Force
    $n++
  }
  if ($n -gt 0) { Log ".trex/: deployed $n NGX DLL(s)" }
} else {
  Warn "NGX runtime not found at $ngxSource - DLSS will be unavailable. (Renderer still works with XeSS/TAA-U fallback.)"
}

# DX11 bridge server (NvRemixDx11Bridge.exe) into .trex/, so a 32-bit DX11 game
# can reach the 64-bit renderer. The DX11 client interposer launches this exact
# name from .trex/ (never the stock NVIDIA NvRemixBridge.exe, which is a D3D9
# server and would fail the handshake). Searched alongside the built bridge.
$bridgeExe = $null
foreach ($r in @(
    (Join-Path $RepoRoot '_dx11_bridge_build\x64'),
    (Join-Path $RepoRoot ('_output\x86\' + $Flavour + '\.trex')),
    (Join-Path $RepoRoot 'bridge_dx11'))) {
  if (-not (Test-Path $r)) { continue }
  $hit = Get-ChildItem -Path $r -Recurse -Filter 'NvRemixDx11Bridge.exe' -File -ErrorAction SilentlyContinue |
         Sort-Object LastWriteTime -Descending | Select-Object -First 1
  if ($hit) { $bridgeExe = $hit.FullName; break }
}
if ($bridgeExe) {
  Assert-Bitness -Path $bridgeExe -Expected $IMAGE_FILE_MACHINE_AMD64 -What 'DX11 bridge server (NvRemixDx11Bridge.exe)'
  Copy-Item -Path $bridgeExe -Destination (Join-Path $trex 'NvRemixDx11Bridge.exe') -Force
  Log ".trex/: staged DX11 bridge server NvRemixDx11Bridge.exe"
  # A default bridge.conf so the interposer has the buffer-size knobs it reads.
  $bridgeConf = Join-Path $trex 'bridge.conf'
  if (-not (Test-Path $bridgeConf)) {
    @(
      '# RTX Remix DX11 bridge configuration.',
      '# Increase these if the log reports an over-size buffer write.',
      'commandRingSizeBytes   = 16777216',
      'sharedHeapSizeBytes    = 268435456',
      'serverWaitTimeoutMs    = 60000'
    ) | Set-Content -Path $bridgeConf -Encoding UTF8
    Log ".trex/: wrote default bridge.conf"
  }
} else {
  Warn "DX11 bridge server NvRemixDx11Bridge.exe not found - build it with bridge_dx11\build_dx11_bridge.ps1. 64-bit DX11 games do not need it; 32-bit DX11 games cannot reach the renderer without it."
}

# 32-bit interposer half: lives in the runtime root, next to the game exe.
if ($client_d3d11) { Copy-Item -Path $client_d3d11 -Destination (Join-Path $OutDir 'd3d11.dll') -Force; Log "root: placed 32-bit interposer d3d11.dll" }
if ($client_dxgi)  { Copy-Item -Path $client_dxgi  -Destination (Join-Path $OutDir 'dxgi.dll')  -Force; Log "root: placed 32-bit interposer dxgi.dll" }

# --- drop an install README into the package ---
$readme = @"
RTX Remix DX11 runtime (dxvk-remix-DX11) - NVIDIA .trex layout
==============================================================

Layout:
  d3d11.dll, dxgi.dll   32-bit interposer - goes in the GAME folder next to the .exe
  .trex\                64-bit Remix renderer + NGX - goes in the GAME folder too

INSTALL (64-bit DX11 game):
  Copy the 64-bit d3d11.dll and dxgi.dll from .trex\ directly next to the game
  .exe. The renderer loads in-process; no interposer or bridge is required.

INSTALL (32-bit DX11 game):
  Copy BOTH this folder's root d3d11.dll/dxgi.dll (32-bit interposer) AND the
  .trex\ folder next to the game .exe, preserving this structure:

      <game>\d3d11.dll        (32-bit, from this root)
      <game>\dxgi.dll         (32-bit, from this root)
      <game>\.trex\d3d11.dll  (64-bit renderer)
      <game>\.trex\dxgi.dll   (64-bit renderer)
      <game>\.trex\nvngx_*.dll

  NOTE: a 32-bit DX11 game additionally requires a DX11 bridge transport
  (NvRemixBridge.exe) in .trex\. NVIDIA's shipped bridge marshals D3D9, not
  DX11, so it will not drive this DX11 renderer - a DX11 bridge is a separate
  component. Until one is present, 32-bit DX11 titles will load the interposer
  but cannot reach the 64-bit renderer. 64-bit DX11 titles are unaffected.

DO NOT mix the two d3d11.dll files: the root one is 32-bit (small, interposer),
the .trex\ one is 64-bit (the renderer). The packager has verified each file's
bitness; if it refused to run, the wrong build output was present.

Verify a build is fresh:
  Select-String -Path .\.trex\d3d11.dll -Pattern "rejectStreak" -SimpleMatch -List
"@
Set-Content -Path (Join-Path $OutDir 'INSTALL_TREX.txt') -Value $readme -Encoding UTF8

Log "Done. Runtime assembled at: $OutDir"
Log "  root  : 32-bit interposer (game-folder half)"
Log "  .trex : 64-bit renderer + NGX (loaded directly by 64-bit DX11 games)"
