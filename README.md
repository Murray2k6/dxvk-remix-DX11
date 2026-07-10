# DXVK-Remix DX11

**Path tracing for Direct3D 11 games, on any GPU.**

This project is a Direct3D 11 fork of [NVIDIA's DXVK-Remix](https://github.com/NVIDIAGameWorks/dxvk-remix) — the runtime behind RTX Remix. Where upstream Remix targets fixed-function D3D8/D3D9 titles, this fork brings the same path-traced rendering, replacement-asset, and modding pipeline to **D3D11 games**: you drop in a `d3d11.dll` + `dxgi.dll`, the layer captures the game's rendering as it happens, and the RTX Remix runtime path-traces the scene in the game's own window.

It works as a **generic capture layer** — no per-game renderer rewrites. Geometry, cameras, materials, and skinning are recovered from the live D3D11 API stream (draw calls, constant buffers, vertex/index buffers, bound textures), so the goal is that **any D3D11 game can work, on NVIDIA, AMD, or Intel GPUs**.

---

## What it does

- **Drop-in frontend** — the game loads this project's `d3d11.dll`/`dxgi.dll` instead of the system ones. No game patches, no engine SDKs.
- **Scene capture from the API stream** — draws are converted into ray-traceable geometry: vertex formats normalized for the Remix interleaver, base/start offsets folded, dynamic buffers snapshotted, skinning replicated from blend weights/indices, mirrored placements winding-corrected.
- **Camera recovery** — projection and view matrices are found by scanning constant buffers, validated structurally, and (where the engine stores one) confirmed against the stored ViewProj product. Frames whose camera can't be resolved pass through to the game's own rendering instead of breaking.
- **Material capture** — albedo textures are selected from bound shader resources with content-based, run-stable hashes, so the Remix texture tagging and replacement workflow (dev menu, viewport picking, USD mods) works the same as on D3D9 titles.
- **Sky, lights, and passes** — skyboxes are auto-detected and become the RT environment; a fallback light guarantees scenes are never unlit; depth-prepass and shadow-map re-renders are excluded so geometry enters the scene exactly once.
- **Full Remix feature set** — the path tracer, denoising infrastructure (NRD), DLSS / XeSS / TAA-U upscaling, Reflex, the Remix Basic/Dev UI, texture categories, and USD replacements all ride along from upstream.
- **x86 bridge for 32-bit games** — a 32-bit client DLL captures the game and streams the scene over IPC to a 64-bit `NvRemixBridge.exe` server that runs the full Remix runtime and presents into the game's window.

## What it is not

- Not a rewrite of the Remix renderer — the path tracer, material system, USD tooling, and option system are upstream DXVK-Remix, extended rather than replaced.
- Not a guarantee that every title works untouched. Modern engines vary wildly; the capture heuristics are built to be engine-agnostic and fail safe (pass through rather than break), and per-game tuning lives in `rtx.conf`.

---

## Installation

### 64-bit games (native path)

Copy the **entire contents** of `_output/x64/` next to the game's executable. The runtime needs its full satellite set — `NRD.dll` (denoiser), `nvngx_*.dll` (DLSS), the USD/XeSS/NRC libraries — not just the two entry DLLs. Feature DLLs (Vulkan, RTXIO, NRC, XeSS, Aftermath, Reflex) are delay-loaded: if one is missing the game still boots and the log names the missing file. The **USD/python set is a hard requirement** — without it the game will not start, and the tell-tale is that `remix-dx11-boot.log` never appears (the DLL could not load at all).

### 32-bit games (bridge path)

Copy the bridge package next to the game's executable:

```text
<game folder>/
  d3d11.dll              (x86 capture client)
  dxgi.dll               (x86)
  NvRemixLauncher32.exe
  .trex/                 (x64 Remix runtime + full satellite payload)
    NvRemixBridge.exe
    d3d11.dll
    dxgi.dll
    NRD.dll, nvngx_*.dll, usd*.dll, ...
```

### Configuration files

- `dxvk.conf` — DXVK/DXGI options (fullscreen emulation, frame latency).
- `rtx.conf` — project-level Remix options and per-game overrides.
- `user.conf` — startup defaults the in-game UI can change and save (upscaler, quality, path length).
- `bridge.conf` — bridge IPC settings (x86 path only).

## Runtime controls

- **Alt + X** — open/close the Remix menu (Basic or Developer).
- **Alt + Del** — toggle the Remix cursor. **Alt + Backspace** — toggle game-input blocking while the menu is open.
- In the Dev menu's texture tab, hover/click objects in the game viewport to select and tag their textures for replacement.

---

## Building from source

Requirements: Windows, Visual Studio 2022+ (MSVC x64 and x86 toolsets), and the checked-in dependency set.

```bat
build.bat
```

The orchestrator (`build_dxvk_all_ninja.ps1`) builds the x64 runtime, compiles the RTX shaders, builds the x86 bridge client and x64 bridge server, and stages everything into `_output/` with the complete satellite payload. `package_release.ps1` zips a release layout.

## Troubleshooting

The runtime writes logs next to the game executable, built to make failures diagnosable from a single file:

- `remix-dx11-boot.log` is written the **instant the DLLs attach**, before anything else can fail. If the game won't start and even this file is absent, the DLLs never loaded: an **anti-cheat blocked them** (see below), the architecture is wrong (x86 game needs the bridge package), or a required DLL is missing.
- The first lines of the main log are a **build stamp** listing every fix in the binary — a log missing the current stamp means a stale DLL is loaded.
- `[Remix-DX11][init]` lines mark each boot step; a hung launch shows exactly the last completed step.
- `[Remix-DX11][crash]` lines record the faulting module and offset if the process dies.
- `MISSING SATELLITE DLL` lines mean the payload copy is incomplete — the game still boots, but the named feature DLL must be copied next to the game exe.
- `[Remix-DX11] NRD denoiser loaded` / `Unable to load NRD` state denoiser status explicitly.

**Anti-cheat games (Halo: MCC, and others shipping EasyAntiCheat/BattlEye):** anti-cheat blocks replacement `d3d11.dll`/`dxgi.dll` outright — the game exits with no logs at all. Remix does not and will not bypass anti-cheat. Use the game's **official anti-cheat-disabled / modding launch mode** (e.g. Halo MCC's "Anti-Cheat Disabled" option in the Steam launch menu). The runtime detects nearby anti-cheat installs and prints this reminder in both logs.

Useful environment switches: `DXVK_REMIX_MTXDUMP=1` (dump constant-buffer matrices for camera debugging), `DXVK_REMIX_PREWARM=1` (opt into shader prewarm), `DXVK_REMIX_USE_NRD=0` (disable NRD pre-composition denoising), `DXVK_REMIX_ENABLE_NRC=1` (opt into the Neural Radiance Cache on supported NVIDIA GPUs), `DXVK_REMIX_TEXCOORD_CAPTURE=0` (disable the stream-out UV recovery for draws whose input layout has no TEXCOORD).

---

## Credits

Thanks to everyone who tested builds, reported issues, contributed fixes, and pushed this project forward:

- **Demoflower**
- **Sparkles (Kim)**
- **Rafeal Santino Supertux**
- **frisser**
- **Behon**
- **Clouds**
- **SW491**

## License

This project inherits its licensing from its upstream components:

- **DXVK** — zlib/libpng license (see `LICENSE`).
- **NVIDIA DXVK-Remix / RTX Remix runtime** — NVIDIA's license terms (see `LICENSE-MIT` and upstream notices).
- Third-party SDKs (DLSS, NRD, NRC, XeSS, USD, and others) remain under their respective licenses in `external/` and `submodules/`.

This is a community fork and is not affiliated with or endorsed by NVIDIA.
