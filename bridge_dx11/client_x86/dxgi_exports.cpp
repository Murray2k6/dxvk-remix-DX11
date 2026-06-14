#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <dxgi1_2.h>
#include "dx11_bridge_client.h"
using namespace remix_dx11_bridge;
using namespace remix_dx11_bridge_client;

static HRESULT CreateFactoryCommon(Cmd cmd, UINT flags, REFIID riid, void** ppFactory) {
  if (!ppFactory) return E_POINTER; *ppFactory = nullptr;
  BridgeConnection c; if (!Connect(c)) return DXGI_ERROR_DEVICE_REMOVED;
  CreateFactoryPayload p = { flags, riid }; Reply r = {};
  HRESULT hr = Send(c, cmd, 0, &p, sizeof(p), r); CloseHandle(c.pipe);
  return FAILED(hr) ? hr : r.hr;
}

extern "C" HRESULT WINAPI CreateDXGIFactory(REFIID riid, void** ppFactory) { return CreateFactoryCommon(Cmd::CreateDXGIFactory, 0, riid, ppFactory); }
extern "C" HRESULT WINAPI CreateDXGIFactory1(REFIID riid, void** ppFactory) { return CreateFactoryCommon(Cmd::CreateDXGIFactory1, 0, riid, ppFactory); }
extern "C" HRESULT WINAPI CreateDXGIFactory2(UINT Flags, REFIID riid, void** ppFactory) { return CreateFactoryCommon(Cmd::CreateDXGIFactory2, Flags, riid, ppFactory); }
extern "C" HRESULT WINAPI DXGIGetDebugInterface1(UINT, REFIID, void**) { return DXGI_ERROR_UNSUPPORTED; }
extern "C" HRESULT WINAPI DXGIDeclareAdapterRemovalSupport() { return S_OK; }

BOOL APIENTRY DllMain(HMODULE, DWORD reason, LPVOID) { if (reason == DLL_PROCESS_ATTACH) BridgeHello(); return TRUE; }
