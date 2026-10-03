[CmdletBinding()]
param(
  [ValidateSet('debug', 'debugoptimized', 'release')][string]$BuildFlavour = 'release',
  # Tracy is on by default: every runtime fix is measured with it, and a
  # reconfigure with 'false' silently strips it from the existing build.
  [ValidateSet('true', 'false')][string]$EnableTracy = 'true',
  [ValidateRange(1, 256)][int]$Jobs = 6,
  [string]$PythonExecutable = '',
  [string]$OutputRoot = '_output',
  [switch]$NoDepsFetch,
  [switch]$ConfigureOnly,
  [switch]$RuntimeOnly,
  [switch]$BridgeOnly,
  [switch]$StageOnly,
  [switch]$NoStage,
  [switch]$SkipZip
)

# Build the checked-in sources directly. Builds must never regenerate or patch
# implementation files: those edits invalidate incremental builds and overwrite
# fixes made by a developer between invocations.
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$repoRoot = $PSScriptRoot
if ($RuntimeOnly -and $BridgeOnly) { throw 'RuntimeOnly and BridgeOnly are mutually exclusive.' }
if ($StageOnly -and ($ConfigureOnly -or $NoStage -or $BridgeOnly)) { throw 'StageOnly cannot be combined with ConfigureOnly, NoStage, or BridgeOnly.' }
. (Join-Path $repoRoot 'build_common.ps1')

function Invoke-BuildTool {
  param([string]$File, [string[]]$ToolArgs, [string]$Directory, [string]$Label)
  $logDirectory = Join-Path $repoRoot '_build_logs'
  New-Item -ItemType Directory -Force -Path $logDirectory | Out-Null
  $logFile = Join-Path $logDirectory ($Label + '.log')
  $start = [Diagnostics.ProcessStartInfo]::new()
  $start.FileName = $File
  $start.Arguments = ($ToolArgs | ForEach-Object { Quote-NativeArgument $_ }) -join ' '
  $start.WorkingDirectory = $Directory
  $start.UseShellExecute = $false
  $start.CreateNoWindow = $true
  $start.RedirectStandardOutput = $true
  $start.RedirectStandardError = $true
  # Rebuild a case-insensitive environment block. Otherwise duplicate PATH/Path
  # entries inherited from a launcher make Python see the stale compiler path.
  $start.EnvironmentVariables.Clear()
  Get-ChildItem Env: | ForEach-Object { $start.EnvironmentVariables[$_.Name] = $_.Value }
  $process = [Diagnostics.Process]::new()
  $process.StartInfo = $start
  $logWriter = [IO.StreamWriter]::new($logFile, $false)
  try {
    [void]$process.Start()
    $stdout = $process.StandardOutput.ReadLineAsync()
    $stderr = $process.StandardError.ReadLineAsync()
    while ($null -ne $stdout -or $null -ne $stderr) {
      if ($null -ne $stdout -and $stdout.IsCompleted) {
        $line = $stdout.GetAwaiter().GetResult()
        if ($null -eq $line) { $stdout = $null } else {
          Write-Host $line
          $logWriter.WriteLine($line)
          $stdout = $process.StandardOutput.ReadLineAsync()
        }
      }
      if ($null -ne $stderr -and $stderr.IsCompleted) {
        $line = $stderr.GetAwaiter().GetResult()
        if ($null -eq $line) { $stderr = $null } else {
          Write-Host $line
          $logWriter.WriteLine($line)
          $stderr = $process.StandardError.ReadLineAsync()
        }
      }
      $logWriter.Flush()
      Start-Sleep -Milliseconds 10
    }
    $process.WaitForExit()
    if ($process.ExitCode -ne 0) { throw "$Label failed with exit code $($process.ExitCode). See $logFile" }
  } finally { $logWriter.Dispose(); $process.Dispose() }
}

function Resolve-BuildTools {
  # Prefer the selected interpreter with local Meson modules over pip launcher
  # executables, whose shebang can name an interpreter from another computer.
  $pythonPath = $PythonExecutable
  if ([string]::IsNullOrWhiteSpace($pythonPath)) {
    $python = Get-Command python.exe -ErrorAction SilentlyContinue
    if ($python) { $pythonPath = $python.Path }
  }
  if ([string]::IsNullOrWhiteSpace($pythonPath)) {
    $bundledPython = Join-Path $repoRoot 'src\usd-plugins\_external\python\python.exe'
    if (Test-Path -LiteralPath $bundledPython) { $pythonPath = $bundledPython }
  }
  if ([string]::IsNullOrWhiteSpace($pythonPath) -or -not (Test-Path -LiteralPath $pythonPath -PathType Leaf)) {
    throw 'Python 3.10 or newer is required. Install it on PATH or pass -PythonExecutable.'
  }
  foreach ($modulePath in @('build_deps\python', '.build_deps\python')) {
    $candidate = Join-Path $repoRoot $modulePath
    if (Test-Path -LiteralPath (Join-Path $candidate 'mesonbuild')) {
      $env:PYTHONPATH = $candidate + [IO.Path]::PathSeparator + $env:PYTHONPATH
      break
    }
  }
  Set-DxvkPreferredNinjaEnvironment -VisualStudioPath $vsPath
  return $pythonPath
}

function Build-SourceTree {
  param([string]$Source, [string]$BuildDir, [string]$Architecture, [string]$Label, [string[]]$Options)
  Set-VisualStudioBuildEnvironment -Architecture $Architecture
  $python = Resolve-BuildTools
  $setupArgs = @('-m', 'mesonbuild.mesonmain', 'setup', '--backend=ninja', "--buildtype=$BuildFlavour")
  if (Test-Path -LiteralPath (Join-Path $BuildDir 'meson-private\coredata.dat')) {
    $infoPath = Join-Path $BuildDir 'meson-info\meson-info.json'
    $relocated = $false
    if (Test-Path -LiteralPath $infoPath) {
      $info = Get-Content -LiteralPath $infoPath -Raw | ConvertFrom-Json
      $relocated = [IO.Path]::GetFullPath($info.directories.source) -ne [IO.Path]::GetFullPath($Source) -or
                   [IO.Path]::GetFullPath($info.directories.build) -ne [IO.Path]::GetFullPath($BuildDir)
    }
    if ($relocated) {
      $resolvedBuild = [IO.Path]::GetFullPath($BuildDir)
      $workspacePrefix = [IO.Path]::GetFullPath($repoRoot).TrimEnd('\') + '\'
      if (-not $resolvedBuild.StartsWith($workspacePrefix, [StringComparison]::OrdinalIgnoreCase) -or
          [IO.Path]::GetFileName($resolvedBuild) -notmatch '^_Comp(32|64)(debug|debugoptimized|release)$') {
        throw "Refusing to reset relocated build metadata outside a managed build directory: $resolvedBuild"
      }
      Write-Host "[build] Recreating relocated Meson build: $resolvedBuild"
      $setupArgs += '--wipe'
    } else {
      $setupArgs += '--reconfigure'
    }
  }
  $setupArgs += $Options
  $setupArgs += @($BuildDir, $Source)
  Invoke-BuildTool $python $setupArgs $Source ($Label + '-configure')
  if (-not $ConfigureOnly) {
    Invoke-BuildTool $python @('-m', 'mesonbuild.mesonmain', 'compile', '-C', $BuildDir, '-j', "$Jobs") $Source ($Label + '-compile')
    # Shader builds copy headers shared with C++ (rtx_shaders/*.h) during the
    # compile, after ninja has already decided which C++ objects are stale.
    # A second pass picks those up; it is a no-op when nothing changed.
    Invoke-BuildTool $python @('-m', 'mesonbuild.mesonmain', 'compile', '-C', $BuildDir, '-j', "$Jobs") $Source ($Label + '-compile-pass2')
  }
  return $python
}

function Assert-PeMachine {
  param([string]$Path, [ValidateSet('x86', 'x64')][string]$Architecture)
  if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { throw "Required build output is missing: $Path" }
  $stream = [IO.File]::OpenRead($Path)
  $reader = [IO.BinaryReader]::new($stream)
  try {
    if ($reader.ReadUInt16() -ne 0x5a4d) { throw "Invalid PE file: $Path" }
    $stream.Position = 0x3c
    $offset = $reader.ReadUInt32()
    $stream.Position = $offset
    if ($reader.ReadUInt32() -ne 0x4550) { throw "Invalid PE signature: $Path" }
    $machine = $reader.ReadUInt16()
    $expected = if ($Architecture -eq 'x64') { 0x8664 } else { 0x14c }
    if ($machine -ne $expected) { throw "Wrong architecture for $Path. Expected $Architecture, got PE machine $machine." }
  } finally { $reader.Dispose() }
}

$bridgeSource = Join-Path $repoRoot 'bridge_dx11_work'
$runtimeBuild = Join-Path $repoRoot ("_Comp64" + $BuildFlavour)
$serverBuild = Join-Path $bridgeSource ("_Comp64" + $BuildFlavour)
$clientBuild = Join-Path $bridgeSource ("_Comp32" + $BuildFlavour)
$bridgeOptions = @('-Denable_tests=false', "-Denable_tracy=$EnableTracy")

if (-not $RuntimeOnly -and -not $StageOnly) {
  [void](Build-SourceTree $bridgeSource $serverBuild x64 'bridge-x64' $bridgeOptions)
  [void](Build-SourceTree $bridgeSource $clientBuild x86 'bridge-x86' $bridgeOptions)
}
if (-not $BridgeOnly -and -not $StageOnly) {
  Set-VisualStudioBuildEnvironment -Architecture x64
  [void](Resolve-BuildTools)
  Ensure-DxvkDependencies -SourceDir $repoRoot -FetchDependencies (-not $NoDepsFetch) -TimeoutSeconds 900 -BuildFlavour $BuildFlavour
  $runtimeOptions = @('-Denable_dxgi=true', '-Denable_d3d11=true', '-Denable_tests=false', "-Denable_tracy=$EnableTracy", '-Dskip_packman_fetch=true')
  $python = Build-SourceTree $repoRoot $runtimeBuild x64 'runtime-x64' $runtimeOptions
  if (-not $ConfigureOnly) {
    Invoke-BuildTool $python @('-m', 'mesonbuild.mesonmain', 'install', '-C', $runtimeBuild, '--no-rebuild', '--tags', 'output') $repoRoot 'runtime-install'
  }
}
if ($ConfigureOnly -or $NoStage -or $BridgeOnly) { return }

$defaultX64 = Join-Path $repoRoot '_output\x64'
Assert-PeMachine (Join-Path $defaultX64 'd3d11.dll') x64
Assert-PeMachine (Join-Path $defaultX64 'dxgi.dll') x64
$resolvedOutput = [IO.Path]::GetFullPath($(if ([IO.Path]::IsPathRooted($OutputRoot)) { $OutputRoot } else { Join-Path $repoRoot $OutputRoot }))
if ($resolvedOutput -eq [IO.Path]::GetFullPath($repoRoot)) { throw 'OutputRoot cannot be the repository root.' }
$x64 = Join-Path $resolvedOutput 'x64'
if ($x64 -ine $defaultX64) {
  $runtimeSourcePrefix = $defaultX64.TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar
  $runtimeTargetPrefix = $x64.TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar
  if ($x64.StartsWith($runtimeSourcePrefix, [StringComparison]::OrdinalIgnoreCase) -or
      $defaultX64.StartsWith($runtimeTargetPrefix, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Custom OutputRoot must not place its x64 directory inside or around the installed runtime directory.'
  }
  New-Item -ItemType Directory -Force -Path $x64 | Out-Null
  Get-ChildItem -LiteralPath $defaultX64 -Force | Copy-Item -Destination $x64 -Recurse -Force
}
if (-not $RuntimeOnly) {
  $x86 = Join-Path $resolvedOutput 'x86'
  $trex = Join-Path $x86 '.trex'
  New-Item -ItemType Directory -Force -Path $trex | Out-Null
  foreach ($name in @('d3d11.dll', 'dxgi.dll')) {
    $source = Join-Path $clientBuild ('src\client_dx11\' + $name)
    Assert-PeMachine $source x86
    Copy-Item -LiteralPath $source -Destination (Join-Path $x86 $name) -Force
  }
  $launcher = Join-Path $clientBuild 'src\launcher\NvRemixLauncher32.exe'
  $server = Join-Path $serverBuild 'src\server\NvRemixBridge.exe'
  Assert-PeMachine $launcher x86
  Assert-PeMachine $server x64
  Copy-Item -LiteralPath $launcher -Destination $x86 -Force
  Get-ChildItem -LiteralPath $x64 -Force | Copy-Item -Destination $trex -Recurse -Force
  Copy-Item -LiteralPath $server -Destination $trex -Force
  $clientVersion = Get-Content -LiteralPath (Join-Path $clientBuild 'version.h') -Raw
  $serverVersion = Get-Content -LiteralPath (Join-Path $serverBuild 'version.h') -Raw
  if ($clientVersion -cne $serverVersion) { throw 'Client and server bridge version headers differ.' }
  $versionMatch = [regex]::Match($serverVersion, '#define\s+BRIDGE_VERSION\s+"([^"]+)"')
  if (-not $versionMatch.Success) { throw 'Cannot read the bridge protocol version.' }
  [IO.File]::WriteAllText((Join-Path $trex 'bridge_version.txt'), $versionMatch.Groups[1].Value)
  # Both sides read bridge.conf beside the server, inside .trex.
  Copy-Item -LiteralPath (Join-Path $bridgeSource 'bridge.conf') -Destination $trex -Force
  foreach ($config in @('rtx.conf', 'dxvk.conf')) {
    $configSource = Join-Path $repoRoot $config
    if (Test-Path -LiteralPath $configSource) { Copy-Item -LiteralPath $configSource -Destination $x86 -Force }
  }
}
if (-not $SkipZip) {
  & (Join-Path $repoRoot 'package_release.ps1') -SourceDir $resolvedOutput
}
Write-Host "Build and staging completed: $resolvedOutput" -ForegroundColor Green
