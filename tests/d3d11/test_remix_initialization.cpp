#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <atomic>
#include <cstdio>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include "../../public/include/remix/remix_c.h"

namespace {
using Microsoft::WRL::ComPtr;
class Watchdog {
  HANDLE stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  HANDLE thread = nullptr;
  static DWORD WINAPI run(void* event) {
    if (WaitForSingleObject(static_cast<HANDLE>(event), 360000) == WAIT_TIMEOUT)
      TerminateProcess(GetCurrentProcess(), 124);
    return 0;
  }
public:
  Watchdog() { if (stop) thread = CreateThread(nullptr, 0, run, stop, 0, nullptr); }
  bool valid() const { return stop && thread; }
  ~Watchdog() {
    if (thread) { SetEvent(stop); WaitForSingleObject(thread, INFINITE); CloseHandle(thread); }
    if (stop) CloseHandle(stop);
  }
};
void require(bool value, const char* message) {
  if (!value) throw std::runtime_error(message);
}
void checked(HRESULT status, const char* operation) {
  if (FAILED(status)) {
    std::fprintf(stderr, "%s failed: HRESULT 0x%08lx\n", operation, status);
    throw std::runtime_error(operation);
  }
}
void checkedApi(remixapi_ErrorCode status, const char* operation) {
  if (status != REMIXAPI_ERROR_CODE_SUCCESS) {
    std::fprintf(stderr, "%s failed: Remix error %d\n", operation, int(status));
    throw std::runtime_error(operation);
  }
}
struct Device {
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> context;
  void reset() { context.Reset(); device.Reset(); }
};
Device create(PFN_D3D11_CREATE_DEVICE createDevice, IDXGIAdapter1* adapter) {
  Device result;
  const D3D_FEATURE_LEVEL requested = D3D_FEATURE_LEVEL_11_0;
  D3D_FEATURE_LEVEL actual = {};
  checked(createDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0,
    &requested, 1, D3D11_SDK_VERSION, result.device.GetAddressOf(), &actual,
    result.context.GetAddressOf()), "D3D11CreateDevice");
  require(actual >= requested, "Insufficient D3D11 feature level");
  return result;
}
struct Window {
  HWND handle = nullptr;
  Window() {
    WNDCLASSW windowClass = {};
    windowClass.lpfnWndProc = DefWindowProcW;
    windowClass.hInstance = GetModuleHandleW(nullptr);
    windowClass.lpszClassName = L"RemixDeviceLifetimeSmoke";
    require(RegisterClassW(&windowClass) != 0, "RegisterClass failed");
    handle = CreateWindowW(windowClass.lpszClassName, L"Remix device lifetime smoke",
      WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 192, 160,
      nullptr, nullptr, windowClass.hInstance, nullptr);
    require(handle != nullptr, "CreateWindow failed");
    ShowWindow(handle, SW_SHOWNOACTIVATE);
  }
  ~Window() { if (handle) DestroyWindow(handle); }
};
struct ApiSession {
  remixapi_Interface api = {};
  bool registered = false;
  ~ApiSession() { if (registered) api.Shutdown(); }
  void registerDevice(ID3D11Device* device) {
    checkedApi(api.dxvk_RegisterD3D11Device(device), "RegisterD3D11Device");
    registered = true;
  }
  void shutdown() {
    checkedApi(api.Shutdown(), "Shutdown");
    registered = false;
  }
};
void renderAndPresent(IDXGIFactory1* factory, Device& device, HWND window, const char* phase) {
  DXGI_SWAP_CHAIN_DESC description = {};
  description.BufferDesc.Width = 96;
  description.BufferDesc.Height = 64;
  description.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  description.SampleDesc.Count = 1;
  description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  description.BufferCount = 2;
  description.OutputWindow = window;
  description.Windowed = TRUE;
  description.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
  ComPtr<IDXGISwapChain> swapchain;
  checked(factory->CreateSwapChain(device.device.Get(), &description, swapchain.GetAddressOf()), "CreateSwapChain");
  ComPtr<ID3D11Texture2D> backbuffer;
  checked(swapchain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(backbuffer.GetAddressOf())), "GetBuffer");
  ComPtr<ID3D11RenderTargetView> view;
  checked(device.device->CreateRenderTargetView(backbuffer.Get(), nullptr, view.GetAddressOf()), "CreateRenderTargetView");
  D3D11_TEXTURE2D_DESC stagingDescription = {};
  backbuffer->GetDesc(&stagingDescription);
  stagingDescription.Usage = D3D11_USAGE_STAGING;
  stagingDescription.BindFlags = 0;
  stagingDescription.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  stagingDescription.MiscFlags = 0;
  ComPtr<ID3D11Texture2D> staging;
  checked(device.device->CreateTexture2D(&stagingDescription, nullptr, staging.GetAddressOf()), "CreateTexture2D staging");
  const float color[] = { 0.125f, 0.25f, 0.5f, 1.0f };
  const unsigned expected[] = { 32, 64, 128, 255 };
  const float partialColor[] = { 0.75f, 0.5f, 0.25f, 1.0f };
  const unsigned partialExpected[] = { 191, 128, 64, 255 };
  ComPtr<ID3D11DeviceContext1> context1;
  checked(device.context.As(&context1), "QueryInterface ID3D11DeviceContext1");
  for (unsigned frame = 0; frame < 3; ++frame) {
    MSG message = {};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
      TranslateMessage(&message);
      DispatchMessageW(&message);
    }
    device.context->ClearRenderTargetView(view.Get(), color);
    device.context->CopyResource(staging.Get(), backbuffer.Get());
    // The intervening copy completes the discard/full-clear pass. A partial
    // clear must subsequently load the attachment's existing image layout.
    const D3D11_RECT rectangle = { 8, 8, 32, 24 };
    context1->ClearView(view.Get(), partialColor, &rectangle, 1);
    device.context->CopyResource(staging.Get(), backbuffer.Get());
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    checked(device.context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "Map readback");
    bool correct = true;
    for (UINT y = 0; y < stagingDescription.Height; ++y) {
      const auto* row = static_cast<const unsigned char*>(mapped.pData) + y * mapped.RowPitch;
      for (UINT x = 0; x < stagingDescription.Width; ++x) {
        const unsigned* pixelExpected = x >= 8 && x < 32 && y >= 8 && y < 24 ? partialExpected : expected;
        for (UINT component = 0; component < 4; ++component) {
          const unsigned value = row[x * 4 + component];
          correct &= value + 1 >= pixelExpected[component] && value <= pixelExpected[component] + 1;
        }
      }
    }
    device.context->Unmap(staging.Get(), 0);
    require(correct, "Surviving/recreated device rendered incorrect pixels");
    checked(swapchain->Present(0, 0), "Present");
    checked(device.device->GetDeviceRemovedReason(), "Device health after Present");
  }
  device.context->ClearState();
  device.context->Flush();
  std::printf("%s: 3 GPU clears, exact-color readbacks, and presentations passed.\n", phase);
  std::fflush(stdout);
}

void testAttachmentViews(Device& device) {
  D3D11_TEXTURE3D_DESC volumeDescription = {};
  volumeDescription.Width = volumeDescription.Height = 16;
  volumeDescription.Depth = 4;
  volumeDescription.MipLevels = 1;
  volumeDescription.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  volumeDescription.Usage = D3D11_USAGE_DEFAULT;
  volumeDescription.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
  ComPtr<ID3D11Texture3D> volume;
  checked(device.device->CreateTexture3D(&volumeDescription, nullptr, volume.GetAddressOf()), "CreateTexture3D");
  D3D11_RENDER_TARGET_VIEW_DESC viewDescription = {};
  viewDescription.Format = volumeDescription.Format;
  viewDescription.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE3D;
  viewDescription.Texture3D.WSize = 4;
  ComPtr<ID3D11RenderTargetView> allSlices;
  checked(device.device->CreateRenderTargetView(volume.Get(), &viewDescription, allSlices.GetAddressOf()), "Volume RTV");
  viewDescription.Texture3D.FirstWSlice = 1;
  viewDescription.Texture3D.WSize = 2;
  ComPtr<ID3D11RenderTargetView> middleSlices;
  checked(device.device->CreateRenderTargetView(volume.Get(), &viewDescription, middleSlices.GetAddressOf()), "Volume slice RTV");
  volumeDescription.Usage = D3D11_USAGE_STAGING;
  volumeDescription.BindFlags = 0;
  volumeDescription.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  ComPtr<ID3D11Texture3D> volumeStaging;
  checked(device.device->CreateTexture3D(&volumeDescription, nullptr, volumeStaging.GetAddressOf()), "Volume staging");
  const float black[] = { 0, 0, 0, 1 };
  const float green[] = { 0, 1, 0, 1 };
  device.context->ClearRenderTargetView(allSlices.Get(), black);
  device.context->CopyResource(volumeStaging.Get(), volume.Get());
  device.context->ClearRenderTargetView(middleSlices.Get(), green);
  device.context->CopyResource(volumeStaging.Get(), volume.Get());
  D3D11_MAPPED_SUBRESOURCE mapped = {};
  checked(device.context->Map(volumeStaging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "Volume readback");
  bool correct = true;
  for (UINT z = 0; z < 4; ++z) {
    for (UINT y = 0; y < 16; ++y) {
      const auto* row = static_cast<const unsigned char*>(mapped.pData) + z * mapped.DepthPitch + y * mapped.RowPitch;
      for (UINT x = 0; x < 16; ++x)
        correct &= row[x * 4] == 0 && row[x * 4 + 1] == (z == 1 || z == 2 ? 255 : 0)
                && row[x * 4 + 2] == 0 && row[x * 4 + 3] == 255;
    }
  }
  device.context->Unmap(volumeStaging.Get(), 0);
  require(correct, "3D attachment slice preservation failed");

  D3D11_TEXTURE2D_DESC depthDescription = {};
  depthDescription.Width = depthDescription.Height = 16;
  depthDescription.MipLevels = depthDescription.ArraySize = 1;
  depthDescription.Format = DXGI_FORMAT_R32_TYPELESS;
  depthDescription.SampleDesc.Count = 1;
  depthDescription.Usage = D3D11_USAGE_DEFAULT;
  depthDescription.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
  ComPtr<ID3D11Texture2D> depth;
  checked(device.device->CreateTexture2D(&depthDescription, nullptr, depth.GetAddressOf()), "Depth texture");
  D3D11_DEPTH_STENCIL_VIEW_DESC depthViewDescription = {};
  depthViewDescription.Format = DXGI_FORMAT_D32_FLOAT;
  depthViewDescription.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
  ComPtr<ID3D11DepthStencilView> depthView;
  checked(device.device->CreateDepthStencilView(depth.Get(), &depthViewDescription, depthView.GetAddressOf()), "Depth view");
  depthDescription.Usage = D3D11_USAGE_STAGING;
  depthDescription.BindFlags = 0;
  depthDescription.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  ComPtr<ID3D11Texture2D> depthStaging;
  checked(device.device->CreateTexture2D(&depthDescription, nullptr, depthStaging.GetAddressOf()), "Depth staging");
  device.context->ClearDepthStencilView(depthView.Get(), D3D11_CLEAR_DEPTH, 0.25f, 0);
  device.context->CopyResource(depthStaging.Get(), depth.Get());
  checked(device.context->Map(depthStaging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "Depth readback");
  correct = true;
  for (UINT y = 0; y < 16; ++y) {
    const auto* row = reinterpret_cast<const float*>(static_cast<const unsigned char*>(mapped.pData) + y * mapped.RowPitch);
    for (UINT x = 0; x < 16; ++x) correct &= row[x] == 0.25f;
  }
  device.context->Unmap(depthStaging.Get(), 0);
  require(correct, "Sampled depth attachment clear/readback failed");
  std::puts("3D attachment slices and sampled depth attachment readbacks passed.");
}

void testLifetime(IDXGIFactory1* factory, IDXGIAdapter1* adapter,
                  PFN_D3D11_CREATE_DEVICE createDevice, PFN_remixapi_InitializeLibrary initialize,
                  BOOL (WINAPI* wasRayTraced)(BOOL)) {
  Window window;
  Device first = create(createDevice, adapter);
  Device second = create(createDevice, adapter);
  require(first.device.Get() != second.device.Get(), "Expected distinct D3D11 devices");
  ApiSession session;
  remixapi_InitializeLibraryInfo library = {};
  library.sType = REMIXAPI_STRUCT_TYPE_INITIALIZE_LIBRARY_INFO;
  library.version = REMIXAPI_VERSION_MAKE(REMIXAPI_VERSION_MAJOR, REMIXAPI_VERSION_MINOR, REMIXAPI_VERSION_PATCH);
  checkedApi(initialize(&library, &session.api), "InitializeLibrary");
  require(session.api.dxvk_RegisterD3D11Device && session.api.Shutdown &&
    session.api.SetCameraMediumMaterial && session.api.Present, "Incomplete Remix API table");
  session.registerDevice(first.device.Get());
  session.registerDevice(first.device.Get());
  require(session.api.dxvk_RegisterD3D11Device(second.device.Get()) != REMIXAPI_ERROR_CODE_SUCCESS,
    "Different device registration replaced an active device");
  remixapi_CameraMediumInfo medium = {};
  medium.sType = REMIXAPI_STRUCT_TYPE_CAMERA_MEDIUM_INFO;
  checkedApi(session.api.SetCameraMediumMaterial(&medium), "SetCameraMediumMaterial");
  require(!wasRayTraced(TRUE) && !wasRayTraced(FALSE), "Empty scene reported a ray-traced frame");
  // Both caller references go away; only the API may keep this device alive.
  first.reset();
  remixapi_PresentInfo present = {};
  present.sType = REMIXAPI_STRUCT_TYPE_PRESENT_INFO;
  present.hwndOverride = window.handle;
  checkedApi(session.api.Present(&present), "API-owned device Present");
  require(!wasRayTraced(TRUE), "Empty API-owned scene reported a ray-traced frame");
  session.shutdown();
  renderAndPresent(factory, second, window.handle, "Second device after first release");
  session.registerDevice(second.device.Get());
  session.registerDevice(second.device.Get());
  session.shutdown();
  require(second.device->GetFeatureLevel() >= D3D_FEATURE_LEVEL_11_0, "Shutdown invalidated caller device");
  renderAndPresent(factory, second, window.handle, "Second device after API shutdown");
  testAttachmentViews(second);
  second.reset();
  std::puts("Both original devices and their presentation resources fully released.");
  std::fflush(stdout);
  Device recreated = create(createDevice, adapter);
  session.registerDevice(recreated.device.Get());
  checkedApi(session.api.SetCameraMediumMaterial(&medium), "Recreated device medium control");
  session.shutdown();
  renderAndPresent(factory, recreated, window.handle, "Recreated device");
  recreated.reset();
  std::puts("Overlapping devices, API-owned references, full release, and recreation passed.");
}

void testParallelCreate(PFN_D3D11_CREATE_DEVICE createDevice, IDXGIAdapter1* adapter) {
  // Native D3D11 creation is independent of the single registered Remix API
  // device. Each thread uses only its own context and offscreen resources.
  std::atomic<bool> start = false;
  std::mutex errorMutex;
  std::string firstError;
  const auto worker = [&] {
    while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
    try {
      for (unsigned iteration = 0; iteration < 3; ++iteration) {
        Device device = create(createDevice, adapter);
        D3D11_TEXTURE2D_DESC description = {};
        description.Width = description.Height = 16;
        description.MipLevels = description.ArraySize = 1;
        description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        description.SampleDesc.Count = 1;
        description.Usage = D3D11_USAGE_DEFAULT;
        description.BindFlags = D3D11_BIND_RENDER_TARGET;
        ComPtr<ID3D11Texture2D> target;
        checked(device.device->CreateTexture2D(&description, nullptr, target.GetAddressOf()), "Parallel target");
        ComPtr<ID3D11RenderTargetView> view;
        checked(device.device->CreateRenderTargetView(target.Get(), nullptr, view.GetAddressOf()), "Parallel RTV");
        description.Usage = D3D11_USAGE_STAGING;
        description.BindFlags = 0;
        description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> staging;
        checked(device.device->CreateTexture2D(&description, nullptr, staging.GetAddressOf()), "Parallel staging");
        const float color[] = { 0.0f, 1.0f, 0.0f, 1.0f };
        device.context->ClearRenderTargetView(view.Get(), color);
        device.context->CopyResource(staging.Get(), target.Get());
        D3D11_MAPPED_SUBRESOURCE mapped = {};
        checked(device.context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "Parallel readback");
        bool correct = true;
        for (UINT y = 0; y < 16; ++y) {
          const auto* row = static_cast<const unsigned char*>(mapped.pData) + y * mapped.RowPitch;
          for (UINT x = 0; x < 16; ++x)
            correct &= row[x * 4] == 0 && row[x * 4 + 1] == 255 && row[x * 4 + 2] == 0 && row[x * 4 + 3] == 255;
        }
        device.context->Unmap(staging.Get(), 0);
        require(correct, "Parallel device clear/readback was incorrect");
        checked(device.device->GetDeviceRemovedReason(), "Parallel device health");
        device.context->ClearState();
        device.context->Flush();
      }
    } catch (const std::exception& error) {
      std::lock_guard<std::mutex> lock(errorMutex);
      if (firstError.empty()) firstError = error.what();
    }
  };
  std::vector<std::thread> workers;
  try {
    workers.emplace_back(worker);
    workers.emplace_back(worker);
  } catch (...) {
    start.store(true, std::memory_order_release);
    for (auto& thread : workers) thread.join();
    throw;
  }
  start.store(true, std::memory_order_release);
  for (auto& thread : workers) thread.join();
  require(firstError.empty(), firstError.c_str());
  std::puts("Parallel create/render/release: 2 threads x 3 iterations passed.");
}
}

// Loads the built sibling DLLs explicitly so an accidentally successful test
// against the system D3D11 runtime cannot hide a packaging/initialization bug.
int wmain(int argc, wchar_t** argv) {
  if (argc != 2) {
    std::fputs("Expected the absolute directory containing the built runtime DLLs.\n", stderr);
    return 2;
  }
  Watchdog watchdog;
  if (!watchdog.valid()) return 1;
  const std::wstring directory = argv[1];
  SetDllDirectoryW(directory.c_str());
  SetEnvironmentVariableW(L"DXVK_REMIX_PREWARM", L"0");
  SetEnvironmentVariableW(L"DXVK_REMIX_FORCE_CURRENT_PROCESS", L"1");
  SetEnvironmentVariableW(L"DXVK_ENABLE_RAYTRACING", L"1");
  HMODULE dxgi = LoadLibraryExW((directory + L"\\dxgi.dll").c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
  HMODULE d3d11 = LoadLibraryExW((directory + L"\\d3d11.dll").c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
  if (!dxgi || !d3d11) {
    std::fprintf(stderr, "Runtime DLL loading failed: Win32 error %lu\n", GetLastError());
    return 1;
  }
  auto createFactory = reinterpret_cast<HRESULT (WINAPI*)(REFIID, void**)>(GetProcAddress(dxgi, "CreateDXGIFactory1"));
  auto createDevice = reinterpret_cast<PFN_D3D11_CREATE_DEVICE>(GetProcAddress(d3d11, "D3D11CreateDevice"));
  auto initialize = reinterpret_cast<PFN_remixapi_InitializeLibrary>(GetProcAddress(d3d11, "remixapi_InitializeLibrary"));
  auto wasRayTraced = reinterpret_cast<BOOL (WINAPI*)(BOOL)>(GetProcAddress(d3d11, "remixapi_dxvk_WasLastPresentRayTraced"));
  if (!createFactory || !createDevice || !initialize || !wasRayTraced) {
    std::fputs("Built DLLs are missing required DXGI/D3D11/Remix exports.\n", stderr);
    return 1;
  }
  IDXGIFactory1* factory = nullptr;
  HRESULT hr = createFactory(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory));
  if (FAILED(hr)) {
    std::fprintf(stderr, "DXGI factory creation failed: 0x%08lx\n", hr);
    return 1;
  }
  IDXGIAdapter1* adapter = nullptr;
  hr = factory->EnumAdapters1(0, &adapter);
  if (FAILED(hr)) {
    factory->Release();
    std::fprintf(stderr, "Adapter enumeration failed: 0x%08lx\n", hr);
    return 1;
  }
  DXGI_ADAPTER_DESC1 description = {};
  adapter->GetDesc1(&description);
  std::wprintf(L"Testing adapter: %ls\n", description.Description);
  bool passed = false;
  try {
    testLifetime(factory, adapter, createDevice, initialize, wasRayTraced);
    testParallelCreate(createDevice, adapter);
    passed = true;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "Remix initialization/lifetime test failed: %s\n", error.what());
  }
  adapter->Release();
  factory->Release();
  if (!passed) return 1;
  std::puts("Built DXGI and D3D11 DLLs initialized successfully.");
  // Normal process unload deliberately exercises the runtime's module cleanup.
  return 0;
}
