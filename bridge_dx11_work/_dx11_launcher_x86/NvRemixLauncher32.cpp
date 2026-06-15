#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <stdio.h>
#include <wchar.h>

static void dirnameOf(const wchar_t* in, wchar_t* out, DWORD cap) {
  out[0] = 0;
  if (!in || !in[0]) return;
  lstrcpynW(out, in, cap);
  wchar_t* slash = wcsrchr(out, L'\\');
  if (slash) slash[1] = 0;
}

static void appendLog(const wchar_t* root, const wchar_t* msg) {
  wchar_t path[MAX_PATH] = {};
  lstrcpynW(path, root, MAX_PATH);
  lstrcatW(path, L"NvRemixLauncher32.log");
  HANDLE h = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return;
  SYSTEMTIME st; GetLocalTime(&st);
  char line[4096] = {};
  char m[3072] = {};
  WideCharToMultiByte(CP_UTF8, 0, msg ? msg : L"", -1, m, sizeof(m), nullptr, nullptr);
  int n = sprintf_s(line, sizeof(line), "[%04u-%02u-%02u %02u:%02u:%02u.%03u] %s\r\n",
    st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, m);
  DWORD wrote = 0;
  if (n > 0) WriteFile(h, line, (DWORD)n, &wrote, nullptr);
  CloseHandle(h);
}

static bool existsFile(const wchar_t* p) {
  DWORD a = GetFileAttributesW(p);
  return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static bool isExcludedExe(const wchar_t* name) {
  return _wcsicmp(name, L"NvRemixLauncher32.exe") == 0 ||
         _wcsicmp(name, L"NvRemixBridge.exe") == 0 ||
         _wcsicmp(name, L"dxvk-remix.exe") == 0 ||
         _wcsicmp(name, L"setup.exe") == 0 ||
         _wcsicmp(name, L"unins000.exe") == 0;
}

static bool autoFindGameExe(const wchar_t* root, wchar_t* out, DWORD cap) {
  wchar_t pattern[MAX_PATH] = {};
  lstrcpynW(pattern, root, MAX_PATH);
  lstrcatW(pattern, L"*.exe");
  WIN32_FIND_DATAW fd = {};
  HANDLE h = FindFirstFileW(pattern, &fd);
  if (h == INVALID_HANDLE_VALUE) return false;
  do {
    if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !isExcludedExe(fd.cFileName)) {
      lstrcpynW(out, root, cap);
      lstrcatW(out, fd.cFileName);
      FindClose(h);
      return true;
    }
  } while (FindNextFileW(h, &fd));
  FindClose(h);
  return false;
}

static void quoteAppend(wchar_t* dst, DWORD cap, const wchar_t* s) {
  lstrcatW(dst, L"\"");
  lstrcatW(dst, s);
  lstrcatW(dst, L"\"");
}

static bool pathLooksRooted(const wchar_t* p) {
  return (p && ((wcslen(p) > 2 && p[1] == L':') || (p[0] == L'\\' && p[1] == L'\\')));
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
  wchar_t self[MAX_PATH] = {};
  GetModuleFileNameW(nullptr, self, MAX_PATH);
  wchar_t root[MAX_PATH] = {};
  dirnameOf(self, root, MAX_PATH);

  appendLog(root, L"NvRemixLauncher32 DX11 client started.");

  wchar_t trex[MAX_PATH] = {};
  lstrcpynW(trex, root, MAX_PATH);
  lstrcatW(trex, L".trex\\");

  wchar_t d3d11[MAX_PATH] = {}, dxgi[MAX_PATH] = {}, bridge[MAX_PATH] = {};
  lstrcpynW(d3d11, root, MAX_PATH); lstrcatW(d3d11, L"d3d11.dll");
  lstrcpynW(dxgi,  root, MAX_PATH); lstrcatW(dxgi,  L"dxgi.dll");
  lstrcpynW(bridge, trex, MAX_PATH); lstrcatW(bridge, L"NvRemixBridge.exe");

  if (!existsFile(d3d11) || !existsFile(dxgi)) {
    appendLog(root, L"ERROR: root d3d11.dll/dxgi.dll missing beside launcher.");
    MessageBoxW(nullptr, L"DX11 Remix root d3d11.dll/dxgi.dll is missing beside NvRemixLauncher32.exe.", L"DX11 Remix Launcher", MB_ICONERROR);
    return 2;
  }
  if (!existsFile(bridge)) {
    appendLog(root, L"ERROR: .trex\\NvRemixBridge.exe missing.");
    MessageBoxW(nullptr, L".trex\\NvRemixBridge.exe is missing beside the DX11 Remix package.", L"DX11 Remix Launcher", MB_ICONERROR);
    return 3;
  }

  int argc = 0;
  LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);

  wchar_t target[MAX_PATH] = {};
  wchar_t extra[4096] = {};
  if (argv && argc >= 2) {
    if (pathLooksRooted(argv[1])) {
      lstrcpynW(target, argv[1], MAX_PATH);
    } else {
      lstrcpynW(target, root, MAX_PATH);
      lstrcatW(target, argv[1]);
    }
    for (int i = 2; i < argc; i++) {
      if (extra[0]) lstrcatW(extra, L" ");
      quoteAppend(extra, 4096, argv[i]);
    }
  } else {
    if (!autoFindGameExe(root, target, MAX_PATH)) {
      appendLog(root, L"ERROR: no game exe argument and no auto-detected exe in package root.");
      MessageBoxW(nullptr, L"Put NvRemixLauncher32.exe beside the 32-bit DX11 game exe, or run:\nNvRemixLauncher32.exe Game.exe", L"DX11 Remix Launcher", MB_ICONERROR);
      if (argv) LocalFree(argv);
      return 4;
    }
  }
  if (argv) LocalFree(argv);

  if (!existsFile(target)) {
    appendLog(root, L"ERROR: requested game exe does not exist.");
    MessageBoxW(nullptr, L"Requested game executable was not found.", L"DX11 Remix Launcher", MB_ICONERROR);
    return 5;
  }

  wchar_t oldPath[32767] = {};
  GetEnvironmentVariableW(L"PATH", oldPath, 32767);
  wchar_t newPath[32767] = {};
  lstrcpynW(newPath, root, 32767);
  lstrcatW(newPath, L";");
  lstrcatW(newPath, trex);
  lstrcatW(newPath, L";");
  lstrcatW(newPath, oldPath);
  SetEnvironmentVariableW(L"PATH", newPath);
  SetEnvironmentVariableW(L"DXVK_REMIX_DX11_BRIDGE", L"1");
  SetEnvironmentVariableW(L"DXVK_REMIX_LAUNCHED_BY_NVREMIXLAUNCHER32", L"1");

  wchar_t cmd[8192] = {};
  quoteAppend(cmd, 8192, target);
  if (extra[0]) { lstrcatW(cmd, L" "); lstrcatW(cmd, extra); }

  wchar_t logMsg[8192] = {};
  swprintf_s(logMsg, L"Launching game: %s ; cwd=%s", cmd, root);
  appendLog(root, logMsg);

  STARTUPINFOW si = {};
  PROCESS_INFORMATION pi = {};
  si.cb = sizeof(si);
  BOOL ok = CreateProcessW(nullptr, cmd, nullptr, nullptr, TRUE, 0, nullptr, root, &si, &pi);
  if (!ok) {
    wchar_t e[512] = {};
    swprintf_s(e, L"ERROR: CreateProcessW failed. GetLastError=%lu", GetLastError());
    appendLog(root, e);
    MessageBoxW(nullptr, e, L"DX11 Remix Launcher", MB_ICONERROR);
    return 6;
  }
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  appendLog(root, L"Game process started successfully.");
  return 0;
}
