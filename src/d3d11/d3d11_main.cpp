#include <array>
#include <filesystem>

#include <windows.h>

#include "../dxgi/dxgi_adapter.h"

#include "../dxvk/dxvk_instance.h"

#include "d3d11_device.h"
#include "d3d11_enums.h"
#include "d3d11_interop.h"

#include "../util/util_env.h"
#include "../util/util_filesys.h"

// Ensure all Remix runtime DLLs (dxgi, NRD, DLSS, etc.) resolve from the
// game directory, not System32.  Launchers (Rockstar, Epic, Steam) often
// change CWD or use restricted search paths.
//
// SetDllDirectoryW  — classic mechanism, always effective.
// AddDllDirectory   — persistent & additive; survives another DLL calling
//                     SetDllDirectoryW(NULL).  Only participates in search
//                     when the process opted into LOAD_LIBRARY_SEARCH_* via
//                     SetDefaultDllDirectories (some games or runtimes do).
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
  Logger Logger::s_instance("d3d11.log");
}
  
extern "C" {
  using namespace dxvk;

  static HMODULE loadSystemD3D11() {
    wchar_t sysPath[MAX_PATH];
    GetSystemDirectoryW(sysPath, MAX_PATH);
    wcscat_s(sysPath, L"\\d3d11.dll");
    return LoadLibraryExW(sysPath, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
  }

  static HRESULT forwardToSystemD3D11CoreCreateDevice(
          IDXGIFactory*       pFactory,
          IDXGIAdapter*       pAdapter,
          UINT                Flags,
    const D3D_FEATURE_LEVEL*  pFeatureLevels,
          UINT                FeatureLevels,
          ID3D11Device**      ppDevice) {
    HMODULE hSys = loadSystemD3D11();
    if (!hSys)
      return E_FAIL;

    using PFN = HRESULT(WINAPI*)(IDXGIFactory*, IDXGIAdapter*, UINT, const D3D_FEATURE_LEVEL*, UINT, ID3D11Device**);
    auto fn = reinterpret_cast<PFN>(GetProcAddress(hSys, "D3D11CoreCreateDevice"));
    if (!fn)
      return E_FAIL;

    return fn(pFactory, pAdapter, Flags, pFeatureLevels, FeatureLevels, ppDevice);
  }

  static HRESULT forwardToSystemD3D11CreateDevice(
          IDXGIAdapter*         pAdapter,
          D3D_DRIVER_TYPE       DriverType,
          HMODULE               Software,
          UINT                  Flags,
    const D3D_FEATURE_LEVEL*    pFeatureLevels,
          UINT                  FeatureLevels,
          UINT                  SDKVersion,
          ID3D11Device**        ppDevice,
          D3D_FEATURE_LEVEL*    pFeatureLevel,
          ID3D11DeviceContext** ppImmediateContext) {
    HMODULE hSys = loadSystemD3D11();
    if (!hSys)
      return E_FAIL;

    using PFN = HRESULT(WINAPI*)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT,
      const D3D_FEATURE_LEVEL*, UINT, UINT, ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);
    auto fn = reinterpret_cast<PFN>(GetProcAddress(hSys, "D3D11CreateDevice"));
    if (!fn)
      return E_FAIL;

    return fn(pAdapter, DriverType, Software, Flags, pFeatureLevels, FeatureLevels,
      SDKVersion, ppDevice, pFeatureLevel, ppImmediateContext);
  }

  static HRESULT forwardToSystemD3D11CreateDeviceAndSwapChain(
          IDXGIAdapter*         pAdapter,
          D3D_DRIVER_TYPE       DriverType,
          HMODULE               Software,
          UINT                  Flags,
    const D3D_FEATURE_LEVEL*    pFeatureLevels,
          UINT                  FeatureLevels,
          UINT                  SDKVersion,
    const DXGI_SWAP_CHAIN_DESC* pSwapChainDesc,
          IDXGISwapChain**      ppSwapChain,
          ID3D11Device**        ppDevice,
          D3D_FEATURE_LEVEL*    pFeatureLevel,
          ID3D11DeviceContext** ppImmediateContext) {
    HMODULE hSys = loadSystemD3D11();
    if (!hSys)
      return E_FAIL;

    using PFN = HRESULT(WINAPI*)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT,
      const D3D_FEATURE_LEVEL*, UINT, UINT, const DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**,
      ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);
    auto fn = reinterpret_cast<PFN>(GetProcAddress(hSys, "D3D11CreateDeviceAndSwapChain"));
    if (!fn)
      return E_FAIL;

    return fn(pAdapter, DriverType, Software, Flags, pFeatureLevels, FeatureLevels,
      SDKVersion, pSwapChainDesc, ppSwapChain, ppDevice, pFeatureLevel, ppImmediateContext);
  }

  static HRESULT forwardToSystemD3D11On12CreateDevice(
          IUnknown*             pDevice,
          UINT                  Flags,
    const D3D_FEATURE_LEVEL*    pFeatureLevels,
          UINT                  FeatureLevels,
          IUnknown* const*      ppCommandQueues,
          UINT                  NumQueues,
          UINT                  NodeMask,
          ID3D11Device**        ppDevice,
          ID3D11DeviceContext** ppImmediateContext,
          D3D_FEATURE_LEVEL*    pChosenFeatureLevel) {
    HMODULE hSys = loadSystemD3D11();
    if (!hSys)
      return E_FAIL;

    using PFN = HRESULT(WINAPI*)(IUnknown*, UINT, const D3D_FEATURE_LEVEL*, UINT,
      IUnknown* const*, UINT, UINT, ID3D11Device**, ID3D11DeviceContext**, D3D_FEATURE_LEVEL*);
    auto fn = reinterpret_cast<PFN>(GetProcAddress(hSys, "D3D11On12CreateDevice"));
    if (!fn)
      return E_FAIL;

    return fn(pDevice, Flags, pFeatureLevels, FeatureLevels, ppCommandQueues,
      NumQueues, NodeMask, ppDevice, ppImmediateContext, pChosenFeatureLevel);
  }
  
  DLLEXPORT HRESULT __stdcall D3D11CoreCreateDevice(
          IDXGIFactory*       pFactory,
          IDXGIAdapter*       pAdapter,
          UINT                Flags,
    const D3D_FEATURE_LEVEL*  pFeatureLevels,
          UINT                FeatureLevels,
          ID3D11Device**      ppDevice) {
    if (env::shouldBypassRemixForCurrentProcess()) {
      Logger::info(str::format("D3D11 bypass for helper process: ", env::getExeName()));
      return forwardToSystemD3D11CoreCreateDevice(pFactory, pAdapter, Flags, pFeatureLevels, FeatureLevels, ppDevice);
    }

    InitReturnPtr(ppDevice);

    // Initialise the RTX filesystem (log/mod/capture paths) relative to the
    // game exe, then re-open the log file under that path.  Must be ONCE since
    // D3D11CoreCreateDevice can be called multiple times.
    ONCE(
      const auto exePath = env::getExePath();
      const auto exeDir  = std::filesystem::path(exePath).parent_path();
      util::RtxFileSys::init(exeDir.string());
      Logger::initRtxLog();
      util::RtxFileSys::print();
    );

    Rc<DxvkAdapter>  dxvkAdapter;
    Rc<DxvkInstance> dxvkInstance;

    Com<IDXGIDXVKAdapter> dxgiVkAdapter;
    
    // Try to find the corresponding Vulkan device for the DXGI adapter
    if (SUCCEEDED(pAdapter->QueryInterface(__uuidof(IDXGIDXVKAdapter), reinterpret_cast<void**>(&dxgiVkAdapter)))) {
      dxvkAdapter  = dxgiVkAdapter->GetDXVKAdapter();
      dxvkInstance = dxgiVkAdapter->GetDXVKInstance();
    } else {
      Logger::warn("D3D11CoreCreateDevice: Adapter is not a DXVK adapter");
      DXGI_ADAPTER_DESC desc;
      pAdapter->GetDesc(&desc);

      dxvkInstance = new DxvkInstance();
      dxvkAdapter  = dxvkInstance->findAdapterByLuid(&desc.AdapterLuid);

      if (dxvkAdapter == nullptr)
        dxvkAdapter = dxvkInstance->findAdapterByDeviceId(desc.VendorId, desc.DeviceId);
      
      if (dxvkAdapter == nullptr)
        dxvkAdapter = dxvkInstance->enumAdapters(0);

      if (dxvkAdapter == nullptr)
        return E_FAIL;
    }
    
    // Feature levels to probe if the
    // application does not specify any.
    std::array<D3D_FEATURE_LEVEL, 6> defaultFeatureLevels = {
      D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1,
      D3D_FEATURE_LEVEL_10_0, D3D_FEATURE_LEVEL_9_3,
      D3D_FEATURE_LEVEL_9_2,  D3D_FEATURE_LEVEL_9_1,
    };
    
    if (pFeatureLevels == nullptr || FeatureLevels == 0) {
      pFeatureLevels = defaultFeatureLevels.data();
      FeatureLevels  = defaultFeatureLevels.size();
    }
    
    // Find the highest feature level supported by the device.
    // This works because the feature level array is ordered.
    UINT flId;

    for (flId = 0 ; flId < FeatureLevels; flId++) {
      Logger::info(str::format("D3D11CoreCreateDevice: Probing ", pFeatureLevels[flId]));
      
      if (D3D11Device::CheckFeatureLevelSupport(dxvkInstance, dxvkAdapter, pFeatureLevels[flId]))
        break;
    }
    
    if (flId == FeatureLevels) {
      Logger::err("D3D11CoreCreateDevice: Requested feature level not supported");
      return E_INVALIDARG;
    }
    
    // Try to create the device with the given parameters.
    const D3D_FEATURE_LEVEL fl = pFeatureLevels[flId];
    
    try {
      Logger::info(str::format("D3D11CoreCreateDevice: Using feature level ", fl));
      Com<D3D11DXGIDevice> device = new D3D11DXGIDevice(
        pAdapter, dxvkInstance, dxvkAdapter, fl, Flags);
      
      return device->QueryInterface(
        __uuidof(ID3D11Device),
        reinterpret_cast<void**>(ppDevice));
    } catch (const DxvkError& e) {
      Logger::err("D3D11CoreCreateDevice: Failed to create D3D11 device");
      return E_FAIL;
    }
  }
  
  
  static HRESULT D3D11InternalCreateDeviceAndSwapChain(
          IDXGIAdapter*         pAdapter,
          D3D_DRIVER_TYPE       DriverType,
          HMODULE               Software,
          UINT                  Flags,
    const D3D_FEATURE_LEVEL*    pFeatureLevels,
          UINT                  FeatureLevels,
          UINT                  SDKVersion,
    const DXGI_SWAP_CHAIN_DESC* pSwapChainDesc,
          IDXGISwapChain**      ppSwapChain,
          ID3D11Device**        ppDevice,
          D3D_FEATURE_LEVEL*    pFeatureLevel,
          ID3D11DeviceContext** ppImmediateContext) {
    InitReturnPtr(ppDevice);
    InitReturnPtr(ppSwapChain);
    InitReturnPtr(ppImmediateContext);

    if (pFeatureLevel)
      *pFeatureLevel = D3D_FEATURE_LEVEL(0);

    HRESULT hr;

    Com<IDXGIFactory> dxgiFactory = nullptr;
    Com<IDXGIAdapter> dxgiAdapter = pAdapter;
    Com<ID3D11Device> device      = nullptr;
    
    if (ppSwapChain && !pSwapChainDesc)
      return E_INVALIDARG;
    
    if (!pAdapter) {
      // We'll treat everything as hardware, even if the
      // Vulkan device is actually a software device.
      if (DriverType != D3D_DRIVER_TYPE_HARDWARE)
        Logger::warn("D3D11CreateDevice: Unsupported driver type");
      
      // We'll use the first adapter returned by a DXGI factory
      hr = CreateDXGIFactory1(__uuidof(IDXGIFactory), reinterpret_cast<void**>(&dxgiFactory));

      if (FAILED(hr)) {
        Logger::err("D3D11CreateDevice: Failed to create a DXGI factory");
        return hr;
      }

      hr = dxgiFactory->EnumAdapters(0, &dxgiAdapter);

      if (FAILED(hr)) {
        Logger::err("D3D11CreateDevice: No default adapter available");
        return hr;
      }
    } else {
      // We should be able to query the DXGI factory from the adapter
      if (FAILED(dxgiAdapter->GetParent(__uuidof(IDXGIFactory), reinterpret_cast<void**>(&dxgiFactory)))) {
        Logger::err("D3D11CreateDevice: Failed to query DXGI factory from DXGI adapter");
        return E_INVALIDARG;
      }
      
      // In theory we could ignore these, but the Microsoft docs explicitly
      // state that we need to return E_INVALIDARG in case the arguments are
      // invalid. Both the driver type and software parameter can only be
      // set if the adapter itself is unspecified.
      // See: https://msdn.microsoft.com/en-us/library/windows/desktop/ff476082(v=vs.85).aspx
      if (DriverType != D3D_DRIVER_TYPE_UNKNOWN || Software)
        return E_INVALIDARG;
    }
    
    // Create the actual device
    hr = D3D11CoreCreateDevice(
      dxgiFactory.ptr(), dxgiAdapter.ptr(),
      Flags, pFeatureLevels, FeatureLevels,
      &device);
    
    if (FAILED(hr))
      return hr;
    
    // Create the swap chain, if requested
    if (ppSwapChain) {
      DXGI_SWAP_CHAIN_DESC desc = *pSwapChainDesc;
      hr = dxgiFactory->CreateSwapChain(device.ptr(), &desc, ppSwapChain);

      if (FAILED(hr)) {
        Logger::err("D3D11CreateDevice: Failed to create swap chain");
        return hr;
      }
    }
    
    // Write back whatever info the application requested
    if (pFeatureLevel)
      *pFeatureLevel = device->GetFeatureLevel();
    
    if (ppDevice)
      *ppDevice = device.ref();
    
    if (ppImmediateContext)
      device->GetImmediateContext(ppImmediateContext);

    // If we were unable to write back the device and the
    // swap chain, the application has no way of working
    // with the device so we should report S_FALSE here.
    if (!ppDevice && !ppImmediateContext && !ppSwapChain)
      return S_FALSE;
    
    return S_OK;
  }
  

  DLLEXPORT HRESULT __stdcall D3D11CreateDevice(
          IDXGIAdapter*         pAdapter,
          D3D_DRIVER_TYPE       DriverType,
          HMODULE               Software,
          UINT                  Flags,
    const D3D_FEATURE_LEVEL*    pFeatureLevels,
          UINT                  FeatureLevels,
          UINT                  SDKVersion,
          ID3D11Device**        ppDevice,
          D3D_FEATURE_LEVEL*    pFeatureLevel,
          ID3D11DeviceContext** ppImmediateContext) {
    if (env::shouldBypassRemixForCurrentProcess()) {
      Logger::info(str::format("D3D11 bypass for helper process: ", env::getExeName()));
      return forwardToSystemD3D11CreateDevice(pAdapter, DriverType, Software, Flags,
        pFeatureLevels, FeatureLevels, SDKVersion, ppDevice, pFeatureLevel, ppImmediateContext);
    }

    return D3D11InternalCreateDeviceAndSwapChain(
      pAdapter, DriverType, Software, Flags,
      pFeatureLevels, FeatureLevels, SDKVersion,
      nullptr, nullptr,
      ppDevice, pFeatureLevel, ppImmediateContext);
  }
  
  
  DLLEXPORT HRESULT __stdcall D3D11CreateDeviceAndSwapChain(
          IDXGIAdapter*         pAdapter,
          D3D_DRIVER_TYPE       DriverType,
          HMODULE               Software,
          UINT                  Flags,
    const D3D_FEATURE_LEVEL*    pFeatureLevels,
          UINT                  FeatureLevels,
          UINT                  SDKVersion,
    const DXGI_SWAP_CHAIN_DESC* pSwapChainDesc,
          IDXGISwapChain**      ppSwapChain,
          ID3D11Device**        ppDevice,
          D3D_FEATURE_LEVEL*    pFeatureLevel,
          ID3D11DeviceContext** ppImmediateContext) {
    if (env::shouldBypassRemixForCurrentProcess()) {
      Logger::info(str::format("D3D11 swapchain bypass for helper process: ", env::getExeName()));
      return forwardToSystemD3D11CreateDeviceAndSwapChain(pAdapter, DriverType, Software, Flags,
        pFeatureLevels, FeatureLevels, SDKVersion, pSwapChainDesc, ppSwapChain,
        ppDevice, pFeatureLevel, ppImmediateContext);
    }

    return D3D11InternalCreateDeviceAndSwapChain(
      pAdapter, DriverType, Software, Flags,
      pFeatureLevels, FeatureLevels, SDKVersion,
      pSwapChainDesc, ppSwapChain,
      ppDevice, pFeatureLevel, ppImmediateContext);
  }
  

  DLLEXPORT HRESULT __stdcall D3D11On12CreateDevice(
          IUnknown*             pDevice,
          UINT                  Flags,
    const D3D_FEATURE_LEVEL*    pFeatureLevels,
          UINT                  FeatureLevels,
          IUnknown* const*      ppCommandQueues,
          UINT                  NumQueues,
          UINT                  NodeMask,
          ID3D11Device**        ppDevice,
          ID3D11DeviceContext** ppImmediateContext,
          D3D_FEATURE_LEVEL*    pChosenFeatureLevel) {
    if (env::shouldBypassRemixForCurrentProcess()) {
      Logger::info(str::format("D3D11On12 bypass for helper process: ", env::getExeName()));
      return forwardToSystemD3D11On12CreateDevice(pDevice, Flags, pFeatureLevels, FeatureLevels,
        ppCommandQueues, NumQueues, NodeMask, ppDevice, ppImmediateContext, pChosenFeatureLevel);
    }

    static bool s_errorShown = false;

    if (!std::exchange(s_errorShown, true))
      Logger::err("D3D11On12CreateDevice: Not implemented");

    return E_NOTIMPL;
  }

}