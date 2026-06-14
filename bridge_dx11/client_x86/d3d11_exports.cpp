#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <cstring>
#include "dx11_bridge_client.h"
using namespace remix_dx11_bridge;
using namespace remix_dx11_bridge_client;

extern "C" HRESULT WINAPI D3D11CreateDevice(
  IDXGIAdapter* pAdapter, D3D_DRIVER_TYPE DriverType, HMODULE Software, UINT Flags,
  const D3D_FEATURE_LEVEL* pFeatureLevels, UINT FeatureLevels, UINT SDKVersion,
  ID3D11Device** ppDevice, D3D_FEATURE_LEVEL* pFeatureLevel, ID3D11DeviceContext** ppImmediateContext) {
  if (ppDevice) *ppDevice = nullptr; if (ppImmediateContext) *ppImmediateContext = nullptr;
  BridgeConnection c; if (!Connect(c)) return DXGI_ERROR_DEVICE_REMOVED;
  uint32_t bytes = sizeof(CreateDevicePayload) + FeatureLevels * sizeof(D3D_FEATURE_LEVEL);
  char stack[sizeof(CreateDevicePayload) + 64 * sizeof(D3D_FEATURE_LEVEL)] = {};
  if (FeatureLevels > 64) return E_INVALIDARG;
  auto* p = reinterpret_cast<CreateDevicePayload*>(stack);
  p->adapterHandle = reinterpret_cast<uintptr_t>(pAdapter); p->driverType = DriverType; p->flags = Flags; p->featureLevelCount = FeatureLevels; p->sdkVersion = SDKVersion;
  if (FeatureLevels && pFeatureLevels) memcpy(stack + sizeof(CreateDevicePayload), pFeatureLevels, FeatureLevels * sizeof(D3D_FEATURE_LEVEL));
  Reply r = {}; HRESULT hr = Send(c, Cmd::D3D11CreateDevice, 0, stack, bytes, r); CloseHandle(c.pipe);
  if (pFeatureLevel && FeatureLevels) *pFeatureLevel = pFeatureLevels ? pFeatureLevels[0] : D3D_FEATURE_LEVEL_11_0;
  return FAILED(hr) ? hr : r.hr;
}

extern "C" HRESULT WINAPI D3D11CreateDeviceAndSwapChain(
  IDXGIAdapter* pAdapter, D3D_DRIVER_TYPE DriverType, HMODULE Software, UINT Flags,
  const D3D_FEATURE_LEVEL* pFeatureLevels, UINT FeatureLevels, UINT SDKVersion,
  const DXGI_SWAP_CHAIN_DESC* pSwapChainDesc, IDXGISwapChain** ppSwapChain,
  ID3D11Device** ppDevice, D3D_FEATURE_LEVEL* pFeatureLevel, ID3D11DeviceContext** ppImmediateContext) {
  if (ppSwapChain) *ppSwapChain = nullptr; if (ppDevice) *ppDevice = nullptr; if (ppImmediateContext) *ppImmediateContext = nullptr;
  if (!pSwapChainDesc) return E_INVALIDARG;
  BridgeConnection c; if (!Connect(c)) return DXGI_ERROR_DEVICE_REMOVED;
  uint32_t flBytes = FeatureLevels * sizeof(D3D_FEATURE_LEVEL);
  uint32_t bytes = sizeof(CreateDeviceSwapChainPayload) + flBytes + sizeof(DXGI_SWAP_CHAIN_DESC);
  char* buf = (char*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, bytes); if (!buf) return E_OUTOFMEMORY;
  auto* p = reinterpret_cast<CreateDeviceSwapChainPayload*>(buf);
  p->adapterHandle = reinterpret_cast<uintptr_t>(pAdapter); p->driverType = DriverType; p->flags = Flags; p->featureLevelCount = FeatureLevels; p->sdkVersion = SDKVersion; p->swapChainDescBytes = sizeof(DXGI_SWAP_CHAIN_DESC);
  if (FeatureLevels && pFeatureLevels) memcpy(buf + sizeof(*p), pFeatureLevels, flBytes);
  memcpy(buf + sizeof(*p) + flBytes, pSwapChainDesc, sizeof(DXGI_SWAP_CHAIN_DESC));
  Reply r = {}; HRESULT hr = Send(c, Cmd::D3D11CreateDeviceAndSwapChain, 0, buf, bytes, r); HeapFree(GetProcessHeap(),0,buf); CloseHandle(c.pipe);
  if (pFeatureLevel && FeatureLevels) *pFeatureLevel = pFeatureLevels ? pFeatureLevels[0] : D3D_FEATURE_LEVEL_11_0;
  return FAILED(hr) ? hr : r.hr;
}

extern "C" HRESULT WINAPI D3D11On12CreateDevice(IUnknown*, UINT, const D3D_FEATURE_LEVEL*, UINT, IUnknown* const*, UINT, UINT, ID3D11Device**, ID3D11DeviceContext**, D3D_FEATURE_LEVEL*) {
  return DXGI_ERROR_UNSUPPORTED;
}

BOOL APIENTRY DllMain(HMODULE, DWORD reason, LPVOID) { if (reason == DLL_PROCESS_ATTACH) BridgeHello(); return TRUE; }
