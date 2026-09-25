#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <array>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>
#include "../../public/include/remix/remix_c.h"

namespace {
// Odd extents exercise the partial workgroups and gradient strata at the edges.
constexpr UINT kWidth = 321;
constexpr UINT kHeight = 241;
constexpr DWORD kWarmupTimeoutMs = 120000;
constexpr DWORD kSteadyTimeoutMs = 30000;
constexpr unsigned kRequiredFrames = 32;

void require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

void checked(remixapi_ErrorCode status, const char* operation) {
  if (status != REMIXAPI_ERROR_CODE_SUCCESS) {
    std::fprintf(stderr, "%s returned Remix error %d\n", operation, int(status));
    throw std::runtime_error(operation);
  }
}

void step(const char* operation) {
  std::puts(operation);
  std::fflush(stdout);
}

// The process limit also covers a driver/API call that never returns. The
// ordinary phase deadline alone cannot bound synchronous pipeline compilation.
class Watchdog {
  HANDLE stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  HANDLE thread = nullptr;
  static DWORD WINAPI run(void* self) {
    const auto* watchdog = static_cast<Watchdog*>(self);
    if (WaitForSingleObject(watchdog->stop, 720000) == WAIT_TIMEOUT) {
      std::fputs("Rendering smoke exceeded its 720-second process limit.\n", stderr);
      std::fflush(stderr);
      TerminateProcess(GetCurrentProcess(), 124);
    }
    return 0;
  }
public:
  Watchdog() {
    require(stop != nullptr, "CreateEvent watchdog failed");
    thread = CreateThread(nullptr, 0, run, this, 0, nullptr);
    require(thread != nullptr, "CreateThread watchdog failed");
  }
  ~Watchdog() {
    SetEvent(stop);
    WaitForSingleObject(thread, INFINITE);
    CloseHandle(thread);
    CloseHandle(stop);
  }
};

struct Runtime {
  remixapi_Interface api = {};
  ID3D11Device* device = nullptr;
  ID3D11DeviceContext* context = nullptr;
  ID3D11Texture2D* output = nullptr;
  ID3D11Texture2D* staging = nullptr;
  remixapi_MaterialHandle material = nullptr;
  remixapi_MeshHandle mesh = nullptr;
  remixapi_LightHandle light = nullptr;
  bool registered = false;
  ~Runtime() {
    if (registered) {
      if (light) api.DestroyLight(light);
      if (mesh) api.DestroyMesh(mesh);
      if (material) api.DestroyMaterial(material);
      api.Shutdown();
    }
    if (staging) staging->Release();
    if (output) output->Release();
    if (context) context->Release();
    if (device) device->Release();
  }
};

LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
  if (message == WM_CLOSE) {
    PostQuitMessage(0);
    return 0;
  }
  return DefWindowProcW(window, message, wparam, lparam);
}

void pumpMessages() {
  MSG message = {};
  while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
    require(message.message != WM_QUIT, "Rendering smoke window was closed");
    TranslateMessage(&message);
    DispatchMessageW(&message);
  }
}

void makeScene(Runtime& runtime) {
  remixapi_MaterialInfoOpaqueEXT opaque = {};
  opaque.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_OPAQUE_EXT;
  opaque.albedoConstant = { 0.7f, 0.55f, 0.35f };
  opaque.opacityConstant = 1.0f;
  opaque.roughnessConstant = 1.0f;
  opaque.alphaTestType = 7; // Always, matching the public material contract.
  remixapi_MaterialInfo material = {};
  material.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO;
  material.pNext = &opaque;
  material.hash = 0x534D4F4B4501ull;
  material.spriteSheetRow = material.spriteSheetCol = 1;
  checked(runtime.api.CreateMaterial(&material, &runtime.material), "CreateMaterial");

  // An open room provides diffuse secondary intersections as well as a
  // directly lit primary surface, exercising SHARC updates and queries.
  std::vector<remixapi_HardcodedVertex> vertices;
  std::vector<uint32_t> indices;
  const auto quad = [&](std::array<float, 3> a, std::array<float, 3> b,
                        std::array<float, 3> c, std::array<float, 3> d,
                        std::array<float, 3> normal) {
    const uint32_t first = static_cast<uint32_t>(vertices.size());
    for (const auto& position : { a, b, c, d }) {
      remixapi_HardcodedVertex vertex = {};
      for (unsigned component = 0; component < 3; ++component) {
        vertex.position[component] = position[component];
        vertex.normal[component] = normal[component];
      }
      vertex.color = 0xffffffffu;
      vertices.push_back(vertex);
    }
    for (uint32_t index : { 0u, 1u, 2u, 0u, 2u, 3u }) indices.push_back(first + index);
  };
  quad({ -2,-1,4 }, { -2,2,4 }, { 2,2,4 }, { 2,-1,4 }, { 0,0,-1 });
  quad({ -2,-1,0 }, { -2,-1,4 }, { 2,-1,4 }, { 2,-1,0 }, { 0,1,0 });
  quad({ -2,-1,0 }, { -2,2,0 }, { -2,2,4 }, { -2,-1,4 }, { 1,0,0 });
  quad({ 2,-1,4 }, { 2,2,4 }, { 2,2,0 }, { 2,-1,0 }, { -1,0,0 });
  quad({ -2,2,4 }, { -2,2,0 }, { 2,2,0 }, { 2,2,4 }, { 0,-1,0 });
  remixapi_MeshInfoSurfaceTriangles surface = {};
  surface.vertices_values = vertices.data();
  surface.vertices_count = vertices.size();
  surface.indices_values = indices.data();
  surface.indices_count = indices.size();
  surface.material = runtime.material;
  remixapi_MeshInfo mesh = {};
  mesh.sType = REMIXAPI_STRUCT_TYPE_MESH_INFO;
  mesh.hash = 0x534D4F4B4502ull;
  mesh.surfaces_values = &surface;
  mesh.surfaces_count = 1;
  checked(runtime.api.CreateMesh(&mesh, &runtime.mesh), "CreateMesh");

  remixapi_LightInfoSphereEXT sphere = {};
  sphere.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_SPHERE_EXT;
  sphere.position = { 0, 1, 1 };
  sphere.radius = 0.15f;
  sphere.volumetricRadianceScale = 1.0f;
  remixapi_LightInfo light = {};
  light.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO;
  light.pNext = &sphere;
  light.hash = 0x534D4F4B4503ull;
  light.radiance = { 100, 100, 100 };
  checked(runtime.api.CreateLight(&light, &runtime.light), "CreateLight");
}

void readback(Runtime& runtime, const char* mode) {
  checked(runtime.api.dxvk_CopyRenderingOutput(runtime.output,
    REMIXAPI_DXVK_COPY_RENDERING_OUTPUT_TYPE_FINAL_COLOR), "CopyRenderingOutput");
  runtime.context->CopyResource(runtime.staging, runtime.output);
  D3D11_MAPPED_SUBRESOURCE mapped = {};
  require(SUCCEEDED(runtime.context->Map(runtime.staging, 0, D3D11_MAP_READ, 0, &mapped)),
          "Final-color staging readback failed");
  bool finite = true;
  double total = 0.0;
  unsigned litPixels = 0;
  for (UINT y = 0; y < kHeight; ++y) {
    const auto* row = reinterpret_cast<const float*>(
      static_cast<const unsigned char*>(mapped.pData) + size_t(y) * mapped.RowPitch);
    for (UINT x = 0; x < kWidth; ++x) {
      const float* rgba = row + size_t(x) * 4;
      for (unsigned component = 0; component < 4; ++component) finite &= std::isfinite(rgba[component]);
      const double sum = double(rgba[0]) + rgba[1] + rgba[2];
      total += sum;
      litPixels += sum > 0.001 ? 1u : 0u;
    }
  }
  runtime.context->Unmap(runtime.staging, 0);
  require(finite, "Final-color image contains NaN or infinity");
  require(litPixels > kWidth * kHeight / 100, "Final-color image is black or almost empty");
  std::printf("%s readback: %u lit pixels, mean RGB %.6f, all finite\n",
              mode, litPixels, total / (double(kWidth) * kHeight * 3.0));
}
}

int wmain(int argc, wchar_t** argv) {
  if (argc < 2 || argc > 4) {
    std::fputs("Usage: test_remix_rendering.exe ABSOLUTE_RUNTIME_DIRECTORY [all|compute|sharc|restir|traceray|di-off|di-no-reuse|di-reset] [--skip-startup-cycle]\n", stderr);
    return 2;
  }
  const std::wstring selected = argc >= 3 ? argv[2] : L"all";
  const bool skipStartupCycle = argc == 4 && std::wstring(argv[3]) == L"--skip-startup-cycle";
  if (argc == 4 && !skipStartupCycle) {
    std::fputs("Unknown rendering diagnostic option.\n", stderr);
    return 2;
  }
  if (selected != L"all" && selected != L"compute" && selected != L"sharc"
   && selected != L"restir" && selected != L"traceray"
   && selected != L"di-off" && selected != L"di-no-reuse" && selected != L"di-reset") {
    std::fputs("Unknown rendering phase selector.\n", stderr);
    return 2;
  }
  HWND window = nullptr;
  try {
    Watchdog watchdog;
    const std::wstring directory = argv[1];
    require(directory.size() > 2 && directory[1] == L':', "Runtime directory must be absolute");
    SetDllDirectoryW(directory.c_str());
    SetEnvironmentVariableW(L"DXVK_REMIX_PREWARM", L"0");
    SetEnvironmentVariableW(L"RTX_PREWARM_ALL_VARIANTS", L"0");
    SetEnvironmentVariableW(L"DXVK_REMIX_FORCE_CURRENT_PROCESS", L"1");
    SetEnvironmentVariableW(L"DXVK_ENABLE_RAYTRACING", L"1");
    HMODULE dxgi = LoadLibraryExW((directory + L"\\dxgi.dll").c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    HMODULE d3d11 = LoadLibraryExW((directory + L"\\d3d11.dll").c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    require(dxgi && d3d11, "Loading built sibling DXGI/D3D11 DLLs failed");
    require(GetProcAddress(dxgi, "CreateDXGIFactory1") != nullptr, "Built DXGI export missing");
    auto initialize = reinterpret_cast<PFN_remixapi_InitializeLibrary>(GetProcAddress(d3d11, "remixapi_InitializeLibrary"));
    auto wasRayTraced = reinterpret_cast<BOOL (WINAPI*)(BOOL)>(GetProcAddress(d3d11, "remixapi_dxvk_WasLastPresentRayTraced"));
    require(initialize && wasRayTraced, "Remix initialization/readiness export missing");
    Runtime runtime;
    remixapi_InitializeLibraryInfo initializeInfo = {};
    initializeInfo.sType = REMIXAPI_STRUCT_TYPE_INITIALIZE_LIBRARY_INFO;
    initializeInfo.version = REMIXAPI_VERSION_MAKE(REMIXAPI_VERSION_MAJOR, REMIXAPI_VERSION_MINOR, REMIXAPI_VERSION_PATCH);
    checked(initialize(&initializeInfo, &runtime.api), "InitializeLibrary");

    WNDCLASSW windowClass = {};
    windowClass.lpfnWndProc = windowProc;
    windowClass.hInstance = GetModuleHandleW(nullptr);
    windowClass.lpszClassName = L"RemixRenderingSmoke";
    require(RegisterClassW(&windowClass) != 0, "RegisterClass failed");
    RECT rectangle = { 0, 0, LONG(kWidth), LONG(kHeight) };
    AdjustWindowRect(&rectangle, WS_OVERLAPPEDWINDOW, FALSE);
    window = CreateWindowExW(0, windowClass.lpszClassName, L"Remix rendering validation",
      WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
      rectangle.right - rectangle.left, rectangle.bottom - rectangle.top,
      nullptr, nullptr, windowClass.hInstance, nullptr);
    require(window != nullptr, "CreateWindow failed");
    // Rendering is the visual subject of this smoke test. Hidden/minimized
    // windows intentionally bypass Remix in D3D11SwapChain::Present.
    ShowWindow(window, SW_SHOWNOACTIVATE);
    UpdateWindow(window);
    remixapi_PresentInfo present = {};
    present.sType = REMIXAPI_STRUCT_TYPE_PRESENT_INFO;
    present.hwndOverride = window;
    if (skipStartupCycle) {
      step("DIAGNOSTIC: skipping preliminary Startup/Shutdown cycle; final device cleanup remains enabled.");
    } else {
      remixapi_StartupInfo startup = {};
      startup.sType = REMIXAPI_STRUCT_TYPE_STARTUP_INFO;
      startup.hwnd = window;
      step("Calling actual-window Startup.");
      checked(runtime.api.Startup(&startup), "Startup");
      runtime.registered = true;
      step("Calling empty-scene Present.");
      checked(runtime.api.Present(&present), "Empty Startup Present");
      step("Checking empty-scene presentation readiness.");
      require(!wasRayTraced(TRUE), "Empty scene incorrectly reported a complete RTX frame");
      step("Calling Startup Shutdown.");
      checked(runtime.api.Shutdown(), "Startup Shutdown");
      runtime.registered = false;
      step("Window Startup, empty-frame readiness, and Shutdown passed.");
    }

    // The explicit-device public API is needed to allocate textures on the
    // same device as the renderer. Startup intentionally exposes no getter.
    step("Creating explicit public API device for rendering/readback.");
    checked(runtime.api.dxvk_CreateD3D11Device(FALSE, &runtime.device), "CreateD3D11Device");
    checked(runtime.api.dxvk_RegisterD3D11Device(runtime.device), "RegisterD3D11Device");
    runtime.registered = true;
    runtime.device->GetImmediateContext(&runtime.context);
    require(runtime.context != nullptr, "GetImmediateContext failed");
    const std::pair<const char*, const char*> options[] = {
      { "rtx.graphicsPreset", "4" }, { "rtx.raytraceModePreset", "0" },
      { "rtx.enableRaytracing", "True" }, { "rtx.enableSecondaryBounces", "True" },
      { "rtx.renderPassGBufferRaytraceMode", "0" }, { "rtx.renderPassIntegrateDirectRaytraceMode", "0" },
      { "rtx.renderPassIntegrateIndirectRaytraceMode", "0" }, { "rtx.upscalerType", "0" },
      { "rtx.enableRayReconstruction", "False" }, { "rtx.sceneScale", "0.01" },
      { "rtx.shader.enableAsyncCompilationUI", "False" }
    };
    for (const auto& option : options) checked(runtime.api.SetConfigVariable(option.first, option.second), option.first);
    D3D11_TEXTURE2D_DESC texture = {};
    texture.Width = kWidth;
    texture.Height = kHeight;
    texture.MipLevels = texture.ArraySize = 1;
    texture.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    texture.SampleDesc.Count = 1;
    texture.Usage = D3D11_USAGE_DEFAULT;
    texture.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    require(SUCCEEDED(runtime.device->CreateTexture2D(&texture, nullptr, &runtime.output)), "Create output texture failed");
    texture.Usage = D3D11_USAGE_STAGING;
    texture.BindFlags = 0;
    texture.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    require(SUCCEEDED(runtime.device->CreateTexture2D(&texture, nullptr, &runtime.staging)), "Create staging texture failed");
    makeScene(runtime);
    remixapi_CameraInfoParameterizedEXT parameters = {};
    parameters.sType = REMIXAPI_STRUCT_TYPE_CAMERA_INFO_PARAMETERIZED_EXT;
    parameters.position = { 0, 0, -1 };
    parameters.forward = { 0, 0, 1 };
    parameters.up = { 0, 1, 0 };
    parameters.right = { 1, 0, 0 };
    parameters.fovYInDegrees = 60;
    parameters.aspect = float(kWidth) / float(kHeight);
    parameters.nearPlane = 0.1f;
    parameters.farPlane = 100;
    remixapi_CameraInfo camera = {};
    camera.sType = REMIXAPI_STRUCT_TYPE_CAMERA_INFO;
    camera.pNext = &parameters;
    camera.type = REMIXAPI_CAMERA_TYPE_WORLD;
    remixapi_InstanceInfo instance = {};
    instance.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO;
    instance.mesh = runtime.mesh;
    instance.doubleSided = TRUE;
    for (unsigned axis = 0; axis < 3; ++axis) instance.transform.matrix[axis][axis] = 1.0f;
    struct Phase {
      const wchar_t* selector;
      const char* name;
      const char* integrationMode;
      const char* gbufferMode;
      const char* directMode;
      const char* indirectMode;
      const char* enableDI;
      const char* reuseDI;
      bool rasterGap;
    };
    const Phase phases[] = {
      { L"compute", "Importance sampled / ray query", "0", "0", "0", "0", "True", "True", false },
      { L"sharc", "SHARC / ray query", "3", "0", "0", "0", "True", "True", false },
      { L"restir", "ReSTIR GI / ray query", "1", "0", "0", "0", "True", "True", false },
      { L"traceray", "Importance sampled / TraceRay", "0", "2", "1", "2", "True", "True", false },
      { L"di-off", "Direct RTXDI disabled", "0", "0", "0", "0", "False", "True", false },
      { L"di-no-reuse", "Direct RTXDI reuse disabled", "0", "0", "0", "0", "True", "False", false },
      { L"di-reset", "Direct RTXDI after raster gap and camera move", "0", "0", "0", "0", "True", "True", true },
    };
    for (const Phase& phase : phases) {
      if (selected != L"all" && selected != phase.selector)
        continue;
      checked(runtime.api.SetConfigVariable("rtx.integrateIndirectMode", phase.integrationMode), "Set indirect mode");
      checked(runtime.api.SetConfigVariable("rtx.renderPassGBufferRaytraceMode", phase.gbufferMode), "Set GBuffer tracing mode");
      checked(runtime.api.SetConfigVariable("rtx.renderPassIntegrateDirectRaytraceMode", phase.directMode), "Set direct tracing mode");
      checked(runtime.api.SetConfigVariable("rtx.renderPassIntegrateIndirectRaytraceMode", phase.indirectMode), "Set indirect tracing mode");
      checked(runtime.api.SetConfigVariable("rtx.useRTXDI", phase.enableDI), "Set direct RTXDI");
      checked(runtime.api.SetConfigVariable("rtx.di.enableTemporalReuse", phase.reuseDI), "Set DI temporal reuse");
      checked(runtime.api.SetConfigVariable("rtx.di.enableSpatialReuse", phase.reuseDI), "Set DI spatial reuse");
      if (phase.rasterGap) {
        checked(runtime.api.SetConfigVariable("rtx.enableRaytracing", "False"), "Disable RTX for history gap");
        for (unsigned frame = 0; frame < 2; ++frame) {
          pumpMessages();
          checked(runtime.api.SetupCamera(&camera), "Setup raster-gap camera");
          checked(runtime.api.DrawInstance(&instance), "Draw raster-gap instance");
          checked(runtime.api.DrawLightInstance(runtime.light), "Draw raster-gap light");
          checked(runtime.api.Present(&present), "Present raster-gap frame");
        }
        require(!wasRayTraced(TRUE), "Disabled RTX frame incorrectly reported rendering readiness");
        parameters.position.x = 0.25f;
        checked(runtime.api.SetConfigVariable("rtx.enableRaytracing", "True"), "Restore RTX after history gap");
        step("Direct RTXDI reset check: two raster frames completed; camera moved before re-enabling RTX.");
      }
      std::printf("Beginning %s (120-second cold warm-up, 30-second steady limit).\n", phase.name);
      std::fflush(stdout);
      const ULONGLONG began = GetTickCount64();
      ULONGLONG firstReady = 0;
      unsigned successful = 0;
      unsigned attempted = 0;
      while (successful < kRequiredFrames) {
        const ULONGLONG now = GetTickCount64();
        if (firstReady ? now - firstReady >= kSteadyTimeoutMs : now - began >= kWarmupTimeoutMs)
          break;
        pumpMessages();
        checked(runtime.api.SetupCamera(&camera), "SetupCamera");
        checked(runtime.api.DrawInstance(&instance), "DrawInstance");
        checked(runtime.api.DrawLightInstance(runtime.light), "DrawLightInstance");
        checked(runtime.api.Present(&present), "Present");
        ++attempted;
        const bool ready = wasRayTraced(TRUE) != FALSE;
        // Options commit at frame end. The first presentation can still use
        // the preceding mode, so it cannot count toward this phase's result.
        if (attempted == 1) {
          Sleep(5);
          continue;
        }
        if (ready) {
          if (firstReady == 0) firstReady = GetTickCount64();
          ++successful;
        } else {
          successful = 0;
          // First-use pipelines can be discovered in several stages. A gap
          // ends the steady interval instead of charging compilation to it.
          firstReady = 0;
        }
        Sleep(5);
      }
      std::printf("%s: %u consecutive complete frames / %u attempts in %llu ms\n",
                  phase.name, successful, attempted, GetTickCount64() - began);
      std::fflush(stdout);
      require(successful == kRequiredFrames, "Timed out waiting for complete ray-traced frames");
      readback(runtime, phase.name);
    }
    std::puts("All selected real-scene rendering phases passed.");
  } catch (const std::exception& error) {
    std::fprintf(stderr, "Rendering smoke failed: %s\n", error.what());
    step("API/device cleanup completed; destroying smoke window after failure.");
    if (window) DestroyWindow(window);
    step("Failure cleanup completed; returning exit code 1.");
    return 1;
  }
  step("API/device cleanup completed; destroying smoke window.");
  if (window) DestroyWindow(window);
  step("Normal cleanup completed; returning exit code 0.");
  // Leave DLL unload to process teardown, after runtime worker shutdown.
  return 0;
}
