[CmdletBinding()]
param(
  [switch]$Clean,
  [switch]$SkipRuntimeBuild,
  [switch]$SkipBridgeBuild,
  [switch]$SourceOnly,
  [switch]$UseExistingRuntime,
  [switch]$RebuildRuntime,
  [switch]$NoDepsFetch,
  [string]$OutputRoot = '_output',
  [string]$BridgeRepoUrl = 'https://github.com/NVIDIAGameWorks/dxvk-remix.git',
  [string]$BridgeBranch = 'main'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$Root = Split-Path -Parent $MyInvocation.MyCommand.Path
$LogDir = Join-Path $Root '_build_logs'
New-Item -ItemType Directory -Force -Path $LogDir | Out-Null

function Log([string]$m) { Write-Host "[dx11-output-v50] $m" }
function Warn([string]$m) { Write-Host "[dx11-output-v50] WARNING: $m" -ForegroundColor Yellow }
function Die([string]$m) { throw "[dx11-output-v50] $m" }
function Get-CommandFilePath([string]$Name) {
  $cmds = @(Get-Command $Name -ErrorAction SilentlyContinue)
  foreach ($cmd in $cmds) {
    foreach ($prop in @('Path','Source','Definition')) {
      $v = $null
      try {
        if ($cmd.PSObject.Properties.Name -contains $prop) { $v = [string]$cmd.$prop }
      } catch { $v = $null }
      if (![string]::IsNullOrWhiteSpace($v) -and (Test-Path -LiteralPath $v -PathType Leaf)) { return $v }
    }
  }
  return $null
}
function CmdPath([string]$n) { return (Get-CommandFilePath $n) }


function Write-TextNoBom {
  param([Parameter(Mandatory)][string]$Path, [Parameter(Mandatory)][string]$Text)
  $enc = New-Object System.Text.UTF8Encoding($false)
  [System.IO.File]::WriteAllText($Path, $Text, $enc)
}

function Remove-Utf8BomFromFile {
  param([Parameter(Mandatory)][string]$Path)
  if (!(Test-Path -LiteralPath $Path -PathType Leaf)) { return $false }
  $bytes = [System.IO.File]::ReadAllBytes($Path)
  if ($bytes.Length -ge 3 -and $bytes[0] -eq 0xEF -and $bytes[1] -eq 0xBB -and $bytes[2] -eq 0xBF) {
    $newLen = $bytes.Length - 3
    $outBytes = New-Object byte[] $newLen
    if ($newLen -gt 0) { [System.Array]::Copy($bytes, 3, $outBytes, 0, $newLen) }
    [System.IO.File]::WriteAllBytes($Path, $outBytes)
    return $true
  }
  return $false
}

function Repair-MesonUtf8NoBom {
  param([Parameter(Mandatory)][string]$BaseDir)
  if (!(Test-Path -LiteralPath $BaseDir -PathType Container)) { return }
  $fixed = 0
  $files = @(Get-ChildItem -LiteralPath $BaseDir -Recurse -File -ErrorAction SilentlyContinue |
    Where-Object {
      $_.Name -in @('meson.build','meson_options.txt') -and
      $_.FullName -notmatch '\\.git\\' -and
      $_.FullName -notmatch '\\meson-private\\' -and
      $_.FullName -notmatch '\\meson-logs\\' -and
      $_.FullName -notmatch '\\_Comp(32|64)' -and
      $_.FullName -notmatch '\\_build_logs\\'
    })
  foreach ($f in $files) {
    if (Remove-Utf8BomFromFile $f.FullName) {
      $fixed++
      Log "Removed UTF-8 BOM from Meson file: $($f.FullName)"
    }
  }
  if ($fixed -gt 0) { Log "Repaired $fixed Meson UTF-8 BOM file(s)." }
}

function Quote-Arg([string]$Arg) {
  if ($null -eq $Arg) { return '""' }
  if ($Arg.Length -eq 0) { return '""' }
  if ($Arg -notmatch '[\s"]') { return $Arg }
  $escaped = $Arg -replace '([\\]*)"', '$1$1\"'
  $escaped = $escaped -replace '([\\]+)$', '$1$1'
  return '"' + $escaped + '"'
}

function Get-CommandLine([string]$Exe, [string[]]$CommandArgs) {
  $parts = New-Object System.Collections.Generic.List[string]
  $parts.Add((Quote-Arg $Exe))
  foreach ($a in @($CommandArgs)) { $parts.Add((Quote-Arg ([string]$a))) }
  return ($parts -join ' ')
}

function Invoke-Logged {
  param(
    [Parameter(Mandatory)][string]$Label,
    [Parameter(Mandatory)][string]$Exe,
    [Parameter(Mandatory)][string[]]$CommandArgs,
    [Parameter(Mandatory)][string]$WorkingDirectory,
    [string]$AllowIfFileExists
  )
  if (!(Test-Path -LiteralPath $Exe -PathType Leaf)) { Die "Tool not found: $Exe" }
  if (!(Test-Path -LiteralPath $WorkingDirectory -PathType Container)) { Die "Working directory not found: $WorkingDirectory" }

  $stamp = Get-Date -Format 'yyyyMMdd_HHmmss_fff'
  $logFile = Join-Path $LogDir "$stamp-$Label.log"
  $cmdLine = Get-CommandLine $Exe $CommandArgs
  Log "cmd> $cmdLine"

  Set-Content -LiteralPath $logFile -Encoding UTF8 -Value @(
    "Command: $cmdLine",
    "WorkingDirectory: $WorkingDirectory",
    ""
  )

  $oldEap = $ErrorActionPreference
  $oldGitPrompt = $env:GIT_TERMINAL_PROMPT
  $oldPyUtf8 = $env:PYTHONUTF8
  $env:GIT_TERMINAL_PROMPT = '0'
  $env:PYTHONUTF8 = '1'
  $ErrorActionPreference = 'Continue'
  $code = 9009

  Push-Location -LiteralPath $WorkingDirectory
  try {
    # IMPORTANT: this is synchronous. PowerShell waits for the native process to exit.
    # Stderr is merged only for logging; it is not passed as an argument to the tool.
    & $Exe @CommandArgs 2>&1 | ForEach-Object {
      $line = [string]$_
      Write-Host $line
      Add-Content -LiteralPath $logFile -Encoding UTF8 -Value $line
    }
    $code = if ($null -eq $global:LASTEXITCODE) { 0 } else { [int]$global:LASTEXITCODE }
  } catch {
    $code = 1
    $msg = $_.Exception.Message
    Write-Host $msg
    Add-Content -LiteralPath $logFile -Encoding UTF8 -Value $msg
  } finally {
    Pop-Location
    $ErrorActionPreference = $oldEap
    if ($null -eq $oldGitPrompt) { Remove-Item Env:\GIT_TERMINAL_PROMPT -ErrorAction SilentlyContinue } else { $env:GIT_TERMINAL_PROMPT = $oldGitPrompt }
    if ($null -eq $oldPyUtf8) { Remove-Item Env:\PYTHONUTF8 -ErrorAction SilentlyContinue } else { $env:PYTHONUTF8 = $oldPyUtf8 }
    Add-Content -LiteralPath $logFile -Encoding UTF8 -Value @('', "ExitCode: $code")
  }

  if ($code -ne 0) {
    if ($AllowIfFileExists -and (Test-Path -LiteralPath $AllowIfFileExists -PathType Leaf)) {
      Warn "$Label returned exit code $code, but required file exists, continuing: $AllowIfFileExists"
      return $code
    }
    Warn "$Label failed with exit code $code. Last log lines:"
    if (Test-Path -LiteralPath $logFile) { Get-Content -LiteralPath $logFile -Tail 200 | ForEach-Object { Write-Host $_ } }
    Die "$Label failed with exit code $code. Log: $logFile"
  }
  return 0
}

function Import-VSEnvironment([string]$VsPath, [string]$Target) {
  $vcvars = Join-Path $VsPath 'VC\Auxiliary\Build\vcvarsall.bat'
  if (!(Test-Path -LiteralPath $vcvars -PathType Leaf)) { Die "vcvarsall.bat not found: $vcvars" }
  foreach ($name in @('CL','_CL_','LINK','_LINK_','CC','CXX','LD','AR','RC','CFLAGS','CXXFLAGS','CPPFLAGS','LDFLAGS')) {
    Remove-Item -Path "ENV:\$name" -Force -ErrorAction SilentlyContinue
  }
  $cmd = 'call "' + $vcvars + '" ' + $Target + ' >nul && set'
  Log "Loading Visual Studio environment: $Target"
  $lines = & $env:ComSpec /d /s /c $cmd 2>&1
  $code = if ($null -eq $global:LASTEXITCODE) { 0 } else { [int]$global:LASTEXITCODE }
  if ($code -ne 0) { Die "vcvarsall.bat failed for $Target with exit code $code. Output:`n$($lines | Out-String)" }
  foreach ($line in $lines) {
    if ($line -match '=') {
      $pair = @([string]$line -split '=', 2)
      if (@($pair).Count -eq 2 -and -not [string]::IsNullOrWhiteSpace($pair[0])) { Set-Item -Force -Path "ENV:\$($pair[0])" -Value $pair[1] }
    }
  }
  foreach ($name in @('CL','_CL_','LINK','_LINK_','CC','CXX','LD','AR','RC','CFLAGS','CXXFLAGS','CPPFLAGS','LDFLAGS')) {
    Remove-Item -Path "ENV:\$name" -Force -ErrorAction SilentlyContinue
  }
  $env:MESON_FORCE_BACKTRACE = '1'
  $env:PYTHONUTF8 = '1'
}

function Find-VSInstall {
  $pf86 = [Environment]::GetEnvironmentVariable('ProgramFiles(x86)')
  $candidates = @()
  $cmdPath = Get-CommandFilePath 'vswhere.exe'
  if ($cmdPath) { $candidates += $cmdPath }
  if ($pf86) { $candidates += (Join-Path $pf86 'Microsoft Visual Studio\Installer\vswhere.exe') }
  $vswhere = $candidates | Where-Object { $_ -and (Test-Path -LiteralPath $_ -PathType Leaf) } | Select-Object -First 1
  if (!$vswhere) { Die 'vswhere.exe not found.' }
  $path = (& $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath | Select-Object -First 1)
  if ([string]::IsNullOrWhiteSpace([string]$path)) { Die 'Visual Studio VC x86/x64 tools not found.' }
  return ([string]$path).Trim()
}
function Find-Meson {
  foreach ($p in @(
    (Join-Path $env:APPDATA 'Python\Python314\Scripts\meson.exe'),
    (Join-Path $env:LOCALAPPDATA 'Programs\Python\Python314\Scripts\meson.exe'),
    (Get-CommandFilePath 'meson.exe')
  )) { if ($p -and (Test-Path -LiteralPath $p -PathType Leaf)) { return $p } }
  Die 'meson.exe not found. Install with: py -m pip install --user meson ninja'
}
function Find-Ninja([string]$VsInstall) {
  $n = Join-Path $VsInstall 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe'
  if (Test-Path -LiteralPath $n -PathType Leaf) { return $n }
  $cmdPath = Get-CommandFilePath 'ninja.exe'
  if ($cmdPath) { return $cmdPath }
  Die 'ninja.exe not found.'
}

function Repair-FutureTimestamps {
  $now = Get-Date
  foreach ($rel in @('meson.build','meson_options.txt','src\usd-plugins\meson.build','bridge\meson.build')) {
    $p = Join-Path $Root $rel
    if (Test-Path -LiteralPath $p) {
      $i = Get-Item -LiteralPath $p
      if ($i.LastWriteTime -gt $now.AddSeconds(1)) { $i.LastWriteTime = $now }
    }
  }
}

function Ensure-Deps([string]$Meson) {
  $need = @((Join-Path $Root 'external\nv_usd_Release\lib\usd.lib'), (Join-Path $Root 'external\nv_usd_release\lib\usd.lib'))
  if (@($need | Where-Object { Test-Path -LiteralPath $_ -PathType Leaf }).Count -gt 0) { Log 'Top-level USD deps present.' } else { Warn 'Top-level USD deps not found; runtime Meson may fetch or fail depending on repo state.' }
  $plug = Join-Path $Root 'src\usd-plugins\_external\nv_usd\release\lib\usd.lib'
  if (Test-Path -LiteralPath $plug -PathType Leaf) { Log 'USD plugin deps present.' } else { Warn 'USD plugin deps not found; runtime Meson may fetch or fail depending on repo state.' }
}

function Test-PeMachine([string]$Path, [string]$Expected) {
  if (!(Test-Path $Path)) { Die "Missing file for PE check: $Path" }
  $fs = [System.IO.File]::OpenRead($Path)
  try {
    $br = New-Object System.IO.BinaryReader($fs)
    $fs.Seek(0x3c, [System.IO.SeekOrigin]::Begin) | Out-Null
    $peOff = $br.ReadInt32()
    $fs.Seek($peOff + 4, [System.IO.SeekOrigin]::Begin) | Out-Null
    $machine = $br.ReadUInt16()
  } finally { $fs.Close() }
  $actual = switch ($machine) { 0x014c { 'x86' } 0x8664 { 'x64' } default { ('0x{0:X4}' -f $machine) } }
  if ($actual -ne $Expected) { Die "$Path is $actual, expected $Expected" }
  Log "Verified ${Expected}: $Path"
}

function Find-FileRecursive([string]$Dir, [string]$Name) {
  if (!(Test-Path $Dir)) { return $null }
  $hits = @(Get-ChildItem -LiteralPath $Dir -Recurse -Filter $Name -File -ErrorAction SilentlyContinue |
    Where-Object { $_.FullName -notmatch '\\meson-private\\' -and $_.FullName -notmatch '\\meson-logs\\' } |
    Sort-Object LastWriteTime -Descending)
  if (@($hits).Count -gt 0) { return @($hits)[0].FullName }
  return $null
}

function Remove-DirectoryRobust([string]$Path) {
  if (!(Test-Path -LiteralPath $Path)) { return }
  for ($i = 1; $i -le 5; $i++) {
    try {
      Remove-Item -LiteralPath $Path -Recurse -Force -ErrorAction Stop
      Start-Sleep -Milliseconds 200
      if (!(Test-Path -LiteralPath $Path)) { return }
    } catch {
      Warn "Remove attempt $i failed for ${Path}: $($_.Exception.Message)"
      Start-Sleep -Milliseconds (300 * $i)
    }
  }
  if (Test-Path -LiteralPath $Path) {
    $dead = "$Path.bad_$(Get-Date -Format 'yyyyMMdd_HHmmss')"
    Warn "Could not delete $Path cleanly; renaming to $dead"
    Rename-Item -LiteralPath $Path -NewName (Split-Path -Leaf $dead) -Force
  }
}


function Patch-BridgeServerRegisterD3D11 {
  param([Parameter(Mandatory)][string]$BridgeWork)

  $mainCpp = Join-Path $BridgeWork 'src\server\main.cpp'
  if (!(Test-Path -LiteralPath $mainCpp -PathType Leaf)) { Die "Bridge server main.cpp not found: $mainCpp" }

  $headers = @(
    @(
      (Join-Path $BridgeWork 'public\include\remix\remix_c.h'),
      (Join-Path $Root 'public\include\remix\remix_c.h')
    ) | Where-Object { Test-Path -LiteralPath $_ -PathType Leaf }
  )

  if (@($headers).Count -eq 0) { Die 'remix_c.h was not found in bridge work tree or repo public include.' }

  $headerText = (@($headers) | ForEach-Object { Get-Content -LiteralPath $_ -Raw }) -join "`n"
  $hasD3D11 = ($headerText -match 'dxvk_RegisterD3D11Device')
  $hasD3D9  = ($headerText -match 'dxvk_RegisterD3D9Device')

  $text = Get-Content -LiteralPath $mainCpp -Raw

  # Clean out helper copies left by older v43-v47 patch attempts before injecting the current one.
  # The previous helper versions used different include guards, so a normal marker check could
  # inject a second function and produce C2084 duplicate-body errors in main.cpp.
  $beforeHelperCleanup = $text
  $text = [regex]::Replace(
    $text,
    '(?s)[\r\n]*#ifndef\s+DX11_BRIDGE_REGISTER_HELPER_V\d+\s*[\r\n]+#define\s+DX11_BRIDGE_REGISTER_HELPER_V\d+.*?[\r\n]+#endif\s*[\r\n]*',
    "`r`n"
  )
  if ($text -ne $beforeHelperCleanup) {
    Log 'Removed stale duplicate DX11 bridge registration helper block(s) from bridge server main.cpp.'
  }

  $original = $text

  # The DX11 bridge path must not compile against the stale D3D9-only Remix API member.
  # If this repo's remix_c.h exposes dxvk_RegisterD3D11Device, redirect registration there.
  # If it does not, the script fails instead of producing a pretend bridge registration.
  if (!$hasD3D11) {
    if ($text -match 'dxvk_RegisterD3D9Device') {
      Die @"
This bridge server still contains dxvk_RegisterD3D9Device calls, but your remix_c.h does not expose dxvk_RegisterD3D11Device.
To register a DX11 device, your public\include\remix\remix_c.h must define a real dxvk_RegisterD3D11Device function pointer and the x64 runtime must implement it.
Detected D3D9 register in header: $hasD3D9
"@
    }
    Log 'Bridge server already has no D3D9 registration call and remix_c.h has no D3D11 registration; no registration patch applied.'
    return
  }

  if ($text -notmatch 'DX11_BRIDGE_REGISTER_HELPER_V50') {
    if ($text -notmatch '#include\s+<d3d11\.h>') {
      $text = $text -replace '#include\s+<d3d9\.h>', "#include <d3d9.h>`r`n#include <d3d11.h>"
    }
    $helper = @'

#ifndef DX11_BRIDGE_REGISTER_HELPER_V50
#define DX11_BRIDGE_REGISTER_HELPER_V50
static inline void BridgeRegisterRemixD3D11DeviceForDx11Bridge(IUnknown* pDeviceUnknown) {
  if (!GlobalOptions::getExposeRemixApi()) {
    return;
  }
  if (remixapi::g_remix.dxvk_RegisterD3D11Device == nullptr) {
    Logger::err("DX11 bridge: remixapi dxvk_RegisterD3D11Device is null; device registration skipped.");
    return;
  }
  auto* pD3D11Device = reinterpret_cast<ID3D11Device*>(pDeviceUnknown);
  const auto remixRegisterResult = remixapi::g_remix.dxvk_RegisterD3D11Device(pD3D11Device);
  if (remixRegisterResult != REMIXAPI_ERROR_CODE_SUCCESS &&
      remixRegisterResult != REMIXAPI_ERROR_CODE_ALREADY_EXISTS) {
    Logger::err(format_string("DX11 bridge: dxvk_RegisterD3D11Device returned 0x%x", remixRegisterResult));
  } else {
    Logger::info("DX11 bridge: registered device with Remix through dxvk_RegisterD3D11Device.");
  }
}
#endif
'@
    $insertAfter = 'using namespace remixapi::util;'
    if ($text -match [regex]::Escape($insertAfter)) {
      $text = $text -replace [regex]::Escape($insertAfter), ($insertAfter + $helper)
    } else {
      $text = $helper + "`r`n" + $text
    }
  }

  # Replace the exact old D3D9 Remix registration blocks in both CreateDeviceEx and CreateDevice.
  $pattern = 'if\s*\(\s*GlobalOptions::getExposeRemixApi\(\)\s*\)\s*\{\s*remixapi::g_device\s*=\s*[^;]+;\s*remixapi::g_remix\.dxvk_RegisterD3D9Device\s*\([^;]+\);\s*\}'
  $replacement = 'BridgeRegisterRemixD3D11DeviceForDx11Bridge(reinterpret_cast<IUnknown*>(pD3DDevice));'
  $text = [regex]::Replace($text, $pattern, $replacement)

  # Also catch any direct one-line remnants.
  $text = $text -replace 'remixapi::g_remix\.dxvk_RegisterD3D9Device\s*\([^;]+\);', 'BridgeRegisterRemixD3D11DeviceForDx11Bridge(reinterpret_cast<IUnknown*>(pD3DDevice));'

  if ($text -match 'dxvk_RegisterD3D9Device') {
    Die 'Patch failed: bridge server main.cpp still contains dxvk_RegisterD3D9Device after DX11 registration patch.'
  }
  if ($text -notmatch 'dxvk_RegisterD3D11Device') {
    Die 'Patch failed: bridge server main.cpp does not contain dxvk_RegisterD3D11Device after patch.'
  }
  $helperCount = ([regex]::Matches($text, 'BridgeRegisterRemixD3D11DeviceForDx11Bridge\s*\(')).Count
  if ($helperCount -lt 1) {
    Die 'Patch failed: bridge server main.cpp has no DX11 bridge registration helper after patch.'
  }

  if ($text -ne $original) {
    Write-TextNoBom -Path $mainCpp -Text $text
    Log 'Patched bridge server main.cpp: dxvk_RegisterD3D9Device -> dxvk_RegisterD3D11Device.'
  } else {
    Log 'Bridge server main.cpp already patched for DX11 registration.'
  }
}


function Ensure-DX11ClientSources {
  param(
    [Parameter(Mandatory)][string]$DstClient,
    [Parameter(Mandatory)][string]$PackDir
  )
  New-Item -ItemType Directory -Force -Path $DstClient | Out-Null

  $packClient = Join-Path $PackDir 'src\client_dx11'
  $copied = $false
  if (Test-Path -LiteralPath $packClient -PathType Container) {
    $items = @(Get-ChildItem -LiteralPath $packClient -Force -ErrorAction SilentlyContinue)
    if ($items.Count -gt 0) {
      Copy-Item -Path (Join-Path $packClient '*') -Destination $DstClient -Recurse -Force -ErrorAction Stop
      $copied = $true
      Log "Copied DX11 client source from bundled folder: $packClient"
    }
  }

  if (!$copied) {
    Log 'Bundled src\client_dx11 folder was not found next to the script; writing embedded DX11 client source files.'
    Write-TextNoBom -Path (Join-Path $DstClient 'd3d11_dx11bridge.cpp') -Text @'
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include "dx11_common.h"

using PFN_D3D11CreateDevice = HRESULT (WINAPI *)(
  IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT,
  const D3D_FEATURE_LEVEL*, UINT, UINT,
  ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);

using PFN_D3D11CreateDeviceAndSwapChain = HRESULT (WINAPI *)(
  IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT,
  const D3D_FEATURE_LEVEL*, UINT, UINT,
  const DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**,
  ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);

static HMODULE g_d3d11System = nullptr;
static PFN_D3D11CreateDevice g_CreateDevice = nullptr;
static PFN_D3D11CreateDeviceAndSwapChain g_CreateDeviceAndSwapChain = nullptr;

static bool ensureSystemD3D11() {
  if (g_d3d11System) return true;
  g_d3d11System = dx11_bridge_getgoing::loadSystemDll("d3d11.dll");
  if (!g_d3d11System) return false;
  g_CreateDevice = reinterpret_cast<PFN_D3D11CreateDevice>(GetProcAddress(g_d3d11System, "D3D11CreateDevice"));
  g_CreateDeviceAndSwapChain = reinterpret_cast<PFN_D3D11CreateDeviceAndSwapChain>(GetProcAddress(g_d3d11System, "D3D11CreateDeviceAndSwapChain"));
  if (!g_CreateDevice || !g_CreateDeviceAndSwapChain) {
    dx11_bridge_getgoing::logLine("d3d11", "System d3d11.dll is missing required exports.");
    return false;
  }
  dx11_bridge_getgoing::logLine("d3d11", "Loaded system d3d11.dll exports.");
  return true;
}

BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH) {
    dx11_bridge_getgoing::setModule(hinst);
    DisableThreadLibraryCalls(hinst);
    dx11_bridge_getgoing::logLine("d3d11", "Loaded x86 DX11 bridge bring-up d3d11.dll.");
  }
  return TRUE;
}

extern "C" HRESULT WINAPI D3D11CreateDevice(
  IDXGIAdapter* pAdapter,
  D3D_DRIVER_TYPE DriverType,
  HMODULE Software,
  UINT Flags,
  const D3D_FEATURE_LEVEL* pFeatureLevels,
  UINT FeatureLevels,
  UINT SDKVersion,
  ID3D11Device** ppDevice,
  D3D_FEATURE_LEVEL* pFeatureLevel,
  ID3D11DeviceContext** ppImmediateContext) {

  dx11_bridge_getgoing::logLine("d3d11", "D3D11CreateDevice intercepted.");
  dx11_bridge_getgoing::launchBridgeServerOnce();
  if (!ensureSystemD3D11()) return E_FAIL;

  HRESULT hr = g_CreateDevice(pAdapter, DriverType, Software, Flags, pFeatureLevels,
    FeatureLevels, SDKVersion, ppDevice, pFeatureLevel, ppImmediateContext);

  char msg[128] = {};
  wsprintfA(msg, "D3D11CreateDevice forwarded hr=0x%08lX", static_cast<unsigned long>(hr));
  dx11_bridge_getgoing::logLine("d3d11", msg);
  return hr;
}

extern "C" HRESULT WINAPI D3D11CreateDeviceAndSwapChain(
  IDXGIAdapter* pAdapter,
  D3D_DRIVER_TYPE DriverType,
  HMODULE Software,
  UINT Flags,
  const D3D_FEATURE_LEVEL* pFeatureLevels,
  UINT FeatureLevels,
  UINT SDKVersion,
  const DXGI_SWAP_CHAIN_DESC* pSwapChainDesc,
  IDXGISwapChain** ppSwapChain,
  ID3D11Device** ppDevice,
  D3D_FEATURE_LEVEL* pFeatureLevel,
  ID3D11DeviceContext** ppImmediateContext) {

  dx11_bridge_getgoing::logLine("d3d11", "D3D11CreateDeviceAndSwapChain intercepted.");
  dx11_bridge_getgoing::launchBridgeServerOnce();
  if (!ensureSystemD3D11()) return E_FAIL;

  HRESULT hr = g_CreateDeviceAndSwapChain(pAdapter, DriverType, Software, Flags,
    pFeatureLevels, FeatureLevels, SDKVersion, pSwapChainDesc, ppSwapChain,
    ppDevice, pFeatureLevel, ppImmediateContext);

  char msg[128] = {};
  wsprintfA(msg, "D3D11CreateDeviceAndSwapChain forwarded hr=0x%08lX", static_cast<unsigned long>(hr));
  dx11_bridge_getgoing::logLine("d3d11", msg);
  return hr;
}

'@
    Write-TextNoBom -Path (Join-Path $DstClient 'd3d11_dx11bridge.def') -Text @'
LIBRARY "d3d11"
EXPORTS
  D3D11CreateDevice
  D3D11CreateDeviceAndSwapChain

'@
    Write-TextNoBom -Path (Join-Path $DstClient 'dx11_common.h') -Text @'
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>

namespace dx11_bridge_getgoing {

inline HMODULE g_module = nullptr;

inline void setModule(HMODULE m) { g_module = m; }

inline void getDllFolder(char* out, DWORD cap) {
  out[0] = 0;
  char path[MAX_PATH] = {};
  if (g_module) GetModuleFileNameA(g_module, path, MAX_PATH);
  char* slash = strrchr(path, '\\');
  if (slash) {
    slash[1] = 0;
    lstrcpynA(out, path, cap);
  }
}

inline void logLine(const char* tag, const char* msg) {
  char dir[MAX_PATH] = {};
  getDllFolder(dir, MAX_PATH);
  char path[MAX_PATH] = {};
  lstrcpynA(path, dir, MAX_PATH);
  lstrcatA(path, "dx11_bridge_getgoing.log");
  FILE* f = nullptr;
  fopen_s(&f, path, "ab");
  if (f) {
    SYSTEMTIME st; GetLocalTime(&st);
    fprintf(f, "[%04u-%02u-%02u %02u:%02u:%02u.%03u] [%s] %s\r\n",
      st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
      tag ? tag : "dx11", msg ? msg : "");
    fclose(f);
  }
  char dbg[1024] = {};
  wsprintfA(dbg, "[%s] %s\n", tag ? tag : "dx11", msg ? msg : "");
  OutputDebugStringA(dbg);
}

inline HMODULE loadSystemDll(const char* name) {
  char sys[MAX_PATH] = {};
  GetSystemDirectoryA(sys, MAX_PATH);
  lstrcatA(sys, "\\");
  lstrcatA(sys, name);
  HMODULE mod = LoadLibraryA(sys);
  if (!mod) {
    char msg[512] = {};
    wsprintfA(msg, "LoadLibrary failed for %s err=%lu", sys, GetLastError());
    logLine("loader", msg);
  }
  return mod;
}

inline bool fileExists(const char* p) { return GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES; }

inline void launchBridgeServerOnce() {
  static LONG launched = 0;
  if (InterlockedCompareExchange(&launched, 1, 0) != 0) return;

  char dir[MAX_PATH] = {};
  getDllFolder(dir, MAX_PATH);
  char server[MAX_PATH] = {};
  lstrcpynA(server, dir, MAX_PATH);
  lstrcatA(server, ".trex\\NvRemixBridge.exe");

  if (!fileExists(server)) {
    logLine("bridge", "No .trex\\NvRemixBridge.exe next to d3d11.dll/dxgi.dll; running DX11 passthrough only.");
    return;
  }

  char cmd[MAX_PATH + 64] = {};
  wsprintfA(cmd, "\"%s\" --dx11", server);
  STARTUPINFOA si = {};
  PROCESS_INFORMATION pi = {};
  si.cb = sizeof(si);
  BOOL ok = CreateProcessA(nullptr, cmd, nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, dir, &si, &pi);
  if (ok) {
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    logLine("bridge", "Started .trex\\NvRemixBridge.exe --dx11");
  } else {
    char msg[256] = {};
    wsprintfA(msg, "Failed to start NvRemixBridge.exe err=%lu", GetLastError());
    logLine("bridge", msg);
  }
}

} // namespace

'@
    Write-TextNoBom -Path (Join-Path $DstClient 'dxgi_dx11bridge.cpp') -Text @'
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dxgi.h>
#include "dx11_common.h"

typedef HRESULT (WINAPI *PFN_CreateDXGIFactory)(REFIID, void**);
typedef HRESULT (WINAPI *PFN_CreateDXGIFactory1)(REFIID, void**);
typedef HRESULT (WINAPI *PFN_CreateDXGIFactory2)(UINT, REFIID, void**);
typedef HRESULT (WINAPI *PFN_DXGIDeclareAdapterRemovalSupport)();
typedef HRESULT (WINAPI *PFN_DXGIGetDebugInterface1)(UINT, REFIID, void**);

static HMODULE g_dxgiSystem = nullptr;
static FARPROC getDxgiProc(const char* name) {
  if (!g_dxgiSystem) {
    g_dxgiSystem = dx11_bridge_getgoing::loadSystemDll("dxgi.dll");
    if (g_dxgiSystem) dx11_bridge_getgoing::logLine("dxgi", "Loaded system dxgi.dll.");
  }
  return g_dxgiSystem ? GetProcAddress(g_dxgiSystem, name) : nullptr;
}

BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH) {
    dx11_bridge_getgoing::setModule(hinst);
    DisableThreadLibraryCalls(hinst);
    dx11_bridge_getgoing::logLine("dxgi", "Loaded x86 DX11 bridge bring-up dxgi.dll.");
  }
  return TRUE;
}

extern "C" HRESULT WINAPI CreateDXGIFactory(REFIID riid, void** ppFactory) {
  dx11_bridge_getgoing::logLine("dxgi", "CreateDXGIFactory intercepted.");
  dx11_bridge_getgoing::launchBridgeServerOnce();
  auto fn = reinterpret_cast<PFN_CreateDXGIFactory>(getDxgiProc("CreateDXGIFactory"));
  return fn ? fn(riid, ppFactory) : E_FAIL;
}

extern "C" HRESULT WINAPI CreateDXGIFactory1(REFIID riid, void** ppFactory) {
  dx11_bridge_getgoing::logLine("dxgi", "CreateDXGIFactory1 intercepted.");
  dx11_bridge_getgoing::launchBridgeServerOnce();
  auto fn = reinterpret_cast<PFN_CreateDXGIFactory1>(getDxgiProc("CreateDXGIFactory1"));
  return fn ? fn(riid, ppFactory) : E_FAIL;
}

extern "C" HRESULT WINAPI CreateDXGIFactory2(UINT Flags, REFIID riid, void** ppFactory) {
  dx11_bridge_getgoing::logLine("dxgi", "CreateDXGIFactory2 intercepted.");
  dx11_bridge_getgoing::launchBridgeServerOnce();
  auto fn = reinterpret_cast<PFN_CreateDXGIFactory2>(getDxgiProc("CreateDXGIFactory2"));
  return fn ? fn(Flags, riid, ppFactory) : E_FAIL;
}

extern "C" HRESULT WINAPI DXGIDeclareAdapterRemovalSupport() {
  auto fn = reinterpret_cast<PFN_DXGIDeclareAdapterRemovalSupport>(getDxgiProc("DXGIDeclareAdapterRemovalSupport"));
  return fn ? fn() : E_NOTIMPL;
}

extern "C" HRESULT WINAPI DXGIGetDebugInterface1(UINT Flags, REFIID riid, void** ppDebug) {
  auto fn = reinterpret_cast<PFN_DXGIGetDebugInterface1>(getDxgiProc("DXGIGetDebugInterface1"));
  return fn ? fn(Flags, riid, ppDebug) : E_NOTIMPL;
}

'@
    Write-TextNoBom -Path (Join-Path $DstClient 'dxgi_dx11bridge.def') -Text @'
LIBRARY "dxgi"
EXPORTS
  CreateDXGIFactory
  CreateDXGIFactory1
  CreateDXGIFactory2
  DXGIDeclareAdapterRemovalSupport
  DXGIGetDebugInterface1

'@
    Write-TextNoBom -Path (Join-Path $DstClient 'meson.build') -Text @'
dx11_bridge_inc = include_directories('.')

d3d11_dx11bridge_dll = shared_library('d3d11',
  files('d3d11_dx11bridge.cpp'),
  files('d3d11_dx11bridge.def'),
  include_directories: [util_include_path, dx11_bridge_inc],
  cpp_args: ['/EHsc'],
  dependencies: [lib_version],
  install: false
)

dxgi_dx11bridge_dll = shared_library('dxgi',
  files('dxgi_dx11bridge.cpp'),
  files('dxgi_dx11bridge.def'),
  include_directories: [util_include_path, dx11_bridge_inc],
  cpp_args: ['/EHsc'],
  dependencies: [lib_version],
  install: false
)

run_target('copy_dx11_bridge_client',
  depends: [d3d11_dx11bridge_dll, dxgi_dx11bridge_dll],
  command: [copy_script_path, meson.current_build_dir().replace('\\', '/'), output_dir, 'd3d11*'])

run_target('copy_dxgi_bridge_client',
  depends: [dxgi_dx11bridge_dll],
  command: [copy_script_path, meson.current_build_dir().replace('\\', '/'), output_dir, 'dxgi*'])

'@
  }

  $required = @(
    'd3d11_dx11bridge.cpp',
    'd3d11_dx11bridge.def',
    'dx11_common.h',
    'dxgi_dx11bridge.cpp',
    'dxgi_dx11bridge.def'
  )
  foreach ($r in $required) {
    $rp = Join-Path $DstClient $r
    if (!(Test-Path -LiteralPath $rp -PathType Leaf)) {
      Die "DX11 client source generation/copy failed; missing required file: $rp"
    }
  }
  Repair-MesonUtf8NoBom $DstClient
  Log "Verified DX11 client source files in: $DstClient"
}

function Prepare-DX11BridgeSource([string]$PackDir) {
  $sourceBridge = Join-Path $Root 'bridge'
  if (!(Test-Path (Join-Path $sourceBridge 'meson.build'))) {
    $cloneRoot = Join-Path $Root '_nvidia_dxvk_remix_for_dx11_bridge'
    if ($Clean -and (Test-Path $cloneRoot)) {
      Log "Removing old clone: $cloneRoot"
      Remove-DirectoryRobust $cloneRoot
    }

    if (Test-Path (Join-Path $cloneRoot 'bridge\meson.build')) {
      Log "Using existing NVIDIA bridge clone: $cloneRoot"
    } else {
      if (Test-Path $cloneRoot) {
        Warn "Existing clone folder is incomplete/non-empty: $cloneRoot"
        Remove-DirectoryRobust $cloneRoot
      }
      $git = CmdPath 'git.exe'; if (!$git) { Die 'git.exe is required to clone NVIDIA bridge source.' }
      Log "Cloning NVIDIA dxvk-remix for bridge source: $BridgeRepoUrl ($BridgeBranch)"
      Invoke-Logged -Label 'git-clone-bridge' -Exe $git -CommandArgs @('clone','--recursive','--branch',$BridgeBranch,$BridgeRepoUrl,$cloneRoot) -WorkingDirectory $Root | Out-Null
      if (!(Test-Path -LiteralPath (Join-Path $cloneRoot 'bridge\meson.build') -PathType Leaf)) { Die "Git clone completed but bridge\meson.build was not created: $cloneRoot" }
    }
    $sourceBridge = Join-Path $cloneRoot 'bridge'
  }
  if (!(Test-Path (Join-Path $sourceBridge 'src\client'))) { Die "Bridge source missing client folder: $sourceBridge" }

  $work = Join-Path $Root 'bridge_dx11_work'
  if ($Clean -and (Test-Path $work)) { Log "Removing old DX11 bridge work tree: $work"; Remove-DirectoryRobust $work }
  if (!(Test-Path $work)) { Log "Copying bridge source to $work"; Copy-Item -LiteralPath $sourceBridge -Destination $work -Recurse -Force }

  $dstClient = Join-Path $work 'src\client_dx11'
  Ensure-DX11ClientSources -DstClient $dstClient -PackDir $PackDir

  $srcMeson = Join-Path $work 'src\meson.build'
  if (!(Test-Path $srcMeson)) { Die "Missing bridge src meson: $srcMeson" }
  $text = Get-Content $srcMeson -Raw
  if ($text -notmatch "subdir\('client_dx11'\)") {
    if ($text -match "subdir\('client'\)") {
      $text = $text -replace "subdir\('client'\)", "subdir('client')`r`nif cpu_family == 'x86'`r`n  subdir('client_dx11')`r`nendif"
    } else { $text += "`r`nif cpu_family == 'x86'`r`n  subdir('client_dx11')`r`nendif`r`n" }
    Write-TextNoBom -Path $srcMeson -Text $text
    Log 'Patched bridge src/meson.build to build client_dx11 on x86.'
  }
  Repair-MesonUtf8NoBom $work

  # Newer MSVC/SDKs can emit warnings in NVIDIA bridge utility sources that become fatal
  # because upstream bridge/meson.build sets default_options : ['werror=true', ...].
  # This project is being used as a source base for a DX11 bridge bring-up, so do not let
  # warnings stop the server build before we can stage/test the DX11 pickup DLLs.
  $bridgeMeson = Join-Path $work 'meson.build'
  if (Test-Path -LiteralPath $bridgeMeson -PathType Leaf) {
    $bridgeText = Get-Content -LiteralPath $bridgeMeson -Raw
    $patchedBridgeText = $bridgeText -replace "'werror=true'", "'werror=false'"
    if ($patchedBridgeText -ne $bridgeText) {
      Write-TextNoBom -Path $bridgeMeson -Text $patchedBridgeText
      Log 'Patched bridge meson.build: werror=false so MSVC warnings do not abort bridge server build.'
    }
  }
  Patch-BridgeServerRegisterD3D11 -BridgeWork $work
  return $work
}

function Get-ExistingRuntimeDir {
  $candidates = @(
    (Join-Path $Root '_output\x64'),
    (Join-Path $Root '_Comp64Release'),
    (Join-Path $Root '_Comp64Release\src\d3d11'),
    (Join-Path $Root '_Comp64Release\src\dxgi')
  )
  foreach ($dir in $candidates) {
    if (!(Test-Path -LiteralPath $dir -PathType Container)) { continue }
    $d3d11 = Find-FileRecursive $dir 'd3d11.dll'
    $dxgi = Find-FileRecursive $dir 'dxgi.dll'
    if ($d3d11 -and $dxgi) {
      try {
        Test-PeMachine $d3d11 'x64'
        Test-PeMachine $dxgi 'x64'
        Log "Found existing x64 DX11 runtime: $dir"
        return $dir
      } catch { Warn "Ignoring runtime candidate ${dir}: $($_.Exception.Message)" }
    }
  }
  return $null
}

function Build-X64Runtime([string]$VsInstall, [string]$Meson, [string]$Ninja) {
  if (!$RebuildRuntime) {
    $existing = Get-ExistingRuntimeDir
    if ($existing) {
      Log 'Using existing x64 runtime. Pass -RebuildRuntime to force Meson/Ninja runtime rebuild.'
      return $existing
    }
  }
  if ($SkipRuntimeBuild) { return (Join-Path $Root '_Comp64Release') }
  Import-VSEnvironment $VsInstall 'x64'
  $env:NINJA = $Ninja
  $env:Path = (Split-Path -Parent $Ninja) + ';' + $env:Path
  Repair-FutureTimestamps
  if (-not $NoDepsFetch) {
    Ensure-Deps $Meson
  } else {
    Warn 'Skipping Packman dependency fetch because -NoDepsFetch was passed.'
  }
  $buildDir = Join-Path $Root '_Comp64Release'
  if ($RebuildRuntime -and (Test-Path $buildDir)) { Log "Removing runtime build because -RebuildRuntime was requested: $buildDir"; Remove-DirectoryRobust $buildDir }
  $buildNinja = Join-Path $buildDir 'build.ninja'
  if (!(Test-Path $buildNinja)) {
    Log 'Configuring x64 DX11+USD runtime.'
    $mesonArgs = @('setup','--buildtype=release','--backend=ninja','-Denable_dxgi=true','-Denable_d3d11=true','-Denable_tests=false','-Denable_tracy=false','-Dskip_packman_fetch=true',$buildDir,$Root)
    Invoke-Logged -Label 'runtime-meson-x64' -Exe $Meson -CommandArgs $mesonArgs -WorkingDirectory $Root -AllowIfFileExists $buildNinja | Out-Null
  }
  if (!(Test-Path $buildNinja)) { Die "Meson did not create build.ninja: $buildNinja" }
  Log 'Building x64 DX11+USD runtime.'
  Invoke-Logged -Label 'runtime-ninja-x64' -Exe $Ninja -CommandArgs @('-C',$buildDir,'-v') -WorkingDirectory $Root | Out-Null
  try { Invoke-Logged -Label 'runtime-install-x64' -Exe $Meson -CommandArgs @('install','-C',$buildDir,'--no-rebuild','--tags','output') -WorkingDirectory $Root | Out-Null } catch { Warn $_.Exception.Message }
  $existing2 = Get-ExistingRuntimeDir
  if ($existing2) { return $existing2 }
  return $buildDir
}

function Find-MSVCVersionRoot([string]$VsInstall) {
  $root = Join-Path $VsInstall 'VC\Tools\MSVC'
  $v = Get-ChildItem -LiteralPath $root -Directory -ErrorAction SilentlyContinue | Sort-Object Name -Descending | Select-Object -First 1
  if (!$v) { Die "MSVC tools folder not found under $root" }
  return $v.FullName
}
function Build-DX11ClientDirect([string]$VsInstall, [string]$ClientSrc) {
  $msvc = Find-MSVCVersionRoot $VsInstall
  $cl = Join-Path $msvc 'bin\Hostx64\x86\cl.exe'
  if (!(Test-Path $cl)) { $cl = Join-Path $msvc 'bin\HostX64\x86\cl.exe' }
  if (!(Test-Path $cl)) { Die "HostX64 x86 cl.exe not found under $msvc" }
  $kitsRoot = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10'
  $sdkIncRoot = Join-Path $kitsRoot 'Include'
  $sdkLibRoot = Join-Path $kitsRoot 'Lib'
  $sdkBinRoot = Join-Path $kitsRoot 'bin'
  $sdkVer = Get-ChildItem -LiteralPath $sdkIncRoot -Directory -ErrorAction SilentlyContinue | Sort-Object Name -Descending | Select-Object -First 1
  if (!$sdkVer) { Die 'Windows 10 SDK include folder not found.' }
  $sdk = $sdkVer.Name
  $env:INCLUDE = @(
    (Join-Path $msvc 'include'),
    (Join-Path $sdkIncRoot "$sdk\ucrt"),
    (Join-Path $sdkIncRoot "$sdk\shared"),
    (Join-Path $sdkIncRoot "$sdk\um"),
    (Join-Path $sdkIncRoot "$sdk\winrt"),
    (Join-Path $sdkIncRoot "$sdk\cppwinrt")
  ) -join ';'
  $env:LIB = @(
    (Join-Path $msvc 'lib\x86'),
    (Join-Path $sdkLibRoot "$sdk\ucrt\x86"),
    (Join-Path $sdkLibRoot "$sdk\um\x86")
  ) -join ';'
  $env:Path = @(
    (Split-Path -Parent $cl),
    (Join-Path $sdkBinRoot "$sdk\x64"),
    (Join-Path $VsInstall 'Common7\IDE'),
    $env:Path
  ) -join ';'
  $out = Join-Path $Root 'bridge_dx11_work\_dx11_client_x86'
  if ($Clean -and (Test-Path $out)) { Remove-Item -LiteralPath $out -Recurse -Force }
  New-Item -ItemType Directory -Force -Path $out | Out-Null
  foreach ($needed in @('d3d11_dx11bridge.cpp','d3d11_dx11bridge.def','dxgi_dx11bridge.cpp','dxgi_dx11bridge.def','dx11_common.h')) {
    $np = Join-Path $ClientSrc $needed
    if (!(Test-Path -LiteralPath $np -PathType Leaf)) { Die "Missing DX11 client build input: $np" }
  }
  $link = Join-Path (Split-Path -Parent $cl) 'link.exe'
  if (!(Test-Path -LiteralPath $link -PathType Leaf)) { Die "link.exe not found next to cl.exe: $link" }

  $d3d11Cpp = Join-Path $ClientSrc 'd3d11_dx11bridge.cpp'
  $d3d11Def = Join-Path $ClientSrc 'd3d11_dx11bridge.def'
  $d3d11Obj = Join-Path $out 'd3d11_dx11bridge.obj'
  $d3d11Dll = Join-Path $out 'd3d11.dll'
  $d3d11Lib = Join-Path $out 'd3d11.lib'

  $dxgiCpp = Join-Path $ClientSrc 'dxgi_dx11bridge.cpp'
  $dxgiDef = Join-Path $ClientSrc 'dxgi_dx11bridge.def'
  $dxgiObj = Join-Path $out 'dxgi_dx11bridge.obj'
  $dxgiDll = Join-Path $out 'dxgi.dll'
  $dxgiLib = Join-Path $out 'dxgi.lib'

  Log "Compiling x86 d3d11 bridge object with: $cl"
  Invoke-Logged -Label 'client-d3d11-x86-compile' -Exe $cl -CommandArgs @(
    '/nologo','/c','/O2','/MT','/EHsc','/std:c++17','/utf-8','/Zc:__cplusplus',
    '/I', $ClientSrc,
    ('/Fo:' + $d3d11Obj),
    $d3d11Cpp
  ) -WorkingDirectory $Root | Out-Null

  Log "Linking x86 d3d11.dll directly with explicit .def: $d3d11Def"
  Invoke-Logged -Label 'client-d3d11-x86-link' -Exe $link -CommandArgs @(
    '/NOLOGO','/DLL','/MACHINE:X86',
    ('/OUT:' + $d3d11Dll),
    ('/IMPLIB:' + $d3d11Lib),
    ('/DEF:' + $d3d11Def),
    $d3d11Obj,
    'user32.lib','kernel32.lib'
  ) -WorkingDirectory $Root | Out-Null

  Log "Compiling x86 dxgi bridge object with: $cl"
  Invoke-Logged -Label 'client-dxgi-x86-compile' -Exe $cl -CommandArgs @(
    '/nologo','/c','/O2','/MT','/EHsc','/std:c++17','/utf-8','/Zc:__cplusplus',
    '/I', $ClientSrc,
    ('/Fo:' + $dxgiObj),
    $dxgiCpp
  ) -WorkingDirectory $Root | Out-Null

  Log "Linking x86 dxgi.dll directly with explicit .def: $dxgiDef"
  Invoke-Logged -Label 'client-dxgi-x86-link' -Exe $link -CommandArgs @(
    '/NOLOGO','/DLL','/MACHINE:X86',
    ('/OUT:' + $dxgiDll),
    ('/IMPLIB:' + $dxgiLib),
    ('/DEF:' + $dxgiDef),
    $dxgiObj,
    'user32.lib','kernel32.lib'
  ) -WorkingDirectory $Root | Out-Null

  if (!(Test-Path -LiteralPath $d3d11Dll -PathType Leaf)) { Die "d3d11 client link finished but output DLL is missing: $d3d11Dll" }
  if (!(Test-Path -LiteralPath $dxgiDll -PathType Leaf)) { Die "dxgi client link finished but output DLL is missing: $dxgiDll" }
  Test-PeMachine $d3d11Dll 'x86'
  Test-PeMachine $dxgiDll 'x86'
  return $out
}

function Build-DX11Bridge([string]$BridgeWork, [string]$VsInstall, [string]$Meson, [string]$Ninja) {
  $b64 = Join-Path $BridgeWork '_Comp64Release'
  if ($Clean -and (Test-Path $b64)) { Log "Removing bridge server build: $b64"; Remove-Item -LiteralPath $b64 -Recurse -Force }
  if (!$SkipBridgeBuild) {
    Import-VSEnvironment $VsInstall 'x64'
    $env:NINJA = $Ninja
    $env:Path = (Split-Path -Parent $Ninja) + ';' + $env:Path
    Repair-MesonUtf8NoBom $BridgeWork
    $buildNinja = Join-Path $b64 'build.ninja'
    Log 'Building official x64 bridge server from source.'
    if (!(Test-Path $buildNinja)) { Invoke-Logged -Label 'bridge-server-meson-x64' -Exe $Meson -CommandArgs @('setup','--buildtype=release','--backend=ninja','-Dwerror=false','-Denable_tests=false',$b64,$BridgeWork) -WorkingDirectory $BridgeWork -AllowIfFileExists $buildNinja | Out-Null }
    Invoke-Logged -Label 'bridge-server-ninja-x64' -Exe $Ninja -CommandArgs @('-C',$b64,'-v') -WorkingDirectory $BridgeWork | Out-Null
  }
  $clientOut = Build-DX11ClientDirect $VsInstall (Join-Path $BridgeWork 'src\client_dx11')
  return @{ Build64 = $b64; Build32 = $clientOut }
}

function Copy-Usd([string]$PkgTrex) {
  $candidates = @(
    (Join-Path $Root '_output\x64\usd'),
    (Join-Path $Root '_output\usd'),
    (Join-Path $Root '_Comp64Release\usd'),
    (Join-Path $Root 'external\nv_usd_Release'),
    (Join-Path $Root 'external\nv_usd_release'),
    (Join-Path $Root 'src\usd-plugins\_external\nv_usd\release')
  )
  foreach ($c in $candidates) {
    if (Test-Path -LiteralPath $c -PathType Container) {
      $dst = Join-Path $PkgTrex 'usd'
      $srcFull = [System.IO.Path]::GetFullPath($c).TrimEnd('\')
      $dstFull = [System.IO.Path]::GetFullPath($dst).TrimEnd('\')
      if ($srcFull -ieq $dstFull) {
        Log "USD folder already staged: $dst"
        return
      }
      if (Test-Path -LiteralPath $dst) { Remove-Item -LiteralPath $dst -Recurse -Force }
      Log "Copying USD runtime/dev tree from: $c"
      Copy-Item -LiteralPath $c -Destination $dst -Recurse -Force
      return
    }
  }
  Warn 'No USD folder found to stage.'
}


function Copy-FileIfDifferent {
  param(
    [Parameter(Mandatory)][string]$Source,
    [Parameter(Mandatory)][string]$Destination
  )
  $srcFull = [System.IO.Path]::GetFullPath($Source).TrimEnd('\')
  $dstFull = [System.IO.Path]::GetFullPath($Destination).TrimEnd('\')
  if ($srcFull -ieq $dstFull) {
    Log "Already staged, skipping self-copy: $Destination"
    return
  }
  $dstDir = Split-Path -Parent $Destination
  if ($dstDir -and !(Test-Path -LiteralPath $dstDir -PathType Container)) {
    New-Item -ItemType Directory -Force -Path $dstDir | Out-Null
  }
  Copy-Item -LiteralPath $Source -Destination $Destination -Force
}

function Copy-DirectoryContentsSafe {
  param(
    [Parameter(Mandatory)][string]$Source,
    [Parameter(Mandatory)][string]$Destination
  )
  if (!(Test-Path -LiteralPath $Source -PathType Container)) { return $false }
  New-Item -ItemType Directory -Force -Path $Destination | Out-Null
  $items = @(Get-ChildItem -LiteralPath $Source -Force -ErrorAction SilentlyContinue)
  foreach ($item in $items) {
    $target = Join-Path $Destination $item.Name
    if (([System.IO.Path]::GetFullPath($item.FullName).TrimEnd('\')) -ieq ([System.IO.Path]::GetFullPath($target).TrimEnd('\'))) {
      Log "Skipping self-copy while mirroring: $target"
      continue
    }
    Copy-Item -LiteralPath $item.FullName -Destination $Destination -Recurse -Force
  }
  return $true
}

function Remove-D3D9Artifacts {
  param([Parameter(Mandatory)][string]$BaseDir)
  foreach ($bad in @(Get-ChildItem -LiteralPath $BaseDir -Recurse -File -ErrorAction SilentlyContinue | Where-Object { $_.Name -ieq 'd3d9.dll' })) {
    Warn "Removing unwanted D3D9 artifact from DX11 output: $($bad.FullName)"
    Remove-Item -LiteralPath $bad.FullName -Force
  }
}

function Resolve-X64RuntimeFiles {
  param([Parameter(Mandatory)][string]$RuntimeBuild)
  $rtD3D11 = Find-FileRecursive $RuntimeBuild 'd3d11.dll'
  $rtDXGI = Find-FileRecursive $RuntimeBuild 'dxgi.dll'
  if (!$rtD3D11 -or !$rtDXGI) {
    $rtD3D11 = Join-Path $Root '_output\x64\d3d11.dll'
    $rtDXGI = Join-Path $Root '_output\x64\dxgi.dll'
  }
  if (!(Test-Path -LiteralPath $rtD3D11 -PathType Leaf) -or !(Test-Path -LiteralPath $rtDXGI -PathType Leaf)) {
    Die "x64 runtime d3d11.dll/dxgi.dll missing. RuntimeBuild=$RuntimeBuild"
  }
  Test-PeMachine $rtD3D11 'x64'
  Test-PeMachine $rtDXGI 'x64'
  return @{ D3D11 = $rtD3D11; DXGI = $rtDXGI }
}

function Same-FullPath {
  param([Parameter(Mandatory)][string]$A, [Parameter(Mandatory)][string]$B)
  $af = [System.IO.Path]::GetFullPath($A).TrimEnd('\')
  $bf = [System.IO.Path]::GetFullPath($B).TrimEnd('\')
  return ($af -ieq $bf)
}

function Stage-DualOutput([string]$RuntimeBuild, [hashtable]$BridgeBuilds) {
  # v50: final user-facing layout is _output\x64 and _output\x86, matching the folder
  # the repo already uses for the x64 Meson install tree. Do not delete _output\x64
  # when it is also the installed runtime tree; keep its support DLL layout intact.
  $outRoot = if ([IO.Path]::IsPathRooted($OutputRoot)) { $OutputRoot } else { Join-Path $Root $OutputRoot }
  $x64Out = Join-Path $outRoot 'x64'
  $x86Out = Join-Path $outRoot 'x86'
  $x86Trex = Join-Path $x86Out '.trex'
  $installedX64 = Join-Path $Root '_output\x64'
  $sameAsInstalledX64 = Same-FullPath -A $x64Out -B $installedX64

  $runtime = Resolve-X64RuntimeFiles $RuntimeBuild

  if ($sameAsInstalledX64) {
    New-Item -ItemType Directory -Force -Path $x64Out | Out-Null
    Log "Using existing Meson x64 output folder as final x64 output: $x64Out"
  } else {
    if (Test-Path -LiteralPath $x64Out) {
      Log "Removing old staged x64 output folder: $x64Out"
      Remove-Item -LiteralPath $x64Out -Recurse -Force
    }
    New-Item -ItemType Directory -Force -Path $x64Out | Out-Null
    if (Test-Path -LiteralPath $installedX64 -PathType Container) {
      Log "Copying installed x64 output tree: $installedX64 -> $x64Out"
      [void](Copy-DirectoryContentsSafe -Source $installedX64 -Destination $x64Out)
    }
  }

  Copy-FileIfDifferent -Source $runtime.D3D11 -Destination (Join-Path $x64Out 'd3d11.dll')
  Copy-FileIfDifferent -Source $runtime.DXGI  -Destination (Join-Path $x64Out 'dxgi.dll')
  Test-PeMachine (Join-Path $x64Out 'd3d11.dll') 'x64'
  Test-PeMachine (Join-Path $x64Out 'dxgi.dll') 'x64'
  Copy-Usd $x64Out
  Remove-D3D9Artifacts $x64Out

  if (Test-Path -LiteralPath $x86Out) {
    Log "Removing old staged x86 output folder: $x86Out"
    Remove-Item -LiteralPath $x86Out -Recurse -Force
  }
  New-Item -ItemType Directory -Force -Path $x86Trex | Out-Null

  $clientD3D11 = Join-Path $BridgeBuilds.Build32 'd3d11.dll'
  $clientDXGI = Join-Path $BridgeBuilds.Build32 'dxgi.dll'
  if (!(Test-Path -LiteralPath $clientD3D11 -PathType Leaf)) { Die "x86 bridge d3d11.dll missing: $clientD3D11" }
  if (!(Test-Path -LiteralPath $clientDXGI -PathType Leaf)) { Die "x86 bridge dxgi.dll missing: $clientDXGI" }

  Copy-FileIfDifferent -Source $clientD3D11 -Destination (Join-Path $x86Out 'd3d11.dll')
  Copy-FileIfDifferent -Source $clientDXGI  -Destination (Join-Path $x86Out 'dxgi.dll')
  Test-PeMachine (Join-Path $x86Out 'd3d11.dll') 'x86'
  Test-PeMachine (Join-Path $x86Out 'dxgi.dll') 'x86'

  # The 32-bit package needs the complete x64 runtime/support tree inside .trex,
  # not just d3d11.dll and dxgi.dll. Mirror the final x64 folder so the x86 layout
  # has the same support DLLs/USD structure as the native x64 output.
  Log "Mirroring final x64 output tree into x86 .trex: $x64Out -> $x86Trex"
  [void](Copy-DirectoryContentsSafe -Source $x64Out -Destination $x86Trex)

  $server = Find-FileRecursive $BridgeBuilds.Build64 'NvRemixBridge.exe'
  if ($server) {
    Copy-FileIfDifferent -Source $server -Destination (Join-Path $x86Trex 'NvRemixBridge.exe')
    Test-PeMachine (Join-Path $x86Trex 'NvRemixBridge.exe') 'x64'
  } else {
    Warn "x64 NvRemixBridge.exe was not found under $($BridgeBuilds.Build64); x86 output will include root DX11 bridge DLLs and .trex runtime but no bridge server."
  }

  # Ensure the .trex runtime entry DLLs are the actual x64 runtime build outputs.
  Copy-FileIfDifferent -Source $runtime.D3D11 -Destination (Join-Path $x86Trex 'd3d11.dll')
  Copy-FileIfDifferent -Source $runtime.DXGI  -Destination (Join-Path $x86Trex 'dxgi.dll')
  Test-PeMachine (Join-Path $x86Trex 'd3d11.dll') 'x64'
  Test-PeMachine (Join-Path $x86Trex 'dxgi.dll') 'x64'
  Copy-Usd $x86Trex
  Remove-D3D9Artifacts $x86Out

  $x64Readme = @"
DXVK Remix DX11 x64 output v50
==============================

Use this folder for native 64-bit DX11 games.

This folder intentionally matches the repo's normal _output\x64 structure:
  d3d11.dll      x64 DX11 runtime from this repo
  dxgi.dll       x64 DXGI runtime from this repo
  usd\           USD runtime/data when available
  *.dll          required x64 runtime/dependency DLLs from the build/install tree

No d3d9.dll is staged here.
"@
  Set-Content -LiteralPath (Join-Path $x64Out 'README_X64_DX11_OUTPUT_V50.txt') -Encoding UTF8 -Value $x64Readme

  $x86Readme = @"
DXVK Remix DX11 x86 bridge output v50
=====================================

Use this folder for 32-bit DX11 games.

Root layout:
  d3d11.dll                 x86 DX11 bridge pickup/interposer DLL
  dxgi.dll                  x86 DXGI bridge pickup/interposer DLL
  .trex\NvRemixBridge.exe   x64 bridge server when the bridge build produced it
  .trex\d3d11.dll           x64 DX11 runtime from this repo
  .trex\dxgi.dll            x64 DXGI runtime from this repo
  .trex\usd\                USD runtime/data when available
  .trex\*.dll               required x64 runtime/dependency DLLs mirrored from _output\x64

No d3d9.dll is staged here.
"@
  Set-Content -LiteralPath (Join-Path $x86Out 'README_X86_DX11_BRIDGE_OUTPUT_V50.txt') -Encoding UTF8 -Value $x86Readme

  $pkgRoot = Join-Path $Root '_packages'
  New-Item -ItemType Directory -Force -Path $pkgRoot | Out-Null
  $zip = Join-Path $pkgRoot 'dxvk-remix-dx11-output-x64-x86-v50.zip'
  if (Test-Path -LiteralPath $zip) { Remove-Item -LiteralPath $zip -Force }
  Add-Type -AssemblyName System.IO.Compression.FileSystem
  [System.IO.Compression.ZipFile]::CreateFromDirectory($outRoot, $zip)

  Log "Output x64: $x64Out"
  Log "Output x86: $x86Out"
  Log "Combined output zip: $zip"
}

function Stage-Package([string]$RuntimeBuild, [hashtable]$BridgeBuilds) {
  $pkg = Join-Path $Root '_packages\rtx-remix-dx11-x86-bridge-getgoing-v50'
  if (Test-Path $pkg) { Remove-Item -LiteralPath $pkg -Recurse -Force }
  $trex = Join-Path $pkg '.trex'
  New-Item -ItemType Directory -Force -Path $trex | Out-Null

  $clientD3D11 = Join-Path $BridgeBuilds.Build32 'd3d11.dll'
  $clientDXGI = Join-Path $BridgeBuilds.Build32 'dxgi.dll'
  $server = Find-FileRecursive $BridgeBuilds.Build64 'NvRemixBridge.exe'
  if (!$server) { Warn "x64 NvRemixBridge.exe was not found under $($BridgeBuilds.Build64); package will include DX11 pickup DLLs and .trex runtime but no bridge server." }
  Copy-Item -LiteralPath $clientD3D11 -Destination (Join-Path $pkg 'd3d11.dll') -Force
  Copy-Item -LiteralPath $clientDXGI -Destination (Join-Path $pkg 'dxgi.dll') -Force
  if ($server) { Copy-Item -LiteralPath $server -Destination (Join-Path $trex 'NvRemixBridge.exe') -Force }
  Test-PeMachine (Join-Path $pkg 'd3d11.dll') 'x86'
  Test-PeMachine (Join-Path $pkg 'dxgi.dll') 'x86'
  if ($server) { Test-PeMachine (Join-Path $trex 'NvRemixBridge.exe') 'x64' }

  $rtD3D11 = Find-FileRecursive $RuntimeBuild 'd3d11.dll'
  $rtDXGI = Find-FileRecursive $RuntimeBuild 'dxgi.dll'
  if (!$rtD3D11 -or !$rtDXGI) {
    $rtD3D11 = Join-Path $Root '_output\x64\d3d11.dll'
    $rtDXGI = Join-Path $Root '_output\x64\dxgi.dll'
  }
  if (!(Test-Path $rtD3D11) -or !(Test-Path $rtDXGI)) { Die "x64 runtime d3d11.dll/dxgi.dll missing. RuntimeBuild=$RuntimeBuild" }
  Copy-Item -LiteralPath $rtD3D11 -Destination (Join-Path $trex 'd3d11.dll') -Force
  Copy-Item -LiteralPath $rtDXGI -Destination (Join-Path $trex 'dxgi.dll') -Force
  Test-PeMachine (Join-Path $trex 'd3d11.dll') 'x64'
  Test-PeMachine (Join-Path $trex 'dxgi.dll') 'x64'
  Copy-Usd $trex
  foreach ($bad in @((Join-Path $pkg 'd3d9.dll'), (Join-Path $trex 'd3d9.dll'))) { if (Test-Path $bad) { Remove-Item -LiteralPath $bad -Force } }

  $readme = @"
RTX Remix DX11 x86 Bridge Get-Going Package v50
================================================

Layout:
  d3d11.dll                 x86 DX11 pickup/interposer DLL
  dxgi.dll                  x86 DXGI pickup/interposer DLL
  .trex\NvRemixBridge.exe   x64 bridge server when bridge source build produced it
  .trex\d3d11.dll           x64 DX11 runtime from this repo
  .trex\dxgi.dll            x64 DXGI runtime from this repo
  .trex\usd\                USD runtime/data copied from dependency/build tree when found

No d3d9.dll is included.

v50 fixes:
  - strips UTF-8 BOMs from bridge Meson files before Meson setup
  - writes patched Meson files as UTF-8 without BOM
  - prints Meson/Ninja logs live and tails them on failure
  - continues if Meson exits nonzero after creating build.ninja
  - builds the x86 root d3d11.dll/dxgi.dll directly with HostX64\\x86 cl.exe, avoiding vcvarsall x64_x86 quoting failures
"@
  Set-Content -Path (Join-Path $pkg 'README_DX11_BRIDGE_GETGOING_V50.txt') -Value $readme -Encoding UTF8
  $zip = "$pkg.zip"
  if (Test-Path $zip) { Remove-Item -LiteralPath $zip -Force }
  Add-Type -AssemblyName System.IO.Compression.FileSystem
  [System.IO.Compression.ZipFile]::CreateFromDirectory($pkg, $zip)
  Log "Package: $pkg"
  Log "Zip: $zip"
}

$vs = Find-VSInstall
$meson = Find-Meson
$ninja = Find-Ninja $vs
Log "Visual Studio: $vs"
Log "Meson: $meson"
Log "Ninja: $ninja"
$bridgeWork = Prepare-DX11BridgeSource $Root
if ($SourceOnly) { Log "Source-only requested. Work tree: $bridgeWork"; exit 0 }
$runtimeBuild = Build-X64Runtime $vs $meson $ninja
$bridgeBuilds = Build-DX11Bridge $bridgeWork $vs $meson $ninja
Stage-DualOutput $runtimeBuild $bridgeBuilds
