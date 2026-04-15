[CmdletBinding()]
param(
    [ValidateSet('debug', 'debugoptimized', 'release')]
    [string]$BuildFlavour = 'debugoptimized',

    [string]$BuildSubDir = 'build',

    [ValidateSet('ninja')]
    [string]$Backend = 'ninja',

    [ValidateSet('true', 'false')]
    [string]$EnableTracy = 'false',

    [string[]]$InstallTags = @('output'),

    [switch]$ConfigureOnly,

    [switch]$ShadersOnly
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Path $PSScriptRoot -Parent

function Write-Step {
    param([string]$Message)
    Write-Host "[build] $Message" -ForegroundColor Cyan
}

function Add-PathEntry {
    param([string]$PathEntry)

    if ([string]::IsNullOrWhiteSpace($PathEntry) -or -not (Test-Path $PathEntry)) {
        return
    }

    $currentPath = @($env:PATH -split ';' | Where-Object { $_ })
    if ($currentPath -notcontains $PathEntry) {
        $env:PATH = "$PathEntry;$env:PATH"
    }
}

function Test-CommandExists {
    param([string]$Name)
    return $null -ne (Get-Command $Name -ErrorAction SilentlyContinue)
}

function Get-CommandPath {
    param([string]$Name)

    $command = Get-Command $Name -ErrorAction SilentlyContinue
    if ($command) {
        return $command.Path
    }

    return $null
}

function Resolve-Python {
    $candidates = @(
        @{ Name = 'py'; Args = @('-3') },
        @{ Name = 'python'; Args = @() }
    )

    foreach ($candidate in $candidates) {
        $command = Get-Command $candidate.Name -ErrorAction SilentlyContinue
        if (-not $command) {
            continue
        }

        $pythonInfo = & $command.Path @($candidate.Args + @('-c', 'import struct,sys; print(str(sys.version_info.major) + chr(46) + str(sys.version_info.minor)); print(str(struct.calcsize(chr(80)) * 8)); print(sys.executable)')) 2>$null
        if ($LASTEXITCODE -ne 0 -or -not $pythonInfo -or $pythonInfo.Count -lt 3) {
            continue
        }

        $version = [Version](($pythonInfo | Select-Object -First 1).Trim())
        if ($version -lt [Version]'3.9') {
            continue
        }

        $bitness = [int](($pythonInfo | Select-Object -Skip 1 -First 1).Trim())
        if ($bitness -ne 64) {
            continue
        }

        $exePath = ($pythonInfo | Select-Object -Skip 2 -First 1).Trim()
        if ([string]::IsNullOrWhiteSpace($exePath)) {
            continue
        }

        $scriptsDir = Join-Path (Split-Path $exePath -Parent) 'Scripts'

        return [PSCustomObject]@{
            Launcher = $command.Path
            Args = $candidate.Args
            Executable = $exePath
            ScriptsDir = $scriptsDir
            Version = $version.ToString(2)
            Bitness = $bitness
        }
    }

    throw 'A 64-bit Python 3.9 or newer installation was not found. Install a standard x64 python.org build and make sure it is available on PATH.'
}

function Ensure-Meson {
    param([pscustomobject]$Python)

    Add-PathEntry $Python.ScriptsDir

    if (Test-CommandExists 'meson') {
        return Get-CommandPath 'meson'
    }

    $mesonExe = Join-Path $Python.ScriptsDir 'meson.exe'
    if (Test-Path $mesonExe) {
        Add-PathEntry $Python.ScriptsDir
        return $mesonExe
    }

    & $Python.Executable -m mesonbuild.mesonmain --help *> $null
    if ($LASTEXITCODE -eq 0) {
        $shimDir = Join-Path $env:TEMP 'dxvk-remix-build-shims'
        New-Item -ItemType Directory -Path $shimDir -Force | Out-Null

        $shimPath = Join-Path $shimDir 'meson.cmd'
        @(
            '@echo off',
            ('"{0}" -m mesonbuild.mesonmain %*' -f $Python.Executable)
        ) | Set-Content -Path $shimPath -Encoding Ascii

        Add-PathEntry $shimDir
        return $shimPath
    }

    throw 'Meson was not found. Install Meson or `pip install meson` for the detected Python installation.'
}

function Ensure-Ninja {
    param([string]$VisualStudioPath, [pscustomobject]$Python)

    if (Test-CommandExists 'ninja') {
        return Get-CommandPath 'ninja'
    }

    $pythonNinja = Join-Path $Python.ScriptsDir 'ninja.exe'
    if (Test-Path $pythonNinja) {
        Add-PathEntry $Python.ScriptsDir
        return $pythonNinja
    }

    $vsNinjaDir = Join-Path $VisualStudioPath 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja'
    $vsNinjaExe = Join-Path $vsNinjaDir 'ninja.exe'
    if (Test-Path $vsNinjaExe) {
        Add-PathEntry $vsNinjaDir
        return $vsNinjaExe
    }

    throw 'Ninja was not found. Install Ninja or add the Visual Studio CMake/Ninja tools to your installation.'
}

function Resolve-VsWhere {
    $vsWhere = Get-Command 'vswhere.exe' -ErrorAction SilentlyContinue
    if ($vsWhere) {
        return $vsWhere.Path
    }

    $fallback = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path $fallback) {
        return $fallback
    }

    throw 'vswhere.exe was not found. Install Visual Studio 2019 or 2022 with the C++ build tools.'
}

function Resolve-VisualStudio {
    param([string]$VsWherePath)

    $vsPath = & $VsWherePath -latest -version '[16.0,18.0)' -products * -requires Microsoft.Component.MSBuild -property installationPath
    if ([string]::IsNullOrWhiteSpace($vsPath)) {
        throw 'A supported Visual Studio installation was not found. Install Visual Studio 2019 or 2022 with MSBuild and the desktop C++ workload.'
    }

    return ($vsPath | Select-Object -First 1).Trim()
}

function Import-VcVars {
    param([string]$VisualStudioPath)

    if ((Test-Path env:LIBPATH) -and (Test-CommandExists 'cl')) {
        return
    }

    $vcVarsPath = Join-Path $VisualStudioPath 'VC\Auxiliary\Build\vcvarsall.bat'
    if (-not (Test-Path $vcVarsPath)) {
        throw "vcvarsall.bat was not found under $VisualStudioPath"
    }

    $envBlock = cmd /c "`"$vcVarsPath`" x64 >nul && set"
    foreach ($line in $envBlock) {
        if ($line -match '=' -and -not $line.Contains('===')) {
            $parts = $line -split '=', 2
            Set-Item -Path "ENV:$($parts[0])" -Value $parts[1]
        }
    }

    if (-not (Test-CommandExists 'cl')) {
        throw 'Failed to load the Visual Studio compiler environment.'
    }

    if (-not $env:WindowsSdkDir) {
        throw 'The Windows SDK was not detected in the Visual Studio toolchain environment.'
    }
}

function Resolve-VulkanSdk {
    $candidateRoots = @()

    if ($env:VULKAN_SDK) {
        $candidateRoots += $env:VULKAN_SDK
    }

    $defaultRoot = 'C:\VulkanSDK'
    if (Test-Path $defaultRoot) {
        $candidateRoots += Get-ChildItem $defaultRoot -Directory | Sort-Object Name -Descending | Select-Object -ExpandProperty FullName
    }

    foreach ($candidate in $candidateRoots | Select-Object -Unique) {
        $includePath = Join-Path $candidate 'Include\vulkan\vulkan.h'
        $libPath = Join-Path $candidate 'Lib\vulkan-1.lib'
        $binPath = Join-Path $candidate 'Bin'

        if ((Test-Path $includePath) -and (Test-Path $libPath) -and (Test-Path $binPath)) {
            $env:VULKAN_SDK = $candidate
            Add-PathEntry $binPath
            return $candidate
        }
    }

    throw 'A Vulkan SDK installation was not found. Install LunarG Vulkan SDK 1.4.313.2 or newer, or set VULKAN_SDK to a valid install.'
}

function Ensure-Submodules {
    Set-Location $repoRoot

    if (-not (Test-Path '.gitmodules')) {
        return
    }

    if (-not (Test-CommandExists 'git')) {
        throw 'Git is required to verify submodules. Install Git and ensure it is available on PATH.'
    }

    $statusLines = & git submodule status --recursive 2>$null
    if ($LASTEXITCODE -ne 0) {
        throw 'Failed to query git submodule status. Make sure this repository is a valid git checkout.'
    }

    $needsUpdate = @($statusLines | Where-Object { $_ -match '^[\-+]' }).Count -gt 0
    if ($needsUpdate) {
        Write-Step 'Missing submodules detected. Running git submodule update --init --recursive.'
        & git submodule update --init --recursive
        if ($LASTEXITCODE -ne 0) {
            throw 'Failed to initialize required submodules.'
        }
    }
}

Push-Location $repoRoot
try {
    Write-Step 'Resolving build requirements'

    $python = Resolve-Python
    $vsWherePath = Resolve-VsWhere
    $visualStudioPath = Resolve-VisualStudio -VsWherePath $vsWherePath
    Import-VcVars -VisualStudioPath $visualStudioPath
    $vulkanSdk = Resolve-VulkanSdk
    $mesonPath = Ensure-Meson -Python $python
    $ninjaPath = Ensure-Ninja -VisualStudioPath $visualStudioPath -Python $python
    Ensure-Submodules

    Write-Host ('Python:        {0} ({1}, {2}-bit)' -f $python.Executable, $python.Version, $python.Bitness) -ForegroundColor Yellow
    Write-Host ('Visual Studio: {0}' -f $visualStudioPath) -ForegroundColor Yellow
    Write-Host ('Windows SDK:   {0}' -f $env:WindowsSdkDir) -ForegroundColor Yellow
    Write-Host ('Vulkan SDK:    {0}' -f $vulkanSdk) -ForegroundColor Yellow
    Write-Host ('Meson:         {0}' -f $mesonPath) -ForegroundColor Yellow
    Write-Host ('Ninja:         {0}' -f $ninjaPath) -ForegroundColor Yellow

    . (Join-Path $repoRoot 'build_common.ps1')

    Write-Step ('Starting build: type={0}, dir={1}, backend={2}, tracy={3}' -f $BuildFlavour, $BuildSubDir, $Backend, $EnableTracy)
    PerformBuild -BuildFlavour $BuildFlavour -BuildSubDir $BuildSubDir -Backend $Backend -EnableTracy $EnableTracy -InstallTags $InstallTags -ConfigureOnly:$ConfigureOnly.IsPresent -ShadersOnly:$ShadersOnly.IsPresent
}
finally {
    Pop-Location
}