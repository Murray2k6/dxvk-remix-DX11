#include "dxgi_factory.h"
#include "dxgi_swapchain.h"
#include "dxgi_swapchain_dispatcher.h"
#include "dxgi_native.h"

namespace dxvk {

  DxgiFactory::DxgiFactory(UINT Flags)
  // DX11_V283_SHARED_VK_INSTANCE: all factories share the process-wide
  // instance; concurrent creations serialize instead of racing the loader.
  : m_instance    (DxvkInstance::getOrCreateSharedInstance()),
    m_monitorInfo (this),
    m_options     (m_instance->config()),
    m_flags       (Flags) {
    for (uint32_t i = 0; m_instance->enumAdapters(i) != nullptr; i++)
      m_instance->enumAdapters(i)->logAdapterInfo();
  }
  
  
  DxgiFactory::~DxgiFactory() {
    
  }

  HRESULT DxgiFactory::getNativeFactory(Com<IDXGIFactory2>& factory) {
    std::lock_guard<std::mutex> lock(m_nativeMutex);
    if (m_nativeFactory == nullptr) {
      Com<IDXGIFactory2> created;
      HRESULT hr = createSystemDxgiFactory(m_flags, "CreateDXGIFactory2",
        __uuidof(IDXGIFactory2), reinterpret_cast<void**>(&created));
      if (FAILED(hr))
        return hr;
      m_nativeFactory = std::move(created);
    }
    factory = m_nativeFactory;
    return S_OK;
  }
  
  
  HRESULT STDMETHODCALLTYPE DxgiFactory::QueryInterface(REFIID riid, void** ppvObject) {
    if (ppvObject == nullptr)
      return E_POINTER;

    *ppvObject = nullptr;
    
    if (riid == __uuidof(IUnknown)
     || riid == __uuidof(IDXGIObject)
     || riid == __uuidof(IDXGIFactory)
     || riid == __uuidof(IDXGIFactory1)
     || riid == __uuidof(IDXGIFactory2)
     || riid == __uuidof(IDXGIFactory3)
     || riid == __uuidof(IDXGIFactory4)
     || riid == __uuidof(IDXGIFactory5)
     || riid == __uuidof(IDXGIFactory6)
     || riid == __uuidof(IDXGIFactory7)) {
      *ppvObject = ref(this);
      return S_OK;
    }

    if (riid == __uuidof(IDXGIVkMonitorInfo)) {
      *ppvObject = ref(&m_monitorInfo);
      return S_OK;
    }
    
    Logger::warn("DxgiFactory::QueryInterface: Unknown interface query");
    Logger::warn(str::format(riid));
    return E_NOINTERFACE;
  }
  
  
  HRESULT STDMETHODCALLTYPE DxgiFactory::GetParent(REFIID riid, void** ppParent) {
    InitReturnPtr(ppParent);
    
    Logger::warn("DxgiFactory::GetParent: Unknown interface query");
    return E_NOINTERFACE;
  }
  
  
  BOOL STDMETHODCALLTYPE DxgiFactory::IsWindowedStereoEnabled() {
    Com<IDXGIFactory2> factory;
    return SUCCEEDED(getNativeFactory(factory)) && factory->IsWindowedStereoEnabled();
  }
  
  
  HRESULT STDMETHODCALLTYPE DxgiFactory::CreateSoftwareAdapter(
          HMODULE         Module,
          IDXGIAdapter**  ppAdapter) {
    InitReturnPtr(ppAdapter);
    
    if (ppAdapter == nullptr)
      return DXGI_ERROR_INVALID_CALL;
    
    Com<IDXGIFactory2> factory;
    const HRESULT hr = getNativeFactory(factory);
    return FAILED(hr) ? hr : factory->CreateSoftwareAdapter(Module, ppAdapter);
  }
  
  
  HRESULT STDMETHODCALLTYPE DxgiFactory::CreateSwapChain(
          IUnknown*             pDevice,
          DXGI_SWAP_CHAIN_DESC* pDesc,
          IDXGISwapChain**      ppSwapChain) {
    if (ppSwapChain == nullptr || pDesc == nullptr || pDevice == nullptr)
      return DXGI_ERROR_INVALID_CALL;
    
    DXGI_SWAP_CHAIN_DESC1 desc;
    desc.Width              = pDesc->BufferDesc.Width;
    desc.Height             = pDesc->BufferDesc.Height;
    desc.Format             = pDesc->BufferDesc.Format;
    desc.Stereo             = FALSE;
    desc.SampleDesc         = pDesc->SampleDesc;
    desc.BufferUsage        = pDesc->BufferUsage;
    desc.BufferCount        = pDesc->BufferCount;
    desc.Scaling            = DXGI_SCALING_STRETCH;
    desc.SwapEffect         = pDesc->SwapEffect;
    desc.AlphaMode          = DXGI_ALPHA_MODE_IGNORE;
    desc.Flags              = pDesc->Flags;
    
    DXGI_SWAP_CHAIN_FULLSCREEN_DESC descFs;
    descFs.RefreshRate      = pDesc->BufferDesc.RefreshRate;
    descFs.ScanlineOrdering = pDesc->BufferDesc.ScanlineOrdering;
    descFs.Scaling          = pDesc->BufferDesc.Scaling;
    descFs.Windowed         = pDesc->Windowed;
    
    IDXGISwapChain1* swapChain = nullptr;
    HRESULT hr = CreateSwapChainForHwnd(
      pDevice, pDesc->OutputWindow,
      &desc, &descFs, nullptr,
      &swapChain);
    
    *ppSwapChain = swapChain;
    return hr;
  }
  
  
  HRESULT STDMETHODCALLTYPE DxgiFactory::CreateSwapChainForHwnd(
          IUnknown*             pDevice,
          HWND                  hWnd,
    const DXGI_SWAP_CHAIN_DESC1* pDesc,
    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pFullscreenDesc,
          IDXGIOutput*          pRestrictToOutput,
          IDXGISwapChain1**     ppSwapChain) {
    InitReturnPtr(ppSwapChain);
    
    if (!ppSwapChain || !pDesc || !hWnd || !pDevice)
      return DXGI_ERROR_INVALID_CALL;
    
    Com<IWineDXGISwapChainFactory> wineDevice;
    
    if (SUCCEEDED(pDevice->QueryInterface(
          __uuidof(IWineDXGISwapChainFactory),
          reinterpret_cast<void**>(&wineDevice)))) {
      IDXGISwapChain4* frontendSwapChain;

      HRESULT hr = wineDevice->CreateSwapChainForHwnd(
        this, hWnd, pDesc, pFullscreenDesc,
        pRestrictToOutput, reinterpret_cast<IDXGISwapChain1**>(&frontendSwapChain));

      // No ref as that's handled by the object we're wrapping
      // which was ref'ed on creation.
      if (SUCCEEDED(hr))
        *ppSwapChain = new DxgiSwapChainDispatcher(frontendSwapChain);

      return hr;
    }
    
    // Native D3D12 command queues and native D3D11 devices do not implement
    // Remix's private swapchain factory. Keep their presentation on Windows
    // DXGI; this branch does not translate their rendering into Remix RTX.
    Com<IDXGIFactory2> nativeFactory;
    HRESULT hr = getNativeFactory(nativeFactory);
    if (FAILED(hr))
      return hr;
    Com<IDXGIOutput> nativeOutput;
    hr = getSystemDxgiOutput(nativeFactory.ptr(), pRestrictToOutput, nativeOutput);
    if (FAILED(hr))
      return hr;
    hr = nativeFactory->CreateSwapChainForHwnd(pDevice, hWnd, pDesc,
      pFullscreenDesc, nativeOutput.ptr(), ppSwapChain);
    if (SUCCEEDED(hr)) {
      // Windows applies these flags to swapchains already owned by the factory.
      // A request saved before its first native swapchain must be applied now;
      // applying it during factory construction silently leaves monitoring on.
      std::lock_guard<std::mutex> lock(m_nativeMutex);
      if (m_hasWindowAssociation && (!m_associatedWindow || m_associatedWindow == hWnd)) {
        const HRESULT associationResult = nativeFactory->MakeWindowAssociation(m_associatedWindow, m_windowAssociationFlags);
        if (FAILED(associationResult)) {
          (*ppSwapChain)->Release();
          *ppSwapChain = nullptr;
          return associationResult;
        }
      }
    }
    return hr;
  }
  
  
  HRESULT STDMETHODCALLTYPE DxgiFactory::CreateSwapChainForCoreWindow(
          IUnknown*             pDevice,
          IUnknown*             pWindow,
    const DXGI_SWAP_CHAIN_DESC1* pDesc,
          IDXGIOutput*          pRestrictToOutput,
          IDXGISwapChain1**     ppSwapChain) {
    InitReturnPtr(ppSwapChain);
    
    if (!pDevice || !pWindow || !pDesc || !ppSwapChain)
      return DXGI_ERROR_INVALID_CALL;
    Com<IDXGIFactory2> factory;
    HRESULT hr = getNativeFactory(factory);
    if (FAILED(hr))
      return hr;
    Com<IDXGIOutput> output;
    hr = getSystemDxgiOutput(factory.ptr(), pRestrictToOutput, output);
    return FAILED(hr) ? hr : factory->CreateSwapChainForCoreWindow(pDevice, pWindow, pDesc, output.ptr(), ppSwapChain);
  }
  
  
  HRESULT STDMETHODCALLTYPE DxgiFactory::CreateSwapChainForComposition(
          IUnknown*             pDevice,
    const DXGI_SWAP_CHAIN_DESC1* pDesc,
          IDXGIOutput*          pRestrictToOutput,
          IDXGISwapChain1**     ppSwapChain) {
    InitReturnPtr(ppSwapChain);
    
    if (!pDevice || !pDesc || !ppSwapChain)
      return DXGI_ERROR_INVALID_CALL;
    Com<IDXGIFactory2> factory;
    HRESULT hr = getNativeFactory(factory);
    if (FAILED(hr))
      return hr;
    Com<IDXGIOutput> output;
    hr = getSystemDxgiOutput(factory.ptr(), pRestrictToOutput, output);
    return FAILED(hr) ? hr : factory->CreateSwapChainForComposition(pDevice, pDesc, output.ptr(), ppSwapChain);
  }
  
  
  HRESULT STDMETHODCALLTYPE DxgiFactory::EnumAdapters(
          UINT            Adapter,
          IDXGIAdapter**  ppAdapter) {
    InitReturnPtr(ppAdapter);
    
    if (ppAdapter == nullptr)
      return DXGI_ERROR_INVALID_CALL;
    
    IDXGIAdapter1* handle = nullptr;
    HRESULT hr = this->EnumAdapters1(Adapter, &handle);
    *ppAdapter = handle;
    return hr;
  }
  
  
  HRESULT STDMETHODCALLTYPE DxgiFactory::EnumAdapters1(
          UINT            Adapter,
          IDXGIAdapter1** ppAdapter) {
    InitReturnPtr(ppAdapter);
    
    if (ppAdapter == nullptr)
      return DXGI_ERROR_INVALID_CALL;
    
    Rc<DxvkAdapter> dxvkAdapter
      = m_instance->enumAdapters(Adapter);
    
    if (dxvkAdapter == nullptr)
      return DXGI_ERROR_NOT_FOUND;
    
    *ppAdapter = ref(new DxgiAdapter(this, dxvkAdapter, Adapter));
    return S_OK;
  }
  
  
  HRESULT STDMETHODCALLTYPE DxgiFactory::EnumAdapterByLuid(
          LUID                  AdapterLuid,
          REFIID                riid,
          void**                ppvAdapter) {
    InitReturnPtr(ppvAdapter);
    uint32_t adapterId = 0;

    while (true) {
      Com<IDXGIAdapter> adapter;
      HRESULT hr = EnumAdapters(adapterId++, &adapter);

      if (FAILED(hr))
        return hr;
      
      DXGI_ADAPTER_DESC desc;
      adapter->GetDesc(&desc);

      if (!std::memcmp(&AdapterLuid, &desc.AdapterLuid, sizeof(LUID)))
        return adapter->QueryInterface(riid, ppvAdapter);
    }

    // This should be unreachable
    return DXGI_ERROR_NOT_FOUND;
  }

  
  HRESULT STDMETHODCALLTYPE DxgiFactory::EnumAdapterByGpuPreference(
          UINT                  Adapter,
          DXGI_GPU_PREFERENCE   GpuPreference,
          REFIID                riid,
          void**                ppvAdapter) {
    InitReturnPtr(ppvAdapter);
    uint32_t adapterCount = m_instance->adapterCount();

    if (Adapter >= adapterCount)
      return DXGI_ERROR_NOT_FOUND;

    // We know that the backend lists dedicated GPUs before
    // any integrated ones, so just list adapters in reverse
    // order. We have no other way to estimate performance.
    if (GpuPreference == DXGI_GPU_PREFERENCE_MINIMUM_POWER)
      Adapter = adapterCount - Adapter - 1;

    Com<IDXGIAdapter> adapter;
    HRESULT hr = this->EnumAdapters(Adapter, &adapter);

    if (FAILED(hr))
      return hr;

    return adapter->QueryInterface(riid, ppvAdapter);
  }


  HRESULT STDMETHODCALLTYPE DxgiFactory::EnumWarpAdapter(
          REFIID                riid,
          void**                ppvAdapter) {
    InitReturnPtr(ppvAdapter);

    Com<IDXGIFactory2> factory;
    HRESULT hr = getNativeFactory(factory);
    if (FAILED(hr))
      return hr;
    Com<IDXGIFactory4> factory4;
    hr = factory->QueryInterface(__uuidof(IDXGIFactory4), reinterpret_cast<void**>(&factory4));
    return FAILED(hr) ? hr : factory4->EnumWarpAdapter(riid, ppvAdapter);
  }


  HRESULT STDMETHODCALLTYPE DxgiFactory::GetWindowAssociation(HWND *pWindowHandle) {
    if (pWindowHandle == nullptr)
      return DXGI_ERROR_INVALID_CALL;
    
    std::lock_guard<std::mutex> lock(m_nativeMutex);
    if (m_nativeFactory != nullptr)
      return m_nativeFactory->GetWindowAssociation(pWindowHandle);
    *pWindowHandle = m_associatedWindow;
    return S_OK;
  }
  
  
  HRESULT STDMETHODCALLTYPE DxgiFactory::GetSharedResourceAdapterLuid(
          HANDLE                hResource,
          LUID*                 pLuid) {
    Com<IDXGIFactory2> factory;
    const HRESULT hr = getNativeFactory(factory);
    return FAILED(hr) ? hr : factory->GetSharedResourceAdapterLuid(hResource, pLuid);
  }
  
  
  HRESULT STDMETHODCALLTYPE DxgiFactory::MakeWindowAssociation(HWND WindowHandle, UINT Flags) {
    if ((Flags & ~(DXGI_MWA_NO_WINDOW_CHANGES | DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_PRINT_SCREEN))
        || (!WindowHandle && Flags) || (WindowHandle && !IsWindow(WindowHandle)))
      return DXGI_ERROR_INVALID_CALL;
    std::lock_guard<std::mutex> lock(m_nativeMutex);
    if (m_nativeFactory != nullptr) {
      const HRESULT hr = m_nativeFactory->MakeWindowAssociation(WindowHandle, Flags);
      if (FAILED(hr))
        return hr;
    }
    m_associatedWindow = WindowHandle;
    m_windowAssociationFlags = Flags;
    m_hasWindowAssociation = true;
    return S_OK;
  }
  
  
  BOOL STDMETHODCALLTYPE DxgiFactory::IsCurrent() {
    std::lock_guard<std::mutex> lock(m_nativeMutex);
    return m_nativeFactory == nullptr || m_nativeFactory->IsCurrent();
  }
  
  
  HRESULT STDMETHODCALLTYPE DxgiFactory::RegisterOcclusionStatusWindow(
          HWND                  WindowHandle,
          UINT                  wMsg,
          DWORD*                pdwCookie) {
    Com<IDXGIFactory2> factory;
    const HRESULT hr = getNativeFactory(factory);
    return FAILED(hr) ? hr : factory->RegisterOcclusionStatusWindow(WindowHandle, wMsg, pdwCookie);
  }
  
  
  HRESULT STDMETHODCALLTYPE DxgiFactory::RegisterStereoStatusEvent(
          HANDLE                hEvent,
          DWORD*                pdwCookie) {
    Com<IDXGIFactory2> factory;
    const HRESULT hr = getNativeFactory(factory);
    return FAILED(hr) ? hr : factory->RegisterStereoStatusEvent(hEvent, pdwCookie);
  }
  
  
  HRESULT STDMETHODCALLTYPE DxgiFactory::RegisterStereoStatusWindow(
          HWND                  WindowHandle,
          UINT                  wMsg,
          DWORD*                pdwCookie) {
    Com<IDXGIFactory2> factory;
    const HRESULT hr = getNativeFactory(factory);
    return FAILED(hr) ? hr : factory->RegisterStereoStatusWindow(WindowHandle, wMsg, pdwCookie);
  }
  

  HRESULT STDMETHODCALLTYPE DxgiFactory::RegisterOcclusionStatusEvent(
          HANDLE                hEvent,
          DWORD*                pdwCookie) {
    Com<IDXGIFactory2> factory;
    const HRESULT hr = getNativeFactory(factory);
    return FAILED(hr) ? hr : factory->RegisterOcclusionStatusEvent(hEvent, pdwCookie);
  }
  

  void STDMETHODCALLTYPE DxgiFactory::UnregisterStereoStatus(
          DWORD                 dwCookie) {
    std::lock_guard<std::mutex> lock(m_nativeMutex);
    if (m_nativeFactory != nullptr)
      m_nativeFactory->UnregisterStereoStatus(dwCookie);
  }
  
  
  void STDMETHODCALLTYPE DxgiFactory::UnregisterOcclusionStatus(
          DWORD                 dwCookie) {
    std::lock_guard<std::mutex> lock(m_nativeMutex);
    if (m_nativeFactory != nullptr)
      m_nativeFactory->UnregisterOcclusionStatus(dwCookie);
  }


  UINT STDMETHODCALLTYPE DxgiFactory::GetCreationFlags() {
    return m_flags;
  }


  HRESULT STDMETHODCALLTYPE DxgiFactory::CheckFeatureSupport(
          DXGI_FEATURE          Feature,
          void*                 pFeatureSupportData,
          UINT                  FeatureSupportDataSize) {
    switch (Feature) {
      case DXGI_FEATURE_PRESENT_ALLOW_TEARING: {
        if (!pFeatureSupportData || FeatureSupportDataSize != sizeof(BOOL))
          return E_INVALIDARG;

        auto info = static_cast<BOOL*>(pFeatureSupportData);
        
        *info = TRUE;
      } return S_OK;

      default:
        Logger::err(str::format("DxgiFactory: CheckFeatureSupport: Unknown feature: ", uint32_t(Feature)));
        return E_INVALIDARG;
    }
  }


  HRESULT STDMETHODCALLTYPE DxgiFactory::RegisterAdaptersChangedEvent(
          HANDLE                hEvent,
          DWORD*                pdwCookie) {
    Com<IDXGIFactory2> factory;
    HRESULT hr = getNativeFactory(factory);
    if (FAILED(hr))
      return hr;
    Com<IDXGIFactory7> factory7;
    hr = factory->QueryInterface(__uuidof(IDXGIFactory7), reinterpret_cast<void**>(&factory7));
    return FAILED(hr) ? hr : factory7->RegisterAdaptersChangedEvent(hEvent, pdwCookie);
  }


  HRESULT STDMETHODCALLTYPE DxgiFactory::UnregisterAdaptersChangedEvent(
          DWORD                 Cookie) {
    Com<IDXGIFactory2> factory;
    HRESULT hr = getNativeFactory(factory);
    if (FAILED(hr))
      return hr;
    Com<IDXGIFactory7> factory7;
    hr = factory->QueryInterface(__uuidof(IDXGIFactory7), reinterpret_cast<void**>(&factory7));
    return FAILED(hr) ? hr : factory7->UnregisterAdaptersChangedEvent(Cookie);
  }


}
