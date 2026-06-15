#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include "dx11_bridge_client.h"

using PFN_D3D11CreateDevice = HRESULT (WINAPI *)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL*, UINT, UINT, ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);
using PFN_D3D11CreateDeviceAndSwapChain = HRESULT (WINAPI *)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL*, UINT, UINT, const DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**, ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);
static HMODULE gSystem = nullptr;
static PFN_D3D11CreateDevice pD3D11CreateDevice = nullptr;
static PFN_D3D11CreateDeviceAndSwapChain pD3D11CreateDeviceAndSwapChain = nullptr;
static bool LoadSystem() {
  if (gSystem) return true;
  gSystem = dx11_bridge_client::LoadSystemDll("d3d11.dll");
  if (!gSystem) return false;
  pD3D11CreateDevice = (PFN_D3D11CreateDevice)GetProcAddress(gSystem, "D3D11CreateDevice");
  pD3D11CreateDeviceAndSwapChain = (PFN_D3D11CreateDeviceAndSwapChain)GetProcAddress(gSystem, "D3D11CreateDeviceAndSwapChain");
  return pD3D11CreateDevice && pD3D11CreateDeviceAndSwapChain;
}
BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH) { DisableThreadLibraryCalls(hinst); dx11_bridge_client::SetModule(hinst); dx11_bridge_client::Attach(); }
  if (reason == DLL_PROCESS_DETACH) { dx11_bridge_client::Detach(); }
  return TRUE;
}
extern "C" HRESULT WINAPI D3D11CreateDevice(IDXGIAdapter* a, D3D_DRIVER_TYPE t, HMODULE s, UINT f, const D3D_FEATURE_LEVEL* fl, UINT flc, UINT sdk, ID3D11Device** dev, D3D_FEATURE_LEVEL* got, ID3D11DeviceContext** ctx) {
  dx11_bridge_client::EnsureServer();
  if (!LoadSystem()) return E_FAIL;
  return pD3D11CreateDevice(a,t,s,f,fl,flc,sdk,dev,got,ctx);
}
extern "C" HRESULT WINAPI D3D11CreateDeviceAndSwapChain(IDXGIAdapter* a, D3D_DRIVER_TYPE t, HMODULE s, UINT f, const D3D_FEATURE_LEVEL* fl, UINT flc, UINT sdk, const DXGI_SWAP_CHAIN_DESC* sd, IDXGISwapChain** sc, ID3D11Device** dev, D3D_FEATURE_LEVEL* got, ID3D11DeviceContext** ctx) {
  dx11_bridge_client::EnsureServer();
  if (!LoadSystem()) return E_FAIL;
  return pD3D11CreateDeviceAndSwapChain(a,t,s,f,fl,flc,sdk,sd,sc,dev,got,ctx);
}