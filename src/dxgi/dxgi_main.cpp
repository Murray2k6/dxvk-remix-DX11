#include "dxgi_factory.h"
#include "dxgi_include.h"

#include "../util/util_env.h"

// Same DLL search path fix as d3d11_main.cpp — ensures Remix runtime DLLs
// are found in the game directory when loaded through launchers with
// restricted search paths.
BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
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
        }
    }
    return TRUE;
}

namespace dxvk {
  
  Logger Logger::s_instance("dxgi.log");

  // Intel Vulkan ICD (igvk64.dll/ControlLib.dll) calls CreateDXGIFactory1 from
  // inside vkCreateInstance to enumerate adapters. That re-enters our dxgi.dll,
  // creates another DxvkInstance, calls vkCreateInstance again → stack overflow.
  // Guard: on reentrant calls forward to the real system dxgi.dll instead.
  static HRESULT forwardToSystemDxgi(UINT Flags, const char* exportName, REFIID riid, void** ppFactory) {
    wchar_t sysPath[MAX_PATH];
    GetSystemDirectoryW(sysPath, MAX_PATH);
    wcscat_s(sysPath, L"\\dxgi.dll");

    HMODULE hSys = LoadLibraryExW(sysPath, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!hSys) return E_FAIL;

    if (!std::strcmp(exportName, "CreateDXGIFactory2")) {
      using PFN_CreateDXGIFactory2 = HRESULT(WINAPI*)(UINT, REFIID, void**);
      auto fn = reinterpret_cast<PFN_CreateDXGIFactory2>(GetProcAddress(hSys, exportName));

      if (!fn) {
        FreeLibrary(hSys);
        return E_FAIL;
      }

      return fn(Flags, riid, ppFactory);
    }

    using PFN_CreateDXGIFactory = HRESULT(WINAPI*)(REFIID, void**);
    auto fn = reinterpret_cast<PFN_CreateDXGIFactory>(GetProcAddress(hSys, exportName));

    if (!fn) {
      FreeLibrary(hSys);
      return E_FAIL;
    }

    return fn(riid, ppFactory);
  }

  HRESULT createDxgiFactory(UINT Flags, const char* exportName, REFIID riid, void **ppFactory) {
    if (env::shouldBypassRemixForCurrentProcess()) {
      Logger::info(str::format("DXGI bypass for helper process: ", env::getExeName()));
      return forwardToSystemDxgi(Flags, exportName, riid, ppFactory);
    }

    static thread_local bool s_creating = false;
    if (s_creating)
      return forwardToSystemDxgi(Flags, exportName, riid, ppFactory);

    s_creating = true;
    HRESULT hr;
    try {
      Com<DxgiFactory> factory = new DxgiFactory(Flags);
      hr = factory->QueryInterface(riid, ppFactory);
    } catch (const DxvkError& e) {
      Logger::err(e.message());
      hr = E_FAIL;
    }
    s_creating = false;
    return hr;
  }
}

extern "C" {
  DLLEXPORT HRESULT __stdcall CreateDXGIFactory2(UINT Flags, REFIID riid, void **ppFactory) {
    dxvk::Logger::info("CreateDXGIFactory2: Ignoring flags");
    return dxvk::createDxgiFactory(Flags, "CreateDXGIFactory2", riid, ppFactory);
  }

  DLLEXPORT HRESULT __stdcall CreateDXGIFactory1(REFIID riid, void **ppFactory) {
    return dxvk::createDxgiFactory(0, "CreateDXGIFactory1", riid, ppFactory);
  }
  
  DLLEXPORT HRESULT __stdcall CreateDXGIFactory(REFIID riid, void **ppFactory) {
    return dxvk::createDxgiFactory(0, "CreateDXGIFactory", riid, ppFactory);
  }

  DLLEXPORT HRESULT __stdcall DXGIDeclareAdapterRemovalSupport() {
    static bool enabled = false;

    if (std::exchange(enabled, true))
      return 0x887a0036; // DXGI_ERROR_ALREADY_EXISTS;

    dxvk::Logger::warn("DXGIDeclareAdapterRemovalSupport: Stub");
    return S_OK;
  }

  DLLEXPORT HRESULT __stdcall DXGIGetDebugInterface1(UINT Flags, REFIID riid, void **ppDebug) {
    static bool errorShown = false;

    if (!std::exchange(errorShown, true))
      dxvk::Logger::warn("DXGIGetDebugInterface1: Stub");

    return E_NOINTERFACE;
  }

}