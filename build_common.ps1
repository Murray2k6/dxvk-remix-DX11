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
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
# v6 fixed script. Expected to live in the repository root next to meson.build.
$DxvkBuildRoot = if ($PSScriptRoot) {
  [IO.Path]::GetFullPath($PSScriptRoot)
} else {
  [IO.Path]::GetFullPath((Get-Location).Path)
}
function Get-NormalizedFullPath {
  param(
    [Parameter(Mandatory)]
    [string]$Path
  )
  return [IO.Path]::GetFullPath($Path).TrimEnd([char[]]@('\', '/')).ToLowerInvariant()
}
function Add-PathEntryForCurrentProcess {
  param(
    [Parameter(Mandatory)]
    [string]$PathToAdd
  )
  if ([string]::IsNullOrWhiteSpace($PathToAdd) -or -not (Test-Path $PathToAdd)) {
    return
  }
  $normalizedPathToAdd = Get-NormalizedFullPath -Path $PathToAdd
  $currentEntries = @()
  if ($env:Path) {
    $currentEntries = $env:Path -split ';' | Where-Object { -not [string]::IsNullOrWhiteSpace($_) }
  }
  foreach ($entry in $currentEntries) {
    try {
      if ((Get-NormalizedFullPath -Path $entry) -eq $normalizedPathToAdd) {
        return
      }
    } catch {
    }
  }
  $env:Path = "$PathToAdd;$env:Path"
  Write-Host "[build] Added Python Scripts folder to this PowerShell session PATH: $PathToAdd" -ForegroundColor Yellow
}
function Add-PythonScriptDirsToPath {
  $candidateDirs = New-Object System.Collections.Generic.List[string]
  if ($env:APPDATA) {
    $roamingPythonRoot = Join-Path $env:APPDATA 'Python'
    if (Test-Path $roamingPythonRoot) {
      Get-ChildItem -Path $roamingPythonRoot -Directory -Filter 'Python*' -ErrorAction SilentlyContinue | ForEach-Object {
        $candidateDirs.Add((Join-Path $_.FullName 'Scripts'))
      }
    }
  }
  if ($env:LOCALAPPDATA) {
    $localProgramsPythonRoot = Join-Path $env:LOCALAPPDATA 'Programs\Python'
    if (Test-Path $localProgramsPythonRoot) {
      Get-ChildItem -Path $localProgramsPythonRoot -Directory -Filter 'Python*' -ErrorAction SilentlyContinue | ForEach-Object {
        $candidateDirs.Add((Join-Path $_.FullName 'Scripts'))
      }
    }
  }
  foreach ($pyName in @('py', 'python')) {
    $pyCmd = Get-Command $pyName -ErrorAction SilentlyContinue
    if (-not $pyCmd) { continue }
    try {
      $scriptsDir = & $pyCmd.Path -c "import sysconfig; print(sysconfig.get_path('scripts') or '')" 2>$null
      if ($LASTEXITCODE -eq 0 -and -not [string]::IsNullOrWhiteSpace($scriptsDir)) {
        $candidateDirs.Add([string]$scriptsDir)
      }
    } catch {
    }
  }
  $seen = @{}
  foreach ($dir in $candidateDirs) {
    if ([string]::IsNullOrWhiteSpace($dir)) { continue }
    try {
      $full = [IO.Path]::GetFullPath($dir)
    } catch {
      continue
    }
    $key = $full.ToLowerInvariant()
    if ($seen.ContainsKey($key)) { continue }
    $seen[$key] = $true
    Add-PathEntryForCurrentProcess -PathToAdd $full
  }
}
function Resolve-RequiredCommand {
  param(
    [Parameter(Mandatory)]
    [string]$Name,
    [string]$InstallHint = '',
    # Optional Python module fallback. Example: mesonbuild.mesonmain lets the
    # script run Meson even when meson.exe was not put on PATH by pip.
    [string]$PythonModule = ''
  )
  Add-PythonScriptDirsToPath
  $cmd = Get-Command $Name -ErrorAction SilentlyContinue
  if ($cmd) {
    return [pscustomobject]@{
      FilePath = $cmd.Path
      ArgumentsPrefix = @()
      DisplayName = $cmd.Path
    }
  }
  if (-not [string]::IsNullOrWhiteSpace($PythonModule)) {
    foreach ($pyName in @('py', 'python')) {
      $pyCmd = Get-Command $pyName -ErrorAction SilentlyContinue
      if (-not $pyCmd) { continue }
      try {
        & $pyCmd.Path -m $PythonModule --version *> $null
        if ($LASTEXITCODE -eq 0) {
          $display = ('{0} -m {1}' -f $pyCmd.Path, $PythonModule)
          Write-Host "[build] Using Python module fallback for $Name`: $display" -ForegroundColor Yellow
          return [pscustomobject]@{
            FilePath = $pyCmd.Path
            ArgumentsPrefix = @('-m', $PythonModule)
            DisplayName = $display
          }
        }
      } catch {
      }
    }
  }
  $msg = "Required command '$Name' was not found in PATH or Python Scripts folders."
  if (-not [string]::IsNullOrWhiteSpace($InstallHint)) {
    $msg += " $InstallHint"
  }
  Write-Error $msg -ErrorAction Stop
}
function Invoke-CheckedNative {
  param(
    [Parameter(Mandatory)]
    [string]$FilePath,
    [Parameter(Mandatory)]
    [string[]]$Arguments,
    [string[]]$ArgumentsPrefix = @(),
    [Parameter(Mandatory)]
    [string]$FailureMessage,
    [string]$WorkingDirectory = $DxvkBuildRoot
  )
  $fullArguments = @()
  if ($ArgumentsPrefix) { $fullArguments += $ArgumentsPrefix }
  if ($Arguments) { $fullArguments += $Arguments }
  Push-Location $WorkingDirectory
  try {
    & $FilePath @fullArguments
    $exitCode = if ($null -eq $LASTEXITCODE) { 0 } else { $LASTEXITCODE }
    if ($exitCode -ne 0) {
      $displayCommand = ('{0} {1}' -f $FilePath, ($fullArguments -join ' '))
      throw ("{0} (exit code {1})`nCommand: {2}" -f $FailureMessage, $exitCode, $displayCommand)
    }
  } finally {
    Pop-Location
  }
}
function Get-MesonOptionNames {
  param(
    [Parameter(Mandatory)]
    [string]$SourceDir
  )
  $optionFile = Join-Path $SourceDir 'meson_options.txt'
  if (-not (Test-Path $optionFile)) {
    Write-Host "[build] WARNING: meson_options.txt was not found. User Meson -D options will be skipped." -ForegroundColor Yellow
    return @()
  }
  $text = Get-Content -Path $optionFile -Raw
  $matches = [regex]::Matches($text, 'option\(\s*[''"]([^''"]+)[''"]')
  $names = @()
  foreach ($m in $matches) {
    $names += $m.Groups[1].Value
  }
  return $names
}
function New-MesonOptionArg {
  param(
    [Parameter(Mandatory)]
    [string[]]$AvailableOptions,
    [Parameter(Mandatory)]
    [string]$Name,
    [Parameter(Mandatory)]
    [string]$Value
  )
  if ($AvailableOptions -contains $Name) {
    return "-D$Name=$Value"
  }
  Write-Host "[build] WARNING: Meson option '$Name' is not declared in meson_options.txt; not passing -D$Name=$Value" -ForegroundColor Yellow
  return $null
}
function Get-ConfiguredMesonSourceDir {
  param(
    [Parameter(Mandatory)]
    [string]$BuildDir
  )
  $mesonInfoPath = Join-Path $BuildDir 'meson-info\meson-info.json'
  if (Test-Path $mesonInfoPath) {
    try {
      $mesonInfo = Get-Content $mesonInfoPath -Raw | ConvertFrom-Json
      if ($mesonInfo.directories.source) {
        return $mesonInfo.directories.source
      }
    } catch {
      Write-Host "[build] WARNING: Could not read $mesonInfoPath; build directory will be regenerated." -ForegroundColor Yellow
    }
  }
  $buildNinjaPath = Join-Path $BuildDir 'build.ninja'
  if (Test-Path $buildNinjaPath) {
    $buildNinja = Get-Content $buildNinjaPath -Raw
    $regexes = @(
      [regex]'--internal regenerate\s+"([^"]+)"\s+"\."',
      [regex]'--internal regenerate\s+([^\s]+)\s+\.'
    )
    foreach ($regex in $regexes) {
      $match = $regex.Match($buildNinja)
      if ($match.Success) {
        return $match.Groups[1].Value
      }
    }
  }
  return $null
}
function Repair-FutureFileTimestamps {
  param(
    [Parameter(Mandatory)]
    [string]$SourceDir,
    [string]$BuildDir = ''
  )
  $now = Get-Date
  $futureLimit = $now.AddSeconds(2)
  $safeTime = $now.AddSeconds(-60)
  $fixedCount = 0
  $rootsToScan = New-Object System.Collections.Generic.List[string]
  if (Test-Path $SourceDir) {
    $rootsToScan.Add([IO.Path]::GetFullPath($SourceDir))
  }
  if (-not [string]::IsNullOrWhiteSpace($BuildDir) -and (Test-Path $BuildDir)) {
    $rootsToScan.Add([IO.Path]::GetFullPath($BuildDir))
  }
  $seenFiles = @{}
  $sourceNorm = if (Test-Path $SourceDir) { Get-NormalizedFullPath -Path $SourceDir } else { '' }
  foreach ($root in $rootsToScan) {
    if ([string]::IsNullOrWhiteSpace($root) -or -not (Test-Path $root)) { continue }
    $rootNorm = Get-NormalizedFullPath -Path $root
    $rootIsSourceRoot = ($sourceNorm -and ($rootNorm -eq $sourceNorm))
    # Do not scan .git or normal output folders from the source-root pass.
    # If BuildDir is supplied separately, it is scanned directly so Meson/Ninja
    # metadata can also be repaired after a bad ZIP extraction or clock change.
    $files = @()
    try {
      $files = Get-ChildItem -LiteralPath $root -Recurse -Force -File -ErrorAction SilentlyContinue |
        Where-Object {
          $full = $_.FullName
          if ($full -like '*\.git\*') { return $false }
          if ($full -like '*\_output\*') { return $false }
          if ($rootIsSourceRoot) {
            if ($full -like '*\_Comp64Debug\*') { return $false }
            if ($full -like '*\_Comp64DebugOptimized\*') { return $false }
            if ($full -like '*\_Comp64Release\*') { return $false }
            if ($full -like '*\_Comp32Debug\*') { return $false }
            if ($full -like '*\_Comp32DebugOptimized\*') { return $false }
            if ($full -like '*\_Comp32Release\*') { return $false }
          }
          return $true
        }
    } catch {
      Write-Host "[build] WARNING: Could not fully scan '$root' for future timestamps: $($_.Exception.Message)" -ForegroundColor Yellow
      $files = @()
    }
    foreach ($file in $files) {
      if (-not $file) { continue }
      $key = $file.FullName.ToLowerInvariant()
      if ($seenFiles.ContainsKey($key)) { continue }
      $seenFiles[$key] = $true
      try {
        $item = Get-Item -LiteralPath $file.FullName -Force
        if (($item.LastWriteTime -gt $futureLimit) -or ($item.LastAccessTime -gt $futureLimit) -or ($item.CreationTime -gt $futureLimit)) {
          $deltaSeconds = [math]::Round(($item.LastWriteTime - $now).TotalSeconds, 3)
          Write-Host "[build] Fixing future timestamp (+$deltaSeconds sec): $($item.FullName)" -ForegroundColor Yellow
          $item.CreationTime = $safeTime
          $item.LastWriteTime = $safeTime
          $item.LastAccessTime = $safeTime
          $fixedCount++
        }
      } catch {
        Write-Host "[build] WARNING: Could not repair timestamp for '$($file.FullName)': $($_.Exception.Message)" -ForegroundColor Yellow
      }
    }
  }
  if ($fixedCount -gt 0) {
    Write-Host "[build] Repaired $fixedCount future file timestamp(s) before Meson/Ninja setup." -ForegroundColor Yellow
  }
}
function Repair-StaleBuildDirectory {
  param(
    [Parameter(Mandatory)]
    [string]$SourceDir,
    [Parameter(Mandatory)]
    [string]$BuildDir
  )
  if (-not (Test-Path $BuildDir)) {
    return $false
  }
  $configuredSourceDir = Get-ConfiguredMesonSourceDir -BuildDir $BuildDir
  if ([string]::IsNullOrWhiteSpace($configuredSourceDir)) {
    Write-Host "[build] Removing partial or unreadable Meson build directory: $BuildDir" -ForegroundColor Yellow
    Remove-Item $BuildDir -Recurse -Force
    return $false
  }
  $normalizedSourceDir = Get-NormalizedFullPath -Path $SourceDir
  $normalizedConfiguredSourceDir = Get-NormalizedFullPath -Path $configuredSourceDir
  if ($normalizedSourceDir -ne $normalizedConfiguredSourceDir) {
    Write-Host "[build] Removing stale Meson build directory '$BuildDir' configured for '$configuredSourceDir'" -ForegroundColor Yellow
    Remove-Item $BuildDir -Recurse -Force
    return $false
  }
  return $true
}
# Find vswhere (installed with recent Visual Studio versions).
if ($vsWhereCommand = Get-Command 'vswhere.exe' -ErrorAction SilentlyContinue) {
  $vsWhere = $vsWhereCommand.Path
} elseif (Test-Path "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe") {
  $vsWhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
} else {
  Write-Error "vswhere not found. Install Visual Studio 2019/2022 with C++ build tools, or add vswhere.exe to PATH." -ErrorAction Stop
}
Write-Host "[build] vswhere found at: $vsWhere" -ForegroundColor Yellow
# Get path to Visual Studio installation using vswhere.
$vsWherePrimaryArgs = @(
  '-latest',
  '-version', '[16.0,18.0)',
  '-products', '*',
  '-requires', 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64',
  '-property', 'installationPath'
)
$vsPath = (& $vsWhere @vsWherePrimaryArgs 2>$null | Select-Object -First 1)
if ([string]::IsNullOrWhiteSpace([string]$vsPath)) {
  $vsWhereFallbackArgs = @(
    '-latest',
    '-version', '[16.0,18.0)',
    '-products', '*',
    '-requires', 'Microsoft.Component.MSBuild',
    '-property', 'installationPath'
  )
  $vsPath = (& $vsWhere @vsWhereFallbackArgs 2>$null | Select-Object -First 1)
}
if (-not [string]::IsNullOrWhiteSpace([string]$vsPath)) {
  $vsPath = ([string]$vsPath).Trim()
}
if ([string]::IsNullOrWhiteSpace([string]$vsPath)) {
  Write-Error "Failed to find Visual Studio 2019/2022 with MSVC C++ build tools. Aborting." -ErrorAction Stop
}
Write-Host "[build] Using Visual Studio installation at: ${vsPath}" -ForegroundColor Yellow
# Capture the environment before selecting a compiler architecture. Each build can
# switch between x64 and x86 without reusing the previous architecture's CL/LIB/INCLUDE.
$script:DxvkInitialEnvironment = @{}
Get-ChildItem Env: | ForEach-Object { $script:DxvkInitialEnvironment[$_.Name] = $_.Value }

function Restore-DxvkInitialEnvironment {
  $currentNames = @(Get-ChildItem Env: | ForEach-Object { $_.Name })
  foreach ($name in $currentNames) {
    if (-not $script:DxvkInitialEnvironment.ContainsKey($name)) {
      Remove-Item -Path "ENV:\$name" -ErrorAction SilentlyContinue
    }
  }
  foreach ($entry in $script:DxvkInitialEnvironment.GetEnumerator()) {
    Set-Item -Force -Path "ENV:\$($entry.Key)" -Value $entry.Value
  }
}

function Set-VisualStudioBuildEnvironment {
  param(
    [Parameter(Mandatory)]
    [ValidateSet('x64', 'x86')]
    [string]$Architecture
  )

  Restore-DxvkInitialEnvironment

  $targetArch = if ($Architecture -eq 'x86') { 'x86' } else { 'x64' }
  $hostArch = 'Hostx64'

  $vcRoot = Join-Path $vsPath 'VC\Tools\MSVC'
  if (-not (Test-Path -LiteralPath $vcRoot)) {
    Write-Error ('MSVC tools folder was not found: {0}. Install the MSVC C++ x64/x86 build tools workload.' -f $vcRoot) -ErrorAction Stop
  }

  $vcCandidates = @(Get-ChildItem -LiteralPath $vcRoot -Directory -ErrorAction SilentlyContinue | Where-Object {
    Test-Path -LiteralPath (Join-Path $_.FullName ('bin\{0}\{1}\cl.exe' -f $hostArch, $targetArch))
  } | Sort-Object Name -Descending)

  if ($vcCandidates.Count -eq 0) {
    Write-Error ('No MSVC {0}\{1} compiler was found under {2}. Install MSVC x86/x64 build tools.' -f $hostArch, $targetArch, $vcRoot) -ErrorAction Stop
  }

  $vcToolsDir = $vcCandidates[0].FullName
  $vcBin = Join-Path $vcToolsDir ('bin\{0}\{1}' -f $hostArch, $targetArch)

  $programFilesX86 = [Environment]::GetFolderPath('ProgramFilesX86')
  $sdkRoot = $null
  if (-not [string]::IsNullOrWhiteSpace($env:WindowsSdkDir) -and (Test-Path -LiteralPath $env:WindowsSdkDir)) {
    $sdkRoot = $env:WindowsSdkDir
  }
  if ([string]::IsNullOrWhiteSpace($sdkRoot)) {
    $candidateSdk = Join-Path $programFilesX86 'Windows Kits\10'
    if (Test-Path -LiteralPath $candidateSdk) { $sdkRoot = $candidateSdk }
  }
  if ([string]::IsNullOrWhiteSpace($sdkRoot)) {
    Write-Error 'Windows 10/11 SDK root was not found. Install Windows SDK with Visual Studio Installer.' -ErrorAction Stop
  }

  $sdkRoot = (Resolve-Path -LiteralPath $sdkRoot).ProviderPath
  $sdkLibRoot = Join-Path $sdkRoot 'Lib'
  $sdkIncludeRoot = Join-Path $sdkRoot 'Include'
  $sdkBinRoot = Join-Path $sdkRoot 'bin'

  $sdkVersions = @(Get-ChildItem -LiteralPath $sdkLibRoot -Directory -ErrorAction SilentlyContinue | Where-Object {
    (Test-Path -LiteralPath (Join-Path $_.FullName ('ucrt\{0}' -f $targetArch))) -and
    (Test-Path -LiteralPath (Join-Path $_.FullName ('um\{0}' -f $targetArch)))
  } | Sort-Object Name -Descending)

  if ($sdkVersions.Count -eq 0) {
    Write-Error ('No Windows SDK library version for {0} was found under {1}.' -f $targetArch, $sdkLibRoot) -ErrorAction Stop
  }

  $sdkVer = $sdkVersions[0].Name

  function Add-ExistingDirectory {
    param(
      [Parameter(Mandatory)] [object]$List,
      [string]$Path
    )
    if (-not [string]::IsNullOrWhiteSpace($Path) -and (Test-Path -LiteralPath $Path)) {
      [void]$List.Add((Resolve-Path -LiteralPath $Path).ProviderPath)
    }
  }

  $pathList = New-Object System.Collections.ArrayList
  Add-ExistingDirectory -List $pathList -Path $vcBin
  Add-ExistingDirectory -List $pathList -Path (Join-Path $sdkBinRoot (Join-Path $sdkVer 'x64'))
  Add-ExistingDirectory -List $pathList -Path (Join-Path $sdkBinRoot (Join-Path $sdkVer $targetArch))
  if (-not [string]::IsNullOrWhiteSpace($env:PATH)) {
    foreach ($p in ($env:PATH -split ';')) {
      if (-not [string]::IsNullOrWhiteSpace($p)) { [void]$pathList.Add($p) }
    }
  }

  $includeList = New-Object System.Collections.ArrayList
  Add-ExistingDirectory -List $includeList -Path (Join-Path $vcToolsDir 'include')
  foreach ($part in @('ucrt', 'shared', 'um', 'winrt', 'cppwinrt')) {
    Add-ExistingDirectory -List $includeList -Path (Join-Path $sdkIncludeRoot (Join-Path $sdkVer $part))
  }

  $libList = New-Object System.Collections.ArrayList
  Add-ExistingDirectory -List $libList -Path (Join-Path $vcToolsDir (Join-Path 'lib' $targetArch))
  Add-ExistingDirectory -List $libList -Path (Join-Path $sdkLibRoot (Join-Path $sdkVer (Join-Path 'ucrt' $targetArch)))
  Add-ExistingDirectory -List $libList -Path (Join-Path $sdkLibRoot (Join-Path $sdkVer (Join-Path 'um' $targetArch)))

  $libPathList = New-Object System.Collections.ArrayList
  Add-ExistingDirectory -List $libPathList -Path (Join-Path $vcToolsDir (Join-Path 'lib' $targetArch))
  Add-ExistingDirectory -List $libPathList -Path (Join-Path $sdkLibRoot (Join-Path $sdkVer (Join-Path 'ucrt' $targetArch)))
  Add-ExistingDirectory -List $libPathList -Path (Join-Path $sdkLibRoot (Join-Path $sdkVer (Join-Path 'um' $targetArch)))

  if ($includeList.Count -eq 0 -or $libList.Count -eq 0) {
    Write-Error ('MSVC/SDK INCLUDE or LIB paths were empty for {0}. MSVC={1} SDK={2}' -f $targetArch, $vcToolsDir, $sdkRoot) -ErrorAction Stop
  }

  $env:PATH = ($pathList.ToArray() -join ';')
  $env:INCLUDE = ($includeList.ToArray() -join ';')
  $env:LIB = ($libList.ToArray() -join ';')
  $env:LIBPATH = ($libPathList.ToArray() -join ';')
  $env:DXVK_BUILD_ARCH = $Architecture
  $env:VSCMD_ARG_HOST_ARCH = 'x64'
  $env:VSCMD_ARG_TGT_ARCH = $targetArch
  $env:Platform = if ($targetArch -eq 'x86') { 'Win32' } else { 'x64' }
  $env:VCToolsInstallDir = ($vcToolsDir.TrimEnd('\') + '\')
  $env:VCToolsVersion = Split-Path $vcToolsDir -Leaf
  $env:WindowsSdkDir = ($sdkRoot.TrimEnd('\') + '\')
  $env:WindowsSDKVersion = ($sdkVer.TrimEnd('\') + '\')
  $env:WindowsSdkVerBinPath = (Join-Path $sdkBinRoot ($sdkVer + '\'))
  $env:UCRTVersion = $sdkVer
  $env:UniversalCRTSdkDir = ($sdkRoot.TrimEnd('\') + '\')

  Write-Host ('[build] MSVC environment set directly for {0}: MSVC {1}, SDK {2}' -f $Architecture, $env:VCToolsVersion, $sdkVer) -ForegroundColor Yellow
}

function PerformBuild {
  param(
    [ValidateSet('x64', 'x86')]
    [string]$Architecture = 'x64',
    [Parameter(Mandatory)]
    [string]$Backend,
    [Parameter(Mandatory)]
    [ValidateSet('debug', 'debugoptimized', 'release')]
    [string]$BuildFlavour,
    [Parameter(Mandatory)]
    [string]$BuildSubDir,
    [Parameter(Mandatory)]
    [ValidateSet('true', 'false')]
    [string]$EnableTracy,
    [string]$BuildTarget,
    [string[]]$InstallTags,
    [bool]$ConfigureOnly = $false,
    [bool]$ShadersOnly = $false
  )
  if (-not $InstallTags -or $InstallTags.Count -eq 0) {
    $InstallTags = @('output')
  }
  $SourceDir = [IO.Path]::GetFullPath($DxvkBuildRoot)
  $OutputDir = [IO.Path]::Combine($SourceDir, '_output', $Architecture)
  $BuildDir = [IO.Path]::Combine($SourceDir, $BuildSubDir)
  Set-VisualStudioBuildEnvironment -Architecture $Architecture
  if (-not (Test-Path (Join-Path $SourceDir 'meson.build'))) {
    Write-Error "meson.build was not found at '$SourceDir'. Put build_common.ps1 and build_dxvk_all_ninja.ps1 in the repository root next to meson.build, then run build_dxvk_all_ninja.ps1 from there." -ErrorAction Stop
  }
  Repair-FutureFileTimestamps -SourceDir $SourceDir -BuildDir $BuildDir
  $mesonCommand = Resolve-RequiredCommand -Name 'meson' -PythonModule 'mesonbuild.mesonmain' -InstallHint 'Install Meson with: py -m pip install --user meson'
  if ($Backend -eq 'ninja') {
    [void](Resolve-RequiredCommand -Name 'ninja' -InstallHint 'Install Ninja with: py -m pip install --user ninja')
  }
  $availableOptions = Get-MesonOptionNames -SourceDir $SourceDir
  Write-Host "[build] Starting $Architecture build for $BuildFlavour..." -ForegroundColor Cyan
  Write-Host "[build] Source directory: $SourceDir" -ForegroundColor DarkGray
  Write-Host "[build] Build directory:  $BuildDir" -ForegroundColor DarkGray
  $buildDirMatchesCurrentSource = Repair-StaleBuildDirectory -SourceDir $SourceDir -BuildDir $BuildDir
  $mesonArgs = @('setup')
  if ($buildDirMatchesCurrentSource) {
    $mesonArgs += '--reconfigure'
  }
  $mesonArgs += @(
    '--buildtype', $BuildFlavour,
    '--backend', $Backend
  )
  foreach ($opt in @(
    @{ Name = 'enable_dxgi';   Value = 'true' },
    @{ Name = 'enable_d3d11';  Value = 'true' },
    @{ Name = 'enable_tests';  Value = 'false' },
    @{ Name = 'enable_tracy';  Value = $EnableTracy }
  )) {
    $arg = New-MesonOptionArg -AvailableOptions $availableOptions -Name $opt.Name -Value $opt.Value
    if ($arg) { $mesonArgs += $arg }
  }
  if ($ShadersOnly) {
    $arg = New-MesonOptionArg -AvailableOptions $availableOptions -Name 'download_apics' -Value 'false'
    if ($arg) { $mesonArgs += $arg }
  }
  $mesonArgs += $BuildDir
  if (-not $buildDirMatchesCurrentSource) {
    $mesonArgs += $SourceDir
  }
  Write-Host "[build] Running Meson setup..." -ForegroundColor Cyan
  Invoke-CheckedNative -FilePath $mesonCommand.FilePath -ArgumentsPrefix $mesonCommand.ArgumentsPrefix -Arguments $mesonArgs -FailureMessage 'Failed to run Meson setup' -WorkingDirectory $SourceDir
  if ($ShadersOnly) {
    Write-Host "[build] Building shaders only..." -ForegroundColor Cyan
    Invoke-CheckedNative -FilePath $mesonCommand.FilePath -ArgumentsPrefix $mesonCommand.ArgumentsPrefix -Arguments @('compile', '-C', $BuildDir, 'rtx_shaders') -FailureMessage 'Failed to build shaders' -WorkingDirectory $SourceDir
    return
  }
  if (-not $ConfigureOnly) {
    Write-Host "[build] Compiling..." -ForegroundColor Cyan
    $compileArgs = @('compile', '-C', $BuildDir)
    if (-not [string]::IsNullOrWhiteSpace($BuildTarget)) {
      $compileArgs += $BuildTarget
    }
    $compileArgs += '-v'
    Invoke-CheckedNative -FilePath $mesonCommand.FilePath -ArgumentsPrefix $mesonCommand.ArgumentsPrefix -Arguments $compileArgs -FailureMessage 'Failed to run build step' -WorkingDirectory $SourceDir
    $tagList = $InstallTags -join ','
    Write-Host "[build] Installing tag(s) '$tagList' to $OutputDir..." -ForegroundColor Cyan
    Invoke-CheckedNative -FilePath $mesonCommand.FilePath -ArgumentsPrefix $mesonCommand.ArgumentsPrefix -Arguments @('install', '-C', $BuildDir, '--tags', $tagList) -FailureMessage 'Failed to run install step' -WorkingDirectory $SourceDir
    # Deploy the NGX runtime DLLs (DLSS / Ray Reconstruction / Frame
    # Generation) next to every installed/built d3d11.dll. NGX DLLs cannot
    # cross the process-bitness boundary - a 32-bit game process loads 32-bit
    # DLLs, a 64-bit process loads 64-bit ones - so each architecture needs
    # its own NGX binaries. The hdremix runtime ships 64-bit NGX; if you have
    # 32-bit NGX DLLs for x86 builds, drop them in nv-private\hdremix\bin\release-x86
    # (or set DXVK_NGX_SOURCE_X86) and they will be deployed for x86 builds.
    if ($Architecture -eq 'x86') {
      $ngxSource = $env:DXVK_NGX_SOURCE_X86
      if ([string]::IsNullOrWhiteSpace($ngxSource)) {
        $ngxSource = Join-Path $SourceDir 'nv-private\hdremix\bin\release-x86'
      }
    } else {
      $ngxSource = $env:DXVK_NGX_SOURCE_X64
      if ([string]::IsNullOrWhiteSpace($ngxSource)) {
        $ngxSource = Join-Path $SourceDir 'nv-private\hdremix\bin\release'
      }
    }
    if (Test-Path $ngxSource) {
      $ngxDlls = Get-ChildItem -Path $ngxSource -Filter 'nvngx_*.dll' -File
      $ngxTargets = @()
      foreach ($searchRoot in @($OutputDir, $BuildDir)) {
        if ($searchRoot -and (Test-Path $searchRoot)) {
          $ngxTargets += Get-ChildItem -Path $searchRoot -Recurse -Filter 'd3d11.dll' -File -ErrorAction SilentlyContinue
        }
      }
      foreach ($t in $ngxTargets) {
        foreach ($dll in $ngxDlls) {
          Copy-Item -Path $dll.FullName -Destination $t.DirectoryName -Force
        }
      }
      if ($ngxTargets.Count -gt 0) {
        Write-Host ("[build] Deployed {0} NGX DLLs next to {1} d3d11.dll location(s)" -f $ngxDlls.Count, $ngxTargets.Count) -ForegroundColor Cyan
      }
    } else {
      if ($Architecture -eq 'x86') {
        Write-Host "[build] NGX runtime DLLs not found at $ngxSource - DLSS unavailable for this x86 build. Provide 32-bit NGX DLLs there (or via DXVK_NGX_SOURCE_X86) to enable it; the renderer otherwise falls back to XeSS/TAA-U." -ForegroundColor Yellow
      } else {
        Write-Host "[build] WARNING: NGX runtime DLLs not found at $ngxSource - DLSS will be unavailable in deployed builds." -ForegroundColor Yellow
      }
    }
    Write-Host "[build] Build completed successfully for $Architecture $BuildFlavour" -ForegroundColor Green
  } else {
    Write-Host "[build] Configuration completed for $Architecture $BuildFlavour (no build performed)" -ForegroundColor Green
  }
}
