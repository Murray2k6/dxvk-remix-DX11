#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>
#include "../../public/include/remix/remix_c.h"

namespace {
using Microsoft::WRL::ComPtr;
constexpr UINT kWidth = 321, kHeight = 241;
constexpr UINT kVertexCapacity = 2048, kFirstDomain = 1024, kSecondDomain = 1536;
constexpr UINT kSide = 9, kIndexCount = (kSide - 1) * (kSide - 1) * 6;
using FrameCounter = uint64_t (WINAPI*)(ID3D11Device*);
std::chrono::steady_clock::time_point gStarted;
bool gFirstNativePresent=false,gFirstCompletedRt=false;
void milestone(const char* name) {
  const double ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-gStarted).count();
  std::printf("Startup milestone: %s elapsedMs=%.3f\n",name,ms);std::fflush(stdout);
}
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void checked(HRESULT result, const char* operation) {
  if (FAILED(result)) {
    std::fprintf(stderr, "%s failed: 0x%08lx\n", operation, result);
    throw std::runtime_error(operation);
  }
}
void apiChecked(remixapi_ErrorCode result, const char* operation) {
  if (result != REMIXAPI_ERROR_CODE_SUCCESS) {
    std::fprintf(stderr, "%s failed: Remix error %d\n", operation, int(result));
    throw std::runtime_error(operation);
  }
}
class Watchdog {
  HANDLE stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  HANDLE worker = nullptr;
  static DWORD WINAPI run(void* event) {
    if (WaitForSingleObject(static_cast<HANDLE>(event), 1200000) == WAIT_TIMEOUT)
      TerminateProcess(GetCurrentProcess(), 124);
    return 0;
  }
public:
  Watchdog() {
    require(stop != nullptr, "CreateEvent watchdog");
    worker = CreateThread(nullptr, 0, run, stop, 0, nullptr);
    require(worker != nullptr, "CreateThread watchdog");
  }
  ~Watchdog() { SetEvent(stop); WaitForSingleObject(worker, INFINITE); CloseHandle(worker); CloseHandle(stop); }
};
struct Window {
  HWND handle = nullptr;
  Window() {
    WNDCLASSW wc = {};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"RemixNativeCaptureValidation";
    require(RegisterClassW(&wc) != 0, "RegisterClass");
    RECT area = { 0, 0, LONG(kWidth), LONG(kHeight) };
    AdjustWindowRect(&area, WS_OVERLAPPEDWINDOW, FALSE);
    handle = CreateWindowW(wc.lpszClassName, L"Remix native D3D11 capture validation", WS_OVERLAPPEDWINDOW,
      CW_USEDEFAULT, CW_USEDEFAULT, area.right - area.left, area.bottom - area.top,
      nullptr, nullptr, wc.hInstance, nullptr);
    require(handle != nullptr, "CreateWindow");
    ShowWindow(handle, SW_SHOWNOACTIVATE);
    UpdateWindow(handle);
  }
  ~Window() { if (handle) DestroyWindow(handle); }
};
struct Api {
  remixapi_Interface table = {};
  bool registered = false;
  remixapi_LightHandle light = nullptr;
  ~Api() {
    if (registered) {
      if (light) table.DestroyLight(light);
      table.Shutdown();
    }
  }
};
struct Vertex { float position[3]; float normal[3]; float uv[2]; };
struct Matrices { float world[16]; float view[16]; float projection[16]; };
void identity(float* matrix) {
  std::memset(matrix, 0, sizeof(float) * 16);
  for (unsigned i = 0; i < 4; ++i) matrix[i * 5] = 1;
}
void fillDomain(std::vector<Vertex>& vertices, UINT first, float centerX) {
  for (UINT y = 0; y < kSide; ++y) for (UINT x = 0; x < kSide; ++x) {
    const float u = float(x) / float(kSide - 1), v = float(y) / float(kSide - 1);
    vertices[first + y * kSide + x] = { { centerX + (u - 0.5f) * 2.0f, (v - 0.5f) * 2.0f, 0 },
                                     { 0, 0, -1 }, { u, v } };
  }
}
std::vector<uint16_t> indicesFor(UINT first) {
  std::vector<uint16_t> indices;
  for (UINT y = 0; y + 1 < kSide; ++y) for (UINT x = 0; x + 1 < kSide; ++x) {
    const UINT a = first + y * kSide + x, b = a + 1, c = a + kSide, d = c + 1;
    for (UINT index : { a, d, b, a, c, d }) indices.push_back(uint16_t(index));
  }
  return indices;
}
ComPtr<ID3DBlob> compile(const char* source, const char* entry, const char* target) {
  ComPtr<ID3DBlob> bytecode, errors;
  const HRESULT result = D3DCompile(source, std::strlen(source), "native-capture-regression", nullptr, nullptr,
    entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL3 | D3DCOMPILE_PACK_MATRIX_ROW_MAJOR, 0,
    bytecode.GetAddressOf(), errors.GetAddressOf());
  if (errors) std::fwrite(errors->GetBufferPointer(), 1, errors->GetBufferSize(), stderr);
  checked(result, "D3DCompile");
  return bytecode;
}
const char* kFixtureShader = R"(
cbuffer Camera : register(b0) { row_major float4x4 World; row_major float4x4 View; row_major float4x4 Projection; };
struct Input { float3 position : POSITION; float3 normal : NORMAL; float2 uv : TEXCOORD0; float3 instanceOffset : INSTANCE_OFFSET; uint instanceId : SV_InstanceID; };
struct Output { float4 position : SV_Position; float2 uv : TEXCOORD0; float3 normal : TEXCOORD1; };
Output vsMain(Input input) {
  Output result;
  float4 position = mul(float4(input.position, 1), World);
  position.xyz += input.instanceOffset;
  result.position = mul(mul(position, View), Projection);
  result.uv = input.uv; result.normal = input.normal;
  return result;
}
Output vsInstanced(Input input) {
  input.position.xy *= 0.3;
  input.position.x += float(input.instanceId) * 0.7 - 2.0;
  return vsMain(input);
}
Output vsShaderPlacement(Input input) {
  input.position.z += 6.0;
  return vsMain(input);
}
Texture2D<float4> Color : register(t0); SamplerState ColorSampler : register(s0);
float4 psMain(Output input) : SV_Target { return Color.Sample(ColorSampler, input.uv) * (0.75 + 0.25 * abs(input.normal.z)); }
)";
const char* kGpuDrawShader = R"(
struct Input { float3 position : POSITION; float3 normal : NORMAL; float2 uv : TEXCOORD0; };
struct Output { float4 position : SV_Position; float3 normal : NORMAL; float2 uv : TEXCOORD0; };
Output vsStream(Input input) {
  Output result; result.position = float4(input.position, 1);
  result.normal = input.normal; result.uv = input.uv; return result;
}
[maxvertexcount(3)]
void gsStream(triangle Output input[3], inout TriangleStream<Output> stream) {
  for (uint i = 0; i < 3; ++i) stream.Append(input[i]);
  stream.RestartStrip();
}
RWByteAddressBuffer Arguments : register(u0);
[numthreads(1, 1, 1)]
void csArguments(uint3 id : SV_DispatchThreadID) {
  Arguments.Store4(0, uint4(6, 1, 0, 0));
  Arguments.Store(16, 0);
}
)";
struct ImageMetrics { unsigned hits = 0; double centroidX = 0; double meanColor = 0; };
struct Renderer {
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> context;
  ComPtr<IDXGISwapChain> swapchain;
  ComPtr<ID3D11RenderTargetView> target;
  ComPtr<ID3D11DepthStencilView> depth;
  ComPtr<ID3D11Texture2D> output, staging;
  ComPtr<ID3D11Texture2D> backbuffer, nativeStaging;
  ComPtr<ID3D11Buffer> dynamicVertices, gpuVertices, indices, constants, instances, compositeVertices;
  ComPtr<ID3D11VertexShader> vertexShader, instanceIdShader, shaderPlacement;
  ComPtr<ID3D11PixelShader> pixelShader;
  ComPtr<ID3D11InputLayout> layout, divisorLayout;
  ComPtr<ID3D11ShaderResourceView> albedo, compositeSource;
  ComPtr<ID3D11RenderTargetView> compositeTarget;
  ComPtr<ID3D11SamplerState> sampler;
  ComPtr<ID3D11RasterizerState> rasterizer;
  ComPtr<ID3D11DepthStencilState> depthState, compositeDepthState;
  ComPtr<ID3D11Buffer> gpuDrawVertices, gpuDrawIndices, gpuDrawArguments, streamVertices;
  ComPtr<ID3D11UnorderedAccessView> argumentUav;
  ComPtr<ID3D11ComputeShader> argumentShader;
  ComPtr<ID3D11VertexShader> streamShader;
  ComPtr<ID3D11GeometryShader> streamOutputShader;
  Api api;
  FrameCounter frameCounter = nullptr;
  Matrices matrices = {};
  uint64_t completedFrames() {
    const uint64_t frames=frameCounter(device.Get());
    if(frames&&!gFirstCompletedRt) {gFirstCompletedRt=true;milestone("first-completed-rt");}
    return frames;
  }

  ComPtr<ID3D11Buffer> buffer(UINT bytes, UINT bind, D3D11_USAGE usage, const void* data = nullptr) {
    D3D11_BUFFER_DESC desc = {};
    desc.ByteWidth = bytes; desc.BindFlags = bind; desc.Usage = usage;
    if (usage == D3D11_USAGE_DYNAMIC) desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    D3D11_SUBRESOURCE_DATA initial = {}; initial.pSysMem = data;
    ComPtr<ID3D11Buffer> result;
    checked(device->CreateBuffer(&desc, data ? &initial : nullptr, result.GetAddressOf()), "CreateBuffer");
    return result;
  }
  void write(ID3D11Buffer* destination, const void* source, size_t bytes, D3D11_MAP mode = D3D11_MAP_WRITE_DISCARD) {
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    checked(context->Map(destination, 0, mode, 0, &mapped), "Map dynamic geometry");
    std::memcpy(mapped.pData, source, bytes);
    context->Unmap(destination, 0);
  }
  void setup(IDXGIFactory1* factory, HWND window) {
    DXGI_SWAP_CHAIN_DESC sc = {};
    sc.BufferDesc.Width = kWidth; sc.BufferDesc.Height = kHeight;
    sc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sc.SampleDesc.Count = 1; sc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sc.BufferCount = 2; sc.OutputWindow = window; sc.Windowed = TRUE; sc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    checked(factory->CreateSwapChain(device.Get(), &sc, swapchain.GetAddressOf()), "CreateSwapChain");
    checked(swapchain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(backbuffer.GetAddressOf())), "GetBuffer");
    checked(device->CreateRenderTargetView(backbuffer.Get(), nullptr, target.GetAddressOf()), "CreateRenderTargetView");
    D3D11_TEXTURE2D_DESC nativeReadback = {};
    backbuffer->GetDesc(&nativeReadback);
    nativeReadback.Usage = D3D11_USAGE_STAGING; nativeReadback.BindFlags = 0;
    nativeReadback.CPUAccessFlags = D3D11_CPU_ACCESS_READ; nativeReadback.MiscFlags = 0;
    checked(device->CreateTexture2D(&nativeReadback, nullptr, nativeStaging.GetAddressOf()), "Create native readback");
    D3D11_TEXTURE2D_DESC image = {};
    image.Width = kWidth; image.Height = kHeight; image.MipLevels = image.ArraySize = 1;
    image.SampleDesc.Count = 1; image.Format = DXGI_FORMAT_D32_FLOAT;
    image.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    ComPtr<ID3D11Texture2D> depthImage;
    checked(device->CreateTexture2D(&image, nullptr, depthImage.GetAddressOf()), "Create depth texture");
    checked(device->CreateDepthStencilView(depthImage.Get(), nullptr, depth.GetAddressOf()), "CreateDepthStencilView");
    image.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    image.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    checked(device->CreateTexture2D(&image, nullptr, output.GetAddressOf()), "Create output texture");
    image.Usage = D3D11_USAGE_STAGING; image.BindFlags = 0; image.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    checked(device->CreateTexture2D(&image, nullptr, staging.GetAddressOf()), "Create staging texture");


    auto vs = compile(kFixtureShader, "vsMain", "vs_5_0"), ps = compile(kFixtureShader, "psMain", "ps_5_0");
    auto instancedVs = compile(kFixtureShader, "vsInstanced", "vs_5_0");
    auto placedVs = compile(kFixtureShader, "vsShaderPlacement", "vs_5_0");
    checked(device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, vertexShader.GetAddressOf()), "CreateVertexShader");
    checked(device->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, pixelShader.GetAddressOf()), "CreatePixelShader");
    checked(device->CreateVertexShader(instancedVs->GetBufferPointer(), instancedVs->GetBufferSize(), nullptr, instanceIdShader.GetAddressOf()), "Create SV_InstanceID shader");
    checked(device->CreateVertexShader(placedVs->GetBufferPointer(), placedVs->GetBufferSize(), nullptr, shaderPlacement.GetAddressOf()), "Create shader-placement VS");
    D3D11_INPUT_ELEMENT_DESC elements[] = {
      { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
      { "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 },
      { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0 },
      { "INSTANCE_OFFSET", 0, DXGI_FORMAT_R32G32B32_FLOAT, 1, 0, D3D11_INPUT_PER_INSTANCE_DATA, 1 },
    };
    checked(device->CreateInputLayout(elements, UINT(std::size(elements)), vs->GetBufferPointer(), vs->GetBufferSize(), layout.GetAddressOf()), "CreateInputLayout");
    elements[3].InstanceDataStepRate = 2;
    checked(device->CreateInputLayout(elements, UINT(std::size(elements)), instancedVs->GetBufferPointer(), instancedVs->GetBufferSize(), divisorLayout.GetAddressOf()), "Create divisor input layout");
    std::vector<Vertex> vertices(kVertexCapacity);
    fillDomain(vertices, kFirstDomain, -0.9f); fillDomain(vertices, kSecondDomain, -1.6f);
    dynamicVertices = buffer(UINT(vertices.size() * sizeof(Vertex)), D3D11_BIND_VERTEX_BUFFER, D3D11_USAGE_DYNAMIC, vertices.data());
    gpuVertices = buffer(UINT(vertices.size() * sizeof(Vertex)), D3D11_BIND_VERTEX_BUFFER, D3D11_USAGE_DEFAULT, vertices.data());
    auto initialIndices = indicesFor(kFirstDomain);
    indices = buffer(UINT(initialIndices.size() * sizeof(uint16_t)), D3D11_BIND_INDEX_BUFFER, D3D11_USAGE_DYNAMIC, initialIndices.data());
    const float offsets[3][3] = { { 0, 0, 0 }, { 1.5f, 0, 0 }, { -1.5f, 0, 0 } };
    instances = buffer(sizeof(offsets), D3D11_BIND_VERTEX_BUFFER, D3D11_USAGE_IMMUTABLE, offsets);
    identity(matrices.world); identity(matrices.view); matrices.view[14] = 6.0f;
    const float yScale = 1.0f / std::tan(60.0f * 3.14159265359f / 360.0f);
    matrices.projection[0] = yScale * float(kHeight) / float(kWidth);
    matrices.projection[5] = yScale; matrices.projection[10] = 100.0f / 99.9f;
    matrices.projection[11] = 1; matrices.projection[14] = -10.0f / 99.9f;
    constants = buffer(sizeof(matrices), D3D11_BIND_CONSTANT_BUFFER, D3D11_USAGE_DYNAMIC, &matrices);
    image = {}; image.Width = image.Height = 2; image.MipLevels = image.ArraySize = 1;
    image.Format = DXGI_FORMAT_R8G8B8A8_UNORM; image.SampleDesc.Count = 1;
    image.Usage = D3D11_USAGE_IMMUTABLE; image.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    const uint32_t texels[] = { 0xff60b0f0, 0xffe0a060, 0xffe0a060, 0xff60b0f0 };
    D3D11_SUBRESOURCE_DATA pixels = {}; pixels.pSysMem = texels; pixels.SysMemPitch = 8;
    ComPtr<ID3D11Texture2D> color;
    checked(device->CreateTexture2D(&image, &pixels, color.GetAddressOf()), "Create albedo texture");
    checked(device->CreateShaderResourceView(color.Get(), nullptr, albedo.GetAddressOf()), "CreateShaderResourceView");
    image.Width = kWidth; image.Height = kHeight;
    image.Usage = D3D11_USAGE_DEFAULT;
    image.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> compositeImage;
    checked(device->CreateTexture2D(&image, nullptr, compositeImage.GetAddressOf()), "Create composite source");
    checked(device->CreateRenderTargetView(compositeImage.Get(), nullptr, compositeTarget.GetAddressOf()), "Create composite RTV");
    checked(device->CreateShaderResourceView(compositeImage.Get(), nullptr, compositeSource.GetAddressOf()), "Create composite SRV");
    // A full-screen attachment composite retains the same real camera CB.
    const Vertex quad[] = {
      { { -5, -4, 0 }, { 0, 0, -1 }, { 0, 1 } }, { { -5, 4, 0 }, { 0, 0, -1 }, { 0, 0 } },
      { { 5, 4, 0 }, { 0, 0, -1 }, { 1, 0 } }, { { -5, -4, 0 }, { 0, 0, -1 }, { 0, 1 } },
      { { 5, 4, 0 }, { 0, 0, -1 }, { 1, 0 } }, { { 5, -4, 0 }, { 0, 0, -1 }, { 1, 1 } },
    };
    compositeVertices = buffer(sizeof(quad), D3D11_BIND_VERTEX_BUFFER, D3D11_USAGE_IMMUTABLE, quad);
    D3D11_SAMPLER_DESC sample = {}; sample.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sample.AddressU = sample.AddressV = sample.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sample.MaxLOD = D3D11_FLOAT32_MAX;
    checked(device->CreateSamplerState(&sample, sampler.GetAddressOf()), "CreateSamplerState");
    D3D11_RASTERIZER_DESC raster = {}; raster.FillMode = D3D11_FILL_SOLID; raster.CullMode = D3D11_CULL_NONE; raster.DepthClipEnable = TRUE;
    checked(device->CreateRasterizerState(&raster, rasterizer.GetAddressOf()), "CreateRasterizerState");
    D3D11_DEPTH_STENCIL_DESC z = {}; z.DepthEnable = TRUE; z.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL; z.DepthFunc = D3D11_COMPARISON_LESS;
    checked(device->CreateDepthStencilState(&z, depthState.GetAddressOf()), "CreateDepthStencilState");
    z.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO; z.DepthFunc = D3D11_COMPARISON_ALWAYS;
    checked(device->CreateDepthStencilState(&z, compositeDepthState.GetAddressOf()), "Create composite depth state");
  }
  void draw(ID3D11Buffer* vertices, UINT firstInstance, bool instanced,
            UINT instanceCount, bool useDivisor, bool composite, bool placed, bool probe = false,
            UINT gpuDraw = 0) {
    MSG message;
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&message); DispatchMessageW(&message); }
    ID3D11RenderTargetView* rtv = target.Get(); context->OMSetRenderTargets(1, &rtv, depth.Get());
    context->OMSetDepthStencilState(depthState.Get(), 0);
    const float clear[4] = { 0.01f, 0.01f, 0.01f, 1 };
    context->ClearRenderTargetView(target.Get(), clear); context->ClearDepthStencilView(depth.Get(), D3D11_CLEAR_DEPTH, 1, 0);
    D3D11_VIEWPORT viewport = { 0, 0, float(kWidth), float(kHeight), 0, 1 };
    context->RSSetViewports(1, &viewport); context->RSSetState(rasterizer.Get());
    context->IASetInputLayout(useDivisor ? divisorLayout.Get() : layout.Get()); context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11Buffer* streams[] = { vertices, instances.Get() };
    const UINT strides[] = { sizeof(Vertex), sizeof(float) * 3 }, offsets[] = { 0, 0 };
    context->IASetVertexBuffers(0, 2, streams, strides, offsets); context->IASetIndexBuffer(indices.Get(), DXGI_FORMAT_R16_UINT, 0);
    context->VSSetShader(placed ? shaderPlacement.Get() : (useDivisor ? instanceIdShader.Get() : vertexShader.Get()), nullptr, 0); context->PSSetShader(pixelShader.Get(), nullptr, 0);
    ID3D11Buffer* cb = constants.Get(); context->VSSetConstantBuffers(0, 1, &cb);
    ID3D11ShaderResourceView* srv = albedo.Get(); context->PSSetShaderResources(0, 1, &srv);
    ID3D11SamplerState* smp = sampler.Get(); context->PSSetSamplers(0, 1, &smp);
    if (instanced) context->DrawIndexedInstanced(kIndexCount, instanceCount, 0, 0, firstInstance);
    else context->DrawIndexed(kIndexCount, 0, 0);
    if (composite) {
      const float magenta[4] = { 0.2f, 0, 0.2f, 1 };
      context->ClearRenderTargetView(compositeTarget.Get(), magenta);
      context->OMSetDepthStencilState(compositeDepthState.Get(), 0);
      streams[0] = compositeVertices.Get(); context->IASetVertexBuffers(0, 2, streams, strides, offsets);
      srv = compositeSource.Get(); context->PSSetShaderResources(0, 1, &srv);
      context->Draw(6, 0);
      srv = nullptr; context->PSSetShaderResources(0, 1, &srv);
    }
    if (gpuDraw != 0) drawGpuOnly(gpuDraw);
    if (probe) verifyNativeRaster(composite);
    apiChecked(api.table.DrawLightInstance(api.light), "DrawLightInstance");
    checked(swapchain->Present(0, 0), "Native IDXGISwapChain::Present");
    if(!gFirstNativePresent) {gFirstNativePresent=true;milestone("first-native-present");}
    checked(device->GetDeviceRemovedReason(), "Device health");
  }
  void setupGpuOnlyDraws() {
    const Vertex quad[] = {
      { { -2.8f, -.5f, -1 }, { 0, 0, -1 }, { 0, 1 } },
      { { -2.8f, .5f, -1 }, { 0, 0, -1 }, { 0, 0 } },
      { { -1.2f, .5f, -1 }, { 0, 0, -1 }, { 1, 0 } },
      { { -2.8f, -.5f, -1 }, { 0, 0, -1 }, { 0, 1 } },
      { { -1.2f, .5f, -1 }, { 0, 0, -1 }, { 1, 0 } },
      { { -1.2f, -.5f, -1 }, { 0, 0, -1 }, { 1, 1 } },
    };
    gpuDrawVertices = buffer(sizeof(quad), D3D11_BIND_VERTEX_BUFFER, D3D11_USAGE_IMMUTABLE, quad);
    const uint16_t indexData[] = { 0, 1, 2, 3, 4, 5 };
    gpuDrawIndices = buffer(sizeof(indexData), D3D11_BIND_INDEX_BUFFER, D3D11_USAGE_IMMUTABLE, indexData);
    streamVertices = buffer(sizeof(quad), D3D11_BIND_VERTEX_BUFFER | D3D11_BIND_STREAM_OUTPUT, D3D11_USAGE_DEFAULT);
    D3D11_BUFFER_DESC args = {};
    args.ByteWidth = 20; args.Usage = D3D11_USAGE_DEFAULT;
    args.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    args.MiscFlags = D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS | D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
    checked(device->CreateBuffer(&args, nullptr, gpuDrawArguments.GetAddressOf()), "Create GPU indirect arguments");
    D3D11_UNORDERED_ACCESS_VIEW_DESC uav = {};
    uav.Format = DXGI_FORMAT_R32_TYPELESS; uav.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    uav.Buffer.NumElements = 5; uav.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
    checked(device->CreateUnorderedAccessView(gpuDrawArguments.Get(), &uav, argumentUav.GetAddressOf()), "Create argument UAV");
    auto cs = compile(kGpuDrawShader, "csArguments", "cs_5_0");
    auto vs = compile(kGpuDrawShader, "vsStream", "vs_5_0");
    auto gs = compile(kGpuDrawShader, "gsStream", "gs_5_0");
    checked(device->CreateComputeShader(cs->GetBufferPointer(), cs->GetBufferSize(), nullptr, argumentShader.GetAddressOf()), "Create GPU argument shader");
    checked(device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, streamShader.GetAddressOf()), "Create SO vertex shader");
    const D3D11_SO_DECLARATION_ENTRY declaration[] = {
      { 0, "SV_Position", 0, 0, 3, 0 }, { 0, "NORMAL", 0, 0, 3, 0 }, { 0, "TEXCOORD", 0, 0, 2, 0 },
    };
    const UINT stride = sizeof(Vertex);
    checked(device->CreateGeometryShaderWithStreamOutput(gs->GetBufferPointer(), gs->GetBufferSize(),
      declaration, UINT(std::size(declaration)), &stride, 1, D3D11_SO_NO_RASTERIZED_STREAM, nullptr,
      streamOutputShader.GetAddressOf()), "Create SO geometry shader");
  }
  void drawGpuOnly(UINT mode) {
    ID3D11Buffer* streams[] = { gpuDrawVertices.Get(), instances.Get() };
    const UINT strides[] = { sizeof(Vertex), sizeof(float) * 3 }, offsets[] = { 0, 0 };
    context->IASetVertexBuffers(0, 2, streams, strides, offsets);
    if (mode == 3) {
      context->OMSetRenderTargets(0, nullptr, nullptr);
      context->VSSetShader(streamShader.Get(), nullptr, 0);
      context->GSSetShader(streamOutputShader.Get(), nullptr, 0);
      context->PSSetShader(nullptr, nullptr, 0);
      ID3D11Buffer* outputBuffer = streamVertices.Get(); const UINT offset = 0;
      context->SOSetTargets(1, &outputBuffer, &offset);
      context->Draw(6, 0);
      outputBuffer = nullptr; context->SOSetTargets(1, &outputBuffer, &offset);
      context->GSSetShader(nullptr, nullptr, 0);
      context->VSSetShader(vertexShader.Get(), nullptr, 0);
      context->PSSetShader(pixelShader.Get(), nullptr, 0);
      ID3D11RenderTargetView* rtv = target.Get(); context->OMSetRenderTargets(1, &rtv, depth.Get());
      streams[0] = streamVertices.Get(); context->IASetVertexBuffers(0, 2, streams, strides, offsets);
      context->DrawAuto();
    } else {
      // Produce counts on the GPU immediately before consumption. A mapped
      // allocation at API-record time cannot stand in for this command order.
      const UINT zero[] = { 0, 0, 0, 0 };
      context->ClearUnorderedAccessViewUint(argumentUav.Get(), zero);
      context->CSSetShader(argumentShader.Get(), nullptr, 0);
      ID3D11UnorderedAccessView* uav = argumentUav.Get();
      context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
      context->Dispatch(1, 1, 1);
      uav = nullptr; context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
      context->CSSetShader(nullptr, nullptr, 0);
      if (mode == 1) context->DrawInstancedIndirect(gpuDrawArguments.Get(), 0);
      else {
        context->IASetIndexBuffer(gpuDrawIndices.Get(), DXGI_FORMAT_R16_UINT, 0);
        context->DrawIndexedInstancedIndirect(gpuDrawArguments.Get(), 0);
      }
    }
  }
  std::vector<unsigned char> nativePixels() {
    context->CopyResource(nativeStaging.Get(), backbuffer.Get());
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    checked(context->Map(nativeStaging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "Map presented native frame");
    std::vector<unsigned char> pixels(size_t(kWidth) * kHeight * 4);
    for (UINT y = 0; y < kHeight; ++y)
      std::memcpy(pixels.data() + size_t(y) * kWidth * 4,
        static_cast<const char*>(mapped.pData) + size_t(y) * mapped.RowPitch, kWidth * 4);
    context->Unmap(nativeStaging.Get(), 0);
    return pixels;
  }
  void saveNativePixels(const std::string& name, const std::vector<unsigned char>& pixels) {
    FILE* file = nullptr; fopen_s(&file, name.c_str(), "wb");
    require(file != nullptr, "Create native fallback image");
    std::fprintf(file, "P6\n%u %u\n255\n", kWidth, kHeight);
    for (size_t i = 0; i < pixels.size(); i += 4) std::fwrite(pixels.data() + i, 1, 3, file);
    std::fclose(file);
  }
  void verifyGpuOnlyFallback() {
    setupGpuOnlyDraws();
    apiChecked(api.table.SetConfigVariable("rtx.enableRaytracing", "False"), "Disable RT for native GPU-draw oracle");
    draw(dynamicVertices.Get(), 0, false, 1, false, false, false);
    const auto directOnly = nativePixels();
    saveNativePixels("gpu-fallback-direct-only.ppm", directOnly);
    for (UINT mode = 1; mode <= 3; ++mode) {
      apiChecked(api.table.SetConfigVariable("rtx.enableRaytracing", "False"), "Disable RT for mixed native oracle");
      draw(dynamicVertices.Get(), 0, false, 1, false, false, false, false, mode);
      const auto oracle = nativePixels();
      saveNativePixels("gpu-fallback-" + std::to_string(mode) + "-oracle.ppm", oracle);
      unsigned changedPixels = 0;
      for (size_t i = 0; i < oracle.size(); i += 4)
        changedPixels += std::memcmp(oracle.data() + i, directOnly.data() + i, 4) != 0;
      require(changedPixels > 200, "GPU-produced draw did not add visible native geometry");
      apiChecked(api.table.SetConfigVariable("rtx.enableRaytracing", "True"), "Enable RT for GPU-draw fallback");
      const uint64_t before = completedFrames();
      for (unsigned frame = 0; frame < 3; ++frame) {
        draw(dynamicVertices.Get(), 0, false, 1, false, false, false, false, mode);
        const auto actual = nativePixels();
        const uint64_t after = completedFrames();
        unsigned mismatchedPixels = 0, maxDifference = 0;
        for (size_t i = 0; i < actual.size(); i += 4) {
          mismatchedPixels += std::memcmp(actual.data() + i, oracle.data() + i, 4) != 0;
          for (size_t c = 0; c < 4; ++c)
            maxDifference = (std::max)(maxDifference, unsigned(std::abs(int(actual[i + c]) - int(oracle[i + c]))));
        }
        std::printf("GPU-only comparison: mode=%u frame=%u mismatchedPixels=%u maxDifference=%u tracedBefore=%llu tracedAfter=%llu\n",
          mode, frame, mismatchedPixels, maxDifference, static_cast<unsigned long long>(before), static_cast<unsigned long long>(after));
        std::fflush(stdout);
        saveNativePixels("gpu-fallback-" + std::to_string(mode) + "-actual-" + std::to_string(frame) + ".ppm", actual);
        require(actual == oracle, "RTX overwrote part of the mixed direct/GPU-produced native frame");
        require(after == before, "Incomplete GPU-produced scene was ray traced");
      }
      std::printf("GPU-only fallback verified: mode=%u frames=3 visibleAddedPixels=%u exactNativePixels=1 unchangedTracedCounter=1\n", mode, changedPixels);
      std::fflush(stdout);
    }
  }
  void verifyNativeRaster(bool composite) {
    // Sample the actual game raster target after its draw, before the next RTX
    // injection. Rebinding the same API resources must work across RT frames.
    context->CopyResource(nativeStaging.Get(), backbuffer.Get());
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    checked(context->Map(nativeStaging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "Map native raster color");
    unsigned colored = 0;
    for (UINT y = 0; y < kHeight; ++y) {
      const auto* row = static_cast<const unsigned char*>(mapped.pData) + size_t(y) * mapped.RowPitch;
      for (UINT x = 0; x < kWidth; ++x) {
        const auto* pixel = row + x * 4;
        colored += composite
          ? (pixel[0] >= 50 && pixel[0] <= 52 && pixel[1] == 0 && pixel[2] >= 50 && pixel[2] <= 52)
          : (pixel[0] >= 94 && pixel[0] <= 242 && pixel[1] >= 158 && pixel[1] <= 178 && pixel[2] >= 94 && pixel[2] <= 226);
      }
    }
    context->Unmap(nativeStaging.Get(), 0);
    require(composite ? colored == kWidth * kHeight : (colored > 200 && colored < kWidth * kHeight * 3 / 4),
      "Native raster shader resources or framebuffer were not restored after RTX");
    std::printf("Native raster bindings verified: coloredPixels=%u composite=%u\n", colored, unsigned(composite));
    std::fflush(stdout);
  }
  ImageMetrics readback(const char* phase) {
    ImageMetrics result;
    apiChecked(api.table.dxvk_CopyRenderingOutput(output.Get(), REMIXAPI_DXVK_COPY_RENDERING_OUTPUT_TYPE_DEPTH), "Copy traced depth");
    context->CopyResource(staging.Get(), output.Get());
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    checked(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "Map traced depth");
    bool finite = true;
    unsigned borderHits = 0, nearPlaneHits = 0;
    const std::string filename = std::string(phase) + "-depth.pgm";
    FILE* file = nullptr; fopen_s(&file, filename.c_str(), "wb");
    if (file) std::fprintf(file, "P5\n%u %u\n255\n", kWidth, kHeight);
    for (UINT y = 0; y < kHeight; ++y) {
      const auto* row = reinterpret_cast<const float*>(static_cast<const unsigned char*>(mapped.pData) + size_t(y) * mapped.RowPitch);
      for (UINT x = 0; x < kWidth; ++x) {
        const float z = row[x * 4]; finite &= std::isfinite(z);
        const bool hit = z > 0 && z < 0.999f;
        if (hit) { ++result.hits; result.centroidX += x; }
        if (hit && (x < 8 || x >= kWidth - 8 || y < 8 || y >= kHeight - 8)) ++borderHits;
        if (hit && z < 0.9f) ++nearPlaneHits;
        if (file) std::fputc(hit ? 255 : 0, file);
      }
    }
    if (file) std::fclose(file);
    context->Unmap(staging.Get(), 0);
    require(finite, "Traced depth contains NaN or infinity");
    require(result.hits > 200 && result.hits < kWidth * kHeight * 3 / 4, "Captured geometry is missing or covers the camera");
    require(borderHits == 0 && nearPlaneHits == 0, "Unexpected camera enclosure or near-plane obstruction");
    result.centroidX /= result.hits;
    apiChecked(api.table.dxvk_CopyRenderingOutput(output.Get(), REMIXAPI_DXVK_COPY_RENDERING_OUTPUT_TYPE_FINAL_COLOR), "Copy traced color");
    context->CopyResource(staging.Get(), output.Get());
    checked(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "Map traced color");
    for (UINT y = 0; y < kHeight; ++y) {
      const auto* row = reinterpret_cast<const float*>(static_cast<const unsigned char*>(mapped.pData) + size_t(y) * mapped.RowPitch);
      for (UINT x = 0; x < kWidth * 4; ++x) finite &= std::isfinite(row[x]);
      for (UINT x = 0; x < kWidth; ++x) result.meanColor += row[x * 4] + row[x * 4 + 1] + row[x * 4 + 2];
    }
    context->Unmap(staging.Get(), 0);
    result.meanColor /= double(kWidth) * kHeight * 3;
    require(finite && result.meanColor > 0.0001, "Traced image is invalid or black");
    std::printf("%s: depth hits=%u centroidX=%.3f meanRGB=%.6f, all finite\n", phase, result.hits, result.centroidX, result.meanColor);
    std::fflush(stdout);
    return result;
  }
  ImageMetrics phase(const char* name, ID3D11Buffer* vertices, UINT firstInstance = 0,
                     bool instanced = false, UINT instanceCount = 1, bool divisor = false,
                     bool composite = false, bool placed = false) {
    std::printf("Beginning native capture phase %s\n", name); std::fflush(stdout);
    const ULONGLONG started = GetTickCount64();
    uint64_t before = completedFrames();
    UINT consecutive = 0, attempts = 0;
    while (consecutive < 16 && GetTickCount64() - started < 150000) {
      draw(vertices, firstInstance, instanced, instanceCount, divisor, composite, placed); ++attempts;
      const uint64_t after = completedFrames();
      consecutive = after > before ? consecutive + 1 : 0; before = after;
      Sleep(5);
    }
    require(consecutive == 16, "Native D3D11 geometry never reached complete ray-traced frames");
    std::printf("%s: 16 consecutive native ray-traced frames, %u attempts\n", name, attempts);
    draw(vertices, firstInstance, instanced, instanceCount, divisor, composite, placed, true);
    return readback(name);
  }
};
}

int wmain(int argc, wchar_t** argv) {
  gStarted=std::chrono::steady_clock::now();milestone("entry");
  if (argc == 2 && std::wstring(argv[1]) == L"--compile-shaders") {
    try {
      compile(kFixtureShader, "vsMain", "vs_5_0");
      compile(kFixtureShader, "vsInstanced", "vs_5_0");
      compile(kFixtureShader, "vsShaderPlacement", "vs_5_0");
      compile(kFixtureShader, "psMain", "ps_5_0");
      compile(kGpuDrawShader, "csArguments", "cs_5_0");
      compile(kGpuDrawShader, "vsStream", "vs_5_0");
      compile(kGpuDrawShader, "gsStream", "gs_5_0");
      std::puts("Native capture HLSL compiles successfully."); return 0;
    } catch (const std::exception&) { return 1; }
  }
  if (argc != 2) { std::fputs("Usage: test_remix_capture.exe ABSOLUTE_RUNTIME_DIRECTORY\n", stderr); return 2; }
  try {
    Watchdog watchdog; Window window; Renderer renderer;
    const std::wstring directory = argv[1];
    require(directory.size() > 2 && directory[1] == L':', "Runtime path must be absolute");
    SetDllDirectoryW(directory.c_str());
    SetEnvironmentVariableW(L"DXVK_REMIX_FORCE_CURRENT_PROCESS", L"1");
    SetEnvironmentVariableW(L"DXVK_ENABLE_RAYTRACING", L"1");
    HMODULE dxgi = LoadLibraryExW((directory + L"\\dxgi.dll").c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    HMODULE d3d11 = LoadLibraryExW((directory + L"\\d3d11.dll").c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    require(dxgi && d3d11, "Load built DXGI/D3D11");
    milestone("dll-load-complete");
    const auto createFactory = reinterpret_cast<HRESULT (WINAPI*)(REFIID, void**)>(GetProcAddress(dxgi, "CreateDXGIFactory1"));
    const auto createDevice = reinterpret_cast<PFN_D3D11_CREATE_DEVICE>(GetProcAddress(d3d11, "D3D11CreateDevice"));
    const auto initialize = reinterpret_cast<PFN_remixapi_InitializeLibrary>(GetProcAddress(d3d11, "remixapi_InitializeLibrary"));
    renderer.frameCounter = reinterpret_cast<FrameCounter>(GetProcAddress(d3d11, "remixapi_dxvk_GetCompletedRaytracedFrameCount"));
    require(createFactory && createDevice && initialize && renderer.frameCounter, "Runtime capture diagnostics exports");
    ComPtr<IDXGIFactory1> factory;
    checked(createFactory(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(factory.GetAddressOf())), "CreateDXGIFactory1");
    milestone("factory-created");
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT index = 0; ; ++index) {
      ComPtr<IDXGIAdapter1> candidate;
      if (factory->EnumAdapters1(index, candidate.GetAddressOf()) == DXGI_ERROR_NOT_FOUND) break;
      DXGI_ADAPTER_DESC1 desc = {}; checked(candidate->GetDesc1(&desc), "GetDesc1");
      if (desc.VendorId == 0x10de && !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) { adapter = candidate; break; }
    }
    require(adapter != nullptr, "No NVIDIA hardware adapter for native capture validation");
    milestone("adapter-selected");
    const D3D_FEATURE_LEVEL feature = D3D_FEATURE_LEVEL_11_0;
    checked(createDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, &feature, 1, D3D11_SDK_VERSION,
      renderer.device.GetAddressOf(), nullptr, renderer.context.GetAddressOf()), "D3D11CreateDevice");
    milestone("device-created");
    remixapi_InitializeLibraryInfo info = {}; info.sType = REMIXAPI_STRUCT_TYPE_INITIALIZE_LIBRARY_INFO;
    info.version = REMIXAPI_VERSION_MAKE(REMIXAPI_VERSION_MAJOR, REMIXAPI_VERSION_MINOR, REMIXAPI_VERSION_PATCH);
    apiChecked(initialize(&info, &renderer.api.table), "InitializeLibrary");
    apiChecked(renderer.api.table.dxvk_RegisterD3D11Device(renderer.device.Get()), "Register native device");
    renderer.api.registered = true;
    for (const auto& option : std::array<std::pair<const char*, const char*>, 12>{{
      { "rtx.graphicsPreset", "4" }, { "rtx.enableRaytracing", "True" }, { "rtx.useVertexCapture", "True" },
      { "rtx.enableRayReconstruction", "False" }, { "rtx.upscalerType", "0" }, { "rtx.integrateIndirectMode", "0" },
      { "rtx.renderPassGBufferRaytraceMode", "0" }, { "rtx.renderPassIntegrateDirectRaytraceMode", "0" },
      { "rtx.renderPassIntegrateIndirectRaytraceMode", "0" }, { "rtx.sceneScale", "0.01" },
      { "rtx.shader.enableAsyncCompilationUI", "False" },
      // Pixel oracles compare the application image across different times;
      // the independently animated welcome overlay is not part of that image.
      { "rtx.hideSplashMessage", "True" }
    }}) apiChecked(renderer.api.table.SetConfigVariable(option.first, option.second), option.first);
    const remixapi_Float4D clearDepth = { 1, 1, 1, 1 };
    apiChecked(renderer.api.table.dxvk_SetDefaultOutput(REMIXAPI_DXVK_COPY_RENDERING_OUTPUT_TYPE_DEPTH, &clearDepth), "Set depth clear");
    renderer.setup(factory.Get(), window.handle);
    remixapi_LightInfoSphereEXT sphere = {}; sphere.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_SPHERE_EXT;
    sphere.position = { 0, 3, -2 }; sphere.radius = 0.2f; sphere.volumetricRadianceScale = 1;
    remixapi_LightInfo light = {}; light.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO; light.pNext = &sphere;
    light.hash = 0x4341505455524531ull; light.radiance = { 200, 200, 200 };
    apiChecked(renderer.api.table.CreateLight(&light, &renderer.api.light), "Create fixture light");
    std::vector<Vertex> vertices(kVertexCapacity);
    fillDomain(vertices, kFirstDomain, 0);
    for (UINT y = 0; y < kSide; ++y) for (UINT x = 0; x < kSide; ++x)
      vertices[kFirstDomain + y * kSide + x].position[2] = (float(x % 3) - 1.0f) * 0.25f;
    renderer.write(renderer.dynamicVertices.Get(), vertices.data(), vertices.size() * sizeof(Vertex));
    renderer.matrices.view[14] = 0; renderer.write(renderer.constants.Get(), &renderer.matrices, sizeof(renderer.matrices));
    renderer.phase("shader-placed-origin-bounds", renderer.dynamicVertices.Get(), 0, false, 1, false, false, true);
    renderer.matrices.view[14] = 6; renderer.write(renderer.constants.Get(), &renderer.matrices, sizeof(renderer.matrices));
    fillDomain(vertices, kFirstDomain, -0.9f); fillDomain(vertices, kSecondDomain, -1.6f);
    renderer.write(renderer.dynamicVertices.Get(), vertices.data(), vertices.size() * sizeof(Vertex));
    const auto baseline = renderer.phase("dynamic-baseline", renderer.dynamicVertices.Get());
    fillDomain(vertices, kFirstDomain, 0.9f); fillDomain(vertices, kSecondDomain, -1.6f);
    renderer.write(renderer.dynamicVertices.Get(), vertices.data(), vertices.size() * sizeof(Vertex), D3D11_MAP_WRITE_NO_OVERWRITE);
    const auto dynamic = renderer.phase("dynamic-updated", renderer.dynamicVertices.Get());
    require(dynamic.centroidX > baseline.centroidX + 20, "Dynamic indexed source-span update was not captured");
    auto changedIndices = indicesFor(kSecondDomain);
    renderer.write(renderer.indices.Get(), changedIndices.data(), changedIndices.size() * sizeof(uint16_t));
    const auto indexed = renderer.phase("index-updated", renderer.dynamicVertices.Get());
    require(indexed.centroidX + 25 < dynamic.centroidX, "Changed index contents reused stale captured geometry");
    changedIndices = indicesFor(kFirstDomain);
    renderer.write(renderer.indices.Get(), changedIndices.data(), changedIndices.size() * sizeof(uint16_t));
    const auto gpuBaseline = renderer.phase("gpu-default-baseline", renderer.gpuVertices.Get());
    renderer.context->UpdateSubresource(renderer.gpuVertices.Get(), 0, nullptr, vertices.data(), 0, 0);
    const auto gpuChanged = renderer.phase("gpu-default-updated", renderer.gpuVertices.Get());
    require(gpuChanged.centroidX > gpuBaseline.centroidX + 20, "GPU DEFAULT buffer update was not recaptured");
    const auto single = renderer.phase("one-instance-zero", renderer.dynamicVertices.Get(), 0, true);
    const auto shifted = renderer.phase("one-instance-offset", renderer.dynamicVertices.Get(), 1, true);
    require(shifted.centroidX > single.centroidX + 15, "StartInstanceLocation was lost for a single instance");
    const auto multi = renderer.phase("divisor-instance-id", renderer.dynamicVertices.Get(), 0, true, 6, true);
    require(multi.hits > 800 && multi.centroidX > 115 && multi.centroidX < 185,
      "SV_InstanceID or divisor-based IA placement was lost");
    const auto layered = renderer.phase("fullscreen-composite", renderer.dynamicVertices.Get(), 0, false, 1, false, true);
    require(std::abs(layered.centroidX - single.centroidX) < 3
      && std::abs(double(layered.hits) - single.hits) < single.hits * 0.05,
      "Full-screen attachment composite entered the ray-traced scene");
    renderer.verifyGpuOnlyFallback();
    renderer.phase("gpu-fallback-recovery", renderer.dynamicVertices.Get());
    renderer.context->ClearState(); renderer.context->Flush();
    std::puts("All eleven native D3D11 capture phases and three GPU-only fallback checks passed.");
    milestone("workload-complete");
  } catch (const std::exception& error) {
    std::fprintf(stderr, "Native capture validation failed: %s\n", error.what()); return 1;
  }
  std::puts("Native capture API/device cleanup completed normally.");
  milestone("process-complete");
  return 0;
}
