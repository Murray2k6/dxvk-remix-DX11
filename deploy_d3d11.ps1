# Deploy the built Remix runtime from _output\x64 next to a game executable.
# Build first (build.bat -RuntimeOnly -SkipZip; build_vkd3d_proton.ps1 for DX12).
#
# Usage:
#   .\deploy_d3d11.ps1 -GameDir "E:\SteamLibrary\steamapps\common\Fallout 4"
#   .\deploy_d3d11.ps1 -GameDir "D:\...\Starfield" -Dx12
#
# DX11 games: d3d11.dll and dxgi.dll (with their .pdb files).
# DX12 games (-Dx12): also vkd3d-proton's d3d12.dll and d3d12core.dll from
# _output\x64\vkd3d (both are needed: Remix's heap-index export lives in
# d3d12core.dll). DX12 and Vulkan games also need the Remix Vulkan layer
# registered once (install_remix_vk_layer.ps1, writes HKCU) and an rtx.conf
# or rtx-remix\ folder next to the game.
[CmdletBinding()]
param(
  [Parameter(Mandatory = $true)][string]$GameDir,
  [switch]$Dx12
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$out = Join-Path $PSScriptRoot '_output\x64'

if (-not (Test-Path -LiteralPath $GameDir -PathType Container)) {
  throw "Game folder not found: $GameDir"
}

$files = @(
  @{ src = Join-Path $out 'd3d11.dll'; required = $true },
  @{ src = Join-Path $out 'dxgi.dll';  required = $true },
  @{ src = Join-Path $out 'd3d11.pdb'; required = $false },
  @{ src = Join-Path $out 'dxgi.pdb';  required = $false }
)

if ($Dx12) {
  $files += @(
    @{ src = Join-Path $out 'vkd3d\d3d12.dll';     required = $true },
    @{ src = Join-Path $out 'vkd3d\d3d12core.dll'; required = $true }
  )
}

# A running game keeps its DLLs open; copying would fail half-way.
foreach ($exe in Get-ChildItem -LiteralPath $GameDir -Filter *.exe -File) {
  if (Get-Process -Name $exe.BaseName -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq $exe.FullName }) {
    throw "$($exe.Name) is running; close the game before deploying."
  }
}

foreach ($f in $files) {
  if (-not (Test-Path -LiteralPath $f.src)) {
    if ($f.required) { throw "$($f.src) not found; build first." }
    continue
  }

  $dst = Join-Path $GameDir (Split-Path -Leaf $f.src)
  Copy-Item -LiteralPath $f.src -Destination $dst -Force
  $item = Get-Item -LiteralPath $dst
  Write-Host ("  {0,-14} {1:yyyy-MM-dd HH:mm:ss}  {2} bytes" -f $item.Name, $item.LastWriteTime, $item.Length)
}

Write-Host "Deployed to $GameDir" -ForegroundColor Green
