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

function Log([string]$m) { Write-Host "[dx11-output-v57] $m" }
function Warn([string]$m) { Write-Host "[dx11-output-v57] WARNING: $m" -ForegroundColor Yellow }
function Die([string]$m) { throw "[dx11-output-v57] $m" }
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

  if ($text -notmatch 'DX11_BRIDGE_REGISTER_HELPER_V57') {
    if ($text -notmatch '#include\s+<d3d11\.h>') {
      $text = $text -replace '#include\s+<d3d9\.h>', "#include <d3d9.h>`r`n#include <d3d11.h>"
    }
    $helper = @'

#ifndef DX11_BRIDGE_REGISTER_HELPER_V57
#define DX11_BRIDGE_REGISTER_HELPER_V57
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
  # v57: always refresh the generated DX11 client source. Older runs left stale
  # client DLL source that launched NvRemixBridge.exe with the wrong command line.
  # Do not trust existing bridge_dx11_work\src\client_dx11 contents.
  if (Test-Path -LiteralPath $packClient -PathType Container) {
    $items = @(Get-ChildItem -LiteralPath $packClient -Force -ErrorAction SilentlyContinue)
    if ($items.Count -gt 0) {
      Copy-Item -Path (Join-Path $packClient '*') -Destination $DstClient -Recurse -Force -ErrorAction Stop
      $copied = $true
      Log "Refreshed DX11 client source from bundled folder: $packClient"
    }
  }

  if (!$copied) {
    Log 'Writing embedded DX11 client source files with v57 GUID/version bridge launch fix.'
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
#include <objbase.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>

namespace dx11_bridge_getgoing {

inline HMODULE g_module = nullptr;

inline void setModule(HMODULE m) { g_module = m; }

inline void getDllFolder(char* out, DWORD cap) {
  out[0] = 0;
  char path[MAX_PATH] = {};
  if (g_module) GetModuleFileNameA(g_module, path, MAX_PATH);
  char* slash = strrchr(path, '\');
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
    fprintf(f, "[%04u-%02u-%02u %02u:%02u:%02u.%03u] [%s] %s
",
      st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
      tag ? tag : "dx11", msg ? msg : "");
    fclose(f);
  }
  char dbg[1024] = {};
  wsprintfA(dbg, "[%s] %s
", tag ? tag : "dx11", msg ? msg : "");
  OutputDebugStringA(dbg);
}

inline HMODULE loadSystemDll(const char* name) {
  char sys[MAX_PATH] = {};
  GetSystemDirectoryA(sys, MAX_PATH);
  lstrcatA(sys, "\");
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

inline void trimAscii(char* s) {
  if (!s) return;
  char* start = s;
  while (*start && isspace((unsigned char)*start)) ++start;
  if (start != s) memmove(s, start, strlen(start) + 1);
  size_t len = strlen(s);
  while (len > 0 && isspace((unsigned char)s[len - 1])) s[--len] = 0;
}

inline bool readTextFileSmall(const char* path, char* out, DWORD cap) {
  if (!out || cap == 0) return false;
  out[0] = 0;
  HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  DWORD got = 0;
  BOOL ok = ReadFile(h, out, cap - 1, &got, nullptr);
  CloseHandle(h);
  if (!ok) return false;
  out[got] = 0;
  trimAscii(out);
  return out[0] != 0;
}

inline bool readBridgeVersion(char* out, DWORD cap) {
  char dir[MAX_PATH] = {};
  getDllFolder(dir, MAX_PATH);
  char path[MAX_PATH] = {};
  lstrcpynA(path, dir, MAX_PATH);
  lstrcatA(path, ".trex\\bridge_version.txt");
  if (readTextFileSmall(path, out, cap)) return true;

  lstrcpynA(path, dir, MAX_PATH);
  lstrcatA(path, "bridge_version.txt");
  if (readTextFileSmall(path, out, cap)) return true;

  logLine("bridge", "Missing bridge_version.txt; cannot launch NvRemixBridge.exe with matching version.");
  return false;
}

inline bool makeGuidString(char* out, DWORD cap) {
  if (!out || cap < 37) return false;
  out[0] = 0;

  GUID g = {};
  HRESULT hr = CoCreateGuid(&g);
  if (FAILED(hr)) {
    char msg[128] = {};
    wsprintfA(msg, "CoCreateGuid failed hr=0x%08lX", static_cast<unsigned long>(hr));
    logLine("bridge", msg);
    return false;
  }

  wchar_t wideGuid[64] = {};
  int wideLen = StringFromGUID2(g, wideGuid, 64); // produces {xxxxxxxx-....}
  if (wideLen <= 0) {
    logLine("bridge", "StringFromGUID2 failed.");
    return false;
  }

  char tmp[64] = {};
  int mb = WideCharToMultiByte(CP_ACP, 0, wideGuid, -1, tmp, sizeof(tmp), nullptr, nullptr);
  if (mb <= 0) {
    logLine("bridge", "WideCharToMultiByte failed for GUID.");
    return false;
  }

  // NVIDIA bridge server's Guid::setGuid expects exactly 36 chars, no braces.
  // StringFromGUID2 returns braces, so strip them deterministically.
  size_t len = strlen(tmp);
  if (len == 38 && tmp[0] == '{' && tmp[37] == '}') {
    memcpy(out, tmp + 1, 36);
    out[36] = 0;
  } else if (len == 36) {
    lstrcpynA(out, tmp, cap);
  } else {
    char msg[128] = {};
    wsprintfA(msg, "Unexpected GUID string length %lu", static_cast<unsigned long>(len));
    logLine("bridge", msg);
    return false;
  }

  char msg[128] = {};
  wsprintfA(msg, "Generated bridge GUID '%s' len=%lu", out, static_cast<unsigned long>(strlen(out)));
  logLine("bridge", msg);
  return strlen(out) == 36;
}

inline void appendLaunchLog(const char* text) {
  char dir[MAX_PATH] = {};
  getDllFolder(dir, MAX_PATH);
  char path[MAX_PATH] = {};
  lstrcpynA(path, dir, MAX_PATH);
  lstrcatA(path, "dx11_bridge_launch.log");
  HANDLE h = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return;
  DWORD wrote = 0;
  if (text) WriteFile(h, text, (DWORD)strlen(text), &wrote, nullptr);
  const char* crlf = "
";
  WriteFile(h, crlf, 2, &wrote, nullptr);
  CloseHandle(h);
}

inline void launchBridgeServerOnce() {
  static LONG launched = 0;
  if (InterlockedCompareExchange(&launched, 1, 0) != 0) return;

  char mutexName[96] = {};
  sprintf_s(mutexName, sizeof(mutexName), "Local\DxvkRemixDx11BridgeLauncher_%lu", GetCurrentProcessId());
  static HANDLE launchMutex = nullptr;
  launchMutex = CreateMutexA(nullptr, TRUE, mutexName);
  if (!launchMutex) {
    char msg[128] = {};
    wsprintfA(msg, "CreateMutex failed err=%lu", GetLastError());
    logLine("bridge", msg);
    appendLaunchLog(msg);
    return;
  }
  if (GetLastError() == ERROR_ALREADY_EXISTS) {
    logLine("bridge", "Bridge server launch already owned by another DX11 bridge DLL in this process.");
    appendLaunchLog("Bridge server launch already owned by another DX11 bridge DLL in this process.");
    return;
  }

  char dir[MAX_PATH] = {};
  getDllFolder(dir, MAX_PATH);
  char server[MAX_PATH] = {};
  lstrcpynA(server, dir, MAX_PATH);
  lstrcatA(server, ".trex\NvRemixBridge.exe");

  if (!fileExists(server)) {
    logLine("bridge", "No .trex\NvRemixBridge.exe next to d3d11.dll/dxgi.dll; running DX11 passthrough only.");
    appendLaunchLog("No .trex\NvRemixBridge.exe next to d3d11.dll/dxgi.dll; running DX11 passthrough only.");
    return;
  }

  char version[128] = {};
  if (!readBridgeVersion(version, sizeof(version))) {
    appendLaunchLog("Missing bridge_version.txt; cannot launch NvRemixBridge.exe with matching version.");
    return;
  }

  char guid[64] = {};
  if (!makeGuidString(guid, sizeof(guid))) {
    appendLaunchLog("GUID generation failed; not launching NvRemixBridge.exe.");
    return;
  }

  // NVIDIA bridge bootstrap rule: the server must see pCmdLine as:
  //   <GUID> <BRIDGE_VERSION>
  // Its server main parses CommandLineToArgvW(pCmdLine), then expects
  // argList[0] to be the 36-char GUID and argList[1] to be BRIDGE_VERSION_W.
  // This DLL is the bridge client bootstrap. NvRemixLauncher32.exe is only an
  // optional injector for games that refuse to load the root DLL by search order.
  // Use lpApplicationName=server and lpCommandLine=GUID VERSION so WinMain's
  // pCmdLine is exactly what NvRemixBridge.exe expects, with no executable path
  // accidentally becoming argList[0].
  char cmd[MAX_PATH + 320] = {};
  sprintf_s(cmd, sizeof(cmd), "%s %s", guid, version);

  // Also provide fallback paths for the patched DX11 bridge server.
  // These are inherited by NvRemixBridge.exe and used only if pCmdLine is empty
  // or a launcher/CRT strips it unexpectedly.
  SetEnvironmentVariableA("DX11_BRIDGE_GUID", guid);
  SetEnvironmentVariableA("DX11_BRIDGE_VERSION", version);
  SetEnvironmentVariableA("DX11_BRIDGE_MODE", "d3d11");

  char argFile[MAX_PATH] = {};
  lstrcpynA(argFile, dir, MAX_PATH);
  lstrcatA(argFile, ".trex\dx11_bridge_args.txt");
  HANDLE af = CreateFileA(argFile, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (af != INVALID_HANDLE_VALUE) {
    DWORD wrote = 0;
    WriteFile(af, guid, (DWORD)strlen(guid), &wrote, nullptr);
    WriteFile(af, "\r\n", 2, &wrote, nullptr);
    WriteFile(af, version, (DWORD)strlen(version), &wrote, nullptr);
    WriteFile(af, "\r\n", 2, &wrote, nullptr);
    CloseHandle(af);
  }

  char logMsg[768] = {};
  sprintf_s(logMsg, sizeof(logMsg), "Launching NVIDIA-style DX11 client->server app='%s' args='%s' cwd='%s' envGUID='%s' envVersion='%s' argsFile='%s'", server, cmd, dir, guid, version, argFile);
  logLine("bridge", logMsg);
  appendLaunchLog(logMsg);

  STARTUPINFOA si = {};
  PROCESS_INFORMATION pi = {};
  si.cb = sizeof(si);
  BOOL ok = CreateProcessA(server, cmd, nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, dir, &si, &pi);
  if (ok) {
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    logLine("bridge", "Started .trex\NvRemixBridge.exe with GUID/version arguments.");
    appendLaunchLog("Started .trex\NvRemixBridge.exe with GUID/version arguments.");
  } else {
    char msg[256] = {};
    wsprintfA(msg, "Failed to start NvRemixBridge.exe err=%lu", GetLastError());
    logLine("bridge", msg);
    appendLaunchLog(msg);
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
  Patch-BridgeServerDx11ArgFallback -BridgeWork $work
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
    'user32.lib','kernel32.lib','ole32.lib'
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
    'user32.lib','kernel32.lib','ole32.lib'
  ) -WorkingDirectory $Root | Out-Null

  if (!(Test-Path -LiteralPath $d3d11Dll -PathType Leaf)) { Die "d3d11 client link finished but output DLL is missing: $d3d11Dll" }
  if (!(Test-Path -LiteralPath $dxgiDll -PathType Leaf)) { Die "dxgi client link finished but output DLL is missing: $dxgiDll" }
  Test-PeMachine $d3d11Dll 'x86'
  Test-PeMachine $dxgiDll 'x86'


  # v57: build the NVIDIA-style 32-bit launcher EXE at the same root level
  # as the x86 DX11 bridge DLLs. This mirrors the public x86 Remix package
  # placement: root launcher + root interposer DLLs + .trex x64 runtime/server.
  $launcherCpp = Join-Path $out 'NvRemixLauncher32.cpp'
  $launcherObj = Join-Path $out 'NvRemixLauncher32.obj'
  $launcherExe = Join-Path $out 'NvRemixLauncher32.exe'
  $launcherPdb = Join-Path $out 'NvRemixLauncher32.pdb'
  Set-Content -LiteralPath $launcherCpp -Encoding UTF8 -Value @'
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <stdio.h>
#include <wchar.h>

static void dirnameOf(const wchar_t* in, wchar_t* out, DWORD cap) {
  out[0] = 0;
  if (!in || !in[0]) return;
  lstrcpynW(out, in, cap);
  wchar_t* slash = wcsrchr(out, L'\\');
  if (slash) slash[1] = 0;
}

static void appendLog(const wchar_t* root, const wchar_t* msg) {
  wchar_t path[MAX_PATH] = {};
  lstrcpynW(path, root, MAX_PATH);
  lstrcatW(path, L"NvRemixLauncher32.log");
  HANDLE h = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return;
  SYSTEMTIME st; GetLocalTime(&st);
  char line[4096] = {};
  char m[3072] = {};
  WideCharToMultiByte(CP_UTF8, 0, msg ? msg : L"", -1, m, sizeof(m), nullptr, nullptr);
  int n = sprintf_s(line, sizeof(line), "[%04u-%02u-%02u %02u:%02u:%02u.%03u] %s\r\n",
    st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, m);
  DWORD wrote = 0;
  if (n > 0) WriteFile(h, line, (DWORD)n, &wrote, nullptr);
  CloseHandle(h);
}

static bool existsFile(const wchar_t* p) {
  DWORD a = GetFileAttributesW(p);
  return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static bool isExcludedExe(const wchar_t* name) {
  return _wcsicmp(name, L"NvRemixLauncher32.exe") == 0 ||
         _wcsicmp(name, L"NvRemixBridge.exe") == 0 ||
         _wcsicmp(name, L"dxvk-remix.exe") == 0 ||
         _wcsicmp(name, L"setup.exe") == 0 ||
         _wcsicmp(name, L"unins000.exe") == 0;
}

static bool autoFindGameExe(const wchar_t* root, wchar_t* out, DWORD cap) {
  wchar_t pattern[MAX_PATH] = {};
  lstrcpynW(pattern, root, MAX_PATH);
  lstrcatW(pattern, L"*.exe");
  WIN32_FIND_DATAW fd = {};
  HANDLE h = FindFirstFileW(pattern, &fd);
  if (h == INVALID_HANDLE_VALUE) return false;
  do {
    if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !isExcludedExe(fd.cFileName)) {
      lstrcpynW(out, root, cap);
      lstrcatW(out, fd.cFileName);
      FindClose(h);
      return true;
    }
  } while (FindNextFileW(h, &fd));
  FindClose(h);
  return false;
}

static void quoteAppend(wchar_t* dst, DWORD cap, const wchar_t* s) {
  lstrcatW(dst, L"\"");
  lstrcatW(dst, s);
  lstrcatW(dst, L"\"");
}

static bool pathLooksRooted(const wchar_t* p) {
  return (p && ((wcslen(p) > 2 && p[1] == L':') || (p[0] == L'\\' && p[1] == L'\\')));
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
  wchar_t self[MAX_PATH] = {};
  GetModuleFileNameW(nullptr, self, MAX_PATH);
  wchar_t root[MAX_PATH] = {};
  dirnameOf(self, root, MAX_PATH);

  appendLog(root, L"NvRemixLauncher32 DX11 started.");

  // Match the x86 Remix package placement: launcher in root, x64 runtime/server in .trex.
  wchar_t trex[MAX_PATH] = {};
  lstrcpynW(trex, root, MAX_PATH);
  lstrcatW(trex, L".trex\\");

  wchar_t d3d11[MAX_PATH] = {}, dxgi[MAX_PATH] = {}, bridge[MAX_PATH] = {};
  lstrcpynW(d3d11, root, MAX_PATH); lstrcatW(d3d11, L"d3d11.dll");
  lstrcpynW(dxgi,  root, MAX_PATH); lstrcatW(dxgi,  L"dxgi.dll");
  lstrcpynW(bridge, trex, MAX_PATH); lstrcatW(bridge, L"NvRemixBridge.exe");

  if (!existsFile(d3d11) || !existsFile(dxgi)) {
    appendLog(root, L"ERROR: root d3d11.dll/dxgi.dll missing beside launcher.");
    MessageBoxW(nullptr, L"DX11 Remix root d3d11.dll/dxgi.dll is missing beside NvRemixLauncher32.exe.", L"DX11 Remix Launcher", MB_ICONERROR);
    return 2;
  }
  if (!existsFile(bridge)) {
    appendLog(root, L"ERROR: .trex\\NvRemixBridge.exe missing.");
    MessageBoxW(nullptr, L".trex\\NvRemixBridge.exe is missing beside the DX11 Remix package.", L"DX11 Remix Launcher", MB_ICONERROR);
    return 3;
  }

  int argc = 0;
  LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);

  wchar_t target[MAX_PATH] = {};
  wchar_t extra[4096] = {};
  if (argv && argc >= 2) {
    if (pathLooksRooted(argv[1])) {
      lstrcpynW(target, argv[1], MAX_PATH);
    } else {
      lstrcpynW(target, root, MAX_PATH);
      lstrcatW(target, argv[1]);
    }
    for (int i = 2; i < argc; i++) {
      if (extra[0]) lstrcatW(extra, L" ");
      quoteAppend(extra, 4096, argv[i]);
    }
  } else {
    if (!autoFindGameExe(root, target, MAX_PATH)) {
      appendLog(root, L"ERROR: no game exe argument and no auto-detected exe in package root.");
      MessageBoxW(nullptr, L"Put NvRemixLauncher32.exe beside the 32-bit DX11 game exe, or run:\nNvRemixLauncher32.exe Game.exe", L"DX11 Remix Launcher", MB_ICONERROR);
      if (argv) LocalFree(argv);
      return 4;
    }
  }
  if (argv) LocalFree(argv);

  if (!existsFile(target)) {
    appendLog(root, L"ERROR: requested game exe does not exist.");
    MessageBoxW(nullptr, L"Requested game executable was not found.", L"DX11 Remix Launcher", MB_ICONERROR);
    return 5;
  }

  // Inherit a PATH where the game can resolve root x86 DLLs and .trex support DLLs.
  wchar_t oldPath[32767] = {};
  GetEnvironmentVariableW(L"PATH", oldPath, 32767);
  wchar_t newPath[32767] = {};
  lstrcpynW(newPath, root, 32767);
  lstrcatW(newPath, L";");
  lstrcatW(newPath, trex);
  lstrcatW(newPath, L";");
  lstrcatW(newPath, oldPath);
  SetEnvironmentVariableW(L"PATH", newPath);
  SetEnvironmentVariableW(L"DXVK_REMIX_DX11_BRIDGE", L"1");
  SetEnvironmentVariableW(L"DXVK_REMIX_LAUNCHED_BY_NVREMIXLAUNCHER32", L"1");

  wchar_t cmd[8192] = {};
  quoteAppend(cmd, 8192, target);
  if (extra[0]) { lstrcatW(cmd, L" "); lstrcatW(cmd, extra); }

  wchar_t logMsg[8192] = {};
  swprintf_s(logMsg, L"Launching game: %s ; cwd=%s", cmd, root);
  appendLog(root, logMsg);

  STARTUPINFOW si = {};
  PROCESS_INFORMATION pi = {};
  si.cb = sizeof(si);
  BOOL ok = CreateProcessW(nullptr, cmd, nullptr, nullptr, TRUE, 0, nullptr, root, &si, &pi);
  if (!ok) {
    wchar_t e[512] = {};
    swprintf_s(e, L"ERROR: CreateProcessW failed. GetLastError=%lu", GetLastError());
    appendLog(root, e);
    MessageBoxW(nullptr, e, L"DX11 Remix Launcher", MB_ICONERROR);
    return 6;
  }
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  appendLog(root, L"Game process started successfully.");
  return 0;
}
'@

  Log "Compiling x86 NvRemixLauncher32.exe with: $cl"
  Invoke-Logged -Label 'launcher32-x86-compile' -Exe $cl -CommandArgs @(
    '/nologo','/c','/O2','/MT','/EHsc','/std:c++17','/utf-8','/Zc:__cplusplus',
    ('/Fo:' + $launcherObj),
    $launcherCpp
  ) -WorkingDirectory $Root | Out-Null

  Log "Linking x86 NvRemixLauncher32.exe"
  Invoke-Logged -Label 'launcher32-x86-link' -Exe $link -CommandArgs @(
    '/NOLOGO','/MACHINE:X86','/SUBSYSTEM:WINDOWS',
    ('/OUT:' + $launcherExe),
    ('/PDB:' + $launcherPdb),
    $launcherObj,
    'user32.lib','kernel32.lib','shell32.lib'
  ) -WorkingDirectory $Root | Out-Null

  if (!(Test-Path -LiteralPath $launcherExe -PathType Leaf)) { Die "NvRemixLauncher32.exe link finished but output is missing: $launcherExe" }
  Test-PeMachine $launcherExe 'x86'

  return $out
}


function Patch-BridgeServerDx11ArgFallback {
  param([Parameter(Mandatory)][string]$BridgeWork)
  $main = Join-Path $BridgeWork 'src\server\main.cpp'
  if (!(Test-Path -LiteralPath $main -PathType Leaf)) { Die "Bridge server main.cpp missing: $main" }
  $text = Get-Content -LiteralPath $main -Raw
  if ($text -match 'DX11_BRIDGE_ARG_FALLBACK_V57') {
    Log 'Bridge server DX11 arg fallback already patched.'
    return
  }

  $helper = @'

// DX11_BRIDGE_ARG_FALLBACK_V57
// The stock bridge server expects wWinMain pCmdLine to contain exactly:
//   <36-char-guid> <BRIDGE_VERSION_W>
// For DX11 bridge bring-up we also accept DX11_BRIDGE_GUID/DX11_BRIDGE_VERSION
// from the x86 launcher environment, and .trex\dx11_bridge_args.txt as a file
// fallback. This prevents the server from exiting before the DX11 client IPC path
// can be brought up.
static wchar_t g_Dx11BridgeGuidArgV57[64] = {};
static wchar_t g_Dx11BridgeVersionArgV57[256] = {};
static LPWSTR g_Dx11BridgeArgListV57[2] = { g_Dx11BridgeGuidArgV57, g_Dx11BridgeVersionArgV57 };

static bool Dx11BridgeReadEnvArgV57(const wchar_t* name, wchar_t* out, DWORD cap) {
  if (!name || !out || cap == 0) return false;
  out[0] = 0;
  const DWORD got = GetEnvironmentVariableW(name, out, cap);
  return got > 0 && got < cap && out[0] != 0;
}

static bool Dx11BridgeReadArgsFileV57(wchar_t* guidOut, DWORD guidCap, wchar_t* verOut, DWORD verCap) {
  wchar_t path[MAX_PATH] = {};
  if (GetModuleFileNameW(nullptr, path, MAX_PATH) == 0) return false;
  wchar_t* slash = wcsrchr(path, L'\\');
  if (!slash) return false;
  slash[1] = 0;
  if (wcslen(path) + wcslen(L"dx11_bridge_args.txt") + 1 >= MAX_PATH) return false;
  wcscat_s(path, L"dx11_bridge_args.txt");

  HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) {
    if (GetModuleFileNameW(nullptr, path, MAX_PATH) == 0) return false;
    slash = wcsrchr(path, L'\\');
    if (!slash) return false;
    slash[1] = 0;
    if (wcslen(path) + wcslen(L".trex\\dx11_bridge_args.txt") + 1 >= MAX_PATH) return false;
    wcscat_s(path, L".trex\\dx11_bridge_args.txt");
    h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
  }

  char buf[512] = {};
  DWORD got = 0;
  BOOL ok = ReadFile(h, buf, sizeof(buf) - 1, &got, nullptr);
  CloseHandle(h);
  if (!ok || got == 0) return false;
  buf[got] = 0;

  char* first = buf;
  while (*first == ' ' || *first == '\t' || *first == '\r' || *first == '\n') ++first;
  char* second = first;
  while (*second && *second != '\r' && *second != '\n') ++second;
  if (*second) *second++ = 0;
  while (*second == ' ' || *second == '\t' || *second == '\r' || *second == '\n') ++second;
  char* end2 = second;
  while (*end2 && *end2 != '\r' && *end2 != '\n') ++end2;
  *end2 = 0;
  if (!first[0] || !second[0]) return false;

  MultiByteToWideChar(CP_ACP, 0, first, -1, guidOut, guidCap);
  MultiByteToWideChar(CP_ACP, 0, second, -1, verOut, verCap);
  return guidOut[0] != 0 && verOut[0] != 0;
}

static LPWSTR* Dx11BridgeBuildFallbackArgListV57(int* pArgCount) {
  if (!pArgCount) return nullptr;
  *pArgCount = 0;
  bool gotGuid = Dx11BridgeReadEnvArgV57(L"DX11_BRIDGE_GUID", g_Dx11BridgeGuidArgV57, _countof(g_Dx11BridgeGuidArgV57));
  bool gotVer = Dx11BridgeReadEnvArgV57(L"DX11_BRIDGE_VERSION", g_Dx11BridgeVersionArgV57, _countof(g_Dx11BridgeVersionArgV57));
  if (!gotGuid || !gotVer) {
    gotGuid = gotVer = Dx11BridgeReadArgsFileV57(g_Dx11BridgeGuidArgV57, _countof(g_Dx11BridgeGuidArgV57), g_Dx11BridgeVersionArgV57, _countof(g_Dx11BridgeVersionArgV57));
  }
  if (!gotGuid || !gotVer) return nullptr;
  *pArgCount = 2;
  return g_Dx11BridgeArgListV57;
}
'@

  $marker = 'int WINAPI wWinMain('
  if ($text -notmatch [regex]::Escape($marker)) { Die 'Could not find wWinMain in bridge server main.cpp for DX11 arg fallback patch.' }
  $text = $text.Replace($marker, $helper + "`r`n" + $marker)

  $old = 'int argCount; LPWSTR* argList = CommandLineToArgvW(pCmdLine, &argCount); BRIDGE_ASSERT_LOG((argCount >= 2), "Command line argument count received to launch server is not as expected");'
  $new = 'int argCount = 0; LPWSTR* argList = CommandLineToArgvW(pCmdLine, &argCount); bool dx11FallbackArgListV57 = false; if (argCount < 2 || argList == nullptr) { Logger::warn("DX11 bridge: missing/empty command line arguments; trying DX11_BRIDGE_GUID/DX11_BRIDGE_VERSION fallback."); if (argList) { LocalFree(argList); argList = nullptr; } argList = Dx11BridgeBuildFallbackArgListV57(&argCount); dx11FallbackArgListV57 = true; } if (argCount < 2 || argList == nullptr) { Logger::err("DX11 bridge: server still has no GUID/version after command-line and fallback parsing."); return 1; }'
  if ($text.Contains($old)) {
    $text = $text.Replace($old, $new)
  } else {
    # Current dxvk-remix bridge may have line breaks; use a conservative regex.
    $pat = 'int\s+argCount\s*;\s*LPWSTR\*\s*argList\s*=\s*CommandLineToArgvW\(pCmdLine,\s*&argCount\);\s*BRIDGE_ASSERT_LOG\s*\(\s*\(argCount\s*>=\s*2\)\s*,\s*"Command line argument count received to launch server is not as expected"\s*\);'
    $text2 = [regex]::Replace($text, $pat, $new, 1)
    if ($text2 -eq $text) { Die 'Could not patch bridge server argument parser.' }
    $text = $text2
  }

  $text = $text.Replace('LocalFree(argList); initModuleBridge();', 'if (!dx11FallbackArgListV57 && argList) { LocalFree(argList); } initModuleBridge();')
  Write-TextNoBom -Path $main -Text $text
  Log 'Patched bridge server: GUID/version command-line fallback for DX11 launcher.'
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


function Get-BridgeVersionFromBuild {
  param([Parameter(Mandatory)][string]$BridgeBuild64)
  $candidates = @(
    (Join-Path $BridgeBuild64 'version.h'),
    (Join-Path (Split-Path -Parent $BridgeBuild64) 'version.h')
  )
  foreach ($c in $candidates) {
    if (Test-Path -LiteralPath $c -PathType Leaf) {
      $txt = Get-Content -LiteralPath $c -Raw -ErrorAction SilentlyContinue
      $m = [regex]::Match($txt, '#define\s+BRIDGE_VERSION\s+"([^"]+)"')
      if ($m.Success -and $m.Groups[1].Value.Trim().Length -gt 0) {
        return $m.Groups[1].Value.Trim()
      }
    }
  }
  return $null
}

function Write-BridgeVersionFile {
  param(
    [Parameter(Mandatory)][string]$TrexDir,
    [Parameter(Mandatory)][string]$BridgeBuild64
  )
  $ver = Get-BridgeVersionFromBuild -BridgeBuild64 $BridgeBuild64
  if (!$ver) {
    Warn "Could not resolve bridge BRIDGE_VERSION from $BridgeBuild64; x86 bridge server launch may fail version check."
    return
  }
  New-Item -ItemType Directory -Force -Path $TrexDir | Out-Null
  $dst = Join-Path $TrexDir 'bridge_version.txt'
  Set-Content -LiteralPath $dst -Value $ver -Encoding ASCII -NoNewline
  Log "Wrote bridge version for x86 client/server launch: $dst = $ver"
}

function Stage-DualOutput([string]$RuntimeBuild, [hashtable]$BridgeBuilds) {
  # v57: final user-facing layout is _output\x64 and _output\x86, matching the folder
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

  $launcher32 = Join-Path $BridgeBuilds.Build32 'NvRemixLauncher32.exe'
  $launcher32Pdb = Join-Path $BridgeBuilds.Build32 'NvRemixLauncher32.pdb'
  if (Test-Path -LiteralPath $launcher32 -PathType Leaf) {
    Copy-FileIfDifferent -Source $launcher32 -Destination (Join-Path $x86Out 'NvRemixLauncher32.exe')
    Test-PeMachine (Join-Path $x86Out 'NvRemixLauncher32.exe') 'x86'
    if (Test-Path -LiteralPath $launcher32Pdb -PathType Leaf) { Copy-FileIfDifferent -Source $launcher32Pdb -Destination (Join-Path $x86Out 'NvRemixLauncher32.pdb') }
  } else {
    Warn "NvRemixLauncher32.exe was not built; x86 output will still load through normal game launch if d3d11.dll/dxgi.dll are in the game folder."
  }

  $artifactReadme = @"
DXVK Remix DX11 x86 Bridge Package v57
======================================

Placement matches the NVIDIA x86 bridge package style:

Root:
  d3d11.dll                  32-bit DX11 bridge pickup DLL
  dxgi.dll                   32-bit DXGI bridge pickup DLL
  NvRemixLauncher32.exe      32-bit launcher/bootstrap helper for DX11 games
  NvRemixLauncher32.pdb      launcher symbols when built
  artifacts_readme.txt       this file

.trex:
  NvRemixBridge.exe          64-bit bridge server
  bridge_version.txt         bridge version passed by the root DX11 bridge DLLs
  d3d11.dll                  64-bit DX11 runtime from this repo
  dxgi.dll                   64-bit DXGI runtime from this repo
  usd\                       USD runtime/data when available
  *.dll                      64-bit support DLLs mirrored from _output\x64

Usage:
  Preferred: copy this folder next to the 32-bit DX11 game exe and launch the game normally. The root d3d11.dll/dxgi.dll are the DX11 bridge client and start/connect to .trex\NvRemixBridge.exe automatically. NvRemixLauncher32.exe is optional injection fallback only.
  Also works: launch the game normally after copying d3d11.dll, dxgi.dll, and .trex beside the game exe.

No d3d9.dll is staged in this DX11 package.
"@
  Set-Content -LiteralPath (Join-Path $x86Out 'artifacts_readme.txt') -Encoding UTF8 -Value $artifactReadme

  # The 32-bit package needs the complete x64 runtime/support tree inside .trex,
  # not just d3d11.dll and dxgi.dll. Mirror the final x64 folder so the x86 layout
  # has the same support DLLs/USD structure as the native x64 output.
  Log "Mirroring final x64 output tree into x86 .trex: $x64Out -> $x86Trex"
  [void](Copy-DirectoryContentsSafe -Source $x64Out -Destination $x86Trex)

  $server = Find-FileRecursive $BridgeBuilds.Build64 'NvRemixBridge.exe'
  if ($server) {
    Copy-FileIfDifferent -Source $server -Destination (Join-Path $x86Trex 'NvRemixBridge.exe')
    Test-PeMachine (Join-Path $x86Trex 'NvRemixBridge.exe') 'x64'
    Write-BridgeVersionFile -TrexDir $x86Trex -BridgeBuild64 $BridgeBuilds.Build64
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
DXVK Remix DX11 x64 output v57
==============================

Use this folder for native 64-bit DX11 games.

This folder intentionally matches the repo's normal _output\x64 structure:
  d3d11.dll      x64 DX11 runtime from this repo
  dxgi.dll       x64 DXGI runtime from this repo
  usd\           USD runtime/data when available
  *.dll          required x64 runtime/dependency DLLs from the build/install tree

No d3d9.dll is staged here.
"@
  Set-Content -LiteralPath (Join-Path $x64Out 'README_X64_DX11_OUTPUT_V57.txt') -Encoding UTF8 -Value $x64Readme

  $x86Readme = @"
DXVK Remix DX11 x86 bridge output v57
=====================================

Use this folder for 32-bit DX11 games.

Root layout:
  d3d11.dll                 x86 DX11 bridge pickup/interposer DLL
  dxgi.dll                  x86 DXGI bridge pickup/interposer DLL
  NvRemixLauncher32.exe     optional x86 injection helper; the root d3d11.dll/dxgi.dll are the client by default
  artifacts_readme.txt      root package layout notes
  .trex\NvRemixBridge.exe   x64 bridge server when the bridge build produced it
  .trex\bridge_version.txt   exact server BRIDGE_VERSION passed by x86 launcher
  .trex\d3d11.dll           x64 DX11 runtime from this repo
  .trex\dxgi.dll            x64 DXGI runtime from this repo
  .trex\usd\                USD runtime/data when available
  .trex\*.dll               required x64 runtime/dependency DLLs mirrored from _output\x64

No d3d9.dll is staged here.
"@
  Set-Content -LiteralPath (Join-Path $x86Out 'README_X86_DX11_BRIDGE_OUTPUT_V57.txt') -Encoding UTF8 -Value $x86Readme

  $pkgRoot = Join-Path $Root '_packages'
  New-Item -ItemType Directory -Force -Path $pkgRoot | Out-Null
  $zip = Join-Path $pkgRoot 'dxvk-remix-dx11-output-x64-x86-v57.zip'
  if (Test-Path -LiteralPath $zip) { Remove-Item -LiteralPath $zip -Force }
  Add-Type -AssemblyName System.IO.Compression.FileSystem
  [System.IO.Compression.ZipFile]::CreateFromDirectory($outRoot, $zip)

  Log "Output x64: $x64Out"
  Log "Output x86: $x86Out"
  Log "Combined output zip: $zip"
}

function Stage-Package([string]$RuntimeBuild, [hashtable]$BridgeBuilds) {
  $pkg = Join-Path $Root '_packages\rtx-remix-dx11-x86-bridge-getgoing-v57'
  if (Test-Path $pkg) { Remove-Item -LiteralPath $pkg -Recurse -Force }
  $trex = Join-Path $pkg '.trex'
  New-Item -ItemType Directory -Force -Path $trex | Out-Null

  $clientD3D11 = Join-Path $BridgeBuilds.Build32 'd3d11.dll'
  $clientDXGI = Join-Path $BridgeBuilds.Build32 'dxgi.dll'
  $server = Find-FileRecursive $BridgeBuilds.Build64 'NvRemixBridge.exe'
  if (!$server) { Warn "x64 NvRemixBridge.exe was not found under $($BridgeBuilds.Build64); package will include DX11 pickup DLLs and .trex runtime but no bridge server." }
  Copy-Item -LiteralPath $clientD3D11 -Destination (Join-Path $pkg 'd3d11.dll') -Force
  Copy-Item -LiteralPath $clientDXGI -Destination (Join-Path $pkg 'dxgi.dll') -Force
  if ($server) {
    Copy-Item -LiteralPath $server -Destination (Join-Path $trex 'NvRemixBridge.exe') -Force
    Write-BridgeVersionFile -TrexDir $trex -BridgeBuild64 $BridgeBuilds.Build64
  }
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
RTX Remix DX11 x86 Bridge Get-Going Package v57
================================================

Layout:
  d3d11.dll                 x86 DX11 pickup/interposer DLL
  dxgi.dll                  x86 DXGI pickup/interposer DLL
  .trex\NvRemixBridge.exe   x64 bridge server when bridge source build produced it
  .trex\d3d11.dll           x64 DX11 runtime from this repo
  .trex\dxgi.dll            x64 DXGI runtime from this repo
  .trex\usd\                USD runtime/data copied from dependency/build tree when found

No d3d9.dll is included.

v57 fixes:
  - launches NvRemixBridge.exe with required GUID and matching BRIDGE_VERSION arguments
  - writes .trex\bridge_version.txt from bridge server version.h
- wires the x86 root d3d11.dll/dxgi.dll as the client bootstrap to .trex\NvRemixBridge.exe
  - strips UTF-8 BOMs from bridge Meson files before Meson setup
  - writes patched Meson files as UTF-8 without BOM
  - prints Meson/Ninja logs live and tails them on failure
  - continues if Meson exits nonzero after creating build.ninja
  - builds the x86 root d3d11.dll/dxgi.dll directly with HostX64\\x86 cl.exe, avoiding vcvarsall x64_x86 quoting failures
"@
  Set-Content -Path (Join-Path $pkg 'README_DX11_BRIDGE_GETGOING_V57.txt') -Value $readme -Encoding UTF8
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
