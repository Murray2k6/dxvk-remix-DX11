# DX11 builds and deployment

The supported build entry point is `build.bat`, which runs
`build_dxvk_all_ninja.ps1`. It builds the checked-in source without generating
replacement C++ files. The active 32-bit frontend is under
`bridge_dx11_work/src/client_dx11`; the top-level `src/client_dx11` directory is
not the frontend compiled by this build.

## Build

Use Windows with the MSVC x86 and x64 tools, a Windows SDK, Ninja, Meson and
Python 3.10 or newer. The driver discovers Visual Studio, selects the correct
tool architecture per build directory and uses local Meson modules when present
under `build_deps/python`. Pass `-PythonExecutable` to select an interpreter
explicitly. The release configuration has been compiled with MSVC 19.51,
Meson 1.11.1 and Ninja 1.13.2.

```powershell
.\build_dxvk_all_ninja.ps1 -BuildFlavour release -Jobs 6
```

The driver checks and fetches manifest dependencies. `-NoDepsFetch` makes missing
dependencies an error. The USD plugin uses the root USD SDK and its own manifest
Python. If its schema generator reports missing Python modules, install the
checked-in `src/usd-plugins/requirements-schema.txt` into that Python environment.
The checked SDK versions must be present before claiming a debug build is usable;
release validation does not validate debug SDK binaries.

The build directories are `_Comp64release` for the runtime,
`bridge_dx11_work/_Comp64release` for the server, and
`bridge_dx11_work/_Comp32release` for the client and launcher. Per-step logs are
in `_build_logs`. Shader compilation uses the canonical source files and a
transitive depfile; unchanged shader builds do not rerun the compiler.
The offline shader compiler defaults to four workers. Set
`DXVK_SHADER_COMPILER_JOBS` explicitly to choose between one and sixteen.
Runtime preload uses observed pipeline state, bounded CPU/memory concurrency,
and cancellation at device teardown. Scanning game archives is opt-in through
`DXVK_GAME_SHADER_SCAN=1` or the developer menu.

Useful switches are `-ConfigureOnly`, `-RuntimeOnly`, `-BridgeOnly`, `-NoStage`
and `-SkipZip`. `-StageOnly -SkipZip` assembles existing built binaries without
rebuilding. `build_dxvk.ps1 -Architecture x86` also routes through the complete
client/server/runtime build because the 32-bit package requires all three.

## Deploy the complete architecture folder

For a 64-bit game, copy all contents of `_output/x64` beside the game executable.
Keep `d3d11.dll`, `dxgi.dll`, the satellite libraries and the `usd` directory
together. D3D11 explicitly loads its sibling DXGI when adapting a system-created
adapter. The local DXGI factory therefore participates even when the game first
obtained its adapter from the system DXGI. Adapter selection matches the actual
requested adapter instead of assuming the first enumerated GPU.

When the proxy receives a native Windows D3D11 device or D3D12 command queue,
it forwards swapchain creation and presentation to system DXGI. This preserves
native presentation for paths such as Starfield's D3D12 renderer; it does not
provide D3D12 Remix capture or ray tracing. Use the matching DXGI/D3D11 pair.

For a 32-bit game, copy all contents of `_output/x86`, including `.trex`:

```text
game directory/
  d3d11.dll                 x86 capture frontend
  dxgi.dll                  x86 system-DXGI forwarding and swapchain hooks
  NvRemixLauncher32.exe     x86 bridge startup helper
  .trex/
    NvRemixBridge.exe       x64 server
    d3d11.dll               x64 Remix runtime
    dxgi.dll                matching x64 DXGI
    bridge.conf
    bridge_version.txt
    ...                     complete x64 satellite and USD payload
```

The x86 DXGI frontend does not own an independent IPC connection. D3D11 owns
startup and sends a window-aware Remix Startup request; capture is enabled only
after the server acknowledges successful runtime initialization. Both bridge
binaries carry the same protocol version (`dx11-4` in this source). Do not mix
client/server files from different builds. The launcher gives the x64 server its
own DLL search environment and monitors the real game process; the game process
does not inherit the x64 DLL path. The server and launcher terminate when that
game exits. After its command threads stop, the server calls the Remix API's
`Shutdown` and waits for device resources and background work to retire before
normal process exit. Shutdown errors and an exceeded cleanup timeout are logged
as failures.

The frontend updates the HWND on each presentation, including a switch from a
splash window to the main window. Remix presents to its own independently pumped
child window so the native DXGI swapchain and Vulkan do not compete for the same
window. That child is hidden during native fallback. Native presentation is retained until the
current frame has captured geometry, a current world camera and an acknowledged,
completed ray-traced output. The server drains queued CPU presentation work
before handing the window back to the native swapchain.

The mesh cache owns and releases server meshes with bounded memory and entry
counts. Native resource lifetime tags release CPU buffer/shader/texture/layout
metadata when the corresponding D3D11 resource is destroyed.

## Capture limits

The x64 native frontend admits up to 2,097,152 post-VS vertices in one draw,
including its instances. Large triangle lists replay in chunks of at most
262,144 vertices while preserving the original instance range and IA divisors.
The default aggregate capture budget is 96 MiB per frame, separately from a
384 MiB capture-cache limit. Indexed capture expands each triangle to three
vertices; triangle count and actual capture stride determine its cost.
Unchanged exact captures reuse their buffers. Budget exhaustion retains the
complete native frame; it does not display stale poses or an incomplete world.

GPU-generated indirect arguments and stream-output `DrawAuto` counts are not
CPU snapshots. These calls now retain native rendering and prevent subsequent
RTX injection for that frame, without a blocking readback or guessed counts.
The regression mixes each path with captured direct geometry, checks native
pixels and the completed-RTX counter, then verifies direct-only recovery.
This does not implement ray-traced capture of GPU-driven draw counts. If an
unsupported call arrives after an earlier UI-triggered RTX composite has
already executed, the original native target cannot be recovered retroactively;
that interleaved rendering path still needs game-specific validation.

The x86 capture frontend currently reconstructs supported `Draw` and
`DrawIndexed` geometry. It does not reconstruct arbitrary per-instance shader
transforms, GPU-generated draw arguments, stream-output draws or deferred
command-list contents. Nonempty instanced calls, indirect/DrawAuto calls with
unknown counts, and executed deferred command lists retain native presentation
for that frame, including when mixed with successfully captured geometry.
Known zero-count instanced draws do not prevent a complete captured frame from
presenting. This avoids showing a partial scene; it does not claim those drawing
paths have been converted to ray tracing.

The generic camera/material heuristics still need testing against each game's
rendering path. A successful initialization test does not establish complete
capture support for every game or GPU.

## Validation

`tests/d3d11/test_remix_initialization.cpp` loads the paired x64 DLLs and validates
API ownership, overlapping devices, complete release and recreation, and two
concurrent create/render/release threads. It reads back native 2D, 3D and depth
clears, including restricted attachment views, and requires balanced device
generations with normal process exit and clean Vulkan validation. The x86 test
`tests/d3d11/test_dx11_bridge_initialization.cpp` exercises real client/server
startup, two distinct game HWNDs and acknowledged frames with a bounded timeout.
It requires the newly written startup/presentation log messages, so a native-only
fallback cannot count as a bridge initialization pass. Its lifecycle runner also
requires explicit API shutdown, retirement of the final shared device and normal
server/launcher exit; successful game presentation alone is insufficient.

`tests/rtx/unit/test_dx11_frame_capture_coverage.cpp` uses the production capture
eligibility code to check mixed draw orderings, empty draws, camera freshness and
frame transitions on both architectures. The separate
`tests/d3d11/test_remix_rendering.cpp` performs actual ray-traced rendering and
readback; its results must be considered separately from initialization checks.

`test_remix_capture.cpp` exercises actual D3D11 shaders, mutable/GPU buffers,
index ranges, instancing and fullscreen composites through native Present. Its
depth checks reject camera obstruction. `test_dxgi_native_presentation.cpp`
exercises native D3D12 and D3D11 presentation through the DXGI proxy. Test meshes
and mod fixtures belong to isolated validation executables/directories; the
architecture payloads do not include a test scene or a box around the camera.

`package_release.ps1` creates `_release/dxvk-remix-dx11-<version>.zip`. Packaging
preserves hidden files and directories, including `.trex`.
