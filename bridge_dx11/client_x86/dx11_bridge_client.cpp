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

  // Report the 32-bit host's usable address space once at startup. The bridge
  // keeps the large RTX allocations (BVH, replacement textures, the path
  // tracer's working set) in the x64 server, so the 32-bit game process only
  // ever holds its own assets plus this thin proxy. The remaining limit is the
  // game .exe's own user address space: 2 GB unless the EXE header has the
  // LARGEADDRESSAWARE bit, which raises it to 4 GB on 64-bit Windows. Flipping
  // that bit requires patching the game's PE header (a launcher/installer
  // step, not something a loaded DLL can do to its already-running host), so
  // we surface the current state clearly instead.
  {
    MEMORYSTATUSEX ms = {}; ms.dwLength = sizeof(ms);
    GlobalMemoryStatusEx(&ms);

    bool exeIsLargeAddressAware = false;
    if (HMODULE exe = GetModuleHandleW(nullptr)) {
      auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(exe);
      if (dos->e_magic == IMAGE_DOS_SIGNATURE) {
        auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(
          reinterpret_cast<const uint8_t*>(exe) + dos->e_lfanew);
        if (nt->Signature == IMAGE_NT_SIGNATURE) {
          exeIsLargeAddressAware =
            (nt->FileHeader.Characteristics & IMAGE_FILE_LARGE_ADDRESS_AWARE) != 0;
        }
      }
    }

    Log(L"host process: %.0f MB total VA, LargeAddressAware=%d (%s). Large RTX "
        L"allocations live in the x64 server; the 32-bit game's own address "
        L"space is the only in-process limit.",
        (double) (ms.ullTotalVirtual / (1024ull * 1024ull)),
        exeIsLargeAddressAware ? 1 : 0,
        exeIsLargeAddressAware ? L"4 GB user space"
                               : L"2 GB user space - patch the EXE LAA bit for 4 GB");
  }
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

  // Launch OUR server under its own name (NvRemixDx11Bridge.exe), never the
  // stock NVIDIA NvRemixBridge.exe. A game folder that previously had any
  // NVIDIA Remix runtime installed will already contain NvRemixBridge.exe,
  // and that stock server speaks the D3D9 GUID-handshake protocol - launching
  // it produced "Server was invoked with invalid GUID! Unable to establish
  // bridge, exiting..." in bridge64.log because it never sees our env pipe.
  // Prefer the DX11-named binary; fall back to the legacy name only if a
  // DX11-named one is not staged (older deploys), and warn in that case.
  wchar_t exe[MAX_PATH]; StringCchPrintfW(exe, MAX_PATH, L"%s\\NvRemixDx11Bridge.exe", trex);
  if (!PathFileExistsW(exe)) {
    wchar_t legacy[MAX_PATH]; StringCchPrintfW(legacy, MAX_PATH, L"%s\\NvRemixBridge.exe", trex);
    if (!PathFileExistsW(legacy)) { Log(L"missing DX11 bridge server: %s", exe); return false; }
    Log(L"WARNING: NvRemixDx11Bridge.exe not found; launching %s. If this is the stock NVIDIA D3D9 bridge the handshake will fail - stage the DX11 server build into the .trex folder.", legacy);
    StringCchCopyW(exe, MAX_PATH, legacy);
  }

  wchar_t cmd[2048];
  // Our server reads the pipe name from the environment (NVREMIX_DX11_BRIDGE_PIPE)
  // and ignores argv, but we still pass a version token for forward-compat and
  // so the process command line is self-describing in a debugger / log.
  StringCchPrintfW(cmd, 2048, L"\"%s\" remix-dx11 v%u", exe, (unsigned) kProtocolVersion);
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
