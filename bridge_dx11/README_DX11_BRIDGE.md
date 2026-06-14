# DX11 bridge replacement source

This folder is a DX11-only replacement path for the NVIDIA D3D9 bridge layout. It does not rename d3d9.dll and it does not build the old D3D9 client.

Build:

```powershell
powershell -ExecutionPolicy Bypass -File .\bridge_dx11\build_dx11_bridge.ps1 -AllConfigs
```

Outputs:

```text
_output\x86\debug\d3d11.dll
_output\x86\debug\dxgi.dll
_output\x86\debug\.trex\NvRemixDx11Bridge.exe
```

The x64 DX11 runtime must be staged separately into the `.trex` folders by the top-level build script.
