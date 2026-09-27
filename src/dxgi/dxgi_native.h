#pragma once

#include "dxgi_include.h"

namespace dxvk {
  FARPROC getSystemDxgiProcAddress(const char* name);
  HRESULT createSystemDxgiFactory(UINT flags, const char* name, REFIID iid, void** factory);
  HRESULT getSystemDxgiAdapter(IDXGIFactory1* factory, IDXGIAdapter* adapter, Com<IDXGIAdapter>& nativeAdapter);
  HRESULT getSystemDxgiOutput(IDXGIFactory1* factory, IDXGIOutput* output, Com<IDXGIOutput>& nativeOutput);
}
