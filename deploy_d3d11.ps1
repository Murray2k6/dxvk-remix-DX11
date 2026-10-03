# Deploy the built Remix runtime from _output\x64 next to a game executable.
# Build first (build.bat -SkipZip; build_vkd3d_proton.ps1 for DX12).
#
# Usage:
#   .\deploy_d3d11.ps1 -GameDir "E:\SteamLibrary\steamapps\common\Fallout 4"
#
# Copies the whole x64 folder: the runtime (d3d11.dll, dxgi.dll and their
# satellite libraries), the Remix Vulkan layer and vkd3d-proton's d3d12.dll /
# d3d12core.dll. Everything stays in the game folder: DX12 games use the
# vkd3d DLLs there, the layer turns itself on for them, and nothing is
# registered with Windows. DX11 games never load the DX12 files.
[CmdletBinding()]
param(
  [Parameter(Mandatory = $true)][string]$GameDir
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$out = Join-Path $PSScriptRoot '_output\x64'

if (-not (Test-Path -LiteralPath $GameDir -PathType Container)) {
  throw "Game folder not found: $GameDir"
}

foreach ($required in 'd3d11.dll', 'dxgi.dll') {
  if (-not (Test-Path -LiteralPath (Join-Path $out $required))) {
    throw "$required not found in $out; build first."
  }
}

# A running game keeps its DLLs open; copying would fail half-way.
foreach ($exe in Get-ChildItem -LiteralPath $GameDir -Filter *.exe -File) {
  if (Get-Process -Name $exe.BaseName -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq $exe.FullName }) {
    throw "$($exe.Name) is running; close the game before deploying."
  }
}

Get-ChildItem -LiteralPath $out -Force | Copy-Item -Destination $GameDir -Recurse -Force

foreach ($name in 'd3d11.dll', 'dxgi.dll', 'remix_vk_layer.dll', 'd3d12.dll', 'd3d12core.dll') {
  $item = Get-Item -LiteralPath (Join-Path $GameDir $name) -ErrorAction SilentlyContinue
  if ($item) {
    Write-Host ("  {0,-20} {1:yyyy-MM-dd HH:mm:ss}" -f $item.Name, $item.LastWriteTime)
  } else {
    Write-Host ("  {0,-20} not built" -f $name) -ForegroundColor Yellow
  }
}

Write-Host "Deployed to $GameDir" -ForegroundColor Green
