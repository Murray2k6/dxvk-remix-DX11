#include <array>
#include <atomic>
#include <filesystem>
#include <utility>

#include <windows.h>

#include "../dxgi/dxgi_adapter.h"

#include "../dxvk/dxvk_instance.h"

#include "d3d11_device.h"
#include "d3d11_enums.h"
#include "d3d11_interop.h"

#include "../util/util_env.h"
#include "../util/util_filesys.h"

namespace {
  HMODULE g_d3d11Module = nullptr;

  // DX11_V263_CRASH_FILTER_SAFE (replaces the V261 vectored handler)
  // V261 used AddVectoredExceptionHandler(1): it fired on EVERY hardware
  // exception in the process, including exceptions games and DRM wrappers
  // throw AND CATCH by design during startup (guarded memory probes,
  // unpacker stubs). Running file I/O, heap allocation, and module lookups
  // inside those contexts (loader lock, heap lock, DRM stub) can hang or
  // kill the game before it ever creates a device - "no ray tracing and no
  // logs". This filter instead installs via SetUnhandledExceptionFilter:
  // it runs ONLY for truly fatal, unhandled exceptions - zero interference
  // with a running game - logs the signature (module + offset, resolvable
  // against this build's PDB), then forwards to whatever filter the game
  // had installed so its own crash handling and WER behave exactly as
  // before.
  std::atomic<LPTOP_LEVEL_EXCEPTION_FILTER> g_prevCrashFilter = { nullptr };
  std::atomic<bool> g_crashFilterInstalled = { false };
  std::atomic<bool> g_inCrashFilter = { false };

  LONG WINAPI remixUnhandledCrashFilter(EXCEPTION_POINTERS* xp) {
    // Recursion guard: if logging itself faults, step aside immediately.
    bool expected = false;
    const bool firstEntry = g_inCrashFilter.compare_exchange_strong(expected, true);

    if (firstEntry && xp != nullptr && xp->ExceptionRecord != nullptr) {
      const DWORD code = xp->ExceptionRecord->ExceptionCode;
      void* addr = xp->ExceptionRecord->ExceptionAddress;

      char modPath[MAX_PATH] = {};
      uintptr_t offset = 0;
      HMODULE mod = nullptr;
      if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                           | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                             reinterpret_cast<LPCWSTR>(addr), &mod) && mod != nullptr) {
        GetModuleFileNameA(mod, modPath, MAX_PATH);
        offset = reinterpret_cast<uintptr_t>(addr) - reinterpret_cast<uintptr_t>(mod);
      }

      // Access violations carry the access type and target address.
      uintptr_t avType = 0;
      uintptr_t avTarget = 0;
      const bool isAv = (code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_IN_PAGE_ERROR)
                     && xp->ExceptionRecord->NumberParameters >= 2;
      if (isAv) {
        avType = static_cast<uintptr_t>(xp->ExceptionRecord->ExceptionInformation[0]);
        avTarget = static_cast<uintptr_t>(xp->ExceptionRecord->ExceptionInformation[1]);
      }

      std::string avInfo;
      if (isAv) {
        avInfo = dxvk::str::format(
          avType == 1 ? " write-to=0x" : (avType == 8 ? " exec-at=0x" : " read-from=0x"),
          std::hex, avTarget);
      }

      dxvk::Logger::err(dxvk::str::format(
        "[Remix-DX11][crash] UNHANDLED exception code=0x", std::hex, code,
        " addr=0x", reinterpret_cast<uintptr_t>(addr),
        " module=", modPath[0] ? modPath : "<unknown>",
        " offset=0x", offset, avInfo));

      g_inCrashFilter.store(false, std::memory_order_relaxed);
    }

    // Forward to the game's own filter so its crash handling still runs.
    LPTOP_LEVEL_EXCEPTION_FILTER prev = g_prevCrashFilter.load(std::memory_order_relaxed);
    if (prev != nullptr && prev != &remixUnhandledCrashFilter)
      return prev(xp);
    return EXCEPTION_CONTINUE_SEARCH;
  }

  HMODULE loadSiblingDxgi() {
    if (g_d3d11Module == nullptr)
      return nullptr;

    wchar_t path[MAX_PATH];
    if (!GetModuleFileNameW(g_d3d11Module, path, MAX_PATH))
      return nullptr;

    wchar_t* sep = wcsrchr(path, L'\\');
    if (sep == nullptr)
      return nullptr;

    *(sep + 1) = L'\0';
    wcscat_s(path, L"dxgi.dll");

    const DWORD attributes = GetFileAttributesW(path);
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
      return nullptr;

    return LoadLibraryExW(path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
  }
}

// DX11_V263_CRASH_FILTER_SAFE: (re)install the log-only unhandled-exception
// filter. Games install their own filter during startup (Unity CrashHandler,
// launcher SEH), which would replace ours and silently eat the crash
// signature; calling this periodically (EndFrame) keeps ours last-installed
// while chaining to theirs, so both run. External linkage - called from
// d3d11_rtx.cpp.
void RemixReassertCrashSignatureFilter() {
  LPTOP_LEVEL_EXCEPTION_FILTER prev = SetUnhandledExceptionFilter(&remixUnhandledCrashFilter);
  if (prev != nullptr && prev != &remixUnhandledCrashFilter)
    g_prevCrashFilter.store(prev, std::memory_order_relaxed);
  g_crashFilterInstalled.store(true, std::memory_order_relaxed);
}

// Ensure all Remix runtime DLLs (dxgi, NRD, DLSS, etc.) resolve from the
// game directory, not System32.  Launchers (Rockstar, Epic, Steam) often
// change CWD or use restricted search paths.
//
// SetDllDirectoryW  — classic mechanism, always effective.
// AddDllDirectory   — persistent & additive; survives another DLL calling
//                     SetDllDirectoryW(NULL).  Only participates in search
//                     when the process opted into LOAD_LIBRARY_SEARCH_* via
//                     SetDefaultDllDirectories (some games or runtimes do).
void logRemixDx11BuildBanner() {
  // First lines of any log: makes the running build unambiguous so a stale
  // d3d11.dll left in a game folder can never again be mistaken for a fresh
  // one. This function is in the global namespace (before `namespace dxvk`),
  // so dxvk symbols are fully qualified here.
  static std::atomic<bool> s_logged = { false };
  bool expected = false;
  if (!s_logged.compare_exchange_strong(expected, true))
    return;
  dxvk::Logger::info("=====================================================");
  dxvk::Logger::info(dxvk::str::format("[Remix-DX11] build stamp: ", __DATE__, " ", __TIME__));
  dxvk::Logger::info("[Remix-DX11] fixes: nrcVendorFallback texcoordSINT SR4viewport budgetHeadroom inputSeparation grayPlaceholder revertedDLSSGuard initStepLogging allVendorPrewarmSkip anyGameAlbedoFix significanceCullingOptIn gpuSceneUI remixapiExport taauDefaultAllVendors rtxOptionsNullGuard rtxOptionsRootCreate intelFseSwapchainFix steamOverlayVkDisable wsiSelfDeadlockPump vramLeakDiag restoreIconicWindow intelFifoPresent intelForceFSEallowed gdiInteropPresent frameLatencyHandleGuard noUvFlatAlbedo baseVertexGeometry gpuSceneBBox perVendorPresent gdiWsiFallback nrcOptIn nvidiaPrewarm rawInputHandoff noInfiniteWaits nonblockingRtPipelines tlasFirewall interleaverFormatNorm dynamicVbSnapshot color0Norm interleaverSkipGuard intUvDecode menuPassthrough migrationPersists fullresTargetGuard viewInProjCbuffer fallbackRadianceTame textureHashStability bridgeTextureContentHash skinningNameGate preciseCamera vpConfirmOncePerFrame crashFilterSafe nrdDenoiserPayload strongerDenoising bridgePresentCamera bootMarkerLogCleanup vertexColorFormats texcoordFormats pickResolveLog nrdRobustLoad menuToggleDebounce uiDisplayMatchesSurface noBlackFromVertexColor nrdDenoiserSafeDefault useRealAlbedo");
  dxvk::Logger::info("[Remix-DX11] if this line is absent or older than your last build, the game loaded a STALE d3d11.dll.");
  dxvk::Logger::info("=====================================================");
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH) {
    g_d3d11Module = hModule;
    // DX11_V263_CRASH_FILTER_SAFE: unhandled-only, chained - never fires
    // during normal operation, so it cannot interfere with game/DRM startup
    // the way the V261 vectored handler could.
    RemixReassertCrashSignatureFilter();
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
  } else if (reason == DLL_PROCESS_DETACH) {
    if (g_crashFilterInstalled.load(std::memory_order_relaxed)) {
      SetUnhandledExceptionFilter(g_prevCrashFilter.load(std::memory_order_relaxed));
      g_crashFilterInstalled.store(false, std::memory_order_relaxed);
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

  static HRESULT createPreferredDxgiFactory(Com<IDXGIFactory>& dxgiFactory) {
    dxgiFactory = nullptr;

    HMODULE siblingDxgi = loadSiblingDxgi();
    if (siblingDxgi != nullptr) {
      using PFN_CreateDXGIFactory1 = HRESULT(WINAPI*)(REFIID, void**);
      auto fn = reinterpret_cast<PFN_CreateDXGIFactory1>(GetProcAddress(siblingDxgi, "CreateDXGIFactory1"));

      if (fn != nullptr) {
        HRESULT hr = fn(__uuidof(IDXGIFactory), reinterpret_cast<void**>(&dxgiFactory));
        if (SUCCEEDED(hr)) {
          Logger::info("D3D11CreateDevice: Using sibling Remix dxgi.dll factory");
          return hr;
        }

        Logger::warn(str::format("D3D11CreateDevice: sibling Remix dxgi.dll factory failed: ", hr));
      }
    }

    return CreateDXGIFactory1(__uuidof(IDXGIFactory), reinterpret_cast<void**>(&dxgiFactory));
  }

  static HRESULT createSwapChainViaD3D11Device(
          IDXGIFactory*           pFactory,
          ID3D11Device*           pDevice,
    const DXGI_SWAP_CHAIN_DESC*   pDesc,
          IDXGISwapChain**        ppSwapChain) {
    InitReturnPtr(ppSwapChain);

    if (!pFactory || !pDevice || !pDesc || !ppSwapChain || !pDesc->OutputWindow)
      return DXGI_ERROR_INVALID_CALL;

    Com<IWineDXGISwapChainFactory> wineDevice;
    HRESULT hr = pDevice->QueryInterface(
      __uuidof(IWineDXGISwapChainFactory),
      reinterpret_cast<void**>(&wineDevice));

    if (FAILED(hr))
      return hr;

    DXGI_SWAP_CHAIN_DESC1 desc;
    desc.Width       = pDesc->BufferDesc.Width;
    desc.Height      = pDesc->BufferDesc.Height;
    desc.Format      = pDesc->BufferDesc.Format;
    desc.Stereo      = FALSE;
    desc.SampleDesc  = pDesc->SampleDesc;
    desc.BufferUsage = pDesc->BufferUsage;
    desc.BufferCount = pDesc->BufferCount;
    desc.Scaling     = DXGI_SCALING_STRETCH;
    desc.SwapEffect  = pDesc->SwapEffect;
    desc.AlphaMode   = DXGI_ALPHA_MODE_IGNORE;
    desc.Flags       = pDesc->Flags;

    DXGI_SWAP_CHAIN_FULLSCREEN_DESC descFs;
    descFs.RefreshRate      = pDesc->BufferDesc.RefreshRate;
    descFs.ScanlineOrdering = pDesc->BufferDesc.ScanlineOrdering;
    descFs.Scaling          = pDesc->BufferDesc.Scaling;
    descFs.Windowed         = pDesc->Windowed;

    IDXGISwapChain1* swapChain = nullptr;
    hr = wineDevice->CreateSwapChainForHwnd(
      pFactory,
      pDesc->OutputWindow,
      &desc,
      &descFs,
      nullptr,
      &swapChain);

    *ppSwapChain = swapChain;
    return hr;
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

    logRemixDx11BuildBanner();

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
      hr = createPreferredDxgiFactory(dxgiFactory);

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
        Logger::warn(str::format("D3D11CreateDevice: DXGI factory swap chain path failed: ", hr,
          "; retrying through D3D11 Remix swap chain factory"));
        hr = createSwapChainViaD3D11Device(dxgiFactory.ptr(), device.ptr(), &desc, ppSwapChain);

        if (FAILED(hr)) {
          Logger::err("D3D11CreateDevice: Failed to create swap chain");
          return hr;
        }
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
