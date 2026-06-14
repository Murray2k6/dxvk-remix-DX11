param(
  [ValidateSet('debug','debugoptimized','release')]
  [string]$Config = 'release',
  [string]$ProjectDir = '',
  [string]$OutRoot = '',
  [switch]$AllConfigs,
  [switch]$Clean
)
Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'

if ([string]::IsNullOrWhiteSpace($ProjectDir)) {
  $ProjectDir = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
} else {
  $ProjectDir = [IO.Path]::GetFullPath($ProjectDir)
}
if ([string]::IsNullOrWhiteSpace($OutRoot)) {
  $OutRoot = Join-Path $ProjectDir '_output'
} else {
  $OutRoot = [IO.Path]::GetFullPath($OutRoot)
}

function Write-Info([string]$Message) { Write-Host "[dx11-bridge] $Message" -ForegroundColor Cyan }
function Write-Warn([string]$Message) { Write-Host "[dx11-bridge][warn] $Message" -ForegroundColor Yellow }
function Ensure-Dir([string]$Path) { if (!(Test-Path -LiteralPath $Path)) { New-Item -ItemType Directory -Force -Path $Path | Out-Null } }

function Get-LatestDirectory([string]$Path, [string]$What) {
  if (!(Test-Path -LiteralPath $Path)) { throw "$What root not found: $Path" }
  $dirs = @(Get-ChildItem -LiteralPath $Path -Directory | Sort-Object Name -Descending)
  if ($dirs.Count -eq 0) { throw "No $What versions found under: $Path" }
  return $dirs[0].FullName
}

function Find-VisualStudioRoot {
  $pf86 = ${env:ProgramFiles(x86)}
  $candidates = @()
  if (![string]::IsNullOrWhiteSpace($pf86)) {
    $candidates += (Join-Path $pf86 'Microsoft Visual Studio\Installer\vswhere.exe')
  }
  $candidates += 'C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe'
  foreach ($vswhere in $candidates) {
    if (Test-Path -LiteralPath $vswhere) {
      $out = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath -nologo 2>$null
      if ($LASTEXITCODE -eq 0 -and ![string]::IsNullOrWhiteSpace($out)) { return ([string]$out).Trim() }
    }
  }
  $roots = @(
    'C:\Program Files\Microsoft Visual Studio\2022\Community',
    'C:\Program Files\Microsoft Visual Studio\2022\Professional',
    'C:\Program Files\Microsoft Visual Studio\2022\Enterprise',
    'C:\Program Files\Microsoft Visual Studio\18\Community',
    'C:\Program Files\Microsoft Visual Studio\18\Professional',
    'C:\Program Files\Microsoft Visual Studio\18\Enterprise'
  )
  foreach ($r in $roots) {
    if (Test-Path -LiteralPath (Join-Path $r 'VC\Tools\MSVC')) { return $r }
  }
  throw 'Visual Studio with MSVC x86/x64 tools not found. Install the C++ desktop workload and MSVC x86/x64 build tools.'
}

function Get-Toolchain([ValidateSet('x86','x64')] [string]$Arch) {
  $vs = Find-VisualStudioRoot
  $vcRoot = Join-Path $vs 'VC\Tools\MSVC'
  $vc = Get-LatestDirectory $vcRoot 'MSVC toolset'
  $sdkLibRoot = 'C:\Program Files (x86)\Windows Kits\10\Lib'
  $sdkIncRoot = 'C:\Program Files (x86)\Windows Kits\10\Include'
  $sdkLib = Get-LatestDirectory $sdkLibRoot 'Windows SDK lib'
  $sdkInc = Get-LatestDirectory $sdkIncRoot 'Windows SDK include'
  $target = if ($Arch -eq 'x86') { 'x86' } else { 'x64' }
  $bin = Join-Path $vc ("bin\Hostx64\$target")
  $cl = Join-Path $bin 'cl.exe'
  $link = Join-Path $bin 'link.exe'
  if (!(Test-Path -LiteralPath $cl)) { throw "cl.exe not found: $cl" }
  if (!(Test-Path -LiteralPath $link)) { throw "link.exe not found: $link" }
  return [pscustomobject]@{
    Vs = $vs
    Vc = $vc
    SdkLib = $sdkLib
    SdkInc = $sdkInc
    Bin = $bin
    Cl = $cl
    Link = $link
    Include = @(
      (Join-Path $vc 'include'),
      (Join-Path $sdkInc 'ucrt'),
      (Join-Path $sdkInc 'shared'),
      (Join-Path $sdkInc 'um'),
      (Join-Path $sdkInc 'winrt')
    )
    Lib = @(
      (Join-Path $vc ("lib\$target")),
      (Join-Path $sdkLib ("ucrt\$target")),
      (Join-Path $sdkLib ("um\$target"))
    )
  }
}

function Set-ToolEnv($Toolchain) {
  $env:PATH = $Toolchain.Bin + ';' + $env:PATH
  $env:INCLUDE = ($Toolchain.Include -join ';')
  $env:LIB = ($Toolchain.Lib -join ';')
  $env:LIBPATH = $env:LIB
}

function ConvertTo-RspArg([string]$Arg) {
  if ($null -eq $Arg) { return '""' }
  $s = [string]$Arg
  if ($s.Length -eq 0) { return '""' }
  if ($s -match '[\s"]') {
    $s = $s -replace '"','\"'
    return '"' + $s + '"'
  }
  return $s
}

$script:InvokeCounter = 0
function Get-LogTail([string]$Path, [int]$Lines = 80) {
  if (!(Test-Path -LiteralPath $Path)) { return '' }
  return ((Get-Content -LiteralPath $Path -Tail $Lines -ErrorAction SilentlyContinue) -join [Environment]::NewLine)
}

function Invoke-ToolRsp([string]$Exe, [string[]]$ToolArgs, [string]$WorkDir, [string]$LogPath, [string]$Label) {
  if ([string]::IsNullOrWhiteSpace($Exe)) { throw 'Invoke-ToolRsp got an empty executable path.' }
  if ($null -eq $ToolArgs -or $ToolArgs.Count -eq 0) { throw "Invoke-ToolRsp got no arguments for $Exe" }
  Ensure-Dir $WorkDir
  $script:InvokeCounter++
  $rsp = Join-Path $WorkDir ("invoke_{0:D3}_{1}.rsp" -f $script:InvokeCounter, $Label)
  $lines = @()
  foreach ($a in $ToolArgs) { $lines += (ConvertTo-RspArg ([string]$a)) }
  Set-Content -LiteralPath $rsp -Value $lines -Encoding ASCII
  Add-Content -LiteralPath $LogPath -Value (">>> $Exe @`"$rsp`"")
  Add-Content -LiteralPath $LogPath -Value ('--- response file: ' + $rsp + ' ---')
  Add-Content -LiteralPath $LogPath -Value (Get-Content -LiteralPath $rsp)
  Add-Content -LiteralPath $LogPath -Value '--- end response file ---'
  $psi = New-Object System.Diagnostics.ProcessStartInfo
  $psi.FileName = $Exe
  $psi.Arguments = '@"' + $rsp + '"'
  $psi.WorkingDirectory = $WorkDir
  $psi.UseShellExecute = $false
  $psi.RedirectStandardOutput = $true
  $psi.RedirectStandardError = $true
  $p = New-Object System.Diagnostics.Process
  $p.StartInfo = $psi
  [void]$p.Start()
  $stdout = $p.StandardOutput.ReadToEnd()
  $stderr = $p.StandardError.ReadToEnd()
  $p.WaitForExit()
  if (![string]::IsNullOrWhiteSpace($stdout)) { Add-Content -LiteralPath $LogPath -Value $stdout }
  if (![string]::IsNullOrWhiteSpace($stderr)) { Add-Content -LiteralPath $LogPath -Value $stderr }
  if ($p.ExitCode -ne 0) {
    $tail = Get-LogTail $LogPath 120
    throw ("Command failed with exit code $($p.ExitCode): $Exe`nBuild log tail:`n$tail")
  }
}

function Get-PeMachine([string]$Path) {
  if (!(Test-Path -LiteralPath $Path)) { throw "PE file missing: $Path" }
  $fs = [IO.File]::OpenRead($Path)
  try {
    $br = New-Object IO.BinaryReader($fs)
    if ($br.ReadUInt16() -ne 0x5A4D) { return 'not-pe' }
    $fs.Seek(0x3C, [IO.SeekOrigin]::Begin) | Out-Null
    $pe = $br.ReadInt32()
    $fs.Seek($pe, [IO.SeekOrigin]::Begin) | Out-Null
    if ($br.ReadUInt32() -ne 0x00004550) { return 'not-pe' }
    $m = $br.ReadUInt16()
    switch ($m) { 0x014c { 'x86' } 0x8664 { 'x64' } default { ('0x{0:X4}' -f $m) } }
  } finally { $fs.Close() }
}

function Assert-PeMachine([string]$Path, [string]$Expected) {
  $m = Get-PeMachine $Path
  if ($m -ne $Expected) { throw "Wrong PE machine for $Path. Expected $Expected, got $m" }
}

function Copy-TreeContents([string]$SourceDir, [string]$DestDir) {
  if (!(Test-Path -LiteralPath $SourceDir)) { return }
  Ensure-Dir $DestDir
  $items = @(Get-ChildItem -LiteralPath $SourceDir -Force)
  foreach ($item in $items) {
    $dest = Join-Path $DestDir $item.Name
    if ($item.PSIsContainer) { Copy-Item -LiteralPath $item.FullName -Destination $dest -Recurse -Force }
    else { Copy-Item -LiteralPath $item.FullName -Destination $dest -Force }
  }
}

function Get-ConfigDefines([string]$Cfg) {
  $defs = @('/nologo','/EHsc','/std:c++17','/DWIN32','/D_WINDOWS','/DUNICODE','/D_UNICODE')
  if ($Cfg -eq 'debug') { $defs += @('/MTd','/Od','/Zi','/D_DEBUG') }
  elseif ($Cfg -eq 'debugoptimized') { $defs += @('/MT','/O2','/Zi','/D_DEBUGOPTIMIZED') }
  else { $defs += @('/MT','/O2','/DNDEBUG') }
  return $defs
}

function Compile-One($Toolchain, [string]$Cfg, [string]$SrcFile, [string]$ObjFile, [string]$ObjDir, [string]$LogPath) {
  $args = @()
  $args += (Get-ConfigDefines $Cfg)
  $args += ('/I' + $PSScriptRoot)
  $args += ('/I' + (Join-Path $PSScriptRoot 'shared'))
  $args += ('/I' + (Join-Path $PSScriptRoot 'client_x86'))
  $args += ('/I' + (Join-Path $PSScriptRoot 'server_x64'))
  $args += '/c'
  $args += $SrcFile
  $args += ('/Fo' + $ObjFile)
  Invoke-ToolRsp -Exe $Toolchain.Cl -ToolArgs $args -WorkDir $ObjDir -LogPath $LogPath -Label 'cl'
  if (!(Test-Path -LiteralPath $ObjFile)) { throw "Compiler returned success but object file is missing: $ObjFile`n$(Get-LogTail $LogPath 120)" }
}

function Build-Client([string]$Cfg) {
  Write-Info "building x86 DX11 client for $Cfg"
  $tc = Get-Toolchain x86
  Set-ToolEnv $tc
  $src = Join-Path $PSScriptRoot 'client_x86'
  $out = Join-Path $ProjectDir ("_dx11_bridge_build\x86\$Cfg")
  $obj = Join-Path $out 'obj'
  Ensure-Dir $obj
  $log = Join-Path $out 'build.log'
  Set-Content -LiteralPath $log -Value "DX11 x86 client build $Cfg"
  $objects = @()
  foreach ($s in @('dx11_bridge_client.cpp','d3d11_exports.cpp','dxgi_exports.cpp')) {
    $srcFile = Join-Path $src $s
    if (!(Test-Path -LiteralPath $srcFile)) { throw "Missing x86 client source: $srcFile" }
    $objFile = Join-Path $obj ([IO.Path]::GetFileNameWithoutExtension($s) + '.obj')
    Compile-One $tc $Cfg $srcFile $objFile $obj $log
    $objects += $objFile
  }
  $commonObj = Join-Path $obj 'dx11_bridge_client.obj'
  $d3d11Obj = Join-Path $obj 'd3d11_exports.obj'
  $dxgiObj = Join-Path $obj 'dxgi_exports.obj'
  $d3d11Dll = Join-Path $out 'd3d11.dll'
  $dxgiDll = Join-Path $out 'dxgi.dll'
  $d3d11Args = @('/NOLOGO','/DLL','/MACHINE:X86',('/OUT:' + $d3d11Dll),('/IMPLIB:' + (Join-Path $out 'd3d11.lib')),('/DEF:' + (Join-Path $src 'd3d11_client.def')),$commonObj,$d3d11Obj,'kernel32.lib','user32.lib','advapi32.lib','shlwapi.lib','ole32.lib','uuid.lib')
  Invoke-ToolRsp -Exe $tc.Link -ToolArgs $d3d11Args -WorkDir $out -LogPath $log -Label 'link_d3d11'
  $dxgiArgs = @('/NOLOGO','/DLL','/MACHINE:X86',('/OUT:' + $dxgiDll),('/IMPLIB:' + (Join-Path $out 'dxgi.lib')),('/DEF:' + (Join-Path $src 'dxgi_client.def')),$commonObj,$dxgiObj,'kernel32.lib','user32.lib','advapi32.lib','shlwapi.lib','ole32.lib','uuid.lib')
  Invoke-ToolRsp -Exe $tc.Link -ToolArgs $dxgiArgs -WorkDir $out -LogPath $log -Label 'link_dxgi'
  Assert-PeMachine $d3d11Dll 'x86'
  Assert-PeMachine $dxgiDll 'x86'
  return $out
}

function Build-Server([string]$Cfg) {
  Write-Info "building x64 DX11 bridge server for $Cfg"
  $tc = Get-Toolchain x64
  Set-ToolEnv $tc
  $src = Join-Path $PSScriptRoot 'server_x64'
  $out = Join-Path $ProjectDir ("_dx11_bridge_build\x64\$Cfg")
  $obj = Join-Path $out 'obj'
  Ensure-Dir $obj
  $log = Join-Path $out 'build.log'
  Set-Content -LiteralPath $log -Value "DX11 x64 bridge server build $Cfg"
  $srcFile = Join-Path $src 'dx11_bridge_server.cpp'
  if (!(Test-Path -LiteralPath $srcFile)) { throw "Missing x64 server source: $srcFile" }
  $objFile = Join-Path $obj 'dx11_bridge_server.obj'
  Compile-One $tc $Cfg $srcFile $objFile $obj $log
  $exe = Join-Path $out 'NvRemixBridge.exe'
  $linkArgs = @('/NOLOGO','/SUBSYSTEM:WINDOWS','/MACHINE:X64',('/OUT:' + $exe),$objFile,'kernel32.lib','user32.lib','advapi32.lib','shlwapi.lib','ole32.lib','uuid.lib','d3d11.lib','dxgi.lib')
  Invoke-ToolRsp -Exe $tc.Link -ToolArgs $linkArgs -WorkDir $out -LogPath $log -Label 'link_server'
  Assert-PeMachine $exe 'x64'
  return $out
}

function Stage-Config([string]$Cfg, [string]$ClientDir, [string]$ServerDir) {
  $x86 = Join-Path $OutRoot ("x86\$Cfg")
  $trex = Join-Path $x86 '.trex'
  Ensure-Dir $x86
  Ensure-Dir $trex
  Copy-Item -LiteralPath (Join-Path $ClientDir 'd3d11.dll') -Destination (Join-Path $x86 'd3d11.dll') -Force
  Copy-Item -LiteralPath (Join-Path $ClientDir 'dxgi.dll') -Destination (Join-Path $x86 'dxgi.dll') -Force
  Copy-Item -LiteralPath (Join-Path $ServerDir 'NvRemixBridge.exe') -Destination (Join-Path $trex 'NvRemixBridge.exe') -Force
  $x64Runtime = Join-Path $OutRoot ("x64\$Cfg")
  if (Test-Path -LiteralPath $x64Runtime) {
    Copy-TreeContents $x64Runtime $trex
  } else {
    Write-Warn "x64 runtime folder not found yet: $x64Runtime. Build x64 first so .trex gets d3d11.dll and dxgi.dll."
  }
  Assert-PeMachine (Join-Path $x86 'd3d11.dll') 'x86'
  Assert-PeMachine (Join-Path $x86 'dxgi.dll') 'x86'
  Assert-PeMachine (Join-Path $trex 'NvRemixBridge.exe') 'x64'
  if (Test-Path -LiteralPath (Join-Path $trex 'd3d11.dll')) { Assert-PeMachine (Join-Path $trex 'd3d11.dll') 'x64' }
  if (Test-Path -LiteralPath (Join-Path $trex 'dxgi.dll')) { Assert-PeMachine (Join-Path $trex 'dxgi.dll') 'x64' }
  Write-Info "staged _output\x86\$Cfg"
}

$configs = if ($AllConfigs) { @('debug','debugoptimized','release') } else { @($Config) }
if ($Clean) {
  $b = Join-Path $ProjectDir '_dx11_bridge_build'
  if (Test-Path -LiteralPath $b) { Remove-Item -LiteralPath $b -Recurse -Force }
}
foreach ($cfg in $configs) {
  $client = Build-Client $cfg
  $server = Build-Server $cfg
  Stage-Config $cfg $client $server
}
