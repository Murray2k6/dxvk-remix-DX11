#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dxgi.h>
#include <cstdio>
#include <cstdint>
#include "dx11_bridge_client.h"

#define DX11_V219_REAL_DXGI_SWAPCHAIN_CAPTURE_LAYER 1

using PFN_CreateDXGIFactory = HRESULT (WINAPI *)(REFIID, void**);
using PFN_CreateDXGIFactory1 = HRESULT (WINAPI *)(REFIID, void**);
using PFN_CreateDXGIFactory2 = HRESULT (WINAPI *)(UINT, REFIID, void**);

static HMODULE gSystemDxgi = nullptr;
static PFN_CreateDXGIFactory pCreateDXGIFactory = nullptr;
static PFN_CreateDXGIFactory1 pCreateDXGIFactory1 = nullptr;
static PFN_CreateDXGIFactory2 pCreateDXGIFactory2 = nullptr;

using PFN_FactoryCreateSwapChain = HRESULT (STDMETHODCALLTYPE *)(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);
// DX11_V219_X86_DXGI_FACTORY2_ABI_SAFE
using PFN_Factory2CreateSwapChainForHwnd = HRESULT (STDMETHODCALLTYPE *)(void*, IUnknown*, HWND, const void*, const void*, IDXGIOutput*, void**);

static PFN_FactoryCreateSwapChain oFactoryCreateSwapChain = nullptr;
static PFN_Factory2CreateSwapChainForHwnd oFactory2CreateSwapChainForHwnd = nullptr;

static void DLog(const char* text) {
  dx11_bridge_client::LogLine("dxgi", text);
}

static HMODULE LoadSystemDxgiV219() {
  if (gSystemDxgi) return gSystemDxgi;
  gSystemDxgi = dx11_bridge_client::LoadSystemDll("dxgi.dll");
  if (!gSystemDxgi) return nullptr;
  pCreateDXGIFactory = reinterpret_cast<PFN_CreateDXGIFactory>(GetProcAddress(gSystemDxgi, "CreateDXGIFactory"));
  pCreateDXGIFactory1 = reinterpret_cast<PFN_CreateDXGIFactory1>(GetProcAddress(gSystemDxgi, "CreateDXGIFactory1"));
  pCreateDXGIFactory2 = reinterpret_cast<PFN_CreateDXGIFactory2>(GetProcAddress(gSystemDxgi, "CreateDXGIFactory2"));
  return gSystemDxgi;
}

template <typename T>
static bool HookVTable(void* object, size_t slot, void* detour, T* original, const char* name) {
  if (!object || !detour || !original) return false;
  void*** obj = reinterpret_cast<void***>(object);
  void** vt = *obj;
  if (!vt) return false;
  if (vt[slot] == detour) return true;

  DWORD oldProtect = 0;
  if (!VirtualProtect(&vt[slot], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect)) {
    return false;
  }

  if (*original == nullptr) {
    *original = reinterpret_cast<T>(vt[slot]);
  }

  vt[slot] = detour;

  DWORD ignored = 0;
  VirtualProtect(&vt[slot], sizeof(void*), oldProtect, &ignored);
  FlushInstructionCache(GetCurrentProcess(), &vt[slot], sizeof(void*));

  char msg[256] = {};
  sprintf_s(msg, sizeof(msg), "DX11_V219_REAL_DXGI_SWAPCHAIN_CAPTURE_LAYER: hooked %s slot=%u.", name, (unsigned) slot);
  DLog(msg);
  return true;
}

using PFN_D3D11SwapChainHook = void (WINAPI *)(IDXGISwapChain*);
static void TryInstallD3D11SwapChainHook(IDXGISwapChain* swapChain) {
  if (!swapChain) return;

  HMODULE d3d11 = GetModuleHandleA("d3d11.dll");
  if (!d3d11) {
    DLog("DX11_V219_REAL_DXGI_SWAPCHAIN_CAPTURE_LAYER: d3d11.dll module is not loaded yet; swapchain hook deferred.");
    return;
  }

  auto hook = reinterpret_cast<PFN_D3D11SwapChainHook>(GetProcAddress(d3d11, "DX11BridgeInstallSwapChainCapture"));
  if (!hook) {
    DLog("DX11_V219_REAL_DXGI_SWAPCHAIN_CAPTURE_LAYER: local d3d11.dll lacks DX11BridgeInstallSwapChainCapture export.");
    return;
  }

  hook(swapChain);
  DLog("DX11_V219_REAL_DXGI_SWAPCHAIN_CAPTURE_LAYER: installed d3d11 swapchain Present capture from DXGI factory path.");
}

static HRESULT STDMETHODCALLTYPE HFactoryCreateSwapChain(IDXGIFactory* self, IUnknown* device, DXGI_SWAP_CHAIN_DESC* desc, IDXGISwapChain** swapChain) {
  HRESULT hr = oFactoryCreateSwapChain ? oFactoryCreateSwapChain(self, device, desc, swapChain) : DXGI_ERROR_UNSUPPORTED;
  if (SUCCEEDED(hr) && swapChain && *swapChain) {
    dx11_bridge_client::EnsureServer();
    TryInstallD3D11SwapChainHook(*swapChain);
  }
  return hr;
}

static HRESULT STDMETHODCALLTYPE HFactory2CreateSwapChainForHwnd(void* self, IUnknown* device, HWND hwnd, const void* desc, const void* fsDesc, IDXGIOutput* restrictToOutput, void** swapChain) {
  HRESULT hr = oFactory2CreateSwapChainForHwnd ? oFactory2CreateSwapChainForHwnd(self, device, hwnd, desc, fsDesc, restrictToOutput, swapChain) : DXGI_ERROR_UNSUPPORTED;
  if (SUCCEEDED(hr) && swapChain && *swapChain) {
    dx11_bridge_client::EnsureServer();
    TryInstallD3D11SwapChainHook(reinterpret_cast<IDXGISwapChain*>(*swapChain));
  }
  return hr;
}

static void InstallFactoryHooks(void* factory) {
  if (!factory) return;
  // IDXGIFactory::CreateSwapChain slot 10.
  HookVTable(factory, 10, reinterpret_cast<void*>(&HFactoryCreateSwapChain), &oFactoryCreateSwapChain, "IDXGIFactory::CreateSwapChain");
  // IDXGIFactory2::CreateSwapChainForHwnd slot 15. Safe when object is Factory2; harmless if not used.
  HookVTable(factory, 15, reinterpret_cast<void*>(&HFactory2CreateSwapChainForHwnd), &oFactory2CreateSwapChainForHwnd, "IDXGIFactory2::CreateSwapChainForHwnd");
}

BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID reserved) {
  if (reason == DLL_PROCESS_ATTACH) {
    DisableThreadLibraryCalls(hinst);
    dx11_bridge_client::SetModule(hinst);
    DLog("DX11_V219_REAL_DXGI_SWAPCHAIN_CAPTURE_LAYER: game loaded root dxgi.dll factory capture layer.");
  }
  if (reason == DLL_PROCESS_DETACH && reserved != nullptr) {
    dx11_bridge_client::Detach();
  }
  return TRUE;
}

extern "C" HRESULT WINAPI CreateDXGIFactory(REFIID riid, void** ppFactory) {
  DLog("DX11_V219_REAL_DXGI_SWAPCHAIN_CAPTURE_LAYER: CreateDXGIFactory intercepted.");
  if (!LoadSystemDxgiV219() || !pCreateDXGIFactory) return DXGI_ERROR_UNSUPPORTED;
  HRESULT hr = pCreateDXGIFactory(riid, ppFactory);
  if (SUCCEEDED(hr) && ppFactory && *ppFactory) InstallFactoryHooks(*ppFactory);
  return hr;
}

extern "C" HRESULT WINAPI CreateDXGIFactory1(REFIID riid, void** ppFactory) {
  DLog("DX11_V219_REAL_DXGI_SWAPCHAIN_CAPTURE_LAYER: CreateDXGIFactory1 intercepted.");
  if (!LoadSystemDxgiV219() || !pCreateDXGIFactory1) return DXGI_ERROR_UNSUPPORTED;
  HRESULT hr = pCreateDXGIFactory1(riid, ppFactory);
  if (SUCCEEDED(hr) && ppFactory && *ppFactory) InstallFactoryHooks(*ppFactory);
  return hr;
}

extern "C" HRESULT WINAPI CreateDXGIFactory2(UINT flags, REFIID riid, void** ppFactory) {
  DLog("DX11_V219_REAL_DXGI_SWAPCHAIN_CAPTURE_LAYER: CreateDXGIFactory2 intercepted.");
  if (!LoadSystemDxgiV219() || !pCreateDXGIFactory2) return DXGI_ERROR_UNSUPPORTED;
  HRESULT hr = pCreateDXGIFactory2(flags, riid, ppFactory);
  if (SUCCEEDED(hr) && ppFactory && *ppFactory) InstallFactoryHooks(*ppFactory);
  return hr;
}