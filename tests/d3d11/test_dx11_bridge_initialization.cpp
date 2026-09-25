#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>

// Run as x86 against a complete staged x86 package. Successful native D3D11
// passthrough is insufficient: fresh client log records must confirm both the
// Remix device startup and two completed presentations in the x64 server.
static DWORD WINAPI EnforceTimeout(void* completed) {
  if (WaitForSingleObject(static_cast<HANDLE>(completed), 90000) == WAIT_TIMEOUT) {
    std::fputs("Bridge initialization exceeded 90 seconds.\n", stderr);
    TerminateProcess(GetCurrentProcess(), 124);
  }
  return 0;
}

int wmain(int argc, wchar_t** argv) {
  static_assert(sizeof(void*) == 4, "The bridge initialization test must be compiled for x86.");
  if (argc != 2) {
    std::fputs("Expected the absolute staged x86 package directory.\n", stderr);
    return 2;
  }
  const HANDLE completed = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  const HANDLE watchdog = completed ? CreateThread(nullptr, 0, EnforceTimeout, completed, 0, nullptr) : nullptr;
  if (!watchdog) return 2;
  const std::wstring directory = argv[1];
  const std::wstring logPath = directory + L"\\dx11_bridge_client.log";
  std::ifstream before(logPath, std::ios::binary | std::ios::ate);
  const std::streamoff oldSize = before ? std::streamoff(before.tellg()) : 0;
  before.close();
  SetCurrentDirectoryW(directory.c_str());
  SetEnvironmentVariableW(L"DXVK_REMIX_PREWARM", L"0");
  SetEnvironmentVariableW(L"DXVK_ENABLE_RAYTRACING", L"1");
  HMODULE proxy = LoadLibraryExW((directory + L"\\d3d11.dll").c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
  HMODULE dxgi = LoadLibraryExW((directory + L"\\dxgi.dll").c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
  auto create = proxy ? reinterpret_cast<PFN_D3D11_CREATE_DEVICE_AND_SWAP_CHAIN>(
    GetProcAddress(proxy, "D3D11CreateDeviceAndSwapChain")) : nullptr;
  auto createFactory = dxgi ? reinterpret_cast<HRESULT (WINAPI*)(REFIID, void**)>(
    GetProcAddress(dxgi, "CreateDXGIFactory1")) : nullptr;
  if (!create || !createFactory) {
    std::fprintf(stderr, "Loading the bridge client failed: Win32 error %lu\n", GetLastError());
    return 1;
  }
  WNDCLASSW windowClass = {};
  windowClass.lpfnWndProc = DefWindowProcW;
  windowClass.hInstance = GetModuleHandleW(nullptr);
  windowClass.lpszClassName = L"RemixBridgeInitializationTest";
  if (!RegisterClassW(&windowClass)) return 1;
  HWND window = CreateWindowW(windowClass.lpszClassName, L"Remix bridge initialization test",
    WS_OVERLAPPEDWINDOW, 0, 0, 320, 240, nullptr, nullptr, windowClass.hInstance, nullptr);
  if (!window) return 1;
  DXGI_SWAP_CHAIN_DESC desc = {};
  desc.BufferDesc.Width = 320;
  desc.BufferDesc.Height = 240;
  desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  desc.SampleDesc.Count = 1;
  desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  desc.BufferCount = 1;
  desc.OutputWindow = window;
  desc.Windowed = TRUE;
  desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
  const D3D_FEATURE_LEVEL requested = D3D_FEATURE_LEVEL_11_0;
  ID3D11Device* device = nullptr;
  ID3D11DeviceContext* context = nullptr;
  IDXGISwapChain* swapChain = nullptr;
  HRESULT hr = create(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &requested, 1,
    D3D11_SDK_VERSION, &desc, &swapChain, &device, nullptr, &context);
  if (FAILED(hr)) {
    std::fprintf(stderr, "Bridge device/swapchain creation failed: 0x%08lx\n", hr);
    return 1;
  }
  for (unsigned frame = 0; frame < 2; ++frame) {
    if (frame == 1) {
      swapChain->Release();
      swapChain = nullptr;
      // Create the replacement before destroying the old window so Windows
      // cannot reuse the same HWND and hide a stale destination bug.
      HWND replacement = CreateWindowW(windowClass.lpszClassName, L"Remix replacement window",
        WS_OVERLAPPEDWINDOW, 0, 0, 400, 300, nullptr, nullptr, windowClass.hInstance, nullptr);
      if (!replacement) return 1;
      DestroyWindow(window);
      window = replacement;
      desc.OutputWindow = window;
      IDXGIFactory* factory = nullptr;
      hr = createFactory(__uuidof(IDXGIFactory), reinterpret_cast<void**>(&factory));
      if (SUCCEEDED(hr)) hr = factory->CreateSwapChain(device, &desc, &swapChain);
      if (factory) factory->Release();
      if (FAILED(hr)) break;
    }
    ID3D11Texture2D* backBuffer = nullptr;
    ID3D11RenderTargetView* target = nullptr;
    hr = swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&backBuffer));
    if (SUCCEEDED(hr)) hr = device->CreateRenderTargetView(backBuffer, nullptr, &target);
    if (backBuffer) backBuffer->Release();
    if (FAILED(hr)) break;
    const float color[4] = { 0.125f * frame, 0.0f, 0.0f, 1.0f };
    context->ClearRenderTargetView(target, color);
    target->Release();
    hr = swapChain->Present(0, 0);
    if (FAILED(hr)) break;
  }
  if (swapChain) swapChain->Release();
  context->Release();
  device->Release();
  DestroyWindow(window);
  SetEvent(completed);
  WaitForSingleObject(watchdog, INFINITE);
  CloseHandle(watchdog);
  CloseHandle(completed);
  if (FAILED(hr)) {
    std::fprintf(stderr, "Bridge presentation failed: 0x%08lx\n", hr);
    return 1;
  }
  std::ifstream after(logPath, std::ios::binary);
  after.seekg(oldSize);
  const std::string log((std::istreambuf_iterator<char>(after)), std::istreambuf_iterator<char>());
  const std::string marker = "Remix frame presented and acknowledged by the x64 server.";
  const size_t first = log.find(marker);
  if (log.find("Remix runtime started (game hwnd=") == std::string::npos
      || first == std::string::npos || log.find(marker, first + marker.size()) == std::string::npos) {
    std::fputs("The x86 game fell back to native rendering; the x64 Remix startup/present handshake did not complete.\n", stderr);
    return 1;
  }
  std::puts("The x86 client initialized the x64 Remix runtime and completed two acknowledged frames.");
  return 0;
}
