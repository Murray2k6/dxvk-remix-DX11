# dxvk-remix DX11

A Direct3D 11 fork of [NVIDIA's dxvk-remix](https://github.com/NVIDIAGameWorks/dxvk-remix) that swaps the upstream D3D9 frontend for a D3D11 frontend while keeping the RTX Remix runtime, USD export, material editing, and path-traced rendering pipeline.

Release `0.4` is packaged to match the local `_output` deployment layout exactly.

## Overview

This project provides drop-in `d3d11.dll` and `dxgi.dll` replacements for D3D11 games and emulators. The bridge intercepts D3D11 draws, extracts transforms and geometry, and feeds that data into the RTX Remix renderer.

Current goals:

- Engine-agnostic D3D11 capture with no per-game matrix config by default
- Stable Remix-owned presentation rather than a DXVK HUD-driven front layer
- Better compatibility with games, custom engines, and D3D11-based emulators
- Release artifacts that can be copied directly from the packaged archive into a target runtime directory

## Highlights

- Automatic projection, view, and world extraction from bound constant buffers
- Row-major and column-major matrix handling with convention auto-detection
- Automatic Y-flip, handedness, and axis-orientation correction
- Render-pass filtering to skip shadow, depth-only, and obvious helper passes
- TAA jitter stripping for more stable RT camera extraction
- Deferred-context safety and pinned async geometry hashing
- Support for normal non-skinned geometry and D3D11 skinned input streams when `BLENDWEIGHT` and `BLENDINDICES` are present
- Delay-loaded `RemixParticleSystem.dll` so it remains under `usd/plugins/`

## Compatibility

This fork targets applications that render through Direct3D 11, including:

- Unreal Engine 4 titles
- Unity titles using D3D11
- Custom engines
- Emulators such as Dolphin, Pcsx2 etc when configured for D3D11

Hardware and driver requirements are still the upstream RTX Remix requirements. This project changes the frontend capture path, not the underlying RTX Remix renderer requirements.

The runtime is also self-contained: sibling DLLs are resolved from the runtime directory, so the Remix payload can live in a separate folder from the game or emulator executable when the target loader supports it.

## Runtime Controls

- `Alt+X`: open or close the RTX Remix UI
- `Alt+Delete`: toggle the Remix UI cursor while the UI is open
- `Alt+Backspace`: toggle whether the game continues receiving input while the Remix UI is open

`Alt+Backspace` changes input routing only. It does not disable Remix rendering or change capture state.

## Config Files

Keep DXVK-facing settings and RTX-facing settings separate:

- Put `dxgi.*`, `d3d11.*`, and `dxvk.*` settings in `dxvk.conf`
- Put `rtx.*` settings in `rtx.conf`

### Example `dxvk.conf`

```ini
# Delay surface creation until the first real Present.
dxgi.deferSurfaceCreation = True

# Leave vendor spoofing disabled unless a specific title needs it.
dxgi.nvapiHack = False

# Let DXVK choose tear-free behavior automatically.
dxgi.tearFree = Auto

# Keep compiler thread count adaptive.
dxvk.numCompilerThreads = 0

# Leave raw SSBO handling automatic.
dxvk.useRawSsbo = Auto

# Apply the NVIDIA HVV heap workaround only when DXVK decides it is needed.
dxvk.halveNvidiaHVVHeap = Auto

# Keep the DXVK HUD off so Remix owns the visible final frame.
dxvk.hud = none
```

### Example `rtx.conf`

```ini
# Keep automatic projection correction enabled.
rtx.camera.correctProjectionYFlip = True

# Keep the RT/PT path enabled.
rtx.enableRaytracing = True

# Preserve the last valid RT scene briefly during short camera gaps.
rtx.sceneKeepAliveFrames = 2

# Start with the UI hidden.
rtx.showUI = 0

# Use the legacy game-window UI input path.
rtx.useNewGuiInputMethod = False

# Show the Remix cursor while the UI is open.
rtx.showUICursor = True

# Block game input while the UI is open by default.
rtx.blockInputToGameInUI = True

# Do not warp the game cursor back on UI close unless a title needs it.
rtx.restoreCursorPosition = False

# Keep decal blending enabled for compatibility.
rtx.enableDecalMaterialBlending = True

# Use the local replacement-content path by default.
rtx.baseGameModPathRegex = ""
```

These are example defaults, not a mandatory full config. Start small and only keep overrides that are actually helping your target.

## Release Layout

The `0.4` release archive mirrors `_output` directly instead of inventing a second packaging structure.

Top-level release contents include:

- Runtime DLLs such as `d3d11.dll`, `dxgi.dll`, `NRD.dll`, `NRC_Vulkan.dll`, `nvngx_dlss.dll`, and the other Remix runtime dependencies
- Symbol files `d3d11.pdb` and `dxgi.pdb`
- Import and export files `d3d11.lib`, `d3d11.exp`, `dxgi.lib`, and `dxgi.exp`
- `dxvk.conf`
- `rtx.conf`
- The full `usd/` tree, including `usd/plugins/RemixParticleSystem/RemixParticleSystem.dll`

## Installation

### Standard game-directory install

1. Extract the release archive.
2. Copy the archive contents into the target game directory.
3. Confirm that `d3d11.dll`, `dxgi.dll`, the runtime DLLs, and the `usd/` directory stay together.
4. Do not move `usd/plugins/RemixParticleSystem/RemixParticleSystem.dll` next to the executable.

### Emulator or separated-runtime install

1. Extract the release archive into a dedicated runtime folder.
2. Keep all packaged files together in that folder.
3. Configure the emulator or loader to use the custom `d3d11.dll` and `dxgi.dll` from that runtime directory.

This layout is intended to work for cases where you do not want to scatter Remix files directly beside the emulator executable.

## Building From Source

### Requirements

1. Windows 10 or 11
2. [Git](https://git-scm.com/download/win)
3. Visual Studio 2019 or 2022 Build Tools
4. Windows SDK
5. [Meson](https://mesonbuild.com/)
6. Vulkan SDK
7. [Python](https://www.python.org/downloads/) 3.9 or newer

Use a standard 64-bit Python install, not the Microsoft Store build.

### Clone

```powershell
git clone --recursive https://github.com/Murray2k6/dxvk-remix-DX11.git
cd dxvk-remix-DX11
```

If you cloned without submodules:

```powershell
git submodule update --init --recursive
```

### Build

Enable local PowerShell script execution once if needed:

```powershell
Set-ExecutionPolicy -ExecutionPolicy RemoteSigned -Scope CurrentUser
```

Useful build commands:

```powershell
# Default debugoptimized build into build/
.\build.bat

# Explicit release build
.\build.bat release

# Configure only
.\build.bat -ConfigureOnly -BuildSubDir _Comp64DebugOptimized
```

The normal runtime build does not need a separate test build to produce `d3d11.dll` and `dxgi.dll`.

### Package a release locally

After a successful build that populates `_output`, create a release-staged folder and zip:

```powershell
.\package_release.ps1 -Version 0.4
```

That command stages the package under `_release/` and creates a zip that matches the deploy layout.

## How It Works

The D3D11 bridge hooks the immediate-context draw path and performs a capture pipeline that is intended to stay generic across engines:

1. Pre-filter unsupported or obviously non-scene draws
2. Scan bound constant buffers for plausible projection matrices
3. Recover view and world transforms from the surrounding matrix data
4. Auto-detect layout and axis conventions
5. Read input-layout semantics, vertex/index data, and material inputs
6. Submit geometry and materials into the Remix RT pipeline

Recent bridge work also keeps the final presented frame Remix-owned, improves camera fallback behavior, and preserves non-skinned compatibility while enabling D3D11-native skinned geometry submission when usable blend streams are present.

## Deployment Notes

- The packaged release is the authoritative deploy layout.
- If files are not overwriting an existing Remix runtime, you are probably copying into the wrong directory.
- Keep `dxvk.conf` and `rtx.conf` at the runtime root unless you intentionally manage them elsewhere.
- When debugging, avoid turning on the DXVK HUD unless you specifically need it.

## Project Documentation

- [Contributing Guide](/CONTRIBUTING.md)
- [Remix API](/documentation/RemixSDK.md)
- [Rtx Options](/RtxOptions.md)
- [Anti-Culling System](/documentation/AntiCullingSystem.md)
- [Foliage System](/documentation/FoliageSystem.md)
- [GPU Print](/documentation/GpuPrint.md)
- [Opacity Micromap](/documentation/OpacityMicromap.md)
- [Terrain System](/documentation/TerrainSystem.md)
- [Unit Test](/documentation/UnitTest.md)
