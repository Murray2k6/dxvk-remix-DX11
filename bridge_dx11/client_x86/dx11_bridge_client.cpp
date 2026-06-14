#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "dx11_bridge_client.h"
#include <shlwapi.h>
#include <stdio.h>
#include <stdarg.h>
#pragma comment(lib, "shlwapi.lib")

using namespace remix_dx11_bridge;

namespace remix_dx11_bridge_client {
static INIT_ONCE g_once = INIT_ONCE_STATIC_INIT;
static CRITICAL_SECTION g_cs;
static wchar_t g_folder[MAX_PATH] = {};
static wchar_t g_pipeName[256] = {};
static HANDLE g_serverProcess = nullptr;

static BOOL CALLBACK InitOnceFn(PINIT_ONCE, PVOID, PVOID*) {
  InitializeCriticalSection(&g_cs);
  HMODULE self = nullptr;
  GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
    reinterpret_cast<LPCWSTR>(&InitOnceFn), &self);
  GetModuleFileNameW(self, g_folder, MAX_PATH);
  PathRemoveFileSpecW(g_folder);
  StringCchPrintfW(g_pipeName, 256, L"\\.\\pipe\\NvRemixDx11Bridge_%lu", GetCurrentProcessId());
  return TRUE;
}
static void Init() { InitOnceExecuteOnce(&g_once, InitOnceFn, nullptr, nullptr); }

void Log(const wchar_t* fmt, ...) {
  Init();
  EnterCriticalSection(&g_cs);
  wchar_t path[MAX_PATH]; StringCchPrintfW(path, MAX_PATH, L"%s\\remix-dx11-bridge-client.log", g_folder);
  HANDLE h = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ|FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h != INVALID_HANDLE_VALUE) {
    wchar_t msg[4096]; va_list ap; va_start(ap, fmt); StringCchVPrintfW(msg, 4096, fmt, ap); va_end(ap);
    wchar_t line[4608]; SYSTEMTIME st; GetLocalTime(&st);
    StringCchPrintfW(line, 4608, L"[%02u:%02u:%02u.%03u] %s\r\n", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, msg);
    DWORD wr=0; WriteFile(h, line, (DWORD)(wcslen(line)*sizeof(wchar_t)), &wr, nullptr); CloseHandle(h);
  }
  LeaveCriticalSection(&g_cs);
}

static void NewGuidString(wchar_t* out, size_t chars) {
  GUID g; CoCreateGuid(&g);
  StringFromGUID2(g, out, (int)chars);
}

bool StartDx11BridgeServer() {
  Init();
  if (g_serverProcess) return true;
  wchar_t trex[MAX_PATH]; StringCchPrintfW(trex, MAX_PATH, L"%s\\.trex", g_folder);
  wchar_t exe[MAX_PATH]; StringCchPrintfW(exe, MAX_PATH, L"%s\\NvRemixBridge.exe", trex);
  if (!PathFileExistsW(exe)) { Log(L"missing DX11 bridge server: %s", exe); return false; }

  wchar_t guid[64]; NewGuidString(guid, 64);
  wchar_t cmd[2048];
  // NVIDIA bridge server expects argv[1]=GUID argv[2]=version. Keep that order; expose DX11 pipe in env.
  StringCchPrintfW(cmd, 2048, L"\"%s\" %s remix-dx11", exe, guid);
  wchar_t env[4096];
  StringCchPrintfW(env, 4096, L"NVREMIX_DX11_BRIDGE_PIPE=%s\0NVREMIX_DX11_CLIENT_PID=%lu\0NVREMIX_BRIDGE_API=dx11\0\0", g_pipeName, GetCurrentProcessId());

  STARTUPINFOW si = {}; PROCESS_INFORMATION pi = {}; si.cb = sizeof(si);
  BOOL ok = CreateProcessW(exe, cmd, nullptr, nullptr, FALSE, CREATE_NO_WINDOW|CREATE_UNICODE_ENVIRONMENT, env, trex, &si, &pi);
  if (!ok) { Log(L"CreateProcess failed for %s gle=%lu", exe, GetLastError()); return false; }
  CloseHandle(pi.hThread); g_serverProcess = pi.hProcess;
  Log(L"started DX11 bridge server pid=%lu pipe=%s", pi.dwProcessId, g_pipeName);
  return true;
}

bool Connect(BridgeConnection& out) {
  Init();
  out.pipe = INVALID_HANDLE_VALUE; out.uid = 1;
  if (!StartDx11BridgeServer()) return false;
  for (int i=0; i<100; ++i) {
    HANDLE h = CreateFileW(g_pipeName, GENERIC_READ|GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) { out.pipe = h; DWORD mode = PIPE_READMODE_MESSAGE; SetNamedPipeHandleState(h, &mode, nullptr, nullptr); return true; }
    Sleep(50);
  }
  Log(L"failed to connect pipe %s gle=%lu", g_pipeName, GetLastError());
  return false;
}

HRESULT Send(BridgeConnection& c, Cmd cmd, Handle32 self, const void* payload, uint32_t payloadBytes, Reply& reply, void* replyPayload, uint32_t* replyPayloadBytes) {
  if (c.pipe == INVALID_HANDLE_VALUE && !Connect(c)) return DXGI_ERROR_DEVICE_REMOVED;
  Header h = { kProtocolMagic, kProtocolVersion, cmd, c.uid++, self, payloadBytes };
  DWORD wr=0, rd=0;
  if (!WriteFile(c.pipe, &h, sizeof(h), &wr, nullptr) || wr != sizeof(h)) return HRESULT_FROM_WIN32(GetLastError());
  if (payloadBytes) {
    if (!payload || payloadBytes > kMaxInlineBytes) return E_INVALIDARG;
    if (!WriteFile(c.pipe, payload, payloadBytes, &wr, nullptr) || wr != payloadBytes) return HRESULT_FROM_WIN32(GetLastError());
  }
  if (!ReadFile(c.pipe, &reply, sizeof(reply), &rd, nullptr) || rd != sizeof(reply)) return HRESULT_FROM_WIN32(GetLastError());
  if (reply.magic != kProtocolMagic || reply.version != kProtocolVersion || reply.uid != h.uid) return E_FAIL;
  if (reply.payloadBytes && replyPayload && replyPayloadBytes && *replyPayloadBytes >= reply.payloadBytes) {
    if (!ReadFile(c.pipe, replyPayload, reply.payloadBytes, &rd, nullptr)) return HRESULT_FROM_WIN32(GetLastError());
    *replyPayloadBytes = rd;
  } else if (reply.payloadBytes) {
    char tmp[4096]; uint32_t left = reply.payloadBytes;
    while (left) { DWORD chunk = left > sizeof(tmp) ? sizeof(tmp) : left; if (!ReadFile(c.pipe,tmp,chunk,&rd,nullptr)) break; left -= rd; }
  }
  return reply.hr;
}

HRESULT BridgeHello() {
  BridgeConnection c; if (!Connect(c)) return DXGI_ERROR_DEVICE_REMOVED;
  HelloPayload p = { GetCurrentProcessId(), 32, 0, 0 }; Reply r = {};
  HRESULT hr = Send(c, Cmd::Hello, 0, &p, sizeof(p), r);
  CloseHandle(c.pipe);
  return FAILED(hr) ? hr : r.hr;
}
}
