// Exercises a proxy factory with real Windows D3D12 and D3D11 devices.
// This verifies presentation forwarding; it does not exercise Remix RTX.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <cstdio>
#include <stdexcept>
#include <string>

using Microsoft::WRL::ComPtr;

static void check(HRESULT result, const char* operation) {
  if (FAILED(result)) {
    std::fprintf(stderr, "%s failed: 0x%08lx\n", operation, static_cast<unsigned long>(result));
    throw std::runtime_error(operation);
  }
}

static void require(bool condition, const char* operation) {
  if (!condition)
    throw std::runtime_error(operation);
}

struct Module {
  HMODULE handle = nullptr;
  explicit Module(const std::wstring& path) : handle(LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH)) {
    require(handle != nullptr, "LoadLibraryExW");
  }
  ~Module() {
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(handle, path, MAX_PATH);
    std::fwprintf(stderr, L"Unloading module: %s\n", path);
    FreeLibrary(handle);
    std::fwprintf(stderr, L"Unloaded module: %s\n", path);
  }
  template<typename T> T proc(const char* name) const {
    auto result = reinterpret_cast<T>(GetProcAddress(handle, name));
    require(result != nullptr, name);
    return result;
  }
};

struct Window {
  HWND handle = nullptr;
  Window() {
    WNDCLASSW definition = {};
    definition.lpfnWndProc = DefWindowProcW;
    definition.hInstance = GetModuleHandleW(nullptr);
    definition.lpszClassName = L"RemixNativePresentationValidation";
    require(RegisterClassW(&definition) != 0, "RegisterClassW");
    handle = CreateWindowW(definition.lpszClassName, L"Native DXGI forwarding validation", WS_OVERLAPPEDWINDOW,
      CW_USEDEFAULT, CW_USEDEFAULT, 384, 288, nullptr, nullptr, definition.hInstance, nullptr);
    require(handle != nullptr, "CreateWindowW");
  }
  ~Window() { DestroyWindow(handle); }
};

static std::wstring systemPath(const wchar_t* dll) {
  wchar_t directory[MAX_PATH] = {};
  const UINT size = GetSystemDirectoryW(directory, MAX_PATH);
  require(size && size < MAX_PATH, "GetSystemDirectoryW");
  return std::wstring(directory) + L"\\" + dll;
}

static DXGI_SWAP_CHAIN_DESC1 swapchainDescription() {
  DXGI_SWAP_CHAIN_DESC1 result = {};
  result.Width = 321;
  result.Height = 241;
  result.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  result.SampleDesc.Count = 1;
  result.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  result.BufferCount = 2;
  result.Scaling = DXGI_SCALING_STRETCH;
  result.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
  result.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
  return result;
}

static void renderD3D12(ID3D12Device* device, ID3D12CommandQueue* queue, IDXGISwapChain1* swapchain) {
  ComPtr<IDXGISwapChain3> chain;
  check(swapchain->QueryInterface(IID_PPV_ARGS(&chain)), "QueryInterface SwapChain3");
  ComPtr<ID3D12Resource> buffer;
  check(chain->GetBuffer(chain->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&buffer)), "GetBuffer");
  D3D12_DESCRIPTOR_HEAP_DESC heapDescription = {};
  heapDescription.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
  heapDescription.NumDescriptors = 1;
  ComPtr<ID3D12DescriptorHeap> heap;
  check(device->CreateDescriptorHeap(&heapDescription, IID_PPV_ARGS(&heap)), "CreateDescriptorHeap");
  const auto rtv = heap->GetCPUDescriptorHandleForHeapStart();
  device->CreateRenderTargetView(buffer.Get(), nullptr, rtv);
  ComPtr<ID3D12CommandAllocator> allocator;
  check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)), "CreateCommandAllocator");
  ComPtr<ID3D12GraphicsCommandList> commands;
  check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&commands)), "CreateCommandList");
  D3D12_RESOURCE_BARRIER barrier = {};
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barrier.Transition.pResource = buffer.Get();
  barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
  barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
  commands->ResourceBarrier(1, &barrier);
  const float color[4] = { 0.15f, 0.35f, 0.7f, 1.0f };
  commands->ClearRenderTargetView(rtv, color, 0, nullptr);
  barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
  barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
  commands->ResourceBarrier(1, &barrier);
  check(commands->Close(), "Close command list");
  ID3D12CommandList* submission = commands.Get();
  queue->ExecuteCommandLists(1, &submission);
  check(chain->Present(0, 0), "D3D12 Present");
  ComPtr<ID3D12Fence> fence;
  check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "CreateFence");
  check(queue->Signal(fence.Get(), 1), "Signal fence");
  HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  require(event != nullptr, "CreateEventW");
  const HRESULT fenceResult = fence->SetEventOnCompletion(1, event);
  const DWORD waitResult = SUCCEEDED(fenceResult) ? WaitForSingleObject(event, 30000) : WAIT_FAILED;
  CloseHandle(event);
  check(fenceResult, "SetEventOnCompletion");
  require(waitResult == WAIT_OBJECT_0, "D3D12 fence timed out");
  check(device->GetDeviceRemovedReason(), "D3D12 device removed");
}

static void validateParent(IDXGISwapChain1* swapchain, HWND expectedWindow, UINT expectedFlags, UINT associationFlags = DXGI_MWA_NO_ALT_ENTER) {
  ComPtr<IDXGIFactory3> parent;
  check(swapchain->GetParent(IID_PPV_ARGS(&parent)), "Swapchain GetParent");
  require(parent->GetCreationFlags() == expectedFlags, "Native creation flags lost");
  HWND associated = nullptr;
  check(parent->GetWindowAssociation(&associated), "Native GetWindowAssociation");
  // Windows can report no monitored window when message monitoring is disabled.
  // Compare the forwarded state to the same operation called on its real parent,
  // rather than assuming GetWindowAssociation echoes the requested HWND.
  check(parent->MakeWindowAssociation(expectedWindow, associationFlags), "Control native MakeWindowAssociation");
  HWND controlAssociation = nullptr;
  check(parent->GetWindowAssociation(&controlAssociation), "Control native GetWindowAssociation");
  if (associated != controlAssociation)
    std::fprintf(stderr, "Window association before=%p, native control=%p, request=%p flags=%u\n",
      static_cast<void*>(associated), static_cast<void*>(controlAssociation), static_cast<void*>(expectedWindow), associationFlags);
  require(associated == controlAssociation, "Native window association differs from Windows control");
}

int wmain(int argc, wchar_t** argv) {
  try {
    require(argc == 2, "Usage: test-dxgi-native-presentation <runtime-directory>");
    SetEnvironmentVariableW(L"DXVK_REMIX_DXGI_PASSTHROUGH", nullptr);
    SetEnvironmentVariableW(L"DXVK_REMIX_RUNTIME_DIR", argv[1]);
    const std::wstring runtime = argv[1];
    Module proxy(runtime + L"\\dxgi.dll");
    Module systemDxgi(systemPath(L"dxgi.dll"));
    Module d3d12(systemPath(L"d3d12.dll"));
    const auto createFactory = proxy.proc<HRESULT (WINAPI*)(UINT, REFIID, void**)>("CreateDXGIFactory2");
    const auto createNativeFactory = systemDxgi.proc<HRESULT (WINAPI*)(UINT, REFIID, void**)>("CreateDXGIFactory2");
    const auto createDevice = d3d12.proc<PFN_D3D12_CREATE_DEVICE>("D3D12CreateDevice");
    ComPtr<IDXGIFactory4> factory;
    check(createFactory(0, IID_PPV_ARGS(&factory)), "Proxy CreateDXGIFactory2");
    ComPtr<IDXGIFactory4> nativeFactory;
    check(createNativeFactory(0, IID_PPV_ARGS(&nativeFactory)), "System CreateDXGIFactory2");
    ComPtr<IDXGIAdapter1> adapter;
    check(factory->EnumAdapters1(0, &adapter), "Proxy EnumAdapters1");
    DXGI_ADAPTER_DESC1 adapterDescription = {};
    check(adapter->GetDesc1(&adapterDescription), "Proxy adapter GetDesc1");
    ComPtr<IDXGIAdapter1> nativeAdapter;
    check(nativeFactory->EnumAdapterByLuid(adapterDescription.AdapterLuid, IID_PPV_ARGS(&nativeAdapter)), "Native adapter LUID match");
    ComPtr<ID3D12Device> device;
    check(createDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)), "Native D3D12CreateDevice with proxy adapter");
    D3D12_COMMAND_QUEUE_DESC queueDescription = {};
    queueDescription.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> queue;
    check(device->CreateCommandQueue(&queueDescription, IID_PPV_ARGS(&queue)), "CreateCommandQueue");
    Window window;
    check(factory->MakeWindowAssociation(window.handle, DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES), "Proxy MakeWindowAssociation before native creation");
    auto description = swapchainDescription();
    ComPtr<IDXGISwapChain1> chain;
    check(factory->CreateSwapChainForHwnd(queue.Get(), window.handle, &description, nullptr, nullptr, &chain), "Proxy D3D12 CreateSwapChainForHwnd");
    validateParent(chain.Get(), window.handle, 0, DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);
    for (unsigned frame = 0; frame < 4; ++frame)
      renderD3D12(device.Get(), queue.Get(), chain.Get());
    check(chain->ResizeBuffers(2, 227, 173, description.Format, 0), "Native ResizeBuffers");
    renderD3D12(device.Get(), queue.Get(), chain.Get());
    check(factory->MakeWindowAssociation(nullptr, 0), "Clear native association");
    validateParent(chain.Get(), nullptr, 0, 0);
    check(factory->MakeWindowAssociation(window.handle, DXGI_MWA_NO_ALT_ENTER), "Update native association");
    validateParent(chain.Get(), window.handle, 0);
    chain.Reset();
    std::puts("PASS: native D3D12 queue + proxy factory HWND rendering, resize, and window association");

    ComPtr<IDXGIOutput> output;
    check(adapter->EnumOutputs(0, &output), "Proxy EnumOutputs");
    check(factory->CreateSwapChainForHwnd(queue.Get(), window.handle, &description, nullptr, output.Get(), &chain), "Proxy restricted-output D3D12 swapchain");
    ComPtr<IDXGIOutput> restrictedOutput;
    check(chain->GetRestrictToOutput(&restrictedOutput), "Native GetRestrictToOutput");
    require(restrictedOutput != nullptr, "Native output restriction lost");
    DXGI_OUTPUT_DESC requestedOutput = {};
    DXGI_OUTPUT_DESC actualOutput = {};
    check(output->GetDesc(&requestedOutput), "Proxy output description");
    check(restrictedOutput->GetDesc(&actualOutput), "Native output description");
    require(requestedOutput.Monitor == actualOutput.Monitor, "Native output monitor mismatch");
    renderD3D12(device.Get(), queue.Get(), chain.Get());
    chain.Reset();
    std::puts("PASS: proxy restrict-to-output maps to native output");

    description.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    check(factory->CreateSwapChainForComposition(queue.Get(), &description, nullptr, &chain), "Proxy native composition swapchain");
    renderD3D12(device.Get(), queue.Get(), chain.Get());
    chain.Reset();
    std::puts("PASS: native D3D12 composition swapchain");

    ComPtr<IDXGIFactory4> debugSystemFactory;
    const HRESULT debugSupport = createNativeFactory(DXGI_CREATE_FACTORY_DEBUG, IID_PPV_ARGS(&debugSystemFactory));
    if (SUCCEEDED(debugSupport)) {
      ComPtr<IDXGIFactory4> debugProxyFactory;
      check(createFactory(DXGI_CREATE_FACTORY_DEBUG, IID_PPV_ARGS(&debugProxyFactory)), "Proxy debug factory");
      check(debugProxyFactory->MakeWindowAssociation(window.handle, DXGI_MWA_NO_ALT_ENTER), "Debug factory window association");
      description = swapchainDescription();
      check(debugProxyFactory->CreateSwapChainForHwnd(queue.Get(), window.handle, &description, nullptr, nullptr, &chain), "Proxy debug factory native swapchain");
      validateParent(chain.Get(), window.handle, DXGI_CREATE_FACTORY_DEBUG);
      renderD3D12(device.Get(), queue.Get(), chain.Get());
      chain.Reset();
      std::puts("PASS: nonzero native factory creation flags preserved");
    } else {
      require(debugSupport == DXGI_ERROR_SDK_COMPONENT_MISSING, "Unexpected native debug factory error");
      std::puts("SKIP: debug factory flag check requires the Windows graphics debug component");
    }

    Module d3d11(systemPath(L"d3d11.dll"));
    const auto createD3D11 = d3d11.proc<PFN_D3D11_CREATE_DEVICE>("D3D11CreateDevice");
    ComPtr<ID3D11Device> device11;
    ComPtr<ID3D11DeviceContext> context11;
    check(createD3D11(nativeAdapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0,
      D3D11_SDK_VERSION, &device11, nullptr, &context11), "Native D3D11CreateDevice with proxy DLL loaded");
    description = swapchainDescription();
    check(factory->CreateSwapChainForHwnd(device11.Get(), window.handle, &description, nullptr, output.Get(), &chain), "Proxy native D3D11 swapchain");
    ComPtr<ID3D11Texture2D> buffer11;
    check(chain->GetBuffer(0, IID_PPV_ARGS(&buffer11)), "D3D11 GetBuffer");
    ComPtr<ID3D11RenderTargetView> rtv11;
    check(device11->CreateRenderTargetView(buffer11.Get(), nullptr, &rtv11), "D3D11 CreateRenderTargetView");
    const float color11[4] = { 0.2f, 0.6f, 0.3f, 1.0f };
    context11->ClearRenderTargetView(rtv11.Get(), color11);
    check(chain->Present(0, 0), "D3D11 Present");
    context11->Flush();
    check(device11->GetDeviceRemovedReason(), "D3D11 device removed");
    D3D11_QUERY_DESC queryDescription = { D3D11_QUERY_EVENT, 0 };
    ComPtr<ID3D11Query> completion;
    check(device11->CreateQuery(&queryDescription, &completion), "D3D11 CreateQuery");
    context11->End(completion.Get());
    context11->Flush();
    const ULONGLONG deadline = GetTickCount64() + 30000;
    HRESULT queryResult;
    while ((queryResult = context11->GetData(completion.Get(), nullptr, 0, 0)) == S_FALSE && GetTickCount64() < deadline)
      Sleep(1);
    check(queryResult, "D3D11 event query");
    require(queryResult == S_OK, "D3D11 GPU completion timed out");
    context11->ClearState();
    chain.Reset();
    std::puts("PASS: native D3D11 device + proxy factory rendering");
    std::puts("Native DXGI presentation forwarding checks passed; no Remix DX12 ray tracing is implied.");
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "FAIL: %s\n", error.what());
    return 1;
  }
}
