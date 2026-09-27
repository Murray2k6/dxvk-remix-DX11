#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>
#include <algorithm>
#include <chrono>
#include <fstream>
#include <limits>
#include <psapi.h>
#include <wincodec.h>
#include "../../public/include/remix/remix_c.h"

namespace {
using Microsoft::WRL::ComPtr;
constexpr UINT kWidth = 641, kHeight = 361;


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
    wc.lpszClassName = L"RemixComplexSceneValidation";
    require(RegisterClassW(&wc) != 0, "RegisterClass");
    RECT area = { 0, 0, LONG(kWidth), LONG(kHeight) };
    AdjustWindowRect(&area, WS_OVERLAPPEDWINDOW, FALSE);
    handle = CreateWindowW(wc.lpszClassName, L"Remix complex native atrium validation", WS_OVERLAPPEDWINDOW,
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
  position.y += float(input.instanceId) * 0.01;
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
struct Vec { float x, y, z; };
Vec operator+(Vec a, Vec b) { return { a.x+b.x, a.y+b.y, a.z+b.z }; }
Vec operator-(Vec a, Vec b) { return { a.x-b.x, a.y-b.y, a.z-b.z }; }
Vec operator*(Vec a, float b) { return { a.x*b, a.y*b, a.z*b }; }
float dot(Vec a, Vec b) { return a.x*b.x+a.y*b.y+a.z*b.z; }
Vec cross(Vec a, Vec b) { return { a.y*b.z-a.z*b.y, a.z*b.x-a.x*b.z, a.x*b.y-a.y*b.x }; }
Vec unit(Vec a) { return a * (1.0f/std::sqrt(dot(a,a))); }
constexpr float kPi = 3.14159265359f, kNear = 0.1f, kFar = 150.0f;
UINT gTorusRings=160,gTorusSides=64;
bool gStress=false;
struct Bounds { Vec low, high; };
struct Mesh {
  std::vector<Vertex> vertices;
  std::vector<uint32_t> indices;
  void vertex(Vec p, Vec n, float u, float v) { vertices.push_back({ {p.x,p.y,p.z}, {n.x,n.y,n.z}, {u,v} }); }
  void quad(Vec a, Vec b, Vec c, Vec d, Vec normal) {
    const uint32_t first = uint32_t(vertices.size());
    vertex(a,normal,0,0); vertex(b,normal,0,2); vertex(c,normal,2,2); vertex(d,normal,2,0);
    for (uint32_t i : {0u,1u,2u,0u,2u,3u}) indices.push_back(first+i);
  }
  void box(Vec center, Vec size) {
    const Vec a=center-size*0.5f, b=center+size*0.5f;
    quad({a.x,a.y,a.z},{a.x,b.y,a.z},{b.x,b.y,a.z},{b.x,a.y,a.z},{0,0,-1});
    quad({b.x,a.y,b.z},{b.x,b.y,b.z},{a.x,b.y,b.z},{a.x,a.y,b.z},{0,0,1});
    quad({a.x,a.y,b.z},{a.x,b.y,b.z},{a.x,b.y,a.z},{a.x,a.y,a.z},{-1,0,0});
    quad({b.x,a.y,a.z},{b.x,b.y,a.z},{b.x,b.y,b.z},{b.x,a.y,b.z},{1,0,0});
    quad({a.x,b.y,a.z},{a.x,b.y,b.z},{b.x,b.y,b.z},{b.x,b.y,a.z},{0,1,0});
    quad({a.x,a.y,b.z},{a.x,a.y,a.z},{b.x,a.y,a.z},{b.x,a.y,b.z},{0,-1,0});
  }
  void cylinder(Vec center, float radius, float height, UINT segments=32) {
    for (UINT i=0;i<segments;++i) {
      const float a=2*kPi*float(i)/float(segments), b=2*kPi*float(i+1)/float(segments);
      const Vec na={std::cos(a),0,std::sin(a)}, nb={std::cos(b),0,std::sin(b)};
      const Vec pa=center+na*radius, pb=center+nb*radius;
      quad(pa+Vec{0,-height*.5f,0},pa+Vec{0,height*.5f,0},pb+Vec{0,height*.5f,0},pb+Vec{0,-height*.5f,0},unit(na+nb));
      const uint32_t first=uint32_t(vertices.size());
      vertex(center+Vec{0,height*.5f,0},{0,1,0},.5f,.5f);
      vertex(pa+Vec{0,height*.5f,0},{0,1,0},0,0); vertex(pb+Vec{0,height*.5f,0},{0,1,0},1,0);
      for (uint32_t n : {0u,1u,2u}) indices.push_back(first+n);
    }
  }
  void torus(Vec center, float major, float minor, float deformation=0,UINT explicitRings=0,UINT explicitSides=0) {
    const UINT rings=explicitRings?explicitRings:gTorusRings,sides=explicitSides?explicitSides:gTorusSides;
    const uint32_t first=uint32_t(vertices.size());
    for (UINT i=0;i<=rings;++i) for (UINT j=0;j<=sides;++j) {
      const float a=2*kPi*float(i)/float(rings), b=2*kPi*float(j)/float(sides);
      const float detail=.035f*std::sin(a*11)*std::sin(b*7);
      const float r=major+minor*std::cos(b)+deformation*std::sin(a*3)+detail;
      const Vec n={std::cos(a)*std::cos(b),std::sin(a)*std::cos(b),std::sin(b)};
      vertex(center+Vec{r*std::cos(a),r*std::sin(a),minor*std::sin(b)},n,float(i)/float(rings)*4,float(j)/float(sides));
    }
    for (UINT i=0;i<rings;++i) for (UINT j=0;j<sides;++j) {
      const uint32_t a=first+i*(sides+1)+j,b=a+sides+1;
      for (uint32_t index : {a,b,a+1,a+1,b,b+1}) indices.push_back(index);
    }
  }
};
struct Scene {
  std::array<Mesh,12> materials;
  Mesh deforming, moving, instanceMesh;
  std::vector<Vec> offsets;
  std::vector<Bounds> solidBounds;
  unsigned objects=0;
  void box(unsigned material, Vec center, Vec size) {
    materials[material].box(center,size); solidBounds.push_back({center-size*.5f,center+size*.5f}); ++objects;
  }
  Scene() {
    // An open atrium with side galleries and a clear central walkway. No test enclosure follows the camera.
    box(0,{0,-.35f,20},{24,.6f,44});
    for (int side : {-1,1}) {
      const float s=float(side);
      for (float height : {4.0f,8.0f}) {
        box(1,{s*9,height,23},{5,.3f,34});
        box(2,{s*6.6f,height+1,23},{.12f,.12f,34});
        for (UINT n=0;n<18;++n) box(2,{s*6.6f,height+.5f,7+float(n)*1.85f},{.08f,1,.08f});
      }
      for (UINT step=0;step<20;++step) {
        const float height=.2f*float(step+1);
        box(3,{s*9,height*.5f,2+float(step)*.35f},{3,height,.4f});
      }
      for (UINT n=0;n<7;++n) {
        const float z=8+float(n)*5;
        materials[4].cylinder({s*6,4.5f,z},.4f,9);
        solidBounds.push_back({{s*6-.4f,0,z-.4f},{s*6+.4f,9,z+.4f}}); ++objects;
        box(3,{s*6,.25f,z},{1.2f,.5f,1.2f}); box(3,{s*6,8.8f,z},{1.2f,.4f,1.2f});
        box(5,{s*11.8f,4.5f,z},{.4f,9,1.3f});
        box(6,{s*11.8f,8.7f,z+2.5f},{.4f,.6f,4});
      }
      box(1,{s*9,9.4f,23},{5,.3f,34});
    }
    for (float z : {12.0f,22.0f,32.0f}) box(6,{0,9,z},{24,.45f,.6f});
    for (int x=-10;x<=10;x+=4) { box(5,{float(x),4.5f,40},{1.2f,9,.5f}); box(6,{float(x),8.8f,40},{4,.4f,.6f}); }
    for (UINT n=0;n<48;++n) {
      const float s=(n%2)?1.0f:-1.0f, z=9+float(n/8)*4.5f;
      const float x=s*(3.8f+float((n/2)%4)*.42f), y=.4f+float((n/4)%2)*.82f;
      box(7+n%3,{x,y,z},{.8f,.8f,.85f});
    }
    // Staggered depth layers and curved silhouettes exercise occlusion and parallax.
    for (UINT n=0;n<12;++n) {
      const float s=(n%2)?1.0f:-1.0f;
      box(10,{s*(3.2f+float(n%3)*.25f),2.2f,12+float(n/2)*4},{.10f,2.4f,1.6f});
      materials[11].torus({s*4.5f,2.8f,10+float(n/2)*5},.65f,.15f); ++objects;
    }
    deforming.torus({0,2.4f,19},1.65f,.23f);
    moving.torus({-2,2.4f,26},1.0f,.3f);
    if(gStress) instanceMesh.torus({0,.6f,0},.3f,.09f,0,64,32);
    else instanceMesh.cylinder({0,.6f,0},.22f,1.2f,24);
    for (UINT n=0;n<32;++n) offsets.push_back({(n%2)?5.0f:-5.0f,4.2f,8+float(n/2)*1.8f});
    objects+=unsigned(offsets.size())+2;
  }
  void cameraClear(Vec eye,bool log) const {
    float closest=std::numeric_limits<float>::max();
    for (const auto& b : solidBounds) {
      const Vec d={std::max({b.low.x-eye.x,0.0f,eye.x-b.high.x}),std::max({b.low.y-eye.y,0.0f,eye.y-b.high.y}),std::max({b.low.z-eye.z,0.0f,eye.z-b.high.z})};
      closest=std::min(closest,std::sqrt(dot(d,d)));
    }
    require(closest>.35f,"Camera intersects architecture or near-plane clearance");
    if(log) std::printf("Camera solid clearance: %.3f world units\n",closest);
  }
};
void savePng(const std::string& name, const std::vector<unsigned char>& rgba) {
  ComPtr<IWICImagingFactory> factory;
  checked(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(factory.GetAddressOf())),"WIC factory");
  ComPtr<IWICStream> stream; checked(factory->CreateStream(stream.GetAddressOf()),"WIC stream");
  const std::wstring wide(name.begin(),name.end());
  checked(stream->InitializeFromFilename(wide.c_str(),GENERIC_WRITE),"PNG file");
  ComPtr<IWICBitmapEncoder> encoder; checked(factory->CreateEncoder(GUID_ContainerFormatPng,nullptr,encoder.GetAddressOf()),"PNG encoder");
  checked(encoder->Initialize(stream.Get(),WICBitmapEncoderNoCache),"PNG initialize");
  ComPtr<IWICBitmapFrameEncode> frame; checked(encoder->CreateNewFrame(frame.GetAddressOf(),nullptr),"PNG frame");
  checked(frame->Initialize(nullptr),"PNG frame init"); checked(frame->SetSize(kWidth,kHeight),"PNG size");
  WICPixelFormatGUID format=GUID_WICPixelFormat32bppRGBA; checked(frame->SetPixelFormat(&format),"PNG format");
  ComPtr<IWICBitmap> bitmap;checked(factory->CreateBitmapFromMemory(kWidth,kHeight,GUID_WICPixelFormat32bppRGBA,kWidth*4,UINT(rgba.size()),const_cast<BYTE*>(rgba.data()),bitmap.GetAddressOf()),"PNG source bitmap");
  ComPtr<IWICFormatConverter> converter;checked(factory->CreateFormatConverter(converter.GetAddressOf()),"PNG converter");
  checked(converter->Initialize(bitmap.Get(),format,WICBitmapDitherTypeNone,nullptr,0,WICBitmapPaletteTypeCustom),"PNG negotiated format conversion");
  checked(frame->WriteSource(converter.Get(),nullptr),"PNG pixels");
  checked(frame->Commit(),"PNG frame commit"); checked(encoder->Commit(),"PNG commit");
}
void saveDepth(const std::string& name, const std::vector<float>& depth) {
  std::ofstream file(name+".pfm",std::ios::binary); require(bool(file),"Create PFM");
  file<<"Pf\n"<<kWidth<<" "<<kHeight<<"\n-1.0\n";
  for (UINT y=kHeight;y>0;--y) file.write(reinterpret_cast<const char*>(depth.data()+size_t(y-1)*kWidth),kWidth*sizeof(float));
  std::vector<unsigned char> rgba(size_t(kWidth)*kHeight*4,255);
  for (size_t i=0;i<depth.size();++i) {
    const float z=depth[i], distance=kNear*kFar/(kFar-z*(kFar-kNear));
    const unsigned char value=(z>0&&z<.9999f)?static_cast<unsigned char>(255*std::clamp(1.0f-distance/60.0f,0.0f,1.0f)):0;
    rgba[i*4]=rgba[i*4+1]=rgba[i*4+2]=value;
  }
  savePng(name+".png",rgba);
}
struct Memory { uint64_t workingSet,privateBytes; };
Memory processMemory() {
  PROCESS_MEMORY_COUNTERS_EX memory={}; memory.cb=sizeof(memory);
  require(GetProcessMemoryInfo(GetCurrentProcess(),reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory),sizeof(memory))!=FALSE,"Process memory counters");
  return {uint64_t(memory.WorkingSetSize),uint64_t(memory.PrivateUsage)};
}
struct GpuMesh { ComPtr<ID3D11Buffer> vertices, indices; UINT indexCount=0; unsigned material=0; UINT instances=1; };
struct PhaseMetrics { std::string name; double meanMs=0,medianMs=0,p95Ms=0,depthAgreement=0,interiorAgreement=0,nativeRasterDifference=0,meanRgb=0; Memory memoryBefore={},memoryAfter={}; unsigned nativeHits=0,tracedHits=0,comparedSamples=0,interiorSamples=0,falseHits=0,falseMisses=0; };
struct Renderer {
  ComPtr<ID3D11Device> device; ComPtr<ID3D11DeviceContext> context; ComPtr<IDXGISwapChain> swapchain;
  ComPtr<ID3D11Texture2D> backbuffer,nativeReadback,depthImage,depthReadback,output,staging;
  ComPtr<ID3D11RenderTargetView> target; ComPtr<ID3D11DepthStencilView> depth;
  ComPtr<ID3D11VertexShader> vertexShader; ComPtr<ID3D11PixelShader> pixelShader; ComPtr<ID3D11InputLayout> layout;
  ComPtr<ID3D11Buffer> constants,zeroInstance,instanceOffsets;
  ComPtr<ID3D11SamplerState> sampler; ComPtr<ID3D11RasterizerState> rasterizer; ComPtr<ID3D11DepthStencilState> depthState;
  std::array<ComPtr<ID3D11ShaderResourceView>,12> textures;
  std::vector<GpuMesh> meshes; std::vector<remixapi_LightHandle> lights;
  Scene scene; Api api; FrameCounter frameCounter=nullptr; Matrices matrices={};
  std::vector<unsigned char> nativeColor; std::vector<float> nativeDepth, tracedDepth;
  uint64_t completedFrames() {
    const uint64_t frames=frameCounter(device.Get());
    if(frames&&!gFirstCompletedRt) {gFirstCompletedRt=true;milestone("first-completed-rt");}
    return frames;
  }
  ~Renderer() { if(context) {context->ClearState();context->Flush();} for (auto light : lights) api.table.DestroyLight(light); }
  ComPtr<ID3D11Buffer> buffer(UINT bytes,UINT bind,D3D11_USAGE usage,const void* data) {
    D3D11_BUFFER_DESC desc={}; desc.ByteWidth=bytes; desc.BindFlags=bind; desc.Usage=usage;
    desc.CPUAccessFlags=(usage==D3D11_USAGE_DYNAMIC)?D3D11_CPU_ACCESS_WRITE:0;
    D3D11_SUBRESOURCE_DATA init={};init.pSysMem=data; ComPtr<ID3D11Buffer> result;
    checked(device->CreateBuffer(&desc,data?&init:nullptr,result.GetAddressOf()),"Create scene buffer");return result;
  }
  void write(ID3D11Buffer* resource,const void* data,size_t size) {
    D3D11_MAPPED_SUBRESOURCE map={};checked(context->Map(resource,0,D3D11_MAP_WRITE_DISCARD,0,&map),"Map scene buffer");
    std::memcpy(map.pData,data,size);context->Unmap(resource,0);
  }
  void upload(const Mesh& mesh,unsigned material,D3D11_USAGE usage=D3D11_USAGE_IMMUTABLE,UINT count=1) {
    GpuMesh gpu; gpu.vertices=buffer(UINT(mesh.vertices.size()*sizeof(Vertex)),D3D11_BIND_VERTEX_BUFFER,usage,mesh.vertices.data());
    gpu.indices=buffer(UINT(mesh.indices.size()*sizeof(uint32_t)),D3D11_BIND_INDEX_BUFFER,D3D11_USAGE_IMMUTABLE,mesh.indices.data());
    gpu.indexCount=UINT(mesh.indices.size());gpu.material=material;gpu.instances=count;meshes.push_back(std::move(gpu));
  }
  void setup(IDXGIFactory1* factory,HWND window) {
    DXGI_SWAP_CHAIN_DESC sc={};sc.BufferDesc.Width=kWidth;sc.BufferDesc.Height=kHeight;sc.BufferDesc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
    sc.SampleDesc.Count=1;sc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;sc.BufferCount=2;sc.OutputWindow=window;sc.Windowed=TRUE;
    checked(factory->CreateSwapChain(device.Get(),&sc,swapchain.GetAddressOf()),"Create scene swapchain");
    checked(swapchain->GetBuffer(0,IID_PPV_ARGS(backbuffer.GetAddressOf())),"Scene backbuffer");
    checked(device->CreateRenderTargetView(backbuffer.Get(),nullptr,target.GetAddressOf()),"Scene RTV");
    D3D11_TEXTURE2D_DESC image={};backbuffer->GetDesc(&image);image.Usage=D3D11_USAGE_STAGING;image.BindFlags=0;image.CPUAccessFlags=D3D11_CPU_ACCESS_READ;image.MiscFlags=0;
    checked(device->CreateTexture2D(&image,nullptr,nativeReadback.GetAddressOf()),"Native color staging");
    image.Usage=D3D11_USAGE_DEFAULT;image.CPUAccessFlags=0;image.Format=DXGI_FORMAT_D32_FLOAT;image.BindFlags=D3D11_BIND_DEPTH_STENCIL;
    checked(device->CreateTexture2D(&image,nullptr,depthImage.GetAddressOf()),"Native depth");
    checked(device->CreateDepthStencilView(depthImage.Get(),nullptr,depth.GetAddressOf()),"Native DSV");
    image.Usage=D3D11_USAGE_STAGING;image.CPUAccessFlags=D3D11_CPU_ACCESS_READ;image.BindFlags=0;
    checked(device->CreateTexture2D(&image,nullptr,depthReadback.GetAddressOf()),"Native depth staging");
    image.Usage=D3D11_USAGE_DEFAULT;image.CPUAccessFlags=0;image.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;image.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;
    checked(device->CreateTexture2D(&image,nullptr,output.GetAddressOf()),"Traced output");
    image.Usage=D3D11_USAGE_STAGING;image.CPUAccessFlags=D3D11_CPU_ACCESS_READ;image.BindFlags=0;
    checked(device->CreateTexture2D(&image,nullptr,staging.GetAddressOf()),"Traced staging");
    const auto vs=compile(kFixtureShader,"vsMain","vs_5_0"),ps=compile(kFixtureShader,"psMain","ps_5_0");
    checked(device->CreateVertexShader(vs->GetBufferPointer(),vs->GetBufferSize(),nullptr,vertexShader.GetAddressOf()),"Scene VS");
    checked(device->CreatePixelShader(ps->GetBufferPointer(),ps->GetBufferSize(),nullptr,pixelShader.GetAddressOf()),"Scene PS");
    const D3D11_INPUT_ELEMENT_DESC elements[]={{"POSITION",0,DXGI_FORMAT_R32G32B32_FLOAT,0,0,D3D11_INPUT_PER_VERTEX_DATA,0},{"NORMAL",0,DXGI_FORMAT_R32G32B32_FLOAT,0,12,D3D11_INPUT_PER_VERTEX_DATA,0},{"TEXCOORD",0,DXGI_FORMAT_R32G32_FLOAT,0,24,D3D11_INPUT_PER_VERTEX_DATA,0},{"INSTANCE_OFFSET",0,DXGI_FORMAT_R32G32B32_FLOAT,1,0,D3D11_INPUT_PER_INSTANCE_DATA,1}};
    checked(device->CreateInputLayout(elements,UINT(std::size(elements)),vs->GetBufferPointer(),vs->GetBufferSize(),layout.GetAddressOf()),"Scene layout");
    const Vec zero={0,0,0};zeroInstance=buffer(sizeof(zero),D3D11_BIND_VERTEX_BUFFER,D3D11_USAGE_IMMUTABLE,&zero);
    instanceOffsets=buffer(UINT(scene.offsets.size()*sizeof(Vec)),D3D11_BIND_VERTEX_BUFFER,D3D11_USAGE_IMMUTABLE,scene.offsets.data());
    identity(matrices.world);identity(matrices.view);
    const float yScale=1/std::tan(kPi/6);matrices.projection[0]=yScale*float(kHeight)/kWidth;matrices.projection[5]=yScale;
    matrices.projection[10]=kFar/(kFar-kNear);matrices.projection[11]=1;matrices.projection[14]=-kNear*kFar/(kFar-kNear);
    constants=buffer(sizeof(matrices),D3D11_BIND_CONSTANT_BUFFER,D3D11_USAGE_DYNAMIC,&matrices);
    for (unsigned n=0;n<12;++n) {
      std::array<uint32_t,64*64> pixels={};
      const uint32_t colors[]={0xffbbb5a4,0xffafa3a0,0xff82776d,0xff7a8496,0xffd9d3c3,0xff748da0,0xff5c696b,0xff3b7bb5,0xffab745d,0xff67aa7d,0xff925098,0xff50b7d9};
      for (UINT y=0;y<64;++y) for (UINT x=0;x<64;++x) {
        const uint32_t c=colors[n];const bool seam=(x%16<2)||(y%16<2);uint32_t pixel=0xff000000;
        for (unsigned channel=0;channel<3;++channel) pixel|=(((c>>(channel*8))&255u)*(seam?3u:4u)/4u)<<(channel*8);
        pixels[y*64+x]=pixel;
      }
      image={};image.Width=image.Height=64;image.MipLevels=image.ArraySize=1;image.Format=DXGI_FORMAT_R8G8B8A8_UNORM;image.SampleDesc.Count=1;image.Usage=D3D11_USAGE_IMMUTABLE;image.BindFlags=D3D11_BIND_SHADER_RESOURCE;
      D3D11_SUBRESOURCE_DATA init={};init.pSysMem=pixels.data();init.SysMemPitch=64*4;ComPtr<ID3D11Texture2D> texture;
      checked(device->CreateTexture2D(&image,&init,texture.GetAddressOf()),"Scene material texture");
      checked(device->CreateShaderResourceView(texture.Get(),nullptr,textures[n].GetAddressOf()),"Scene material SRV");
      upload(scene.materials[n],n);
    }
    upload(scene.deforming,11,D3D11_USAGE_DYNAMIC);upload(scene.moving,8,D3D11_USAGE_DEFAULT);upload(scene.instanceMesh,2,D3D11_USAGE_IMMUTABLE,UINT(scene.offsets.size()));
    D3D11_SAMPLER_DESC sample={};sample.Filter=D3D11_FILTER_MIN_MAG_MIP_LINEAR;sample.AddressU=sample.AddressV=sample.AddressW=D3D11_TEXTURE_ADDRESS_WRAP;sample.MaxLOD=D3D11_FLOAT32_MAX;
    checked(device->CreateSamplerState(&sample,sampler.GetAddressOf()),"Scene sampler");
    D3D11_RASTERIZER_DESC raster={};raster.FillMode=D3D11_FILL_SOLID;raster.CullMode=D3D11_CULL_NONE;raster.DepthClipEnable=TRUE;
    checked(device->CreateRasterizerState(&raster,rasterizer.GetAddressOf()),"Scene rasterizer");
    D3D11_DEPTH_STENCIL_DESC z={};z.DepthEnable=TRUE;z.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ALL;z.DepthFunc=D3D11_COMPARISON_LESS;
    checked(device->CreateDepthStencilState(&z,depthState.GetAddressOf()),"Scene depth state");
    // These bindings deliberately remain untouched across native and RTX frames.
    context->VSSetShader(vertexShader.Get(),nullptr,0);context->PSSetShader(pixelShader.Get(),nullptr,0);
    ID3D11SamplerState* samplePtr=sampler.Get();context->PSSetSamplers(0,1,&samplePtr);
    ID3D11Buffer* cb=constants.Get();context->VSSetConstantBuffers(0,1,&cb);
    context->IASetInputLayout(layout.Get());context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    for (UINT n=0;n<4;++n) {
      remixapi_LightInfoSphereEXT sphere={};sphere.sType=REMIXAPI_STRUCT_TYPE_LIGHT_INFO_SPHERE_EXT;sphere.position={0,7,5+float(n)*10};sphere.radius=.3f;sphere.volumetricRadianceScale=1;
      remixapi_LightInfo light={};light.sType=REMIXAPI_STRUCT_TYPE_LIGHT_INFO;light.pNext=&sphere;light.hash=0x434f4d504c455800ull+n;light.radiance={800,700,600};remixapi_LightHandle handle=nullptr;
      apiChecked(api.table.CreateLight(&light,&handle),"Create atrium light");lights.push_back(handle);
    }
  }
  void camera(Vec eye,Vec targetPoint,bool log=true) {
    scene.cameraClear(eye,log);const Vec forward=unit(targetPoint-eye),right=unit(cross({0,1,0},forward)),up=cross(forward,right);
    identity(matrices.view);matrices.view[0]=right.x;matrices.view[4]=right.y;matrices.view[8]=right.z;matrices.view[12]=-dot(eye,right);
    matrices.view[1]=up.x;matrices.view[5]=up.y;matrices.view[9]=up.z;matrices.view[13]=-dot(eye,up);
    matrices.view[2]=forward.x;matrices.view[6]=forward.y;matrices.view[10]=forward.z;matrices.view[14]=-dot(eye,forward);
    write(constants.Get(),&matrices,sizeof(matrices));
  }
  void animate(float amount) {
    Mesh a,b;a.torus({amount*1.2f,2.4f,19},1.65f,.23f,amount*.25f);b.torus({-2+amount*3,2.4f,26},1,.3f);
    write(meshes[12].vertices.Get(),a.vertices.data(),a.vertices.size()*sizeof(Vertex));
    context->UpdateSubresource(meshes[13].vertices.Get(),0,nullptr,b.vertices.data(),0,0);
  }
  void captureNative() {
    context->CopyResource(nativeReadback.Get(),backbuffer.Get());context->CopyResource(depthReadback.Get(),depthImage.Get());
    D3D11_MAPPED_SUBRESOURCE map={};checked(context->Map(nativeReadback.Get(),0,D3D11_MAP_READ,0,&map),"Map native scene color");
    nativeColor.resize(size_t(kWidth)*kHeight*4);
    for (UINT y=0;y<kHeight;++y) std::memcpy(nativeColor.data()+size_t(y)*kWidth*4,static_cast<const char*>(map.pData)+size_t(y)*map.RowPitch,kWidth*4);
    context->Unmap(nativeReadback.Get(),0);checked(context->Map(depthReadback.Get(),0,D3D11_MAP_READ,0,&map),"Map native scene depth");
    nativeDepth.resize(size_t(kWidth)*kHeight);
    for (UINT y=0;y<kHeight;++y) std::memcpy(nativeDepth.data()+size_t(y)*kWidth,static_cast<const char*>(map.pData)+size_t(y)*map.RowPitch,kWidth*sizeof(float));
    context->Unmap(depthReadback.Get(),0);
  }
  void draw(bool capture=false) {
    MSG message;while (PeekMessageW(&message,nullptr,0,0,PM_REMOVE)) {TranslateMessage(&message);DispatchMessageW(&message);}
    ID3D11RenderTargetView* rtv=target.Get();context->OMSetRenderTargets(1,&rtv,depth.Get());context->OMSetDepthStencilState(depthState.Get(),0);
    const float clear[]={.025f,.035f,.055f,1};context->ClearRenderTargetView(target.Get(),clear);context->ClearDepthStencilView(depth.Get(),D3D11_CLEAR_DEPTH,1,0);
    const D3D11_VIEWPORT viewport={0,0,float(kWidth),float(kHeight),0,1};context->RSSetViewports(1,&viewport);context->RSSetState(rasterizer.Get());
    for (const auto& mesh : meshes) {
      ID3D11Buffer* streams[]={mesh.vertices.Get(),mesh.instances>1?instanceOffsets.Get():zeroInstance.Get()};const UINT strides[]={sizeof(Vertex),sizeof(Vec)},offsets[]={0,0};
      context->IASetVertexBuffers(0,2,streams,strides,offsets);context->IASetIndexBuffer(mesh.indices.Get(),DXGI_FORMAT_R32_UINT,0);
      ID3D11ShaderResourceView* texture=textures[mesh.material].Get();context->PSSetShaderResources(0,1,&texture);
      if (mesh.instances>1) context->DrawIndexedInstanced(mesh.indexCount,mesh.instances,0,0,0);
      else if (&mesh < meshes.data()+12) {
        // Range batching retains the same shader, texture, sampler and VB. The
        // stress tier deliberately includes draws above the historical 512k
        // capture-vertex limit; no production budget is overridden by the test.
        const UINT parts=3u,span=(mesh.indexCount/(parts*3))*3;
        for(UINT part=0;part<parts;++part) context->DrawIndexed(part+1<parts?span:mesh.indexCount-span*part,span*part,0);
      } else context->DrawIndexed(mesh.indexCount,0,0);
    }
    if (capture) captureNative();
    for (auto light : lights) apiChecked(api.table.DrawLightInstance(light),"Draw atrium light");
    checked(swapchain->Present(0,0),"Native scene Present");checked(device->GetDeviceRemovedReason(),"Scene device health");
    if(!gFirstNativePresent) {gFirstNativePresent=true;milestone("first-native-present");}
  }
  std::vector<float> readOutput(remixapi_dxvk_CopyRenderingOutputType type) {
    apiChecked(api.table.dxvk_CopyRenderingOutput(output.Get(),type),"Copy traced scene output");context->CopyResource(staging.Get(),output.Get());
    D3D11_MAPPED_SUBRESOURCE map={};checked(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&map),"Map traced scene output");
    std::vector<float> pixels(size_t(kWidth)*kHeight*4);
    for (UINT y=0;y<kHeight;++y) std::memcpy(pixels.data()+size_t(y)*kWidth*4,static_cast<const char*>(map.pData)+size_t(y)*map.RowPitch,kWidth*4*sizeof(float));
    context->Unmap(staging.Get(),0);for (float value : pixels) require(std::isfinite(value),"Nonfinite traced scene pixel");return pixels;
  }
  void validateImage(PhaseMetrics& result) {
    const auto depthPixels=readOutput(REMIXAPI_DXVK_COPY_RENDERING_OUTPUT_TYPE_DEPTH);tracedDepth.resize(nativeDepth.size());
    unsigned agreed=0,interiorMatches=0,phantomNear=0;
    for (UINT y=0;y<kHeight;++y) for (UINT x=0;x<kWidth;++x) {
      const size_t p=size_t(y)*kWidth+x;const float z=depthPixels[p*4];tracedDepth[p]=z;
      const bool traced=z>0&&z<.9999f,native=nativeDepth[p]>0&&nativeDepth[p]<.9999f;
      require(std::isfinite(nativeDepth[p]),"Native depth contains nonfinite values");
      result.tracedHits+=traced;result.nativeHits+=native;result.falseHits+=traced&&!native;result.falseMisses+=native&&!traced;
      if (!traced&&!native) continue;++result.comparedSamples;
      bool matches=false,interior=native&&x>0&&y>0&&x+1<kWidth&&y+1<kHeight;
      const float centerDistance=kNear*kFar/(kFar-nativeDepth[p]*(kFar-kNear));
      const float tracedDistance=kNear*kFar/(kFar-z*(kFar-kNear));float nearestNative=kFar;
      for (int dy=-1;dy<=1;++dy) for (int dx=-1;dx<=1;++dx) {
        const int px=int(x)+dx,py=int(y)+dy;if (px<0||py<0||px>=int(kWidth)||py>=int(kHeight)) continue;
        const float n=nativeDepth[size_t(py)*kWidth+UINT(px)];const bool hit=n>0&&n<.9999f;
        if (!traced&&!hit) matches=true;
        const float distance=kNear*kFar/(kFar-n*(kFar-kNear));
        if(hit) nearestNative=std::min(nearestNative,distance);
        if(!hit||std::abs(distance-centerDistance)>std::max(.03f,centerDistance*.01f)) interior=false;
        if(traced&&hit&&std::abs(tracedDistance-distance)<std::max(.02f,distance*.005f)) matches=true;
      }
      if(interior) {++result.interiorSamples;interiorMatches+=traced&&std::abs(tracedDistance-centerDistance)<std::max(.02f,centerDistance*.005f);}
      agreed+=matches;if (traced&&tracedDistance<.8f&&nearestNative>tracedDistance+.25f) ++phantomNear;
    }
    result.depthAgreement=double(agreed)/std::max(result.comparedSamples,1u);result.interiorAgreement=double(interiorMatches)/std::max(result.interiorSamples,1u);
    savePng(result.name+"-native-color.png",nativeColor);saveDepth(result.name+"-native-depth",nativeDepth);saveDepth(result.name+"-traced-depth",tracedDepth);
    const auto color=readOutput(REMIXAPI_DXVK_COPY_RENDERING_OUTPUT_TYPE_FINAL_COLOR);std::vector<unsigned char> rgba(color.size(),255);
    for (size_t p=0;p<color.size()/4;++p) for (unsigned channel=0;channel<3;++channel) {const float value=color[p*4+channel];result.meanRgb+=value;rgba[p*4+channel]=static_cast<unsigned char>(255*std::pow(std::clamp(value,0.0f,1.0f),1.0f/2.2f));}
    result.meanRgb/=double(kWidth)*kHeight*3;savePng(result.name+"-traced-color.png",rgba);
    std::printf("%s: nativeHits=%u tracedHits=%u compared=%u depthAgreement=%.6f interiorSamples=%u interiorAgreement=%.6f falseHits=%u falseMisses=%u nativeRasterDifference=%.8f meanRGB=%.6f meanFrameMs=%.3f p95FrameMs=%.3f\n",result.name.c_str(),result.nativeHits,result.tracedHits,result.comparedSamples,result.depthAgreement,result.interiorSamples,result.interiorAgreement,result.falseHits,result.falseMisses,result.nativeRasterDifference,result.meanRgb,result.meanMs,result.p95Ms);std::fflush(stdout);
    require(result.nativeHits>kWidth*kHeight/5&&result.tracedHits>kWidth*kHeight/5,"Complex scene coverage missing");
    require(result.depthAgreement>.98&&result.interiorSamples>10000&&result.interiorAgreement>.99&&phantomNear==0,"Traced scene disagrees with native 3D depth or contains camera obstruction");
    require(result.meanRgb>.0001,"Complex traced scene is black");  }
  PhaseMetrics phase(const char* name,Vec eye,Vec targetPoint,float animation) {
    std::printf("Beginning complex phase %s\n",name);std::fflush(stdout);camera(eye,targetPoint);animate(animation);
    uint64_t before=completedFrames();UINT consecutive=0;const ULONGLONG start=GetTickCount64();
    while (consecutive<24&&GetTickCount64()-start<180000) {draw();const uint64_t after=completedFrames();consecutive=after>before?consecutive+1:0;before=after;Sleep(2);}
    require(consecutive==24,"Complex native scene failed to reach completed RT frames");
    draw(true);const auto reference=nativeColor;
    const uint64_t referenceFrame=completedFrames();require(referenceFrame>before,"Reference raster probe lost RT");before=referenceFrame;
    PhaseMetrics result;result.name=name;result.memoryBefore=processMemory();std::vector<double> times;
    for (UINT frame=0;frame<32;++frame) {
      const auto begun=std::chrono::steady_clock::now();draw();const uint64_t after=completedFrames();
      require(after>before,"Complex scene lost ray tracing after warmup");before=after;
      times.push_back(std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-begun).count());
    }
    result.memoryAfter=processMemory();for (double ms : times) result.meanMs+=ms/32;std::sort(times.begin(),times.end());result.medianMs=(times[15]+times[16])*.5;result.p95Ms=times[30];
    draw(true);require(completedFrames()>before,"Final native probe lost RT");
    for (size_t i=0;i<reference.size();++i) result.nativeRasterDifference+=std::abs(int(reference[i])-int(nativeColor[i]));
    result.nativeRasterDifference/=double(reference.size())*255;
    require(result.nativeRasterDifference<.0001,"Unchanged native shader bindings produced different raster pixels after RTX");
    validateImage(result);
    return result;
  }
  std::vector<PhaseMetrics> walk(bool snapshots,std::vector<double>* elapsed=nullptr) {
    std::vector<PhaseMetrics> results;uint64_t before=completedFrames();
    for(UINT frame=0;frame<96;++frame) {
      const auto begun=std::chrono::steady_clock::now();const float t=float(frame)/95;
      camera({.65f*std::sin(t*2*kPi),2.2f,-3+12*t},{1.5f*std::sin(t*kPi),3,25},false);
      animate(.5f+.5f*std::sin(t*4*kPi));
      const bool probe=snapshots&&(frame==31||frame==63||frame==95);
      // Readback frames are not included in timing statistics.
      draw(probe);const uint64_t after=completedFrames();require(after>before,"Continuous camera or geometry update lost RT");before=after;
      if(elapsed&&!probe) elapsed->push_back(std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-begun).count());
      if(probe) {PhaseMetrics metrics;metrics.name="continuous-"+std::to_string(frame+1);validateImage(metrics);results.push_back(metrics);}
    }
    std::puts(snapshots?"Continuous96-frame camera and dynamic geometry sweep passed.":"Continuous96-frame camera and geometry warm/repeat sweep completed.");return results;
  }
  PhaseMetrics budgetFallback() {
    // Disable capture admission before drawing the oracle: native rendering
    // can still populate capture caches while final RTX injection is disabled.
    apiChecked(api.table.SetConfigVariable("rtx.dx11.captureMaxMiBPerFrame","1"),"Set explicit low capture budget");
    apiChecked(api.table.SetConfigVariable("rtx.enableRaytracing","False"),"Disable RT for native fallback oracle");
    camera({.3f,2.2f,3.5f},{-.5f,3,24});animate(.73f);draw(true);
    const auto reference=nativeColor;savePng("budget-fallback-native-reference.png",reference);
    const uint64_t before=completedFrames();
    apiChecked(api.table.SetConfigVariable("rtx.enableRaytracing","True"),"Enable RT with constrained capture");
    for(UINT frame=0;frame<3;++frame) {
      draw();require(completedFrames()==before,"Incomplete capture budget frame advanced traced counter");
      captureNative();double difference=0;size_t changedBytes=0;int maxDelta=0;
      for(size_t i=0;i<reference.size();++i) {
        const int delta=std::abs(int(reference[i])-int(nativeColor[i]));
        difference+=delta;changedBytes+=delta!=0;maxDelta=std::max(maxDelta,delta);
      }
      difference/=double(reference.size())*255;
      std::printf("Budget fallback frame%u: nativeDifference=%.9f changedBytes=%zu maxDelta=%d\n",frame,difference,changedBytes,maxDelta);
      if(difference>=.0001) savePng("budget-fallback-native-mismatch.png",nativeColor);
      require(difference<.0001,"Budget fallback did not preserve complete native scene");
    }
    savePng("budget-fallback-native-presented.png",nativeColor);
    std::puts("Budget fallback:3 complete native frames, traced counter unchanged, native pixels identical.");
    // This restores the production default; all earlier workload phases used
    // unchanged defaults, including oversized and multi-instance captures.
    apiChecked(api.table.SetConfigVariable("rtx.dx11.captureMaxMiBPerFrame","96"),"Restore default capture budget");
    return phase("budget-recovery",{.3f,2.2f,3.5f},{-.5f,3,24},.73f);
  }
};
}


int wmain(int argc,wchar_t** argv) {
  gStarted=std::chrono::steady_clock::now();milestone("entry");
  if (argc==2&&std::wstring(argv[1])==L"--compile-shaders") {
    try {compile(kFixtureShader,"vsMain","vs_5_0");compile(kFixtureShader,"psMain","ps_5_0");std::puts("Complex scene HLSL compilation passed.");return 0;}
    catch(const std::exception& error) {std::fprintf(stderr,"%s\n",error.what());return 1;}
  }
  if(argc<2||argc>3) {std::fputs("Usage: test_remix_complex_scene.exe ABSOLUTE_RUNTIME_DIRECTORY [regular|stress]\n",stderr);return 2;}
  if(argc==3) {const std::wstring tier=argv[2];if(tier!=L"regular"&&tier!=L"stress") return 2;gStress=tier==L"stress";}
  if(gStress) {gTorusRings=320;gTorusSides=128;}
  if(std::wstring(argv[1])==L"--scene-stats") {
    const Scene scene;uint64_t triangles=0,vertices=0;
    for(const auto& mesh:scene.materials) {triangles+=mesh.indices.size()/3;vertices+=mesh.vertices.size();}
    triangles+=(scene.deforming.indices.size()+scene.moving.indices.size())/3+scene.instanceMesh.indices.size()/3*scene.offsets.size();
    vertices+=scene.deforming.vertices.size()+scene.moving.vertices.size()+scene.instanceMesh.vertices.size();
    std::printf("tier=%s objects=%u triangles=%llu uniqueVertices=%llu largestNativeDrawIndices=%llu instancedTriangles=%llu instancedCapturedVertices=%llu\n",gStress?"stress":"regular",scene.objects,static_cast<unsigned long long>(triangles),static_cast<unsigned long long>(vertices),static_cast<unsigned long long>(scene.materials[11].indices.size()/3),static_cast<unsigned long long>(scene.instanceMesh.indices.size()/3),static_cast<unsigned long long>(scene.instanceMesh.indices.size()*scene.offsets.size()));return 0;
  }
  const HRESULT comResult=CoInitializeEx(nullptr,COINIT_MULTITHREADED);
  if (FAILED(comResult)) return 3;  try {
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
    // Present-time splash animation is unrelated to native scene preservation.
    // Disable it explicitly so fast startup does not alter the pixel oracle.
    for (const auto& option : std::array<std::pair<const char*, const char*>, 15>{{
      { "rtx.hideSplashMessage", "True" },
      { "rtx.graphicsPreset", "4" }, { "rtx.enableRaytracing", "True" }, { "rtx.useVertexCapture", "True" },
      { "rtx.enableRayReconstruction", "False" }, { "rtx.upscalerType", "0" }, { "rtx.integrateIndirectMode", "0" },
      { "rtx.renderPassGBufferRaytraceMode", "0" }, { "rtx.renderPassIntegrateDirectRaytraceMode", "0" },
      { "rtx.renderPassIntegrateIndirectRaytraceMode", "0" }, { "rtx.sceneScale", "0.01" },
      { "rtx.shader.enableAsyncCompilationUI", "False" },
      { "rtx.forceCameraJitter", "False" }, { "rtx.enableNearPlaneOverride", "False" },
      { "rtx.useRTXDI", "True" }
    }}) apiChecked(renderer.api.table.SetConfigVariable(option.first, option.second), option.first);
    const remixapi_Float4D clearDepth = { 1, 1, 1, 1 };
    apiChecked(renderer.api.table.dxvk_SetDefaultOutput(REMIXAPI_DXVK_COPY_RENDERING_OUTPUT_TYPE_DEPTH, &clearDepth), "Set depth clear");
    renderer.setup(factory.Get(), window.handle);
    struct View { const char* name;Vec eye,target;float animation;const char* mode; };
    const std::array<View,7> views={{{"entrance",{0,2,-3},{0,3,24},0,"0"},
      {"moving-geometry",{0,2,-3},{0,3,24},1,"0"},
      {"parallax-right",{2,2.4f,7},{-1,3,25},1,"0"},
      {"upper-gallery",{-8,6,19},{0,3,24},1,"0"},
      {"reverse-aisle",{0,2.2f,32},{0,3,8},1,"0"},
      {"sharc",{0,2,-3},{0,3,24},1,"3"},
      {"restir-gi",{0,2,-3},{0,3,24},1,"1"}}};
    uint64_t triangles=0,uniqueVertices=0;
    for(const auto& mesh:renderer.meshes) {triangles+=uint64_t(mesh.indexCount/3)*mesh.instances;}
    for(const auto& mesh:renderer.scene.materials) uniqueVertices+=mesh.vertices.size();
    uniqueVertices+=renderer.scene.deforming.vertices.size()+renderer.scene.moving.vertices.size()+renderer.scene.instanceMesh.vertices.size();
    std::printf("Complex scene: objects=%u triangles=%llu uniqueVertices=%llu nativeDraws=%u materials=12 instances=32 cameras=4 phases=7 modes=3\n",renderer.scene.objects,static_cast<unsigned long long>(triangles),static_cast<unsigned long long>(uniqueVertices),39u);std::fflush(stdout);
    // Warm every material, camera and dynamic-update path before memory baseline.
    for(const auto& view:views) {
      apiChecked(renderer.api.table.SetConfigVariable("rtx.integrateIndirectMode",view.mode),"Set complex warmup mode");
      renderer.camera(view.eye,view.target);renderer.animate(view.animation);
      uint64_t before=renderer.completedFrames();UINT consecutive=0;const ULONGLONG start=GetTickCount64();
      while(consecutive<24&&GetTickCount64()-start<180000) {renderer.draw();const uint64_t after=renderer.completedFrames();consecutive=after>before?consecutive+1:0;before=after;Sleep(2);}
      require(consecutive==24,"Complex all-path warmup failed");
    }
    apiChecked(renderer.api.table.SetConfigVariable("rtx.integrateIndirectMode","0"),"Set continuous warmup mode");renderer.walk(false);
    const Memory warmed=processMemory();std::vector<PhaseMetrics> metrics;std::vector<float> entranceDepth;
    for(const auto& view:views) {
      apiChecked(renderer.api.table.SetConfigVariable("rtx.integrateIndirectMode",view.mode),"Set complex measured mode");
      metrics.push_back(renderer.phase(view.name,view.eye,view.target,view.animation));
      if(entranceDepth.empty()) entranceDepth=renderer.tracedDepth;
      else if(std::string(view.name)=="moving-geometry") {
        unsigned changed=0;for(size_t i=0;i<entranceDepth.size();++i) changed+=std::abs(entranceDepth[i]-renderer.tracedDepth[i])>.0001f;
        std::printf("Fixed-camera dynamic depth changed pixels: %u\n",changed);require(changed>150,"Dynamic objects did not change traced depth at fixed camera");
      }
    }
    apiChecked(renderer.api.table.SetConfigVariable("rtx.integrateIndirectMode","0"),"Set continuous measured mode");
    std::vector<double> walkTimes;const auto walkMetrics=renderer.walk(true,&walkTimes);std::sort(walkTimes.begin(),walkTimes.end());
    const Memory afterFirst=processMemory();
    // Replay the same complete view/update sequence after artifacts and caches have
    // been allocated. Observe growth without treating noisy working-set data as FPS.
    for(const auto& view:views) {
      apiChecked(renderer.api.table.SetConfigVariable("rtx.integrateIndirectMode",view.mode),"Set complex repeated mode");
      renderer.camera(view.eye,view.target);renderer.animate(view.animation);
      uint64_t before=renderer.completedFrames();
      for(UINT frame=0;frame<24;++frame) {renderer.draw();const uint64_t after=renderer.completedFrames();require(after>before,"Repeat camera path lost RT");before=after;}
    }
    apiChecked(renderer.api.table.SetConfigVariable("rtx.integrateIndirectMode","0"),"Set continuous repeat mode");renderer.walk(false);
    const Memory afterRepeat=processMemory();
    const auto budgetRecovery=renderer.budgetFallback();
    std::ofstream report("scene-metrics.json");require(bool(report),"Create scene metrics");
    report<<"{\n  \"tier\": \""<<(gStress?"stress":"regular")<<"\",\n  \"objects\": "<<renderer.scene.objects<<",\n  \"trianglesIncludingInstances\": "<<triangles<<",\n  \"uniqueVertices\": "<<uniqueVertices<<",\n  \"materials\": 12,\n  \"nativeDrawsPerFrame\": "<<(39u)<<",\n  \"hardwareInstances\": 32,\n  \"cameras\": 4,\n  \"measurement\": \"Validation-enabled synthetic scene; CPU wall time of Draw/Present plus CS completion counter; excludes image readback/writes; not GPU timings or game FPS\",\n";
    report<<"  \"workingSetAfterWarmup\": "<<warmed.workingSet<<",\n  \"privateBytesAfterWarmup\": "<<warmed.privateBytes<<",\n  \"workingSetAfterFirstPath\": "<<afterFirst.workingSet<<",\n  \"privateBytesAfterFirstPath\": "<<afterFirst.privateBytes<<",\n  \"workingSetAfterRepeatPath\": "<<afterRepeat.workingSet<<",\n  \"privateBytesAfterRepeatPath\": "<<afterRepeat.privateBytes<<",\n  \"repeatWorkingSetDelta\": "<<int64_t(afterRepeat.workingSet)-int64_t(afterFirst.workingSet)<<",\n  \"repeatPrivateBytesDelta\": "<<int64_t(afterRepeat.privateBytes)-int64_t(afterFirst.privateBytes)<<",\n  \"phases\": [\n";
    for(size_t i=0;i<metrics.size();++i) {const auto& m=metrics[i];report<<"    {\"name\": \""<<m.name<<"\", \"timedFrames\": 32, \"meanFrameMs\": "<<m.meanMs<<", \"medianFrameMs\": "<<m.medianMs<<", \"p95FrameMs\": "<<m.p95Ms<<", \"nativeDepthHits\": "<<m.nativeHits<<", \"tracedDepthHits\": "<<m.tracedHits<<", \"depthAgreement\": "<<m.depthAgreement<<", \"comparedSamples\": "<<m.comparedSamples<<", \"interiorSamples\": "<<m.interiorSamples<<", \"interiorAgreement\": "<<m.interiorAgreement<<", \"falseHitPixels\": "<<m.falseHits<<", \"falseMissPixels\": "<<m.falseMisses<<", \"nativeRasterDifference\": "<<m.nativeRasterDifference<<", \"meanRgb\": "<<m.meanRgb<<", \"workingSetBefore\": "<<m.memoryBefore.workingSet<<", \"workingSetAfter\": "<<m.memoryAfter.workingSet<<", \"privateBytesBefore\": "<<m.memoryBefore.privateBytes<<", \"privateBytesAfter\": "<<m.memoryAfter.privateBytes<<"}"<<(i+1<metrics.size()?",":"")<<"\n";}
    report<<"  ],\n  \"continuousFrames\": 96,\n  \"continuousTimedFrames\": "<<walkTimes.size()<<",\n  \"continuousMedianFrameMs\": "<<walkTimes[walkTimes.size()/2]<<",\n  \"continuousP95FrameMs\": "<<walkTimes[walkTimes.size()*95/100]<<",\n  \"continuousSnapshots\": [\n";
    for(size_t i=0;i<walkMetrics.size();++i) {const auto& m=walkMetrics[i];report<<"    {\"name\": \""<<m.name<<"\", \"nativeDepthHits\": "<<m.nativeHits<<", \"tracedDepthHits\": "<<m.tracedHits<<", \"depthAgreement\": "<<m.depthAgreement<<", \"interiorSamples\": "<<m.interiorSamples<<", \"interiorAgreement\": "<<m.interiorAgreement<<", \"comparedSamples\": "<<m.comparedSamples<<", \"falseHitPixels\": "<<m.falseHits<<", \"falseMissPixels\": "<<m.falseMisses<<"}"<<(i+1<walkMetrics.size()?",":"")<<"\n";}
    report<<"  ],\n  \"instancedChunkCoverage\": "<<(gStress?"true":"false")<<",\n  \"instancedSourceTriangles\": "<<renderer.scene.instanceMesh.indices.size()/3<<",\n  \"instancedCapturedVertices\": "<<renderer.scene.instanceMesh.indices.size()*renderer.scene.offsets.size()<<",\n  \"largestNativeDrawIndices\": "<<renderer.scene.materials[11].indices.size()/3<<",\n  \"budgetFallbackVerified\": true,\n  \"budgetFallbackFrames\": 3,\n  \"budgetRecovery\": {\"name\": \""<<budgetRecovery.name<<"\", \"depthAgreement\": "<<budgetRecovery.depthAgreement<<", \"interiorAgreement\": "<<budgetRecovery.interiorAgreement<<", \"nativeRasterDifference\": "<<budgetRecovery.nativeRasterDifference<<"}\n}\n";report.close();
    renderer.context->ClearState();renderer.context->Flush();
    std::puts("All seven complex native 3D scene phases and repeated camera path passed.");
    milestone("workload-complete");
  } catch(const std::exception& error) {std::fprintf(stderr,"Complex scene failed: %s\n",error.what());CoUninitialize();return 1;}
  CoUninitialize();std::puts("Complex scene cleanup completed normally.");milestone("process-complete");return 0;
}







