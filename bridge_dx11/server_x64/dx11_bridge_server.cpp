#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <strsafe.h>
#include <stdint.h>
#include <stdarg.h>
#include <wchar.h>
#include "../shared/dx11_bridge_protocol.h"
using namespace remix_dx11_bridge;

static wchar_t g_trex[MAX_PATH];
static HMODULE g_d3d11 = nullptr, g_dxgi = nullptr;

template<typename T> static T LoadProc(HMODULE m, const char* n) { return reinterpret_cast<T>(GetProcAddress(m,n)); }
static void Log(const wchar_t* fmt, ...) {
  wchar_t path[MAX_PATH]; StringCchPrintfW(path, MAX_PATH, L"%s\\remix-dx11-bridge-server.log", g_trex);
  HANDLE h = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ|FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return;
  wchar_t msg[4096]; va_list ap; va_start(ap, fmt); StringCchVPrintfW(msg,4096,fmt,ap); va_end(ap);
  wchar_t line[4608]; StringCchPrintfW(line,4608,L"%s\r\n",msg); DWORD wr=0; WriteFile(h,line,(DWORD)(wcslen(line)*sizeof(wchar_t)),&wr,nullptr); CloseHandle(h);
}
static bool LoadRuntime() {
  GetModuleFileNameW(nullptr, g_trex, MAX_PATH); wchar_t* slash = wcsrchr(g_trex, L'\\'); if (slash) *slash = 0;
  wchar_t d3d11[MAX_PATH], dxgi[MAX_PATH]; StringCchPrintfW(d3d11,MAX_PATH,L"%s\\d3d11.dll",g_trex); StringCchPrintfW(dxgi,MAX_PATH,L"%s\\dxgi.dll",g_trex);
  g_dxgi = LoadLibraryW(dxgi); g_d3d11 = LoadLibraryW(d3d11);
  Log(L"DX11 bridge server runtime load dxgi=%p d3d11=%p", g_dxgi, g_d3d11);
  return g_dxgi && g_d3d11;
}
static void SendReply(HANDLE pipe, const Header& h, HRESULT hr, Handle32 handle=0, ObjectKind kind=ObjectKind::Unknown) {
  Reply r = { kProtocolMagic, kProtocolVersion, h.uid, hr, handle, kind, 0 }; DWORD wr=0; WriteFile(pipe,&r,sizeof(r),&wr,nullptr);
}
static void DrainPayload(HANDLE pipe, uint32_t bytes, void* out=nullptr) {
  DWORD rd=0; if (!bytes) return; if (out) { ReadFile(pipe,out,bytes,&rd,nullptr); return; }
  char tmp[4096]; while(bytes){ DWORD n=bytes>sizeof(tmp)?sizeof(tmp):bytes; if(!ReadFile(pipe,tmp,n,&rd,nullptr)||!rd)break; bytes-=rd; }
}
static HRESULT HandleCreateDevice(HANDLE pipe, const Header& h, bool swapchain) {
  char* buf=(char*)HeapAlloc(GetProcessHeap(),0,h.payloadBytes); if(!buf){DrainPayload(pipe,h.payloadBytes); return E_OUTOFMEMORY;} DrainPayload(pipe,h.payloadBytes,buf);
  using PFN = HRESULT (WINAPI*)(IDXGIAdapter*,D3D_DRIVER_TYPE,HMODULE,UINT,const D3D_FEATURE_LEVEL*,UINT,UINT,ID3D11Device**,D3D_FEATURE_LEVEL*,ID3D11DeviceContext**);
  using PFNSC = HRESULT (WINAPI*)(IDXGIAdapter*,D3D_DRIVER_TYPE,HMODULE,UINT,const D3D_FEATURE_LEVEL*,UINT,UINT,const DXGI_SWAP_CHAIN_DESC*,IDXGISwapChain**,ID3D11Device**,D3D_FEATURE_LEVEL*,ID3D11DeviceContext**);
  HRESULT hr=E_FAIL;
  if (!swapchain) {
    auto* p=(CreateDevicePayload*)buf; const D3D_FEATURE_LEVEL* fl=(const D3D_FEATURE_LEVEL*)(buf+sizeof(*p));
    auto fn=LoadProc<PFN>(g_d3d11,"D3D11CreateDevice"); ID3D11Device* dev=nullptr; ID3D11DeviceContext* ctx=nullptr; D3D_FEATURE_LEVEL got{};
    hr=fn?fn(nullptr,(D3D_DRIVER_TYPE)p->driverType,nullptr,p->flags,fl,p->featureLevelCount,p->sdkVersion,&dev,&got,&ctx):E_FAIL;
    if (ctx) ctx->Release(); if (dev) dev->Release();
  } else {
    auto* p=(CreateDeviceSwapChainPayload*)buf; const D3D_FEATURE_LEVEL* fl=(const D3D_FEATURE_LEVEL*)(buf+sizeof(*p)); const DXGI_SWAP_CHAIN_DESC* sd=(const DXGI_SWAP_CHAIN_DESC*)(buf+sizeof(*p)+p->featureLevelCount*sizeof(D3D_FEATURE_LEVEL));
    auto fn=LoadProc<PFNSC>(g_d3d11,"D3D11CreateDeviceAndSwapChain"); IDXGISwapChain* sc=nullptr; ID3D11Device* dev=nullptr; ID3D11DeviceContext* ctx=nullptr; D3D_FEATURE_LEVEL got{};
    hr=fn?fn(nullptr,(D3D_DRIVER_TYPE)p->driverType,nullptr,p->flags,fl,p->featureLevelCount,p->sdkVersion,sd,&sc,&dev,&got,&ctx):E_FAIL;
    if(sc)sc->Release(); if(ctx)ctx->Release(); if(dev)dev->Release();
  }
  HeapFree(GetProcessHeap(),0,buf); return hr;
}
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
  const wchar_t* pipeName = nullptr;
  wchar_t env[512]; DWORD n=GetEnvironmentVariableW(L"NVREMIX_DX11_BRIDGE_PIPE",env,512); if(n>0&&n<512) pipeName=env;
  if(!pipeName) return 2; if(!LoadRuntime()) return 3;
  HANDLE pipe=CreateNamedPipeW(pipeName,PIPE_ACCESS_DUPLEX,PIPE_TYPE_MESSAGE|PIPE_READMODE_MESSAGE|PIPE_WAIT,1,1<<20,1<<20,0,nullptr);
  if(pipe==INVALID_HANDLE_VALUE) return 4; if(!ConnectNamedPipe(pipe,nullptr)&&GetLastError()!=ERROR_PIPE_CONNECTED) return 5;
  for(;;){ Header h={}; DWORD rd=0; if(!ReadFile(pipe,&h,sizeof(h),&rd,nullptr)||rd!=sizeof(h)) break; if(h.magic!=kProtocolMagic){DrainPayload(pipe,h.payloadBytes); SendReply(pipe,h,E_INVALIDARG); continue;}
    switch(h.cmd){
      case Cmd::Hello: DrainPayload(pipe,h.payloadBytes); SendReply(pipe,h,S_OK); break;
      case Cmd::Shutdown: DrainPayload(pipe,h.payloadBytes); SendReply(pipe,h,S_OK); CloseHandle(pipe); return 0;
      case Cmd::D3D11CreateDevice: { HRESULT hr=HandleCreateDevice(pipe,h,false); SendReply(pipe,h,hr); break; }
      case Cmd::D3D11CreateDeviceAndSwapChain: { HRESULT hr=HandleCreateDevice(pipe,h,true); SendReply(pipe,h,hr); break; }
      case Cmd::CreateDXGIFactory:
      case Cmd::CreateDXGIFactory1:
      case Cmd::CreateDXGIFactory2: { DrainPayload(pipe,h.payloadBytes); SendReply(pipe,h,S_OK,1,ObjectKind::Factory); break; }
      default: DrainPayload(pipe,h.payloadBytes); SendReply(pipe,h,DXGI_ERROR_UNSUPPORTED); break;
    }
  }
  CloseHandle(pipe); return 0;
}
