# RTX Remix for DX11, DX12 & Vulkan

**Path tracing for Direct3D 11, Direct3D 12 and Vulkan games.**

A community fork of [NVIDIA's DXVK-Remix](https://github.com/NVIDIAGameWorks/dxvk-remix), the runtime behind RTX Remix. Upstream Remix targets fixed-function D3D8/D3D9 games; this fork captures modern games from their live API stream (draws, constant buffers, textures) and path traces them with the Remix renderer, replacement assets and dev tools included. No per-game renderer rewrites.

- **DX11:** drop-in `d3d11.dll` + `dxgi.dll` (32-bit games through the x86 bridge).
- **DX12:** through a patched [vkd3d-proton](https://github.com/HansKristian-Work/vkd3d-proton) and the Remix Vulkan layer, Remix running on the game's own Vulkan device.
- **Vulkan:** the Remix Vulkan layer.

## Install

- **64-bit DX11 games:** copy all of `_output/x64` next to the game executable.
- **32-bit DX11 games:** copy `_output/x86` (with its `.trex` folder) next to the game executable.
- **DX12 games:** the same; `_output/x64` includes vkd3d-proton's `d3d12.dll` and `d3d12core.dll`. Everything stays in the game folder: the Remix Vulkan layer turns itself on, no config or registry step.
- **Vulkan games:** copy `_output/x64` next to the game executable and register the layer once with `install_remix_vk_layer.ps1`.

Games with anti-cheat (EasyAntiCheat, BattlEye, ...) are not supported; Remix stays off next to them.

**Alt + X** opens the Remix menu. Logs are written next to the game executable.

## Build

Windows with Visual Studio 2022 or later:

```bat
build.bat
```

DX12 also needs `build_vkd3d_proton.ps1` (MSYS2 UCRT64). Details: [build and deployment](documentation/DX11_BUILD_AND_DEPLOYMENT.md).

## Credits

Built on:

- [NVIDIA RTX Remix / DXVK-Remix](https://github.com/NVIDIAGameWorks/dxvk-remix) and NVIDIA's RTX SDKs (DLSS, NRD, NRC, RTXDI, SHARC, Reflex)
- [DXVK](https://github.com/doitsujin/dxvk) by Philip Rebohle and contributors
- [vkd3d-proton](https://github.com/HansKristian-Work/vkd3d-proton) by Hans-Kristian Arntzen and contributors
- [Intel XeSS](https://github.com/intel/xess), [OpenUSD](https://github.com/PixarAnimationStudios/OpenUSD), glslang and the other SDKs listed in `ThirdPartyLicenses.txt`

Research sources:

- [DarkStarSword/3d-fixes](https://github.com/DarkStarSword/3d-fixes) (Katana engine light layouts)
- [ReShade](https://github.com/crosire/reshade) (Vulkan layer present handling)
- [dxvk-nvapi](https://github.com/jp7677/dxvk-nvapi) (vkd3d-proton interop)
- [HALS8/dxvk-remix](https://github.com/HALS8/dxvk-remix) (terrain baker work)
- Unreal Engine and Unity public source and docs, Frostbite "Moving Frostbite to PBR", Simon Coenen's DOOM Eternal study, and the engine talks cited in `documentation/engine_knowledge`

Testers and contributors: **Demoflower**, **Sparkles (Kim)**, **Rafeal Santino Supertux**, **frisser**, **Behon**, **Clouds**, **SW491**.

## License

DXVK is zlib/libpng (`LICENSE`); NVIDIA's DXVK-Remix terms are in `LICENSE-MIT`; third-party SDKs keep their own licenses (`ThirdPartyLicenses.txt`). Not affiliated with or endorsed by NVIDIA.
