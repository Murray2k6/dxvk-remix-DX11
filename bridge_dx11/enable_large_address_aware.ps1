# enable_large_address_aware.ps1
# ------------------------------------------------------------------------------
# Sets the LARGEADDRESSAWARE bit in a 32-bit (PE32) game executable's header so
# the process can use up to 4 GB of user address space on 64-bit Windows,
# instead of the default 2 GB. This is the standard, safe way to lift the RAM
# ceiling for an old x86 game running under the DX11 Remix bridge: the bridge
# already keeps the large RTX allocations in the x64 server, and this removes
# the remaining limit inside the game process itself.
#
# A timestamped backup of the original EXE is written next to it before any
# change. 64-bit executables already have a 128 TB address space and are
# skipped. Re-running on an already-patched EXE is a no-op.
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File .\enable_large_address_aware.ps1 -ExePath "C:\Path\To\Game.exe"
#   powershell -ExecutionPolicy Bypass -File .\enable_large_address_aware.ps1 -ExePath "C:\Path\To\Game.exe" -Revert
[CmdletBinding()]
param(
  [Parameter(Mandatory = $true)] [string] $ExePath,
  [switch] $Revert
)
$ErrorActionPreference = 'Stop'
function Log([string]$m) { Write-Host ("[LAA] {0}" -f $m) }

if (-not (Test-Path -LiteralPath $ExePath)) { Log "ERROR: file not found: $ExePath"; exit 1 }

$IMAGE_FILE_LARGE_ADDRESS_AWARE = 0x0020

$bytes = [System.IO.File]::ReadAllBytes($ExePath)
if ($bytes.Length -lt 0x40) { Log "ERROR: file too small to be a PE image."; exit 1 }

# DOS header: 'MZ', e_lfanew at 0x3C points to the PE header.
if ($bytes[0] -ne 0x4D -or $bytes[1] -ne 0x5A) { Log "ERROR: not an MZ/PE executable."; exit 1 }
$peOffset = [System.BitConverter]::ToInt32($bytes, 0x3C)
if ($peOffset -le 0 -or ($peOffset + 24) -ge $bytes.Length) { Log "ERROR: invalid PE offset."; exit 1 }

# PE signature: 'PE\0\0'
if ($bytes[$peOffset] -ne 0x50 -or $bytes[$peOffset + 1] -ne 0x45 -or $bytes[$peOffset + 2] -ne 0 -or $bytes[$peOffset + 3] -ne 0) {
  Log "ERROR: PE signature not found."; exit 1
}

# COFF File Header starts right after the 4-byte signature.
# Machine = first 2 bytes; Characteristics = at offset +18 within the file header.
$machine = [System.BitConverter]::ToUInt16($bytes, $peOffset + 4)
$charsOffset = $peOffset + 4 + 18
$chars = [System.BitConverter]::ToUInt16($bytes, $charsOffset)

$IMAGE_FILE_MACHINE_I386 = 0x014c
if ($machine -ne $IMAGE_FILE_MACHINE_I386) {
  Log ("This is not a 32-bit (x86) executable (machine=0x{0:X4}); 64-bit images already have a huge address space. Nothing to do." -f $machine)
  exit 0
}

$isSet = ($chars -band $IMAGE_FILE_LARGE_ADDRESS_AWARE) -ne 0

if ($Revert) {
  if (-not $isSet) { Log "LARGEADDRESSAWARE is already clear; nothing to revert."; exit 0 }
  $chars = $chars -band (-bnot $IMAGE_FILE_LARGE_ADDRESS_AWARE)
} else {
  if ($isSet) { Log "LARGEADDRESSAWARE is already set on this EXE; the game can already use 4 GB. Nothing to do."; exit 0 }
  $chars = $chars -bor $IMAGE_FILE_LARGE_ADDRESS_AWARE
}

# Back up the original before writing.
$backup = "{0}.laa-backup-{1}" -f $ExePath, (Get-Date -Format "yyyyMMdd_HHmmss")
Copy-Item -LiteralPath $ExePath -Destination $backup -Force
Log "Backup written: $backup"

$patched = [System.BitConverter]::GetBytes([uint16]$chars)
$bytes[$charsOffset]     = $patched[0]
$bytes[$charsOffset + 1] = $patched[1]
[System.IO.File]::WriteAllBytes($ExePath, $bytes)

if ($Revert) {
  Log "LARGEADDRESSAWARE cleared. The game is back to a 2 GB address space."
} else {
  Log "LARGEADDRESSAWARE set. On 64-bit Windows the game can now use up to 4 GB of RAM."
  Log "If the game misbehaves (rare, for engines that store flags in pointer high bits), re-run with -Revert."
}
