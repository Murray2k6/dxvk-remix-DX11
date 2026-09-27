#include "dxgi_native.h"

#include <cstring>
#include <cwchar>

namespace dxvk {
  FARPROC getSystemDxgiProcAddress(const char* name) {
    // One process-lifetime reference: forwarded native COM objects can outlive
    // any proxy factory. Never load by bare name and re-enter this DLL.
    static HMODULE systemDxgi = [] {
      wchar_t path[MAX_PATH] = {};
      const UINT length = GetSystemDirectoryW(path, MAX_PATH);
      if (!length || length + 10 >= MAX_PATH)
        return HMODULE(nullptr);
      wcscat_s(path, L"\\dxgi.dll");
      return LoadLibraryExW(path, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    }();
    return systemDxgi ? GetProcAddress(systemDxgi, name) : nullptr;
  }

  HRESULT createSystemDxgiFactory(UINT flags, const char* name, REFIID iid, void** factory) {
    if (!factory)
      return E_POINTER;
    *factory = nullptr;
    const auto address = getSystemDxgiProcAddress(name);
    if (!address)
      return HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
    if (!std::strcmp(name, "CreateDXGIFactory2")) {
      const auto create = reinterpret_cast<HRESULT (WINAPI*)(UINT, REFIID, void**)>(address);
      return create(flags, iid, factory);
    }
    const auto create = reinterpret_cast<HRESULT (WINAPI*)(REFIID, void**)>(address);
    return create(iid, factory);
  }

  static bool sameLuid(const LUID& a, const LUID& b) {
    return a.LowPart == b.LowPart && a.HighPart == b.HighPart;
  }

  HRESULT getSystemDxgiAdapter(IDXGIFactory1* factory, IDXGIAdapter* adapter, Com<IDXGIAdapter>& nativeAdapter) {
    nativeAdapter = nullptr;
    if (!adapter)
      return S_OK;
    DXGI_ADAPTER_DESC requested = {};
    HRESULT hr = adapter->GetDesc(&requested);
    if (FAILED(hr))
      return hr;
    for (UINT index = 0; ; ++index) {
      Com<IDXGIAdapter1> candidate;
      hr = factory->EnumAdapters1(index, &candidate);
      if (FAILED(hr))
        return hr;
      DXGI_ADAPTER_DESC1 description = {};
      hr = candidate->GetDesc1(&description);
      if (FAILED(hr))
        return hr;
      if (sameLuid(requested.AdapterLuid, description.AdapterLuid))
        return candidate->QueryInterface(__uuidof(IDXGIAdapter), reinterpret_cast<void**>(&nativeAdapter));
    }
  }

  HRESULT getSystemDxgiOutput(IDXGIFactory1* factory, IDXGIOutput* output, Com<IDXGIOutput>& nativeOutput) {
    nativeOutput = nullptr;
    if (!output)
      return S_OK;
    DXGI_OUTPUT_DESC requested = {};
    HRESULT hr = output->GetDesc(&requested);
    if (FAILED(hr))
      return hr;
    Com<IDXGIAdapter> parent;
    hr = output->GetParent(__uuidof(IDXGIAdapter), reinterpret_cast<void**>(&parent));
    if (FAILED(hr))
      return hr;
    DXGI_ADAPTER_DESC parentDescription = {};
    hr = parent->GetDesc(&parentDescription);
    if (FAILED(hr))
      return hr;

    // Prefer the reported adapter LUID. DXVK's output enumeration also exposes
    // monitors attached to another adapter, so match that physical monitor in
    // a second pass rather than forwarding a foreign proxy output to Windows.
    for (unsigned pass = 0; pass < 2; ++pass) {
      for (UINT adapterIndex = 0; ; ++adapterIndex) {
        Com<IDXGIAdapter1> adapter;
        hr = factory->EnumAdapters1(adapterIndex, &adapter);
        if (hr == DXGI_ERROR_NOT_FOUND)
          break;
        if (FAILED(hr))
          return hr;
        DXGI_ADAPTER_DESC1 description = {};
        hr = adapter->GetDesc1(&description);
        if (FAILED(hr))
          return hr;
        if (sameLuid(parentDescription.AdapterLuid, description.AdapterLuid) != (pass == 0))
          continue;
        for (UINT outputIndex = 0; ; ++outputIndex) {
          Com<IDXGIOutput> candidate;
          hr = adapter->EnumOutputs(outputIndex, &candidate);
          if (hr == DXGI_ERROR_NOT_FOUND)
            break;
          if (FAILED(hr))
            return hr;
          DXGI_OUTPUT_DESC candidateDescription = {};
          hr = candidate->GetDesc(&candidateDescription);
          if (FAILED(hr))
            return hr;
          if (requested.Monitor && requested.Monitor == candidateDescription.Monitor) {
            nativeOutput = std::move(candidate);
            return S_OK;
          }
          if (!requested.Monitor && !candidateDescription.Monitor
              && !wcscmp(requested.DeviceName, candidateDescription.DeviceName)) {
            nativeOutput = std::move(candidate);
            return S_OK;
          }
        }
      }
    }
    return DXGI_ERROR_NOT_FOUND;
  }
}
