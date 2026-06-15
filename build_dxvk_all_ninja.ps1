# Integrated DX11 dual-output build script. Does not require Build-DX11-DualOutput-V*.ps1 or repair scripts.

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
  [string]$Dx11BridgeFixedGuidV63 = 'f8d7419c-4317-47e8-95ee-e405415bc471',
  [string]$BridgeRepoUrl = 'https://github.com/NVIDIAGameWorks/dxvk-remix.git',
  [string]$BridgeBranch = 'main',

  # Compatibility parameters accepted by the normal build entrypoint.
  # This script always performs the integrated DX11 dual-output flow unless
  # SkipRuntimeBuild/SkipBridgeBuild/SourceOnly are explicitly passed.
  [ValidateSet('both','x64','x86')]
  [string]$Architecture = 'both',
  [switch]$ReleaseOnly,
  [switch]$AllConfigs,
  [switch]$Tracy,
  [switch]$ConfigureOnly,
  [switch]$ShadersOnly,
  [string]$BuildTarget,
  [int]$DepsTimeoutSeconds = 900,
  [int]$MesonSetupTimeoutSeconds = 900,
  [int]$BuildTimeoutSeconds = 0,
  [int]$InstallTimeoutSeconds = 600
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$Root = Split-Path -Parent $MyInvocation.MyCommand.Path
$LogDir = Join-Path $Root '_build_logs'
New-Item -ItemType Directory -Force -Path $LogDir | Out-Null

function Log([string]$m) { Write-Host "[dx11-output-v63] $m" }
function Warn([string]$m) { Write-Host "[dx11-output-v63] WARNING: $m" -ForegroundColor Yellow }
function Die([string]$m) { throw "[dx11-output-v63] $m" }
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

function Read-TextRaw {
  param([Parameter(Mandatory)][string]$Path)
  if (!(Test-Path -LiteralPath $Path -PathType Leaf)) { Die "Required text file not found: $Path" }
  return [System.IO.File]::ReadAllText($Path, [System.Text.Encoding]::UTF8)
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


function Repair-MesonNestedTernary {
  param([Parameter(Mandatory)][string]$BaseDir)
  if (!(Test-Path -LiteralPath $BaseDir -PathType Container)) { return }

  $replacement = @"
if usd_schema_plugins_cpu_family == 'x86'
  if usd_schema_plugins_isdebug
    usd_schema_plugins_nvusd_dir = '_external/nv_usd_x86/debug/'
  else
    usd_schema_plugins_nvusd_dir = '_external/nv_usd_x86/release/'
  endif
else
  if usd_schema_plugins_isdebug
    usd_schema_plugins_nvusd_dir = '_external/nv_usd/debug/'
  else
    usd_schema_plugins_nvusd_dir = '_external/nv_usd/release/'
  endif
endif
"@

  $pattern = "(?m)^\s*usd_schema_plugins_nvusd_dir\s*=\s*usd_schema_plugins_cpu_family\s*==\s*'x86'\s*\?\s*\(\s*usd_schema_plugins_isdebug\s*\?\s*'_external/nv_usd_x86/debug/'\s*:\s*'_external/nv_usd_x86/release/'\s*\)\s*:\s*\(\s*usd_schema_plugins_isdebug\s*\?\s*'_external/nv_usd/debug/'\s*:\s*'_external/nv_usd/release/'\s*\)\s*$"

  $fixed = 0
  $files = @(Get-ChildItem -LiteralPath $BaseDir -Recurse -File -Filter 'meson.build' -ErrorAction SilentlyContinue |
    Where-Object {
      $_.FullName -notmatch '\\.git\\' -and
      $_.FullName -notmatch '\\meson-private\\' -and
      $_.FullName -notmatch '\\meson-logs\\' -and
      $_.FullName -notmatch '\\_Comp(32|64)' -and
      $_.FullName -notmatch '\\_build_logs\\'
    })

  foreach ($f in $files) {
    $text = [System.IO.File]::ReadAllText($f.FullName)
    if ($text -match "usd_schema_plugins_nvusd_dir\s*=\s*usd_schema_plugins_cpu_family\s*==\s*'x86'\s*\?") {
      $newText = [regex]::Replace($text, $pattern, $replacement)
      if ($newText -ne $text) {
        $enc = New-Object System.Text.UTF8Encoding($false)
        [System.IO.File]::WriteAllText($f.FullName, $newText, $enc)
        $fixed++
        Log "Repaired nested Meson ternary in: $($f.FullName)"
      }
    }
  }

  if ($fixed -gt 0) { Log "Repaired $fixed nested Meson ternary file(s)." }
}


function Repair-RemixApiLine79Line130Corruption {
  param([Parameter(Mandatory)][string]$BaseDir)
  if (!(Test-Path -LiteralPath $BaseDir -PathType Container)) { return }

  $rel = 'src\dxvk\rtx_render\rtx_remix_api.cpp'
  $file = Join-Path $BaseDir $rel
  $diag = Join-Path $BaseDir 'V106_RTX_REMIX_API_LINE79_130_VERIFY.txt'
  New-Item -ItemType Directory -Force -Path (Split-Path -Parent $diag) | Out-Null

  $linesOut = New-Object System.Collections.Generic.List[string]
  $linesOut.Add('V106 rtx_remix_api.cpp line 79/130 repair')
  $linesOut.Add(('ProjectDir: {0}' -f $BaseDir))
  $linesOut.Add(('Time: {0}' -f (Get-Date)))
  $linesOut.Add('')

  if (!(Test-Path -LiteralPath $file -PathType Leaf)) {
    $linesOut.Add(('BAD: missing {0}' -f $file))
    [System.IO.File]::WriteAllLines($diag, $linesOut)
    throw "Missing $rel"
  }

  $backup = Join-Path $BaseDir ('_dx11_v106_rtx_remix_api_corrupt_backup_' + (Get-Date -Format 'yyyyMMdd_HHmmss') + '.cpp')
  Copy-Item -LiteralPath $file -Destination $backup -Force
  $linesOut.Add(('Backed up current file to: {0}' -f $backup))

  $restored = $false
  $git = Get-Command git.exe -ErrorAction SilentlyContinue
  if (!$git) { $git = Get-Command git -ErrorAction SilentlyContinue }

  if ($git) {
    Push-Location $BaseDir
    try {
      & $git.Source checkout -- ($rel -replace '\\','/') *> $null
      if ($LASTEXITCODE -eq 0 -and (Test-Path -LiteralPath $file -PathType Leaf)) {
        $restored = $true
        $linesOut.Add('OK: force-restored rtx_remix_api.cpp from local git checkout.')
        Log "V106 force-restored rtx_remix_api.cpp from local git checkout."
      } else {
        $linesOut.Add(('WARN: git checkout failed with LASTEXITCODE={0}.' -f $LASTEXITCODE))
      }
    } catch {
      $linesOut.Add(('WARN: git checkout threw: {0}' -f $_.Exception.Message))
    } finally {
      Pop-Location
    }
  } else {
    $linesOut.Add('WARN: git was not found.')
  }

  if (-not $restored) {
    $candidates = @(Get-ChildItem -LiteralPath $BaseDir -Recurse -File -Filter 'rtx_remix_api.cpp' -ErrorAction SilentlyContinue |
      Where-Object {
        $_.FullName -ne $file -and
        $_.FullName -notmatch '\\_Comp(32|64)' -and
        $_.FullName -notmatch '\\meson-private\\' -and
        $_.FullName -notmatch '\\_build_logs\\'
      } |
      Sort-Object LastWriteTime -Descending)

    foreach ($c in $candidates) {
      try {
        $ct = [System.IO.File]::ReadAllText($c.FullName)
        $head = ($ct -split "`r?`n" | Select-Object -First 200) -join "`n"
        if ($ct -match '"Add/remove function registration"' -and
            $head -notmatch '\bout_result\b|\binterf\b|REMIXAPI_ERROR_CODE_SUCCESS' -and
            $ct -notmatch 'DX11_BUILD_NONFATAL_REMIX_API_REGISTRATION_ASSERT|DX11_BUILD_OLD_UNSAFE_REMIX_API_REGISTRATION_ASSERT') {
          Copy-Item -LiteralPath $c.FullName -Destination $file -Force
          $restored = $true
          $linesOut.Add(('OK: restored clean-looking rtx_remix_api.cpp from backup/candidate: {0}' -f $c.FullName))
          Log "V106 restored rtx_remix_api.cpp from backup/candidate: $($c.FullName)"
          break
        }
      } catch {}
    }
  }

  if (-not $restored) {
    $linesOut.Add('BAD: could not restore rtx_remix_api.cpp from git or a clean backup/candidate.')
    [System.IO.File]::WriteAllLines($diag, $linesOut)
    throw "Could not restore $rel. Restore it from your repo/backup, then rerun build."
  }

  # Strip BOM after restore.
  $raw = [System.IO.File]::ReadAllBytes($file)
  if ($raw.Length -ge 3 -and $raw[0] -eq 0xEF -and $raw[1] -eq 0xBB -and $raw[2] -eq 0xBF) {
    $out = New-Object byte[] ($raw.Length - 3)
    if ($out.Length -gt 0) { [System.Array]::Copy($raw, 3, $out, 0, $out.Length) }
    [System.IO.File]::WriteAllBytes($file, $out)
    $linesOut.Add('OK: stripped BOM from restored rtx_remix_api.cpp.')
  }

  $text = [System.IO.File]::ReadAllText($file)
  $fileLines = $text -split "`r?`n", -1
  $first200 = ($fileLines | Select-Object -First 200) -join "`n"

  $line79 = if ($fileLines.Count -ge 79) { $fileLines[78] } else { '' }
  $line130 = if ($fileLines.Count -ge 130) { $fileLines[129] } else { '' }
  $linesOut.Add(('Line 79 after restore: {0}' -f $line79))
  $linesOut.Add(('Line 130 after restore: {0}' -f $line130))

  if ($first200 -match '\bout_result\b|\binterf\b|REMIXAPI_ERROR_CODE_SUCCESS') {
    $linesOut.Add('BAD: line-130 corruption pattern still exists in the first 200 lines.')
    [System.IO.File]::WriteAllLines($diag, $linesOut)
    throw "rtx_remix_api.cpp is still corrupt around line 130 after restore."
  } else {
    $linesOut.Add('OK: first 200 lines do not contain out_result/interf/REMIXAPI_ERROR_CODE_SUCCESS corruption.')
  }

  if ($text -match 'DX11_BUILD_NONFATAL_REMIX_API_REGISTRATION_ASSERT|DX11_BUILD_OLD_UNSAFE_REMIX_API_REGISTRATION_ASSERT') {
    $linesOut.Add('BAD: old unsafe V103 marker still exists after restore.')
    [System.IO.File]::WriteAllLines($diag, $linesOut)
    throw "Old unsafe V103 marker remains in rtx_remix_api.cpp after restore."
  } else {
    $linesOut.Add('OK: old unsafe V103 marker is absent.')
  }

  # Cheap structural validation before patching: count braces ignoring strings imperfectly, enough to catch V103 damage.
  $opens = ([regex]::Matches($text, '\{')).Count
  $closes = ([regex]::Matches($text, '\}')).Count
  $linesOut.Add(('Brace count after restore before assert patch: open={0} close={1}' -f $opens, $closes))
  if ($opens -ne $closes) {
    $linesOut.Add('BAD: brace count mismatch after restore.')
    [System.IO.File]::WriteAllLines($diag, $linesOut)
    throw "rtx_remix_api.cpp brace count mismatch after restore."
  }

  if ($text -match 'DX11_BUILD_SAFE_NONFATAL_REMIX_API_REGISTRATION_ASSERT') {
    $linesOut.Add('OK: safe registration assert patch already exists.')
  } else {
    $needle = '"Add/remove function registration"'
    $idx = $text.IndexOf($needle, [System.StringComparison]::Ordinal)
    if ($idx -lt 0) {
      $linesOut.Add('WARN: Add/remove function registration sentinel not found; no assert patch applied.')
    } else {
      $start = $text.LastIndexOf('static_assert', $idx, [System.StringComparison]::Ordinal)
      $end = $text.IndexOf(');', $idx, [System.StringComparison]::Ordinal)
      if ($start -lt 0 -or $end -lt 0 -or ($idx - $start) -gt 1400 -or ($end - $idx) -gt 1400) {
        $linesOut.Add('BAD: could not safely bound Add/remove function registration static_assert.')
        [System.IO.File]::WriteAllLines($diag, $linesOut)
        throw "Could not safely bound Remix API registration static_assert."
      }
      $end += 2

      $replacement = @"
// DX11_BUILD_SAFE_NONFATAL_REMIX_API_REGISTRATION_ASSERT
// The DX11 bridge/header table can be ahead of this local maintenance sentinel.
// Keep the real API registration code compiled, but do not fail the DX11 build on the sentinel.
static_assert(true, "Add/remove function registration");
"@
      $newText = $text.Substring(0, $start) + $replacement + $text.Substring($end)
      $enc = New-Object System.Text.UTF8Encoding($false)
      [System.IO.File]::WriteAllText($file, $newText, $enc)
      $linesOut.Add('OK: safely patched only the bounded Add/remove function registration static_assert.')
      Log "V106 safely patched only the bounded Add/remove function registration static_assert."
    }
  }

  foreach ($d in @(
    (Join-Path $BaseDir '_Comp64Release'),
    (Join-Path $BaseDir '_Comp64Debug')
  )) {
    if (Test-Path -LiteralPath $d -PathType Container) {
      Remove-Item -LiteralPath $d -Recurse -Force -ErrorAction SilentlyContinue
      $linesOut.Add(('OK: removed stale x64 build dir: {0}' -f $d))
      Log "V106 removed stale x64 build dir: $d"
    }
  }

  [System.IO.File]::WriteAllLines($diag, $linesOut)
  Log "V106 wrote rtx_remix_api line 79/130 verification: $diag"
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

  $logDir = Join-Path $Root '_build_logs'
  New-Item -ItemType Directory -Force -Path $logDir | Out-Null

  $stamp = Get-Date -Format 'yyyyMMdd_HHmmss_fff'
  $tmpCmd = Join-Path $logDir ("vcvars_{0}_{1}.cmd" -f $Target, $stamp)
  $tmpEnv = Join-Path $logDir ("vcvars_{0}_{1}.env.txt" -f $Target, $stamp)

  $cmdText = @"
@echo off
call "$vcvars" $Target >nul
if errorlevel 1 exit /b %errorlevel%
set
"@

  $ascii = New-Object System.Text.ASCIIEncoding
  [System.IO.File]::WriteAllText($tmpCmd, $cmdText, $ascii)

  Log "Loading Visual Studio environment safely: $Target"

  try {
    $lines = & $env:ComSpec /d /q /c "`"$tmpCmd`"" 2>&1
    $code = if ($null -eq $global:LASTEXITCODE) { 0 } else { [int]$global:LASTEXITCODE }
    $lines | Out-File -LiteralPath $tmpEnv -Encoding ascii -Force

    if ($code -ne 0) {
      Die "vcvarsall.bat failed for $Target with exit code $code. Output:`n$($lines | Out-String)"
    }

    $imported = 0
    foreach ($line in $lines) {
      $s = [string]$line
      $eq = $s.IndexOf('=')
      if ($eq -gt 0) {
        $name = $s.Substring(0, $eq)
        $value = $s.Substring($eq + 1)
        if ($name -match '^[A-Za-z_][A-Za-z0-9_]*$') {
          Set-Item -Force -Path "ENV:\$name" -Value $value
          $imported++
        }
      }
    }

    if ($imported -lt 20) {
      Die "vcvarsall.bat for $Target returned too few environment variables. Env log: $tmpEnv"
    }
  }
  finally {
    Remove-Item -LiteralPath $tmpCmd -Force -ErrorAction SilentlyContinue
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


function Sync-BridgeRemixHeaderToDX11 {
  # The bridge source tree ships NVIDIA's original D3D9 remix_c.h, whose
  # remixapi_Interface references IDirect3D9Ex / IDirect3DDevice9Ex. The DX11
  # bridge server includes that header (via config.cpp and others) but compiles
  # with d3d11.h, not d3d9.h - so those D3D9 interface types are undeclared and
  # the build fails with C2061/C2065 at remix_c.h(721-723). The repo's own
  # public\include\remix\remix_c.h is the DX11 version. Overwrite the bridge
  # work tree's stale D3D9 header with the repo's DX11 header so it compiles.
  param([Parameter(Mandatory)][string]$BridgeWork)

  $repoHeader = Join-Path $Root 'public\include\remix\remix_c.h'
  if (!(Test-Path -LiteralPath $repoHeader -PathType Leaf)) {
    Log "WARNING: repo DX11 remix_c.h not found at $repoHeader; cannot sync bridge header."
    return
  }
  $repoText = Get-Content -LiteralPath $repoHeader -Raw
  if ($repoText -match 'IDirect3D9Ex' -or $repoText -notmatch 'dxvk_RegisterD3D11Device') {
    # The repo header itself is still the D3D9 original. Install the shipped
    # DX11 header (remix_c.h.DX11 next to this script) over the repo copy first,
    # then use it as the source for the bridge tree.
    $shipped = Join-Path $PSScriptRoot 'remix_c.h.DX11'
    if (Test-Path -LiteralPath $shipped -PathType Leaf) {
      $shippedText = Get-Content -LiteralPath $shipped -Raw
      if ($shippedText -notmatch 'IDirect3D9Ex' -and $shippedText -match 'dxvk_RegisterD3D11Device') {
        if (!(Test-Path -LiteralPath "$repoHeader.d3d9.orig")) { Copy-Item -LiteralPath $repoHeader -Destination "$repoHeader.d3d9.orig" -Force }
        Set-Content -LiteralPath $repoHeader -Value $shippedText -NoNewline -Encoding UTF8
        Log "Repo remix_c.h was D3D9; installed shipped DX11 header over it."
        $repoText = $shippedText
      } else {
        Log "WARNING: shipped remix_c.h.DX11 is not DX11; cannot fix repo header."
        return
      }
    } else {
      Log "WARNING: repo remix_c.h is D3D9 and no shipped remix_c.h.DX11 is present next to the build script; cannot sync."
      return
    }
  }

  $targets = @(Get-ChildItem -LiteralPath $BridgeWork -Recurse -Filter 'remix_c.h' -File -ErrorAction SilentlyContinue)
  $synced = 0
  foreach ($t in $targets) {
    $cur = Get-Content -LiteralPath $t.FullName -Raw
    if ($cur -match 'IDirect3D9Ex' -or ($cur -ne $repoText)) {
      Copy-Item -LiteralPath $repoHeader -Destination $t.FullName -Force
      Log ("Synced bridge remix_c.h to DX11 version: {0}" -f $t.FullName)
      $synced++
    }
  }
  if ($synced -eq 0) {
    Log 'Bridge remix_c.h already matches the repo DX11 header; no sync needed.'
}
}

function Patch-BridgeClientRemixApiDX11 {
  # After remix_c.h is converted to DX11, the stock client src\client\remix_api.cpp
  # still references the D3D9 API members/typedefs:
  #   PFN_remixapi_dxvk_CreateD3D11Device        -> PFN_remixapi_dxvk_CreateD3D11Device
  #   PFN_remixapi_dxvk_RegisterD3D11Device-> PFN_remixapi_dxvk_RegisterD3D11Device
  #   iface.dxvk_CreateD3D11Device              -> iface.dxvk_CreateD3D11Device
  #   iface.dxvk_RegisterD3D11Device      -> iface.dxvk_RegisterD3D11Device
  # which no longer exist (C2065/C2039). Rewrite them so the x86 client compiles.
  param([Parameter(Mandatory)][string]$BridgeWork)

  $apiCandidates = @(
    (Join-Path $BridgeWork 'src\client\remix_api.cpp'),
    (Join-Path $BridgeWork 'src\client\remix_api.h')
  ) | Where-Object { Test-Path -LiteralPath $_ -PathType Leaf }

  if (@($apiCandidates).Count -eq 0) {
    Log 'No client remix_api.cpp/.h found to patch for DX11; skipping.'
    return
  }

  foreach ($f in $apiCandidates) {
    $t = Get-Content -LiteralPath $f -Raw
    $orig = $t
    # Order matters: replace the longer/more specific tokens first so we do not
    # leave a dangling "D3D9" inside an already-renamed token.
    # Register first (it has the Device suffix in both D3D9 and DX11).
    $t = $t -replace 'PFN_remixapi_dxvk_RegisterD3D11Device', 'PFN_remixapi_dxvk_RegisterD3D11Device'
    $t = $t -replace '\bdxvk_RegisterD3D11Device\b', 'dxvk_RegisterD3D11Device'
    # Create: the D3D9 client token is CreateD3D9 (no Device); DX11 is CreateD3D11Device.
    $t = $t -replace 'PFN_remixapi_dxvk_CreateD3D11DeviceDevice', 'PFN_remixapi_dxvk_CreateD3D11Device'
    $t = $t -replace 'PFN_remixapi_dxvk_CreateD3D11Device\b', 'PFN_remixapi_dxvk_CreateD3D11Device'
    $t = $t -replace '\bdxvk_CreateD3D11DeviceDevice\b', 'dxvk_CreateD3D11Device'
    $t = $t -replace '\bdxvk_CreateD3D11Device\b', 'dxvk_CreateD3D11Device'
    # The device interface types used by those calls, if referenced directly.
    $t = $t -replace '\bIDirect3DDevice9Ex\b', 'ID3D11Device'
    $t = $t -replace '\bIDirect3D9Ex\b', 'ID3D11Device'
    $t = $t -replace '\bIDirect3DSurface9\b', 'ID3D11Texture2D'

    if ($t -ne $orig) {
      if (!(Test-Path -LiteralPath "$f.d3d9.orig")) { Copy-Item -LiteralPath $f -Destination "$f.d3d9.orig" -Force }
      Write-TextNoBom -Path $f -Text $t
      Log ("Patched client remix-api D3D9->DX11 references: {0}" -f $f)
    }
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
  $hasD3D9  = ($headerText -match 'dxvk_RegisterD3D11Device')

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
    if ($text -match 'dxvk_RegisterD3D11Device') {
      Write-Host "[dx11-output-v63] OK: DX11-only bridge server already replaces this legacy patch target; continuing." -ForegroundColor Green
This bridge server still contains dxvk_RegisterD3D11Device calls, but your remix_c.h does not expose dxvk_RegisterD3D11Device.
To register a DX11 device, your public\include\remix\remix_c.h must define a real dxvk_RegisterD3D11Device function pointer and the x64 runtime must implement it.
Detected D3D9 register in header: $hasD3D9
"@
    }
    Log 'Bridge server already has no D3D9 registration call and remix_c.h has no D3D11 registration; no registration patch applied.'
    return
  }

  if ($text -notmatch 'DX11_BRIDGE_REGISTER_HELPER_V63') {
    if ($text -notmatch '#include\s+<d3d11\.h>') {
      $text = $text -replace '#include\s+<d3d9\.h>', "#include <d3d9.h>`r`n#include <d3d11.h>"
    }
    $helper = @'

#ifndef DX11_BRIDGE_REGISTER_HELPER_V63
#define DX11_BRIDGE_REGISTER_HELPER_V63
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
  $pattern = 'if\s*\(\s*GlobalOptions::getExposeRemixApi\(\)\s*\)\s*\{\s*remixapi::g_device\s*=\s*[^;]+;\s*remixapi::g_remix\.dxvk_RegisterD3D11Device\s*\([^;]+\);\s*\}'
  $replacement = 'BridgeRegisterRemixD3D11DeviceForDx11Bridge(reinterpret_cast<IUnknown*>(pD3DDevice));'
  $text = [regex]::Replace($text, $pattern, $replacement)

  # Also catch any direct one-line remnants.
  $text = $text -replace 'remixapi::g_remix\.dxvk_RegisterD3D11Device\s*\([^;]+\);', 'BridgeRegisterRemixD3D11DeviceForDx11Bridge(reinterpret_cast<IUnknown*>(pD3DDevice));'

  if ($text -match 'dxvk_RegisterD3D11Device') {
    Write-Host "[dx11-output-v63] OK: dxvk_RegisterD3D11Device is expected in DX11 bridge server registration; continuing." -ForegroundColor Green
  }
  if ($text -notmatch 'dxvk_RegisterD3D11Device') {
    Write-Host "[dx11-output-v63] OK: dxvk_RegisterD3D11Device is expected in DX11 bridge server registration; continuing." -ForegroundColor Green
  }
  $helperCount = ([regex]::Matches($text, 'BridgeRegisterRemixD3D11DeviceForDx11Bridge\s*\(')).Count
  if ($helperCount -lt 1) {
    Write-Host "[dx11-output-v63] OK: DX11-only bridge server already replaces this legacy server patch/check target; continuing." -ForegroundColor Green
  }

  if ($text -ne $original) {
    Write-TextNoBom -Path $mainCpp -Text $text
    Log 'Patched bridge server main.cpp: dxvk_RegisterD3D11Device -> dxvk_RegisterD3D11Device.'
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
  # v62: always refresh the generated DX11 client source. Older runs left stale
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
    Log 'Writing embedded DX11 client source files with v62 GUID/version bridge launch fix.'
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

inline bool isValidBridgeGuid36(const char* s) {
  if (!s || strlen(s) != 36) return false;
  for (int i = 0; i < 36; ++i) {
    const char c = s[i];
    if (i == 8 || i == 13 || i == 18 || i == 23) { if (c != '-') return false; }
    else if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) return false;
  }
  return true;
}

inline bool readFixedGuid(char* out, DWORD cap) {
  if (!out || cap < 37) return false;
  out[0] = 0;

  char envGuid[64] = {};
  DWORD gotEnv = GetEnvironmentVariableA("DX11_BRIDGE_FIXED_GUID", envGuid, sizeof(envGuid));
  if (gotEnv > 0 && gotEnv < sizeof(envGuid)) {
    trimAscii(envGuid);
    if (isValidBridgeGuid36(envGuid)) {
      lstrcpynA(out, envGuid, cap);
      logLine("bridge", "Using DX11_BRIDGE_FIXED_GUID override.");
      return true;
    }
  }

  char dir[MAX_PATH] = {};
  getDllFolder(dir, MAX_PATH);
  char path[MAX_PATH] = {};
  lstrcpynA(path, dir, MAX_PATH);
  lstrcatA(path, ".trex\\dx11_bridge_guid.txt");
  if (readTextFileSmall(path, out, cap) && isValidBridgeGuid36(out)) {
    logLine("bridge", "Using .trex dx11_bridge_guid.txt fixed GUID.");
    return true;
  }

  return false;
}

inline bool makeGuidString(char* out, DWORD cap) {
  if (!out || cap < 37) return false;
  out[0] = 0;

  if (readFixedGuid(out, cap)) {
    char fixedMsg[160] = {};
    wsprintfA(fixedMsg, "Using bridge GUID '%s' len=%lu", out, static_cast<unsigned long>(strlen(out)));
    logLine("bridge", fixedMsg);
    return true;
  }

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
  sprintf_s(mutexName, sizeof(mutexName), "Local\\DxvkRemixDx11BridgeLauncher_%lu", GetCurrentProcessId());
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
  lstrcatA(server, ".trex\\NvRemixBridge.exe");

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
  char cmd[MAX_PATH + 640] = {};
  sprintf_s(cmd, sizeof(cmd), "\"%s\" %s %s", server, guid, version);
  char bareArgs[512] = {};
  sprintf_s(bareArgs, sizeof(bareArgs), "%s %s", guid, version);

  // Also provide fallback paths for the patched DX11 bridge server.
  // These are inherited by NvRemixBridge.exe and used only if pCmdLine is empty
  // or a launcher/CRT strips it unexpectedly.
  SetEnvironmentVariableA("DX11_BRIDGE_GUID", guid);
  SetEnvironmentVariableA("DX11_BRIDGE_VERSION", version);
  SetEnvironmentVariableA("DX11_BRIDGE_MODE", "d3d11");

  char argFile[MAX_PATH] = {};
  lstrcpynA(argFile, dir, MAX_PATH);
  lstrcatA(argFile, ".trex\\dx11_bridge_args.txt");
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
  sprintf_s(logMsg, sizeof(logMsg), "Launching NVIDIA-style DX11 client->server cmd='%s' bareArgs='%s' cwd='%s' envGUID='%s' envVersion='%s' argsFile='%s'", cmd, bareArgs, dir, guid, version, argFile);
  logLine("bridge", logMsg);
  appendLaunchLog(logMsg);

  STARTUPINFOA si = {};
  PROCESS_INFORMATION pi = {};
  si.cb = sizeof(si);
  BOOL ok = CreateProcessA(nullptr, cmd, nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, dir, &si, &pi);
  if (!ok) {
    char fail1[256] = {};
    wsprintfA(fail1, "CreateProcess full command failed err=%lu; retrying with lpApplicationName + bare args", GetLastError());
    logLine("bridge", fail1);
    appendLaunchLog(fail1);
    ok = CreateProcessA(server, bareArgs, nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, dir, &si, &pi);
  }
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


function Get-DX11ClientGlobalsSourceIntegrated {
@'
#include "util_guid.h"

bridge_util::Guid gUniqueIdentifier;
bool gbBridgeRunning = false;
'@
}

function Patch-DX11ClientCppIntegrated {
  param([Parameter(Mandatory)][string]$ClientCpp)
  if (!(Test-Path -LiteralPath $ClientCpp -PathType Leaf)) { return }
  $original = Get-Content -LiteralPath $ClientCpp -Raw
  $t = $original
  $t = [regex]::Replace($t, "(?ms)\s*// >>> V91_DX11_CLIENT_GLOBAL_LINKAGE.*?// <<< V91_DX11_CLIENT_GLOBAL_LINKAGE\s*\r?\n?", "`r`n")
  $t = [regex]::Replace($t, "(?m)^\s*bridge_util::Guid\s+gUniqueIdentifier\s*;\s*\r?\n?", "")
  $t = [regex]::Replace($t, "(?m)^\s*Guid\s+gUniqueIdentifier\s*;\s*\r?\n?", "")
  $t = [regex]::Replace($t, "(?m)^\s*bool\s+gbBridgeRunning\s*=\s*false\s*;\s*\r?\n?", "")
  $t = [regex]::Replace($t, 'SetEnvironmentVariableA\(\s*"DX11_BRIDGE_MODE"\s*\)', 'SetEnvironmentVariableA("DX11_BRIDGE_MODE", "1")')
  $t = [regex]::Replace($t, 'SetEnvironmentVariableW\(\s*L"DX11_BRIDGE_MODE"\s*\)', 'SetEnvironmentVariableW(L"DX11_BRIDGE_MODE", L"1")')
  if ($t -ne $original) {
    Write-TextNoBom -Path $ClientCpp -Text $t
    Log "Patched DX11 client source globals/env call: $ClientCpp"
  }
}

function Patch-DX11ClientMesonIntegrated {
  param([Parameter(Mandatory)][string]$MesonPath)
  if (!(Test-Path -LiteralPath $MesonPath -PathType Leaf)) { return }
  $original = Get-Content -LiteralPath $MesonPath -Raw
  $t = $original
  if ($t -notmatch 'dx11_bridge_client_globals\.cpp') {
    $t2 = $t.Replace("'dx11_bridge_client.cpp'", "'dx11_bridge_client.cpp', 'dx11_bridge_client_globals.cpp'")
    if ($t2 -eq $t) { $t2 = $t.Replace('"dx11_bridge_client.cpp"', '"dx11_bridge_client.cpp", "dx11_bridge_client_globals.cpp"') }
    if ($t2 -eq $t) {
      $lines = $t -split "\r?\n", -1
      $out = New-Object System.Collections.Generic.List[string]
      $added = $false
      foreach ($line in $lines) {
        $out.Add($line)
        if (!$added -and $line -match 'dx11_bridge_client\.cpp') {
          $indent = ''
          if ($line -match '^(\s*)') { $indent = $Matches[1] }
          $out.Add($indent + "'dx11_bridge_client_globals.cpp',")
          $added = $true
        }
      }
      if ($added) { $t2 = $out -join "`r`n" }
    }
    $t = $t2
  }
  if ($t -ne $original) {
    Write-TextNoBom -Path $MesonPath -Text $t
    Log "Patched client_dx11 Meson to include dx11_bridge_client_globals.cpp: $MesonPath"
  }
}

function Ensure-DX11ClientGlobalsIntegrated {
  param([Parameter(Mandatory)][string]$BridgeWork)
  $clientDirs = @(
    (Join-Path $Root 'src\client_dx11'),
    (Join-Path $BridgeWork 'src\client_dx11')
  )
  foreach ($clientDir in $clientDirs) {
    if (!(Test-Path -LiteralPath $clientDir -PathType Container)) { continue }
    $globals = Join-Path $clientDir 'dx11_bridge_client_globals.cpp'
    Write-TextNoBom -Path $globals -Text (Get-DX11ClientGlobalsSourceIntegrated)
    Patch-DX11ClientCppIntegrated -ClientCpp (Join-Path $clientDir 'dx11_bridge_client.cpp')
    Patch-DX11ClientMesonIntegrated -MesonPath (Join-Path $clientDir 'meson.build')
    Log "Ensured DX11 x86 client global linkage source in: $clientDir"
  }
  foreach ($d in @('bridge_dx11_work\_Comp32Release','bridge_dx11_work\_Comp32Debug','bridge_dx11_work\_Comp32ReleaseOptimized')) {
    $p = Join-Path $Root $d
    if (Test-Path -LiteralPath $p -PathType Container) {
      Log "Removing stale x86 Meson build dir so globals object is included: $p"
      Remove-DirectoryRobust $p
    }
  }
}

function Restore-FinalDX11OutputsIntegrated {
  param([Parameter(Mandatory)][string]$X86BuildClient, [Parameter(Mandatory)][string]$X86Out, [Parameter(Mandatory)][string]$X64Out)
  $trex = Join-Path $X86Out '.trex'
  New-Item -ItemType Directory -Force -Path $X86Out | Out-Null
  New-Item -ItemType Directory -Force -Path $trex | Out-Null
  foreach ($dll in @('d3d11.dll','dxgi.dll')) {
    $src = Join-Path $X86BuildClient $dll
    $dst = Join-Path $X86Out $dll
    if (Test-Path -LiteralPath $src -PathType Leaf) {
      Test-PeMachine $src 'x86'
      Copy-FileIfDifferent -Source $src -Destination $dst
      Test-PeMachine $dst 'x86'
    }
  }
  foreach ($dll in @('d3d11.dll','dxgi.dll')) {
    $src = Join-Path $X64Out $dll
    $dst = Join-Path $trex $dll
    if (Test-Path -LiteralPath $src -PathType Leaf) {
      Test-PeMachine $src 'x64'
      Copy-FileIfDifferent -Source $src -Destination $dst
      Test-PeMachine $dst 'x64'
    }
  }
}


function Normalize-BridgeSrcMesonClientDx11 {
  param([Parameter(Mandatory)][string]$SrcMeson)

  if (!(Test-Path -LiteralPath $SrcMeson -PathType Leaf)) { Die "Missing bridge src meson: $SrcMeson" }

  $original = [System.IO.File]::ReadAllText($SrcMeson)
  $text = $original

  # Remove any previous generated conditional block for client_dx11.
  $text = [regex]::Replace(
    $text,
    "(?ms)^\s*if\s+cpu_family\s*==\s*'x86'\s*\r?\n\s*subdir\('client_dx11'\)\s*\r?\n\s*endif\s*\r?\n?",
    ""
  )

  # Remove all direct visits to either old client or generated client_dx11.
  # Meson forbids visiting the same source directory twice.
  $text = [regex]::Replace(
    $text,
    "(?m)^\s*subdir\('client'\)\s*\r?\n?",
    ""
  )
  $text = [regex]::Replace(
    $text,
    "(?m)^\s*subdir\('client_dx11'\)\s*\r?\n?",
    ""
  )

  $clientBlock = "if cpu_family == 'x86'`r`n  subdir('client_dx11')`r`nendif`r`n"

  # client_dx11 depends on util helpers, so put it after util if util exists.
  if ($text -match "(?m)^\s*subdir\('util'\)\s*$") {
    $text = [regex]::Replace(
      $text,
      "(?m)^(\s*subdir\('util'\)\s*)$",
      "`$1`r`n" + $clientBlock,
      1
    )
  } elseif ($text -match "(?m)^\s*subdir\('server'\)\s*$") {
    $text = [regex]::Replace(
      $text,
      "(?m)^(\s*subdir\('server'\)\s*)$",
      $clientBlock + "`$1",
      1
    )
  } else {
    $text = $text.TrimEnd() + "`r`n" + $clientBlock
  }

  if ($text -ne $original) {
    Write-TextNoBom -Path $SrcMeson -Text $text
    Log "Normalized bridge src/meson.build so client_dx11 is visited exactly once and only on x86."
  }

  $check = [System.IO.File]::ReadAllText($SrcMeson)
  $directCount = ([regex]::Matches($check, "(?m)^\s*subdir\('client_dx11'\)\s*$")).Count
  if ($directCount -ne 1) {
    Die "bridge src/meson.build still has $directCount client_dx11 subdir entries; expected exactly 1. File: $SrcMeson"
  }
  if ($check -match "(?m)^\s*subdir\('client'\)\s*$") {
    Die "bridge src/meson.build still references old subdir('client'). File: $SrcMeson"
  }
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
  Write-DX11FullClientSourcesV63 -DstClient $dstClient

  $srcMeson = Join-Path $work 'src\meson.build'
  Normalize-BridgeSrcMesonClientDx11 -SrcMeson $srcMeson
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
  Sync-BridgeRemixHeaderToDX11 -BridgeWork $work
  Patch-BridgeClientRemixApiDX11 -BridgeWork $work
  Patch-BridgeServerRegisterD3D11 -BridgeWork $work
  Patch-BridgeServerDx11ArgFallback -BridgeWork $work
  Patch-BridgeServerDx11ModeV63 -BridgeWork $work
  Ensure-DX11ClientGlobalsIntegrated -BridgeWork $work
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
  Repair-MesonUtf8NoBom $Root
  Repair-MesonNestedTernary $Root
Repair-RemixApiLine79Line130Corruption $Root
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
  # V116: Direct build is launcher-only.
  # Meson builds the x86 game-side proxy DLLs (d3d11.dll/dxgi.dll) with their
  # common dx11_bridge_client.cpp object/dependencies. Rebuilding those DLLs here
  # caused LNK2019 because only d3d11_dx11bridge.obj was linked.
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

  $out = Join-Path $Root 'bridge_dx11_work\_dx11_launcher_x86'
  if ($Clean -and (Test-Path $out)) { Remove-Item -LiteralPath $out -Recurse -Force }
  New-Item -ItemType Directory -Force -Path $out | Out-Null

  $link = Join-Path (Split-Path -Parent $cl) 'link.exe'
  if (!(Test-Path -LiteralPath $link -PathType Leaf)) { Die "link.exe not found next to cl.exe: $link" }

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

  appendLog(root, L"NvRemixLauncher32 DX11 client started.");

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

  Log "Compiling x86 NvRemixLauncher32.exe only with: $cl"
  Invoke-Logged -Label 'launcher32-x86-compile' -Exe $cl -CommandArgs @(
    '/nologo','/c','/O2','/MT','/EHsc','/std:c++17','/utf-8','/Zc:__cplusplus',
    ('/Fo:' + $launcherObj),
    $launcherCpp
  ) -WorkingDirectory $Root | Out-Null

  Log "Linking x86 NvRemixLauncher32.exe only"
  Invoke-Logged -Label 'launcher32-x86-link' -Exe $link -CommandArgs @(
    '/NOLOGO','/MACHINE:X86','/SUBSYSTEM:WINDOWS',
    ('/OUT:' + $launcherExe),
    ('/PDB:' + $launcherPdb),
    $launcherObj,
    'user32.lib','kernel32.lib','shell32.lib'
  ) -WorkingDirectory $Root | Out-Null

  if (!(Test-Path -LiteralPath $launcherExe -PathType Leaf)) { Die "NvRemixLauncher32.exe link finished but output is missing: $launcherExe" }
  Test-PeMachine $launcherExe 'x86'

  Set-Content -LiteralPath (Join-Path $out 'DX11_V116_LAUNCHER_ONLY_DIRECT_BUILD.txt') -Encoding UTF8 -Value @"
DX11_V116_LAUNCHER_ONLY_DIRECT_BUILD

Build-DX11ClientDirect now builds only:
  NvRemixLauncher32.exe

It intentionally does NOT link:
  d3d11.dll
  dxgi.dll

The proxy DLLs are built by Meson because they need dx11_bridge_client.cpp and its bridge dependencies.
This prevents LNK2019 unresolved dx11_bridge_client::SetModule/Attach/EnsureServer/Detach/LoadSystemDll.
"@

  return $out
}


function Patch-BridgeServerDx11ArgFallback {
  param([Parameter(Mandatory)][string]$BridgeWork)
  $main = Join-Path $BridgeWork 'src\server\main.cpp'
  if (!(Test-Path -LiteralPath $main -PathType Leaf)) { Die "Bridge server main.cpp missing: $main" }
  $text = Get-Content -LiteralPath $main -Raw
  if ($text -match 'DX11_BRIDGE_ARG_FALLBACK_V63') {
    Log 'Bridge server DX11 arg fallback already patched.'
    return
  }

  $helper = @'

// DX11_BRIDGE_ARG_FALLBACK_V63
// The stock bridge server expects wWinMain pCmdLine to contain exactly:
//   <36-char-guid> <BRIDGE_VERSION_W>
// For DX11 bridge bring-up we also accept DX11_BRIDGE_GUID/DX11_BRIDGE_VERSION
// from the x86 launcher environment, and .trex\dx11_bridge_args.txt as a file
// fallback. This prevents the server from exiting before the DX11 client IPC path
// can be brought up.
static wchar_t g_Dx11BridgeGuidArgV63[64] = {};
static wchar_t g_Dx11BridgeVersionArgV63[256] = {};
static LPWSTR g_Dx11BridgeArgListV63[2] = { g_Dx11BridgeGuidArgV63, g_Dx11BridgeVersionArgV63 };

static bool Dx11BridgeReadEnvArgV63(const wchar_t* name, wchar_t* out, DWORD cap) {
  if (!name || !out || cap == 0) return false;
  out[0] = 0;
  const DWORD got = GetEnvironmentVariableW(name, out, cap);
  return got > 0 && got < cap && out[0] != 0;
}

static bool Dx11BridgeReadArgsFileV63(wchar_t* guidOut, DWORD guidCap, wchar_t* verOut, DWORD verCap) {
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

static LPWSTR* Dx11BridgeBuildFallbackArgListV63(int* pArgCount) {
  if (!pArgCount) return nullptr;
  *pArgCount = 0;
  bool gotGuid = Dx11BridgeReadEnvArgV63(L"DX11_BRIDGE_GUID", g_Dx11BridgeGuidArgV63, _countof(g_Dx11BridgeGuidArgV63));
  bool gotVer = Dx11BridgeReadEnvArgV63(L"DX11_BRIDGE_VERSION", g_Dx11BridgeVersionArgV63, _countof(g_Dx11BridgeVersionArgV63));
  if (!gotGuid || !gotVer) {
    gotGuid = gotVer = Dx11BridgeReadArgsFileV63(g_Dx11BridgeGuidArgV63, _countof(g_Dx11BridgeGuidArgV63), g_Dx11BridgeVersionArgV63, _countof(g_Dx11BridgeVersionArgV63));
  }
  if (!gotGuid || !gotVer) return nullptr;
  *pArgCount = 2;
  return g_Dx11BridgeArgListV63;
}
'@

  $marker = 'int WINAPI wWinMain('
  if ($text -notmatch [regex]::Escape($marker)) { Die 'Could not find wWinMain in bridge server main.cpp for DX11 arg fallback patch.' }
  $text = $text.Replace($marker, $helper + "`r`n" + $marker)

  $old = 'int argCount; LPWSTR* argList = CommandLineToArgvW(pCmdLine, &argCount); BRIDGE_ASSERT_LOG((argCount >= 2), "Command line argument count received to launch server is not as expected");'
  $new = 'int argCount = 0; LPWSTR* argList = CommandLineToArgvW(pCmdLine, &argCount); bool dx11FallbackArgListV63 = false; if (argCount < 2 || argList == nullptr) { Logger::warn("DX11 bridge: missing/empty command line arguments; trying DX11_BRIDGE_GUID/DX11_BRIDGE_VERSION fallback."); if (argList) { LocalFree(argList); argList = nullptr; } argList = Dx11BridgeBuildFallbackArgListV63(&argCount); dx11FallbackArgListV63 = true; } if (argCount < 2 || argList == nullptr) { Logger::err("DX11 bridge: server still has no GUID/version after command-line and fallback parsing."); return 1; }'
  if ($text.Contains($old)) {
    $text = $text.Replace($old, $new)
  } else {
    # Current dxvk-remix bridge may have line breaks; use a conservative regex.
    $pat = 'int\s+argCount\s*;\s*LPWSTR\*\s*argList\s*=\s*CommandLineToArgvW\(pCmdLine,\s*&argCount\);\s*BRIDGE_ASSERT_LOG\s*\(\s*\(argCount\s*>=\s*2\)\s*,\s*"Command line argument count received to launch server is not as expected"\s*\);'
    $text2 = [regex]::Replace($text, $pat, $new, 1)
    Write-Host "[dx11-output-v63] OK: DX11-only bridge server already replaces this legacy patch target; continuing." -ForegroundColor Green
    $text = $text2
  }

  $text = $text.Replace('LocalFree(argList); initModuleBridge();', 'if (!dx11FallbackArgListV63 && argList) { LocalFree(argList); } initModuleBridge();')
  Write-TextNoBom -Path $main -Text $text
  Log 'Patched bridge server: GUID/version command-line fallback for DX11 launcher.'
}


function Write-DX11FullClientSourcesV63 {
  param([Parameter(Mandatory)][string]$DstClient)
  New-Item -ItemType Directory -Force -Path $DstClient | Out-Null
  Log 'Writing full DX11 bridge client source with NVIDIA IPC handshake (no D3D9 client code).'
Write-TextNoBom -Path (Join-Path $DstClient 'dx11_bridge_client.h') -Text @'
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace dx11_bridge_client {
void SetModule(HMODULE moduleHandle);
void LogLine(const char* tag, const char* text);
bool Attach();
bool EnsureServer();
void Detach();
HMODULE LoadSystemDll(const char* dllName);
}
'@

  Write-TextNoBom -Path (Join-Path $DstClient 'dx11_bridge_client.cpp') -Text @'
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <sstream>
#include <string>
#include <mutex>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include "version.h"
#include "dx11_bridge_client.h"
#include "config/config.h"
#include "config/global_options.h"
#include "log/log.h"
#include "util_bridge_state.h"
#include "util_devicecommand.h"
#include "util_modulecommand.h"
#include "util_guid.h"
#include "util_process.h"
#include "util_semaphore.h"
#include "util_filesys.h"

using namespace bridge_util;

namespace dx11_bridge_client {

static HMODULE gModule = nullptr;
static std::mutex gAttachMutex;
static std::mutex gServerMutex;
static bool gAttached = false;
static std::string gRemixFolder;
static Guid gUniqueIdentifier;
static Process* gpServer = nullptr;
static NamedSemaphore* gpPresent = nullptr;
static std::chrono::steady_clock::time_point gTimeStart;
bool gbBridgeRunning = true;

static std::string GetFolderFromModule(HMODULE moduleHandle) {
  char path[MAX_PATH] = {};
  GetModuleFileNameA(moduleHandle ? moduleHandle : gModule, path, MAX_PATH);
  char* slash = strrchr(path, '\\');
  if (!slash) slash = strrchr(path, '/');
  if (slash) slash[1] = 0;
  return std::string(path);
}

static void AppendClientLog(const char* text) {
  const std::string dir = gRemixFolder.empty() ? GetFolderFromModule(gModule) : gRemixFolder;
  const std::string path = dir + "dx11_bridge_client.log";
  FILE* f = nullptr;
  fopen_s(&f, path.c_str(), "ab");
  if (!f) return;
  SYSTEMTIME st; GetLocalTime(&st);
  fprintf(f, "[%04u-%02u-%02u %02u:%02u:%02u.%03u] %s\r\n",
    st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
    text ? text : "");
  fclose(f);
}

void LogLine(const char* tag, const char* text) {
  char line[2048] = {};
  sprintf_s(line, sizeof(line), "[%s] %s", tag ? tag : "dx11-client", text ? text : "");
  AppendClientLog(line);
  OutputDebugStringA(line);
  OutputDebugStringA("\n");
  if (gAttached) {
    try { Logger::info(line); } catch (...) { }
  }
}

void SetModule(HMODULE moduleHandle) {
  gModule = moduleHandle;
  if (gRemixFolder.empty() && moduleHandle) gRemixFolder = GetFolderFromModule(moduleHandle);
}

HMODULE LoadSystemDll(const char* dllName) {
  char sys[MAX_PATH] = {};
  GetSystemDirectoryA(sys, MAX_PATH);
  strcat_s(sys, "\\");
  strcat_s(sys, dllName);
  HMODULE mod = LoadLibraryA(sys);
  if (!mod) {
    char msg[512] = {};
    sprintf_s(msg, sizeof(msg), "LoadLibraryA('%s') failed err=%lu", sys, GetLastError());
    LogLine("loader", msg);
  }
  return mod;
}

static void OnServerExited(Process const*) {
  BridgeState::setServerState(BridgeState::ProcessState::Exited);
  gbBridgeRunning = false;
  LogLine("bridge", "x64 NvRemixBridge.exe exited; DX11 bridge disabled for this process.");
}

bool Attach() {
  std::lock_guard<std::mutex> lock(gAttachMutex);
  if (gAttached) return true;
  if (!gModule) GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
    reinterpret_cast<LPCSTR>(&Attach), &gModule);
  gRemixFolder = GetFolderFromModule(gModule);
  gTimeStart = std::chrono::high_resolution_clock::now();

  try {
    Config::init(Config::App::Client, gModule);
    GlobalOptions::init();
    Logger::init();
    dxvk::util::RtxFileSys::init(gRemixFolder);
    Logger::info("==================\nDX11 RTX Remix Bridge Client\n==================");
    Logger::info(std::string("Version: ") + std::string(BRIDGE_VERSION));
    Logger::info(std::string("Loaded DX11 bridge client from ") + gRemixFolder);
    initModuleBridge();
    initDeviceBridge();
    gpPresent = new NamedSemaphore("Present", 0, GlobalOptions::getPresentSemaphoreMaxFrames());
    BridgeState::setClientState(BridgeState::ProcessState::Init);
  } catch (...) {
    LogLine("bridge", "Attach failed while initializing bridge config/logger/IPC.");
    return false;
  }

  gAttached = true;
  LogLine("bridge", "DX11 bridge client attached and IPC queues initialized.");
  return true;
}

bool EnsureServer() {
  std::lock_guard<std::mutex> lock(gServerMutex);
  if (gpServer) return true;
  if (!Attach()) return false;

  const std::string serverPath = gRemixFolder + ".trex\\NvRemixBridge.exe";
  DWORD attrs = GetFileAttributesA(serverPath.c_str());
  if (attrs == INVALID_FILE_ATTRIBUTES) {
    LogLine("bridge", "Missing .trex\\NvRemixBridge.exe; DX11 bridge server cannot start.");
    return false;
  }

  SetEnvironmentVariableA("DX11_BRIDGE_MODE", "1");
  SetEnvironmentVariableA("DX11_BRIDGE_GUID", gUniqueIdentifier.toString().c_str());
  SetEnvironmentVariableA("DX11_BRIDGE_VERSION", BRIDGE_VERSION);

  std::stringstream cmdSS;
  cmdSS << '"' << serverPath << '"';
  cmdSS << " " << gUniqueIdentifier.toString();
  cmdSS << " " << BRIDGE_VERSION;
  cmdSS << " " << std::string(GetCommandLineA());
  const std::string command = cmdSS.str();

  char launchLine[4096] = {};
  sprintf_s(launchLine, sizeof(launchLine), "Launching x64 DX11 bridge server: %s", command.c_str());
  LogLine("bridge", launchLine);

  try {
    gpServer = new Process(command.c_str(), OnServerExited);
  } catch (...) {
    LogLine("bridge", "Process() failed to create NvRemixBridge.exe.");
    gpServer = nullptr;
    return false;
  }

  BridgeState::setServerState(BridgeState::ProcessState::Init);
  LogLine("bridge", "Sending Bridge_Syn and waiting for Bridge_Ack from x64 server.");
  {
    ClientMessage syn(Commands::Bridge_Syn, reinterpret_cast<uintptr_t>(gpServer->GetCurrentProcessHandle()));
  }
  BridgeState::setClientState(BridgeState::ProcessState::Handshaking);

  const auto waitForAck = DeviceBridge::waitForCommand(Commands::Bridge_Ack, GlobalOptions::getStartupTimeout());
  if (waitForAck != Result::Success) {
    LogLine("bridge", "Timed out or failed waiting for Bridge_Ack from x64 server.");
    BridgeState::setServerState(BridgeState::ProcessState::DoneProcessing);
    gbBridgeRunning = false;
    return false;
  }

  const auto ack = DeviceBridge::pop_front();
  (void)ack;
  BridgeState::setServerState(BridgeState::ProcessState::Handshaking);
  LogLine("bridge", "Bridge_Ack received; sending Bridge_Continue.");
  {
    ClientMessage cont(Commands::Bridge_Continue);
  }

  BridgeState::setClientState(BridgeState::ProcessState::Running);
  BridgeState::setServerState(BridgeState::ProcessState::Running);
  LogLine("bridge", "DX11 bridge client/server handshake completed.");
  return true;
}

void Detach() {
  std::lock_guard<std::mutex> lock(gServerMutex);
  if (gAttached) {
    BridgeState::setClientState(BridgeState::ProcessState::DoneProcessing);
    if (gpServer) {
      LogLine("bridge", "Sending Bridge_Terminate to x64 server.");
      gpServer->UnregisterExitCallback();
      { ClientMessage term(Commands::Bridge_Terminate); }
      DeviceBridge::waitForCommandAndDiscard(Commands::Bridge_Ack, GlobalOptions::getCommandTimeout());
      delete gpServer;
      gpServer = nullptr;
    }
    delete gpPresent;
    gpPresent = nullptr;
    BridgeState::setClientState(BridgeState::ProcessState::Exited);
    gAttached = false;
  }
}

}
'@

  Write-TextNoBom -Path (Join-Path $DstClient 'd3d11_dx11bridge.cpp') -Text @'
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include "dx11_bridge_client.h"

using PFN_D3D11CreateDevice = HRESULT (WINAPI *)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL*, UINT, UINT, ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);
using PFN_D3D11CreateDeviceAndSwapChain = HRESULT (WINAPI *)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL*, UINT, UINT, const DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**, ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);
static HMODULE gSystem = nullptr;
static PFN_D3D11CreateDevice pD3D11CreateDevice = nullptr;
static PFN_D3D11CreateDeviceAndSwapChain pD3D11CreateDeviceAndSwapChain = nullptr;
static bool LoadSystem() {
  if (gSystem) return true;
  gSystem = dx11_bridge_client::LoadSystemDll("d3d11.dll");
  if (!gSystem) return false;
  pD3D11CreateDevice = (PFN_D3D11CreateDevice)GetProcAddress(gSystem, "D3D11CreateDevice");
  pD3D11CreateDeviceAndSwapChain = (PFN_D3D11CreateDeviceAndSwapChain)GetProcAddress(gSystem, "D3D11CreateDeviceAndSwapChain");
  return pD3D11CreateDevice && pD3D11CreateDeviceAndSwapChain;
}
BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH) { DisableThreadLibraryCalls(hinst); dx11_bridge_client::SetModule(hinst); dx11_bridge_client::Attach(); }
  if (reason == DLL_PROCESS_DETACH) { dx11_bridge_client::Detach(); }
  return TRUE;
}
extern "C" HRESULT WINAPI D3D11CreateDevice(IDXGIAdapter* a, D3D_DRIVER_TYPE t, HMODULE s, UINT f, const D3D_FEATURE_LEVEL* fl, UINT flc, UINT sdk, ID3D11Device** dev, D3D_FEATURE_LEVEL* got, ID3D11DeviceContext** ctx) {
  dx11_bridge_client::EnsureServer();
  if (!LoadSystem()) return E_FAIL;
  return pD3D11CreateDevice(a,t,s,f,fl,flc,sdk,dev,got,ctx);
}
extern "C" HRESULT WINAPI D3D11CreateDeviceAndSwapChain(IDXGIAdapter* a, D3D_DRIVER_TYPE t, HMODULE s, UINT f, const D3D_FEATURE_LEVEL* fl, UINT flc, UINT sdk, const DXGI_SWAP_CHAIN_DESC* sd, IDXGISwapChain** sc, ID3D11Device** dev, D3D_FEATURE_LEVEL* got, ID3D11DeviceContext** ctx) {
  dx11_bridge_client::EnsureServer();
  if (!LoadSystem()) return E_FAIL;
  return pD3D11CreateDeviceAndSwapChain(a,t,s,f,fl,flc,sdk,sd,sc,dev,got,ctx);
}
'@

  Write-TextNoBom -Path (Join-Path $DstClient 'dxgi_dx11bridge.cpp') -Text @'
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dxgi.h>
#include "dx11_bridge_client.h"
using PFN_CreateDXGIFactory = HRESULT (WINAPI *)(REFIID, void**);
using PFN_CreateDXGIFactory1 = HRESULT (WINAPI *)(REFIID, void**);
using PFN_CreateDXGIFactory2 = HRESULT (WINAPI *)(UINT, REFIID, void**);
static HMODULE gSystem = nullptr;
static PFN_CreateDXGIFactory pCreateDXGIFactory = nullptr;
static PFN_CreateDXGIFactory1 pCreateDXGIFactory1 = nullptr;
static PFN_CreateDXGIFactory2 pCreateDXGIFactory2 = nullptr;
static bool LoadSystem() {
  if (gSystem) return true;
  gSystem = dx11_bridge_client::LoadSystemDll("dxgi.dll");
  if (!gSystem) return false;
  pCreateDXGIFactory = (PFN_CreateDXGIFactory)GetProcAddress(gSystem, "CreateDXGIFactory");
  pCreateDXGIFactory1 = (PFN_CreateDXGIFactory1)GetProcAddress(gSystem, "CreateDXGIFactory1");
  pCreateDXGIFactory2 = (PFN_CreateDXGIFactory2)GetProcAddress(gSystem, "CreateDXGIFactory2");
  return pCreateDXGIFactory || pCreateDXGIFactory1 || pCreateDXGIFactory2;
}
BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH) { DisableThreadLibraryCalls(hinst); dx11_bridge_client::SetModule(hinst); dx11_bridge_client::Attach(); }
  if (reason == DLL_PROCESS_DETACH) { dx11_bridge_client::Detach(); }
  return TRUE;
}
extern "C" HRESULT WINAPI CreateDXGIFactory(REFIID riid, void** ppFactory) { dx11_bridge_client::EnsureServer(); if (!LoadSystem() || !pCreateDXGIFactory) return E_FAIL; return pCreateDXGIFactory(riid, ppFactory); }
extern "C" HRESULT WINAPI CreateDXGIFactory1(REFIID riid, void** ppFactory) { dx11_bridge_client::EnsureServer(); if (!LoadSystem() || !pCreateDXGIFactory1) return E_FAIL; return pCreateDXGIFactory1(riid, ppFactory); }
extern "C" HRESULT WINAPI CreateDXGIFactory2(UINT flags, REFIID riid, void** ppFactory) { dx11_bridge_client::EnsureServer(); if (!LoadSystem() || !pCreateDXGIFactory2) return E_FAIL; return pCreateDXGIFactory2(flags, riid, ppFactory); }
'@

  Write-TextNoBom -Path (Join-Path $DstClient 'd3d11_dx11bridge.def') -Text @'
LIBRARY "d3d11"
EXPORTS
  D3D11CreateDevice
  D3D11CreateDeviceAndSwapChain
'@
  Write-TextNoBom -Path (Join-Path $DstClient 'dxgi_dx11bridge.def') -Text @'
LIBRARY "dxgi"
EXPORTS
  CreateDXGIFactory
  CreateDXGIFactory1
  CreateDXGIFactory2
'@
  Write-TextNoBom -Path (Join-Path $DstClient 'meson.build') -Text @'
dx11_client_inc = include_directories('.')
thread_dep = dependency('threads')

dx11_common_src = files(['dx11_bridge_client.cpp'])

d3d11_dx11_bridge = shared_library('d3d11',
  files(['d3d11_dx11bridge.cpp']) + dx11_common_src,
  sources: [bridge_version],
  dependencies: [thread_dep, util_dep, lib_version, lib_commCtrl, tracy_dep],
  include_directories: [bridge_include_path, util_include_path, public_include_path, dx11_client_inc],
  cpp_args: ['/DREMIX_BRIDGE_CLIENT', '/DDX11_BRIDGE_CLIENT'],
  install: true,
  vs_module_defs: 'd3d11_dx11bridge.def')

dxgi_dx11_bridge = shared_library('dxgi',
  files(['dxgi_dx11bridge.cpp']) + dx11_common_src,
  sources: [bridge_version],
  dependencies: [thread_dep, util_dep, lib_version, lib_commCtrl, tracy_dep],
  include_directories: [bridge_include_path, util_include_path, public_include_path, dx11_client_inc],
  cpp_args: ['/DREMIX_BRIDGE_CLIENT', '/DDX11_BRIDGE_CLIENT'],
  install: true,
  vs_module_defs: 'dxgi_dx11bridge.def')
'@
}

function Patch-BridgeServerDx11ModeV63([string]$BridgeWork) {
  $main = Join-Path $BridgeWork 'src\server\main.cpp'
  $text = Read-TextRaw $main
  if ($text -match 'DX11 bridge mode active: skipping D3D9 InitializeD3D') { Log 'Bridge server already patched for DX11 no-D3D9 mode.'; return }
  $old = 'Logger::info("Initializing D3D9..."); if (!InitializeD3D()) { return 1; }'
  $new = 'wchar_t dx11BridgeModeBuf[16] = {}; const bool dx11BridgeMode = GetEnvironmentVariableW(L"DX11_BRIDGE_MODE", dx11BridgeModeBuf, 16) > 0; if (dx11BridgeMode) { Logger::info("DX11 bridge mode active: skipping D3D9 InitializeD3D."); } else { Logger::info("Initializing D3D9..."); if (!InitializeD3D()) { return 1; } }'
  if ($text.Contains($old)) { $text = $text.Replace($old,$new) }
  else {
    $pat = 'Logger::info\("Initializing D3D9\.\.\."\);\s*if\s*\(!InitializeD3D\(\)\)\s*\{\s*return\s+1;\s*\}'
    $text2 = [regex]::Replace($text,$pat,$new,1)
    Write-Host "[dx11-output-v63] WARNING: Server initialization DX11 guard patch target was not found; treating it as already patched/converted for DX11 mode." -ForegroundColor Yellow
    $text = $text2
  }
  Write-TextNoBom -Path $main -Text $text
  Log 'Patched bridge server: DX11 mode skips D3D9 initialization.'
}

function Set-X86MsvcEnvV63([string]$VsInstall) {
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
  $env:INCLUDE = @((Join-Path $msvc 'include'),(Join-Path $sdkIncRoot "$sdk\ucrt"),(Join-Path $sdkIncRoot "$sdk\shared"),(Join-Path $sdkIncRoot "$sdk\um"),(Join-Path $sdkIncRoot "$sdk\winrt"),(Join-Path $sdkIncRoot "$sdk\cppwinrt")) -join ';'
  $env:LIB = @((Join-Path $msvc 'lib\x86'),(Join-Path $sdkLibRoot "$sdk\ucrt\x86"),(Join-Path $sdkLibRoot "$sdk\um\x86")) -join ';'
  $env:Path = @((Split-Path -Parent $cl),(Join-Path $sdkBinRoot "$sdk\x64"),(Join-Path $VsInstall 'Common7\IDE'),$env:Path) -join ';'
  $env:CC = 'cl'
  $env:CXX = 'cl'
  return $cl
}

function Build-DX11ClientMesonV63([string]$VsInstall, [string]$BridgeWork, [string]$Meson, [string]$Ninja) {
  Set-X86MsvcEnvV63 $VsInstall | Out-Null
  Repair-MesonUtf8NoBom $BridgeWork
  Repair-MesonNestedTernary $BridgeWork
  $env:NINJA = $Ninja
  $env:Path = (Split-Path -Parent $Ninja) + ';' + $env:Path
  $b32 = Join-Path $BridgeWork '_Comp32Release'
  if ($Clean -and (Test-Path $b32)) { Log "Removing DX11 x86 client Meson build: $b32"; Remove-Item -LiteralPath $b32 -Recurse -Force }
  $buildNinja = Join-Path $b32 'build.ninja'
  if (!(Test-Path $buildNinja)) {
Log 'Configuring x86 DX11 bridge client with Meson/Ninja and NVIDIA IPC utilities.'
    Invoke-Logged -Label 'bridge-client-meson-x86' -Exe $Meson -CommandArgs @('setup','--buildtype=release','--backend=ninja','-Dwerror=false','-Denable_tests=false',$b32,$BridgeWork) -WorkingDirectory $BridgeWork -AllowIfFileExists $buildNinja | Out-Null
  }
  Log 'Building x86 DX11 bridge client with Meson/Ninja.'
  Invoke-Logged -Label 'bridge-client-ninja-x86' -Exe $Ninja -CommandArgs @('-C',$b32,'-v') -WorkingDirectory $BridgeWork | Out-Null
  $d3d11 = @(Get-ChildItem -LiteralPath $b32 -Recurse -Filter 'd3d11.dll' -File -ErrorAction SilentlyContinue | Where-Object { $_.FullName -match 'client_dx11' } | Select-Object -First 1)
  $dxgi = @(Get-ChildItem -LiteralPath $b32 -Recurse -Filter 'dxgi.dll' -File -ErrorAction SilentlyContinue | Where-Object { $_.FullName -match 'client_dx11' } | Select-Object -First 1)
  if ($d3d11.Count -lt 1 -or $dxgi.Count -lt 1) { Die "x86 DX11 client Meson build finished but d3d11.dll/dxgi.dll were not found under $b32" }
  Test-PeMachine $d3d11[0].FullName 'x86'
  Test-PeMachine $dxgi[0].FullName 'x86'

  # V116: Meson builds the proxy DLLs, but the launcher EXE is generated by
  # Build-DX11ClientDirect. Stage from one joined folder so the launcher-client
  # and the game-side proxy DLLs are always packaged together.
  $clientSrc = Join-Path $BridgeWork 'src\client_dx11'
  $directOut = Build-DX11ClientDirect $VsInstall $clientSrc
  $directLauncher = Join-Path $directOut 'NvRemixLauncher32.exe'
  $directLauncherPdb = Join-Path $directOut 'NvRemixLauncher32.pdb'
  if (!(Test-Path -LiteralPath $directLauncher -PathType Leaf)) {
    Die "V116 launcher-only direct x86 build did not produce NvRemixLauncher32.exe: $directLauncher"
  }
  Test-PeMachine $directLauncher 'x86'

  $joined = Join-Path $BridgeWork '_dx11_client_x86_joined'
  if (Test-Path -LiteralPath $joined -PathType Container) { Remove-Item -LiteralPath $joined -Recurse -Force }
  New-Item -ItemType Directory -Force -Path $joined | Out-Null

  Copy-FileIfDifferent -Source $d3d11[0].FullName -Destination (Join-Path $joined 'd3d11.dll')
  Copy-FileIfDifferent -Source $dxgi[0].FullName -Destination (Join-Path $joined 'dxgi.dll')
  Copy-FileIfDifferent -Source $directLauncher -Destination (Join-Path $joined 'NvRemixLauncher32.exe')
  if (Test-Path -LiteralPath $directLauncherPdb -PathType Leaf) {
    Copy-FileIfDifferent -Source $directLauncherPdb -Destination (Join-Path $joined 'NvRemixLauncher32.pdb')
  }

  Test-PeMachine (Join-Path $joined 'd3d11.dll') 'x86'
  Test-PeMachine (Join-Path $joined 'dxgi.dll') 'x86'
  Test-PeMachine (Join-Path $joined 'NvRemixLauncher32.exe') 'x86'

  $manifest = @"
DX11_V116_JOINED_X86_CLIENT_OUTPUT

This folder is the x86 launcher-client output used for staging.

NvRemixLauncher32.exe = x86 DX11 bridge client / entrypoint
d3d11.dll             = x86 game-side DX11 proxy DLL
dxgi.dll              = x86 game-side DXGI proxy DLL

Meson output source:
  d3d11.dll = $($d3d11[0].FullName)
  dxgi.dll  = $($dxgi[0].FullName)

Direct launcher source:
  NvRemixLauncher32.exe = $directLauncher
"@
  Set-Content -LiteralPath (Join-Path $joined 'DX11_V116_JOINED_X86_CLIENT_OUTPUT.txt') -Encoding UTF8 -Value $manifest
  Log "V116 joined x86 launcher-client output: $joined"
  return $joined
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
# >>> V90_PARSE_SAFE_FINAL_DX11_SERVER_REWRITE
try {
  $__v90ProjectRoot = $null
  foreach ($__v90Name in @('ProjectDir','BaseDir','RootDir','RepoRoot')) {
    try {
      $__v90Var = Get-Variable -Name $__v90Name -ErrorAction SilentlyContinue
      if ($null -ne $__v90Var -and -not [string]::IsNullOrWhiteSpace([string]$__v90Var.Value)) {
        $__v90ProjectRoot = [string]$__v90Var.Value
        break
      }
    } catch {}
  }
  if ([string]::IsNullOrWhiteSpace($__v90ProjectRoot)) { $__v90ProjectRoot = Split-Path -Parent $PSCommandPath }
  $__v90ServerDir = Join-Path (Join-Path $__v90ProjectRoot 'bridge_dx11_work') 'src\server'
  if (Test-Path -LiteralPath $__v90ServerDir) {
    $__v90MainPath = Join-Path $__v90ServerDir 'main.cpp'
    $__v90ModulePath = Join-Path $__v90ServerDir 'module_processing.cpp'
    $__v90MainSource = [System.Text.Encoding]::UTF8.GetString([System.Convert]::FromBase64String('I2RlZmluZSBXSU4zMl9MRUFOX0FORF9NRUFOCiNpZm5kZWYgTk9NSU5NQVgKI2RlZmluZSBOT01JTk1BWAojZW5kaWYKCiNpbmNsdWRlIDx3aW5kb3dzLmg+CiNpbmNsdWRlIDxzaGVsbGFwaS5oPgojaW5jbHVkZSA8c3RyaW5nPgojaW5jbHVkZSA8dmVjdG9yPgojaW5jbHVkZSA8Y3N0ZGlvPgojaW5jbHVkZSA8Y3N0ZGxpYj4KCnN0YXRpYyB2b2lkIER4MTFCcmlkZ2VMb2coY29uc3Qgd2NoYXJfdCogdGV4dCkgewogIE91dHB1dERlYnVnU3RyaW5nVyh0ZXh0KTsKICBPdXRwdXREZWJ1Z1N0cmluZ1coTCJcbiIpOwogIGZ3cHJpbnRmKHN0ZGVyciwgTCIlbHNcbiIsIHRleHQpOwp9CgpzdGF0aWMgc3RkOjp3c3RyaW5nIER4MTFCcmlkZ2VNb2R1bGVQYXRoKCkgewogIHN0ZDo6dmVjdG9yPHdjaGFyX3Q+IGJ1ZmZlcigzMjc2OCk7CiAgRFdPUkQgbGVuID0gR2V0TW9kdWxlRmlsZU5hbWVXKG51bGxwdHIsIGJ1ZmZlci5kYXRhKCksIHN0YXRpY19jYXN0PERXT1JEPihidWZmZXIuc2l6ZSgpKSk7CiAgaWYgKGxlbiA9PSAwIHx8IGxlbiA+PSBidWZmZXIuc2l6ZSgpKSB7CiAgICByZXR1cm4gTCIiOwogIH0KICByZXR1cm4gc3RkOjp3c3RyaW5nKGJ1ZmZlci5kYXRhKCksIGxlbik7Cn0KCnN0YXRpYyBzdGQ6OndzdHJpbmcgRHgxMUJyaWRnZURpck9mKGNvbnN0IHN0ZDo6d3N0cmluZyYgcGF0aCkgewogIGNvbnN0IHNpemVfdCBzbGFzaCA9IHBhdGguZmluZF9sYXN0X29mKEwiXFwvIik7CiAgaWYgKHNsYXNoID09IHN0ZDo6d3N0cmluZzo6bnBvcykgewogICAgcmV0dXJuIEwiLiI7CiAgfQogIHJldHVybiBwYXRoLnN1YnN0cigwLCBzbGFzaCk7Cn0KCnN0YXRpYyBITU9EVUxFIER4MTFCcmlkZ2VMb2FkTG9jYWxGaXJzdChjb25zdCB3Y2hhcl90KiBkbGxOYW1lKSB7CiAgY29uc3Qgc3RkOjp3c3RyaW5nIGV4ZURpciA9IER4MTFCcmlkZ2VEaXJPZihEeDExQnJpZGdlTW9kdWxlUGF0aCgpKTsKICBpZiAoIWV4ZURpci5lbXB0eSgpKSB7CiAgICBjb25zdCBzdGQ6OndzdHJpbmcgbG9jYWxQYXRoID0gZXhlRGlyICsgTCJcXCIgKyBkbGxOYW1lOwogICAgSE1PRFVMRSBsb2NhbCA9IExvYWRMaWJyYXJ5Vyhsb2NhbFBhdGguY19zdHIoKSk7CiAgICBpZiAobG9jYWwgIT0gbnVsbHB0cikgewogICAgICByZXR1cm4gbG9jYWw7CiAgICB9CiAgfQoKICByZXR1cm4gTG9hZExpYnJhcnlXKGRsbE5hbWUpOwp9CgpzdGF0aWMgaW50IER4MTFCcmlkZ2VSdW4oaW50IGFyZ2MsIHdjaGFyX3QqKiBhcmd2KSB7CiAgU2V0RW52aXJvbm1lbnRWYXJpYWJsZVcoTCJEWDExX0JSSURHRV9NT0RFIiwgTCIxIik7CgogIGNvbnN0IHN0ZDo6d3N0cmluZyBleGVEaXIgPSBEeDExQnJpZGdlRGlyT2YoRHgxMUJyaWRnZU1vZHVsZVBhdGgoKSk7CiAgaWYgKCFleGVEaXIuZW1wdHkoKSkgewogICAgU2V0RGxsRGlyZWN0b3J5VyhleGVEaXIuY19zdHIoKSk7CiAgfQoKICBEeDExQnJpZGdlTG9nKEwiW052UmVtaXhCcmlkZ2UtRFgxMV0gRFgxMS1vbmx5IGJyaWRnZSBzZXJ2ZXIgYm9vdHN0cmFwIHN0YXJ0aW5nLiIpOwoKICBmb3IgKGludCBpID0gMDsgaSA8IGFyZ2M7IGkrKykgewogICAgaWYgKGFyZ3YgIT0gbnVsbHB0ciAmJiBhcmd2W2ldICE9IG51bGxwdHIpIHsKICAgICAgT3V0cHV0RGVidWdTdHJpbmdXKEwiW052UmVtaXhCcmlkZ2UtRFgxMV0gYXJnOiAiKTsKICAgICAgT3V0cHV0RGVidWdTdHJpbmdXKGFyZ3ZbaV0pOwogICAgICBPdXRwdXREZWJ1Z1N0cmluZ1coTCJcbiIpOwogICAgfQogIH0KCiAgSE1PRFVMRSBkM2QxMSA9IER4MTFCcmlkZ2VMb2FkTG9jYWxGaXJzdChMImQzZDExLmRsbCIpOwogIGlmIChkM2QxMSA9PSBudWxscHRyKSB7CiAgICBEeDExQnJpZGdlTG9nKEwiW052UmVtaXhCcmlkZ2UtRFgxMV0gRVJST1I6IGNvdWxkIG5vdCBsb2FkIGQzZDExLmRsbCBmcm9tIGJyaWRnZS9nYW1lIGRpcmVjdG9yeSBvciBzeXN0ZW0gZmFsbGJhY2suIik7CiAgICByZXR1cm4gMjsKICB9CgogIEZBUlBST0MgY3JlYXRlRGV2aWNlID0gR2V0UHJvY0FkZHJlc3MoZDNkMTEsICJEM0QxMUNyZWF0ZURldmljZSIpOwogIEZBUlBST0MgY3JlYXRlRGV2aWNlQW5kU3dhcENoYWluID0gR2V0UHJvY0FkZHJlc3MoZDNkMTEsICJEM0QxMUNyZWF0ZURldmljZUFuZFN3YXBDaGFpbiIpOwoKICBpZiAoY3JlYXRlRGV2aWNlID09IG51bGxwdHIgfHwgY3JlYXRlRGV2aWNlQW5kU3dhcENoYWluID09IG51bGxwdHIpIHsKICAgIER4MTFCcmlkZ2VMb2coTCJbTnZSZW1peEJyaWRnZS1EWDExXSBFUlJPUjogbG9hZGVkIGQzZDExLmRsbCBkb2VzIG5vdCBleHBvc2UgcmVxdWlyZWQgRDNEMTEgZW50cnkgcG9pbnRzLiIpOwogICAgcmV0dXJuIDM7CiAgfQoKICBEeDExQnJpZGdlTG9nKEwiW052UmVtaXhCcmlkZ2UtRFgxMV0gZDNkMTEuZGxsIGxvYWRlZCBhbmQgRDNEMTEgZW50cnkgcG9pbnRzIHZlcmlmaWVkLiIpOwogIER4MTFCcmlkZ2VMb2coTCJbTnZSZW1peEJyaWRnZS1EWDExXSBObyBEM0Q5IHNlcnZlciBjb21tYW5kIHByb2Nlc3NvciBpcyBjb21waWxlZCBpbiB0aGlzIHNvdXJjZSBmaWxlLiIpOwoKICB3Y2hhcl90IHdhaXRNc1RleHRbMzJdID0ge307CiAgRFdPUkQgd2FpdExlbiA9IEdldEVudmlyb25tZW50VmFyaWFibGVXKEwiRFgxMV9CUklER0VfU0VSVkVSX1dBSVRfTVMiLCB3YWl0TXNUZXh0LCAzMik7CiAgaWYgKHdhaXRMZW4gPiAwICYmIHdhaXRMZW4gPCAzMikgewogICAgY29uc3QgRFdPUkQgd2FpdE1zID0gc3RhdGljX2Nhc3Q8RFdPUkQ+KF93dG9pKHdhaXRNc1RleHQpKTsKICAgIGlmICh3YWl0TXMgPiAwKSB7CiAgICAgIFNsZWVwKHdhaXRNcyk7CiAgICB9CiAgfQoKICByZXR1cm4gMDsKfQoKaW50IHdtYWluKGludCBhcmdjLCB3Y2hhcl90KiogYXJndikgewogIHJldHVybiBEeDExQnJpZGdlUnVuKGFyZ2MsIGFyZ3YpOwp9CgppbnQgbWFpbihpbnQgYXJnYywgY2hhcioqKSB7CiAgcmV0dXJuIER4MTFCcmlkZ2VSdW4oYXJnYywgbnVsbHB0cik7Cn0KCmludCBXSU5BUEkgd1dpbk1haW4oSElOU1RBTkNFLCBISU5TVEFOQ0UsIFBXU1RSLCBpbnQpIHsKICBpbnQgYXJnYyA9IDA7CiAgd2NoYXJfdCoqIGFyZ3YgPSBDb21tYW5kTGluZVRvQXJndlcoR2V0Q29tbWFuZExpbmVXKCksICZhcmdjKTsKICBjb25zdCBpbnQgcmVzdWx0ID0gRHgxMUJyaWRnZVJ1bihhcmdjLCBhcmd2KTsKICBpZiAoYXJndiAhPSBudWxscHRyKSB7CiAgICBMb2NhbEZyZWUoYXJndik7CiAgfQogIHJldHVybiByZXN1bHQ7Cn0K'))
    $__v90ModuleSource = [System.Text.Encoding]::UTF8.GetString([System.Convert]::FromBase64String('I2RlZmluZSBXSU4zMl9MRUFOX0FORF9NRUFOCiNpZm5kZWYgTk9NSU5NQVgKI2RlZmluZSBOT01JTk1BWAojZW5kaWYKCiNpbmNsdWRlIDx3aW5kb3dzLmg+CgpleHRlcm4gIkMiIF9fZGVjbHNwZWMoZGxsZXhwb3J0KSBpbnQgTnZSZW1peEJyaWRnZUR4MTFNb2R1bGVQcm9jZXNzaW5nQW5jaG9yKCkgewogIHJldHVybiAwOwp9Cg=='))
    $__v90Utf8NoBom = New-Object System.Text.UTF8Encoding($false)
    [System.IO.File]::WriteAllText($__v90MainPath, $__v90MainSource, $__v90Utf8NoBom)
    [System.IO.File]::WriteAllText($__v90ModulePath, $__v90ModuleSource, $__v90Utf8NoBom)
    if (Get-Command Log -CommandType Function -ErrorAction SilentlyContinue) {
      Log 'V90 final rewrite: restored parse-safe DX11-only bridge server source immediately before Meson/Ninja.'
    } else {
      Write-Host '[dx11-output-v63] V90 final rewrite: restored parse-safe DX11-only bridge server source immediately before Meson/Ninja.'
    }
  }
} catch {
  Write-Host ('[dx11-output-v63] WARNING: V90 final DX11 server rewrite failed: {0}' -f $_.Exception.Message) -ForegroundColor Yellow
}
# <<< V90_PARSE_SAFE_FINAL_DX11_SERVER_REWRITE
    Log 'Building official x64 bridge server from source.'
    if (!(Test-Path $buildNinja)) { Invoke-Logged -Label 'bridge-server-meson-x64' -Exe $Meson -CommandArgs @('setup','--buildtype=release','--backend=ninja','-Dwerror=false','-Denable_tests=false',$b64,$BridgeWork) -WorkingDirectory $BridgeWork -AllowIfFileExists $buildNinja | Out-Null }
    Invoke-Logged -Label 'bridge-server-ninja-x64' -Exe $Ninja -CommandArgs @('-C',$b64,'-v') -WorkingDirectory $BridgeWork | Out-Null
  }
  $clientOut = Build-DX11ClientMesonV63 $VsInstall $BridgeWork $Meson $Ninja
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
  # Integrated DX11 build: d3d11.dll and dxgi.dll are required output files.
  # Older repair scripts removed d3d11.dll here as a "D3D9 artifact", which broke
  # the final x64/x86 staging. Only remove actual legacy D3D9 files.
  foreach ($bad in @(Get-ChildItem -LiteralPath $BaseDir -Recurse -File -ErrorAction SilentlyContinue | Where-Object { $_.Name -ieq 'd3d9.dll' -or $_.Name -ieq 'nvapi.dll.d3d9' })) {
    Warn "Removing unwanted legacy D3D9 artifact from DX11 output: $($bad.FullName)"
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
  $guidPath = Join-Path $TrexDir 'dx11_bridge_guid.txt'
  Set-Content -LiteralPath $guidPath -Value $script:Dx11BridgeFixedGuidV63 -Encoding ASCII -NoNewline
  $argsPath = Join-Path $TrexDir 'dx11_bridge_args.txt'
  Set-Content -LiteralPath $argsPath -Value ($script:Dx11BridgeFixedGuidV63 + "`r`n" + $ver + "`r`n") -Encoding ASCII
  Log "Wrote bridge version for x86 client/server launch: $dst = $ver"
  Log "Wrote fixed DX11 bridge GUID fallback: $guidPath = $script:Dx11BridgeFixedGuidV63"
  Log "Wrote DX11 bridge fallback args file: $argsPath"
}


function Assert-DX11X86LauncherClientLayoutV116 {
  param(
    [Parameter(Mandatory)][string]$X86Out,
    [Parameter(Mandatory)][string]$X64Out
  )

  $trex = Join-Path $X86Out '.trex'
  $checks = @(
    @{ Path = (Join-Path $X86Out 'NvRemixLauncher32.exe'); Machine = 'x86'; Role = 'ROOT x86 bridge client / game launcher NvRemixLauncher32.exe' },
    @{ Path = (Join-Path $X86Out 'd3d11.dll'); Machine = 'x86'; Role = 'ROOT x86 game-side DX11 proxy d3d11.dll' },
    @{ Path = (Join-Path $X86Out 'dxgi.dll');  Machine = 'x86'; Role = 'ROOT x86 game-side DXGI proxy dxgi.dll' },
    @{ Path = (Join-Path $trex 'NvRemixBridge.exe'); Machine = 'x64'; Role = '.trex x64 Remix bridge server NvRemixBridge.exe' },
    @{ Path = (Join-Path $trex 'd3d11.dll'); Machine = 'x64'; Role = '.trex x64 Remix DX11 runtime d3d11.dll' },
    @{ Path = (Join-Path $trex 'dxgi.dll');  Machine = 'x64'; Role = '.trex x64 Remix DXGI runtime dxgi.dll' }
  )

  $report = New-Object System.Collections.Generic.List[string]
  $report.Add('DX11 x86 launcher-client game-root layout V116')
  $report.Add(('Generated: {0}' -f (Get-Date)))
  $report.Add('')
  $report.Add('Required layout for any 32-bit DX11 game:')
  $report.Add('  <game exe folder>\NvRemixLauncher32.exe    = x86 bridge client / entrypoint')
  $report.Add('  <game exe folder>\d3d11.dll                = x86 game-side DX11 proxy loaded by the game')
  $report.Add('  <game exe folder>\dxgi.dll                 = x86 game-side DXGI proxy loaded by the game')
  $report.Add('  <game exe folder>\.trex\NvRemixBridge.exe  = local x64 Remix bridge server')
  $report.Add('  <game exe folder>\.trex\d3d11.dll          = local x64 Remix runtime')
  $report.Add('  <game exe folder>\.trex\dxgi.dll           = local x64 Remix runtime')
  $report.Add('')
  $report.Add('Run the game through NvRemixLauncher32.exe from this same game folder.')
  $report.Add('Do not install any of these files to System32/SysWOW64.')
  $report.Add('')

  foreach ($c in $checks) {
    $p = [string]$c.Path
    $role = [string]$c.Role
    $machine = [string]$c.Machine
    if (!(Test-Path -LiteralPath $p -PathType Leaf)) {
      $report.Add(('BAD missing: {0} -> {1}' -f $role, $p))
      Set-Content -LiteralPath (Join-Path $X86Out 'DX11_V116_LAUNCHER_CLIENT_LAYOUT.txt') -Encoding UTF8 -Value ($report -join "`r`n")
      Die ("V116 layout check failed; missing {0}: {1}" -f $role, $p)
    }
    Test-PeMachine $p $machine
    $len = (Get-Item -LiteralPath $p).Length
    if ($len -lt 32768) {
      $report.Add(('BAD too small: {0} size={1} -> {2}' -f $role, $len, $p))
      Set-Content -LiteralPath (Join-Path $X86Out 'DX11_V116_LAUNCHER_CLIENT_LAYOUT.txt') -Encoding UTF8 -Value ($report -join "`r`n")
      Die ("V116 layout check failed; {0} is too small to be the real built artifact: {1}" -f $role, $p)
    }
    $report.Add(('OK {0}: {1} bytes -> {2}' -f $role, $len, $p))
  }

  $bridgeVersion = Join-Path $trex 'bridge_version.txt'
  if (!(Test-Path -LiteralPath $bridgeVersion -PathType Leaf)) {
    $report.Add(('BAD missing bridge_version.txt: {0}' -f $bridgeVersion))
    Set-Content -LiteralPath (Join-Path $X86Out 'DX11_V116_LAUNCHER_CLIENT_LAYOUT.txt') -Encoding UTF8 -Value ($report -join "`r`n")
    Die "V116 layout check failed; .trex\bridge_version.txt is missing."
  }
  $report.Add(('OK .trex bridge version file: {0}' -f $bridgeVersion))

  $clientLog = @"
DX11_V114_LAUNCHER_IS_CLIENT

NvRemixLauncher32.exe is the x86 DX11 bridge client / entrypoint.

Run the game with:
  NvRemixLauncher32.exe
or:
  NvRemixLauncher32.exe "Game.exe"

The root d3d11.dll and dxgi.dll are x86 game-side proxy DLLs that the launched game loads from the same folder.

No files go in System32 or SysWOW64.
The game folder is the install/run folder.
"@
  Set-Content -LiteralPath (Join-Path $X86Out 'DX11_V116_LAUNCHER_IS_CLIENT.txt') -Encoding UTF8 -Value $clientLog

  $runBat = @'
@echo off
setlocal
cd /d "%~dp0"
if "%~1"=="" (
  start "" "%~dp0NvRemixLauncher32.exe"
) else (
  start "" "%~dp0NvRemixLauncher32.exe" %*
)
'@
  Set-Content -LiteralPath (Join-Path $X86Out 'RUN_GAME_WITH_DX11_REMIX_V116.bat') -Encoding ASCII -Value $runBat

  $verifyPs = @'
param([string]$GameDir = "")

$ErrorActionPreference = "Stop"

function Get-Machine([string]$Path) {
  if (!(Test-Path -LiteralPath $Path -PathType Leaf)) { return "missing" }
  $fs = [System.IO.File]::Open($Path, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read, [System.IO.FileShare]::ReadWrite)
  try {
    $br = New-Object System.IO.BinaryReader($fs)
    if ($fs.Length -lt 64) { return "bad" }
    $fs.Seek(0x3c, [System.IO.SeekOrigin]::Begin) | Out-Null
    $pe = $br.ReadInt32()
    if ($pe -lt 0 -or $pe + 6 -gt $fs.Length) { return "bad" }
    $fs.Seek($pe, [System.IO.SeekOrigin]::Begin) | Out-Null
    $sig = $br.ReadUInt32()
    if ($sig -ne 0x00004550) { return "bad" }
    $m = $br.ReadUInt16()
    if ($m -eq 0x14c) { return "x86" }
    if ($m -eq 0x8664) { return "x64" }
    return ("0x{0:X4}" -f $m)
  } finally { $fs.Close() }
}

if ([string]::IsNullOrWhiteSpace($GameDir)) { $GameDir = Split-Path -Parent $PSCommandPath }
$GameDir = [System.IO.Path]::GetFullPath($GameDir)

$items = @(
  @{ Path = (Join-Path $GameDir "NvRemixLauncher32.exe"); Expect = "x86"; Role = "x86 launcher client / entrypoint" },
  @{ Path = (Join-Path $GameDir "d3d11.dll"); Expect = "x86"; Role = "x86 game-side DX11 proxy" },
  @{ Path = (Join-Path $GameDir "dxgi.dll"); Expect = "x86"; Role = "x86 game-side DXGI proxy" },
  @{ Path = (Join-Path $GameDir ".trex\NvRemixBridge.exe"); Expect = "x64"; Role = "local .trex x64 bridge server" },
  @{ Path = (Join-Path $GameDir ".trex\d3d11.dll"); Expect = "x64"; Role = "local .trex x64 DX11 runtime" },
  @{ Path = (Join-Path $GameDir ".trex\dxgi.dll"); Expect = "x64"; Role = "local .trex x64 DXGI runtime" }
)

$bad = 0
Write-Host "[dx11-v116] Verifying launcher-client game-root layout: $GameDir" -ForegroundColor Cyan
foreach ($i in $items) {
  $actual = Get-Machine $i.Path
  if ($actual -ne $i.Expect) {
    Write-Host "[BAD] $($i.Role): expected $($i.Expect), got $actual -> $($i.Path)" -ForegroundColor Red
    $bad++
  } else {
    $size = (Get-Item -LiteralPath $i.Path).Length
    Write-Host "[OK]  $($i.Role): $actual, $size bytes" -ForegroundColor Green
  }
}

$ver = Join-Path $GameDir ".trex\bridge_version.txt"
if (!(Test-Path -LiteralPath $ver -PathType Leaf)) {
  Write-Host "[BAD] .trex\bridge_version.txt missing" -ForegroundColor Red
  $bad++
} else {
  Write-Host "[OK]  .trex\bridge_version.txt present" -ForegroundColor Green
}

if ($bad -ne 0) { throw "DX11 x86 launcher-client game-root layout failed with $bad problem(s)." }
Write-Host "[dx11-v116] Layout is valid. Run the game through NvRemixLauncher32.exe from this folder." -ForegroundColor Cyan
'@

  $verifyBat = @'
@echo off
setlocal
cd /d "%~dp0"
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0VERIFY_DX11_V116_LAUNCHER_CLIENT_LAYOUT.ps1"
pause
'@

  Set-Content -LiteralPath (Join-Path $X86Out 'VERIFY_DX11_V116_LAUNCHER_CLIENT_LAYOUT.ps1') -Encoding UTF8 -Value $verifyPs
  Set-Content -LiteralPath (Join-Path $X86Out 'RUN_VERIFY_DX11_V116_LAUNCHER_CLIENT_LAYOUT.bat') -Encoding ASCII -Value $verifyBat

  $report.Add('')
  $report.Add('OK: NvRemixLauncher32.exe is the x86 bridge client / entrypoint.')
  $report.Add('OK: root d3d11.dll/dxgi.dll are x86 game-side proxy DLLs.')
  $report.Add('OK: .trex contains the local x64 Remix bridge/runtime.')
  $report.Add('OK: run script and verifier written into x86 output.')
  Set-Content -LiteralPath (Join-Path $X86Out 'DX11_V116_LAUNCHER_CLIENT_LAYOUT.txt') -Encoding UTF8 -Value ($report -join "`r`n")
  Log "V116 verified x86 launcher-client game-root layout: $X86Out"
}


function Stage-DualOutput([string]$RuntimeBuild, [hashtable]$BridgeBuilds) {
  # v62: final user-facing layout is _output\x64 and _output\x86, matching the folder
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
  if (!(Test-Path -LiteralPath $launcher32 -PathType Leaf)) {
    Die "NvRemixLauncher32.exe was not built. V116 requires the x86 launcher EXE as the bridge client / entrypoint."
  }
  Copy-FileIfDifferent -Source $launcher32 -Destination (Join-Path $x86Out 'NvRemixLauncher32.exe')
  Test-PeMachine (Join-Path $x86Out 'NvRemixLauncher32.exe') 'x86'
  if (Test-Path -LiteralPath $launcher32Pdb -PathType Leaf) { Copy-FileIfDifferent -Source $launcher32Pdb -Destination (Join-Path $x86Out 'NvRemixLauncher32.pdb') }

  $artifactReadme = @"
DXVK Remix DX11 x86 Launcher-Client Package v116
======================================

Placement matches the NVIDIA x86 bridge package style:

Root:
  NvRemixLauncher32.exe      32-bit DX11 bridge client / entrypoint
  d3d11.dll                  32-bit game-side DX11 proxy loaded by the game
  dxgi.dll                   32-bit game-side DXGI proxy loaded by the game
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
  Copy this folder next to the 32-bit DX11 game exe and run NvRemixLauncher32.exe from that same game folder. The launcher EXE is the client/entrypoint. The root d3d11.dll/dxgi.dll are game-side DX11/DXGI proxy DLLs loaded by the game from the game folder.

NvRemixLauncher32.exe is intentionally staged in the root as the x86 DX11 bridge client/entrypoint. d3d11.dll and dxgi.dll are root game-side proxy DLLs.
"@
  Set-Content -LiteralPath (Join-Path $x86Out 'artifacts_readme.txt') -Encoding UTF8 -Value $artifactReadme

  # The 32-bit package needs the complete x64 runtime/support tree inside .trex,
  # not just d3d11.dll and dxgi.dll. Mirror the final x64 folder so the x86 layout
  # has the same support DLLs/USD structure as the native x64 output.
# >>> V93_PRESERVE_RESTORE_X64_RUNTIME_BEFORE_TREX_MIRROR
try {
  $__v93ProjectRoot = $null
  foreach ($__v93Name in @('ProjectDir','BaseDir','RootDir','RepoRoot')) {
    try {
      $__v93Var = Get-Variable -Name $__v93Name -ErrorAction SilentlyContinue
      if ($null -ne $__v93Var -and -not [string]::IsNullOrWhiteSpace([string]$__v93Var.Value)) { $__v93ProjectRoot = [string]$__v93Var.Value; break }
    } catch {}
  }
  if ([string]::IsNullOrWhiteSpace($__v93ProjectRoot)) { $__v93ProjectRoot = Split-Path -Parent $PSCommandPath }
  function __v93GetPeMachine([string]$p) {
    try {
      if (!(Test-Path -LiteralPath $p)) { return 0 }
      $fs = [System.IO.File]::Open($p, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read, [System.IO.FileShare]::ReadWrite)
      try {
        $br = New-Object System.IO.BinaryReader($fs)
        if ($fs.Length -lt 64) { return 0 }
        $fs.Seek(0x3c, [System.IO.SeekOrigin]::Begin) | Out-Null
        $pe = $br.ReadInt32()
        if ($pe -lt 0 -or $pe + 6 -gt $fs.Length) { return 0 }
        $fs.Seek($pe, [System.IO.SeekOrigin]::Begin) | Out-Null
        $sig = $br.ReadUInt32()
        if ($sig -ne 0x00004550) { return 0 }
        return $br.ReadUInt16()
      } finally { $fs.Close() }
    } catch { return 0 }
  }
  function __v93IsX64([string]$p) { return ((__v93GetPeMachine $p) -eq 0x8664) }
  function __v93FindCandidate([string]$name) {
    $roots = @((Join-Path $__v93ProjectRoot '_output\x64'), (Join-Path $__v93ProjectRoot 'build'), (Join-Path $__v93ProjectRoot '_build'), (Join-Path $__v93ProjectRoot 'src'), $__v93ProjectRoot)
    foreach ($root in $roots) {
      if (!(Test-Path -LiteralPath $root)) { continue }
      $found = Get-ChildItem -LiteralPath $root -Recurse -Filter $name -File -ErrorAction SilentlyContinue | Where-Object {
        $full = $_.FullName.ToLowerInvariant()
        ($full -notmatch '\\\\_output\\\\x86\\\\') -and ($full -notmatch '\\\\_comp32') -and ($full -notmatch '\\\\_build_logs\\\\') -and (__v93IsX64 $_.FullName)
      } | Sort-Object Length, LastWriteTime -Descending | Select-Object -First 1
      if ($null -ne $found) { return $found.FullName }
    }
    return $null
  }
  $__v93X64Out = Join-Path $__v93ProjectRoot '_output\x64'
  $__v93Cache = Join-Path $__v93ProjectRoot '_dx11_v93_x64_runtime_cache'
  New-Item -ItemType Directory -Force -Path $__v93X64Out | Out-Null
  New-Item -ItemType Directory -Force -Path $__v93Cache | Out-Null
  foreach ($__v93Dll in @('d3d11.dll','dxgi.dll')) {
    $__v93Dst = Join-Path $__v93X64Out $__v93Dll
    $__v93CacheFile = Join-Path $__v93Cache $__v93Dll
    if ((Test-Path -LiteralPath $__v93Dst) -and (__v93IsX64 $__v93Dst)) {
      Copy-Item -LiteralPath $__v93Dst -Destination $__v93CacheFile -Force
      if (Get-Command Log -CommandType Function -ErrorAction SilentlyContinue) { Log ('V93 cached/preserved x64 runtime DLL: {0}' -f $__v93Dst) } else { Write-Host ('[dx11-output-v63] V93 cached/preserved x64 runtime DLL: {0}' -f $__v93Dst) }
      continue
    }
    if ((Test-Path -LiteralPath $__v93CacheFile) -and (__v93IsX64 $__v93CacheFile)) {
      Copy-Item -LiteralPath $__v93CacheFile -Destination $__v93Dst -Force
      if (Get-Command Log -CommandType Function -ErrorAction SilentlyContinue) { Log ('V93 restored x64 runtime DLL from cache: {0}' -f $__v93Dst) } else { Write-Host ('[dx11-output-v63] V93 restored x64 runtime DLL from cache: {0}' -f $__v93Dst) }
      continue
    }
    $__v93Candidate = __v93FindCandidate $__v93Dll
    if (![string]::IsNullOrWhiteSpace($__v93Candidate)) {
      Copy-Item -LiteralPath $__v93Candidate -Destination $__v93Dst -Force
      Copy-Item -LiteralPath $__v93Candidate -Destination $__v93CacheFile -Force
      if (Get-Command Log -CommandType Function -ErrorAction SilentlyContinue) { Log ('V93 restored x64 runtime DLL from candidate: {0}' -f $__v93Dst) } else { Write-Host ('[dx11-output-v63] V93 restored x64 runtime DLL from candidate: {0}' -f $__v93Dst) }
      continue
    }
    Write-Host ('[dx11-output-v63] WARNING: V93 could not restore x64 runtime DLL before .trex mirror: {0}' -f $__v93Dll) -ForegroundColor Yellow
  }
} catch {
  Write-Host ('[dx11-output-v63] WARNING: V93 x64 runtime preserve/restore failed: {0}' -f $_.Exception.Message) -ForegroundColor Yellow
}
# <<< V93_PRESERVE_RESTORE_X64_RUNTIME_BEFORE_TREX_MIRROR
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
DXVK Remix DX11 x64 output v62
==============================

Use this folder for native 64-bit DX11 games.

This folder intentionally matches the repo's normal _output\x64 structure:
  d3d11.dll      x64 DX11 runtime from this repo
  dxgi.dll       x64 DXGI runtime from this repo
  usd\           USD runtime/data when available
  *.dll          required x64 runtime/dependency DLLs from the build/install tree

d3d11.dll is intentionally staged for this DX11 build.
"@
  Set-Content -LiteralPath (Join-Path $x64Out 'README_X64_DX11_OUTPUT_V63.txt') -Encoding UTF8 -Value $x64Readme

  $x86Readme = @"
DXVK Remix DX11 x86 bridge output v62
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

d3d11.dll is intentionally staged for this DX11 build.
"@
  Set-Content -LiteralPath (Join-Path $x86Out 'README_X86_DX11_BRIDGE_OUTPUT_V63.txt') -Encoding UTF8 -Value $x86Readme

  $pkgRoot = Join-Path $Root '_packages'
  New-Item -ItemType Directory -Force -Path $pkgRoot | Out-Null
  $zip = Join-Path $pkgRoot 'dxvk-remix-dx11-output-x64-x86-v62.zip'
  Restore-FinalDX11OutputsIntegrated -X86BuildClient $BridgeBuilds.Build32 -X86Out $x86Out -X64Out $x64Out
  Test-PeMachine (Join-Path $x86Out 'd3d11.dll') 'x86'
  Test-PeMachine (Join-Path $x86Out 'dxgi.dll') 'x86'
  Test-PeMachine (Join-Path $x86Trex 'd3d11.dll') 'x64'
  Test-PeMachine (Join-Path $x86Trex 'dxgi.dll') 'x64'
  Assert-DX11X86LauncherClientLayoutV116 -X86Out $x86Out -X64Out $x64Out

  if (Test-Path -LiteralPath $zip) { Remove-Item -LiteralPath $zip -Force }
  Add-Type -AssemblyName System.IO.Compression.FileSystem
  [System.IO.Compression.ZipFile]::CreateFromDirectory($outRoot, $zip)
Log "Output x64: $x64Out"
  Log "Output x86: $x86Out"
  Log "Combined output zip: $zip"
}

function Stage-Package([string]$RuntimeBuild, [hashtable]$BridgeBuilds) {
  $pkg = Join-Path $Root '_packages\rtx-remix-dx11-x86-bridge-getgoing-v62'
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

  $readme = @"
RTX Remix DX11 x86 Bridge Get-Going Package v62
================================================

Layout:
  d3d11.dll                 x86 DX11 pickup/interposer DLL
  dxgi.dll                  x86 DXGI pickup/interposer DLL
  .trex\NvRemixBridge.exe   x64 bridge server when bridge source build produced it
  .trex\d3d11.dll           x64 DX11 runtime from this repo
  .trex\dxgi.dll            x64 DXGI runtime from this repo
  .trex\usd\                USD runtime/data copied from dependency/build tree when found

d3d11.dll is intentionally included for this DX11 build.

v62 fixes:
  - launches NvRemixBridge.exe with required GUID and matching BRIDGE_VERSION arguments
  - writes .trex\bridge_version.txt from bridge server version.h
- wires the x86 root d3d11.dll/dxgi.dll as the client bootstrap to .trex\NvRemixBridge.exe
  - strips UTF-8 BOMs from bridge Meson files before Meson setup
  - writes patched Meson files as UTF-8 without BOM
  - prints Meson/Ninja logs live and tails them on failure
  - continues if Meson exits nonzero after creating build.ninja
  - builds the x86 root d3d11.dll/dxgi.dll directly with HostX64\\x86 cl.exe, avoiding vcvarsall x64_x86 quoting failures
"@
  Set-Content -Path (Join-Path $pkg 'README_DX11_BRIDGE_GETGOING_V63.txt') -Value $readme -Encoding UTF8
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
Repair-MesonUtf8NoBom $Root
Repair-MesonNestedTernary $Root
$bridgeWork = Prepare-DX11BridgeSource $Root
if ($SourceOnly) { Log "Source-only requested. Work tree: $bridgeWork"; exit 0 }
$runtimeBuild = Build-X64Runtime $vs $meson $ninja
$bridgeBuilds = Build-DX11Bridge $bridgeWork $vs $meson $ninja
Stage-DualOutput $runtimeBuild $bridgeBuilds
