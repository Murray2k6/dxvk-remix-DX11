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

param(
  # Build with the Tracy profiler compiled in (connect the Tracy server app to
  # the running game for frame-level CPU/GPU timing). Off by default.
  [switch]$Tracy
)

.   ".\build_common.ps1"

$BuildFlavours = @("debug","debugoptimized","release")
$BuildSubDirs = @("_Comp64Debug","_Comp64DebugOptimized","_Comp64Release")

$EnableTracyValue = if ($Tracy) { "true" } else { "false" }

For ($i=0; $i -lt $BuildFlavours.Length; $i++) {
  PerformBuild -BuildFlavour $BuildFlavours[$i] -BuildSubDir $BuildSubDirs[$i] -Backend ninja -EnableTracy $EnableTracyValue
}

# Deploy the NGX runtime DLLs (DLSS, Ray Reconstruction, Frame Generation
# snippets) next to every built d3d11.dll. NGX loads these from the runtime
# directory at startup; without them DLSS reports "not available" even on
# RTX hardware with current drivers.
$repoRoot  = if ($PSScriptRoot) { $PSScriptRoot } else { (Get-Location).Path }
$ngxSource = Join-Path $repoRoot "nv-private\hdremix\bin\release"
if (Test-Path $ngxSource) {
  $ngxDlls = Get-ChildItem -Path $ngxSource -Filter "nvngx_*.dll" -File
  foreach ($subDir in $BuildSubDirs) {
    $buildRoot = Join-Path $repoRoot $subDir
    if (-not (Test-Path $buildRoot)) { continue }
    $targets = Get-ChildItem -Path $buildRoot -Recurse -Filter "d3d11.dll" -File -ErrorAction SilentlyContinue
    foreach ($t in $targets) {
      foreach ($dll in $ngxDlls) {
        Copy-Item -Path $dll.FullName -Destination $t.DirectoryName -Force
      }
      Write-Host ("Deployed {0} NGX DLLs next to {1}" -f $ngxDlls.Count, $t.FullName)
    }
  }
} else {
  Write-Host "WARNING: NGX runtime DLLs not found at $ngxSource - DLSS will be unavailable in deployed builds."
}