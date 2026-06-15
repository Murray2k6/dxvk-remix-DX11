#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dxgi.h>
#include "dx11_bridge_client.h"
using PFN_CreateDXGIFactory = HRESULT (WINAPI *)(REFIID, void**);
using PFN_CreateDXGIFactory1 = HRESULT (WINAPI *)(REFIID, void**);
using PFN_CreateDXGIFactory2 = HRESULT (WINAPI *)(UINT, REFIID, void**);
static HMODULE gSystem = nullptr;
static PFN_CreateDXGIFactory pCreateDXGIFactory = nullptr;
static PFN_CreateDXGIFactory1 pCreateDXGIFactory1 = nullptr;
static PFN_CreateDXGIFactory2 pCreateDXGIFactory2 = nullptr;
static bool LoadSystem() {
  if (gSystem) return true;
  gSystem = dx11_bridge_client::LoadSystemDll("dxgi.dll");
  if (!gSystem) return false;
  pCreateDXGIFactory = (PFN_CreateDXGIFactory)GetProcAddress(gSystem, "CreateDXGIFactory");
  pCreateDXGIFactory1 = (PFN_CreateDXGIFactory1)GetProcAddress(gSystem, "CreateDXGIFactory1");
  pCreateDXGIFactory2 = (PFN_CreateDXGIFactory2)GetProcAddress(gSystem, "CreateDXGIFactory2");
  return pCreateDXGIFactory || pCreateDXGIFactory1 || pCreateDXGIFactory2;
}
BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH) { DisableThreadLibraryCalls(hinst); dx11_bridge_client::SetModule(hinst); dx11_bridge_client::Attach(); }
  if (reason == DLL_PROCESS_DETACH) { dx11_bridge_client::Detach(); }
  return TRUE;
}
extern "C" HRESULT WINAPI CreateDXGIFactory(REFIID riid, void** ppFactory) { dx11_bridge_client::EnsureServer(); if (!LoadSystem() || !pCreateDXGIFactory) return E_FAIL; return pCreateDXGIFactory(riid, ppFactory); }
extern "C" HRESULT WINAPI CreateDXGIFactory1(REFIID riid, void** ppFactory) { dx11_bridge_client::EnsureServer(); if (!LoadSystem() || !pCreateDXGIFactory1) return E_FAIL; return pCreateDXGIFactory1(riid, ppFactory); }
extern "C" HRESULT WINAPI CreateDXGIFactory2(UINT flags, REFIID riid, void** ppFactory) { dx11_bridge_client::EnsureServer(); if (!LoadSystem() || !pCreateDXGIFactory2) return E_FAIL; return pCreateDXGIFactory2(flags, riid, ppFactory); }