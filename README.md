# DXVK-Remix DX11 Architecture

A Direct3D 11 frontend for NVIDIA DXVK-Remix-style rendering. This project focuses on receiving D3D11 game rendering commands, translating/capturing scene data through a DXVK/Vulkan-backed path, and submitting usable geometry, materials, camera data, and frame presentation into the RTX Remix runtime pipeline.

This README intentionally describes the **DX11 architecture only**. It does not document the original D3D8/D3D11 fixed-function Remix bridge path except where needed to explain what this fork replaces.

---

## Project Summary

NVIDIA RTX Remix is a runtime and tooling ecosystem for remastering older games with modern rendering features such as path tracing, physically based replacement assets, capture/replay workflows, RTX lighting, DLSS, and related runtime systems.

DXVK-Remix is the runtime-side graphics translation layer used by RTX Remix. It is based on DXVK, but it adds Remix-specific capture, material, geometry, lighting, USD, UI, and path-traced rendering systems on top of the normal Direct3D-to-Vulkan translation idea.

This DX11-focused architecture replaces the traditional D3D11-facing frontend with a Direct3D 11 frontend:

- The game loads `d3d11.dll` and/or `dxgi.dll` from this project.
- The frontend interposes on D3D11 device, context, swap chain, resource, shader, input-layout, and draw calls.
- The D3D11 state is inspected and converted into Remix-friendly scene information.
- The backend uses Vulkan/DXVK-Remix infrastructure for resource management, rendering, presentation, and RTX Remix integration.
- The final frame is owned by the Remix runtime path instead of being a plain game swap-chain present.

The goal is to make D3D11 applications and emulators visible to the Remix pipeline without requiring per-game renderer rewrites.

---

## What This Project Is

This project is a **D3D11 capture and translation frontend for DXVK-Remix**.

It is meant to:

- Be used as a drop-in `d3d11.dll` / `dxgi.dll` frontend interposer (DLL replacement).
- Capture D3D11 draw calls and render state.
- Detect camera, projection, world, and view transforms from D3D11 constant buffers.
- Extract geometry from D3D11 vertex/index buffers.
- Identify material inputs from bound shader resources and render state.
- Filter non-scene passes such as shadow maps, depth-only helpers, UI passes, and transient post-processing where possible.
- Submit scene data into the Remix runtime for path tracing, replacement assets, material editing, capture, and presentation.
- Keep the deployment model close to normal DXVK/Remix DLL replacement workflows.

---

## What This Project Is Not

This project is not a complete rewrite of RTX Remix.

It does not replace:

- The Remix path-traced renderer.
- Remix material and replacement-asset systems.
- USD capture/export/import systems.
- DLSS, NRD, NRC, Reflex, or NGX integration code.
- Vulkan device management inside DXVK-Remix.
- The core RTX option system.
- The ImGui/Remix user interface.

It also does not guarantee that every D3D11 game will work automatically. D3D11 games are much less uniform than fixed-function-era D3D8/D3D11 games. Modern engines use custom shaders, deferred renderers, temporal effects, compute passes, dynamic buffers, engine-specific culling, skinned meshes, clustered lighting, and post-processing chains that must be handled carefully.

---

## High-Level DX11 Architecture

```text
D3D11 Game / Emulator
        |
        v
Drop-in d3d11.dll / dxgi.dll
        |
        v
D3D11 Frontend Capture Layer
        |
        |-- Device and swap-chain creation hooks
        |-- Immediate-context draw interception
        |-- Deferred-context safety handling
        |-- Resource and view tracking
        |-- Shader and input-layout tracking
        |-- Constant-buffer matrix scanning
        |-- Vertex/index buffer extraction
        |-- Material texture candidate selection
        |-- Render-pass filtering
        |
        v
DXVK / Vulkan Translation Layer
        |
        |-- Vulkan device
        |-- Memory allocator
        |-- Image and buffer objects
        |-- Pipeline state translation
        |-- Synchronization
        |-- Presentation
        |
        v
RTX Remix Runtime Integration
        |
        |-- Camera data
        |-- Geometry data
        |-- Material data
        |-- Replacement assets
        |-- USD capture/export
        |-- RTX options
        |-- Remix UI
        |-- Path-traced output
        |
        v
Final Presented Frame
```

---

## Core Runtime Flow

### 1. DLL Injection by Normal Windows Loader Rules

The target application loads this project's `d3d11.dll` and/or `dxgi.dll` from the application directory or configured loader path.

This frontend provides the expected Direct3D and DXGI entry points and forwards or implements them through the DXVK-Remix runtime path.

Important entry points include:

- `D3D11CreateDevice`
- `D3D11CreateDeviceAndSwapChain`
- DXGI factory creation
- Swap-chain creation and presentation
- Device/context query interfaces

### 2. D3D11 Device Creation

When the game creates a D3D11 device, the frontend creates or retrieves the matching DXVK-backed device objects.

The frontend must preserve the behavior expected by the game while also attaching Remix-specific systems:

- Feature-level negotiation
- DXGI adapter selection
- Vulkan physical-device selection
- Device extension probing
- Swap-chain compatibility
- RTX option initialization
- Remix UI initialization
- Runtime dependency loading

The device path must be stable even when NVIDIA-specific optional systems are unavailable. Unsupported DLSS, NGX, Reflex, NRC, SER, or OMM paths should disable cleanly instead of crashing device creation.

### 3. Swap-Chain Creation and Ownership

The D3D11 frontend intercepts swap-chain creation through DXGI.

Responsibilities include:

- Tracking the application `HWND`.
- Tracking buffer format, size, count, and present mode.
- Handling fullscreen and borderless transitions.
- Recreating Vulkan swap-chain resources when the game resizes or changes display mode.
- Deciding which swap chain is the primary game output.
- Ensuring Remix owns the final composited frame.

A DX11 Remix path should avoid treating the DXVK HUD or plain backbuffer as the final output when Remix rendering is active.

### 4. Draw Call Interception

The frontend watches D3D11 draw calls on the immediate context.

Common draw calls include:

- `Draw`
- `DrawIndexed`
- `DrawInstanced`
- `DrawIndexedInstanced`
- `DrawAuto`

For each relevant draw, the frontend inspects the active D3D11 state:

- Input layout
- Vertex buffers
- Index buffer
- Primitive topology
- Vertex shader
- Pixel shader
- Constant buffers
- Shader resource views
- Samplers
- Render-target views
- Depth-stencil state
- Rasterizer state
- Blend state
- Viewports and scissors

The capture layer then decides whether the draw looks like real scene geometry or a pass that should be ignored.

### 5. Render-Pass Filtering

D3D11 games produce many passes that are not useful as Remix scene geometry.

The frontend should filter or down-rank:

- Shadow-map passes
- Depth-only pre-passes
- Screen-space fullscreen triangles/quads
- UI and HUD passes
- Bloom and post-processing passes
- TAA resolve passes
- SSAO/SSR helper passes
- Intermediate render targets
- Browser/video overlays
- Compute-only helpers

Filtering is heuristic. It uses viewport size, render-target format, depth state, shader-resource patterns, primitive count, topology, known transient surfaces, and whether camera/projection data is valid.

### 6. Camera and Matrix Recovery

A DX11 game usually does not expose fixed-function matrices. The frontend must recover camera data from shader constants.

The matrix scanner searches bound constant buffers for plausible:

- Projection matrices
- View matrices
- World matrices
- View-projection matrices
- World-view-projection matrices
- Previous-frame matrices

The scanner should account for:

- Row-major and column-major packing
- Left-handed and right-handed conventions
- Reversed-Z projections
- Y-flip differences
- Jittered projection matrices from TAA
- Infinite far planes
- Engine-specific clip-space conventions
- Per-object matrices packed into arrays

The result is a camera model Remix can use for path tracing and scene submission.

### 7. Geometry Extraction

Once a draw is accepted, the frontend extracts geometry from the active D3D11 buffers.

The extraction path reads:

- Vertex buffer bindings
- Stride and offset
- Index buffer format and offset
- Input-layout semantics
- Position attributes
- Normal attributes
- Tangent attributes
- Texture coordinates
- Color attributes
- Blend weights and blend indices when skinned data is present

The geometry is converted into a Remix-friendly representation and associated with the recovered transform data.

For skinned meshes, the frontend can only submit correct results when enough usable information is present. If the game performs skinning entirely in shaders or compute buffers without readable semantic mapping, the frontend may need fallback behavior.

### 8. Material and Texture Detection

The frontend inspects pixel-shader resources and render state to choose material inputs.

Possible material sources include:

- Diffuse/albedo textures
- Normal maps
- Roughness/metalness/specular maps
- Emissive textures
- Alpha masks
- Render-target-sized intermediate textures
- Scene color/depth buffers

The frontend should avoid treating transient render targets or screen-space buffers as object materials. It should score candidates by dimensions, format, binding flags, mip count, shader slot, render-target history, and whether the resource looks like a real asset texture.

### 9. Scene Submission into Remix

Accepted scene draws are submitted into the Remix runtime with:

- Object identity or hash
- Geometry buffers
- Index buffers
- Transform data
- Material references
- Texture references
- Camera state
- Instance information
- Frame timing and presentation metadata

The Remix runtime can then apply replacement assets, material overrides, lighting changes, capture/export behavior, and path-traced rendering.

### 10. Presentation

At the end of the frame, the frontend coordinates with the Remix renderer and swap chain.

The final path should:

- Submit all accepted scene data.
- Allow Remix to render or composite the final frame.
- Present through the Vulkan/DXGI-backed swap-chain path.
- Preserve game timing and resize behavior.
- Avoid injecting RTX output during invalid startup/loading frames when no trusted camera or scene exists.

---

## Important Components

### `d3d11.dll`

The Direct3D 11 interposer and frontend implementation.

Responsibilities:

- Export expected D3D11 entry points.
- Create wrapped D3D11 devices and contexts.
- Track D3D11 rendering state.
- Intercept draw calls.
- Extract camera, geometry, material, and resource information.
- Submit accepted scene data to the Remix path.

### `dxgi.dll`

The DXGI interposer and swap-chain layer.

Responsibilities:

- Export expected DXGI factory and adapter entry points.
- Create and wrap swap chains.
- Track display mode changes.
- Handle fullscreen/borderless transitions.
- Own final presentation coordination.

### DXVK Core

The translation and Vulkan backend layer.

Responsibilities:

- Translate D3D11/DXGI concepts into Vulkan-backed objects.
- Manage Vulkan devices, queues, buffers, images, synchronization, and pipeline state.
- Provide the low-level rendering infrastructure used by Remix.

### RTX Remix Runtime Layer

The Remix-specific rendering and content layer.

Responsibilities:

- RTX option system
- Remix UI
- Scene capture
- USD integration
- Material system
- Replacement assets
- Path-traced rendering
- Denoising and reconstruction integrations
- Final composition

---

## Supported Target Type

This DX11 architecture targets applications that use Direct3D 11 as their primary rendering API.

Examples include:

- Unreal Engine 4 games using D3D11
- Unity games using D3D11
- Custom D3D11 engines
- D3D11 renderers in emulators
- Older PC games with D3D11 render paths
- Tools or test applications using standard D3D11 draw calls

Best results are expected when the application uses a conventional 3D scene pipeline with stable camera matrices, readable vertex/index buffers, and standard shader resource bindings.

---

## Current Limitations

D3D11 support is harder than classic fixed-function D3D11 support because D3D11 exposes less semantic meaning to the wrapper.

Expected limitations include:

- Some games may not expose clean camera matrices.
- Some engines use heavily packed or obfuscated constant buffers.
- Some games perform skinning, culling, or geometry generation in compute shaders.
- Some deferred renderers may make it difficult to distinguish scene geometry from intermediate passes.
- Some UI and video overlays may look like scene draws.
- Some engines use reversed-Z, custom clip space, jittered projections, or nonstandard transforms.
- Some resource hazards may prevent safe readback of vertex/index data.
- Anti-cheat protected games should not be targeted.
- Multiplayer or competitive games may reject injected graphics DLLs.
- RTX-specific features may depend on GPU, driver, and Vulkan extension support.

The correct fallback behavior is to skip unsafe or untrusted Remix injection for a frame rather than crash the game.

---

## Configuration Files

Keep DXVK-facing and RTX-facing settings separate.

### `dxvk.conf`

Use for DXGI, D3D11, and DXVK translation behavior.

Example:

```ini
# Delay surface creation until a real presentation path exists.
dxgi.deferSurfaceCreation = True

# Leave vendor spoofing disabled unless a specific title needs it.
dxgi.nvapiHack = False

# Let DXVK choose tear-free behavior automatically.
dxgi.tearFree = Auto

# Keep compiler thread count adaptive.
dxvk.numCompilerThreads = 0

# Keep the DXVK HUD disabled so Remix owns the visible output.
dxvk.hud = none
```

### `rtx.conf`

Use for Remix runtime behavior.

Example:

```ini
# Enable the RTX rendering path.
rtx.enableRaytracing = True

# Start with the Remix UI hidden.
rtx.showUI = 0

# Keep automatic camera correction enabled.
rtx.camera.correctProjectionYFlip = True

# Preserve a scene briefly during short camera gaps.
rtx.sceneKeepAliveFrames = 2

# Use the newer Remix UI input path.
rtx.useNewGuiInputMethod = True

# Show the Remix cursor while the UI is open.
rtx.showUICursor = True

# Block game input while the Remix UI is open.
rtx.blockInputToGameInUI = True
```

Only keep overrides that help the target application. Bad config values can make debugging much harder.

---

## Runtime Controls

Default Remix-style controls:

- `Alt + X` opens or closes the Remix UI.
- `Alt + Delete` toggles the Remix cursor while the UI is open.
- `Alt + Backspace` toggles whether the game receives input while the Remix UI is open.

Input handling should never crash if the UI is not fully initialized. If an option callback fires before ImGui has a valid context, the callback should be deferred and replayed after the context exists.

---

## Deployment Layout

A DX11 deployment should keep all runtime files together.

Recommended layout:

```text
GameFolder/
    Game.exe
    d3d11.dll
    dxgi.dll
    dxvk.conf
    rtx.conf
    NRD.dll
    NRC_Vulkan.dll
    nvngx_dlss.dll
    nvngx_dlssg.dll
    Remix runtime dependencies...
    usd/
        plugins/
            RemixParticleSystem/
                RemixParticleSystem.dll
```

Do not scatter runtime DLLs into unrelated folders unless the loader path is intentionally configured for that structure.

---

## Installation

### Standard Game Install

1. Build or download the DX11 runtime package.
2. Copy the package contents next to the target game executable.
3. Confirm `d3d11.dll`, `dxgi.dll`, `dxvk.conf`, `rtx.conf`, and the `usd/` folder are together.
4. Start the game normally.
5. Use `Alt + X` to check whether the Remix UI opens.
6. Check `remix-dxvk.log` if the game crashes, shows a black screen, or never reaches the Remix UI.

### Emulator Install

1. Set the emulator renderer to D3D11.
2. Place the DX11 runtime package where the emulator will load custom D3D11/DXGI DLLs.
3. Keep the runtime directory intact.
4. Start with a simple title or test scene.
5. Enable more aggressive RTX settings only after basic presentation is stable.

---

## Building From Source

Typical requirements:

- Windows 10 or Windows 11
- Git
- Visual Studio 2019 or 2022 Build Tools
- Windows SDK
- Python 3.9 or newer
- Meson
- Ninja
- Vulkan SDK
- DirectX runtime dependencies

Basic build flow:

```powershell
git clone --recursive <this-repository>
cd dxvk-remix-DX11

# If submodules were not cloned:
git submodule update --init --recursive

# Allow local build scripts if needed:
Set-ExecutionPolicy -ExecutionPolicy RemoteSigned -Scope CurrentUser

# Build using the project script:
.\build.bat
```

Expected build outputs include runtime DLLs such as `d3d11.dll` and `dxgi.dll`, plus the Remix runtime dependency tree needed for deployment.

---

## Debugging Launch Crashes

Start with the log files:

- `remix-dxvk.log`
- Game engine crash logs
- Windows Event Viewer application crash entries
- Minidumps when available

Common launch-crash areas:

- D3D11 device creation
- DXGI swap-chain creation
- Vulkan device or extension setup
- Optional NVIDIA feature initialization on unsupported hardware
- RTX option callbacks firing before dependent systems are initialized
- ImGui setup before an ImGui context exists
- Missing runtime DLLs
- Wrong architecture, such as x86 game with x64 runtime DLLs
- Incorrect deployment folder

A robust DX11 frontend should treat unsupported optional features as disabled, not fatal. Device creation should continue when DLSS, NGX, Reflex, NRC, SER, OMM, or other optional paths are unavailable.

---

## Debugging Rendering Problems

If the game launches but the scene is wrong, investigate in this order:

1. Confirm the primary swap chain is detected correctly.
2. Confirm the game is using D3D11 and not D3D12, Vulkan, OpenGL, or another backend.
3. Confirm real scene draws are being accepted.
4. Check whether fullscreen post-processing draws are being filtered.
5. Check camera validity.
6. Check projection matrix detection.
7. Check world/view transform extraction.
8. Check vertex/index buffer extraction.
9. Check material texture scoring.
10. Check whether skinned meshes need a dedicated path.
11. Check whether UI/HUD is being submitted as geometry.
12. Check whether RTX injection is being skipped because the frame is not trusted.

Useful logging categories include:

- Device creation
- Swap-chain creation and recreation
- Primary swap-chain claiming
- Draw counts
- Camera detection
- Matrix candidates
- Material candidates
- Scene acceptance/rejection
- End-frame submission
- Present count

---

## Design Rules for the DX11 Frontend

1. **Do not crash on missing optional RTX features.**  
   Unsupported GPU features should disable cleanly.

2. **Do not submit untrusted frames.**  
   If the camera is invalid or scene data is clearly not real geometry, pass through instead of forcing RTX injection.

3. **Prefer heuristics over per-game hardcoding.**  
   Per-game profiles can help later, but the default path should be engine-agnostic.

4. **Keep presentation Remix-owned.**  
   The final visible frame should come from the Remix path when Remix rendering is active.

5. **Keep config separation clean.**  
   DXVK/DXGI/D3D11 settings belong in `dxvk.conf`; RTX runtime settings belong in `rtx.conf`.

6. **Never assume D3D11 exposes fixed-function meaning.**  
   Recover meaning from shaders, buffers, resources, and draw state.

7. **Defer initialization callbacks safely.**  
   UI and option callbacks may happen before all runtime systems exist. Defer and replay them when dependencies are valid.

8. **Fail open when possible.**  
   If Remix cannot process a frame, the game should still present normally where possible.

---

## Roadmap

High-priority DX11 work:

- Improve camera and projection detection across engines.
- Improve scene/pass filtering for deferred renderers.
- Improve skinned mesh capture.
- Improve material texture candidate scoring.
- Improve deferred-context handling.
- Improve fullscreen/borderless swap-chain handling.
- Improve startup/loading-frame pass-through behavior.
- Improve fallback behavior on non-NVIDIA hardware.
- Improve logging for accepted/rejected scene draws.
- Add targeted test scenes for D3D11 matrix layouts and resource bindings.

Longer-term work:

- Better engine fingerprints without hardcoding game-specific hacks.
- More reliable motion vector and previous-frame transform handling.
- Better compute-generated geometry detection.
- More robust capture for emulators using D3D11.
- Automated validation using RenderDoc captures and known test scenes.

---

## Compatibility Notes

This DX11 frontend changes how Remix receives game rendering commands. It does not remove the hardware, driver, and feature requirements of the underlying Remix renderer.

The runtime should still run defensively on unsupported hardware by disabling unsupported optional features. However, path tracing, denoising, ray reconstruction, frame generation, and related RTX features may depend on GPU support, driver support, Vulkan extensions, and runtime DLL availability.

---

## Repository Purpose

This repository exists to explore a D3D11-native path into the DXVK-Remix / RTX Remix runtime stack.

The core idea is:

```text
D3D11 game calls -> DX11 capture frontend -> DXVK/Vulkan runtime -> RTX Remix renderer -> Remix-owned final frame
```

That is the architecture this README documents.

---

## License

This DX11 fork keeps the original license structure from DXVK and DXVK-Remix. Do not remove or replace the root license files when redistributing source code or binary packages.

### DXVK Base License

DXVK-derived code is covered by the **zlib/libpng license**. The original DXVK copyright notices include:

```text
Copyright (c) 2017-2021 Philip Rebohle
Copyright (c) 2019-2021 Joshua Ashton
```

The zlib/libpng license allows use, modification, and redistribution, including commercial use, provided the original authorship is not misrepresented, altered source versions are clearly marked, and the license notice is preserved in source distributions.

### NVIDIA DXVK-Remix / RTX Remix Runtime License

NVIDIA-authored DXVK-Remix and RTX Remix runtime portions are covered by the repository's **MIT license notice**. The NVIDIA copyright notice is:

```text
Copyright 2021-2023 NVIDIA CORPORATION & AFFILIATES.
All rights reserved.
```

The MIT-covered portions may be used, copied, modified, merged, published, distributed, sublicensed, and/or sold, provided the copyright and permission notice are included in copies or substantial portions of the software.

### Third-Party Notices

This project may include third-party libraries, SDK integrations, tools, headers, and runtime components with their own license terms. Keep the following files with the repository and release packages when applicable:

- `LICENSE`
- `LICENSE-MIT`
- `ThirdPartyLicenses.txt`
- Any additional license files included with bundled runtime DLLs, SDK components, tools, or submodules

### Fork Notice

This repository is a modified DX11-focused fork of NVIDIA DXVK-Remix. Altered source versions must be plainly marked as modified and must not be represented as the original upstream DXVK or NVIDIA DXVK-Remix source.
