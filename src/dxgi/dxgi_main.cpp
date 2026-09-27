#include "dxgi_factory.h"
#include "dxgi_include.h"
#include "dxgi_native.h"

#include <delayimp.h>
#include <d3dcommon.h>

#include "../util/util_env.h"

// DX11_V282_SATELLITE_DELAYLOAD: same policy as d3d11_main.cpp - a missing
// delay-loaded satellite is logged at first use instead of killing the
// process at load time with no logs.
static FARPROC WINAPI remixDxgiDelayLoadFailureHook(unsigned dliNotify, PDelayLoadInfo pdli) {
  if ((dliNotify == dliFailLoadLib || dliNotify == dliFailGetProc)
   && pdli != nullptr && pdli->szDll != nullptr) {
    char msg[320];
    size_t pos = 0;
    const char* head = "MISSING SATELLITE DLL (copy the FULL x64 payload next to the game exe): ";
    for (const char* s = head; *s != '\0' && pos < sizeof(msg) - 1; ++s)
      msg[pos++] = *s;
    for (const char* s = pdli->szDll; *s != '\0' && pos < sizeof(msg) - 1; ++s)
      msg[pos++] = *s;
    msg[pos] = '\0';
    dxvk::env::remixAppendBootLine("dxgi.dll", msg);
    dxvk::Logger::err(std::string("[Remix-DX11] ") + msg);
  }
  return nullptr;
}
extern "C" const PfnDliHook __pfnDliFailureHook2 = remixDxgiDelayLoadFailureHook;

// DX11_V290_RUNTIME_DIR: same delay-load redirect as d3d11_main.cpp - prefer
// the dedicated Remix runtime directory when it exists.
static FARPROC WINAPI remixDxgiDelayLoadNotifyHook(unsigned dliNotify, PDelayLoadInfo pdli) {
  if (dliNotify == dliNotePreLoadLibrary
   && pdli != nullptr && pdli->szDll != nullptr) {
    wchar_t fullPath[MAX_PATH];
    const DWORD dirLen = dxvk::env::remixResolveRuntimeDirectoryW(fullPath, MAX_PATH);
    if (dirLen != 0) {
      size_t pos = dirLen;
      if (pos < MAX_PATH - 1)
        fullPath[pos++] = L'\\';
      const char* name = pdli->szDll;
      for (; *name != '\0' && pos < MAX_PATH - 1; ++name)
        fullPath[pos++] = wchar_t(static_cast<unsigned char>(*name));
      fullPath[pos] = L'\0';
      if (*name == '\0'
       && ::GetFileAttributesW(fullPath) != INVALID_FILE_ATTRIBUTES) {
        const HMODULE loaded =
          ::LoadLibraryExW(fullPath, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (loaded != nullptr)
          return reinterpret_cast<FARPROC>(loaded);
      }
    }
  }
  return nullptr;
}
extern "C" const PfnDliHook __pfnDliNotifyHook2 = remixDxgiDelayLoadNotifyHook;

// Same DLL search path fix as d3d11_main.cpp — ensures Remix runtime DLLs
// are found in the game directory when loaded through launchers with
// restricted search paths.
BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
    // DX11_V282_BOOT_BREADCRUMB (kernel32-only, loader-lock safe; see
    // util_env.h - proves the DLL attached even when no other log exists).
    dxvk::env::remixAppendBootLine("dxgi.dll",
      dxvk::env::shouldBypassRemixForCurrentProcess() ? "attached (bypass)" : "attached");
    if (!dxvk::env::shouldBypassRemixForCurrentProcess()) {
      wchar_t path[MAX_PATH];
      if (GetModuleFileNameW(hModule, path, MAX_PATH)) {
        wchar_t* sep = wcsrchr(path, L'\\');
        if (sep) {
          *sep = L'\0';
          SetDllDirectoryW(path);
          AddDllDirectory(path);
        }
      }
      // DX11_V290_RUNTIME_DIR: dxgi.dll is loaded BEFORE d3d11.dll in the
      // standard adapter-enumeration flow, so registering the runtime
      // directory here is what allows d3d11.dll's hard USD/python/boost
      // load-time imports to resolve from rtx-remix\runtime (or
      // DXVK_REMIX_RUNTIME_DIR) instead of the game folder.
      wchar_t runtimeDir[MAX_PATH];
      if (dxvk::env::remixResolveRuntimeDirectoryW(runtimeDir, MAX_PATH) != 0) {
        SetDllDirectoryW(runtimeDir);
        AddDllDirectory(runtimeDir);
        dxvk::env::remixAppendBootLine("dxgi.dll", "runtime directory registered (rtx-remix\\runtime or DXVK_REMIX_RUNTIME_DIR)");
      }
    }
    }
    return TRUE;
}

namespace dxvk {
  
  Logger Logger::s_instance("dxgi.log");

  HRESULT createDxgiFactory(UINT Flags, const char* exportName, REFIID riid, void **ppFactory) {
    if (!ppFactory)
      return E_POINTER;
    *ppFactory = nullptr;
    if (env::shouldBypassRemixForCurrentProcess()) {
      Logger::info(str::format("DXGI bypass for helper process: ", env::getExeName()));
      return createSystemDxgiFactory(Flags, exportName, riid, ppFactory);
    }

    // Some Vulkan ICDs enumerate Windows adapters during vkCreateInstance.
    // Reentrant factory creation must use Windows rather than recurse into Vulkan.
    static thread_local bool s_creating = false;
    if (s_creating)
      return createSystemDxgiFactory(Flags, exportName, riid, ppFactory);

    // DX11_V238_INTEROP_DXGI_PASSTHROUGH: the Intel D3D11/D3D12-interop present path needs a REAL DXGI
    // factory (the present runs on a real D3D12 device that bypasses the broken Intel Vulkan WSI).
    // D3D12CreateDevice internally calls CreateDXGIFactory2 by name, which resolves to THIS (our Remix)
    // dxgi.dll and would loop back into DXVK/Vulkan. While the interop sets this env flag (only around
    // its own D3D12 device/swapchain creation), forward to the real system dxgi so the present device
    // gets real DXGI. The game's own DXGI calls (flag clear) still go through DXVK/Remix as normal.
    if (env::getEnvVar("DXVK_REMIX_DXGI_PASSTHROUGH") == "1")
      return createSystemDxgiFactory(Flags, exportName, riid, ppFactory);

    s_creating = true;
    struct CreationGuard {
      bool& creating;
      ~CreationGuard() { creating = false; }
    } guard { s_creating };
    HRESULT hr;
    try {
      Com<DxgiFactory> factory = new DxgiFactory(Flags);
      hr = factory->QueryInterface(riid, ppFactory);
    } catch (const DxvkError& e) {
      Logger::err(e.message());
      hr = E_FAIL;
    }
    return hr;
  }
}

extern "C" {
  DLLEXPORT HRESULT __stdcall CreateDXGIFactory2(UINT Flags, REFIID riid, void **ppFactory) {
    return dxvk::createDxgiFactory(Flags, "CreateDXGIFactory2", riid, ppFactory);
  }

  DLLEXPORT HRESULT __stdcall CreateDXGIFactory1(REFIID riid, void **ppFactory) {
    return dxvk::createDxgiFactory(0, "CreateDXGIFactory1", riid, ppFactory);
  }
  
  DLLEXPORT HRESULT __stdcall CreateDXGIFactory(REFIID riid, void **ppFactory) {
    return dxvk::createDxgiFactory(0, "CreateDXGIFactory", riid, ppFactory);
  }

  DLLEXPORT HRESULT __stdcall DXGIDeclareAdapterRemovalSupport() {
    using Function = HRESULT (WINAPI*)();
    const auto function = reinterpret_cast<Function>(dxvk::getSystemDxgiProcAddress("DXGIDeclareAdapterRemovalSupport"));
    return function ? function() : HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
  }

  DLLEXPORT HRESULT __stdcall DXGIGetDebugInterface1(UINT Flags, REFIID riid, void **ppDebug) {
    if (!ppDebug)
      return E_POINTER;
    *ppDebug = nullptr;
    using Function = HRESULT (WINAPI*)(UINT, REFIID, void**);
    const auto function = reinterpret_cast<Function>(dxvk::getSystemDxgiProcAddress("DXGIGetDebugInterface1"));
    return function ? function(Flags, riid, ppDebug) : DXGI_ERROR_SDK_COMPONENT_MISSING;
  }

  // Windows D3D10/11 imports these private entry points by name. Forward the
  // native ABI, including all seven CreateDevice arguments, rather than merely
  // satisfying the loader and failing device creation later.
  DLLEXPORT HRESULT __stdcall DXGID3D10CreateDevice(
    HMODULE hModule, IDXGIFactory* pFactory, IDXGIAdapter* pAdapter,
    UINT Flags, const D3D_FEATURE_LEVEL* pFeatureLevels, UINT FeatureLevelCount, void** ppDevice) {
    using Function = HRESULT (WINAPI*)(HMODULE, IDXGIFactory*, IDXGIAdapter*, UINT,
      const D3D_FEATURE_LEVEL*, UINT, void**);
    const auto function = reinterpret_cast<Function>(dxvk::getSystemDxgiProcAddress("DXGID3D10CreateDevice"));
    if (!function)
      return HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
    dxvk::Com<IDXGIVkMonitorInfo> proxy;
    if (pFactory && SUCCEEDED(pFactory->QueryInterface(__uuidof(IDXGIVkMonitorInfo), reinterpret_cast<void**>(&proxy)))) {
      dxvk::Com<IDXGIFactory3> proxyFlags;
      pFactory->QueryInterface(__uuidof(IDXGIFactory3), reinterpret_cast<void**>(&proxyFlags));
      dxvk::Com<IDXGIFactory1> nativeFactory;
      HRESULT hr = dxvk::createSystemDxgiFactory(proxyFlags != nullptr ? proxyFlags->GetCreationFlags() : 0,
        "CreateDXGIFactory2", __uuidof(IDXGIFactory1), reinterpret_cast<void**>(&nativeFactory));
      if (FAILED(hr))
        return hr;
      dxvk::Com<IDXGIAdapter> nativeAdapter;
      hr = dxvk::getSystemDxgiAdapter(nativeFactory.ptr(), pAdapter, nativeAdapter);
      if (FAILED(hr))
        return hr;
      return function(hModule, nativeFactory.ptr(), nativeAdapter.ptr(), Flags, pFeatureLevels, FeatureLevelCount, ppDevice);
    }
    return function(hModule, pFactory, pAdapter, Flags, pFeatureLevels, FeatureLevelCount, ppDevice);
  }

  DLLEXPORT HRESULT __stdcall DXGID3D10CreateLayeredDevice(
    void* unknown0, void* unknown1, void* unknown2, void* unknown3, void* unknown4) {
    using Function = HRESULT (WINAPI*)(void*, void*, void*, void*, void*);
    const auto function = reinterpret_cast<Function>(dxvk::getSystemDxgiProcAddress("DXGID3D10CreateLayeredDevice"));
    return function ? function(unknown0, unknown1, unknown2, unknown3, unknown4) : HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
  }

  DLLEXPORT SIZE_T __stdcall DXGID3D10GetLayeredDeviceSize(
    const void* pLayers, UINT NumLayers) {
    using Function = SIZE_T (WINAPI*)(const void*, UINT);
    const auto function = reinterpret_cast<Function>(dxvk::getSystemDxgiProcAddress("DXGID3D10GetLayeredDeviceSize"));
    return function ? function(pLayers, NumLayers) : 0;
  }

  DLLEXPORT HRESULT __stdcall DXGID3D10RegisterLayers(
    const void* pLayers, UINT NumLayers) {
    using Function = HRESULT (WINAPI*)(const void*, UINT);
    const auto function = reinterpret_cast<Function>(dxvk::getSystemDxgiProcAddress("DXGID3D10RegisterLayers"));
    return function ? function(pLayers, NumLayers) : HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
  }

}
