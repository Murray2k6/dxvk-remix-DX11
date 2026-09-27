#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include "../../bridge_dx11_work/src/client_dx11/dx11_resource_lifetime.h"
#include <cstdio>
#include <cstdlib>
#include <unordered_map>
#include <vector>

static std::unordered_map<ID3D11DeviceChild*, std::vector<unsigned char>> shadows;
static unsigned releases = 0;
static void retire(ID3D11DeviceChild* resource) { shadows.erase(resource); ++releases; }
static void require(bool condition, const char* message) {
  if (!condition) { std::fprintf(stderr, "%s\n", message); std::exit(1); }
}
static void attach(ID3D11DeviceChild* resource) {
  require(dx11_capture::ResourceLifetime::attach<ID3D11DeviceChild, &retire>(resource), "native private-data attachment failed");
  shadows[resource] = std::vector<unsigned char>(65536, 0x7f);
}

int main() {
  // Load only system D3D11. WARP exercises real native COM lifetimes without
  // the bridge/server or a hardware GPU workload.
  HMODULE library = LoadLibraryExW(L"d3d11.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
  require(library != nullptr, "load system D3D11");
  const auto create = reinterpret_cast<decltype(&D3D11CreateDevice)>(GetProcAddress(library, "D3D11CreateDevice"));
  require(create != nullptr, "find native device export");
  ID3D11Device* device = nullptr;
  ID3D11DeviceContext* context = nullptr;
  const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
  require(SUCCEEDED(create(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, &level, 1,
    D3D11_SDK_VERSION, &device, nullptr, &context)), "create native WARP device");
  require(!dx11_capture::ResourceLifetime::attach<ID3D11DeviceChild, &retire>(nullptr), "null resource accepted");

  for (unsigned iteration = 0; iteration < 10000; ++iteration) {
    D3D11_BUFFER_DESC desc = {};
    desc.ByteWidth = 4096;
    desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    ID3D11Buffer* buffer = nullptr;
    require(SUCCEEDED(device->CreateBuffer(&desc, nullptr, &buffer)), "create native buffer");
    attach(buffer);
    require(shadows.size() == 1, "old resource shadow retained");
    const unsigned before = releases;
    // Repeated native Create calls may intern the same object. Replace the tag
    // before inserting metadata, so its old callback cannot erase the new data.
    attach(buffer);
    require(releases == before + 1 && shadows.size() == 1, "tag replacement order");
    buffer->AddRef();
    buffer->Release();
    require(shadows.size() == 1, "nonfinal Release erased live metadata");
    buffer->Release();
    require(shadows.empty() && releases == before + 2, "final native buffer Release leaked metadata/cycled ownership");
  }

  D3D11_TEXTURE2D_DESC textureDesc = {};
  textureDesc.Width = textureDesc.Height = textureDesc.MipLevels = textureDesc.ArraySize = 1;
  textureDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  textureDesc.SampleDesc.Count = 1;
  textureDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  ID3D11Texture2D* texture = nullptr;
  require(SUCCEEDED(device->CreateTexture2D(&textureDesc, nullptr, &texture)), "create native texture");
  attach(texture);
  texture->Release();
  require(shadows.empty(), "texture metadata leaked");

  const char source[] = "float4 main(float3 p:POSITION):SV_Position{return float4(p,1);}";
  ID3DBlob* bytecode = nullptr;
  require(SUCCEEDED(D3DCompile(source, sizeof(source), nullptr, nullptr, nullptr,
    "main", "vs_5_0", 0, 0, &bytecode, nullptr)), "compile lifetime-test vertex shader");
  ID3D11VertexShader* shader = nullptr;
  require(SUCCEEDED(device->CreateVertexShader(bytecode->GetBufferPointer(), bytecode->GetBufferSize(), nullptr, &shader)), "create native vertex shader");
  D3D11_INPUT_ELEMENT_DESC element = { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 };
  ID3D11InputLayout* layout = nullptr;
  require(SUCCEEDED(device->CreateInputLayout(&element, 1, bytecode->GetBufferPointer(), bytecode->GetBufferSize(), &layout)), "create native input layout");
  bytecode->Release();
  attach(shader);
  attach(layout);
  require(shadows.size() == 2, "shader/layout shadows missing");
  layout->Release();
  shader->Release();
  require(shadows.empty(), "shader/layout metadata leaked");
  context->Release();
  device->Release();
  FreeLibrary(library);
  std::printf("Native resource lifetime tags passed: 10,000 buffers, replacement ordering, final Release, texture, vertex shader and input layout; callbacks=%u.\n", releases);
}
