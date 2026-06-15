
param(
  [switch]$Build,
  [switch]$Clean,
  [switch]$NoSourcePatch,
  [switch]$NoConfigPatch
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
function Log([string]$m) { Write-Host "[dx11-visual-v58] $m" }
function Die([string]$m) { throw "[dx11-visual-v58] $m" }
$Root = Split-Path -Parent $MyInvocation.MyCommand.Path
if (-not (Test-Path (Join-Path $Root 'src\d3d11\d3d11_rtx.cpp'))) {
  Die "Run this from the dxvk-remix-DX11 repo root. Could not find src\d3d11\d3d11_rtx.cpp"
}
$backup = Join-Path $Root ('_dx11_visual_v58_backup_' + (Get-Date -Format 'yyyyMMdd_HHmmss'))
New-Item -ItemType Directory -Force -Path $backup | Out-Null
function Read-Text([string]$Path) {
  return [System.IO.File]::ReadAllText($Path)
}
function Write-Utf8NoBom([string]$Path, [string]$Text) {
  $enc = New-Object System.Text.UTF8Encoding($false)
  [System.IO.File]::WriteAllText($Path, $Text, $enc)
}
function Backup-File([string]$Path) {
  $rel = [IO.Path]::GetRelativePath($Root, $Path)
  $dst = Join-Path $backup $rel
  New-Item -ItemType Directory -Force -Path (Split-Path -Parent $dst) | Out-Null
  Copy-Item -LiteralPath $Path -Destination $dst -Force
}
function Replace-Once([string]$Path, [string]$Find, [string]$Replace, [string]$Name) {
  $txt = Read-Text $Path
  if ($txt.Contains($Replace)) { Log "Already applied: $Name"; return $false }
  if (-not $txt.Contains($Find)) { Die "Could not find patch target for $Name in $Path" }
  Backup-File $Path
  $txt = $txt.Replace($Find, $Replace)
  Write-Utf8NoBom $Path $txt
  Log "Patched: $Name"
  return $true
}
function Write-Dx11Config([string]$Dir) {
  New-Item -ItemType Directory -Force -Path $Dir | Out-Null
  $rtx = @'
# DX11 visual/runtime fix pack v58
# Stops the option-layer blocker seen in the logs by setting the raytrace modes explicitly.
# The logs showed Auto failed to apply renderPassIntegrateIndirectRaytraceMode expected=2.
rtx.raytraceModePreset = 0
rtx.renderPassGBufferRaytraceMode = 1
rtx.renderPassIntegrateDirectRaytraceMode = 1
rtx.renderPassIntegrateIndirectRaytraceMode = 2
rtx.neuralRadianceCache.enable = True
rtx.enableRaytracing = True
rtx.dx11.forceInjection = True
rtx.taggableUntexturedDraws = True
'@
  $dxvk = @'
# DX11 visual/runtime fix pack v58
# Keep presentation immediate like the current logs, but avoid stale HUD/output ambiguity.
dxgi.deferSurfaceCreation = True
dxgi.tearFree = Auto
dxvk.numCompilerThreads = 0
'@
  Write-Utf8NoBom (Join-Path $Dir 'rtx.conf') $rtx
  Write-Utf8NoBom (Join-Path $Dir 'dxvk.conf') $dxvk
  $remix = Join-Path $Dir 'rtx-remix'
  New-Item -ItemType Directory -Force -Path $remix | Out-Null
  Write-Utf8NoBom (Join-Path $remix 'rtx.conf') $rtx
  Write-Utf8NoBom (Join-Path $remix 'dxvk.conf') $dxvk
}
if (-not $NoSourcePatch) {
  $rtx = Join-Path $Root 'src\d3d11\d3d11_rtx.cpp'
  Log "Patching source: $rtx"
  $find1 = 'const bool acceptViewportFallback = usableViewport && plausibleSceneAspect && !stripViewport && ('
  $rep1  = 'const bool haveReliableTargetExtentForFallback = targetWidth >= 64u && targetHeight >= 64u; const bool acceptViewportFallback = haveReliableTargetExtentForFallback && usableViewport && plausibleSceneAspect && !stripViewport && ('
  Replace-Once $rtx $find1 $rep1 'reject 8x8/zero-target viewport fallback before real output extent exists' | Out-Null

  $find2 = 'const bool writesColor = m_context->m_state.om.renderTargetViews[0].ptr() != nullptr; if (textureID == 0 && writesColor && RtxOptions::taggableUntexturedDraws()) {'
  $rep2  = 'const bool writesColor = m_context->m_state.om.renderTargetViews[0].ptr() != nullptr; const bool dx11RealCameraColorSceneDraw = writesColor && !dcs.transformData.usedViewportFallbackProjection && dcs.geometryData.calculatePrimitiveCount() >= 16; if (textureID == 0 && dx11RealCameraColorSceneDraw && (RtxOptions::taggableUntexturedDraws() || RtxOptions::forceInjection())) {'
  Replace-Once $rtx $find2 $rep2 'give real-camera color scene draws a placeholder material when SRV candidate scan finds zero textures' | Out-Null

  $find3 = 'return hasValidCamera && (hasGameSceneDraws || previousSceneAvailable);'
  $rep3  = 'return hasValidCamera && (hasGameSceneDraws || previousSceneAvailable || RtxOptions::forceInjection());'
  Replace-Once $rtx $find3 $rep3 'allow forced DX11 injection after a valid camera exists even during short empty scene gaps' | Out-Null
}
if (-not $NoConfigPatch) {
  Log "Writing DX11 config fix files into repo root and existing output folders."
  Write-Dx11Config $Root
  foreach ($d in @((Join-Path $Root '_output\x64'), (Join-Path $Root '_output\x86'), (Join-Path $Root '_output\x86\.trex'))) {
    if (Test-Path $d) { Write-Dx11Config $d }
  }
}
Log "v58 visual/source fix pass complete. Backup: $backup"
if ($Build) {
  $build = Join-Path $Root 'Build-DX11-DualOutput-V58.ps1'
  if (-not (Test-Path $build)) { Die "Build-DX11-DualOutput-V58.ps1 not found next to this patcher." }
  & powershell -NoProfile -ExecutionPolicy Bypass -File $build @($Clean ? '-Clean' : $null)
  if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}
