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

#
# Find vswhere (installed with recent Visual Studio versions).
#
If ($vsWhere = Get-Command "vswhere.exe" -ErrorAction SilentlyContinue) {
  $vsWhere = $vsWhere.Path
} ElseIf (Test-Path "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe") {
  $vsWhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
}
 Else {
  Write-Error "vswhere not found. Aborting." -ErrorAction Stop
}
Write-Host "vswhere found at: $vsWhere" -ForegroundColor Yellow


#
# Get path to Visual Studio installation using vswhere.
#
$vsPath = &$vsWhere -latest -version "[16.0,18.0)" -products * `
 -requires Microsoft.Component.MSBuild `
 -property installationPath
If ([string]::IsNullOrEmpty("$vsPath")) {
  Write-Error "Failed to find Visual Studio 2019 installation. Aborting." -ErrorAction Stop
}
Write-Host "Using Visual Studio installation at: ${vsPath}" -ForegroundColor Yellow


#
# Make sure the Visual Studio Command Prompt variables are set.
#
If (Test-Path env:LIBPATH) {
  Write-Host "Visual Studio Command Prompt variables already set." -ForegroundColor Yellow
} Else {
  # Load VC vars
  Push-Location "${vsPath}\VC\Auxiliary\Build"
  cmd /c "vcvarsall.bat x64&set" |
	ForEach-Object {
	  # Due to some odd behavior with how powershell core (pwsh) (powershell 5.X not tested) interprets a specific
	  # predefined gitlab CI variable (in this case CI_MERGE_REQUEST_DESCRIPTION) with a value that includes ===  
	  # The `Contains` method is used to ignore the string === to prevent pwsh from erroneously encountering an error.
	  If ($_ -match "=") {
		  If (-not ($_.Contains('==='))) {
			  $v = $_.split("="); Set-Item -Force -Path "ENV:\$($v[0])" -Value "$($v[1])"
		  }
	  }
	}
  Pop-Location
  Write-Host "Visual Studio Command Prompt variables set." -ForegroundColor Yellow
}

function Get-NormalizedFullPath {
	param(
		[Parameter(Mandatory)]
		[string]$Path
	)

	return [IO.Path]::GetFullPath($Path).TrimEnd('\').ToLowerInvariant()
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
		}
	}

	$buildNinjaPath = Join-Path $BuildDir 'build.ninja'
	if (Test-Path $buildNinjaPath) {
		$buildNinja = Get-Content $buildNinjaPath -Raw
		$regex = [regex]'--internal regenerate\s+"([^"]+)"\s+"\."'
		$match = $regex.Match($buildNinja)
		if ($match.Success) {
			return $match.Groups[1].Value
		}
	}

	return $null
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
		Write-Host "Removing partial or unreadable Meson build directory: $BuildDir" -ForegroundColor Yellow
		Remove-Item $BuildDir -Recurse -Force
		return $false
	}

	$normalizedSourceDir = Get-NormalizedFullPath -Path $SourceDir
	$normalizedConfiguredSourceDir = Get-NormalizedFullPath -Path $configuredSourceDir
	if ($normalizedSourceDir -ne $normalizedConfiguredSourceDir) {
		Write-Host "Removing stale Meson build directory '$BuildDir' configured for '$configuredSourceDir'" -ForegroundColor Yellow
		Remove-Item $BuildDir -Recurse -Force
		return $false
	}

	return $true
}

function PerformBuild {
	param(
		[Parameter(Mandatory)]
		[string]$Backend,

		[Parameter(Mandatory)]
		[string]$BuildFlavour,
		
		[Parameter(Mandatory)]
		[string]$BuildSubDir,

		[Parameter(Mandatory)]
		[string]$EnableTracy,

		[string]$BuildTarget,

		[string[]]$InstallTags,

		[bool]$ConfigureOnly = $false,

		[bool]$ShadersOnly = $false
	)

	if (-not $InstallTags -or $InstallTags.Count -eq 0) {
		$InstallTags = @('output')
	}

	$CurrentDir = Get-Location
	$OutputDir = [IO.Path]::Combine($CurrentDir, "_output")
	$BuildDir = [IO.Path]::Combine($CurrentDir, $BuildSubDir)
	
	Write-Host "[build] Starting build for $BuildFlavour..." -ForegroundColor Cyan
	Write-Host "[build] Build directory: $BuildDir" -ForegroundColor DarkGray
	
	$buildDirMatchesCurrentSource = Repair-StaleBuildDirectory -SourceDir $CurrentDir -BuildDir $BuildDir

	Push-Location $CurrentDir
		$mesonArgs = @(
			'setup'
		)
		if ($buildDirMatchesCurrentSource) {
			$mesonArgs += '--reconfigure'
		}
		$mesonArgs += @(
			'--buildtype', $BuildFlavour,
			'--backend', $Backend,
			'-Denable_dxgi=true',
			'-Denable_d3d11=true',
			'-Denable_tests=false',
			"-Denable_tracy=$EnableTracy"
		)
		if ( $ShadersOnly ) {
			$mesonArgs += '-Ddownload_apics=false'
		}
		$mesonArgs += $BuildSubDir
		Write-Host "[build] Running meson setup..." -ForegroundColor Cyan
		& meson @mesonArgs
		
		if ( $LASTEXITCODE -ne 0 ) {
			Write-Error "Failed to run meson setup" -ErrorAction Stop
		}
	Pop-Location

	if ($ShadersOnly) {
		Write-Host "[build] Building shaders only..." -ForegroundColor Cyan
		Push-Location $BuildDir
			& meson compile rtx_shaders
		Pop-Location
		if ( $LASTEXITCODE -ne 0 ) {
			Write-Error "Failed to build shaders" -ErrorAction Stop
		}
		exit $LASTEXITCODE
	}

	if (!$ConfigureOnly) {
		Write-Host "[build] Compiling..." -ForegroundColor Cyan
		Push-Location $BuildDir
			& meson compile -v 

			if ( $LASTEXITCODE -ne 0 ) {
				Write-Error "Failed to run build step" -ErrorAction Stop
			}

			# Restrict staging to the DX11 runtime output by default.
			$tagList = $InstallTags -join ','
			Write-Host "[build] Installing to $OutputDir..." -ForegroundColor Cyan
			& meson install --tags $tagList
		Pop-Location

		if ( $LASTEXITCODE -ne 0 ) {
			Write-Error "Failed to run install step" -ErrorAction Stop
		}
		
		Write-Host "[build] Build completed successfully for $BuildFlavour" -ForegroundColor Green
	} else {
		Write-Host "[build] Configuration completed for $BuildFlavour (no build performed)" -ForegroundColor Green
	}
}