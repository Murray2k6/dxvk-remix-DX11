param(
    [string]$SourceDir = "_output",
    [string]$StagingRoot = "_release",
    [string]$Version,
    [string]$PackageName,
    [string]$ValidationManifest,
    [string]$ValidationReport,
    [string[]]$ValidationEvidenceFiles = @(),
    [switch]$SkipZip
)

$ErrorActionPreference = "Stop"

function Copy-IfExists {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Source,

        [Parameter(Mandatory = $true)]
        [string]$Destination
    )

    if (Test-Path $Source) {
        Copy-Item -Path $Source -Destination $Destination -Force
    }
}

$repoRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$resolvedSourceDir = [IO.Path]::GetFullPath($(if ([IO.Path]::IsPathRooted($SourceDir)) { $SourceDir } else { Join-Path $repoRoot $SourceDir }))

if (-not (Test-Path $resolvedSourceDir)) {
    throw "Source directory '$resolvedSourceDir' does not exist. Build the runtime so _output exists first."
}

if (-not $Version -or [string]::IsNullOrWhiteSpace($Version)) {
    $releaseFile = Join-Path $repoRoot "RELEASE"
    if (Test-Path $releaseFile) {
        $Version = Get-Content $releaseFile |
            ForEach-Object { $_.Trim() } |
            Where-Object { $_ -and -not $_.StartsWith('#') } |
            Select-Object -First 1
    }

    if (-not $Version) {
        $Version = "dev"
    }
}

if (-not $PackageName -or [string]::IsNullOrWhiteSpace($PackageName)) {
    $PackageName = "dxvk-remix-dx11-$Version"
}

$resolvedStagingRoot = [IO.Path]::GetFullPath($(if ([IO.Path]::IsPathRooted($StagingRoot)) { $StagingRoot } else { Join-Path $repoRoot $StagingRoot }))
if ([IO.Path]::GetFileName($PackageName) -cne $PackageName -or $PackageName -in @('.', '..')) {
    throw 'PackageName must be a single directory name.'
}
$packageDir = [IO.Path]::GetFullPath((Join-Path $resolvedStagingRoot $PackageName))
$zipPath = [IO.Path]::GetFullPath((Join-Path $resolvedStagingRoot ($PackageName + '.zip')))
$stagingPrefix = $resolvedStagingRoot.TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar
if (-not $packageDir.StartsWith($stagingPrefix, [StringComparison]::OrdinalIgnoreCase) -or
    -not $zipPath.StartsWith($stagingPrefix, [StringComparison]::OrdinalIgnoreCase) -or
    $packageDir -ieq $resolvedSourceDir -or
    $resolvedSourceDir.StartsWith($packageDir + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase) -or
    $packageDir.StartsWith($resolvedSourceDir.TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Package staging and source directories must be separate, and package targets must remain within StagingRoot.'
}

if (Test-Path $packageDir) {
    Remove-Item -LiteralPath $packageDir -Recurse -Force
}

if (Test-Path $zipPath) {
    Remove-Item -LiteralPath $zipPath -Force
}

New-Item -ItemType Directory -Path $packageDir -Force | Out-Null
Get-ChildItem -LiteralPath $resolvedSourceDir -Force | Copy-Item -Destination $packageDir -Recurse -Force

Copy-IfExists -Source (Join-Path $repoRoot "dxvk.conf") -Destination (Join-Path $packageDir "dxvk.conf")
Copy-IfExists -Source (Join-Path $repoRoot "rtx.conf") -Destination (Join-Path $packageDir "rtx.conf")

# Ship the redistribution notices and the instructions for the exact binaries
# in this package. Missing required notices are packaging errors.
foreach ($notice in @('LICENSE', 'LICENSE-MIT', 'ThirdPartyLicenses.txt')) {
    Copy-Item -LiteralPath (Join-Path $repoRoot $notice) -Destination $packageDir -Force
}
$sharcLicenseDirectory = Join-Path $packageDir 'licenses\SHARC'
New-Item -ItemType Directory -Path $sharcLicenseDirectory -Force | Out-Null
foreach ($notice in @('License.md', 'REVISION')) {
    Copy-Item -LiteralPath (Join-Path $repoRoot ('src\dxvk\shaders\rtx\algorithm\sharc\sdk\' + $notice)) -Destination $sharcLicenseDirectory -Force
}
$documentationDirectory = Join-Path $packageDir 'documentation'
New-Item -ItemType Directory -Path $documentationDirectory -Force | Out-Null
foreach ($document in @('DX11_BUILD_AND_DEPLOYMENT.md', 'SHARC_DX11.md')) {
    Copy-Item -LiteralPath (Join-Path $repoRoot ('documentation\' + $document)) -Destination $documentationDirectory -Force
}
if ($ValidationManifest) {
    $manifestPath = if ([IO.Path]::IsPathRooted($ValidationManifest)) { $ValidationManifest } else { Join-Path $repoRoot $ValidationManifest }
    Copy-Item -LiteralPath $manifestPath -Destination (Join-Path $packageDir 'BUILD_VALIDATION.json') -Force
}
if ($ValidationReport) {
    $reportPath = if ([IO.Path]::IsPathRooted($ValidationReport)) { $ValidationReport } else { Join-Path $repoRoot $ValidationReport }
    $reportText = Get-Content -LiteralPath $reportPath -Raw
    # The root-level copy must retain working links when its original report
    # lived beneath _validation. Evidence files below preserve repository paths.
    $reportDirectory = Split-Path -Parent ([IO.Path]::GetFullPath($reportPath))
    $repositoryPrefix = [IO.Path]::GetFullPath($repoRoot).TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar
    $reportText = [regex]::Replace($reportText, '(?<prefix>\]\()(?<target>[^)]+)(?<suffix>\))', {
        param($match)
        $target = $match.Groups['target'].Value
        if ($target -match '^[a-zA-Z][a-zA-Z0-9+.-]*:' -or $target.StartsWith('#')) { return $match.Value }
        $resolvedTarget = [IO.Path]::GetFullPath((Join-Path $reportDirectory $target))
        if (-not $resolvedTarget.StartsWith($repositoryPrefix, [StringComparison]::OrdinalIgnoreCase)) { return $match.Value }
        return '](' + $resolvedTarget.Substring($repositoryPrefix.Length).Replace('\', '/') + ')'
    })
    [IO.File]::WriteAllText((Join-Path $packageDir 'REPAIR_REPORT.md'), $reportText)
}
foreach ($evidence in $ValidationEvidenceFiles) {
    $repositoryPrefix = [IO.Path]::GetFullPath($repoRoot).TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar
    $evidencePath = [IO.Path]::GetFullPath($(if ([IO.Path]::IsPathRooted($evidence)) { $evidence } else { Join-Path $repoRoot $evidence }))
    if (-not $evidencePath.StartsWith($repositoryPrefix, [StringComparison]::OrdinalIgnoreCase) -or
        -not (Test-Path -LiteralPath $evidencePath -PathType Leaf)) {
        throw "Validation evidence must be an existing file inside the repository: $evidence"
    }
    $relativeEvidence = $evidencePath.Substring($repositoryPrefix.Length)
    $evidenceDestination = [IO.Path]::GetFullPath((Join-Path $packageDir $relativeEvidence))
    if (-not $evidenceDestination.StartsWith($packageDir + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
        throw 'Validation evidence escaped the package directory.'
    }
    New-Item -ItemType Directory -Path (Split-Path -Parent $evidenceDestination) -Force | Out-Null
    Copy-Item -LiteralPath $evidencePath -Destination $evidenceDestination -Force
}

$topLevelFiles = Get-ChildItem -Path $packageDir -File | Sort-Object Name
$topLevelDirs = Get-ChildItem -Path $packageDir -Directory | Sort-Object Name

Write-Host "Packaged release layout: $packageDir" -ForegroundColor Green
Write-Host "Top-level files:" -ForegroundColor Cyan
$topLevelFiles | ForEach-Object { Write-Host "  $($_.Name)" }

Write-Host "Top-level directories:" -ForegroundColor Cyan
$topLevelDirs | ForEach-Object { Write-Host "  $($_.Name)" }

if (-not $SkipZip) {
    # Compress-Archive skips hidden files, including the required x86 .trex
    # runtime on installations that mark it hidden. Preserve the complete tree.
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    [IO.Compression.ZipFile]::CreateFromDirectory($packageDir, $zipPath)
    Write-Host "Created release archive: $zipPath" -ForegroundColor Green
}
