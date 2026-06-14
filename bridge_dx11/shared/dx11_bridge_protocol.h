#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdint.h>

namespace remix_dx11_bridge {
static constexpr uint32_t kProtocolMagic = 0x44313142u; // B11D / DX11 bridge
static constexpr uint32_t kProtocolVersion = 1u;
static constexpr uint32_t kMaxInlineBytes = 1024 * 1024;

enum class Cmd : uint32_t {
  Invalid = 0,
  Hello,
  Shutdown,
  CreateDXGIFactory,
  CreateDXGIFactory1,
  CreateDXGIFactory2,
  D3D11CreateDevice,
  D3D11CreateDeviceAndSwapChain,
  QueryInterface,
  AddRef,
  Release,
  Factory_EnumAdapters,
  Factory_EnumAdapters1,
  Factory_CreateSwapChain,
  Factory_CreateSwapChainForHwnd,
  Factory_MakeWindowAssociation,
  Factory_GetWindowAssociation,
  Adapter_EnumOutputs,
  Adapter_GetDesc,
  Adapter_GetDesc1,
  Output_GetDesc,
  SwapChain_Present,
  SwapChain_ResizeBuffers,
  SwapChain_GetBuffer,
  SwapChain_SetFullscreenState,
  SwapChain_GetFullscreenState,
  SwapChain_GetDesc,
  SwapChain_GetContainingOutput,
  SwapChain_GetFrameStatistics,
  Device_CreateBuffer,
  Device_CreateTexture1D,
  Device_CreateTexture2D,
  Device_CreateTexture3D,
  Device_CreateShaderResourceView,
  Device_CreateUnorderedAccessView,
  Device_CreateRenderTargetView,
  Device_CreateDepthStencilView,
  Device_CreateInputLayout,
  Device_CreateVertexShader,
  Device_CreateGeometryShader,
  Device_CreateGeometryShaderWithStreamOutput,
  Device_CreatePixelShader,
  Device_CreateHullShader,
  Device_CreateDomainShader,
  Device_CreateComputeShader,
  Device_CreateClassLinkage,
  Device_CreateBlendState,
  Device_CreateDepthStencilState,
  Device_CreateRasterizerState,
  Device_CreateSamplerState,
  Device_CreateQuery,
  Device_CreatePredicate,
  Device_CreateCounter,
  Device_CreateDeferredContext,
  Device_OpenSharedResource,
  Device_CheckFormatSupport,
  Device_CheckMultisampleQualityLevels,
  Device_CheckCounterInfo,
  Device_CheckCounter,
  Device_CheckFeatureSupport,
  Device_GetFeatureLevel,
  Device_GetCreationFlags,
  Device_GetDeviceRemovedReason,
  Device_GetImmediateContext,
  Device_SetExceptionMode,
  Device_GetExceptionMode,
  Resource_GetType,
  Resource_SetEvictionPriority,
  Resource_GetEvictionPriority,
  Buffer_GetDesc,
  Texture1D_GetDesc,
  Texture2D_GetDesc,
  Texture3D_GetDesc,
  View_GetResource,
  RTV_GetDesc,
  DSV_GetDesc,
  SRV_GetDesc,
  UAV_GetDesc,
  Shader_GetDevice,
  State_GetDesc,
  Query_GetDesc,
  Context_ClearState,
  Context_Flush,
  Context_Draw,
  Context_DrawIndexed,
  Context_DrawInstanced,
  Context_DrawIndexedInstanced,
  Context_DrawAuto,
  Context_DrawIndexedInstancedIndirect,
  Context_DrawInstancedIndirect,
  Context_Dispatch,
  Context_DispatchIndirect,
  Context_IASetInputLayout,
  Context_IASetPrimitiveTopology,
  Context_IASetVertexBuffers,
  Context_IASetIndexBuffer,
  Context_VSSetShader,
  Context_VSSetConstantBuffers,
  Context_VSSetShaderResources,
  Context_VSSetSamplers,
  Context_PSSetShader,
  Context_PSSetConstantBuffers,
  Context_PSSetShaderResources,
  Context_PSSetSamplers,
  Context_GSSetShader,
  Context_GSSetConstantBuffers,
  Context_GSSetShaderResources,
  Context_GSSetSamplers,
  Context_HSSetShader,
  Context_HSSetConstantBuffers,
  Context_HSSetShaderResources,
  Context_HSSetSamplers,
  Context_DSSetShader,
  Context_DSSetConstantBuffers,
  Context_DSSetShaderResources,
  Context_DSSetSamplers,
  Context_CSSetShader,
  Context_CSSetConstantBuffers,
  Context_CSSetShaderResources,
  Context_CSSetSamplers,
  Context_CSSetUnorderedAccessViews,
  Context_OMSetRenderTargets,
  Context_OMSetRenderTargetsAndUnorderedAccessViews,
  Context_OMSetBlendState,
  Context_OMSetDepthStencilState,
  Context_RSSetState,
  Context_RSSetViewports,
  Context_RSSetScissorRects,
  Context_SOSetTargets,
  Context_CopyResource,
  Context_CopySubresourceRegion,
  Context_UpdateSubresource,
  Context_CopyStructureCount,
  Context_ResolveSubresource,
  Context_GenerateMips,
  Context_Map,
  Context_Unmap,
  Context_Begin,
  Context_End,
  Context_GetData,
  Context_SetPredication,
  Context_ExecuteCommandList,
  Context_FinishCommandList,
  PrivateData_Set,
  PrivateData_Get,
  PrivateData_SetInterface
};

enum class ObjectKind : uint32_t {
  Unknown = 0, Factory, Adapter, Output, SwapChain, Device, DeviceContext, CommandList,
  Buffer, Texture1D, Texture2D, Texture3D, RTV, DSV, SRV, UAV,
  VertexShader, PixelShader, GeometryShader, HullShader, DomainShader, ComputeShader,
  InputLayout, SamplerState, BlendState, DepthStencilState, RasterizerState,
  Query, Predicate, Counter, ClassLinkage
};

using Handle32 = uint32_t;
using Handle64 = uint64_t;

#pragma pack(push, 1)
struct Header {
  uint32_t magic;
  uint32_t version;
  Cmd cmd;
  uint32_t uid;
  Handle32 self;
  uint32_t payloadBytes;
};

struct Reply {
  uint32_t magic;
  uint32_t version;
  uint32_t uid;
  HRESULT hr;
  Handle32 handle;
  ObjectKind kind;
  uint32_t payloadBytes;
};

struct HelloPayload {
  uint32_t clientPid;
  uint32_t clientBits;
  uint32_t reserved0;
  uint32_t reserved1;
};

struct CreateFactoryPayload { uint32_t flags; GUID riid; };
struct CreateDevicePayload {
  uint64_t adapterHandle;
  uint32_t driverType;
  uint32_t flags;
  uint32_t featureLevelCount;
  uint32_t sdkVersion;
  // followed by D3D_FEATURE_LEVEL array
};
struct CreateDeviceSwapChainPayload {
  uint64_t adapterHandle;
  uint32_t driverType;
  uint32_t flags;
  uint32_t featureLevelCount;
  uint32_t sdkVersion;
  uint32_t swapChainDescBytes;
  // followed by feature levels and DXGI_SWAP_CHAIN_DESC bytes
};
struct PresentPayload { uint32_t syncInterval; uint32_t flags; };
struct ResizeBuffersPayload { uint32_t count; uint32_t width; uint32_t height; uint32_t format; uint32_t flags; };
struct DrawPayload { uint32_t a; uint32_t b; uint32_t c; uint32_t d; uint32_t e; };
struct SetHandlePayload { uint32_t slot; uint32_t count; Handle32 handles[32]; };
struct RangePayload { uint32_t start; uint32_t count; };
#pragma pack(pop)
}
