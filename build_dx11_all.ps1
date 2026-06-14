param(
  [ValidateSet('both','x64','x86')]
  [string]$Architecture = 'both',
  [switch]$AllConfigs,
  [switch]$ReleaseOnly,
  [switch]$DebugOnly,
  [switch]$DebugOptimizedOnly,
  [switch]$Clean,
  [switch]$ConfigureOnly,
  [switch]$Tracy,
  [string]$BuildTarget = ''
)
Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'
$ProjectDir = if ($PSScriptRoot) { [IO.Path]::GetFullPath($PSScriptRoot) } else { [IO.Path]::GetFullPath((Get-Location).Path) }
Set-Location $ProjectDir
function Write-Info([string]$Message) { Write-Host "[dx11-all] $Message" -ForegroundColor Cyan }
function Ensure-Dir([string]$Path) { if (!(Test-Path -LiteralPath $Path)) { New-Item -ItemType Directory -Force -Path $Path | Out-Null } }
function Get-Configs {
  if ($ReleaseOnly) { return @('release') }
  if ($DebugOnly) { return @('debug') }
  if ($DebugOptimizedOnly) { return @('debugoptimized') }
  return @('debug','debugoptimized','release')
}
function Get-BuildSubDir([string]$Cfg) {
  switch ($Cfg) {
    'debug' { return '_Comp64Debug' }
    'debugoptimized' { return '_Comp64DebugOptimized' }
    'release' { return '_Comp64Release' }
  }
}
function Copy-TreeContentsNoArch([string]$SourceDir, [string]$DestDir) {
  if (!(Test-Path -LiteralPath $SourceDir)) { throw "Source output folder missing: $SourceDir" }
  if (Test-Path -LiteralPath $DestDir) { Remove-Item -LiteralPath $DestDir -Recurse -Force }
  Ensure-Dir $DestDir
  $items = @(Get-ChildItem -LiteralPath $SourceDir -Force | Where-Object { $_.Name -ne 'x64' -and $_.Name -ne 'x86' })
  foreach ($item in $items) {
    $dest = Join-Path $DestDir $item.Name
    if ($item.PSIsContainer) { Copy-Item -LiteralPath $item.FullName -Destination $dest -Recurse -Force }
    else { Copy-Item -LiteralPath $item.FullName -Destination $dest -Force }
  }
}
function Invoke-RootX64([string]$Cfg) {
  Write-Info "building root DX11 x64 runtime: $Cfg"
  $bc = Join-Path $ProjectDir 'build_common.ps1'
  if (!(Test-Path -LiteralPath $bc)) { throw "build_common.ps1 missing: $bc" }
  . $bc
  $cmd = Get-Command PerformBuild -ErrorAction Stop
  $params = @{
    Backend = 'ninja'
    BuildFlavour = $Cfg
    BuildSubDir = (Get-BuildSubDir $Cfg)
    EnableTracy = $(if ($Tracy) { 'true' } else { 'false' })
    ConfigureOnly = [bool]$ConfigureOnly
    ShadersOnly = $false
  }
  if ($cmd.Parameters.ContainsKey('Architecture')) { $params['Architecture'] = 'x64' }
  if ($cmd.Parameters.ContainsKey('BuildTarget') -and ![string]::IsNullOrWhiteSpace($BuildTarget)) { $params['BuildTarget'] = $BuildTarget }
  PerformBuild @params
  if (!$ConfigureOnly) {
    $rootOut = Join-Path $ProjectDir '_output'
    $dest = Join-Path $rootOut ("x64\$Cfg")
    Copy-TreeContentsNoArch $rootOut $dest
    Write-Info "staged _output\x64\$Cfg"
  }
}
function Invoke-BridgeX86([string]$Cfg) {
  Write-Info "building DX11 bridge x86 client/server: $Cfg"
  $bridge = Join-Path $ProjectDir 'bridge_dx11\build_dx11_bridge.ps1'
  if (!(Test-Path -LiteralPath $bridge)) { throw "DX11 bridge builder missing: $bridge" }
  $args = @('-NoProfile','-ExecutionPolicy','Bypass','-File',$bridge,'-Config',$Cfg,'-ProjectDir',$ProjectDir,'-OutRoot',(Join-Path $ProjectDir '_output'))
  if ($Clean) { $args += '-Clean' }
  & powershell @args
  if ($LASTEXITCODE -ne 0) { throw "bridge_dx11 build failed for $Cfg with exit code $LASTEXITCODE" }
}
$configs = Get-Configs
if ($Clean) {
  foreach ($d in @('_Comp32Debug','_Comp32DebugOptimized','_Comp32Release')) {
    $p = Join-Path $ProjectDir $d
    if (Test-Path -LiteralPath $p) { Remove-Item -LiteralPath $p -Recurse -Force }
  }
}
$doX64 = ($Architecture -eq 'both' -or $Architecture -eq 'x64')
$doX86 = ($Architecture -eq 'both' -or $Architecture -eq 'x86')
foreach ($cfg in $configs) {
  if ($doX64) { Invoke-RootX64 $cfg }
  if ($doX86) { Invoke-BridgeX86 $cfg }
}
