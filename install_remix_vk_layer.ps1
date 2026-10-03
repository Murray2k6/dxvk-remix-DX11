[CmdletBinding()]
param(
  # Folder holding remix_vk_layer.dll and remix_vk_layer.json (the build output).
  [string]$LayerDir = (Join-Path $PSScriptRoot '_output\x64'),
  [switch]$Uninstall
)

# Registers the Remix Vulkan layer as an implicit layer for the current user
# (HKCU\SOFTWARE\Khronos\Vulkan\ImplicitLayers, value = manifest path, DWORD 0),
# per the Vulkan loader's Windows layer discovery rules.
#
# Implicit layers load into every Vulkan application of this user. The layer
# itself stays a pass-through unless the game's folder holds Remix's d3d11.dll
# and rtx.conf (or rtx-remix\), and it refuses to start next to anti-cheat.
# Set DISABLE_REMIX_VK_LAYER=1 to turn it off for one launch.
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$key = 'HKCU:\SOFTWARE\Khronos\Vulkan\ImplicitLayers'
$manifest = [IO.Path]::GetFullPath((Join-Path $LayerDir 'remix_vk_layer.json'))

if ($Uninstall) {
  if (Test-Path -LiteralPath $key) {
    Remove-ItemProperty -LiteralPath $key -Name $manifest -ErrorAction SilentlyContinue
  }
  Write-Host "[remix-vk-layer] unregistered $manifest"
  return
}

foreach ($file in @('remix_vk_layer.json', 'remix_vk_layer.dll')) {
  if (-not (Test-Path -LiteralPath (Join-Path $LayerDir $file))) {
    throw "$file not found in $LayerDir; build first"
  }
}

if (-not (Test-Path -LiteralPath $key)) {
  New-Item -Path $key -Force | Out-Null
}

New-ItemProperty -LiteralPath $key -Name $manifest -Value 0 -PropertyType DWord -Force | Out-Null
Write-Host "[remix-vk-layer] registered $manifest"
