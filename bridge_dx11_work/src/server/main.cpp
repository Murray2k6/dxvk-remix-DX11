#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <shellapi.h>
#include <string>
#include <vector>
#include <cstdio>
#include <cstdlib>

static void Dx11BridgeLog(const wchar_t* text) {
  OutputDebugStringW(text);
  OutputDebugStringW(L"\n");
  fwprintf(stderr, L"%ls\n", text);
}

static std::wstring Dx11BridgeModulePath() {
  std::vector<wchar_t> buffer(32768);
  DWORD len = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
  if (len == 0 || len >= buffer.size()) {
    return L"";
  }
  return std::wstring(buffer.data(), len);
}

static std::wstring Dx11BridgeDirOf(const std::wstring& path) {
  const size_t slash = path.find_last_of(L"\\/");
  if (slash == std::wstring::npos) {
    return L".";
  }
  return path.substr(0, slash);
}

static HMODULE Dx11BridgeLoadLocalFirst(const wchar_t* dllName) {
  const std::wstring exeDir = Dx11BridgeDirOf(Dx11BridgeModulePath());
  if (!exeDir.empty()) {
    const std::wstring localPath = exeDir + L"\\" + dllName;
    HMODULE local = LoadLibraryW(localPath.c_str());
    if (local != nullptr) {
      return local;
    }
  }

  return LoadLibraryW(dllName);
}

static int Dx11BridgeRun(int argc, wchar_t** argv) {
  SetEnvironmentVariableW(L"DX11_BRIDGE_MODE", L"1");

  const std::wstring exeDir = Dx11BridgeDirOf(Dx11BridgeModulePath());
  if (!exeDir.empty()) {
    SetDllDirectoryW(exeDir.c_str());
  }

  Dx11BridgeLog(L"[NvRemixBridge-DX11] DX11-only bridge server bootstrap starting.");

  for (int i = 0; i < argc; i++) {
    if (argv != nullptr && argv[i] != nullptr) {
      OutputDebugStringW(L"[NvRemixBridge-DX11] arg: ");
      OutputDebugStringW(argv[i]);
      OutputDebugStringW(L"\n");
    }
  }

  HMODULE d3d11 = Dx11BridgeLoadLocalFirst(L"d3d11.dll");
  if (d3d11 == nullptr) {
    Dx11BridgeLog(L"[NvRemixBridge-DX11] ERROR: could not load d3d11.dll from bridge/game directory or system fallback.");
    return 2;
  }

  FARPROC createDevice = GetProcAddress(d3d11, "D3D11CreateDevice");
  FARPROC createDeviceAndSwapChain = GetProcAddress(d3d11, "D3D11CreateDeviceAndSwapChain");

  if (createDevice == nullptr || createDeviceAndSwapChain == nullptr) {
    Dx11BridgeLog(L"[NvRemixBridge-DX11] ERROR: loaded d3d11.dll does not expose required D3D11 entry points.");
    return 3;
  }

  Dx11BridgeLog(L"[NvRemixBridge-DX11] d3d11.dll loaded and D3D11 entry points verified.");
  Dx11BridgeLog(L"[NvRemixBridge-DX11] No D3D9 server command processor is compiled in this source file.");

  wchar_t waitMsText[32] = {};
  DWORD waitLen = GetEnvironmentVariableW(L"DX11_BRIDGE_SERVER_WAIT_MS", waitMsText, 32);
  if (waitLen > 0 && waitLen < 32) {
    const DWORD waitMs = static_cast<DWORD>(_wtoi(waitMsText));
    if (waitMs > 0) {
      Sleep(waitMs);
    }
  }

  return 0;
}

int wmain(int argc, wchar_t** argv) {
  return Dx11BridgeRun(argc, argv);
}

int main(int argc, char**) {
  return Dx11BridgeRun(argc, nullptr);
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
  int argc = 0;
  wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  const int result = Dx11BridgeRun(argc, argv);
  if (argv != nullptr) {
    LocalFree(argv);
  }
  return result;
}
