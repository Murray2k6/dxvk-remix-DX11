/*
 * Copyright (c) 2022-2024, NVIDIA CORPORATION. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */
#include <windows.h>

#include "version.h"
#include "module_processing.h"
#include "remix_api.h"

#include "util_bridge_assert.h"
#include "util_circularbuffer.h"
#include "util_commands.h"
#include "util_common.h"
#include "util_devicecommand.h"
#include "util_filesys.h"
#include "util_guid.h"
#include "util_hack_d3d_debug.h"
#include "util_messagechannel.h"
#include "util_modulecommand.h"
#include "util_process.h"
#include "util_remixapi.h"
#include "util_seh.h"
#include "util_semaphore.h"
#include "util_sharedheap.h"
#include "util_sharedmemory.h"
#include "util_texture_and_volume.h"
#include "util_version.h"

#include "log/log.h"
#include "config/config.h"
#include "config/global_options.h"
#include "server_options.h"
// DX11_V227: removed stale unused include of the old D3D11 client's client_options.h
// (that client dir was deleted; the server never references ClientOptions).

#include "../tracy/tracy.hpp"
#include <iostream>
#include <d3d11.h>
#include <assert.h>
#include <map>
#include <atomic>
#include <array>

using namespace Commands;
using namespace bridge_util;
using namespace remixapi::util;
#ifndef DX11_BRIDGE_SKIP_LEGACY_D3D11_REGISTER_V180
#define DX11_BRIDGE_SKIP_LEGACY_D3D11_REGISTER_V180
static inline remixapi_ErrorCode BridgeSkipLegacyD3D11RegisterForDx11HeaderV180() {
  Logger::info("DX11 bridge: skipped legacy dxvk_RegisterD3D11Device call because the active Remix API header is DX11 and has no D3D11 registration member.");
  return REMIXAPI_ERROR_CODE_SUCCESS;
}
#endif

// NOTE: This extension is really useful for debugging the Bridge child process from the parent process:
// https://marketplace.visualstudio.com/items?itemName=vsdbgplat.MicrosoftChildProcessDebuggingPowerTool

#define SEND_OPTIONAL_SERVER_RESPONSE(hresult, uid) { \
    if (GlobalOptions::getSendAllServerResponses()) { \
      ServerMessage c(Commands::Bridge_Response, uid); \
      c.send_data(hresult); \
    } \
  } 

#define SEND_OPTIONAL_CREATE_FUNCTION_SERVER_RESPONSE(hresult, uid) { \
    if (GlobalOptions::getSendCreateFunctionServerResponses() || GlobalOptions::getSendAllServerResponses()) { \
      ServerMessage c(Commands::Bridge_Response, uid); \
      c.send_data(hresult); \
    } \
  } 

#define PULL(type, name) const auto& name = (type)DeviceBridge::get_data()
#define PULL_I(name) PULL(INT, name)
#define PULL_U(name) PULL(UINT, name)
#define PULL_D(name) PULL(DWORD, name)
#define PULL_H(name) PULL(HRESULT, name)
#define PULL_HND(name) \
            PULL_U(name); \
            assert(name != NULL)
#define PULL_DATA(size, name) \
            uint32_t name##_len = DeviceBridge::get_data((void**)&name); \
            assert(name##_len == 0 || size == name##_len)
#define PULL_OBJ(type, name) \
            type* name = nullptr; \
            PULL_DATA(sizeof(type), name)
#define CHECK_DATA_OFFSET (DeviceBridge::get_data_pos() == rpcHeader.dataOffset)
#define GET_HND(name) \
            const auto& name = rpcHeader.pHandle; \
            assert(name != NULL)
#define GET_HDR_VAL(name) \
            const DWORD& name = rpcHeader.pHandle;
#define GET_RES(name, map) \
            GET_HND(name##Handle); \
            const auto& name = map[name##Handle]; \
            assert(name != NULL)

// NOTE: MSDN states HWNDs are safe to cross x86-->x64 boundary, and that a truncating cast should be used:
// https://docs.microsoft.com/en-us/windows/win32/winprog64/interprocess-communication?redirectedfrom=MSDN
#define TRUNCATE_HANDLE(type, input) (type)(size_t)(input)

bool bDxvkModuleLoaded = false;
std::chrono::steady_clock::time_point gTimeStart;

// Shared memory and IPC channels
Guid gUniqueIdentifier;
NamedSemaphore* gpPresent = nullptr;
std::unique_ptr<MessageChannelServer> gpClientMessageChannel;
// DX11_V225: DX11 bridge server. The factory is a DXGI factory and all replayed
// resources are native D3D11/DXGI objects (no D3D11 interfaces).
// D3D Library handle
typedef HRESULT (WINAPI* PFN_CreateDXGIFactoryC)(REFIID, void**);
HMODULE ghModule;
IDXGIFactory1* gpD3D;

bool gOverwriteConditionAlreadyActive = false;

// Mapping between client and server pointer addresses
std::unordered_map<uint32_t, ID3D11Device*> gpD3DDevices;
std::unordered_map<uint32_t, ID3D11Resource*> gpD3DResources; // For Textures, Buffers, and Surfaces
std::unordered_map<uint32_t, ID3D11Resource*> gpD3DVolumes;   // 3D textures are ID3D11Texture3D (an ID3D11Resource)
std::unordered_map<uint32_t, ID3D11InputLayout*> gpD3DVertexDeclarations;
std::unordered_map<uint32_t, ID3D11DeviceContext*> gpD3DStateBlocks; // D3D11 has no state blocks; deferred-context captures stand in
std::unordered_map<uint32_t, ID3D11VertexShader*> gpD3DVertexShaders;
std::unordered_map<uint32_t, ID3D11PixelShader*> gpD3DPixelShaders;
std::unordered_map<uint32_t, IDXGISwapChain*> gpD3DSwapChains;
std::unordered_map<uint32_t, ID3D11Query*> gpD3DQuery;
std::unordered_map<uint32_t, void*> gMapRemixApi;

// Global state
bool gbBridgeRunning = true;
HANDLE hWait;

namespace {
template<typename SerializableT>
static void deserializeFromQueue(SerializableT& serializableT) {
  static_assert(is_serializable_v<SerializableT>, "deserializeRemixApiT(...)  may only be called with defined Serializable<T> types");
  void* pSlzdData = nullptr;
  const auto size = DeviceBridge::get_data(&pSlzdData);
  SerializableT dslz(pSlzdData);
  assert(size == dslz.size());
  dslz.deserialize();
  serializableT = std::move(dslz);
}
}

static inline void safeDestroy(IUnknown* obj, uint32_t x86handle) {
  // Note: in DXVK the refcounts of non-standalone objects may go negative!
  // We need to handle such objects appropriately, even though this is not
  // the case in regular system D3D11.

#if defined(_DEBUG) && defined(VERBOSE)
  if (obj) {
    const LONG cnt = static_cast<LONG>(obj->Release());
    if (cnt > 0) {
      Logger::trace(format_string("Object [%p/%lx] refcount at destroy is %d > 1.",
                                  obj, x86handle, cnt + 1));
    }
  }
#endif

  while (obj && static_cast<LONG>(obj->Release()) > 0);
}

// DX11_V225: build a DXGI swap chain description from the raw 32-bit-packed
// presentation parameters the x86 client sends (the hDeviceWindow handle is 4
// bytes inbound but 8 bytes in the x64 HWND, so we truncate-cast it).
DXGI_SWAP_CHAIN_DESC getPresParamFromRaw(const uint32_t* rawPresentationParameters) {
  DXGI_SWAP_CHAIN_DESC desc = {};
  desc.BufferDesc.Width = *reinterpret_cast<const UINT*>(rawPresentationParameters);
  desc.BufferDesc.Height = *reinterpret_cast<const UINT*>(rawPresentationParameters + 1);
  desc.BufferDesc.Format = *reinterpret_cast<const DXGI_FORMAT*>(rawPresentationParameters + 2);
  const UINT backBufferCount = *reinterpret_cast<const UINT*>(rawPresentationParameters + 3);
  desc.BufferCount = backBufferCount ? backBufferCount : 1;

  desc.SampleDesc.Count = 1;
  desc.SampleDesc.Quality = 0;

  desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  desc.OutputWindow = TRUNCATE_HANDLE(HWND, rawPresentationParameters[7]);
  desc.Windowed = *reinterpret_cast<const BOOL*>(rawPresentationParameters + 8) ? TRUE : FALSE;
  desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
  desc.Flags = *reinterpret_cast<const DWORD*>(rawPresentationParameters + 11);

  const UINT refreshHz = *reinterpret_cast<const UINT*>(rawPresentationParameters + 12);
  desc.BufferDesc.RefreshRate.Numerator = refreshHz;
  desc.BufferDesc.RefreshRate.Denominator = refreshHz ? 1 : 0;

  return desc;
}

HRESULT ReturnSurfaceDataToClient(ID3D11Texture2D* pReturnSurfaceData, HRESULT hresult, UINT currentUID) {
  // We send the HRESULT response back to the client even in case of failure
  ServerMessage c(Commands::Bridge_Response, currentUID);

  if (!SUCCEEDED(hresult) || pReturnSurfaceData == nullptr) {
    c.send_data(SUCCEEDED(hresult) ? E_POINTER : hresult);
    return hresult;
  }

  // Using the texture desc to get width, height and format of the surface.
  D3D11_TEXTURE2D_DESC pDesc = {};
  pReturnSurfaceData->GetDesc(OUT & pDesc);

  const uint32_t width = pDesc.Width;
  const uint32_t height = pDesc.Height;
  const DXGI_FORMAT format = pDesc.Format;

  // Map the texture for CPU read (the caller provides a CPU-readable/staging copy)
  // and forward the raw bytes to the client.
  ID3D11Device* device = nullptr;
  pReturnSurfaceData->GetDevice(&device);
  ID3D11DeviceContext* context = nullptr;
  if (device) {
    device->GetImmediateContext(&context);
  }

  D3D11_MAPPED_SUBRESOURCE mapped = {};
  hresult = context ? context->Map(pReturnSurfaceData, 0, D3D11_MAP_READ, 0, OUT & mapped) : E_FAIL;
  if (!SUCCEEDED(hresult)) {
    if (context) context->Release();
    if (device) device->Release();
    c.send_data(hresult);
    return hresult;
  }

  // Sending raw surface buffer details to client
  const uint32_t totalSize = bridge_util::calcTotalSizeOfRect(width, height, format);
  const uint32_t rowSize = bridge_util::calcRowSize(width, format);
  c.send_data(hresult);
  c.send_data(width);
  c.send_data(height);
  c.send_data((uint32_t) format);
  if (auto* blobPacketPtr = c.begin_data_blob(totalSize)) {
    FOR_EACH_RECT_ROW(mapped, height, format, {
      memcpy(blobPacketPtr, ptr, rowSize);
      blobPacketPtr += rowSize;
    });
    c.end_data_blob();
  }

  context->Unmap(pReturnSurfaceData, 0);
  context->Release();
  device->Release();
  return hresult;
}

template<typename T>
static bool dumpLeakedObjects(const char* name, const T& map) {
  if (!map.empty()) {
    bridge_util::Logger::err(format_string("%zd objects discovered in %s map at "
                              "Direct3D module eviction:", map.size(), name));
    for (auto& [handle, obj] : map) {
      bridge_util::Logger::err(format_string("\t%x -> %p", handle, obj));
    }
    return true;
  }
  return false;
}

static bool dumpLeakedObjects() {
  bool anyLeaked = false;

  anyLeaked |= dumpLeakedObjects("Resource", gpD3DResources);
  anyLeaked |= dumpLeakedObjects("Vertex Declaration", gpD3DVertexDeclarations);
  anyLeaked |= dumpLeakedObjects("State Block", gpD3DStateBlocks);
  anyLeaked |= dumpLeakedObjects("Vertex Shader", gpD3DVertexShaders);
  anyLeaked |= dumpLeakedObjects("Pixel Shader", gpD3DPixelShaders);
  anyLeaked |= dumpLeakedObjects("Swapchain", gpD3DSwapChains);
  anyLeaked |= dumpLeakedObjects("Volume", gpD3DVolumes);

  anyLeaked |= dumpLeakedObjects("Device", gpD3DDevices);

  return anyLeaked;
}

// DX11_V225: create a D3D11 device + swap chain through whichever d3d11.dll is
// loaded in this process (the .trex Remix runtime), so the device is RT-capable.
static HRESULT CreateDx11DeviceAndSwapChain(const DXGI_SWAP_CHAIN_DESC& scDesc,
                                            ID3D11Device** ppDevice,
                                            IDXGISwapChain** ppSwapChain) {
  typedef HRESULT (WINAPI* PFN_CreateDS)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT,
                                         const D3D_FEATURE_LEVEL*, UINT, UINT,
                                         const DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**,
                                         ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);
  HMODULE d3d11 = GetModuleHandleW(L"d3d11.dll");
  if (!d3d11) {
    d3d11 = LoadLibraryW(L"d3d11.dll");
  }
  if (!d3d11) {
    return E_FAIL;
  }
  auto create = reinterpret_cast<PFN_CreateDS>(GetProcAddress(d3d11, "D3D11CreateDeviceAndSwapChain"));
  if (!create) {
    return E_FAIL;
  }
  DXGI_SWAP_CHAIN_DESC sc = scDesc;
  D3D_FEATURE_LEVEL achievedLevel = D3D_FEATURE_LEVEL_11_0;
  return create(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
                D3D11_SDK_VERSION, &sc, ppSwapChain, ppDevice, &achievedLevel, nullptr);
}

void ProcessDeviceCommandQueue() {
  // Loop until the client sends terminate instruction
  bool done = false;
  while (!done && DeviceBridge::waitForCommand() == Result::Success) {
    ZoneScopedN("Process Command");
#ifdef LOG_SERVER_COMMAND_TIME
    // Take a snapshot of the current tick count for profiling purposes
    const auto start = GetTickCount64();
#endif

    const Header rpcHeader = DeviceBridge::pop_front();

#ifdef _DEBUG
    // If data batching is enabled and the data offset on the comamnd is different from
    // our current offset we know there must be data to read, so we start a data batch
    // read operation on the data queue buffer.
    if (!CHECK_DATA_OFFSET) {
      const auto result = DeviceBridge::begin_read_data();
      assert(RESULT_SUCCESS(result));
    }
#endif

    {
      ZoneScoped;
      if (ZoneIsActive) {
        const std::string commandStr = toString(rpcHeader.command);
        ZoneName(commandStr.c_str(), commandStr.size());
      }
      PULL_U(currentUID);
#if defined(_DEBUG) || defined(DEBUGOPT)
      if (GlobalOptions::getLogServerCommands()) {
        Logger::info("Device Processing: " + toString(rpcHeader.command) + " UID: " + std::to_string(currentUID));
      }
#endif
      // The mother of all switch statements - every call in the D3D11 interface is mapped here...
      switch (rpcHeader.command) {
      // DX11_V226: The DX11 capture bridge does NOT replay legacy per-call D3D11/D3D11
      // device commands on the server. The 32-bit client (client_dx11) captures the
      // game's real DX11 scene in-process and streams it to the Remix runtime via the
      // RemixApi_* commands below (meshes, materials, lights, instances). The server
      // hosts the .trex Remix runtime and services the control (Bridge_*) and RemixApi_*
      // command set. ~2500 lines of dead D3D11 device-command handlers were removed here
      // because no DX11 client emits them; see memory dx11-bridge-rewrite-architecture.

      /*
       * Other commands
       */
      case Bridge_DebugMessage:
      {
        PULL_U(i);
        const int length = DeviceBridge::getReaderChannel().data->peek();
        void* text = nullptr;
        const int size = DeviceBridge::getReaderChannel().data->pull(&text);
        std::stringstream ss;
        ss << "DebugMessage. i = " << i << ", length = " << length << " = " << size << ", text = '" << (char*) text << "'";
        Logger::info(ss.str().c_str());
        break;
      }
      case Bridge_Terminate:
      {
        done = true;
        break;
      }
      case Bridge_SharedHeap_AddSeg:
      {
        GET_HDR_VAL(_segmentSize);
        const uint32_t segmentSize = (uint32_t) _segmentSize;
        SharedHeap::addNewHeapSegment(segmentSize);
        break;
      }
      case Bridge_SharedHeap_Alloc:
      {
        GET_HDR_VAL(_allocId);
        const auto allocId = (SharedHeap::AllocId) _allocId;
        PULL_U(chunkId);
        SharedHeap::allocate(allocId, chunkId);
        break;
      }
      case Bridge_SharedHeap_Dealloc:
      {
        GET_HDR_VAL(_allocId);
        const auto allocId = (SharedHeap::AllocId) _allocId;
        SharedHeap::deallocate(allocId);
        break;
      }
      case Bridge_UnlinkResource:
      {
        GET_HND(pHandle);
        gpD3DResources.erase(pHandle);
        break;
      }
      case Bridge_UnlinkVolumeResource:
      {
        GET_HND(pHandle);
        gpD3DVolumes.erase(pHandle);
        break;
      }
      /*
       * BridgeApi commands
       */
      case RemixApi_CreateMaterial:
      {
        // Rather than allocate deserialized struct extensions on the heap,
        // allocate them locally, since we know only one instance will be
        // supported at a time
        struct MaterialExtensions {
          serialize::MaterialInfoOpaque opaque;
          serialize::MaterialInfoOpaqueSubsurface opaqueSubsurface;
          serialize::MaterialInfoTranslucent translucent;
          serialize::MaterialInfoPortal portal;
        } exts;
        memset(&exts, 0, sizeof(MaterialExtensions));

        const auto matInfoSType = remixapi::pullSType();
        assert(matInfoSType == REMIXAPI_STRUCT_TYPE_MATERIAL_INFO);
        serialize::MaterialInfo matInfo;
        deserializeFromQueue(matInfo);
        
        matInfo.pNext = nullptr;

        bool bMatExtExists = remixapi::pullBool();
        auto* pInfoProto = &getInfoProto(matInfo);
        while(bMatExtExists) {
          const auto extSType = remixapi::pullSType();
          switch (extSType) {
            case REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_OPAQUE_EXT:
            {
              assert(!exts.opaque.pNext);
              deserializeFromQueue(exts.opaque);
              pInfoProto->pNext = &(exts.opaque);
              pInfoProto = &getInfoProto(exts.opaque);
              break;
            }
            case REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_OPAQUE_SUBSURFACE_EXT:
            {
              assert(!exts.opaqueSubsurface.pNext);
              deserializeFromQueue(exts.opaqueSubsurface);
              pInfoProto->pNext = &(exts.opaqueSubsurface);
              pInfoProto = &getInfoProto(exts.opaqueSubsurface);
              break;
            }
            case REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_TRANSLUCENT_EXT:
            {
              assert(!exts.translucent.pNext);
              deserializeFromQueue(exts.translucent);
              pInfoProto->pNext = &(exts.translucent);
              pInfoProto = &getInfoProto(exts.translucent);
              break;
            }
            case REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_PORTAL_EXT:
            {
              assert(!exts.portal.pNext);
              deserializeFromQueue(exts.portal);
              pInfoProto->pNext = &(exts.portal);
              pInfoProto = &getInfoProto(exts.portal);
              break;
            }
            default:
            {
              Logger::warn("[Api_CreateMaterial] Unknown sType. Skipping.");
              break;
            }
          }
          bMatExtExists = remixapi::pullBool();
        }

        auto bridgeHandle = DeviceBridge::get_data();
        remixapi_MaterialHandle remixApiHandle = nullptr;
        // DX11_V227_BRIDGE_REMIX_INIT: null-guard so a failed Remix init never crashes the server.
        if(remixapi::g_remix_initialized && remixapi::g_remix.CreateMaterial && remixapi::g_remix.CreateMaterial(&matInfo, &remixApiHandle) == REMIXAPI_ERROR_CODE_SUCCESS) {
          MaterialHandle(bridgeHandle, remixApiHandle);
        } else {
          static bool s_warnedCreateMaterial = false;
          if (!s_warnedCreateMaterial) { s_warnedCreateMaterial = true; Logger::err("[RemixApi_CreateMaterial] Remix API unavailable or call failed!"); }
        }
        
        break;
      }

      case RemixApi_DestroyMaterial:
      {
        MaterialHandle handle(DeviceBridge::get_data());
        if(handle.isValid()) {
          remixapi::g_remix.DestroyMaterial(handle);
          handle.invalidate();
        } else {
          Logger::err("[RemixApi_DestroyMaterial] Invalid material handle!" );
        }
        break;
      }

      case RemixApi_CreateMesh:
      {
        const auto meshInfoSType = remixapi::pullSType();
        assert(meshInfoSType == REMIXAPI_STRUCT_TYPE_MESH_INFO);
        serialize::MeshInfo meshInfo;
        deserializeFromQueue(meshInfo);
        meshInfo.pNext = nullptr;
        
        for(size_t iSurf = 0; iSurf < meshInfo.surfaces_count; ++iSurf) {
          // If we don't const_cast, we'd have to copy the entire
          // remixapi_MeshInfo::surfaces_values array in order to get around
          // the remixapi_MeshInfo's member const qualifier and reassign
          // remixapi_MeshInfoSurfaceTriangles::material.
          auto& surf = const_cast<remixapi_MeshInfoSurfaceTriangles&>(meshInfo.surfaces_values[iSurf]);
          MaterialHandle matHandle(surf.material);
          surf.material = matHandle;
        }

        auto bridgeHandle = DeviceBridge::get_data();
        remixapi_MeshHandle remixApiHandle = nullptr;
        // DX11_V227_BRIDGE_REMIX_INIT: null-guard so a failed Remix init never crashes the server.
        if(remixapi::g_remix_initialized && remixapi::g_remix.CreateMesh && remixapi::g_remix.CreateMesh(&meshInfo, &remixApiHandle) == REMIXAPI_ERROR_CODE_SUCCESS) {
          MeshHandle handle(bridgeHandle, remixApiHandle);
        } else {
          static bool s_warnedCreateMesh = false;
          if (!s_warnedCreateMesh) { s_warnedCreateMesh = true; Logger::err("[RemixApi_CreateMesh] Remix API unavailable or call failed!"); }
        }

        break;
      }

      case RemixApi_DestroyMesh:
      {
        MeshHandle handle(DeviceBridge::get_data());
        if(handle.isValid()) {
          remixapi::g_remix.DestroyMesh(handle);
          handle.invalidate();
        } else {
          Logger::err("[RemixApi_DestroyMesh] Invalid mesh handle!" );
        }
        break;
      }

      case RemixApi_DrawInstance:
      {
        // Rather than allocate deserialized struct extensions on the heap,
        // allocate them locally, since we know only one instance will be
        // supported at a time
        struct InstanceExtensions {
          serialize::InstanceInfoObjectPicking objectPicking;
          serialize::InstanceInfoBlend blend;
          serialize::InstanceInfoTransforms boneXforms;
          serialize::InstanceInfoParticleSystem particleSystem;
        } exts;
        memset(&exts, 0, sizeof(InstanceExtensions));

        const auto instSType = remixapi::pullSType();
        assert(instSType == REMIXAPI_STRUCT_TYPE_INSTANCE_INFO);
        serialize::InstanceInfo instInfo;
        deserializeFromQueue(instInfo);
        
        MeshHandle meshHandle(instInfo.mesh);
        if(meshHandle.isValid()) {
          instInfo.mesh = meshHandle;
        } else {
          Logger::err("[RemixApi_DrawInstance] Invalid mesh handle!" );
        }

        instInfo.pNext = nullptr;
        
        bool bInstExtExists = remixapi::pullBool();
        auto* pInfoProto = &getInfoProto(instInfo);
        while(bInstExtExists) {
          const auto extSType = remixapi::pullSType();
          switch (extSType) {
            case REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_OBJECT_PICKING_EXT:
            {
              assert(!exts.objectPicking.pNext);
              deserializeFromQueue(exts.objectPicking);
              pInfoProto->pNext = &(exts.objectPicking);
              pInfoProto = &getInfoProto(exts.objectPicking);
              break;
            }
            case REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_BLEND_EXT:
            {
              assert(!exts.blend.pNext);
              deserializeFromQueue(exts.blend);
              pInfoProto->pNext = &(exts.blend);
              pInfoProto = &getInfoProto(exts.blend);
              break;
            }
            case REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_BONE_TRANSFORMS_EXT:
            {
              assert(!exts.boneXforms.pNext);
              deserializeFromQueue(exts.boneXforms);
              pInfoProto->pNext = &(exts.boneXforms);
              pInfoProto = &getInfoProto(exts.boneXforms);
              break;
            }
            case REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_PARTICLE_SYSTEM_EXT:
            {
              assert(!exts.particleSystem.pNext);
              deserializeFromQueue(exts.particleSystem);
              pInfoProto->pNext = &(exts.particleSystem);
              pInfoProto = &getInfoProto(exts.particleSystem);
              break;
            }
            default:
            {
              Logger::warn("[RemixApi_DrawInstance] Unknown sType. Skipping.");
              break;
            }
          }
          bInstExtExists = remixapi::pullBool();
        }

        // DX11_V227_BRIDGE_REMIX_INIT: null-guard so a failed Remix init never crashes the server.
        if(remixapi::g_remix_initialized && remixapi::g_remix.DrawInstance) {
          if(remixapi::g_remix.DrawInstance(&instInfo) != REMIXAPI_ERROR_CODE_SUCCESS) {
            static bool s_warnedDrawInstance = false;
            if (!s_warnedDrawInstance) { s_warnedDrawInstance = true; Logger::err("[RemixApi_DrawInstance] Remix API call failed!"); }
          }
        }

        break;
      }

      case RemixApi_CreateLight:
      {
        // Rather than allocate deserialized struct extensions on the heap,
        // allocate them locally, since we know only one instance will be
        // supported at a time
        struct LightExtensions {
          serialize::LightInfoSphere sphere;
          serialize::LightInfoRect rect;
          serialize::LightInfoDisk disk;
          serialize::LightInfoCylinder cylinder;
          serialize::LightInfoDistant distant;
          serialize::LightInfoDome dome;
          serialize::LightInfoUSD usd;
        } exts;
        memset(&exts, 0, sizeof(LightExtensions));

        const auto lightSType = remixapi::pullSType();
        assert(lightSType == REMIXAPI_STRUCT_TYPE_LIGHT_INFO);
        serialize::LightInfo lightInfo;
        deserializeFromQueue(lightInfo);
        lightInfo.pNext = nullptr;

        bool bLightExtExists = remixapi::pullBool();
        auto* pInfoProto = &getInfoProto(lightInfo);
        while(bLightExtExists) {
          const auto extSType = remixapi::pullSType();
          switch (extSType) {
            case REMIXAPI_STRUCT_TYPE_LIGHT_INFO_SPHERE_EXT:
            {
              assert(!exts.sphere.pNext);
              deserializeFromQueue(exts.sphere);
              pInfoProto->pNext = &(exts.sphere);
              pInfoProto = &getInfoProto(exts.sphere);
              break;
            }
            case REMIXAPI_STRUCT_TYPE_LIGHT_INFO_RECT_EXT:
            {
              assert(!exts.rect.pNext);
              deserializeFromQueue(exts.rect);
              pInfoProto->pNext = &(exts.rect);
              pInfoProto = &getInfoProto(exts.rect);
              break;
            }
            case REMIXAPI_STRUCT_TYPE_LIGHT_INFO_DISK_EXT:
            {
              assert(!exts.disk.pNext);
              deserializeFromQueue(exts.disk);
              pInfoProto->pNext = &(exts.disk);
              pInfoProto = &getInfoProto(exts.disk);
              break;
            }
            case REMIXAPI_STRUCT_TYPE_LIGHT_INFO_CYLINDER_EXT:
            {
              assert(!exts.cylinder.pNext);
              deserializeFromQueue(exts.cylinder);
              pInfoProto->pNext = &(exts.cylinder);
              pInfoProto = &getInfoProto(exts.cylinder);
              break;
            }
            case REMIXAPI_STRUCT_TYPE_LIGHT_INFO_DISTANT_EXT:
            {
              assert(!exts.distant.pNext);
              deserializeFromQueue(exts.distant);
              pInfoProto->pNext = &(exts.distant);
              pInfoProto = &getInfoProto(exts.distant);
              break;
            }
            case REMIXAPI_STRUCT_TYPE_LIGHT_INFO_DOME_EXT:
            {
              assert(!exts.dome.pNext);
              deserializeFromQueue(exts.dome);
              pInfoProto->pNext = &(exts.dome);
              pInfoProto = &getInfoProto(exts.dome);
              break;
            }
            case REMIXAPI_STRUCT_TYPE_LIGHT_INFO_USD_EXT:
            {
              assert(!exts.usd.pNext);
              deserializeFromQueue(exts.usd);
              pInfoProto->pNext = &(exts.usd);
              pInfoProto = &getInfoProto(exts.usd);
              break;
            }
            default:
            {
              Logger::warn("[RemixApi_CreateLight] Unknown sType. Skipping.");
              break;
            }
          }
          bLightExtExists = remixapi::pullBool();
        }

        auto bridgeHandle = DeviceBridge::get_data();
        remixapi_LightHandle lightHandle = nullptr;
        if(remixapi::g_remix.CreateLight(&lightInfo, &lightHandle) == REMIXAPI_ERROR_CODE_SUCCESS) {
          LightHandle handle(bridgeHandle, lightHandle);
        } else {
          Logger::err("[RemixApi_CreateLight] Remix API call failed!");
        }
        
        break;
      }

      case RemixApi_DestroyLight:
      {
        LightHandle handle(DeviceBridge::get_data());
        if(handle.isValid()) {
          remixapi::g_remix.DestroyLight(handle);
          handle.invalidate();
        } else {
          Logger::err("[RemixApi_DestroyLight] Invalid light handle!" );
        }
        break;
      }

      case RemixApi_DrawLightInstance:
      {
        LightHandle handle(DeviceBridge::get_data());
        if(handle.isValid()) {
          remixapi::g_remix.DrawLightInstance(handle);
        } else {
          Logger::err("[RemixApi_DrawLightInstance] Invalid light handle!" );
        }
        break;
      }

      case RemixApi_SetConfigVariable:
      {
        void* var_ptr = nullptr;
        const uint32_t var_size = DeviceBridge::getReaderChannel().data->pull(&var_ptr);
        std::string var_str((const char*) var_ptr, var_size);

        void* value_ptr = nullptr;
        const uint32_t value_size = DeviceBridge::getReaderChannel().data->pull(&value_ptr);
        std::string value_str((const char*) value_ptr, value_size);

        remixapi::g_remix.SetConfigVariable(var_str.c_str(), value_str.c_str());
        break;
      }

      case RemixApi_CreateD3D11:
      {
        Logger::err("[RemixApi_CreateD3D11] Not yet supported. Device used by Remix API defaults to "
                                          "most recently created by client application.");
        break;
      }

      case RemixApi_RegisterDevice:
      {
        Logger::err("[RemixApi_RegisterDevice] Not yet supported. Device used by Remix API defaults to "
                                              "most recently created by client application.");
        break;
      }
      
      default:
        break;
      }
    }

    // Ensure the data position between client and server is in sync after processing the command
    if (!CHECK_DATA_OFFSET)       {
      Logger::warn("Data not in sync");
    }
    assert(CHECK_DATA_OFFSET);
    *DeviceBridge::getReaderChannel().serverDataPos = DeviceBridge::get_data_pos();
    // Check if overwrite condition was met
    if (*DeviceBridge::getReaderChannel().clientDataExpectedPos != -1) {
      if (!gOverwriteConditionAlreadyActive) {
        gOverwriteConditionAlreadyActive = true;
        Logger::warn("Data Queue overwrite condition triggered");
      }
      // Check if server needs to complete a loop and the position was read
      if (*DeviceBridge::getReaderChannel().serverDataPos > *DeviceBridge::getReaderChannel().clientDataExpectedPos && !(*DeviceBridge::getReaderChannel().serverResetPosRequired)) {
        DeviceBridge::getReaderChannel().dataSemaphore->release(1);
        *DeviceBridge::getReaderChannel().clientDataExpectedPos = -1;
        gOverwriteConditionAlreadyActive = false;
        Logger::info("DataQueue overwrite condition resolved");
      }
    }

    const auto count = DeviceBridge::end_read_data();

#ifdef ENABLE_DATA_BATCHING_TRACE
    Logger::trace(format_string("Finished batch data read with %d data items.", count));
#endif

#ifdef LOG_SERVER_COMMAND_TIME
    // See how long processing this command took
    const auto diff = GetTickCount64() - start;
    if (diff > SERVER_COMMAND_THRESHOLD_MS) {
      std::string command = Commands::toString(rpcHeader.command);
      Logger::trace(format_string("Command %s took %d milliseconds to process!", command.c_str(), diff));
    }
#endif
  }

  // Check if we exited the command processing loop unexpectedly while the bridge is still enabled
  if (!done && gbBridgeRunning) {
    Logger::debug("The device command processing loop was exited unexpectedly, either due to timing out or some other command queue issue.");
  }
}

void CheckD3D11Type(HMODULE d3d11Module) {
  char d3d11Path[MAX_PATH];
  GetModuleFileName(d3d11Module, d3d11Path, sizeof(d3d11Path));
  DWORD rsvd = 0;
  DWORD verSize = GetFileVersionInfoSize(d3d11Path, &rsvd);
  BRIDGE_ASSERT_LOG((verSize > 0), "Issue retrieving D3D11_LS version info");
  BRIDGE_ASSERT_LOG((rsvd == 0), "Issue retrieving D3D11_LS version info");
  Logger::info(format_string("Loaded D3D11 from %s", d3d11Path));
  LPSTR verData = new char[verSize];
  if (GetFileVersionInfo(d3d11Path, rsvd, verSize, verData)) {
    UINT size = 0;
    LPBYTE translationBuffer = nullptr;
    if (VerQueryValue(verData, TEXT("\\VarFileInfo\\Translation"), (LPVOID*) &translationBuffer, &size)) {
      BRIDGE_ASSERT_LOG((size > 0), "Invalid size obtained while retrieving D3D11_ls version data");
      std::stringstream ss;
      ss << std::hex << std::setw(4) << std::setfill('0') << ((uint16_t*) (translationBuffer))[0];
      ss << std::hex << std::setw(4) << std::setfill('0') << ((uint16_t*) (translationBuffer))[1];
      const std::string langCodepageStr = ss.str();
      const std::string verDataProdNameLookupStr = std::string("\\StringFileInfo\\") + ss.str() + std::string("\\ProductName");
      LPBYTE productNameBuffer = nullptr;
      if (VerQueryValue(verData, verDataProdNameLookupStr.c_str(), (LPVOID*) &productNameBuffer, &size)) {
        BRIDGE_ASSERT_LOG((size > 0), "Invalid size obtained while retrieving D3D11_ls version data");
        const std::string productName((char*) productNameBuffer);
        // Assume for now that any d3d11 DLL that doesn't have Microsoft product naming is DXVK.
        bDxvkModuleLoaded = productName.find("Microsoft") == std::string::npos;
        if (!bDxvkModuleLoaded) {
          Logger::warn("Please note that the version of d3d11 loaded is NOT DXVK. Functional restrictions may apply.");
        } else {
          Logger::info("Version of d3d11 loaded is DXVK");
        }
      }
    }
  }
}

bool InitializeD3D() {
  // If vanilla dxvk is enabled attempt to load that first.
  if (ServerOptions::getUseVanillaDxvk()) {
    Logger::info("Loading standard Non-RTX DXVK d3d11 dll.");
    ghModule = LoadLibrary("d3d11vk_x64.dll");
    if (ghModule) {
      Logger::info("Non-RTX standard d3d11vk_x64.dll loaded");
    } else {
      Logger::err("d3d11vk_x64.dll loading failed!");
      return false;
    }
  } else {
    // Since vanilla dxvk is diabled attempt loading regular d3d11.dll which
    // could be either the system d3d11 one or our own Remix dxvk flavor of it.
    ghModule = LoadLibrary("d3d11.dll");
  }
  // Now check if loading the dll actually succeeded or not, and try to
  // create the D3D instance used for the lifetime of this process.
  if (ghModule != nullptr) {
    // DX11_V225: the DX11 bridge server's "factory" is a DXGI factory (CreateDXGIFactory1
    // lives in dxgi.dll / the .trex dxgi.dll, not d3d11.dll).
    HMODULE dxgiModule = GetModuleHandleW(L"dxgi.dll");
    if (!dxgiModule) {
      dxgiModule = LoadLibraryW(L"dxgi.dll");
    }
    auto pCreateDXGIFactory1 = dxgiModule
      ? (PFN_CreateDXGIFactoryC) GetProcAddress(dxgiModule, "CreateDXGIFactory1")
      : nullptr;
    HRESULT facHr = pCreateDXGIFactory1 ? pCreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**) &gpD3D) : E_FAIL;
    if (FAILED(facHr) || gpD3D == nullptr) {
      Logger::err(format_string("DX11 DXGI factory creation failed: 0x%lx\n", facHr));
      return false;
    } else {
      Logger::info("DX11 DXGI factory creation succeeded!");
    }
    // Initialize remixApi
    if (GlobalOptions::getExposeRemixApi()) {
      remixapi_ErrorCode status = remixapi_lib_loadRemixDllAndInitialize(L"d3d11.dll", &remixapi::g_remix, &remixapi::g_remix_dll);
      if (status != REMIXAPI_ERROR_CODE_SUCCESS) {
        Logger::err(format_string("[RemixApi] RemixApi initialization failed: %d\n", status));
      } else {
        remixapi::g_remix_initialized = true;
        Logger::info("[RemixApi] Initialized RemixApi.");
      }
    }
  } else {
    Logger::err(format_string("d3d11.dll loading failed: %ld\n", GetLastError()));
    return false;
  }

  if (!ServerOptions::getUseVanillaDxvk()) {
    FixD3DRecordHRESULT("d3d11.dll", ghModule);
  }

  CheckD3D11Type(ghModule);
  if (bDxvkModuleLoaded) {
    auto queryFeatureVersion = (version::QueryFunc)GetProcAddress(ghModule, version::QueryFuncName);
    if(queryFeatureVersion == NULL) {
      Logger::err(format_string("Unable to resolve %s, may be the result of an outdated Remix DXVK *or* loading vanilla DXVK.\n", version::QueryFuncName));
      return true; // Not necessarily fatal
    }
    std::array<uint64_t,version::nFeatures> dxvkVersions;
    for(size_t feat = 0; feat < version::nFeatures; ++feat) {
      dxvkVersions[feat] = queryFeatureVersion((version::Feature)feat);
    }
    bool bMismatchDetected = false;
    if(version::messageChannelV != dxvkVersions[version::MessageChannel]) {
      Logger::err(format_string("MessageChannel version mismatch! Bridge: 0x%X, DXVK: 0x%X\n",
                                version::messageChannelV, dxvkVersions[version::MessageChannel]));
      bMismatchDetected = true;
    }
    if(version::fileSysV != dxvkVersions[version::FileSys]) {
      Logger::err(format_string("FileSys version mismatch! Bridge: 0x%X, DXVK: 0x%X\n",
                                version::fileSysV, dxvkVersions[version::FileSys]));
      bMismatchDetected = true;
    }
    if(bMismatchDetected) {
      Logger::warn("One or more functional version mismatches detected. If you experience problems, consider updating either bridge or dxvk.");
    } else {
      Logger::info("Feature version parity confirmed!");
    }
  }

  return true;
}

void CALLBACK OnClientExited(void* context, BOOLEAN isTimeout) {
  // DX11_V219_SERVER_REAL_GAME_TARGET_AND_IGNORE_EXIT_CALLBACK
  wchar_t dx11Mode[64] = {};
  if (GetEnvironmentVariableW(L"DX11_BRIDGE_MODE", dx11Mode, _countof(dx11Mode)) > 0) {
    Logger::warn("DX11_V219_SERVER_REAL_GAME_TARGET_AND_IGNORE_EXIT_CALLBACK: ignoring legacy client-exit callback in DX11 bridge mode; server stays alive and waits for IPC/command shutdown.");
    return;
  }
  Logger::err("The client process has unexpectedly exited, shutting down server as well!");
  gbBridgeRunning = false;

  // Log history of recent client side commands sent and received by the server
  Logger::info("Most recent Device Queue commands sent from Client");
  DeviceBridge::Command::print_reader_data_sent();
  Logger::info("Most recent Device Queue commands received by Server");
  DeviceBridge::Command::print_reader_data_received();
  Logger::info("Most recent Module Queue commands sent from Client");
  ModuleBridge::Command::print_reader_data_sent();
  Logger::info("Most recent Module Queue commands received by Server");
  ModuleBridge::Command::print_reader_data_received();

  // Give the server some time to shut down, but then force quit so it doesn't hang forever
  uint32_t numRetries = 0;
  uint32_t maxRetries = ServerOptions::getShutdownRetries();
  uint32_t timeout = ServerOptions::getShutdownTimeout();
  while (ghModule && numRetries++ < maxRetries) {
    Sleep(timeout);
  }
  // We rely on the d3d11 module having been unloaded successfully for this to work
  if (ghModule && numRetries >= maxRetries) {
    // Terminate is stronger than ExitProcess in case some thread doesn't cleanly exit
    TerminateProcess(GetCurrentProcess(), 1);
  }
}

bool RegisterExitCallback(const uint32_t hProcess) {
  BOOL result = RegisterWaitForSingleObject(&hWait, TRUNCATE_HANDLE(HANDLE, hProcess), OnClientExited, NULL, INFINITE, WT_EXECUTEONLYONCE);
  if (!result) {
    DWORD error = GetLastError();
    Logger::err(format_string("RegisterExitCallback() failed with error code %d", error));

    const auto timeClientEnd = std::chrono::high_resolution_clock::now();
    std::stringstream uptimeSS;
    uptimeSS << "[Uptime] Client (estimated): ";
    uptimeSS <<
      std::chrono::duration_cast<std::chrono::seconds>(timeClientEnd - gTimeStart).count();
    uptimeSS << "s";
    Logger::info(uptimeSS.str());
  }

  return result;
}

static bool RegisterMessageChannel() {
  Logger::info("Registering message channel for asynchronous message handling.");

  gpClientMessageChannel = std::make_unique<MessageChannelServer>("MessageChannelServer");

  if (!gpClientMessageChannel->init(nullptr, nullptr)) {
    Logger::err("Unable to register message channel.");
    return false;
  }

  gpClientMessageChannel->registerHandler(WM_KILLFOCUS, [](uint32_t, uint32_t) {
    Logger::info("Client window became inactive, disabling timeouts for bridge server...");
    GlobalOptions::setInfiniteRetries(true);
    return true;
  });

  gpClientMessageChannel->registerHandler(WM_SETFOCUS, [](uint32_t, uint32_t) {
    Logger::info("Client window became active, reenabling timeouts for bridge server!");
    GlobalOptions::setInfiniteRetries(false);
    return true;
  });

  return true;
}

static inline bool initFileSys() {
  // DX11_V219_SERVER_REAL_GAME_TARGET_AND_IGNORE_EXIT_CALLBACK
  // In the DX11 bridge chain the x64 server is launched by NvRemixLauncher32.exe,
  // so parent-process based discovery points at the launcher.  Remix config and
  // filesystem roots must use the real already-running game exe instead.
  wchar_t dx11Mode[64] = {};
  const bool isDx11BridgeMode = GetEnvironmentVariableW(L"DX11_BRIDGE_MODE", dx11Mode, _countof(dx11Mode)) > 0;

  if (isDx11BridgeMode) {
    wchar_t gameExe[MAX_PATH] = {};
    DWORD got = GetEnvironmentVariableW(L"DX11_BRIDGE_REAL_GAME_EXE", gameExe, _countof(gameExe));
    if (got == 0 || got >= _countof(gameExe)) {
      got = GetEnvironmentVariableW(L"DXVK_REMIX_REAL_GAME_EXE", gameExe, _countof(gameExe));
    }

    if (got > 0 && got < _countof(gameExe)) {
      const fspath realGamePath(gameExe);
      const auto realGameDir = realGamePath.parent_path();
      dxvk::util::RtxFileSys::init(realGameDir.string());
      Logger::info(std::string("DX11_V219_SERVER_REAL_GAME_TARGET_AND_IGNORE_EXIT_CALLBACK: server filesystem/config target uses real game exe: ") + realGamePath.string());
      return true;
    }

    Logger::warn("DX11_V219_SERVER_REAL_GAME_TARGET_AND_IGNORE_EXIT_CALLBACK: DX11 mode active but real game exe env missing; falling back to parent process path.");
  }

  auto parentPid = bridge_util::getParentPid();
  DWORD accessRights = PROCESS_QUERY_INFORMATION | PROCESS_VM_READ;
  HANDLE processHandle = OpenProcess(accessRights, FALSE, parentPid);

  {
    auto executable32PathVec = createPathVec();
    if(GetModuleFileNameEx(processHandle, NULL, executable32PathVec.data(), executable32PathVec.size()) == 0) {
      Logger::err("Failed to find executable path!");
      if (processHandle) {
        CloseHandle(processHandle);
      }
      return false;
    }

    const fspath executable32Path(executable32PathVec.data());
    const auto exe32Dir = executable32Path.parent_path();
    dxvk::util::RtxFileSys::init(exe32Dir.string());
  }

  if (processHandle) {
    CloseHandle(processHandle);
  }

  return true;
}

// DX11_BRIDGE_ARG_FALLBACK_V63
// The stock bridge server expects wWinMain pCmdLine to contain exactly:
//   <36-char-guid> <BRIDGE_VERSION_W>
// For DX11 bridge bring-up we also accept DX11_BRIDGE_GUID/DX11_BRIDGE_VERSION
// from the x86 launcher environment, and .trex\dx11_bridge_args.txt as a file
// fallback. This prevents the server from exiting before the DX11 client IPC path
// can be brought up.
static wchar_t g_Dx11BridgeGuidArgV63[64] = {};
static wchar_t g_Dx11BridgeVersionArgV63[256] = {};
static LPWSTR g_Dx11BridgeArgListV63[2] = { g_Dx11BridgeGuidArgV63, g_Dx11BridgeVersionArgV63 };

static bool Dx11BridgeReadEnvArgV63(const wchar_t* name, wchar_t* out, DWORD cap) {
  if (!name || !out || cap == 0) return false;
  out[0] = 0;
  const DWORD got = GetEnvironmentVariableW(name, out, cap);
  return got > 0 && got < cap && out[0] != 0;
}

static bool Dx11BridgeReadArgsFileV63(wchar_t* guidOut, DWORD guidCap, wchar_t* verOut, DWORD verCap) {
  wchar_t path[MAX_PATH] = {};
  if (GetModuleFileNameW(nullptr, path, MAX_PATH) == 0) return false;
  wchar_t* slash = wcsrchr(path, L'\\');
  if (!slash) return false;
  slash[1] = 0;
  if (wcslen(path) + wcslen(L"dx11_bridge_args.txt") + 1 >= MAX_PATH) return false;
  wcscat_s(path, L"dx11_bridge_args.txt");

  HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) {
    if (GetModuleFileNameW(nullptr, path, MAX_PATH) == 0) return false;
    slash = wcsrchr(path, L'\\');
    if (!slash) return false;
    slash[1] = 0;
    if (wcslen(path) + wcslen(L".trex\\dx11_bridge_args.txt") + 1 >= MAX_PATH) return false;
    wcscat_s(path, L".trex\\dx11_bridge_args.txt");
    h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
  }

  char buf[512] = {};
  DWORD got = 0;
  BOOL ok = ReadFile(h, buf, sizeof(buf) - 1, &got, nullptr);
  CloseHandle(h);
  if (!ok || got == 0) return false;
  buf[got] = 0;

  char* first = buf;
  while (*first == ' ' || *first == '\t' || *first == '\r' || *first == '\n') ++first;
  char* second = first;
  while (*second && *second != '\r' && *second != '\n') ++second;
  if (*second) *second++ = 0;
  while (*second == ' ' || *second == '\t' || *second == '\r' || *second == '\n') ++second;
  char* end2 = second;
  while (*end2 && *end2 != '\r' && *end2 != '\n') ++end2;
  *end2 = 0;
  if (!first[0] || !second[0]) return false;

  MultiByteToWideChar(CP_ACP, 0, first, -1, guidOut, guidCap);
  MultiByteToWideChar(CP_ACP, 0, second, -1, verOut, verCap);
  return guidOut[0] != 0 && verOut[0] != 0;
}

static LPWSTR* Dx11BridgeBuildFallbackArgListV63(int* pArgCount) {
  if (!pArgCount) return nullptr;
  *pArgCount = 0;
  bool gotGuid = Dx11BridgeReadEnvArgV63(L"DX11_BRIDGE_GUID", g_Dx11BridgeGuidArgV63, _countof(g_Dx11BridgeGuidArgV63));
  bool gotVer = Dx11BridgeReadEnvArgV63(L"DX11_BRIDGE_VERSION", g_Dx11BridgeVersionArgV63, _countof(g_Dx11BridgeVersionArgV63));
  if (!gotGuid || !gotVer) {
    gotGuid = gotVer = Dx11BridgeReadArgsFileV63(g_Dx11BridgeGuidArgV63, _countof(g_Dx11BridgeGuidArgV63), g_Dx11BridgeVersionArgV63, _countof(g_Dx11BridgeVersionArgV63));
  }
  if (!gotGuid || !gotVer) return nullptr;
  *pArgCount = 2;
  return g_Dx11BridgeArgListV63;
}

#ifndef DX11_V180_SERVER_REMIX_INITIALIZER
#define DX11_V180_SERVER_REMIX_INITIALIZER
static HMODULE gDx11BridgeTrexDxgiV180 = nullptr;
static HMODULE gDx11BridgeTrexD3D11V180 = nullptr;

static void Dx11BridgeServerInitializeTrexRemixRuntimeV180() {
  wchar_t mode[64] = {};
  GetEnvironmentVariableW(L"DX11_BRIDGE_MODE", mode, _countof(mode));

  wchar_t exePath[MAX_PATH] = {};
  if (GetModuleFileNameW(nullptr, exePath, _countof(exePath)) == 0) {
    Logger::warn("DX11_V180_SERVER_REMIX_INITIALIZER: could not resolve NvRemixBridge.exe path.");
    return;
  }

  wchar_t* slash = wcsrchr(exePath, L'\\');
  if (!slash) {
    Logger::warn("DX11_V180_SERVER_REMIX_INITIALIZER: could not resolve .trex folder.");
    return;
  }
  slash[1] = 0;

  SetDllDirectoryW(exePath);
  SetEnvironmentVariableW(L"DXVK_REMIX_TREX_DIR", exePath);
  SetEnvironmentVariableW(L"DXVK_REMIX_BRIDGE_SERVER_HOSTS_RUNTIME", L"1");

  wchar_t dxgiPath[MAX_PATH] = {};
  wchar_t d3d11Path[MAX_PATH] = {};
  wcscpy_s(dxgiPath, exePath);
  wcscat_s(dxgiPath, L"dxgi.dll");
  wcscpy_s(d3d11Path, exePath);
  wcscat_s(d3d11Path, L"d3d11.dll");

  gDx11BridgeTrexDxgiV180 = LoadLibraryW(dxgiPath);
  if (gDx11BridgeTrexDxgiV180) {
    Logger::info("DX11_V180_SERVER_REMIX_INITIALIZER: loaded .trex dxgi.dll into NvRemixBridge.exe.");
    FARPROC f0 = GetProcAddress(gDx11BridgeTrexDxgiV180, "CreateDXGIFactory");
    FARPROC f1 = GetProcAddress(gDx11BridgeTrexDxgiV180, "CreateDXGIFactory1");
    FARPROC f2 = GetProcAddress(gDx11BridgeTrexDxgiV180, "CreateDXGIFactory2");
    if (f0 || f1 || f2) {
      Logger::info("DX11_V180_SERVER_REMIX_INITIALIZER: .trex dxgi.dll exports DXGI factory entrypoints.");
    } else {
      Logger::warn("DX11_V180_SERVER_REMIX_INITIALIZER: .trex dxgi.dll loaded but no DXGI factory exports were found.");
    }
  } else {
    Logger::warn("DX11_V180_SERVER_REMIX_INITIALIZER: failed to load .trex dxgi.dll into NvRemixBridge.exe.");
  }

  gDx11BridgeTrexD3D11V180 = LoadLibraryW(d3d11Path);
  if (gDx11BridgeTrexD3D11V180) {
    Logger::info("DX11_V180_SERVER_REMIX_INITIALIZER: loaded .trex d3d11.dll into NvRemixBridge.exe.");
    FARPROC d0 = GetProcAddress(gDx11BridgeTrexD3D11V180, "D3D11CreateDevice");
    FARPROC d1 = GetProcAddress(gDx11BridgeTrexD3D11V180, "D3D11CreateDeviceAndSwapChain");
    if (d0 || d1) {
      Logger::info("DX11_V180_SERVER_REMIX_INITIALIZER: .trex d3d11.dll exports D3D11 device entrypoints.");
    } else {
      Logger::warn("DX11_V180_SERVER_REMIX_INITIALIZER: .trex d3d11.dll loaded but no D3D11 entrypoints were found.");
    }
  } else {
    Logger::warn("DX11_V180_SERVER_REMIX_INITIALIZER: failed to load .trex d3d11.dll into NvRemixBridge.exe.");
  }
}
#endif

#ifndef DX11_V180_FORCE_TREX_DX11_RUNTIME
#define DX11_V180_FORCE_TREX_DX11_RUNTIME
static HMODULE gDx11BridgeForcedTrexDxgiV180 = nullptr;
static HMODULE gDx11BridgeForcedTrexD3D11V180 = nullptr;

static bool Dx11BridgeForceTrexRuntimePathExistsV180(const wchar_t* path) {
  const DWORD attrs = GetFileAttributesW(path);
  return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
}

static bool Dx11BridgeForceGetTrexDirV180(wchar_t* out, DWORD cap) {
  if (!out || cap == 0) return false;
  out[0] = 0;

  wchar_t envTrex[MAX_PATH] = {};
  DWORD got = GetEnvironmentVariableW(L"DXVK_REMIX_TREX_DIR", envTrex, _countof(envTrex));
  if (got > 0 && got < _countof(envTrex)) {
    lstrcpynW(out, envTrex, cap);
  } else {
    wchar_t exePath[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, exePath, _countof(exePath)) == 0) {
      return false;
    }
    wchar_t* slash = wcsrchr(exePath, L'\\');
    if (!slash) {
      return false;
    }
    slash[1] = 0;
    lstrcpynW(out, exePath, cap);
  }

  size_t len = wcslen(out);
  if (len > 0 && out[len - 1] != L'\\' && len + 2 < cap) {
    out[len] = L'\\';
    out[len + 1] = 0;
  }

  return out[0] != 0;
}

static bool Dx11BridgeServerForceLoadTrexDx11RuntimeV180() {
  wchar_t mode[64] = {};
  if (GetEnvironmentVariableW(L"DX11_BRIDGE_MODE", mode, _countof(mode)) == 0) {
    return true;
  }

  wchar_t trexDir[MAX_PATH] = {};
  if (!Dx11BridgeForceGetTrexDirV180(trexDir, _countof(trexDir))) {
    Logger::err("DX11_V180_FORCE_TREX_DX11_RUNTIME: FAILED: could not resolve .trex directory.");
    return false;
  }

  SetDllDirectoryW(trexDir);

  wchar_t dxgiPath[MAX_PATH] = {};
  wchar_t d3d11Path[MAX_PATH] = {};
  wcscpy_s(dxgiPath, trexDir);
  wcscat_s(dxgiPath, L"dxgi.dll");
  wcscpy_s(d3d11Path, trexDir);
  wcscat_s(d3d11Path, L"d3d11.dll");

  if (!Dx11BridgeForceTrexRuntimePathExistsV180(dxgiPath)) {
    Logger::err("DX11_V180_FORCE_TREX_DX11_RUNTIME: FAILED: .trex\\dxgi.dll is missing.");
    return false;
  }

  if (!Dx11BridgeForceTrexRuntimePathExistsV180(d3d11Path)) {
    Logger::err("DX11_V180_FORCE_TREX_DX11_RUNTIME: FAILED: .trex\\d3d11.dll is missing.");
    return false;
  }

  gDx11BridgeForcedTrexDxgiV180 = LoadLibraryExW(dxgiPath, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
  if (!gDx11BridgeForcedTrexDxgiV180) {
    Logger::err("DX11_V180_FORCE_TREX_DX11_RUNTIME: FAILED: LoadLibraryExW(.trex\\dxgi.dll) failed.");
    return false;
  }

  Logger::info("DX11_V180_FORCE_TREX_DX11_RUNTIME: forced .trex\\dxgi.dll loaded into NvRemixBridge.exe.");

  gDx11BridgeForcedTrexD3D11V180 = LoadLibraryExW(d3d11Path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
  if (!gDx11BridgeForcedTrexD3D11V180) {
    Logger::err("DX11_V180_FORCE_TREX_DX11_RUNTIME: FAILED: LoadLibraryExW(.trex\\d3d11.dll) failed.");
    return false;
  }

  Logger::info("DX11_V180_FORCE_TREX_DX11_RUNTIME: forced .trex\\d3d11.dll loaded into NvRemixBridge.exe.");

  FARPROC createFactory = GetProcAddress(gDx11BridgeForcedTrexDxgiV180, "CreateDXGIFactory");
  FARPROC createFactory1 = GetProcAddress(gDx11BridgeForcedTrexDxgiV180, "CreateDXGIFactory1");
  FARPROC createFactory2 = GetProcAddress(gDx11BridgeForcedTrexDxgiV180, "CreateDXGIFactory2");
  FARPROC createDevice = GetProcAddress(gDx11BridgeForcedTrexD3D11V180, "D3D11CreateDevice");
  FARPROC createDeviceAndSwapChain = GetProcAddress(gDx11BridgeForcedTrexD3D11V180, "D3D11CreateDeviceAndSwapChain");

  if (!createDevice) {
    Logger::err("DX11_V180_FORCE_TREX_DX11_RUNTIME: FAILED: forced .trex\\d3d11.dll lacks D3D11CreateDevice.");
    return false;
  }

  Logger::info("DX11_V180_FORCE_TREX_DX11_RUNTIME: .trex\\d3d11.dll exports D3D11CreateDevice.");

  if (createDeviceAndSwapChain) {
    Logger::info("DX11_V180_FORCE_TREX_DX11_RUNTIME: .trex\\d3d11.dll exports D3D11CreateDeviceAndSwapChain.");
  } else {
    Logger::warn("DX11_V180_FORCE_TREX_DX11_RUNTIME: .trex\\d3d11.dll lacks D3D11CreateDeviceAndSwapChain.");
  }

  if (createFactory || createFactory1 || createFactory2) {
    Logger::info("DX11_V180_FORCE_TREX_DX11_RUNTIME: .trex\\dxgi.dll exports DXGI factory entrypoints.");
  } else {
    Logger::warn("DX11_V180_FORCE_TREX_DX11_RUNTIME: .trex\\dxgi.dll lacks DXGI factory exports.");
  }

  SetEnvironmentVariableW(L"DX11_BRIDGE_TREX_D3D11_FORCED", L"1");
  SetEnvironmentVariableW(L"DX11_BRIDGE_TREX_DXGI_FORCED", L"1");
  return true;
}
#endif

#ifndef DX11_V186_SERVER_REMIX_INITIALIZER
#define DX11_V186_SERVER_REMIX_INITIALIZER
static HMODULE gDx11BridgeTrexDxgiV186 = nullptr;
static HMODULE gDx11BridgeTrexD3D11V186 = nullptr;

static void Dx11BridgeServerInitializeTrexRemixRuntimeV186() {
  wchar_t mode[64] = {};
  GetEnvironmentVariableW(L"DX11_BRIDGE_MODE", mode, _countof(mode));

  wchar_t exePath[MAX_PATH] = {};
  if (GetModuleFileNameW(nullptr, exePath, _countof(exePath)) == 0) {
    Logger::warn("DX11_V186_SERVER_REMIX_INITIALIZER: could not resolve NvRemixBridge.exe path.");
    return;
  }

  wchar_t* slash = wcsrchr(exePath, L'\\');
  if (!slash) {
    Logger::warn("DX11_V186_SERVER_REMIX_INITIALIZER: could not resolve .trex folder.");
    return;
  }
  slash[1] = 0;

  SetDllDirectoryW(exePath);
  SetEnvironmentVariableW(L"DXVK_REMIX_TREX_DIR", exePath);
  SetEnvironmentVariableW(L"DXVK_REMIX_BRIDGE_SERVER_HOSTS_RUNTIME", L"1");

  wchar_t dxgiPath[MAX_PATH] = {};
  wchar_t d3d11Path[MAX_PATH] = {};
  wcscpy_s(dxgiPath, exePath);
  wcscat_s(dxgiPath, L"dxgi.dll");
  wcscpy_s(d3d11Path, exePath);
  wcscat_s(d3d11Path, L"d3d11.dll");

  gDx11BridgeTrexDxgiV186 = LoadLibraryW(dxgiPath);
  if (gDx11BridgeTrexDxgiV186) {
    Logger::info("DX11_V186_SERVER_REMIX_INITIALIZER: loaded .trex dxgi.dll into NvRemixBridge.exe.");
    FARPROC f0 = GetProcAddress(gDx11BridgeTrexDxgiV186, "CreateDXGIFactory");
    FARPROC f1 = GetProcAddress(gDx11BridgeTrexDxgiV186, "CreateDXGIFactory1");
    FARPROC f2 = GetProcAddress(gDx11BridgeTrexDxgiV186, "CreateDXGIFactory2");
    if (f0 || f1 || f2) {
      Logger::info("DX11_V186_SERVER_REMIX_INITIALIZER: .trex dxgi.dll exports DXGI factory entrypoints.");
    } else {
      Logger::warn("DX11_V186_SERVER_REMIX_INITIALIZER: .trex dxgi.dll loaded but no DXGI factory exports were found.");
    }
  } else {
    Logger::warn("DX11_V186_SERVER_REMIX_INITIALIZER: failed to load .trex dxgi.dll into NvRemixBridge.exe.");
  }

  gDx11BridgeTrexD3D11V186 = LoadLibraryW(d3d11Path);
  if (gDx11BridgeTrexD3D11V186) {
    Logger::info("DX11_V186_SERVER_REMIX_INITIALIZER: loaded .trex d3d11.dll into NvRemixBridge.exe.");
    FARPROC d0 = GetProcAddress(gDx11BridgeTrexD3D11V186, "D3D11CreateDevice");
    FARPROC d1 = GetProcAddress(gDx11BridgeTrexD3D11V186, "D3D11CreateDeviceAndSwapChain");
    if (d0 || d1) {
      Logger::info("DX11_V186_SERVER_REMIX_INITIALIZER: .trex d3d11.dll exports D3D11 device entrypoints.");
    } else {
      Logger::warn("DX11_V186_SERVER_REMIX_INITIALIZER: .trex d3d11.dll loaded but no D3D11 entrypoints were found.");
    }
  } else {
    Logger::warn("DX11_V186_SERVER_REMIX_INITIALIZER: failed to load .trex d3d11.dll into NvRemixBridge.exe.");
  }
}
#endif

#ifndef DX11_V186_FORCE_TREX_DX11_RUNTIME
#define DX11_V186_FORCE_TREX_DX11_RUNTIME
static HMODULE gDx11BridgeForcedTrexDxgiV186 = nullptr;
static HMODULE gDx11BridgeForcedTrexD3D11V186 = nullptr;

static bool Dx11BridgeForceTrexRuntimePathExistsV186(const wchar_t* path) {
  const DWORD attrs = GetFileAttributesW(path);
  return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
}

static bool Dx11BridgeForceGetTrexDirV186(wchar_t* out, DWORD cap) {
  if (!out || cap == 0) return false;
  out[0] = 0;

  wchar_t envTrex[MAX_PATH] = {};
  DWORD got = GetEnvironmentVariableW(L"DXVK_REMIX_TREX_DIR", envTrex, _countof(envTrex));
  if (got > 0 && got < _countof(envTrex)) {
    lstrcpynW(out, envTrex, cap);
  } else {
    wchar_t exePath[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, exePath, _countof(exePath)) == 0) {
      return false;
    }
    wchar_t* slash = wcsrchr(exePath, L'\\');
    if (!slash) {
      return false;
    }
    slash[1] = 0;
    lstrcpynW(out, exePath, cap);
  }

  size_t len = wcslen(out);
  if (len > 0 && out[len - 1] != L'\\' && len + 2 < cap) {
    out[len] = L'\\';
    out[len + 1] = 0;
  }

  return out[0] != 0;
}

static bool Dx11BridgeServerForceLoadTrexDx11RuntimeV186() {
  wchar_t mode[64] = {};
  if (GetEnvironmentVariableW(L"DX11_BRIDGE_MODE", mode, _countof(mode)) == 0) {
    return true;
  }

  wchar_t trexDir[MAX_PATH] = {};
  if (!Dx11BridgeForceGetTrexDirV186(trexDir, _countof(trexDir))) {
    Logger::err("DX11_V186_FORCE_TREX_DX11_RUNTIME: FAILED: could not resolve .trex directory.");
    return false;
  }

  SetDllDirectoryW(trexDir);

  wchar_t dxgiPath[MAX_PATH] = {};
  wchar_t d3d11Path[MAX_PATH] = {};
  wcscpy_s(dxgiPath, trexDir);
  wcscat_s(dxgiPath, L"dxgi.dll");
  wcscpy_s(d3d11Path, trexDir);
  wcscat_s(d3d11Path, L"d3d11.dll");

  if (!Dx11BridgeForceTrexRuntimePathExistsV186(dxgiPath)) {
    Logger::err("DX11_V186_FORCE_TREX_DX11_RUNTIME: FAILED: .trex\\dxgi.dll is missing.");
    return false;
  }

  if (!Dx11BridgeForceTrexRuntimePathExistsV186(d3d11Path)) {
    Logger::err("DX11_V186_FORCE_TREX_DX11_RUNTIME: FAILED: .trex\\d3d11.dll is missing.");
    return false;
  }

  gDx11BridgeForcedTrexDxgiV186 = LoadLibraryExW(dxgiPath, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
  if (!gDx11BridgeForcedTrexDxgiV186) {
    Logger::err("DX11_V186_FORCE_TREX_DX11_RUNTIME: FAILED: LoadLibraryExW(.trex\\dxgi.dll) failed.");
    return false;
  }

  Logger::info("DX11_V186_FORCE_TREX_DX11_RUNTIME: forced .trex\\dxgi.dll loaded into NvRemixBridge.exe.");

  gDx11BridgeForcedTrexD3D11V186 = LoadLibraryExW(d3d11Path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
  if (!gDx11BridgeForcedTrexD3D11V186) {
    Logger::err("DX11_V186_FORCE_TREX_DX11_RUNTIME: FAILED: LoadLibraryExW(.trex\\d3d11.dll) failed.");
    return false;
  }

  Logger::info("DX11_V186_FORCE_TREX_DX11_RUNTIME: forced .trex\\d3d11.dll loaded into NvRemixBridge.exe.");

  FARPROC createFactory = GetProcAddress(gDx11BridgeForcedTrexDxgiV186, "CreateDXGIFactory");
  FARPROC createFactory1 = GetProcAddress(gDx11BridgeForcedTrexDxgiV186, "CreateDXGIFactory1");
  FARPROC createFactory2 = GetProcAddress(gDx11BridgeForcedTrexDxgiV186, "CreateDXGIFactory2");
  FARPROC createDevice = GetProcAddress(gDx11BridgeForcedTrexD3D11V186, "D3D11CreateDevice");
  FARPROC createDeviceAndSwapChain = GetProcAddress(gDx11BridgeForcedTrexD3D11V186, "D3D11CreateDeviceAndSwapChain");

  if (!createDevice) {
    Logger::err("DX11_V186_FORCE_TREX_DX11_RUNTIME: FAILED: forced .trex\\d3d11.dll lacks D3D11CreateDevice.");
    return false;
  }

  Logger::info("DX11_V186_FORCE_TREX_DX11_RUNTIME: .trex\\d3d11.dll exports D3D11CreateDevice.");

  if (createDeviceAndSwapChain) {
    Logger::info("DX11_V186_FORCE_TREX_DX11_RUNTIME: .trex\\d3d11.dll exports D3D11CreateDeviceAndSwapChain.");
  } else {
    Logger::warn("DX11_V186_FORCE_TREX_DX11_RUNTIME: .trex\\d3d11.dll lacks D3D11CreateDeviceAndSwapChain.");
  }

  if (createFactory || createFactory1 || createFactory2) {
    Logger::info("DX11_V186_FORCE_TREX_DX11_RUNTIME: .trex\\dxgi.dll exports DXGI factory entrypoints.");
  } else {
    Logger::warn("DX11_V186_FORCE_TREX_DX11_RUNTIME: .trex\\dxgi.dll lacks DXGI factory exports.");
  }

  SetEnvironmentVariableW(L"DX11_BRIDGE_TREX_D3D11_FORCED", L"1");
  SetEnvironmentVariableW(L"DX11_BRIDGE_TREX_DXGI_FORCED", L"1");
  return true;
}
#endif

#ifndef DX11_V187_SERVER_REMIX_INITIALIZER
#define DX11_V187_SERVER_REMIX_INITIALIZER
static HMODULE gDx11BridgeTrexDxgiV187 = nullptr;
static HMODULE gDx11BridgeTrexD3D11V187 = nullptr;

static void Dx11BridgeServerInitializeTrexRemixRuntimeV187() {
  wchar_t mode[64] = {};
  GetEnvironmentVariableW(L"DX11_BRIDGE_MODE", mode, _countof(mode));

  wchar_t exePath[MAX_PATH] = {};
  if (GetModuleFileNameW(nullptr, exePath, _countof(exePath)) == 0) {
    Logger::warn("DX11_V187_SERVER_REMIX_INITIALIZER: could not resolve NvRemixBridge.exe path.");
    return;
  }

  wchar_t* slash = wcsrchr(exePath, L'\\');
  if (!slash) {
    Logger::warn("DX11_V187_SERVER_REMIX_INITIALIZER: could not resolve .trex folder.");
    return;
  }
  slash[1] = 0;

  SetDllDirectoryW(exePath);
  SetEnvironmentVariableW(L"DXVK_REMIX_TREX_DIR", exePath);
  SetEnvironmentVariableW(L"DXVK_REMIX_BRIDGE_SERVER_HOSTS_RUNTIME", L"1");

  wchar_t dxgiPath[MAX_PATH] = {};
  wchar_t d3d11Path[MAX_PATH] = {};
  wcscpy_s(dxgiPath, exePath);
  wcscat_s(dxgiPath, L"dxgi.dll");
  wcscpy_s(d3d11Path, exePath);
  wcscat_s(d3d11Path, L"d3d11.dll");

  gDx11BridgeTrexDxgiV187 = LoadLibraryW(dxgiPath);
  if (gDx11BridgeTrexDxgiV187) {
    Logger::info("DX11_V187_SERVER_REMIX_INITIALIZER: loaded .trex dxgi.dll into NvRemixBridge.exe.");
    FARPROC f0 = GetProcAddress(gDx11BridgeTrexDxgiV187, "CreateDXGIFactory");
    FARPROC f1 = GetProcAddress(gDx11BridgeTrexDxgiV187, "CreateDXGIFactory1");
    FARPROC f2 = GetProcAddress(gDx11BridgeTrexDxgiV187, "CreateDXGIFactory2");
    if (f0 || f1 || f2) {
      Logger::info("DX11_V187_SERVER_REMIX_INITIALIZER: .trex dxgi.dll exports DXGI factory entrypoints.");
    } else {
      Logger::warn("DX11_V187_SERVER_REMIX_INITIALIZER: .trex dxgi.dll loaded but no DXGI factory exports were found.");
    }
  } else {
    Logger::warn("DX11_V187_SERVER_REMIX_INITIALIZER: failed to load .trex dxgi.dll into NvRemixBridge.exe.");
  }

  gDx11BridgeTrexD3D11V187 = LoadLibraryW(d3d11Path);
  if (gDx11BridgeTrexD3D11V187) {
    Logger::info("DX11_V187_SERVER_REMIX_INITIALIZER: loaded .trex d3d11.dll into NvRemixBridge.exe.");
    FARPROC d0 = GetProcAddress(gDx11BridgeTrexD3D11V187, "D3D11CreateDevice");
    FARPROC d1 = GetProcAddress(gDx11BridgeTrexD3D11V187, "D3D11CreateDeviceAndSwapChain");
    if (d0 || d1) {
      Logger::info("DX11_V187_SERVER_REMIX_INITIALIZER: .trex d3d11.dll exports D3D11 device entrypoints.");
    } else {
      Logger::warn("DX11_V187_SERVER_REMIX_INITIALIZER: .trex d3d11.dll loaded but no D3D11 entrypoints were found.");
    }
  } else {
    Logger::warn("DX11_V187_SERVER_REMIX_INITIALIZER: failed to load .trex d3d11.dll into NvRemixBridge.exe.");
  }
}
#endif

#ifndef DX11_V187_FORCE_TREX_DX11_RUNTIME
#define DX11_V187_FORCE_TREX_DX11_RUNTIME
static HMODULE gDx11BridgeForcedTrexDxgiV187 = nullptr;
static HMODULE gDx11BridgeForcedTrexD3D11V187 = nullptr;

static bool Dx11BridgeForceTrexRuntimePathExistsV187(const wchar_t* path) {
  const DWORD attrs = GetFileAttributesW(path);
  return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
}

static bool Dx11BridgeForceGetTrexDirV187(wchar_t* out, DWORD cap) {
  if (!out || cap == 0) return false;
  out[0] = 0;

  wchar_t envTrex[MAX_PATH] = {};
  DWORD got = GetEnvironmentVariableW(L"DXVK_REMIX_TREX_DIR", envTrex, _countof(envTrex));
  if (got > 0 && got < _countof(envTrex)) {
    lstrcpynW(out, envTrex, cap);
  } else {
    wchar_t exePath[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, exePath, _countof(exePath)) == 0) {
      return false;
    }
    wchar_t* slash = wcsrchr(exePath, L'\\');
    if (!slash) {
      return false;
    }
    slash[1] = 0;
    lstrcpynW(out, exePath, cap);
  }

  size_t len = wcslen(out);
  if (len > 0 && out[len - 1] != L'\\' && len + 2 < cap) {
    out[len] = L'\\';
    out[len + 1] = 0;
  }

  return out[0] != 0;
}

static bool Dx11BridgeServerForceLoadTrexDx11RuntimeV187() {
  wchar_t mode[64] = {};
  if (GetEnvironmentVariableW(L"DX11_BRIDGE_MODE", mode, _countof(mode)) == 0) {
    return true;
  }

  wchar_t trexDir[MAX_PATH] = {};
  if (!Dx11BridgeForceGetTrexDirV187(trexDir, _countof(trexDir))) {
    Logger::err("DX11_V187_FORCE_TREX_DX11_RUNTIME: FAILED: could not resolve .trex directory.");
    return false;
  }

  SetDllDirectoryW(trexDir);

  wchar_t dxgiPath[MAX_PATH] = {};
  wchar_t d3d11Path[MAX_PATH] = {};
  wcscpy_s(dxgiPath, trexDir);
  wcscat_s(dxgiPath, L"dxgi.dll");
  wcscpy_s(d3d11Path, trexDir);
  wcscat_s(d3d11Path, L"d3d11.dll");

  if (!Dx11BridgeForceTrexRuntimePathExistsV187(dxgiPath)) {
    Logger::err("DX11_V187_FORCE_TREX_DX11_RUNTIME: FAILED: .trex\\dxgi.dll is missing.");
    return false;
  }

  if (!Dx11BridgeForceTrexRuntimePathExistsV187(d3d11Path)) {
    Logger::err("DX11_V187_FORCE_TREX_DX11_RUNTIME: FAILED: .trex\\d3d11.dll is missing.");
    return false;
  }

  gDx11BridgeForcedTrexDxgiV187 = LoadLibraryExW(dxgiPath, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
  if (!gDx11BridgeForcedTrexDxgiV187) {
    Logger::err("DX11_V187_FORCE_TREX_DX11_RUNTIME: FAILED: LoadLibraryExW(.trex\\dxgi.dll) failed.");
    return false;
  }

  Logger::info("DX11_V187_FORCE_TREX_DX11_RUNTIME: forced .trex\\dxgi.dll loaded into NvRemixBridge.exe.");

  gDx11BridgeForcedTrexD3D11V187 = LoadLibraryExW(d3d11Path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
  if (!gDx11BridgeForcedTrexD3D11V187) {
    Logger::err("DX11_V187_FORCE_TREX_DX11_RUNTIME: FAILED: LoadLibraryExW(.trex\\d3d11.dll) failed.");
    return false;
  }

  Logger::info("DX11_V187_FORCE_TREX_DX11_RUNTIME: forced .trex\\d3d11.dll loaded into NvRemixBridge.exe.");

  FARPROC createFactory = GetProcAddress(gDx11BridgeForcedTrexDxgiV187, "CreateDXGIFactory");
  FARPROC createFactory1 = GetProcAddress(gDx11BridgeForcedTrexDxgiV187, "CreateDXGIFactory1");
  FARPROC createFactory2 = GetProcAddress(gDx11BridgeForcedTrexDxgiV187, "CreateDXGIFactory2");
  FARPROC createDevice = GetProcAddress(gDx11BridgeForcedTrexD3D11V187, "D3D11CreateDevice");
  FARPROC createDeviceAndSwapChain = GetProcAddress(gDx11BridgeForcedTrexD3D11V187, "D3D11CreateDeviceAndSwapChain");

  if (!createDevice) {
    Logger::err("DX11_V187_FORCE_TREX_DX11_RUNTIME: FAILED: forced .trex\\d3d11.dll lacks D3D11CreateDevice.");
    return false;
  }

  Logger::info("DX11_V187_FORCE_TREX_DX11_RUNTIME: .trex\\d3d11.dll exports D3D11CreateDevice.");

  if (createDeviceAndSwapChain) {
    Logger::info("DX11_V187_FORCE_TREX_DX11_RUNTIME: .trex\\d3d11.dll exports D3D11CreateDeviceAndSwapChain.");
  } else {
    Logger::warn("DX11_V187_FORCE_TREX_DX11_RUNTIME: .trex\\d3d11.dll lacks D3D11CreateDeviceAndSwapChain.");
  }

  if (createFactory || createFactory1 || createFactory2) {
    Logger::info("DX11_V187_FORCE_TREX_DX11_RUNTIME: .trex\\dxgi.dll exports DXGI factory entrypoints.");
  } else {
    Logger::warn("DX11_V187_FORCE_TREX_DX11_RUNTIME: .trex\\dxgi.dll lacks DXGI factory exports.");
  }

  SetEnvironmentVariableW(L"DX11_BRIDGE_TREX_D3D11_FORCED", L"1");
  SetEnvironmentVariableW(L"DX11_BRIDGE_TREX_DXGI_FORCED", L"1");
  return true;
}
#endif

#ifndef DX11_V190_SERVER_REMIX_INITIALIZER
#define DX11_V190_SERVER_REMIX_INITIALIZER
static HMODULE gDx11BridgeTrexDxgiV190 = nullptr;
static HMODULE gDx11BridgeTrexD3D11V190 = nullptr;

static void Dx11BridgeServerInitializeTrexRemixRuntimeV190() {
  wchar_t mode[64] = {};
  GetEnvironmentVariableW(L"DX11_BRIDGE_MODE", mode, _countof(mode));

  wchar_t exePath[MAX_PATH] = {};
  if (GetModuleFileNameW(nullptr, exePath, _countof(exePath)) == 0) {
    Logger::warn("DX11_V190_SERVER_REMIX_INITIALIZER: could not resolve NvRemixBridge.exe path.");
    return;
  }

  wchar_t* slash = wcsrchr(exePath, L'\\');
  if (!slash) {
    Logger::warn("DX11_V190_SERVER_REMIX_INITIALIZER: could not resolve .trex folder.");
    return;
  }
  slash[1] = 0;

  SetDllDirectoryW(exePath);
  SetEnvironmentVariableW(L"DXVK_REMIX_TREX_DIR", exePath);
  SetEnvironmentVariableW(L"DXVK_REMIX_BRIDGE_SERVER_HOSTS_RUNTIME", L"1");

  wchar_t dxgiPath[MAX_PATH] = {};
  wchar_t d3d11Path[MAX_PATH] = {};
  wcscpy_s(dxgiPath, exePath);
  wcscat_s(dxgiPath, L"dxgi.dll");
  wcscpy_s(d3d11Path, exePath);
  wcscat_s(d3d11Path, L"d3d11.dll");

  gDx11BridgeTrexDxgiV190 = LoadLibraryW(dxgiPath);
  if (gDx11BridgeTrexDxgiV190) {
    Logger::info("DX11_V190_SERVER_REMIX_INITIALIZER: loaded .trex dxgi.dll into NvRemixBridge.exe.");
    FARPROC f0 = GetProcAddress(gDx11BridgeTrexDxgiV190, "CreateDXGIFactory");
    FARPROC f1 = GetProcAddress(gDx11BridgeTrexDxgiV190, "CreateDXGIFactory1");
    FARPROC f2 = GetProcAddress(gDx11BridgeTrexDxgiV190, "CreateDXGIFactory2");
    if (f0 || f1 || f2) {
      Logger::info("DX11_V190_SERVER_REMIX_INITIALIZER: .trex dxgi.dll exports DXGI factory entrypoints.");
    } else {
      Logger::warn("DX11_V190_SERVER_REMIX_INITIALIZER: .trex dxgi.dll loaded but no DXGI factory exports were found.");
    }
  } else {
    Logger::warn("DX11_V190_SERVER_REMIX_INITIALIZER: failed to load .trex dxgi.dll into NvRemixBridge.exe.");
  }

  gDx11BridgeTrexD3D11V190 = LoadLibraryW(d3d11Path);
  if (gDx11BridgeTrexD3D11V190) {
    Logger::info("DX11_V190_SERVER_REMIX_INITIALIZER: loaded .trex d3d11.dll into NvRemixBridge.exe.");
    FARPROC d0 = GetProcAddress(gDx11BridgeTrexD3D11V190, "D3D11CreateDevice");
    FARPROC d1 = GetProcAddress(gDx11BridgeTrexD3D11V190, "D3D11CreateDeviceAndSwapChain");
    if (d0 || d1) {
      Logger::info("DX11_V190_SERVER_REMIX_INITIALIZER: .trex d3d11.dll exports D3D11 device entrypoints.");
    } else {
      Logger::warn("DX11_V190_SERVER_REMIX_INITIALIZER: .trex d3d11.dll loaded but no D3D11 entrypoints were found.");
    }
  } else {
    Logger::warn("DX11_V190_SERVER_REMIX_INITIALIZER: failed to load .trex d3d11.dll into NvRemixBridge.exe.");
  }
}
#endif

#ifndef DX11_V190_FORCE_TREX_DX11_RUNTIME
#define DX11_V190_FORCE_TREX_DX11_RUNTIME
static HMODULE gDx11BridgeForcedTrexDxgiV190 = nullptr;
static HMODULE gDx11BridgeForcedTrexD3D11V190 = nullptr;

static bool Dx11BridgeForceTrexRuntimePathExistsV190(const wchar_t* path) {
  const DWORD attrs = GetFileAttributesW(path);
  return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
}

static bool Dx11BridgeForceGetTrexDirV190(wchar_t* out, DWORD cap) {
  if (!out || cap == 0) return false;
  out[0] = 0;

  wchar_t envTrex[MAX_PATH] = {};
  DWORD got = GetEnvironmentVariableW(L"DXVK_REMIX_TREX_DIR", envTrex, _countof(envTrex));
  if (got > 0 && got < _countof(envTrex)) {
    lstrcpynW(out, envTrex, cap);
  } else {
    wchar_t exePath[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, exePath, _countof(exePath)) == 0) {
      return false;
    }
    wchar_t* slash = wcsrchr(exePath, L'\\');
    if (!slash) {
      return false;
    }
    slash[1] = 0;
    lstrcpynW(out, exePath, cap);
  }

  size_t len = wcslen(out);
  if (len > 0 && out[len - 1] != L'\\' && len + 2 < cap) {
    out[len] = L'\\';
    out[len + 1] = 0;
  }

  return out[0] != 0;
}

static bool Dx11BridgeServerForceLoadTrexDx11RuntimeV190() {
  wchar_t mode[64] = {};
  if (GetEnvironmentVariableW(L"DX11_BRIDGE_MODE", mode, _countof(mode)) == 0) {
    return true;
  }

  wchar_t trexDir[MAX_PATH] = {};
  if (!Dx11BridgeForceGetTrexDirV190(trexDir, _countof(trexDir))) {
    Logger::err("DX11_V190_FORCE_TREX_DX11_RUNTIME: FAILED: could not resolve .trex directory.");
    return false;
  }

  SetDllDirectoryW(trexDir);

  wchar_t dxgiPath[MAX_PATH] = {};
  wchar_t d3d11Path[MAX_PATH] = {};
  wcscpy_s(dxgiPath, trexDir);
  wcscat_s(dxgiPath, L"dxgi.dll");
  wcscpy_s(d3d11Path, trexDir);
  wcscat_s(d3d11Path, L"d3d11.dll");

  if (!Dx11BridgeForceTrexRuntimePathExistsV190(dxgiPath)) {
    Logger::err("DX11_V190_FORCE_TREX_DX11_RUNTIME: FAILED: .trex\\dxgi.dll is missing.");
    return false;
  }

  if (!Dx11BridgeForceTrexRuntimePathExistsV190(d3d11Path)) {
    Logger::err("DX11_V190_FORCE_TREX_DX11_RUNTIME: FAILED: .trex\\d3d11.dll is missing.");
    return false;
  }

  gDx11BridgeForcedTrexDxgiV190 = LoadLibraryExW(dxgiPath, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
  if (!gDx11BridgeForcedTrexDxgiV190) {
    Logger::err("DX11_V190_FORCE_TREX_DX11_RUNTIME: FAILED: LoadLibraryExW(.trex\\dxgi.dll) failed.");
    return false;
  }

  Logger::info("DX11_V190_FORCE_TREX_DX11_RUNTIME: forced .trex\\dxgi.dll loaded into NvRemixBridge.exe.");

  gDx11BridgeForcedTrexD3D11V190 = LoadLibraryExW(d3d11Path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
  if (!gDx11BridgeForcedTrexD3D11V190) {
    Logger::err("DX11_V190_FORCE_TREX_DX11_RUNTIME: FAILED: LoadLibraryExW(.trex\\d3d11.dll) failed.");
    return false;
  }

  Logger::info("DX11_V190_FORCE_TREX_DX11_RUNTIME: forced .trex\\d3d11.dll loaded into NvRemixBridge.exe.");

  FARPROC createFactory = GetProcAddress(gDx11BridgeForcedTrexDxgiV190, "CreateDXGIFactory");
  FARPROC createFactory1 = GetProcAddress(gDx11BridgeForcedTrexDxgiV190, "CreateDXGIFactory1");
  FARPROC createFactory2 = GetProcAddress(gDx11BridgeForcedTrexDxgiV190, "CreateDXGIFactory2");
  FARPROC createDevice = GetProcAddress(gDx11BridgeForcedTrexD3D11V190, "D3D11CreateDevice");
  FARPROC createDeviceAndSwapChain = GetProcAddress(gDx11BridgeForcedTrexD3D11V190, "D3D11CreateDeviceAndSwapChain");

  if (!createDevice) {
    Logger::err("DX11_V190_FORCE_TREX_DX11_RUNTIME: FAILED: forced .trex\\d3d11.dll lacks D3D11CreateDevice.");
    return false;
  }

  Logger::info("DX11_V190_FORCE_TREX_DX11_RUNTIME: .trex\\d3d11.dll exports D3D11CreateDevice.");

  if (createDeviceAndSwapChain) {
    Logger::info("DX11_V190_FORCE_TREX_DX11_RUNTIME: .trex\\d3d11.dll exports D3D11CreateDeviceAndSwapChain.");
  } else {
    Logger::warn("DX11_V190_FORCE_TREX_DX11_RUNTIME: .trex\\d3d11.dll lacks D3D11CreateDeviceAndSwapChain.");
  }

  if (createFactory || createFactory1 || createFactory2) {
    Logger::info("DX11_V190_FORCE_TREX_DX11_RUNTIME: .trex\\dxgi.dll exports DXGI factory entrypoints.");
  } else {
    Logger::warn("DX11_V190_FORCE_TREX_DX11_RUNTIME: .trex\\dxgi.dll lacks DXGI factory exports.");
  }

  SetEnvironmentVariableW(L"DX11_BRIDGE_TREX_D3D11_FORCED", L"1");
  SetEnvironmentVariableW(L"DX11_BRIDGE_TREX_DXGI_FORCED", L"1");
  return true;
}
#endif

#ifndef DX11_V191_SERVER_REMIX_INITIALIZER
#define DX11_V191_SERVER_REMIX_INITIALIZER
static HMODULE gDx11BridgeTrexDxgiV191 = nullptr;
static HMODULE gDx11BridgeTrexD3D11V191 = nullptr;

static void Dx11BridgeServerInitializeTrexRemixRuntimeV191() {
  wchar_t mode[64] = {};
  GetEnvironmentVariableW(L"DX11_BRIDGE_MODE", mode, _countof(mode));

  wchar_t exePath[MAX_PATH] = {};
  if (GetModuleFileNameW(nullptr, exePath, _countof(exePath)) == 0) {
    Logger::warn("DX11_V191_SERVER_REMIX_INITIALIZER: could not resolve NvRemixBridge.exe path.");
    return;
  }

  wchar_t* slash = wcsrchr(exePath, L'\\');
  if (!slash) {
    Logger::warn("DX11_V191_SERVER_REMIX_INITIALIZER: could not resolve .trex folder.");
    return;
  }
  slash[1] = 0;

  SetDllDirectoryW(exePath);
  SetEnvironmentVariableW(L"DXVK_REMIX_TREX_DIR", exePath);
  SetEnvironmentVariableW(L"DXVK_REMIX_BRIDGE_SERVER_HOSTS_RUNTIME", L"1");

  wchar_t dxgiPath[MAX_PATH] = {};
  wchar_t d3d11Path[MAX_PATH] = {};
  wcscpy_s(dxgiPath, exePath);
  wcscat_s(dxgiPath, L"dxgi.dll");
  wcscpy_s(d3d11Path, exePath);
  wcscat_s(d3d11Path, L"d3d11.dll");

  gDx11BridgeTrexDxgiV191 = LoadLibraryW(dxgiPath);
  if (gDx11BridgeTrexDxgiV191) {
    Logger::info("DX11_V191_SERVER_REMIX_INITIALIZER: loaded .trex dxgi.dll into NvRemixBridge.exe.");
    FARPROC f0 = GetProcAddress(gDx11BridgeTrexDxgiV191, "CreateDXGIFactory");
    FARPROC f1 = GetProcAddress(gDx11BridgeTrexDxgiV191, "CreateDXGIFactory1");
    FARPROC f2 = GetProcAddress(gDx11BridgeTrexDxgiV191, "CreateDXGIFactory2");
    if (f0 || f1 || f2) {
      Logger::info("DX11_V191_SERVER_REMIX_INITIALIZER: .trex dxgi.dll exports DXGI factory entrypoints.");
    } else {
      Logger::warn("DX11_V191_SERVER_REMIX_INITIALIZER: .trex dxgi.dll loaded but no DXGI factory exports were found.");
    }
  } else {
    Logger::warn("DX11_V191_SERVER_REMIX_INITIALIZER: failed to load .trex dxgi.dll into NvRemixBridge.exe.");
  }

  gDx11BridgeTrexD3D11V191 = LoadLibraryW(d3d11Path);
  if (gDx11BridgeTrexD3D11V191) {
    Logger::info("DX11_V191_SERVER_REMIX_INITIALIZER: loaded .trex d3d11.dll into NvRemixBridge.exe.");
    FARPROC d0 = GetProcAddress(gDx11BridgeTrexD3D11V191, "D3D11CreateDevice");
    FARPROC d1 = GetProcAddress(gDx11BridgeTrexD3D11V191, "D3D11CreateDeviceAndSwapChain");
    if (d0 || d1) {
      Logger::info("DX11_V191_SERVER_REMIX_INITIALIZER: .trex d3d11.dll exports D3D11 device entrypoints.");
    } else {
      Logger::warn("DX11_V191_SERVER_REMIX_INITIALIZER: .trex d3d11.dll loaded but no D3D11 entrypoints were found.");
    }
  } else {
    Logger::warn("DX11_V191_SERVER_REMIX_INITIALIZER: failed to load .trex d3d11.dll into NvRemixBridge.exe.");
  }
}
#endif

#ifndef DX11_V191_FORCE_TREX_DX11_RUNTIME
#define DX11_V191_FORCE_TREX_DX11_RUNTIME
static HMODULE gDx11BridgeForcedTrexDxgiV191 = nullptr;
static HMODULE gDx11BridgeForcedTrexD3D11V191 = nullptr;

static bool Dx11BridgeForceTrexRuntimePathExistsV191(const wchar_t* path) {
  const DWORD attrs = GetFileAttributesW(path);
  return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
}

static bool Dx11BridgeForceGetTrexDirV191(wchar_t* out, DWORD cap) {
  if (!out || cap == 0) return false;
  out[0] = 0;

  wchar_t envTrex[MAX_PATH] = {};
  DWORD got = GetEnvironmentVariableW(L"DXVK_REMIX_TREX_DIR", envTrex, _countof(envTrex));
  if (got > 0 && got < _countof(envTrex)) {
    lstrcpynW(out, envTrex, cap);
  } else {
    wchar_t exePath[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, exePath, _countof(exePath)) == 0) {
      return false;
    }
    wchar_t* slash = wcsrchr(exePath, L'\\');
    if (!slash) {
      return false;
    }
    slash[1] = 0;
    lstrcpynW(out, exePath, cap);
  }

  size_t len = wcslen(out);
  if (len > 0 && out[len - 1] != L'\\' && len + 2 < cap) {
    out[len] = L'\\';
    out[len + 1] = 0;
  }

  return out[0] != 0;
}

static bool Dx11BridgeServerForceLoadTrexDx11RuntimeV191() {
  wchar_t mode[64] = {};
  if (GetEnvironmentVariableW(L"DX11_BRIDGE_MODE", mode, _countof(mode)) == 0) {
    return true;
  }

  wchar_t trexDir[MAX_PATH] = {};
  if (!Dx11BridgeForceGetTrexDirV191(trexDir, _countof(trexDir))) {
    Logger::err("DX11_V191_FORCE_TREX_DX11_RUNTIME: FAILED: could not resolve .trex directory.");
    return false;
  }

  SetDllDirectoryW(trexDir);

  wchar_t dxgiPath[MAX_PATH] = {};
  wchar_t d3d11Path[MAX_PATH] = {};
  wcscpy_s(dxgiPath, trexDir);
  wcscat_s(dxgiPath, L"dxgi.dll");
  wcscpy_s(d3d11Path, trexDir);
  wcscat_s(d3d11Path, L"d3d11.dll");

  if (!Dx11BridgeForceTrexRuntimePathExistsV191(dxgiPath)) {
    Logger::err("DX11_V191_FORCE_TREX_DX11_RUNTIME: FAILED: .trex\\dxgi.dll is missing.");
    return false;
  }

  if (!Dx11BridgeForceTrexRuntimePathExistsV191(d3d11Path)) {
    Logger::err("DX11_V191_FORCE_TREX_DX11_RUNTIME: FAILED: .trex\\d3d11.dll is missing.");
    return false;
  }

  gDx11BridgeForcedTrexDxgiV191 = LoadLibraryExW(dxgiPath, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
  if (!gDx11BridgeForcedTrexDxgiV191) {
    Logger::err("DX11_V191_FORCE_TREX_DX11_RUNTIME: FAILED: LoadLibraryExW(.trex\\dxgi.dll) failed.");
    return false;
  }

  Logger::info("DX11_V191_FORCE_TREX_DX11_RUNTIME: forced .trex\\dxgi.dll loaded into NvRemixBridge.exe.");

  gDx11BridgeForcedTrexD3D11V191 = LoadLibraryExW(d3d11Path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
  if (!gDx11BridgeForcedTrexD3D11V191) {
    Logger::err("DX11_V191_FORCE_TREX_DX11_RUNTIME: FAILED: LoadLibraryExW(.trex\\d3d11.dll) failed.");
    return false;
  }

  Logger::info("DX11_V191_FORCE_TREX_DX11_RUNTIME: forced .trex\\d3d11.dll loaded into NvRemixBridge.exe.");

  FARPROC createFactory = GetProcAddress(gDx11BridgeForcedTrexDxgiV191, "CreateDXGIFactory");
  FARPROC createFactory1 = GetProcAddress(gDx11BridgeForcedTrexDxgiV191, "CreateDXGIFactory1");
  FARPROC createFactory2 = GetProcAddress(gDx11BridgeForcedTrexDxgiV191, "CreateDXGIFactory2");
  FARPROC createDevice = GetProcAddress(gDx11BridgeForcedTrexD3D11V191, "D3D11CreateDevice");
  FARPROC createDeviceAndSwapChain = GetProcAddress(gDx11BridgeForcedTrexD3D11V191, "D3D11CreateDeviceAndSwapChain");

  if (!createDevice) {
    Logger::err("DX11_V191_FORCE_TREX_DX11_RUNTIME: FAILED: forced .trex\\d3d11.dll lacks D3D11CreateDevice.");
    return false;
  }

  Logger::info("DX11_V191_FORCE_TREX_DX11_RUNTIME: .trex\\d3d11.dll exports D3D11CreateDevice.");

  if (createDeviceAndSwapChain) {
    Logger::info("DX11_V191_FORCE_TREX_DX11_RUNTIME: .trex\\d3d11.dll exports D3D11CreateDeviceAndSwapChain.");
  } else {
    Logger::warn("DX11_V191_FORCE_TREX_DX11_RUNTIME: .trex\\d3d11.dll lacks D3D11CreateDeviceAndSwapChain.");
  }

  if (createFactory || createFactory1 || createFactory2) {
    Logger::info("DX11_V191_FORCE_TREX_DX11_RUNTIME: .trex\\dxgi.dll exports DXGI factory entrypoints.");
  } else {
    Logger::warn("DX11_V191_FORCE_TREX_DX11_RUNTIME: .trex\\dxgi.dll lacks DXGI factory exports.");
  }

  SetEnvironmentVariableW(L"DX11_BRIDGE_TREX_D3D11_FORCED", L"1");
  SetEnvironmentVariableW(L"DX11_BRIDGE_TREX_DXGI_FORCED", L"1");
  return true;
}
#endif

#ifndef DX11_V193_SERVER_REMIX_INITIALIZER
#define DX11_V193_SERVER_REMIX_INITIALIZER
static HMODULE gDx11BridgeTrexDxgiV193 = nullptr;
static HMODULE gDx11BridgeTrexD3D11V193 = nullptr;

static void Dx11BridgeServerInitializeTrexRemixRuntimeV193() {
  wchar_t mode[64] = {};
  GetEnvironmentVariableW(L"DX11_BRIDGE_MODE", mode, _countof(mode));

  wchar_t exePath[MAX_PATH] = {};
  if (GetModuleFileNameW(nullptr, exePath, _countof(exePath)) == 0) {
    Logger::warn("DX11_V193_SERVER_REMIX_INITIALIZER: could not resolve NvRemixBridge.exe path.");
    return;
  }

  wchar_t* slash = wcsrchr(exePath, L'\\');
  if (!slash) {
    Logger::warn("DX11_V193_SERVER_REMIX_INITIALIZER: could not resolve .trex folder.");
    return;
  }
  slash[1] = 0;

  SetDllDirectoryW(exePath);
  SetEnvironmentVariableW(L"DXVK_REMIX_TREX_DIR", exePath);
  SetEnvironmentVariableW(L"DXVK_REMIX_BRIDGE_SERVER_HOSTS_RUNTIME", L"1");

  wchar_t dxgiPath[MAX_PATH] = {};
  wchar_t d3d11Path[MAX_PATH] = {};
  wcscpy_s(dxgiPath, exePath);
  wcscat_s(dxgiPath, L"dxgi.dll");
  wcscpy_s(d3d11Path, exePath);
  wcscat_s(d3d11Path, L"d3d11.dll");

  gDx11BridgeTrexDxgiV193 = LoadLibraryW(dxgiPath);
  if (gDx11BridgeTrexDxgiV193) {
    Logger::info("DX11_V193_SERVER_REMIX_INITIALIZER: loaded .trex dxgi.dll into NvRemixBridge.exe.");
    FARPROC f0 = GetProcAddress(gDx11BridgeTrexDxgiV193, "CreateDXGIFactory");
    FARPROC f1 = GetProcAddress(gDx11BridgeTrexDxgiV193, "CreateDXGIFactory1");
    FARPROC f2 = GetProcAddress(gDx11BridgeTrexDxgiV193, "CreateDXGIFactory2");
    if (f0 || f1 || f2) {
      Logger::info("DX11_V193_SERVER_REMIX_INITIALIZER: .trex dxgi.dll exports DXGI factory entrypoints.");
    } else {
      Logger::warn("DX11_V193_SERVER_REMIX_INITIALIZER: .trex dxgi.dll loaded but no DXGI factory exports were found.");
    }
  } else {
    Logger::warn("DX11_V193_SERVER_REMIX_INITIALIZER: failed to load .trex dxgi.dll into NvRemixBridge.exe.");
  }

  gDx11BridgeTrexD3D11V193 = LoadLibraryW(d3d11Path);
  if (gDx11BridgeTrexD3D11V193) {
    Logger::info("DX11_V193_SERVER_REMIX_INITIALIZER: loaded .trex d3d11.dll into NvRemixBridge.exe.");
    FARPROC d0 = GetProcAddress(gDx11BridgeTrexD3D11V193, "D3D11CreateDevice");
    FARPROC d1 = GetProcAddress(gDx11BridgeTrexD3D11V193, "D3D11CreateDeviceAndSwapChain");
    if (d0 || d1) {
      Logger::info("DX11_V193_SERVER_REMIX_INITIALIZER: .trex d3d11.dll exports D3D11 device entrypoints.");
    } else {
      Logger::warn("DX11_V193_SERVER_REMIX_INITIALIZER: .trex d3d11.dll loaded but no D3D11 entrypoints were found.");
    }
  } else {
    Logger::warn("DX11_V193_SERVER_REMIX_INITIALIZER: failed to load .trex d3d11.dll into NvRemixBridge.exe.");
  }
}
#endif

#ifndef DX11_V193_FORCE_TREX_DX11_RUNTIME
#define DX11_V193_FORCE_TREX_DX11_RUNTIME
static HMODULE gDx11BridgeForcedTrexDxgiV193 = nullptr;
static HMODULE gDx11BridgeForcedTrexD3D11V193 = nullptr;

static bool Dx11BridgeForceTrexRuntimePathExistsV193(const wchar_t* path) {
  const DWORD attrs = GetFileAttributesW(path);
  return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
}

static bool Dx11BridgeForceGetTrexDirV193(wchar_t* out, DWORD cap) {
  if (!out || cap == 0) return false;
  out[0] = 0;

  wchar_t envTrex[MAX_PATH] = {};
  DWORD got = GetEnvironmentVariableW(L"DXVK_REMIX_TREX_DIR", envTrex, _countof(envTrex));
  if (got > 0 && got < _countof(envTrex)) {
    lstrcpynW(out, envTrex, cap);
  } else {
    wchar_t exePath[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, exePath, _countof(exePath)) == 0) {
      return false;
    }
    wchar_t* slash = wcsrchr(exePath, L'\\');
    if (!slash) {
      return false;
    }
    slash[1] = 0;
    lstrcpynW(out, exePath, cap);
  }

  size_t len = wcslen(out);
  if (len > 0 && out[len - 1] != L'\\' && len + 2 < cap) {
    out[len] = L'\\';
    out[len + 1] = 0;
  }

  return out[0] != 0;
}

static bool Dx11BridgeServerForceLoadTrexDx11RuntimeV193() {
  wchar_t mode[64] = {};
  if (GetEnvironmentVariableW(L"DX11_BRIDGE_MODE", mode, _countof(mode)) == 0) {
    return true;
  }

  wchar_t trexDir[MAX_PATH] = {};
  if (!Dx11BridgeForceGetTrexDirV193(trexDir, _countof(trexDir))) {
    Logger::err("DX11_V193_FORCE_TREX_DX11_RUNTIME: FAILED: could not resolve .trex directory.");
    return false;
  }

  SetDllDirectoryW(trexDir);

  wchar_t dxgiPath[MAX_PATH] = {};
  wchar_t d3d11Path[MAX_PATH] = {};
  wcscpy_s(dxgiPath, trexDir);
  wcscat_s(dxgiPath, L"dxgi.dll");
  wcscpy_s(d3d11Path, trexDir);
  wcscat_s(d3d11Path, L"d3d11.dll");

  if (!Dx11BridgeForceTrexRuntimePathExistsV193(dxgiPath)) {
    Logger::err("DX11_V193_FORCE_TREX_DX11_RUNTIME: FAILED: .trex\\dxgi.dll is missing.");
    return false;
  }

  if (!Dx11BridgeForceTrexRuntimePathExistsV193(d3d11Path)) {
    Logger::err("DX11_V193_FORCE_TREX_DX11_RUNTIME: FAILED: .trex\\d3d11.dll is missing.");
    return false;
  }

  gDx11BridgeForcedTrexDxgiV193 = LoadLibraryExW(dxgiPath, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
  if (!gDx11BridgeForcedTrexDxgiV193) {
    Logger::err("DX11_V193_FORCE_TREX_DX11_RUNTIME: FAILED: LoadLibraryExW(.trex\\dxgi.dll) failed.");
    return false;
  }

  Logger::info("DX11_V193_FORCE_TREX_DX11_RUNTIME: forced .trex\\dxgi.dll loaded into NvRemixBridge.exe.");

  gDx11BridgeForcedTrexD3D11V193 = LoadLibraryExW(d3d11Path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
  if (!gDx11BridgeForcedTrexD3D11V193) {
    Logger::err("DX11_V193_FORCE_TREX_DX11_RUNTIME: FAILED: LoadLibraryExW(.trex\\d3d11.dll) failed.");
    return false;
  }

  Logger::info("DX11_V193_FORCE_TREX_DX11_RUNTIME: forced .trex\\d3d11.dll loaded into NvRemixBridge.exe.");

  FARPROC createFactory = GetProcAddress(gDx11BridgeForcedTrexDxgiV193, "CreateDXGIFactory");
  FARPROC createFactory1 = GetProcAddress(gDx11BridgeForcedTrexDxgiV193, "CreateDXGIFactory1");
  FARPROC createFactory2 = GetProcAddress(gDx11BridgeForcedTrexDxgiV193, "CreateDXGIFactory2");
  FARPROC createDevice = GetProcAddress(gDx11BridgeForcedTrexD3D11V193, "D3D11CreateDevice");
  FARPROC createDeviceAndSwapChain = GetProcAddress(gDx11BridgeForcedTrexD3D11V193, "D3D11CreateDeviceAndSwapChain");

  if (!createDevice) {
    Logger::err("DX11_V193_FORCE_TREX_DX11_RUNTIME: FAILED: forced .trex\\d3d11.dll lacks D3D11CreateDevice.");
    return false;
  }

  Logger::info("DX11_V193_FORCE_TREX_DX11_RUNTIME: .trex\\d3d11.dll exports D3D11CreateDevice.");

  if (createDeviceAndSwapChain) {
    Logger::info("DX11_V193_FORCE_TREX_DX11_RUNTIME: .trex\\d3d11.dll exports D3D11CreateDeviceAndSwapChain.");
  } else {
    Logger::warn("DX11_V193_FORCE_TREX_DX11_RUNTIME: .trex\\d3d11.dll lacks D3D11CreateDeviceAndSwapChain.");
  }

  if (createFactory || createFactory1 || createFactory2) {
    Logger::info("DX11_V193_FORCE_TREX_DX11_RUNTIME: .trex\\dxgi.dll exports DXGI factory entrypoints.");
  } else {
    Logger::warn("DX11_V193_FORCE_TREX_DX11_RUNTIME: .trex\\dxgi.dll lacks DXGI factory exports.");
  }

  SetEnvironmentVariableW(L"DX11_BRIDGE_TREX_D3D11_FORCED", L"1");
  SetEnvironmentVariableW(L"DX11_BRIDGE_TREX_DXGI_FORCED", L"1");
  return true;
}
#endif

#ifndef DX11_V195_SERVER_REMIX_INITIALIZER
#define DX11_V195_SERVER_REMIX_INITIALIZER
static HMODULE gDx11BridgeTrexDxgiV195 = nullptr;
static HMODULE gDx11BridgeTrexD3D11V195 = nullptr;

static void Dx11BridgeServerInitializeTrexRemixRuntimeV195() {
  wchar_t mode[64] = {};
  GetEnvironmentVariableW(L"DX11_BRIDGE_MODE", mode, _countof(mode));

  wchar_t exePath[MAX_PATH] = {};
  if (GetModuleFileNameW(nullptr, exePath, _countof(exePath)) == 0) {
    Logger::warn("DX11_V195_SERVER_REMIX_INITIALIZER: could not resolve NvRemixBridge.exe path.");
    return;
  }

  wchar_t* slash = wcsrchr(exePath, L'\\');
  if (!slash) {
    Logger::warn("DX11_V195_SERVER_REMIX_INITIALIZER: could not resolve .trex folder.");
    return;
  }
  slash[1] = 0;

  SetDllDirectoryW(exePath);
  SetEnvironmentVariableW(L"DXVK_REMIX_TREX_DIR", exePath);
  SetEnvironmentVariableW(L"DXVK_REMIX_BRIDGE_SERVER_HOSTS_RUNTIME", L"1");

  wchar_t dxgiPath[MAX_PATH] = {};
  wchar_t d3d11Path[MAX_PATH] = {};
  wcscpy_s(dxgiPath, exePath);
  wcscat_s(dxgiPath, L"dxgi.dll");
  wcscpy_s(d3d11Path, exePath);
  wcscat_s(d3d11Path, L"d3d11.dll");

  gDx11BridgeTrexDxgiV195 = LoadLibraryW(dxgiPath);
  if (gDx11BridgeTrexDxgiV195) {
    Logger::info("DX11_V195_SERVER_REMIX_INITIALIZER: loaded .trex dxgi.dll into NvRemixBridge.exe.");
    FARPROC f0 = GetProcAddress(gDx11BridgeTrexDxgiV195, "CreateDXGIFactory");
    FARPROC f1 = GetProcAddress(gDx11BridgeTrexDxgiV195, "CreateDXGIFactory1");
    FARPROC f2 = GetProcAddress(gDx11BridgeTrexDxgiV195, "CreateDXGIFactory2");
    if (f0 || f1 || f2) {
      Logger::info("DX11_V195_SERVER_REMIX_INITIALIZER: .trex dxgi.dll exports DXGI factory entrypoints.");
    } else {
      Logger::warn("DX11_V195_SERVER_REMIX_INITIALIZER: .trex dxgi.dll loaded but no DXGI factory exports were found.");
    }
  } else {
    Logger::warn("DX11_V195_SERVER_REMIX_INITIALIZER: failed to load .trex dxgi.dll into NvRemixBridge.exe.");
  }

  gDx11BridgeTrexD3D11V195 = LoadLibraryW(d3d11Path);
  if (gDx11BridgeTrexD3D11V195) {
    Logger::info("DX11_V195_SERVER_REMIX_INITIALIZER: loaded .trex d3d11.dll into NvRemixBridge.exe.");
    FARPROC d0 = GetProcAddress(gDx11BridgeTrexD3D11V195, "D3D11CreateDevice");
    FARPROC d1 = GetProcAddress(gDx11BridgeTrexD3D11V195, "D3D11CreateDeviceAndSwapChain");
    if (d0 || d1) {
      Logger::info("DX11_V195_SERVER_REMIX_INITIALIZER: .trex d3d11.dll exports D3D11 device entrypoints.");
    } else {
      Logger::warn("DX11_V195_SERVER_REMIX_INITIALIZER: .trex d3d11.dll loaded but no D3D11 entrypoints were found.");
    }
  } else {
    Logger::warn("DX11_V195_SERVER_REMIX_INITIALIZER: failed to load .trex d3d11.dll into NvRemixBridge.exe.");
  }
}
#endif

#ifndef DX11_V195_FORCE_TREX_DX11_RUNTIME
#define DX11_V195_FORCE_TREX_DX11_RUNTIME
static HMODULE gDx11BridgeForcedTrexDxgiV195 = nullptr;
static HMODULE gDx11BridgeForcedTrexD3D11V195 = nullptr;

static bool Dx11BridgeForceTrexRuntimePathExistsV195(const wchar_t* path) {
  const DWORD attrs = GetFileAttributesW(path);
  return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
}

static bool Dx11BridgeForceGetTrexDirV195(wchar_t* out, DWORD cap) {
  if (!out || cap == 0) return false;
  out[0] = 0;

  wchar_t envTrex[MAX_PATH] = {};
  DWORD got = GetEnvironmentVariableW(L"DXVK_REMIX_TREX_DIR", envTrex, _countof(envTrex));
  if (got > 0 && got < _countof(envTrex)) {
    lstrcpynW(out, envTrex, cap);
  } else {
    wchar_t exePath[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, exePath, _countof(exePath)) == 0) {
      return false;
    }
    wchar_t* slash = wcsrchr(exePath, L'\\');
    if (!slash) {
      return false;
    }
    slash[1] = 0;
    lstrcpynW(out, exePath, cap);
  }

  size_t len = wcslen(out);
  if (len > 0 && out[len - 1] != L'\\' && len + 2 < cap) {
    out[len] = L'\\';
    out[len + 1] = 0;
  }

  return out[0] != 0;
}

static bool Dx11BridgeServerForceLoadTrexDx11RuntimeV195() {
  wchar_t mode[64] = {};
  if (GetEnvironmentVariableW(L"DX11_BRIDGE_MODE", mode, _countof(mode)) == 0) {
    return true;
  }

  wchar_t trexDir[MAX_PATH] = {};
  if (!Dx11BridgeForceGetTrexDirV195(trexDir, _countof(trexDir))) {
    Logger::err("DX11_V195_FORCE_TREX_DX11_RUNTIME: FAILED: could not resolve .trex directory.");
    return false;
  }

  SetDllDirectoryW(trexDir);

  wchar_t dxgiPath[MAX_PATH] = {};
  wchar_t d3d11Path[MAX_PATH] = {};
  wcscpy_s(dxgiPath, trexDir);
  wcscat_s(dxgiPath, L"dxgi.dll");
  wcscpy_s(d3d11Path, trexDir);
  wcscat_s(d3d11Path, L"d3d11.dll");

  if (!Dx11BridgeForceTrexRuntimePathExistsV195(dxgiPath)) {
    Logger::err("DX11_V195_FORCE_TREX_DX11_RUNTIME: FAILED: .trex\\dxgi.dll is missing.");
    return false;
  }

  if (!Dx11BridgeForceTrexRuntimePathExistsV195(d3d11Path)) {
    Logger::err("DX11_V195_FORCE_TREX_DX11_RUNTIME: FAILED: .trex\\d3d11.dll is missing.");
    return false;
  }

  gDx11BridgeForcedTrexDxgiV195 = LoadLibraryExW(dxgiPath, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
  if (!gDx11BridgeForcedTrexDxgiV195) {
    Logger::err("DX11_V195_FORCE_TREX_DX11_RUNTIME: FAILED: LoadLibraryExW(.trex\\dxgi.dll) failed.");
    return false;
  }

  Logger::info("DX11_V195_FORCE_TREX_DX11_RUNTIME: forced .trex\\dxgi.dll loaded into NvRemixBridge.exe.");

  gDx11BridgeForcedTrexD3D11V195 = LoadLibraryExW(d3d11Path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
  if (!gDx11BridgeForcedTrexD3D11V195) {
    Logger::err("DX11_V195_FORCE_TREX_DX11_RUNTIME: FAILED: LoadLibraryExW(.trex\\d3d11.dll) failed.");
    return false;
  }

  Logger::info("DX11_V195_FORCE_TREX_DX11_RUNTIME: forced .trex\\d3d11.dll loaded into NvRemixBridge.exe.");

  FARPROC createFactory = GetProcAddress(gDx11BridgeForcedTrexDxgiV195, "CreateDXGIFactory");
  FARPROC createFactory1 = GetProcAddress(gDx11BridgeForcedTrexDxgiV195, "CreateDXGIFactory1");
  FARPROC createFactory2 = GetProcAddress(gDx11BridgeForcedTrexDxgiV195, "CreateDXGIFactory2");
  FARPROC createDevice = GetProcAddress(gDx11BridgeForcedTrexD3D11V195, "D3D11CreateDevice");
  FARPROC createDeviceAndSwapChain = GetProcAddress(gDx11BridgeForcedTrexD3D11V195, "D3D11CreateDeviceAndSwapChain");

  if (!createDevice) {
    Logger::err("DX11_V195_FORCE_TREX_DX11_RUNTIME: FAILED: forced .trex\\d3d11.dll lacks D3D11CreateDevice.");
    return false;
  }

  Logger::info("DX11_V195_FORCE_TREX_DX11_RUNTIME: .trex\\d3d11.dll exports D3D11CreateDevice.");

  if (createDeviceAndSwapChain) {
    Logger::info("DX11_V195_FORCE_TREX_DX11_RUNTIME: .trex\\d3d11.dll exports D3D11CreateDeviceAndSwapChain.");
  } else {
    Logger::warn("DX11_V195_FORCE_TREX_DX11_RUNTIME: .trex\\d3d11.dll lacks D3D11CreateDeviceAndSwapChain.");
  }

  if (createFactory || createFactory1 || createFactory2) {
    Logger::info("DX11_V195_FORCE_TREX_DX11_RUNTIME: .trex\\dxgi.dll exports DXGI factory entrypoints.");
  } else {
    Logger::warn("DX11_V195_FORCE_TREX_DX11_RUNTIME: .trex\\dxgi.dll lacks DXGI factory exports.");
  }

  SetEnvironmentVariableW(L"DX11_BRIDGE_TREX_D3D11_FORCED", L"1");
  SetEnvironmentVariableW(L"DX11_BRIDGE_TREX_DXGI_FORCED", L"1");
  return true;
}
#endif

#ifndef DX11_V196_SERVER_REMIX_INITIALIZER
#define DX11_V196_SERVER_REMIX_INITIALIZER
static HMODULE gDx11BridgeTrexDxgiV196 = nullptr;
static HMODULE gDx11BridgeTrexD3D11V196 = nullptr;

static void Dx11BridgeServerInitializeTrexRemixRuntimeV196() {
  wchar_t mode[64] = {};
  GetEnvironmentVariableW(L"DX11_BRIDGE_MODE", mode, _countof(mode));

  wchar_t exePath[MAX_PATH] = {};
  if (GetModuleFileNameW(nullptr, exePath, _countof(exePath)) == 0) {
    Logger::warn("DX11_V196_SERVER_REMIX_INITIALIZER: could not resolve NvRemixBridge.exe path.");
    return;
  }

  wchar_t* slash = wcsrchr(exePath, L'\\');
  if (!slash) {
    Logger::warn("DX11_V196_SERVER_REMIX_INITIALIZER: could not resolve .trex folder.");
    return;
  }
  slash[1] = 0;

  SetDllDirectoryW(exePath);
  SetEnvironmentVariableW(L"DXVK_REMIX_TREX_DIR", exePath);
  SetEnvironmentVariableW(L"DXVK_REMIX_BRIDGE_SERVER_HOSTS_RUNTIME", L"1");

  wchar_t dxgiPath[MAX_PATH] = {};
  wchar_t d3d11Path[MAX_PATH] = {};
  wcscpy_s(dxgiPath, exePath);
  wcscat_s(dxgiPath, L"dxgi.dll");
  wcscpy_s(d3d11Path, exePath);
  wcscat_s(d3d11Path, L"d3d11.dll");

  gDx11BridgeTrexDxgiV196 = LoadLibraryW(dxgiPath);
  if (gDx11BridgeTrexDxgiV196) {
    Logger::info("DX11_V196_SERVER_REMIX_INITIALIZER: loaded .trex dxgi.dll into NvRemixBridge.exe.");
    FARPROC f0 = GetProcAddress(gDx11BridgeTrexDxgiV196, "CreateDXGIFactory");
    FARPROC f1 = GetProcAddress(gDx11BridgeTrexDxgiV196, "CreateDXGIFactory1");
    FARPROC f2 = GetProcAddress(gDx11BridgeTrexDxgiV196, "CreateDXGIFactory2");
    if (f0 || f1 || f2) {
      Logger::info("DX11_V196_SERVER_REMIX_INITIALIZER: .trex dxgi.dll exports DXGI factory entrypoints.");
    } else {
      Logger::warn("DX11_V196_SERVER_REMIX_INITIALIZER: .trex dxgi.dll loaded but no DXGI factory exports were found.");
    }
  } else {
    Logger::warn("DX11_V196_SERVER_REMIX_INITIALIZER: failed to load .trex dxgi.dll into NvRemixBridge.exe.");
  }

  gDx11BridgeTrexD3D11V196 = LoadLibraryW(d3d11Path);
  if (gDx11BridgeTrexD3D11V196) {
    Logger::info("DX11_V196_SERVER_REMIX_INITIALIZER: loaded .trex d3d11.dll into NvRemixBridge.exe.");
    FARPROC d0 = GetProcAddress(gDx11BridgeTrexD3D11V196, "D3D11CreateDevice");
    FARPROC d1 = GetProcAddress(gDx11BridgeTrexD3D11V196, "D3D11CreateDeviceAndSwapChain");
    if (d0 || d1) {
      Logger::info("DX11_V196_SERVER_REMIX_INITIALIZER: .trex d3d11.dll exports D3D11 device entrypoints.");
    } else {
      Logger::warn("DX11_V196_SERVER_REMIX_INITIALIZER: .trex d3d11.dll loaded but no D3D11 entrypoints were found.");
    }
  } else {
    Logger::warn("DX11_V196_SERVER_REMIX_INITIALIZER: failed to load .trex d3d11.dll into NvRemixBridge.exe.");
  }
}
#endif

#ifndef DX11_V196_FORCE_TREX_DX11_RUNTIME
#define DX11_V196_FORCE_TREX_DX11_RUNTIME
static HMODULE gDx11BridgeForcedTrexDxgiV196 = nullptr;
static HMODULE gDx11BridgeForcedTrexD3D11V196 = nullptr;

static bool Dx11BridgeForceTrexRuntimePathExistsV196(const wchar_t* path) {
  const DWORD attrs = GetFileAttributesW(path);
  return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
}

static bool Dx11BridgeForceGetTrexDirV196(wchar_t* out, DWORD cap) {
  if (!out || cap == 0) return false;
  out[0] = 0;

  wchar_t envTrex[MAX_PATH] = {};
  DWORD got = GetEnvironmentVariableW(L"DXVK_REMIX_TREX_DIR", envTrex, _countof(envTrex));
  if (got > 0 && got < _countof(envTrex)) {
    lstrcpynW(out, envTrex, cap);
  } else {
    wchar_t exePath[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, exePath, _countof(exePath)) == 0) {
      return false;
    }
    wchar_t* slash = wcsrchr(exePath, L'\\');
    if (!slash) {
      return false;
    }
    slash[1] = 0;
    lstrcpynW(out, exePath, cap);
  }

  size_t len = wcslen(out);
  if (len > 0 && out[len - 1] != L'\\' && len + 2 < cap) {
    out[len] = L'\\';
    out[len + 1] = 0;
  }

  return out[0] != 0;
}

static bool Dx11BridgeServerForceLoadTrexDx11RuntimeV196() {
  wchar_t mode[64] = {};
  if (GetEnvironmentVariableW(L"DX11_BRIDGE_MODE", mode, _countof(mode)) == 0) {
    return true;
  }

  wchar_t trexDir[MAX_PATH] = {};
  if (!Dx11BridgeForceGetTrexDirV196(trexDir, _countof(trexDir))) {
    Logger::err("DX11_V196_FORCE_TREX_DX11_RUNTIME: FAILED: could not resolve .trex directory.");
    return false;
  }

  SetDllDirectoryW(trexDir);

  wchar_t dxgiPath[MAX_PATH] = {};
  wchar_t d3d11Path[MAX_PATH] = {};
  wcscpy_s(dxgiPath, trexDir);
  wcscat_s(dxgiPath, L"dxgi.dll");
  wcscpy_s(d3d11Path, trexDir);
  wcscat_s(d3d11Path, L"d3d11.dll");

  if (!Dx11BridgeForceTrexRuntimePathExistsV196(dxgiPath)) {
    Logger::err("DX11_V196_FORCE_TREX_DX11_RUNTIME: FAILED: .trex\\dxgi.dll is missing.");
    return false;
  }

  if (!Dx11BridgeForceTrexRuntimePathExistsV196(d3d11Path)) {
    Logger::err("DX11_V196_FORCE_TREX_DX11_RUNTIME: FAILED: .trex\\d3d11.dll is missing.");
    return false;
  }

  gDx11BridgeForcedTrexDxgiV196 = LoadLibraryExW(dxgiPath, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
  if (!gDx11BridgeForcedTrexDxgiV196) {
    Logger::err("DX11_V196_FORCE_TREX_DX11_RUNTIME: FAILED: LoadLibraryExW(.trex\\dxgi.dll) failed.");
    return false;
  }

  Logger::info("DX11_V196_FORCE_TREX_DX11_RUNTIME: forced .trex\\dxgi.dll loaded into NvRemixBridge.exe.");

  gDx11BridgeForcedTrexD3D11V196 = LoadLibraryExW(d3d11Path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
  if (!gDx11BridgeForcedTrexD3D11V196) {
    Logger::err("DX11_V196_FORCE_TREX_DX11_RUNTIME: FAILED: LoadLibraryExW(.trex\\d3d11.dll) failed.");
    return false;
  }

  Logger::info("DX11_V196_FORCE_TREX_DX11_RUNTIME: forced .trex\\d3d11.dll loaded into NvRemixBridge.exe.");

  FARPROC createFactory = GetProcAddress(gDx11BridgeForcedTrexDxgiV196, "CreateDXGIFactory");
  FARPROC createFactory1 = GetProcAddress(gDx11BridgeForcedTrexDxgiV196, "CreateDXGIFactory1");
  FARPROC createFactory2 = GetProcAddress(gDx11BridgeForcedTrexDxgiV196, "CreateDXGIFactory2");
  FARPROC createDevice = GetProcAddress(gDx11BridgeForcedTrexD3D11V196, "D3D11CreateDevice");
  FARPROC createDeviceAndSwapChain = GetProcAddress(gDx11BridgeForcedTrexD3D11V196, "D3D11CreateDeviceAndSwapChain");

  if (!createDevice) {
    Logger::err("DX11_V196_FORCE_TREX_DX11_RUNTIME: FAILED: forced .trex\\d3d11.dll lacks D3D11CreateDevice.");
    return false;
  }

  Logger::info("DX11_V196_FORCE_TREX_DX11_RUNTIME: .trex\\d3d11.dll exports D3D11CreateDevice.");

  if (createDeviceAndSwapChain) {
    Logger::info("DX11_V196_FORCE_TREX_DX11_RUNTIME: .trex\\d3d11.dll exports D3D11CreateDeviceAndSwapChain.");
  } else {
    Logger::warn("DX11_V196_FORCE_TREX_DX11_RUNTIME: .trex\\d3d11.dll lacks D3D11CreateDeviceAndSwapChain.");
  }

  if (createFactory || createFactory1 || createFactory2) {
    Logger::info("DX11_V196_FORCE_TREX_DX11_RUNTIME: .trex\\dxgi.dll exports DXGI factory entrypoints.");
  } else {
    Logger::warn("DX11_V196_FORCE_TREX_DX11_RUNTIME: .trex\\dxgi.dll lacks DXGI factory exports.");
  }

  SetEnvironmentVariableW(L"DX11_BRIDGE_TREX_D3D11_FORCED", L"1");
  SetEnvironmentVariableW(L"DX11_BRIDGE_TREX_DXGI_FORCED", L"1");
  return true;
}
#endif

#ifndef DX11_V197_SERVER_REMIX_INITIALIZER
#define DX11_V197_SERVER_REMIX_INITIALIZER
static HMODULE gDx11BridgeTrexDxgiV197 = nullptr;
static HMODULE gDx11BridgeTrexD3D11V197 = nullptr;

static void Dx11BridgeServerInitializeTrexRemixRuntimeV197() {
  wchar_t mode[64] = {};
  GetEnvironmentVariableW(L"DX11_BRIDGE_MODE", mode, _countof(mode));

  wchar_t exePath[MAX_PATH] = {};
  if (GetModuleFileNameW(nullptr, exePath, _countof(exePath)) == 0) {
    Logger::warn("DX11_V197_SERVER_REMIX_INITIALIZER: could not resolve NvRemixBridge.exe path.");
    return;
  }

  wchar_t* slash = wcsrchr(exePath, L'\\');
  if (!slash) {
    Logger::warn("DX11_V197_SERVER_REMIX_INITIALIZER: could not resolve .trex folder.");
    return;
  }
  slash[1] = 0;

  SetDllDirectoryW(exePath);
  SetEnvironmentVariableW(L"DXVK_REMIX_TREX_DIR", exePath);
  SetEnvironmentVariableW(L"DXVK_REMIX_BRIDGE_SERVER_HOSTS_RUNTIME", L"1");

  wchar_t dxgiPath[MAX_PATH] = {};
  wchar_t d3d11Path[MAX_PATH] = {};
  wcscpy_s(dxgiPath, exePath);
  wcscat_s(dxgiPath, L"dxgi.dll");
  wcscpy_s(d3d11Path, exePath);
  wcscat_s(d3d11Path, L"d3d11.dll");

  gDx11BridgeTrexDxgiV197 = LoadLibraryW(dxgiPath);
  if (gDx11BridgeTrexDxgiV197) {
    Logger::info("DX11_V197_SERVER_REMIX_INITIALIZER: loaded .trex dxgi.dll into NvRemixBridge.exe.");
    FARPROC f0 = GetProcAddress(gDx11BridgeTrexDxgiV197, "CreateDXGIFactory");
    FARPROC f1 = GetProcAddress(gDx11BridgeTrexDxgiV197, "CreateDXGIFactory1");
    FARPROC f2 = GetProcAddress(gDx11BridgeTrexDxgiV197, "CreateDXGIFactory2");
    if (f0 || f1 || f2) {
      Logger::info("DX11_V197_SERVER_REMIX_INITIALIZER: .trex dxgi.dll exports DXGI factory entrypoints.");
    } else {
      Logger::warn("DX11_V197_SERVER_REMIX_INITIALIZER: .trex dxgi.dll loaded but no DXGI factory exports were found.");
    }
  } else {
    Logger::warn("DX11_V197_SERVER_REMIX_INITIALIZER: failed to load .trex dxgi.dll into NvRemixBridge.exe.");
  }

  gDx11BridgeTrexD3D11V197 = LoadLibraryW(d3d11Path);
  if (gDx11BridgeTrexD3D11V197) {
    Logger::info("DX11_V197_SERVER_REMIX_INITIALIZER: loaded .trex d3d11.dll into NvRemixBridge.exe.");
    FARPROC d0 = GetProcAddress(gDx11BridgeTrexD3D11V197, "D3D11CreateDevice");
    FARPROC d1 = GetProcAddress(gDx11BridgeTrexD3D11V197, "D3D11CreateDeviceAndSwapChain");
    if (d0 || d1) {
      Logger::info("DX11_V197_SERVER_REMIX_INITIALIZER: .trex d3d11.dll exports D3D11 device entrypoints.");
    } else {
      Logger::warn("DX11_V197_SERVER_REMIX_INITIALIZER: .trex d3d11.dll loaded but no D3D11 entrypoints were found.");
    }
  } else {
    Logger::warn("DX11_V197_SERVER_REMIX_INITIALIZER: failed to load .trex d3d11.dll into NvRemixBridge.exe.");
  }
}
#endif

#ifndef DX11_V197_FORCE_TREX_DX11_RUNTIME
#define DX11_V197_FORCE_TREX_DX11_RUNTIME
static HMODULE gDx11BridgeForcedTrexDxgiV197 = nullptr;
static HMODULE gDx11BridgeForcedTrexD3D11V197 = nullptr;

static bool Dx11BridgeForceTrexRuntimePathExistsV197(const wchar_t* path) {
  const DWORD attrs = GetFileAttributesW(path);
  return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
}

static bool Dx11BridgeForceGetTrexDirV197(wchar_t* out, DWORD cap) {
  if (!out || cap == 0) return false;
  out[0] = 0;

  wchar_t envTrex[MAX_PATH] = {};
  DWORD got = GetEnvironmentVariableW(L"DXVK_REMIX_TREX_DIR", envTrex, _countof(envTrex));
  if (got > 0 && got < _countof(envTrex)) {
    lstrcpynW(out, envTrex, cap);
  } else {
    wchar_t exePath[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, exePath, _countof(exePath)) == 0) {
      return false;
    }
    wchar_t* slash = wcsrchr(exePath, L'\\');
    if (!slash) {
      return false;
    }
    slash[1] = 0;
    lstrcpynW(out, exePath, cap);
  }

  size_t len = wcslen(out);
  if (len > 0 && out[len - 1] != L'\\' && len + 2 < cap) {
    out[len] = L'\\';
    out[len + 1] = 0;
  }

  return out[0] != 0;
}

static bool Dx11BridgeServerForceLoadTrexDx11RuntimeV197() {
  wchar_t mode[64] = {};
  if (GetEnvironmentVariableW(L"DX11_BRIDGE_MODE", mode, _countof(mode)) == 0) {
    return true;
  }

  wchar_t trexDir[MAX_PATH] = {};
  if (!Dx11BridgeForceGetTrexDirV197(trexDir, _countof(trexDir))) {
    Logger::err("DX11_V197_FORCE_TREX_DX11_RUNTIME: FAILED: could not resolve .trex directory.");
    return false;
  }

  SetDllDirectoryW(trexDir);

  wchar_t dxgiPath[MAX_PATH] = {};
  wchar_t d3d11Path[MAX_PATH] = {};
  wcscpy_s(dxgiPath, trexDir);
  wcscat_s(dxgiPath, L"dxgi.dll");
  wcscpy_s(d3d11Path, trexDir);
  wcscat_s(d3d11Path, L"d3d11.dll");

  if (!Dx11BridgeForceTrexRuntimePathExistsV197(dxgiPath)) {
    Logger::err("DX11_V197_FORCE_TREX_DX11_RUNTIME: FAILED: .trex\\dxgi.dll is missing.");
    return false;
  }

  if (!Dx11BridgeForceTrexRuntimePathExistsV197(d3d11Path)) {
    Logger::err("DX11_V197_FORCE_TREX_DX11_RUNTIME: FAILED: .trex\\d3d11.dll is missing.");
    return false;
  }

  gDx11BridgeForcedTrexDxgiV197 = LoadLibraryExW(dxgiPath, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
  if (!gDx11BridgeForcedTrexDxgiV197) {
    Logger::err("DX11_V197_FORCE_TREX_DX11_RUNTIME: FAILED: LoadLibraryExW(.trex\\dxgi.dll) failed.");
    return false;
  }

  Logger::info("DX11_V197_FORCE_TREX_DX11_RUNTIME: forced .trex\\dxgi.dll loaded into NvRemixBridge.exe.");

  gDx11BridgeForcedTrexD3D11V197 = LoadLibraryExW(d3d11Path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
  if (!gDx11BridgeForcedTrexD3D11V197) {
    Logger::err("DX11_V197_FORCE_TREX_DX11_RUNTIME: FAILED: LoadLibraryExW(.trex\\d3d11.dll) failed.");
    return false;
  }

  Logger::info("DX11_V197_FORCE_TREX_DX11_RUNTIME: forced .trex\\d3d11.dll loaded into NvRemixBridge.exe.");

  FARPROC createFactory = GetProcAddress(gDx11BridgeForcedTrexDxgiV197, "CreateDXGIFactory");
  FARPROC createFactory1 = GetProcAddress(gDx11BridgeForcedTrexDxgiV197, "CreateDXGIFactory1");
  FARPROC createFactory2 = GetProcAddress(gDx11BridgeForcedTrexDxgiV197, "CreateDXGIFactory2");
  FARPROC createDevice = GetProcAddress(gDx11BridgeForcedTrexD3D11V197, "D3D11CreateDevice");
  FARPROC createDeviceAndSwapChain = GetProcAddress(gDx11BridgeForcedTrexD3D11V197, "D3D11CreateDeviceAndSwapChain");

  if (!createDevice) {
    Logger::err("DX11_V197_FORCE_TREX_DX11_RUNTIME: FAILED: forced .trex\\d3d11.dll lacks D3D11CreateDevice.");
    return false;
  }

  Logger::info("DX11_V197_FORCE_TREX_DX11_RUNTIME: .trex\\d3d11.dll exports D3D11CreateDevice.");

  if (createDeviceAndSwapChain) {
    Logger::info("DX11_V197_FORCE_TREX_DX11_RUNTIME: .trex\\d3d11.dll exports D3D11CreateDeviceAndSwapChain.");
  } else {
    Logger::warn("DX11_V197_FORCE_TREX_DX11_RUNTIME: .trex\\d3d11.dll lacks D3D11CreateDeviceAndSwapChain.");
  }

  if (createFactory || createFactory1 || createFactory2) {
    Logger::info("DX11_V197_FORCE_TREX_DX11_RUNTIME: .trex\\dxgi.dll exports DXGI factory entrypoints.");
  } else {
    Logger::warn("DX11_V197_FORCE_TREX_DX11_RUNTIME: .trex\\dxgi.dll lacks DXGI factory exports.");
  }

  SetEnvironmentVariableW(L"DX11_BRIDGE_TREX_D3D11_FORCED", L"1");
  SetEnvironmentVariableW(L"DX11_BRIDGE_TREX_DXGI_FORCED", L"1");
  return true;
}
#endif

#ifndef DX11_V198_SERVER_REMIX_INITIALIZER
#define DX11_V198_SERVER_REMIX_INITIALIZER
static HMODULE gDx11BridgeTrexDxgiV198 = nullptr;
static HMODULE gDx11BridgeTrexD3D11V198 = nullptr;

static void Dx11BridgeServerInitializeTrexRemixRuntimeV198() {
  wchar_t mode[64] = {};
  GetEnvironmentVariableW(L"DX11_BRIDGE_MODE", mode, _countof(mode));

  wchar_t exePath[MAX_PATH] = {};
  if (GetModuleFileNameW(nullptr, exePath, _countof(exePath)) == 0) {
    Logger::warn("DX11_V198_SERVER_REMIX_INITIALIZER: could not resolve NvRemixBridge.exe path.");
    return;
  }

  wchar_t* slash = wcsrchr(exePath, L'\\');
  if (!slash) {
    Logger::warn("DX11_V198_SERVER_REMIX_INITIALIZER: could not resolve .trex folder.");
    return;
  }
  slash[1] = 0;

  SetDllDirectoryW(exePath);
  SetEnvironmentVariableW(L"DXVK_REMIX_TREX_DIR", exePath);
  SetEnvironmentVariableW(L"DXVK_REMIX_BRIDGE_SERVER_HOSTS_RUNTIME", L"1");

  wchar_t dxgiPath[MAX_PATH] = {};
  wchar_t d3d11Path[MAX_PATH] = {};
  wcscpy_s(dxgiPath, exePath);
  wcscat_s(dxgiPath, L"dxgi.dll");
  wcscpy_s(d3d11Path, exePath);
  wcscat_s(d3d11Path, L"d3d11.dll");

  gDx11BridgeTrexDxgiV198 = LoadLibraryW(dxgiPath);
  if (gDx11BridgeTrexDxgiV198) {
    Logger::info("DX11_V198_SERVER_REMIX_INITIALIZER: loaded .trex dxgi.dll into NvRemixBridge.exe.");
    FARPROC f0 = GetProcAddress(gDx11BridgeTrexDxgiV198, "CreateDXGIFactory");
    FARPROC f1 = GetProcAddress(gDx11BridgeTrexDxgiV198, "CreateDXGIFactory1");
    FARPROC f2 = GetProcAddress(gDx11BridgeTrexDxgiV198, "CreateDXGIFactory2");
    if (f0 || f1 || f2) {
      Logger::info("DX11_V198_SERVER_REMIX_INITIALIZER: .trex dxgi.dll exports DXGI factory entrypoints.");
    } else {
      Logger::warn("DX11_V198_SERVER_REMIX_INITIALIZER: .trex dxgi.dll loaded but no DXGI factory exports were found.");
    }
  } else {
    Logger::warn("DX11_V198_SERVER_REMIX_INITIALIZER: failed to load .trex dxgi.dll into NvRemixBridge.exe.");
  }

  gDx11BridgeTrexD3D11V198 = LoadLibraryW(d3d11Path);
  if (gDx11BridgeTrexD3D11V198) {
    Logger::info("DX11_V198_SERVER_REMIX_INITIALIZER: loaded .trex d3d11.dll into NvRemixBridge.exe.");
    FARPROC d0 = GetProcAddress(gDx11BridgeTrexD3D11V198, "D3D11CreateDevice");
    FARPROC d1 = GetProcAddress(gDx11BridgeTrexD3D11V198, "D3D11CreateDeviceAndSwapChain");
    if (d0 || d1) {
      Logger::info("DX11_V198_SERVER_REMIX_INITIALIZER: .trex d3d11.dll exports D3D11 device entrypoints.");
    } else {
      Logger::warn("DX11_V198_SERVER_REMIX_INITIALIZER: .trex d3d11.dll loaded but no D3D11 entrypoints were found.");
    }
  } else {
    Logger::warn("DX11_V198_SERVER_REMIX_INITIALIZER: failed to load .trex d3d11.dll into NvRemixBridge.exe.");
  }
}
#endif

#ifndef DX11_V198_FORCE_TREX_DX11_RUNTIME
#define DX11_V198_FORCE_TREX_DX11_RUNTIME
static HMODULE gDx11BridgeForcedTrexDxgiV198 = nullptr;
static HMODULE gDx11BridgeForcedTrexD3D11V198 = nullptr;

static bool Dx11BridgeForceTrexRuntimePathExistsV198(const wchar_t* path) {
  const DWORD attrs = GetFileAttributesW(path);
  return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
}

static bool Dx11BridgeForceGetTrexDirV198(wchar_t* out, DWORD cap) {
  if (!out || cap == 0) return false;
  out[0] = 0;

  wchar_t envTrex[MAX_PATH] = {};
  DWORD got = GetEnvironmentVariableW(L"DXVK_REMIX_TREX_DIR", envTrex, _countof(envTrex));
  if (got > 0 && got < _countof(envTrex)) {
    lstrcpynW(out, envTrex, cap);
  } else {
    wchar_t exePath[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, exePath, _countof(exePath)) == 0) {
      return false;
    }
    wchar_t* slash = wcsrchr(exePath, L'\\');
    if (!slash) {
      return false;
    }
    slash[1] = 0;
    lstrcpynW(out, exePath, cap);
  }

  size_t len = wcslen(out);
  if (len > 0 && out[len - 1] != L'\\' && len + 2 < cap) {
    out[len] = L'\\';
    out[len + 1] = 0;
  }

  return out[0] != 0;
}

static bool Dx11BridgeServerForceLoadTrexDx11RuntimeV198() {
  wchar_t mode[64] = {};
  if (GetEnvironmentVariableW(L"DX11_BRIDGE_MODE", mode, _countof(mode)) == 0) {
    return true;
  }

  wchar_t trexDir[MAX_PATH] = {};
  if (!Dx11BridgeForceGetTrexDirV198(trexDir, _countof(trexDir))) {
    Logger::err("DX11_V198_FORCE_TREX_DX11_RUNTIME: FAILED: could not resolve .trex directory.");
    return false;
  }

  SetDllDirectoryW(trexDir);

  wchar_t dxgiPath[MAX_PATH] = {};
  wchar_t d3d11Path[MAX_PATH] = {};
  wcscpy_s(dxgiPath, trexDir);
  wcscat_s(dxgiPath, L"dxgi.dll");
  wcscpy_s(d3d11Path, trexDir);
  wcscat_s(d3d11Path, L"d3d11.dll");

  if (!Dx11BridgeForceTrexRuntimePathExistsV198(dxgiPath)) {
    Logger::err("DX11_V198_FORCE_TREX_DX11_RUNTIME: FAILED: .trex\\dxgi.dll is missing.");
    return false;
  }

  if (!Dx11BridgeForceTrexRuntimePathExistsV198(d3d11Path)) {
    Logger::err("DX11_V198_FORCE_TREX_DX11_RUNTIME: FAILED: .trex\\d3d11.dll is missing.");
    return false;
  }

  gDx11BridgeForcedTrexDxgiV198 = LoadLibraryExW(dxgiPath, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
  if (!gDx11BridgeForcedTrexDxgiV198) {
    Logger::err("DX11_V198_FORCE_TREX_DX11_RUNTIME: FAILED: LoadLibraryExW(.trex\\dxgi.dll) failed.");
    return false;
  }

  Logger::info("DX11_V198_FORCE_TREX_DX11_RUNTIME: forced .trex\\dxgi.dll loaded into NvRemixBridge.exe.");

  gDx11BridgeForcedTrexD3D11V198 = LoadLibraryExW(d3d11Path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
  if (!gDx11BridgeForcedTrexD3D11V198) {
    Logger::err("DX11_V198_FORCE_TREX_DX11_RUNTIME: FAILED: LoadLibraryExW(.trex\\d3d11.dll) failed.");
    return false;
  }

  Logger::info("DX11_V198_FORCE_TREX_DX11_RUNTIME: forced .trex\\d3d11.dll loaded into NvRemixBridge.exe.");

  FARPROC createFactory = GetProcAddress(gDx11BridgeForcedTrexDxgiV198, "CreateDXGIFactory");
  FARPROC createFactory1 = GetProcAddress(gDx11BridgeForcedTrexDxgiV198, "CreateDXGIFactory1");
  FARPROC createFactory2 = GetProcAddress(gDx11BridgeForcedTrexDxgiV198, "CreateDXGIFactory2");
  FARPROC createDevice = GetProcAddress(gDx11BridgeForcedTrexD3D11V198, "D3D11CreateDevice");
  FARPROC createDeviceAndSwapChain = GetProcAddress(gDx11BridgeForcedTrexD3D11V198, "D3D11CreateDeviceAndSwapChain");

  if (!createDevice) {
    Logger::err("DX11_V198_FORCE_TREX_DX11_RUNTIME: FAILED: forced .trex\\d3d11.dll lacks D3D11CreateDevice.");
    return false;
  }

  Logger::info("DX11_V198_FORCE_TREX_DX11_RUNTIME: .trex\\d3d11.dll exports D3D11CreateDevice.");

  if (createDeviceAndSwapChain) {
    Logger::info("DX11_V198_FORCE_TREX_DX11_RUNTIME: .trex\\d3d11.dll exports D3D11CreateDeviceAndSwapChain.");
  } else {
    Logger::warn("DX11_V198_FORCE_TREX_DX11_RUNTIME: .trex\\d3d11.dll lacks D3D11CreateDeviceAndSwapChain.");
  }

  if (createFactory || createFactory1 || createFactory2) {
    Logger::info("DX11_V198_FORCE_TREX_DX11_RUNTIME: .trex\\dxgi.dll exports DXGI factory entrypoints.");
  } else {
    Logger::warn("DX11_V198_FORCE_TREX_DX11_RUNTIME: .trex\\dxgi.dll lacks DXGI factory exports.");
  }

  SetEnvironmentVariableW(L"DX11_BRIDGE_TREX_D3D11_FORCED", L"1");
  SetEnvironmentVariableW(L"DX11_BRIDGE_TREX_DXGI_FORCED", L"1");
  return true;
}
#endif

#ifndef DX11_V200_SERVER_REMIX_INITIALIZER
#define DX11_V200_SERVER_REMIX_INITIALIZER
static HMODULE gDx11BridgeTrexDxgiV200 = nullptr;
static HMODULE gDx11BridgeTrexD3D11V200 = nullptr;

static void Dx11BridgeServerInitializeTrexRemixRuntimeV200() {
  wchar_t mode[64] = {};
  GetEnvironmentVariableW(L"DX11_BRIDGE_MODE", mode, _countof(mode));

  wchar_t exePath[MAX_PATH] = {};
  if (GetModuleFileNameW(nullptr, exePath, _countof(exePath)) == 0) {
    Logger::warn("DX11_V200_SERVER_REMIX_INITIALIZER: could not resolve NvRemixBridge.exe path.");
    return;
  }

  wchar_t* slash = wcsrchr(exePath, L'\\');
  if (!slash) {
    Logger::warn("DX11_V200_SERVER_REMIX_INITIALIZER: could not resolve .trex folder.");
    return;
  }
  slash[1] = 0;

  SetDllDirectoryW(exePath);
  SetEnvironmentVariableW(L"DXVK_REMIX_TREX_DIR", exePath);
  SetEnvironmentVariableW(L"DXVK_REMIX_BRIDGE_SERVER_HOSTS_RUNTIME", L"1");

  wchar_t dxgiPath[MAX_PATH] = {};
  wchar_t d3d11Path[MAX_PATH] = {};
  wcscpy_s(dxgiPath, exePath);
  wcscat_s(dxgiPath, L"dxgi.dll");
  wcscpy_s(d3d11Path, exePath);
  wcscat_s(d3d11Path, L"d3d11.dll");

  gDx11BridgeTrexDxgiV200 = LoadLibraryW(dxgiPath);
  if (gDx11BridgeTrexDxgiV200) {
    Logger::info("DX11_V200_SERVER_REMIX_INITIALIZER: loaded .trex dxgi.dll into NvRemixBridge.exe.");
    FARPROC f0 = GetProcAddress(gDx11BridgeTrexDxgiV200, "CreateDXGIFactory");
    FARPROC f1 = GetProcAddress(gDx11BridgeTrexDxgiV200, "CreateDXGIFactory1");
    FARPROC f2 = GetProcAddress(gDx11BridgeTrexDxgiV200, "CreateDXGIFactory2");
    if (f0 || f1 || f2) {
      Logger::info("DX11_V200_SERVER_REMIX_INITIALIZER: .trex dxgi.dll exports DXGI factory entrypoints.");
    } else {
      Logger::warn("DX11_V200_SERVER_REMIX_INITIALIZER: .trex dxgi.dll loaded but no DXGI factory exports were found.");
    }
  } else {
    Logger::warn("DX11_V200_SERVER_REMIX_INITIALIZER: failed to load .trex dxgi.dll into NvRemixBridge.exe.");
  }

  gDx11BridgeTrexD3D11V200 = LoadLibraryW(d3d11Path);
  if (gDx11BridgeTrexD3D11V200) {
    Logger::info("DX11_V200_SERVER_REMIX_INITIALIZER: loaded .trex d3d11.dll into NvRemixBridge.exe.");
    FARPROC d0 = GetProcAddress(gDx11BridgeTrexD3D11V200, "D3D11CreateDevice");
    FARPROC d1 = GetProcAddress(gDx11BridgeTrexD3D11V200, "D3D11CreateDeviceAndSwapChain");
    if (d0 || d1) {
      Logger::info("DX11_V200_SERVER_REMIX_INITIALIZER: .trex d3d11.dll exports D3D11 device entrypoints.");
    } else {
      Logger::warn("DX11_V200_SERVER_REMIX_INITIALIZER: .trex d3d11.dll loaded but no D3D11 entrypoints were found.");
    }
  } else {
    Logger::warn("DX11_V200_SERVER_REMIX_INITIALIZER: failed to load .trex d3d11.dll into NvRemixBridge.exe.");
  }
}
#endif

#ifndef DX11_V200_FORCE_TREX_DX11_RUNTIME
#define DX11_V200_FORCE_TREX_DX11_RUNTIME
static HMODULE gDx11BridgeForcedTrexDxgiV200 = nullptr;
static HMODULE gDx11BridgeForcedTrexD3D11V200 = nullptr;

static bool Dx11BridgeForceTrexRuntimePathExistsV200(const wchar_t* path) {
  const DWORD attrs = GetFileAttributesW(path);
  return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
}

static bool Dx11BridgeForceGetTrexDirV200(wchar_t* out, DWORD cap) {
  if (!out || cap == 0) return false;
  out[0] = 0;

  wchar_t envTrex[MAX_PATH] = {};
  DWORD got = GetEnvironmentVariableW(L"DXVK_REMIX_TREX_DIR", envTrex, _countof(envTrex));
  if (got > 0 && got < _countof(envTrex)) {
    lstrcpynW(out, envTrex, cap);
  } else {
    wchar_t exePath[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, exePath, _countof(exePath)) == 0) {
      return false;
    }
    wchar_t* slash = wcsrchr(exePath, L'\\');
    if (!slash) {
      return false;
    }
    slash[1] = 0;
    lstrcpynW(out, exePath, cap);
  }

  size_t len = wcslen(out);
  if (len > 0 && out[len - 1] != L'\\' && len + 2 < cap) {
    out[len] = L'\\';
    out[len + 1] = 0;
  }

  return out[0] != 0;
}

static bool Dx11BridgeServerForceLoadTrexDx11RuntimeV200() {
  wchar_t mode[64] = {};
  if (GetEnvironmentVariableW(L"DX11_BRIDGE_MODE", mode, _countof(mode)) == 0) {
    return true;
  }

  wchar_t trexDir[MAX_PATH] = {};
  if (!Dx11BridgeForceGetTrexDirV200(trexDir, _countof(trexDir))) {
    Logger::err("DX11_V200_FORCE_TREX_DX11_RUNTIME: FAILED: could not resolve .trex directory.");
    return false;
  }

  SetDllDirectoryW(trexDir);

  wchar_t dxgiPath[MAX_PATH] = {};
  wchar_t d3d11Path[MAX_PATH] = {};
  wcscpy_s(dxgiPath, trexDir);
  wcscat_s(dxgiPath, L"dxgi.dll");
  wcscpy_s(d3d11Path, trexDir);
  wcscat_s(d3d11Path, L"d3d11.dll");

  if (!Dx11BridgeForceTrexRuntimePathExistsV200(dxgiPath)) {
    Logger::err("DX11_V200_FORCE_TREX_DX11_RUNTIME: FAILED: .trex\\dxgi.dll is missing.");
    return false;
  }

  if (!Dx11BridgeForceTrexRuntimePathExistsV200(d3d11Path)) {
    Logger::err("DX11_V200_FORCE_TREX_DX11_RUNTIME: FAILED: .trex\\d3d11.dll is missing.");
    return false;
  }

  gDx11BridgeForcedTrexDxgiV200 = LoadLibraryExW(dxgiPath, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
  if (!gDx11BridgeForcedTrexDxgiV200) {
    Logger::err("DX11_V200_FORCE_TREX_DX11_RUNTIME: FAILED: LoadLibraryExW(.trex\\dxgi.dll) failed.");
    return false;
  }

  Logger::info("DX11_V200_FORCE_TREX_DX11_RUNTIME: forced .trex\\dxgi.dll loaded into NvRemixBridge.exe.");

  gDx11BridgeForcedTrexD3D11V200 = LoadLibraryExW(d3d11Path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
  if (!gDx11BridgeForcedTrexD3D11V200) {
    Logger::err("DX11_V200_FORCE_TREX_DX11_RUNTIME: FAILED: LoadLibraryExW(.trex\\d3d11.dll) failed.");
    return false;
  }

  Logger::info("DX11_V200_FORCE_TREX_DX11_RUNTIME: forced .trex\\d3d11.dll loaded into NvRemixBridge.exe.");

  FARPROC createFactory = GetProcAddress(gDx11BridgeForcedTrexDxgiV200, "CreateDXGIFactory");
  FARPROC createFactory1 = GetProcAddress(gDx11BridgeForcedTrexDxgiV200, "CreateDXGIFactory1");
  FARPROC createFactory2 = GetProcAddress(gDx11BridgeForcedTrexDxgiV200, "CreateDXGIFactory2");
  FARPROC createDevice = GetProcAddress(gDx11BridgeForcedTrexD3D11V200, "D3D11CreateDevice");
  FARPROC createDeviceAndSwapChain = GetProcAddress(gDx11BridgeForcedTrexD3D11V200, "D3D11CreateDeviceAndSwapChain");

  if (!createDevice) {
    Logger::err("DX11_V200_FORCE_TREX_DX11_RUNTIME: FAILED: forced .trex\\d3d11.dll lacks D3D11CreateDevice.");
    return false;
  }

  Logger::info("DX11_V200_FORCE_TREX_DX11_RUNTIME: .trex\\d3d11.dll exports D3D11CreateDevice.");

  if (createDeviceAndSwapChain) {
    Logger::info("DX11_V200_FORCE_TREX_DX11_RUNTIME: .trex\\d3d11.dll exports D3D11CreateDeviceAndSwapChain.");
  } else {
    Logger::warn("DX11_V200_FORCE_TREX_DX11_RUNTIME: .trex\\d3d11.dll lacks D3D11CreateDeviceAndSwapChain.");
  }

  if (createFactory || createFactory1 || createFactory2) {
    Logger::info("DX11_V200_FORCE_TREX_DX11_RUNTIME: .trex\\dxgi.dll exports DXGI factory entrypoints.");
  } else {
    Logger::warn("DX11_V200_FORCE_TREX_DX11_RUNTIME: .trex\\dxgi.dll lacks DXGI factory exports.");
  }

  SetEnvironmentVariableW(L"DX11_BRIDGE_TREX_D3D11_FORCED", L"1");
  SetEnvironmentVariableW(L"DX11_BRIDGE_TREX_DXGI_FORCED", L"1");
  return true;
}
#endif

#ifndef DX11_V201_SERVER_REMIX_INITIALIZER
#define DX11_V201_SERVER_REMIX_INITIALIZER
static HMODULE gDx11BridgeTrexDxgiV201 = nullptr;
static HMODULE gDx11BridgeTrexD3D11V201 = nullptr;

static void Dx11BridgeServerInitializeTrexRemixRuntimeV201() {
  wchar_t mode[64] = {};
  GetEnvironmentVariableW(L"DX11_BRIDGE_MODE", mode, _countof(mode));

  wchar_t exePath[MAX_PATH] = {};
  if (GetModuleFileNameW(nullptr, exePath, _countof(exePath)) == 0) {
    Logger::warn("DX11_V201_SERVER_REMIX_INITIALIZER: could not resolve NvRemixBridge.exe path.");
    return;
  }

  wchar_t* slash = wcsrchr(exePath, L'\\');
  if (!slash) {
    Logger::warn("DX11_V201_SERVER_REMIX_INITIALIZER: could not resolve .trex folder.");
    return;
  }
  slash[1] = 0;

  SetDllDirectoryW(exePath);
  SetEnvironmentVariableW(L"DXVK_REMIX_TREX_DIR", exePath);
  SetEnvironmentVariableW(L"DXVK_REMIX_BRIDGE_SERVER_HOSTS_RUNTIME", L"1");

  wchar_t dxgiPath[MAX_PATH] = {};
  wchar_t d3d11Path[MAX_PATH] = {};
  wcscpy_s(dxgiPath, exePath);
  wcscat_s(dxgiPath, L"dxgi.dll");
  wcscpy_s(d3d11Path, exePath);
  wcscat_s(d3d11Path, L"d3d11.dll");

  gDx11BridgeTrexDxgiV201 = LoadLibraryW(dxgiPath);
  if (gDx11BridgeTrexDxgiV201) {
    Logger::info("DX11_V201_SERVER_REMIX_INITIALIZER: loaded .trex dxgi.dll into NvRemixBridge.exe.");
    FARPROC f0 = GetProcAddress(gDx11BridgeTrexDxgiV201, "CreateDXGIFactory");
    FARPROC f1 = GetProcAddress(gDx11BridgeTrexDxgiV201, "CreateDXGIFactory1");
    FARPROC f2 = GetProcAddress(gDx11BridgeTrexDxgiV201, "CreateDXGIFactory2");
    if (f0 || f1 || f2) {
      Logger::info("DX11_V201_SERVER_REMIX_INITIALIZER: .trex dxgi.dll exports DXGI factory entrypoints.");
    } else {
      Logger::warn("DX11_V201_SERVER_REMIX_INITIALIZER: .trex dxgi.dll loaded but no DXGI factory exports were found.");
    }
  } else {
    Logger::warn("DX11_V201_SERVER_REMIX_INITIALIZER: failed to load .trex dxgi.dll into NvRemixBridge.exe.");
  }

  gDx11BridgeTrexD3D11V201 = LoadLibraryW(d3d11Path);
  if (gDx11BridgeTrexD3D11V201) {
    Logger::info("DX11_V201_SERVER_REMIX_INITIALIZER: loaded .trex d3d11.dll into NvRemixBridge.exe.");
    FARPROC d0 = GetProcAddress(gDx11BridgeTrexD3D11V201, "D3D11CreateDevice");
    FARPROC d1 = GetProcAddress(gDx11BridgeTrexD3D11V201, "D3D11CreateDeviceAndSwapChain");
    if (d0 || d1) {
      Logger::info("DX11_V201_SERVER_REMIX_INITIALIZER: .trex d3d11.dll exports D3D11 device entrypoints.");
    } else {
      Logger::warn("DX11_V201_SERVER_REMIX_INITIALIZER: .trex d3d11.dll loaded but no D3D11 entrypoints were found.");
    }
  } else {
    Logger::warn("DX11_V201_SERVER_REMIX_INITIALIZER: failed to load .trex d3d11.dll into NvRemixBridge.exe.");
  }
}
#endif

#ifndef DX11_V201_FORCE_TREX_DX11_RUNTIME
#define DX11_V201_FORCE_TREX_DX11_RUNTIME
static HMODULE gDx11BridgeForcedTrexDxgiV201 = nullptr;
static HMODULE gDx11BridgeForcedTrexD3D11V201 = nullptr;

static bool Dx11BridgeForceTrexRuntimePathExistsV201(const wchar_t* path) {
  const DWORD attrs = GetFileAttributesW(path);
  return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
}

static bool Dx11BridgeForceGetTrexDirV201(wchar_t* out, DWORD cap) {
  if (!out || cap == 0) return false;
  out[0] = 0;

  wchar_t envTrex[MAX_PATH] = {};
  DWORD got = GetEnvironmentVariableW(L"DXVK_REMIX_TREX_DIR", envTrex, _countof(envTrex));
  if (got > 0 && got < _countof(envTrex)) {
    lstrcpynW(out, envTrex, cap);
  } else {
    wchar_t exePath[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, exePath, _countof(exePath)) == 0) {
      return false;
    }
    wchar_t* slash = wcsrchr(exePath, L'\\');
    if (!slash) {
      return false;
    }
    slash[1] = 0;
    lstrcpynW(out, exePath, cap);
  }

  size_t len = wcslen(out);
  if (len > 0 && out[len - 1] != L'\\' && len + 2 < cap) {
    out[len] = L'\\';
    out[len + 1] = 0;
  }

  return out[0] != 0;
}

static bool Dx11BridgeServerForceLoadTrexDx11RuntimeV201() {
  wchar_t mode[64] = {};
  if (GetEnvironmentVariableW(L"DX11_BRIDGE_MODE", mode, _countof(mode)) == 0) {
    return true;
  }

  wchar_t trexDir[MAX_PATH] = {};
  if (!Dx11BridgeForceGetTrexDirV201(trexDir, _countof(trexDir))) {
    Logger::err("DX11_V201_FORCE_TREX_DX11_RUNTIME: FAILED: could not resolve .trex directory.");
    return false;
  }

  SetDllDirectoryW(trexDir);

  wchar_t dxgiPath[MAX_PATH] = {};
  wchar_t d3d11Path[MAX_PATH] = {};
  wcscpy_s(dxgiPath, trexDir);
  wcscat_s(dxgiPath, L"dxgi.dll");
  wcscpy_s(d3d11Path, trexDir);
  wcscat_s(d3d11Path, L"d3d11.dll");

  if (!Dx11BridgeForceTrexRuntimePathExistsV201(dxgiPath)) {
    Logger::err("DX11_V201_FORCE_TREX_DX11_RUNTIME: FAILED: .trex\\dxgi.dll is missing.");
    return false;
  }

  if (!Dx11BridgeForceTrexRuntimePathExistsV201(d3d11Path)) {
    Logger::err("DX11_V201_FORCE_TREX_DX11_RUNTIME: FAILED: .trex\\d3d11.dll is missing.");
    return false;
  }

  gDx11BridgeForcedTrexDxgiV201 = LoadLibraryExW(dxgiPath, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
  if (!gDx11BridgeForcedTrexDxgiV201) {
    Logger::err("DX11_V201_FORCE_TREX_DX11_RUNTIME: FAILED: LoadLibraryExW(.trex\\dxgi.dll) failed.");
    return false;
  }

  Logger::info("DX11_V201_FORCE_TREX_DX11_RUNTIME: forced .trex\\dxgi.dll loaded into NvRemixBridge.exe.");

  gDx11BridgeForcedTrexD3D11V201 = LoadLibraryExW(d3d11Path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
  if (!gDx11BridgeForcedTrexD3D11V201) {
    Logger::err("DX11_V201_FORCE_TREX_DX11_RUNTIME: FAILED: LoadLibraryExW(.trex\\d3d11.dll) failed.");
    return false;
  }

  Logger::info("DX11_V201_FORCE_TREX_DX11_RUNTIME: forced .trex\\d3d11.dll loaded into NvRemixBridge.exe.");

  FARPROC createFactory = GetProcAddress(gDx11BridgeForcedTrexDxgiV201, "CreateDXGIFactory");
  FARPROC createFactory1 = GetProcAddress(gDx11BridgeForcedTrexDxgiV201, "CreateDXGIFactory1");
  FARPROC createFactory2 = GetProcAddress(gDx11BridgeForcedTrexDxgiV201, "CreateDXGIFactory2");
  FARPROC createDevice = GetProcAddress(gDx11BridgeForcedTrexD3D11V201, "D3D11CreateDevice");
  FARPROC createDeviceAndSwapChain = GetProcAddress(gDx11BridgeForcedTrexD3D11V201, "D3D11CreateDeviceAndSwapChain");

  if (!createDevice) {
    Logger::err("DX11_V201_FORCE_TREX_DX11_RUNTIME: FAILED: forced .trex\\d3d11.dll lacks D3D11CreateDevice.");
    return false;
  }

  Logger::info("DX11_V201_FORCE_TREX_DX11_RUNTIME: .trex\\d3d11.dll exports D3D11CreateDevice.");

  if (createDeviceAndSwapChain) {
    Logger::info("DX11_V201_FORCE_TREX_DX11_RUNTIME: .trex\\d3d11.dll exports D3D11CreateDeviceAndSwapChain.");
  } else {
    Logger::warn("DX11_V201_FORCE_TREX_DX11_RUNTIME: .trex\\d3d11.dll lacks D3D11CreateDeviceAndSwapChain.");
  }

  if (createFactory || createFactory1 || createFactory2) {
    Logger::info("DX11_V201_FORCE_TREX_DX11_RUNTIME: .trex\\dxgi.dll exports DXGI factory entrypoints.");
  } else {
    Logger::warn("DX11_V201_FORCE_TREX_DX11_RUNTIME: .trex\\dxgi.dll lacks DXGI factory exports.");
  }

  SetEnvironmentVariableW(L"DX11_BRIDGE_TREX_D3D11_FORCED", L"1");
  SetEnvironmentVariableW(L"DX11_BRIDGE_TREX_DXGI_FORCED", L"1");
  return true;
}
#endif

#ifndef DX11_V203_SERVER_REMIX_INITIALIZER
#define DX11_V203_SERVER_REMIX_INITIALIZER
static HMODULE gDx11BridgeTrexDxgiV203 = nullptr;
static HMODULE gDx11BridgeTrexD3D11V203 = nullptr;

static void Dx11BridgeServerInitializeTrexRemixRuntimeV203() {
  wchar_t mode[64] = {};
  GetEnvironmentVariableW(L"DX11_BRIDGE_MODE", mode, _countof(mode));

  wchar_t exePath[MAX_PATH] = {};
  if (GetModuleFileNameW(nullptr, exePath, _countof(exePath)) == 0) {
    Logger::warn("DX11_V203_SERVER_REMIX_INITIALIZER: could not resolve NvRemixBridge.exe path.");
    return;
  }

  wchar_t* slash = wcsrchr(exePath, L'\\');
  if (!slash) {
    Logger::warn("DX11_V203_SERVER_REMIX_INITIALIZER: could not resolve .trex folder.");
    return;
  }
  slash[1] = 0;

  SetDllDirectoryW(exePath);
  SetEnvironmentVariableW(L"DXVK_REMIX_TREX_DIR", exePath);
  SetEnvironmentVariableW(L"DXVK_REMIX_BRIDGE_SERVER_HOSTS_RUNTIME", L"1");

  wchar_t dxgiPath[MAX_PATH] = {};
  wchar_t d3d11Path[MAX_PATH] = {};
  wcscpy_s(dxgiPath, exePath);
  wcscat_s(dxgiPath, L"dxgi.dll");
  wcscpy_s(d3d11Path, exePath);
  wcscat_s(d3d11Path, L"d3d11.dll");

  gDx11BridgeTrexDxgiV203 = LoadLibraryW(dxgiPath);
  if (gDx11BridgeTrexDxgiV203) {
    Logger::info("DX11_V203_SERVER_REMIX_INITIALIZER: loaded .trex dxgi.dll into NvRemixBridge.exe.");
    FARPROC f0 = GetProcAddress(gDx11BridgeTrexDxgiV203, "CreateDXGIFactory");
    FARPROC f1 = GetProcAddress(gDx11BridgeTrexDxgiV203, "CreateDXGIFactory1");
    FARPROC f2 = GetProcAddress(gDx11BridgeTrexDxgiV203, "CreateDXGIFactory2");
    if (f0 || f1 || f2) {
      Logger::info("DX11_V203_SERVER_REMIX_INITIALIZER: .trex dxgi.dll exports DXGI factory entrypoints.");
    } else {
      Logger::warn("DX11_V203_SERVER_REMIX_INITIALIZER: .trex dxgi.dll loaded but no DXGI factory exports were found.");
    }
  } else {
    Logger::warn("DX11_V203_SERVER_REMIX_INITIALIZER: failed to load .trex dxgi.dll into NvRemixBridge.exe.");
  }

  gDx11BridgeTrexD3D11V203 = LoadLibraryW(d3d11Path);
  if (gDx11BridgeTrexD3D11V203) {
    Logger::info("DX11_V203_SERVER_REMIX_INITIALIZER: loaded .trex d3d11.dll into NvRemixBridge.exe.");
    FARPROC d0 = GetProcAddress(gDx11BridgeTrexD3D11V203, "D3D11CreateDevice");
    FARPROC d1 = GetProcAddress(gDx11BridgeTrexD3D11V203, "D3D11CreateDeviceAndSwapChain");
    if (d0 || d1) {
      Logger::info("DX11_V203_SERVER_REMIX_INITIALIZER: .trex d3d11.dll exports D3D11 device entrypoints.");
    } else {
      Logger::warn("DX11_V203_SERVER_REMIX_INITIALIZER: .trex d3d11.dll loaded but no D3D11 entrypoints were found.");
    }
  } else {
    Logger::warn("DX11_V203_SERVER_REMIX_INITIALIZER: failed to load .trex d3d11.dll into NvRemixBridge.exe.");
  }
}
#endif

#ifndef DX11_V203_FORCE_TREX_DX11_RUNTIME
#define DX11_V203_FORCE_TREX_DX11_RUNTIME
static HMODULE gDx11BridgeForcedTrexDxgiV203 = nullptr;
static HMODULE gDx11BridgeForcedTrexD3D11V203 = nullptr;

static bool Dx11BridgeForceTrexRuntimePathExistsV203(const wchar_t* path) {
  const DWORD attrs = GetFileAttributesW(path);
  return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
}

static bool Dx11BridgeForceGetTrexDirV203(wchar_t* out, DWORD cap) {
  if (!out || cap == 0) return false;
  out[0] = 0;

  wchar_t envTrex[MAX_PATH] = {};
  DWORD got = GetEnvironmentVariableW(L"DXVK_REMIX_TREX_DIR", envTrex, _countof(envTrex));
  if (got > 0 && got < _countof(envTrex)) {
    lstrcpynW(out, envTrex, cap);
  } else {
    wchar_t exePath[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, exePath, _countof(exePath)) == 0) {
      return false;
    }
    wchar_t* slash = wcsrchr(exePath, L'\\');
    if (!slash) {
      return false;
    }
    slash[1] = 0;
    lstrcpynW(out, exePath, cap);
  }

  size_t len = wcslen(out);
  if (len > 0 && out[len - 1] != L'\\' && len + 2 < cap) {
    out[len] = L'\\';
    out[len + 1] = 0;
  }

  return out[0] != 0;
}

static bool Dx11BridgeServerForceLoadTrexDx11RuntimeV203() {
  wchar_t mode[64] = {};
  if (GetEnvironmentVariableW(L"DX11_BRIDGE_MODE", mode, _countof(mode)) == 0) {
    return true;
  }

  wchar_t trexDir[MAX_PATH] = {};
  if (!Dx11BridgeForceGetTrexDirV203(trexDir, _countof(trexDir))) {
    Logger::err("DX11_V203_FORCE_TREX_DX11_RUNTIME: FAILED: could not resolve .trex directory.");
    return false;
  }

  SetDllDirectoryW(trexDir);

  wchar_t dxgiPath[MAX_PATH] = {};
  wchar_t d3d11Path[MAX_PATH] = {};
  wcscpy_s(dxgiPath, trexDir);
  wcscat_s(dxgiPath, L"dxgi.dll");
  wcscpy_s(d3d11Path, trexDir);
  wcscat_s(d3d11Path, L"d3d11.dll");

  if (!Dx11BridgeForceTrexRuntimePathExistsV203(dxgiPath)) {
    Logger::err("DX11_V203_FORCE_TREX_DX11_RUNTIME: FAILED: .trex\\dxgi.dll is missing.");
    return false;
  }

  if (!Dx11BridgeForceTrexRuntimePathExistsV203(d3d11Path)) {
    Logger::err("DX11_V203_FORCE_TREX_DX11_RUNTIME: FAILED: .trex\\d3d11.dll is missing.");
    return false;
  }

  gDx11BridgeForcedTrexDxgiV203 = LoadLibraryExW(dxgiPath, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
  if (!gDx11BridgeForcedTrexDxgiV203) {
    Logger::err("DX11_V203_FORCE_TREX_DX11_RUNTIME: FAILED: LoadLibraryExW(.trex\\dxgi.dll) failed.");
    return false;
  }

  Logger::info("DX11_V203_FORCE_TREX_DX11_RUNTIME: forced .trex\\dxgi.dll loaded into NvRemixBridge.exe.");

  gDx11BridgeForcedTrexD3D11V203 = LoadLibraryExW(d3d11Path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
  if (!gDx11BridgeForcedTrexD3D11V203) {
    Logger::err("DX11_V203_FORCE_TREX_DX11_RUNTIME: FAILED: LoadLibraryExW(.trex\\d3d11.dll) failed.");
    return false;
  }

  Logger::info("DX11_V203_FORCE_TREX_DX11_RUNTIME: forced .trex\\d3d11.dll loaded into NvRemixBridge.exe.");

  FARPROC createFactory = GetProcAddress(gDx11BridgeForcedTrexDxgiV203, "CreateDXGIFactory");
  FARPROC createFactory1 = GetProcAddress(gDx11BridgeForcedTrexDxgiV203, "CreateDXGIFactory1");
  FARPROC createFactory2 = GetProcAddress(gDx11BridgeForcedTrexDxgiV203, "CreateDXGIFactory2");
  FARPROC createDevice = GetProcAddress(gDx11BridgeForcedTrexD3D11V203, "D3D11CreateDevice");
  FARPROC createDeviceAndSwapChain = GetProcAddress(gDx11BridgeForcedTrexD3D11V203, "D3D11CreateDeviceAndSwapChain");

  if (!createDevice) {
    Logger::err("DX11_V203_FORCE_TREX_DX11_RUNTIME: FAILED: forced .trex\\d3d11.dll lacks D3D11CreateDevice.");
    return false;
  }

  Logger::info("DX11_V203_FORCE_TREX_DX11_RUNTIME: .trex\\d3d11.dll exports D3D11CreateDevice.");

  if (createDeviceAndSwapChain) {
    Logger::info("DX11_V203_FORCE_TREX_DX11_RUNTIME: .trex\\d3d11.dll exports D3D11CreateDeviceAndSwapChain.");
  } else {
    Logger::warn("DX11_V203_FORCE_TREX_DX11_RUNTIME: .trex\\d3d11.dll lacks D3D11CreateDeviceAndSwapChain.");
  }

  if (createFactory || createFactory1 || createFactory2) {
    Logger::info("DX11_V203_FORCE_TREX_DX11_RUNTIME: .trex\\dxgi.dll exports DXGI factory entrypoints.");
  } else {
    Logger::warn("DX11_V203_FORCE_TREX_DX11_RUNTIME: .trex\\dxgi.dll lacks DXGI factory exports.");
  }

  SetEnvironmentVariableW(L"DX11_BRIDGE_TREX_D3D11_FORCED", L"1");
  SetEnvironmentVariableW(L"DX11_BRIDGE_TREX_DXGI_FORCED", L"1");
  return true;
}
#endif

#ifndef DX11_V204_SERVER_REMIX_INITIALIZER
#define DX11_V204_SERVER_REMIX_INITIALIZER
static HMODULE gDx11BridgeTrexDxgiV204 = nullptr;
static HMODULE gDx11BridgeTrexD3D11V204 = nullptr;

static void Dx11BridgeServerInitializeTrexRemixRuntimeV204() {
  wchar_t mode[64] = {};
  GetEnvironmentVariableW(L"DX11_BRIDGE_MODE", mode, _countof(mode));

  wchar_t exePath[MAX_PATH] = {};
  if (GetModuleFileNameW(nullptr, exePath, _countof(exePath)) == 0) {
    Logger::warn("DX11_V204_SERVER_REMIX_INITIALIZER: could not resolve NvRemixBridge.exe path.");
    return;
  }

  wchar_t* slash = wcsrchr(exePath, L'\\');
  if (!slash) {
    Logger::warn("DX11_V204_SERVER_REMIX_INITIALIZER: could not resolve .trex folder.");
    return;
  }
  slash[1] = 0;

  SetDllDirectoryW(exePath);
  SetEnvironmentVariableW(L"DXVK_REMIX_TREX_DIR", exePath);
  SetEnvironmentVariableW(L"DXVK_REMIX_BRIDGE_SERVER_HOSTS_RUNTIME", L"1");

  wchar_t dxgiPath[MAX_PATH] = {};
  wchar_t d3d11Path[MAX_PATH] = {};
  wcscpy_s(dxgiPath, exePath);
  wcscat_s(dxgiPath, L"dxgi.dll");
  wcscpy_s(d3d11Path, exePath);
  wcscat_s(d3d11Path, L"d3d11.dll");

  gDx11BridgeTrexDxgiV204 = LoadLibraryW(dxgiPath);
  if (gDx11BridgeTrexDxgiV204) {
    Logger::info("DX11_V204_SERVER_REMIX_INITIALIZER: loaded .trex dxgi.dll into NvRemixBridge.exe.");
    FARPROC f0 = GetProcAddress(gDx11BridgeTrexDxgiV204, "CreateDXGIFactory");
    FARPROC f1 = GetProcAddress(gDx11BridgeTrexDxgiV204, "CreateDXGIFactory1");
    FARPROC f2 = GetProcAddress(gDx11BridgeTrexDxgiV204, "CreateDXGIFactory2");
    if (f0 || f1 || f2) {
      Logger::info("DX11_V204_SERVER_REMIX_INITIALIZER: .trex dxgi.dll exports DXGI factory entrypoints.");
    } else {
      Logger::warn("DX11_V204_SERVER_REMIX_INITIALIZER: .trex dxgi.dll loaded but no DXGI factory exports were found.");
    }
  } else {
    Logger::warn("DX11_V204_SERVER_REMIX_INITIALIZER: failed to load .trex dxgi.dll into NvRemixBridge.exe.");
  }

  gDx11BridgeTrexD3D11V204 = LoadLibraryW(d3d11Path);
  if (gDx11BridgeTrexD3D11V204) {
    Logger::info("DX11_V204_SERVER_REMIX_INITIALIZER: loaded .trex d3d11.dll into NvRemixBridge.exe.");
    FARPROC d0 = GetProcAddress(gDx11BridgeTrexD3D11V204, "D3D11CreateDevice");
    FARPROC d1 = GetProcAddress(gDx11BridgeTrexD3D11V204, "D3D11CreateDeviceAndSwapChain");
    if (d0 || d1) {
      Logger::info("DX11_V204_SERVER_REMIX_INITIALIZER: .trex d3d11.dll exports D3D11 device entrypoints.");
    } else {
      Logger::warn("DX11_V204_SERVER_REMIX_INITIALIZER: .trex d3d11.dll loaded but no D3D11 entrypoints were found.");
    }
  } else {
    Logger::warn("DX11_V204_SERVER_REMIX_INITIALIZER: failed to load .trex d3d11.dll into NvRemixBridge.exe.");
  }
}
#endif

#ifndef DX11_V204_FORCE_TREX_DX11_RUNTIME
#define DX11_V204_FORCE_TREX_DX11_RUNTIME
static HMODULE gDx11BridgeForcedTrexDxgiV204 = nullptr;
static HMODULE gDx11BridgeForcedTrexD3D11V204 = nullptr;

static bool Dx11BridgeForceTrexRuntimePathExistsV204(const wchar_t* path) {
  const DWORD attrs = GetFileAttributesW(path);
  return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
}

static bool Dx11BridgeForceGetTrexDirV204(wchar_t* out, DWORD cap) {
  if (!out || cap == 0) return false;
  out[0] = 0;

  wchar_t envTrex[MAX_PATH] = {};
  DWORD got = GetEnvironmentVariableW(L"DXVK_REMIX_TREX_DIR", envTrex, _countof(envTrex));
  if (got > 0 && got < _countof(envTrex)) {
    lstrcpynW(out, envTrex, cap);
  } else {
    wchar_t exePath[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, exePath, _countof(exePath)) == 0) {
      return false;
    }
    wchar_t* slash = wcsrchr(exePath, L'\\');
    if (!slash) {
      return false;
    }
    slash[1] = 0;
    lstrcpynW(out, exePath, cap);
  }

  size_t len = wcslen(out);
  if (len > 0 && out[len - 1] != L'\\' && len + 2 < cap) {
    out[len] = L'\\';
    out[len + 1] = 0;
  }

  return out[0] != 0;
}

static bool Dx11BridgeServerForceLoadTrexDx11RuntimeV204() {
  wchar_t mode[64] = {};
  if (GetEnvironmentVariableW(L"DX11_BRIDGE_MODE", mode, _countof(mode)) == 0) {
    return true;
  }

  wchar_t trexDir[MAX_PATH] = {};
  if (!Dx11BridgeForceGetTrexDirV204(trexDir, _countof(trexDir))) {
    Logger::err("DX11_V204_FORCE_TREX_DX11_RUNTIME: FAILED: could not resolve .trex directory.");
    return false;
  }

  SetDllDirectoryW(trexDir);

  wchar_t dxgiPath[MAX_PATH] = {};
  wchar_t d3d11Path[MAX_PATH] = {};
  wcscpy_s(dxgiPath, trexDir);
  wcscat_s(dxgiPath, L"dxgi.dll");
  wcscpy_s(d3d11Path, trexDir);
  wcscat_s(d3d11Path, L"d3d11.dll");

  if (!Dx11BridgeForceTrexRuntimePathExistsV204(dxgiPath)) {
    Logger::err("DX11_V204_FORCE_TREX_DX11_RUNTIME: FAILED: .trex\\dxgi.dll is missing.");
    return false;
  }

  if (!Dx11BridgeForceTrexRuntimePathExistsV204(d3d11Path)) {
    Logger::err("DX11_V204_FORCE_TREX_DX11_RUNTIME: FAILED: .trex\\d3d11.dll is missing.");
    return false;
  }

  gDx11BridgeForcedTrexDxgiV204 = LoadLibraryExW(dxgiPath, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
  if (!gDx11BridgeForcedTrexDxgiV204) {
    Logger::err("DX11_V204_FORCE_TREX_DX11_RUNTIME: FAILED: LoadLibraryExW(.trex\\dxgi.dll) failed.");
    return false;
  }

  Logger::info("DX11_V204_FORCE_TREX_DX11_RUNTIME: forced .trex\\dxgi.dll loaded into NvRemixBridge.exe.");

  gDx11BridgeForcedTrexD3D11V204 = LoadLibraryExW(d3d11Path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
  if (!gDx11BridgeForcedTrexD3D11V204) {
    Logger::err("DX11_V204_FORCE_TREX_DX11_RUNTIME: FAILED: LoadLibraryExW(.trex\\d3d11.dll) failed.");
    return false;
  }

  Logger::info("DX11_V204_FORCE_TREX_DX11_RUNTIME: forced .trex\\d3d11.dll loaded into NvRemixBridge.exe.");

  FARPROC createFactory = GetProcAddress(gDx11BridgeForcedTrexDxgiV204, "CreateDXGIFactory");
  FARPROC createFactory1 = GetProcAddress(gDx11BridgeForcedTrexDxgiV204, "CreateDXGIFactory1");
  FARPROC createFactory2 = GetProcAddress(gDx11BridgeForcedTrexDxgiV204, "CreateDXGIFactory2");
  FARPROC createDevice = GetProcAddress(gDx11BridgeForcedTrexD3D11V204, "D3D11CreateDevice");
  FARPROC createDeviceAndSwapChain = GetProcAddress(gDx11BridgeForcedTrexD3D11V204, "D3D11CreateDeviceAndSwapChain");

  if (!createDevice) {
    Logger::err("DX11_V204_FORCE_TREX_DX11_RUNTIME: FAILED: forced .trex\\d3d11.dll lacks D3D11CreateDevice.");
    return false;
  }

  Logger::info("DX11_V204_FORCE_TREX_DX11_RUNTIME: .trex\\d3d11.dll exports D3D11CreateDevice.");

  if (createDeviceAndSwapChain) {
    Logger::info("DX11_V204_FORCE_TREX_DX11_RUNTIME: .trex\\d3d11.dll exports D3D11CreateDeviceAndSwapChain.");
  } else {
    Logger::warn("DX11_V204_FORCE_TREX_DX11_RUNTIME: .trex\\d3d11.dll lacks D3D11CreateDeviceAndSwapChain.");
  }

  if (createFactory || createFactory1 || createFactory2) {
    Logger::info("DX11_V204_FORCE_TREX_DX11_RUNTIME: .trex\\dxgi.dll exports DXGI factory entrypoints.");
  } else {
    Logger::warn("DX11_V204_FORCE_TREX_DX11_RUNTIME: .trex\\dxgi.dll lacks DXGI factory exports.");
  }

  SetEnvironmentVariableW(L"DX11_BRIDGE_TREX_D3D11_FORCED", L"1");
  SetEnvironmentVariableW(L"DX11_BRIDGE_TREX_DXGI_FORCED", L"1");
  return true;
}
#endif

#ifndef DX11_V209_SERVER_REMIX_INITIALIZER
#define DX11_V209_SERVER_REMIX_INITIALIZER
static HMODULE gDx11BridgeTrexDxgiV209 = nullptr;
static HMODULE gDx11BridgeTrexD3D11V209 = nullptr;

static void Dx11BridgeServerInitializeTrexRemixRuntimeV209() {
  wchar_t mode[64] = {};
  GetEnvironmentVariableW(L"DX11_BRIDGE_MODE", mode, _countof(mode));

  wchar_t exePath[MAX_PATH] = {};
  if (GetModuleFileNameW(nullptr, exePath, _countof(exePath)) == 0) {
    Logger::warn("DX11_V209_SERVER_REMIX_INITIALIZER: could not resolve NvRemixBridge.exe path.");
    return;
  }

  wchar_t* slash = wcsrchr(exePath, L'\\');
  if (!slash) {
    Logger::warn("DX11_V209_SERVER_REMIX_INITIALIZER: could not resolve .trex folder.");
    return;
  }
  slash[1] = 0;

  SetDllDirectoryW(exePath);
  SetEnvironmentVariableW(L"DXVK_REMIX_TREX_DIR", exePath);
  SetEnvironmentVariableW(L"DXVK_REMIX_BRIDGE_SERVER_HOSTS_RUNTIME", L"1");

  wchar_t dxgiPath[MAX_PATH] = {};
  wchar_t d3d11Path[MAX_PATH] = {};
  wcscpy_s(dxgiPath, exePath);
  wcscat_s(dxgiPath, L"dxgi.dll");
  wcscpy_s(d3d11Path, exePath);
  wcscat_s(d3d11Path, L"d3d11.dll");

  gDx11BridgeTrexDxgiV209 = LoadLibraryW(dxgiPath);
  if (gDx11BridgeTrexDxgiV209) {
    Logger::info("DX11_V209_SERVER_REMIX_INITIALIZER: loaded .trex dxgi.dll into NvRemixBridge.exe.");
    FARPROC f0 = GetProcAddress(gDx11BridgeTrexDxgiV209, "CreateDXGIFactory");
    FARPROC f1 = GetProcAddress(gDx11BridgeTrexDxgiV209, "CreateDXGIFactory1");
    FARPROC f2 = GetProcAddress(gDx11BridgeTrexDxgiV209, "CreateDXGIFactory2");
    if (f0 || f1 || f2) {
      Logger::info("DX11_V209_SERVER_REMIX_INITIALIZER: .trex dxgi.dll exports DXGI factory entrypoints.");
    } else {
      Logger::warn("DX11_V209_SERVER_REMIX_INITIALIZER: .trex dxgi.dll loaded but no DXGI factory exports were found.");
    }
  } else {
    Logger::warn("DX11_V209_SERVER_REMIX_INITIALIZER: failed to load .trex dxgi.dll into NvRemixBridge.exe.");
  }

  gDx11BridgeTrexD3D11V209 = LoadLibraryW(d3d11Path);
  if (gDx11BridgeTrexD3D11V209) {
    Logger::info("DX11_V209_SERVER_REMIX_INITIALIZER: loaded .trex d3d11.dll into NvRemixBridge.exe.");
    FARPROC d0 = GetProcAddress(gDx11BridgeTrexD3D11V209, "D3D11CreateDevice");
    FARPROC d1 = GetProcAddress(gDx11BridgeTrexD3D11V209, "D3D11CreateDeviceAndSwapChain");
    if (d0 || d1) {
      Logger::info("DX11_V209_SERVER_REMIX_INITIALIZER: .trex d3d11.dll exports D3D11 device entrypoints.");
    } else {
      Logger::warn("DX11_V209_SERVER_REMIX_INITIALIZER: .trex d3d11.dll loaded but no D3D11 entrypoints were found.");
    }
  } else {
    Logger::warn("DX11_V209_SERVER_REMIX_INITIALIZER: failed to load .trex d3d11.dll into NvRemixBridge.exe.");
  }
}
#endif

#ifndef DX11_V209_FORCE_TREX_DX11_RUNTIME
#define DX11_V209_FORCE_TREX_DX11_RUNTIME
static HMODULE gDx11BridgeForcedTrexDxgiV209 = nullptr;
static HMODULE gDx11BridgeForcedTrexD3D11V209 = nullptr;

static bool Dx11BridgeForceTrexRuntimePathExistsV209(const wchar_t* path) {
  const DWORD attrs = GetFileAttributesW(path);
  return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
}

static bool Dx11BridgeForceGetTrexDirV209(wchar_t* out, DWORD cap) {
  if (!out || cap == 0) return false;
  out[0] = 0;

  wchar_t envTrex[MAX_PATH] = {};
  DWORD got = GetEnvironmentVariableW(L"DXVK_REMIX_TREX_DIR", envTrex, _countof(envTrex));
  if (got > 0 && got < _countof(envTrex)) {
    lstrcpynW(out, envTrex, cap);
  } else {
    wchar_t exePath[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, exePath, _countof(exePath)) == 0) {
      return false;
    }
    wchar_t* slash = wcsrchr(exePath, L'\\');
    if (!slash) {
      return false;
    }
    slash[1] = 0;
    lstrcpynW(out, exePath, cap);
  }

  size_t len = wcslen(out);
  if (len > 0 && out[len - 1] != L'\\' && len + 2 < cap) {
    out[len] = L'\\';
    out[len + 1] = 0;
  }

  return out[0] != 0;
}

static bool Dx11BridgeServerForceLoadTrexDx11RuntimeV209() {
  wchar_t mode[64] = {};
  if (GetEnvironmentVariableW(L"DX11_BRIDGE_MODE", mode, _countof(mode)) == 0) {
    return true;
  }

  wchar_t trexDir[MAX_PATH] = {};
  if (!Dx11BridgeForceGetTrexDirV209(trexDir, _countof(trexDir))) {
    Logger::err("DX11_V209_FORCE_TREX_DX11_RUNTIME: FAILED: could not resolve .trex directory.");
    return false;
  }

  SetDllDirectoryW(trexDir);

  wchar_t dxgiPath[MAX_PATH] = {};
  wchar_t d3d11Path[MAX_PATH] = {};
  wcscpy_s(dxgiPath, trexDir);
  wcscat_s(dxgiPath, L"dxgi.dll");
  wcscpy_s(d3d11Path, trexDir);
  wcscat_s(d3d11Path, L"d3d11.dll");

  if (!Dx11BridgeForceTrexRuntimePathExistsV209(dxgiPath)) {
    Logger::err("DX11_V209_FORCE_TREX_DX11_RUNTIME: FAILED: .trex\\dxgi.dll is missing.");
    return false;
  }

  if (!Dx11BridgeForceTrexRuntimePathExistsV209(d3d11Path)) {
    Logger::err("DX11_V209_FORCE_TREX_DX11_RUNTIME: FAILED: .trex\\d3d11.dll is missing.");
    return false;
  }

  gDx11BridgeForcedTrexDxgiV209 = LoadLibraryExW(dxgiPath, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
  if (!gDx11BridgeForcedTrexDxgiV209) {
    Logger::err("DX11_V209_FORCE_TREX_DX11_RUNTIME: FAILED: LoadLibraryExW(.trex\\dxgi.dll) failed.");
    return false;
  }

  Logger::info("DX11_V209_FORCE_TREX_DX11_RUNTIME: forced .trex\\dxgi.dll loaded into NvRemixBridge.exe.");

  gDx11BridgeForcedTrexD3D11V209 = LoadLibraryExW(d3d11Path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
  if (!gDx11BridgeForcedTrexD3D11V209) {
    Logger::err("DX11_V209_FORCE_TREX_DX11_RUNTIME: FAILED: LoadLibraryExW(.trex\\d3d11.dll) failed.");
    return false;
  }

  Logger::info("DX11_V209_FORCE_TREX_DX11_RUNTIME: forced .trex\\d3d11.dll loaded into NvRemixBridge.exe.");

  FARPROC createFactory = GetProcAddress(gDx11BridgeForcedTrexDxgiV209, "CreateDXGIFactory");
  FARPROC createFactory1 = GetProcAddress(gDx11BridgeForcedTrexDxgiV209, "CreateDXGIFactory1");
  FARPROC createFactory2 = GetProcAddress(gDx11BridgeForcedTrexDxgiV209, "CreateDXGIFactory2");
  FARPROC createDevice = GetProcAddress(gDx11BridgeForcedTrexD3D11V209, "D3D11CreateDevice");
  FARPROC createDeviceAndSwapChain = GetProcAddress(gDx11BridgeForcedTrexD3D11V209, "D3D11CreateDeviceAndSwapChain");

  if (!createDevice) {
    Logger::err("DX11_V209_FORCE_TREX_DX11_RUNTIME: FAILED: forced .trex\\d3d11.dll lacks D3D11CreateDevice.");
    return false;
  }

  Logger::info("DX11_V209_FORCE_TREX_DX11_RUNTIME: .trex\\d3d11.dll exports D3D11CreateDevice.");

  if (createDeviceAndSwapChain) {
    Logger::info("DX11_V209_FORCE_TREX_DX11_RUNTIME: .trex\\d3d11.dll exports D3D11CreateDeviceAndSwapChain.");
  } else {
    Logger::warn("DX11_V209_FORCE_TREX_DX11_RUNTIME: .trex\\d3d11.dll lacks D3D11CreateDeviceAndSwapChain.");
  }

  if (createFactory || createFactory1 || createFactory2) {
    Logger::info("DX11_V209_FORCE_TREX_DX11_RUNTIME: .trex\\dxgi.dll exports DXGI factory entrypoints.");
  } else {
    Logger::warn("DX11_V209_FORCE_TREX_DX11_RUNTIME: .trex\\dxgi.dll lacks DXGI factory exports.");
  }

  SetEnvironmentVariableW(L"DX11_BRIDGE_TREX_D3D11_FORCED", L"1");
  SetEnvironmentVariableW(L"DX11_BRIDGE_TREX_DXGI_FORCED", L"1");
  return true;
}
#endif

#ifndef DX11_V210_SERVER_REMIX_INITIALIZER
#define DX11_V210_SERVER_REMIX_INITIALIZER
static HMODULE gDx11BridgeTrexDxgiV210 = nullptr;
static HMODULE gDx11BridgeTrexD3D11V210 = nullptr;

static void Dx11BridgeServerInitializeTrexRemixRuntimeV210() {
  wchar_t mode[64] = {};
  GetEnvironmentVariableW(L"DX11_BRIDGE_MODE", mode, _countof(mode));

  wchar_t exePath[MAX_PATH] = {};
  if (GetModuleFileNameW(nullptr, exePath, _countof(exePath)) == 0) {
    Logger::warn("DX11_V210_SERVER_REMIX_INITIALIZER: could not resolve NvRemixBridge.exe path.");
    return;
  }

  wchar_t* slash = wcsrchr(exePath, L'\\');
  if (!slash) {
    Logger::warn("DX11_V210_SERVER_REMIX_INITIALIZER: could not resolve .trex folder.");
    return;
  }
  slash[1] = 0;

  SetDllDirectoryW(exePath);
  SetEnvironmentVariableW(L"DXVK_REMIX_TREX_DIR", exePath);
  SetEnvironmentVariableW(L"DXVK_REMIX_BRIDGE_SERVER_HOSTS_RUNTIME", L"1");

  wchar_t dxgiPath[MAX_PATH] = {};
  wchar_t d3d11Path[MAX_PATH] = {};
  wcscpy_s(dxgiPath, exePath);
  wcscat_s(dxgiPath, L"dxgi.dll");
  wcscpy_s(d3d11Path, exePath);
  wcscat_s(d3d11Path, L"d3d11.dll");

  gDx11BridgeTrexDxgiV210 = LoadLibraryW(dxgiPath);
  if (gDx11BridgeTrexDxgiV210) {
    Logger::info("DX11_V210_SERVER_REMIX_INITIALIZER: loaded .trex dxgi.dll into NvRemixBridge.exe.");
    FARPROC f0 = GetProcAddress(gDx11BridgeTrexDxgiV210, "CreateDXGIFactory");
    FARPROC f1 = GetProcAddress(gDx11BridgeTrexDxgiV210, "CreateDXGIFactory1");
    FARPROC f2 = GetProcAddress(gDx11BridgeTrexDxgiV210, "CreateDXGIFactory2");
    if (f0 || f1 || f2) {
      Logger::info("DX11_V210_SERVER_REMIX_INITIALIZER: .trex dxgi.dll exports DXGI factory entrypoints.");
    } else {
      Logger::warn("DX11_V210_SERVER_REMIX_INITIALIZER: .trex dxgi.dll loaded but no DXGI factory exports were found.");
    }
  } else {
    Logger::warn("DX11_V210_SERVER_REMIX_INITIALIZER: failed to load .trex dxgi.dll into NvRemixBridge.exe.");
  }

  gDx11BridgeTrexD3D11V210 = LoadLibraryW(d3d11Path);
  if (gDx11BridgeTrexD3D11V210) {
    Logger::info("DX11_V210_SERVER_REMIX_INITIALIZER: loaded .trex d3d11.dll into NvRemixBridge.exe.");
    FARPROC d0 = GetProcAddress(gDx11BridgeTrexD3D11V210, "D3D11CreateDevice");
    FARPROC d1 = GetProcAddress(gDx11BridgeTrexD3D11V210, "D3D11CreateDeviceAndSwapChain");
    if (d0 || d1) {
      Logger::info("DX11_V210_SERVER_REMIX_INITIALIZER: .trex d3d11.dll exports D3D11 device entrypoints.");
    } else {
      Logger::warn("DX11_V210_SERVER_REMIX_INITIALIZER: .trex d3d11.dll loaded but no D3D11 entrypoints were found.");
    }
  } else {
    Logger::warn("DX11_V210_SERVER_REMIX_INITIALIZER: failed to load .trex d3d11.dll into NvRemixBridge.exe.");
  }
}
#endif

#ifndef DX11_V210_FORCE_TREX_DX11_RUNTIME
#define DX11_V210_FORCE_TREX_DX11_RUNTIME
static HMODULE gDx11BridgeForcedTrexDxgiV210 = nullptr;
static HMODULE gDx11BridgeForcedTrexD3D11V210 = nullptr;

static bool Dx11BridgeForceTrexRuntimePathExistsV210(const wchar_t* path) {
  const DWORD attrs = GetFileAttributesW(path);
  return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
}

static bool Dx11BridgeForceGetTrexDirV210(wchar_t* out, DWORD cap) {
  if (!out || cap == 0) return false;
  out[0] = 0;

  wchar_t envTrex[MAX_PATH] = {};
  DWORD got = GetEnvironmentVariableW(L"DXVK_REMIX_TREX_DIR", envTrex, _countof(envTrex));
  if (got > 0 && got < _countof(envTrex)) {
    lstrcpynW(out, envTrex, cap);
  } else {
    wchar_t exePath[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, exePath, _countof(exePath)) == 0) {
      return false;
    }
    wchar_t* slash = wcsrchr(exePath, L'\\');
    if (!slash) {
      return false;
    }
    slash[1] = 0;
    lstrcpynW(out, exePath, cap);
  }

  size_t len = wcslen(out);
  if (len > 0 && out[len - 1] != L'\\' && len + 2 < cap) {
    out[len] = L'\\';
    out[len + 1] = 0;
  }

  return out[0] != 0;
}

static bool Dx11BridgeServerForceLoadTrexDx11RuntimeV210() {
  wchar_t mode[64] = {};
  if (GetEnvironmentVariableW(L"DX11_BRIDGE_MODE", mode, _countof(mode)) == 0) {
    return true;
  }

  wchar_t trexDir[MAX_PATH] = {};
  if (!Dx11BridgeForceGetTrexDirV210(trexDir, _countof(trexDir))) {
    Logger::err("DX11_V210_FORCE_TREX_DX11_RUNTIME: FAILED: could not resolve .trex directory.");
    return false;
  }

  SetDllDirectoryW(trexDir);

  wchar_t dxgiPath[MAX_PATH] = {};
  wchar_t d3d11Path[MAX_PATH] = {};
  wcscpy_s(dxgiPath, trexDir);
  wcscat_s(dxgiPath, L"dxgi.dll");
  wcscpy_s(d3d11Path, trexDir);
  wcscat_s(d3d11Path, L"d3d11.dll");

  if (!Dx11BridgeForceTrexRuntimePathExistsV210(dxgiPath)) {
    Logger::err("DX11_V210_FORCE_TREX_DX11_RUNTIME: FAILED: .trex\\dxgi.dll is missing.");
    return false;
  }

  if (!Dx11BridgeForceTrexRuntimePathExistsV210(d3d11Path)) {
    Logger::err("DX11_V210_FORCE_TREX_DX11_RUNTIME: FAILED: .trex\\d3d11.dll is missing.");
    return false;
  }

  gDx11BridgeForcedTrexDxgiV210 = LoadLibraryExW(dxgiPath, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
  if (!gDx11BridgeForcedTrexDxgiV210) {
    Logger::err("DX11_V210_FORCE_TREX_DX11_RUNTIME: FAILED: LoadLibraryExW(.trex\\dxgi.dll) failed.");
    return false;
  }

  Logger::info("DX11_V210_FORCE_TREX_DX11_RUNTIME: forced .trex\\dxgi.dll loaded into NvRemixBridge.exe.");

  gDx11BridgeForcedTrexD3D11V210 = LoadLibraryExW(d3d11Path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
  if (!gDx11BridgeForcedTrexD3D11V210) {
    Logger::err("DX11_V210_FORCE_TREX_DX11_RUNTIME: FAILED: LoadLibraryExW(.trex\\d3d11.dll) failed.");
    return false;
  }

  Logger::info("DX11_V210_FORCE_TREX_DX11_RUNTIME: forced .trex\\d3d11.dll loaded into NvRemixBridge.exe.");

  FARPROC createFactory = GetProcAddress(gDx11BridgeForcedTrexDxgiV210, "CreateDXGIFactory");
  FARPROC createFactory1 = GetProcAddress(gDx11BridgeForcedTrexDxgiV210, "CreateDXGIFactory1");
  FARPROC createFactory2 = GetProcAddress(gDx11BridgeForcedTrexDxgiV210, "CreateDXGIFactory2");
  FARPROC createDevice = GetProcAddress(gDx11BridgeForcedTrexD3D11V210, "D3D11CreateDevice");
  FARPROC createDeviceAndSwapChain = GetProcAddress(gDx11BridgeForcedTrexD3D11V210, "D3D11CreateDeviceAndSwapChain");

  if (!createDevice) {
    Logger::err("DX11_V210_FORCE_TREX_DX11_RUNTIME: FAILED: forced .trex\\d3d11.dll lacks D3D11CreateDevice.");
    return false;
  }

  Logger::info("DX11_V210_FORCE_TREX_DX11_RUNTIME: .trex\\d3d11.dll exports D3D11CreateDevice.");

  if (createDeviceAndSwapChain) {
    Logger::info("DX11_V210_FORCE_TREX_DX11_RUNTIME: .trex\\d3d11.dll exports D3D11CreateDeviceAndSwapChain.");
  } else {
    Logger::warn("DX11_V210_FORCE_TREX_DX11_RUNTIME: .trex\\d3d11.dll lacks D3D11CreateDeviceAndSwapChain.");
  }

  if (createFactory || createFactory1 || createFactory2) {
    Logger::info("DX11_V210_FORCE_TREX_DX11_RUNTIME: .trex\\dxgi.dll exports DXGI factory entrypoints.");
  } else {
    Logger::warn("DX11_V210_FORCE_TREX_DX11_RUNTIME: .trex\\dxgi.dll lacks DXGI factory exports.");
  }

  SetEnvironmentVariableW(L"DX11_BRIDGE_TREX_D3D11_FORCED", L"1");
  SetEnvironmentVariableW(L"DX11_BRIDGE_TREX_DXGI_FORCED", L"1");
  return true;
}
#endif

#ifndef DX11_V211_SERVER_REMIX_INITIALIZER
#define DX11_V211_SERVER_REMIX_INITIALIZER
static HMODULE gDx11BridgeTrexDxgiV211 = nullptr;
static HMODULE gDx11BridgeTrexD3D11V211 = nullptr;

static void Dx11BridgeServerInitializeTrexRemixRuntimeV211() {
  wchar_t mode[64] = {};
  GetEnvironmentVariableW(L"DX11_BRIDGE_MODE", mode, _countof(mode));

  wchar_t exePath[MAX_PATH] = {};
  if (GetModuleFileNameW(nullptr, exePath, _countof(exePath)) == 0) {
    Logger::warn("DX11_V211_SERVER_REMIX_INITIALIZER: could not resolve NvRemixBridge.exe path.");
    return;
  }

  wchar_t* slash = wcsrchr(exePath, L'\\');
  if (!slash) {
    Logger::warn("DX11_V211_SERVER_REMIX_INITIALIZER: could not resolve .trex folder.");
    return;
  }
  slash[1] = 0;

  SetDllDirectoryW(exePath);
  SetEnvironmentVariableW(L"DXVK_REMIX_TREX_DIR", exePath);
  SetEnvironmentVariableW(L"DXVK_REMIX_BRIDGE_SERVER_HOSTS_RUNTIME", L"1");

  wchar_t dxgiPath[MAX_PATH] = {};
  wchar_t d3d11Path[MAX_PATH] = {};
  wcscpy_s(dxgiPath, exePath);
  wcscat_s(dxgiPath, L"dxgi.dll");
  wcscpy_s(d3d11Path, exePath);
  wcscat_s(d3d11Path, L"d3d11.dll");

  gDx11BridgeTrexDxgiV211 = LoadLibraryW(dxgiPath);
  if (gDx11BridgeTrexDxgiV211) {
    Logger::info("DX11_V211_SERVER_REMIX_INITIALIZER: loaded .trex dxgi.dll into NvRemixBridge.exe.");
    FARPROC f0 = GetProcAddress(gDx11BridgeTrexDxgiV211, "CreateDXGIFactory");
    FARPROC f1 = GetProcAddress(gDx11BridgeTrexDxgiV211, "CreateDXGIFactory1");
    FARPROC f2 = GetProcAddress(gDx11BridgeTrexDxgiV211, "CreateDXGIFactory2");
    if (f0 || f1 || f2) {
      Logger::info("DX11_V211_SERVER_REMIX_INITIALIZER: .trex dxgi.dll exports DXGI factory entrypoints.");
    } else {
      Logger::warn("DX11_V211_SERVER_REMIX_INITIALIZER: .trex dxgi.dll loaded but no DXGI factory exports were found.");
    }
  } else {
    Logger::warn("DX11_V211_SERVER_REMIX_INITIALIZER: failed to load .trex dxgi.dll into NvRemixBridge.exe.");
  }

  gDx11BridgeTrexD3D11V211 = LoadLibraryW(d3d11Path);
  if (gDx11BridgeTrexD3D11V211) {
    Logger::info("DX11_V211_SERVER_REMIX_INITIALIZER: loaded .trex d3d11.dll into NvRemixBridge.exe.");
    FARPROC d0 = GetProcAddress(gDx11BridgeTrexD3D11V211, "D3D11CreateDevice");
    FARPROC d1 = GetProcAddress(gDx11BridgeTrexD3D11V211, "D3D11CreateDeviceAndSwapChain");
    if (d0 || d1) {
      Logger::info("DX11_V211_SERVER_REMIX_INITIALIZER: .trex d3d11.dll exports D3D11 device entrypoints.");
    } else {
      Logger::warn("DX11_V211_SERVER_REMIX_INITIALIZER: .trex d3d11.dll loaded but no D3D11 entrypoints were found.");
    }
  } else {
    Logger::warn("DX11_V211_SERVER_REMIX_INITIALIZER: failed to load .trex d3d11.dll into NvRemixBridge.exe.");
  }
}
#endif

#ifndef DX11_V211_FORCE_TREX_DX11_RUNTIME
#define DX11_V211_FORCE_TREX_DX11_RUNTIME
static HMODULE gDx11BridgeForcedTrexDxgiV211 = nullptr;
static HMODULE gDx11BridgeForcedTrexD3D11V211 = nullptr;

static bool Dx11BridgeForceTrexRuntimePathExistsV211(const wchar_t* path) {
  const DWORD attrs = GetFileAttributesW(path);
  return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
}

static bool Dx11BridgeForceGetTrexDirV211(wchar_t* out, DWORD cap) {
  if (!out || cap == 0) return false;
  out[0] = 0;

  wchar_t envTrex[MAX_PATH] = {};
  DWORD got = GetEnvironmentVariableW(L"DXVK_REMIX_TREX_DIR", envTrex, _countof(envTrex));
  if (got > 0 && got < _countof(envTrex)) {
    lstrcpynW(out, envTrex, cap);
  } else {
    wchar_t exePath[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, exePath, _countof(exePath)) == 0) {
      return false;
    }
    wchar_t* slash = wcsrchr(exePath, L'\\');
    if (!slash) {
      return false;
    }
    slash[1] = 0;
    lstrcpynW(out, exePath, cap);
  }

  size_t len = wcslen(out);
  if (len > 0 && out[len - 1] != L'\\' && len + 2 < cap) {
    out[len] = L'\\';
    out[len + 1] = 0;
  }

  return out[0] != 0;
}

static bool Dx11BridgeServerForceLoadTrexDx11RuntimeV211() {
  wchar_t mode[64] = {};
  if (GetEnvironmentVariableW(L"DX11_BRIDGE_MODE", mode, _countof(mode)) == 0) {
    return true;
  }

  wchar_t trexDir[MAX_PATH] = {};
  if (!Dx11BridgeForceGetTrexDirV211(trexDir, _countof(trexDir))) {
    Logger::err("DX11_V211_FORCE_TREX_DX11_RUNTIME: FAILED: could not resolve .trex directory.");
    return false;
  }

  SetDllDirectoryW(trexDir);

  wchar_t dxgiPath[MAX_PATH] = {};
  wchar_t d3d11Path[MAX_PATH] = {};
  wcscpy_s(dxgiPath, trexDir);
  wcscat_s(dxgiPath, L"dxgi.dll");
  wcscpy_s(d3d11Path, trexDir);
  wcscat_s(d3d11Path, L"d3d11.dll");

  if (!Dx11BridgeForceTrexRuntimePathExistsV211(dxgiPath)) {
    Logger::err("DX11_V211_FORCE_TREX_DX11_RUNTIME: FAILED: .trex\\dxgi.dll is missing.");
    return false;
  }

  if (!Dx11BridgeForceTrexRuntimePathExistsV211(d3d11Path)) {
    Logger::err("DX11_V211_FORCE_TREX_DX11_RUNTIME: FAILED: .trex\\d3d11.dll is missing.");
    return false;
  }

  gDx11BridgeForcedTrexDxgiV211 = LoadLibraryExW(dxgiPath, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
  if (!gDx11BridgeForcedTrexDxgiV211) {
    Logger::err("DX11_V211_FORCE_TREX_DX11_RUNTIME: FAILED: LoadLibraryExW(.trex\\dxgi.dll) failed.");
    return false;
  }

  Logger::info("DX11_V211_FORCE_TREX_DX11_RUNTIME: forced .trex\\dxgi.dll loaded into NvRemixBridge.exe.");

  gDx11BridgeForcedTrexD3D11V211 = LoadLibraryExW(d3d11Path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
  if (!gDx11BridgeForcedTrexD3D11V211) {
    Logger::err("DX11_V211_FORCE_TREX_DX11_RUNTIME: FAILED: LoadLibraryExW(.trex\\d3d11.dll) failed.");
    return false;
  }

  Logger::info("DX11_V211_FORCE_TREX_DX11_RUNTIME: forced .trex\\d3d11.dll loaded into NvRemixBridge.exe.");

  FARPROC createFactory = GetProcAddress(gDx11BridgeForcedTrexDxgiV211, "CreateDXGIFactory");
  FARPROC createFactory1 = GetProcAddress(gDx11BridgeForcedTrexDxgiV211, "CreateDXGIFactory1");
  FARPROC createFactory2 = GetProcAddress(gDx11BridgeForcedTrexDxgiV211, "CreateDXGIFactory2");
  FARPROC createDevice = GetProcAddress(gDx11BridgeForcedTrexD3D11V211, "D3D11CreateDevice");
  FARPROC createDeviceAndSwapChain = GetProcAddress(gDx11BridgeForcedTrexD3D11V211, "D3D11CreateDeviceAndSwapChain");

  if (!createDevice) {
    Logger::err("DX11_V211_FORCE_TREX_DX11_RUNTIME: FAILED: forced .trex\\d3d11.dll lacks D3D11CreateDevice.");
    return false;
  }

  Logger::info("DX11_V211_FORCE_TREX_DX11_RUNTIME: .trex\\d3d11.dll exports D3D11CreateDevice.");

  if (createDeviceAndSwapChain) {
    Logger::info("DX11_V211_FORCE_TREX_DX11_RUNTIME: .trex\\d3d11.dll exports D3D11CreateDeviceAndSwapChain.");
  } else {
    Logger::warn("DX11_V211_FORCE_TREX_DX11_RUNTIME: .trex\\d3d11.dll lacks D3D11CreateDeviceAndSwapChain.");
  }

  if (createFactory || createFactory1 || createFactory2) {
    Logger::info("DX11_V211_FORCE_TREX_DX11_RUNTIME: .trex\\dxgi.dll exports DXGI factory entrypoints.");
  } else {
    Logger::warn("DX11_V211_FORCE_TREX_DX11_RUNTIME: .trex\\dxgi.dll lacks DXGI factory exports.");
  }

  SetEnvironmentVariableW(L"DX11_BRIDGE_TREX_D3D11_FORCED", L"1");
  SetEnvironmentVariableW(L"DX11_BRIDGE_TREX_DXGI_FORCED", L"1");
  return true;
}
#endif

#ifndef DX11_V212_SERVER_REMIX_INITIALIZER
#define DX11_V212_SERVER_REMIX_INITIALIZER
static HMODULE gDx11BridgeTrexDxgiV212 = nullptr;
static HMODULE gDx11BridgeTrexD3D11V212 = nullptr;

static void Dx11BridgeServerInitializeTrexRemixRuntimeV212() {
  wchar_t mode[64] = {};
  GetEnvironmentVariableW(L"DX11_BRIDGE_MODE", mode, _countof(mode));

  wchar_t exePath[MAX_PATH] = {};
  if (GetModuleFileNameW(nullptr, exePath, _countof(exePath)) == 0) {
    Logger::warn("DX11_V212_SERVER_REMIX_INITIALIZER: could not resolve NvRemixBridge.exe path.");
    return;
  }

  wchar_t* slash = wcsrchr(exePath, L'\\');
  if (!slash) {
    Logger::warn("DX11_V212_SERVER_REMIX_INITIALIZER: could not resolve .trex folder.");
    return;
  }
  slash[1] = 0;

  SetDllDirectoryW(exePath);
  SetEnvironmentVariableW(L"DXVK_REMIX_TREX_DIR", exePath);
  SetEnvironmentVariableW(L"DXVK_REMIX_BRIDGE_SERVER_HOSTS_RUNTIME", L"1");

  wchar_t dxgiPath[MAX_PATH] = {};
  wchar_t d3d11Path[MAX_PATH] = {};
  wcscpy_s(dxgiPath, exePath);
  wcscat_s(dxgiPath, L"dxgi.dll");
  wcscpy_s(d3d11Path, exePath);
  wcscat_s(d3d11Path, L"d3d11.dll");

  gDx11BridgeTrexDxgiV212 = LoadLibraryW(dxgiPath);
  if (gDx11BridgeTrexDxgiV212) {
    Logger::info("DX11_V212_SERVER_REMIX_INITIALIZER: loaded .trex dxgi.dll into NvRemixBridge.exe.");
    FARPROC f0 = GetProcAddress(gDx11BridgeTrexDxgiV212, "CreateDXGIFactory");
    FARPROC f1 = GetProcAddress(gDx11BridgeTrexDxgiV212, "CreateDXGIFactory1");
    FARPROC f2 = GetProcAddress(gDx11BridgeTrexDxgiV212, "CreateDXGIFactory2");
    if (f0 || f1 || f2) {
      Logger::info("DX11_V212_SERVER_REMIX_INITIALIZER: .trex dxgi.dll exports DXGI factory entrypoints.");
    } else {
      Logger::warn("DX11_V212_SERVER_REMIX_INITIALIZER: .trex dxgi.dll loaded but no DXGI factory exports were found.");
    }
  } else {
    Logger::warn("DX11_V212_SERVER_REMIX_INITIALIZER: failed to load .trex dxgi.dll into NvRemixBridge.exe.");
  }

  gDx11BridgeTrexD3D11V212 = LoadLibraryW(d3d11Path);
  if (gDx11BridgeTrexD3D11V212) {
    Logger::info("DX11_V212_SERVER_REMIX_INITIALIZER: loaded .trex d3d11.dll into NvRemixBridge.exe.");
    FARPROC d0 = GetProcAddress(gDx11BridgeTrexD3D11V212, "D3D11CreateDevice");
    FARPROC d1 = GetProcAddress(gDx11BridgeTrexD3D11V212, "D3D11CreateDeviceAndSwapChain");
    if (d0 || d1) {
      Logger::info("DX11_V212_SERVER_REMIX_INITIALIZER: .trex d3d11.dll exports D3D11 device entrypoints.");
    } else {
      Logger::warn("DX11_V212_SERVER_REMIX_INITIALIZER: .trex d3d11.dll loaded but no D3D11 entrypoints were found.");
    }
  } else {
    Logger::warn("DX11_V212_SERVER_REMIX_INITIALIZER: failed to load .trex d3d11.dll into NvRemixBridge.exe.");
  }
}
#endif

#ifndef DX11_V212_FORCE_TREX_DX11_RUNTIME
#define DX11_V212_FORCE_TREX_DX11_RUNTIME
static HMODULE gDx11BridgeForcedTrexDxgiV212 = nullptr;
static HMODULE gDx11BridgeForcedTrexD3D11V212 = nullptr;

static bool Dx11BridgeForceTrexRuntimePathExistsV212(const wchar_t* path) {
  const DWORD attrs = GetFileAttributesW(path);
  return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
}

static bool Dx11BridgeForceGetTrexDirV212(wchar_t* out, DWORD cap) {
  if (!out || cap == 0) return false;
  out[0] = 0;

  wchar_t envTrex[MAX_PATH] = {};
  DWORD got = GetEnvironmentVariableW(L"DXVK_REMIX_TREX_DIR", envTrex, _countof(envTrex));
  if (got > 0 && got < _countof(envTrex)) {
    lstrcpynW(out, envTrex, cap);
  } else {
    wchar_t exePath[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, exePath, _countof(exePath)) == 0) {
      return false;
    }
    wchar_t* slash = wcsrchr(exePath, L'\\');
    if (!slash) {
      return false;
    }
    slash[1] = 0;
    lstrcpynW(out, exePath, cap);
  }

  size_t len = wcslen(out);
  if (len > 0 && out[len - 1] != L'\\' && len + 2 < cap) {
    out[len] = L'\\';
    out[len + 1] = 0;
  }

  return out[0] != 0;
}

static bool Dx11BridgeServerForceLoadTrexDx11RuntimeV212() {
  wchar_t mode[64] = {};
  if (GetEnvironmentVariableW(L"DX11_BRIDGE_MODE", mode, _countof(mode)) == 0) {
    return true;
  }

  wchar_t trexDir[MAX_PATH] = {};
  if (!Dx11BridgeForceGetTrexDirV212(trexDir, _countof(trexDir))) {
    Logger::err("DX11_V212_FORCE_TREX_DX11_RUNTIME: FAILED: could not resolve .trex directory.");
    return false;
  }

  SetDllDirectoryW(trexDir);

  wchar_t dxgiPath[MAX_PATH] = {};
  wchar_t d3d11Path[MAX_PATH] = {};
  wcscpy_s(dxgiPath, trexDir);
  wcscat_s(dxgiPath, L"dxgi.dll");
  wcscpy_s(d3d11Path, trexDir);
  wcscat_s(d3d11Path, L"d3d11.dll");

  if (!Dx11BridgeForceTrexRuntimePathExistsV212(dxgiPath)) {
    Logger::err("DX11_V212_FORCE_TREX_DX11_RUNTIME: FAILED: .trex\\dxgi.dll is missing.");
    return false;
  }

  if (!Dx11BridgeForceTrexRuntimePathExistsV212(d3d11Path)) {
    Logger::err("DX11_V212_FORCE_TREX_DX11_RUNTIME: FAILED: .trex\\d3d11.dll is missing.");
    return false;
  }

  gDx11BridgeForcedTrexDxgiV212 = LoadLibraryExW(dxgiPath, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
  if (!gDx11BridgeForcedTrexDxgiV212) {
    Logger::err("DX11_V212_FORCE_TREX_DX11_RUNTIME: FAILED: LoadLibraryExW(.trex\\dxgi.dll) failed.");
    return false;
  }

  Logger::info("DX11_V212_FORCE_TREX_DX11_RUNTIME: forced .trex\\dxgi.dll loaded into NvRemixBridge.exe.");

  gDx11BridgeForcedTrexD3D11V212 = LoadLibraryExW(d3d11Path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
  if (!gDx11BridgeForcedTrexD3D11V212) {
    Logger::err("DX11_V212_FORCE_TREX_DX11_RUNTIME: FAILED: LoadLibraryExW(.trex\\d3d11.dll) failed.");
    return false;
  }

  Logger::info("DX11_V212_FORCE_TREX_DX11_RUNTIME: forced .trex\\d3d11.dll loaded into NvRemixBridge.exe.");

  FARPROC createFactory = GetProcAddress(gDx11BridgeForcedTrexDxgiV212, "CreateDXGIFactory");
  FARPROC createFactory1 = GetProcAddress(gDx11BridgeForcedTrexDxgiV212, "CreateDXGIFactory1");
  FARPROC createFactory2 = GetProcAddress(gDx11BridgeForcedTrexDxgiV212, "CreateDXGIFactory2");
  FARPROC createDevice = GetProcAddress(gDx11BridgeForcedTrexD3D11V212, "D3D11CreateDevice");
  FARPROC createDeviceAndSwapChain = GetProcAddress(gDx11BridgeForcedTrexD3D11V212, "D3D11CreateDeviceAndSwapChain");

  if (!createDevice) {
    Logger::err("DX11_V212_FORCE_TREX_DX11_RUNTIME: FAILED: forced .trex\\d3d11.dll lacks D3D11CreateDevice.");
    return false;
  }

  Logger::info("DX11_V212_FORCE_TREX_DX11_RUNTIME: .trex\\d3d11.dll exports D3D11CreateDevice.");

  if (createDeviceAndSwapChain) {
    Logger::info("DX11_V212_FORCE_TREX_DX11_RUNTIME: .trex\\d3d11.dll exports D3D11CreateDeviceAndSwapChain.");
  } else {
    Logger::warn("DX11_V212_FORCE_TREX_DX11_RUNTIME: .trex\\d3d11.dll lacks D3D11CreateDeviceAndSwapChain.");
  }

  if (createFactory || createFactory1 || createFactory2) {
    Logger::info("DX11_V212_FORCE_TREX_DX11_RUNTIME: .trex\\dxgi.dll exports DXGI factory entrypoints.");
  } else {
    Logger::warn("DX11_V212_FORCE_TREX_DX11_RUNTIME: .trex\\dxgi.dll lacks DXGI factory exports.");
  }

  SetEnvironmentVariableW(L"DX11_BRIDGE_TREX_D3D11_FORCED", L"1");
  SetEnvironmentVariableW(L"DX11_BRIDGE_TREX_DXGI_FORCED", L"1");
  return true;
}
#endif

#ifndef DX11_V213_SERVER_REMIX_INITIALIZER
#define DX11_V213_SERVER_REMIX_INITIALIZER
static HMODULE gDx11BridgeTrexDxgiV213 = nullptr;
static HMODULE gDx11BridgeTrexD3D11V213 = nullptr;

static void Dx11BridgeServerInitializeTrexRemixRuntimeV213() {
  wchar_t mode[64] = {};
  GetEnvironmentVariableW(L"DX11_BRIDGE_MODE", mode, _countof(mode));

  wchar_t exePath[MAX_PATH] = {};
  if (GetModuleFileNameW(nullptr, exePath, _countof(exePath)) == 0) {
    Logger::warn("DX11_V213_SERVER_REMIX_INITIALIZER: could not resolve NvRemixBridge.exe path.");
    return;
  }

  wchar_t* slash = wcsrchr(exePath, L'\\');
  if (!slash) {
    Logger::warn("DX11_V213_SERVER_REMIX_INITIALIZER: could not resolve .trex folder.");
    return;
  }
  slash[1] = 0;

  SetDllDirectoryW(exePath);
  SetEnvironmentVariableW(L"DXVK_REMIX_TREX_DIR", exePath);
  SetEnvironmentVariableW(L"DXVK_REMIX_BRIDGE_SERVER_HOSTS_RUNTIME", L"1");

  wchar_t dxgiPath[MAX_PATH] = {};
  wchar_t d3d11Path[MAX_PATH] = {};
  wcscpy_s(dxgiPath, exePath);
  wcscat_s(dxgiPath, L"dxgi.dll");
  wcscpy_s(d3d11Path, exePath);
  wcscat_s(d3d11Path, L"d3d11.dll");

  gDx11BridgeTrexDxgiV213 = LoadLibraryW(dxgiPath);
  if (gDx11BridgeTrexDxgiV213) {
    Logger::info("DX11_V213_SERVER_REMIX_INITIALIZER: loaded .trex dxgi.dll into NvRemixBridge.exe.");
    FARPROC f0 = GetProcAddress(gDx11BridgeTrexDxgiV213, "CreateDXGIFactory");
    FARPROC f1 = GetProcAddress(gDx11BridgeTrexDxgiV213, "CreateDXGIFactory1");
    FARPROC f2 = GetProcAddress(gDx11BridgeTrexDxgiV213, "CreateDXGIFactory2");
    if (f0 || f1 || f2) {
      Logger::info("DX11_V213_SERVER_REMIX_INITIALIZER: .trex dxgi.dll exports DXGI factory entrypoints.");
    } else {
      Logger::warn("DX11_V213_SERVER_REMIX_INITIALIZER: .trex dxgi.dll loaded but no DXGI factory exports were found.");
    }
  } else {
    Logger::warn("DX11_V213_SERVER_REMIX_INITIALIZER: failed to load .trex dxgi.dll into NvRemixBridge.exe.");
  }

  gDx11BridgeTrexD3D11V213 = LoadLibraryW(d3d11Path);
  if (gDx11BridgeTrexD3D11V213) {
    Logger::info("DX11_V213_SERVER_REMIX_INITIALIZER: loaded .trex d3d11.dll into NvRemixBridge.exe.");
    FARPROC d0 = GetProcAddress(gDx11BridgeTrexD3D11V213, "D3D11CreateDevice");
    FARPROC d1 = GetProcAddress(gDx11BridgeTrexD3D11V213, "D3D11CreateDeviceAndSwapChain");
    if (d0 || d1) {
      Logger::info("DX11_V213_SERVER_REMIX_INITIALIZER: .trex d3d11.dll exports D3D11 device entrypoints.");
    } else {
      Logger::warn("DX11_V213_SERVER_REMIX_INITIALIZER: .trex d3d11.dll loaded but no D3D11 entrypoints were found.");
    }
  } else {
    Logger::warn("DX11_V213_SERVER_REMIX_INITIALIZER: failed to load .trex d3d11.dll into NvRemixBridge.exe.");
  }
}
#endif

#ifndef DX11_V213_FORCE_TREX_DX11_RUNTIME
#define DX11_V213_FORCE_TREX_DX11_RUNTIME
static HMODULE gDx11BridgeForcedTrexDxgiV213 = nullptr;
static HMODULE gDx11BridgeForcedTrexD3D11V213 = nullptr;

static bool Dx11BridgeForceTrexRuntimePathExistsV213(const wchar_t* path) {
  const DWORD attrs = GetFileAttributesW(path);
  return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
}

static bool Dx11BridgeForceGetTrexDirV213(wchar_t* out, DWORD cap) {
  if (!out || cap == 0) return false;
  out[0] = 0;

  wchar_t envTrex[MAX_PATH] = {};
  DWORD got = GetEnvironmentVariableW(L"DXVK_REMIX_TREX_DIR", envTrex, _countof(envTrex));
  if (got > 0 && got < _countof(envTrex)) {
    lstrcpynW(out, envTrex, cap);
  } else {
    wchar_t exePath[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, exePath, _countof(exePath)) == 0) {
      return false;
    }
    wchar_t* slash = wcsrchr(exePath, L'\\');
    if (!slash) {
      return false;
    }
    slash[1] = 0;
    lstrcpynW(out, exePath, cap);
  }

  size_t len = wcslen(out);
  if (len > 0 && out[len - 1] != L'\\' && len + 2 < cap) {
    out[len] = L'\\';
    out[len + 1] = 0;
  }

  return out[0] != 0;
}

static bool Dx11BridgeServerForceLoadTrexDx11RuntimeV213() {
  wchar_t mode[64] = {};
  if (GetEnvironmentVariableW(L"DX11_BRIDGE_MODE", mode, _countof(mode)) == 0) {
    return true;
  }

  wchar_t trexDir[MAX_PATH] = {};
  if (!Dx11BridgeForceGetTrexDirV213(trexDir, _countof(trexDir))) {
    Logger::err("DX11_V213_FORCE_TREX_DX11_RUNTIME: FAILED: could not resolve .trex directory.");
    return false;
  }

  SetDllDirectoryW(trexDir);

  wchar_t dxgiPath[MAX_PATH] = {};
  wchar_t d3d11Path[MAX_PATH] = {};
  wcscpy_s(dxgiPath, trexDir);
  wcscat_s(dxgiPath, L"dxgi.dll");
  wcscpy_s(d3d11Path, trexDir);
  wcscat_s(d3d11Path, L"d3d11.dll");

  if (!Dx11BridgeForceTrexRuntimePathExistsV213(dxgiPath)) {
    Logger::err("DX11_V213_FORCE_TREX_DX11_RUNTIME: FAILED: .trex\\dxgi.dll is missing.");
    return false;
  }

  if (!Dx11BridgeForceTrexRuntimePathExistsV213(d3d11Path)) {
    Logger::err("DX11_V213_FORCE_TREX_DX11_RUNTIME: FAILED: .trex\\d3d11.dll is missing.");
    return false;
  }

  gDx11BridgeForcedTrexDxgiV213 = LoadLibraryExW(dxgiPath, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
  if (!gDx11BridgeForcedTrexDxgiV213) {
    Logger::err("DX11_V213_FORCE_TREX_DX11_RUNTIME: FAILED: LoadLibraryExW(.trex\\dxgi.dll) failed.");
    return false;
  }

  Logger::info("DX11_V213_FORCE_TREX_DX11_RUNTIME: forced .trex\\dxgi.dll loaded into NvRemixBridge.exe.");

  gDx11BridgeForcedTrexD3D11V213 = LoadLibraryExW(d3d11Path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
  if (!gDx11BridgeForcedTrexD3D11V213) {
    Logger::err("DX11_V213_FORCE_TREX_DX11_RUNTIME: FAILED: LoadLibraryExW(.trex\\d3d11.dll) failed.");
    return false;
  }

  Logger::info("DX11_V213_FORCE_TREX_DX11_RUNTIME: forced .trex\\d3d11.dll loaded into NvRemixBridge.exe.");

  FARPROC createFactory = GetProcAddress(gDx11BridgeForcedTrexDxgiV213, "CreateDXGIFactory");
  FARPROC createFactory1 = GetProcAddress(gDx11BridgeForcedTrexDxgiV213, "CreateDXGIFactory1");
  FARPROC createFactory2 = GetProcAddress(gDx11BridgeForcedTrexDxgiV213, "CreateDXGIFactory2");
  FARPROC createDevice = GetProcAddress(gDx11BridgeForcedTrexD3D11V213, "D3D11CreateDevice");
  FARPROC createDeviceAndSwapChain = GetProcAddress(gDx11BridgeForcedTrexD3D11V213, "D3D11CreateDeviceAndSwapChain");

  if (!createDevice) {
    Logger::err("DX11_V213_FORCE_TREX_DX11_RUNTIME: FAILED: forced .trex\\d3d11.dll lacks D3D11CreateDevice.");
    return false;
  }

  Logger::info("DX11_V213_FORCE_TREX_DX11_RUNTIME: .trex\\d3d11.dll exports D3D11CreateDevice.");

  if (createDeviceAndSwapChain) {
    Logger::info("DX11_V213_FORCE_TREX_DX11_RUNTIME: .trex\\d3d11.dll exports D3D11CreateDeviceAndSwapChain.");
  } else {
    Logger::warn("DX11_V213_FORCE_TREX_DX11_RUNTIME: .trex\\d3d11.dll lacks D3D11CreateDeviceAndSwapChain.");
  }

  if (createFactory || createFactory1 || createFactory2) {
    Logger::info("DX11_V213_FORCE_TREX_DX11_RUNTIME: .trex\\dxgi.dll exports DXGI factory entrypoints.");
  } else {
    Logger::warn("DX11_V213_FORCE_TREX_DX11_RUNTIME: .trex\\dxgi.dll lacks DXGI factory exports.");
  }

  SetEnvironmentVariableW(L"DX11_BRIDGE_TREX_D3D11_FORCED", L"1");
  SetEnvironmentVariableW(L"DX11_BRIDGE_TREX_DXGI_FORCED", L"1");
  return true;
}
#endif

#ifndef DX11_V214_SERVER_REMIX_INITIALIZER
#define DX11_V214_SERVER_REMIX_INITIALIZER
static HMODULE gDx11BridgeTrexDxgiV214 = nullptr;
static HMODULE gDx11BridgeTrexD3D11V214 = nullptr;

static void Dx11BridgeServerInitializeTrexRemixRuntimeV214() {
  wchar_t mode[64] = {};
  GetEnvironmentVariableW(L"DX11_BRIDGE_MODE", mode, _countof(mode));

  wchar_t exePath[MAX_PATH] = {};
  if (GetModuleFileNameW(nullptr, exePath, _countof(exePath)) == 0) {
    Logger::warn("DX11_V214_SERVER_REMIX_INITIALIZER: could not resolve NvRemixBridge.exe path.");
    return;
  }

  wchar_t* slash = wcsrchr(exePath, L'\\');
  if (!slash) {
    Logger::warn("DX11_V214_SERVER_REMIX_INITIALIZER: could not resolve .trex folder.");
    return;
  }
  slash[1] = 0;

  SetDllDirectoryW(exePath);
  SetEnvironmentVariableW(L"DXVK_REMIX_TREX_DIR", exePath);
  SetEnvironmentVariableW(L"DXVK_REMIX_BRIDGE_SERVER_HOSTS_RUNTIME", L"1");

  wchar_t dxgiPath[MAX_PATH] = {};
  wchar_t d3d11Path[MAX_PATH] = {};
  wcscpy_s(dxgiPath, exePath);
  wcscat_s(dxgiPath, L"dxgi.dll");
  wcscpy_s(d3d11Path, exePath);
  wcscat_s(d3d11Path, L"d3d11.dll");

  gDx11BridgeTrexDxgiV214 = LoadLibraryW(dxgiPath);
  if (gDx11BridgeTrexDxgiV214) {
    Logger::info("DX11_V214_SERVER_REMIX_INITIALIZER: loaded .trex dxgi.dll into NvRemixBridge.exe.");
    FARPROC f0 = GetProcAddress(gDx11BridgeTrexDxgiV214, "CreateDXGIFactory");
    FARPROC f1 = GetProcAddress(gDx11BridgeTrexDxgiV214, "CreateDXGIFactory1");
    FARPROC f2 = GetProcAddress(gDx11BridgeTrexDxgiV214, "CreateDXGIFactory2");
    if (f0 || f1 || f2) {
      Logger::info("DX11_V214_SERVER_REMIX_INITIALIZER: .trex dxgi.dll exports DXGI factory entrypoints.");
    } else {
      Logger::warn("DX11_V214_SERVER_REMIX_INITIALIZER: .trex dxgi.dll loaded but no DXGI factory exports were found.");
    }
  } else {
    Logger::warn("DX11_V214_SERVER_REMIX_INITIALIZER: failed to load .trex dxgi.dll into NvRemixBridge.exe.");
  }

  gDx11BridgeTrexD3D11V214 = LoadLibraryW(d3d11Path);
  if (gDx11BridgeTrexD3D11V214) {
    Logger::info("DX11_V214_SERVER_REMIX_INITIALIZER: loaded .trex d3d11.dll into NvRemixBridge.exe.");
    FARPROC d0 = GetProcAddress(gDx11BridgeTrexD3D11V214, "D3D11CreateDevice");
    FARPROC d1 = GetProcAddress(gDx11BridgeTrexD3D11V214, "D3D11CreateDeviceAndSwapChain");
    if (d0 || d1) {
      Logger::info("DX11_V214_SERVER_REMIX_INITIALIZER: .trex d3d11.dll exports D3D11 device entrypoints.");
    } else {
      Logger::warn("DX11_V214_SERVER_REMIX_INITIALIZER: .trex d3d11.dll loaded but no D3D11 entrypoints were found.");
    }
  } else {
    Logger::warn("DX11_V214_SERVER_REMIX_INITIALIZER: failed to load .trex d3d11.dll into NvRemixBridge.exe.");
  }
}
#endif

#ifndef DX11_V214_FORCE_TREX_DX11_RUNTIME
#define DX11_V214_FORCE_TREX_DX11_RUNTIME
static HMODULE gDx11BridgeForcedTrexDxgiV214 = nullptr;
static HMODULE gDx11BridgeForcedTrexD3D11V214 = nullptr;

static bool Dx11BridgeForceTrexRuntimePathExistsV214(const wchar_t* path) {
  const DWORD attrs = GetFileAttributesW(path);
  return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
}

static bool Dx11BridgeForceGetTrexDirV214(wchar_t* out, DWORD cap) {
  if (!out || cap == 0) return false;
  out[0] = 0;

  wchar_t envTrex[MAX_PATH] = {};
  DWORD got = GetEnvironmentVariableW(L"DXVK_REMIX_TREX_DIR", envTrex, _countof(envTrex));
  if (got > 0 && got < _countof(envTrex)) {
    lstrcpynW(out, envTrex, cap);
  } else {
    wchar_t exePath[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, exePath, _countof(exePath)) == 0) {
      return false;
    }
    wchar_t* slash = wcsrchr(exePath, L'\\');
    if (!slash) {
      return false;
    }
    slash[1] = 0;
    lstrcpynW(out, exePath, cap);
  }

  size_t len = wcslen(out);
  if (len > 0 && out[len - 1] != L'\\' && len + 2 < cap) {
    out[len] = L'\\';
    out[len + 1] = 0;
  }

  return out[0] != 0;
}

static bool Dx11BridgeServerForceLoadTrexDx11RuntimeV214() {
  wchar_t mode[64] = {};
  if (GetEnvironmentVariableW(L"DX11_BRIDGE_MODE", mode, _countof(mode)) == 0) {
    return true;
  }

  wchar_t trexDir[MAX_PATH] = {};
  if (!Dx11BridgeForceGetTrexDirV214(trexDir, _countof(trexDir))) {
    Logger::err("DX11_V214_FORCE_TREX_DX11_RUNTIME: FAILED: could not resolve .trex directory.");
    return false;
  }

  SetDllDirectoryW(trexDir);

  wchar_t dxgiPath[MAX_PATH] = {};
  wchar_t d3d11Path[MAX_PATH] = {};
  wcscpy_s(dxgiPath, trexDir);
  wcscat_s(dxgiPath, L"dxgi.dll");
  wcscpy_s(d3d11Path, trexDir);
  wcscat_s(d3d11Path, L"d3d11.dll");

  if (!Dx11BridgeForceTrexRuntimePathExistsV214(dxgiPath)) {
    Logger::err("DX11_V214_FORCE_TREX_DX11_RUNTIME: FAILED: .trex\\dxgi.dll is missing.");
    return false;
  }

  if (!Dx11BridgeForceTrexRuntimePathExistsV214(d3d11Path)) {
    Logger::err("DX11_V214_FORCE_TREX_DX11_RUNTIME: FAILED: .trex\\d3d11.dll is missing.");
    return false;
  }

  gDx11BridgeForcedTrexDxgiV214 = LoadLibraryExW(dxgiPath, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
  if (!gDx11BridgeForcedTrexDxgiV214) {
    Logger::err("DX11_V214_FORCE_TREX_DX11_RUNTIME: FAILED: LoadLibraryExW(.trex\\dxgi.dll) failed.");
    return false;
  }

  Logger::info("DX11_V214_FORCE_TREX_DX11_RUNTIME: forced .trex\\dxgi.dll loaded into NvRemixBridge.exe.");

  gDx11BridgeForcedTrexD3D11V214 = LoadLibraryExW(d3d11Path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
  if (!gDx11BridgeForcedTrexD3D11V214) {
    Logger::err("DX11_V214_FORCE_TREX_DX11_RUNTIME: FAILED: LoadLibraryExW(.trex\\d3d11.dll) failed.");
    return false;
  }

  Logger::info("DX11_V214_FORCE_TREX_DX11_RUNTIME: forced .trex\\d3d11.dll loaded into NvRemixBridge.exe.");

  FARPROC createFactory = GetProcAddress(gDx11BridgeForcedTrexDxgiV214, "CreateDXGIFactory");
  FARPROC createFactory1 = GetProcAddress(gDx11BridgeForcedTrexDxgiV214, "CreateDXGIFactory1");
  FARPROC createFactory2 = GetProcAddress(gDx11BridgeForcedTrexDxgiV214, "CreateDXGIFactory2");
  FARPROC createDevice = GetProcAddress(gDx11BridgeForcedTrexD3D11V214, "D3D11CreateDevice");
  FARPROC createDeviceAndSwapChain = GetProcAddress(gDx11BridgeForcedTrexD3D11V214, "D3D11CreateDeviceAndSwapChain");

  if (!createDevice) {
    Logger::err("DX11_V214_FORCE_TREX_DX11_RUNTIME: FAILED: forced .trex\\d3d11.dll lacks D3D11CreateDevice.");
    return false;
  }

  Logger::info("DX11_V214_FORCE_TREX_DX11_RUNTIME: .trex\\d3d11.dll exports D3D11CreateDevice.");

  if (createDeviceAndSwapChain) {
    Logger::info("DX11_V214_FORCE_TREX_DX11_RUNTIME: .trex\\d3d11.dll exports D3D11CreateDeviceAndSwapChain.");
  } else {
    Logger::warn("DX11_V214_FORCE_TREX_DX11_RUNTIME: .trex\\d3d11.dll lacks D3D11CreateDeviceAndSwapChain.");
  }

  if (createFactory || createFactory1 || createFactory2) {
    Logger::info("DX11_V214_FORCE_TREX_DX11_RUNTIME: .trex\\dxgi.dll exports DXGI factory entrypoints.");
  } else {
    Logger::warn("DX11_V214_FORCE_TREX_DX11_RUNTIME: .trex\\dxgi.dll lacks DXGI factory exports.");
  }

  SetEnvironmentVariableW(L"DX11_BRIDGE_TREX_D3D11_FORCED", L"1");
  SetEnvironmentVariableW(L"DX11_BRIDGE_TREX_DXGI_FORCED", L"1");
  return true;
}
#endif

#ifndef DX11_V215_SERVER_REMIX_INITIALIZER
#define DX11_V215_SERVER_REMIX_INITIALIZER
static HMODULE gDx11BridgeTrexDxgiV215 = nullptr;
static HMODULE gDx11BridgeTrexD3D11V215 = nullptr;

static void Dx11BridgeServerInitializeTrexRemixRuntimeV215() {
  wchar_t mode[64] = {};
  GetEnvironmentVariableW(L"DX11_BRIDGE_MODE", mode, _countof(mode));

  wchar_t exePath[MAX_PATH] = {};
  if (GetModuleFileNameW(nullptr, exePath, _countof(exePath)) == 0) {
    Logger::warn("DX11_V215_SERVER_REMIX_INITIALIZER: could not resolve NvRemixBridge.exe path.");
    return;
  }

  wchar_t* slash = wcsrchr(exePath, L'\\');
  if (!slash) {
    Logger::warn("DX11_V215_SERVER_REMIX_INITIALIZER: could not resolve .trex folder.");
    return;
  }
  slash[1] = 0;

  SetDllDirectoryW(exePath);
  SetEnvironmentVariableW(L"DXVK_REMIX_TREX_DIR", exePath);
  SetEnvironmentVariableW(L"DXVK_REMIX_BRIDGE_SERVER_HOSTS_RUNTIME", L"1");

  wchar_t dxgiPath[MAX_PATH] = {};
  wchar_t d3d11Path[MAX_PATH] = {};
  wcscpy_s(dxgiPath, exePath);
  wcscat_s(dxgiPath, L"dxgi.dll");
  wcscpy_s(d3d11Path, exePath);
  wcscat_s(d3d11Path, L"d3d11.dll");

  gDx11BridgeTrexDxgiV215 = LoadLibraryW(dxgiPath);
  if (gDx11BridgeTrexDxgiV215) {
    Logger::info("DX11_V215_SERVER_REMIX_INITIALIZER: loaded .trex dxgi.dll into NvRemixBridge.exe.");
    FARPROC f0 = GetProcAddress(gDx11BridgeTrexDxgiV215, "CreateDXGIFactory");
    FARPROC f1 = GetProcAddress(gDx11BridgeTrexDxgiV215, "CreateDXGIFactory1");
    FARPROC f2 = GetProcAddress(gDx11BridgeTrexDxgiV215, "CreateDXGIFactory2");
    if (f0 || f1 || f2) {
      Logger::info("DX11_V215_SERVER_REMIX_INITIALIZER: .trex dxgi.dll exports DXGI factory entrypoints.");
    } else {
      Logger::warn("DX11_V215_SERVER_REMIX_INITIALIZER: .trex dxgi.dll loaded but no DXGI factory exports were found.");
    }
  } else {
    Logger::warn("DX11_V215_SERVER_REMIX_INITIALIZER: failed to load .trex dxgi.dll into NvRemixBridge.exe.");
  }

  gDx11BridgeTrexD3D11V215 = LoadLibraryW(d3d11Path);
  if (gDx11BridgeTrexD3D11V215) {
    Logger::info("DX11_V215_SERVER_REMIX_INITIALIZER: loaded .trex d3d11.dll into NvRemixBridge.exe.");
    FARPROC d0 = GetProcAddress(gDx11BridgeTrexD3D11V215, "D3D11CreateDevice");
    FARPROC d1 = GetProcAddress(gDx11BridgeTrexD3D11V215, "D3D11CreateDeviceAndSwapChain");
    if (d0 || d1) {
      Logger::info("DX11_V215_SERVER_REMIX_INITIALIZER: .trex d3d11.dll exports D3D11 device entrypoints.");
    } else {
      Logger::warn("DX11_V215_SERVER_REMIX_INITIALIZER: .trex d3d11.dll loaded but no D3D11 entrypoints were found.");
    }
  } else {
    Logger::warn("DX11_V215_SERVER_REMIX_INITIALIZER: failed to load .trex d3d11.dll into NvRemixBridge.exe.");
  }
}
#endif

#ifndef DX11_V215_FORCE_TREX_DX11_RUNTIME
#define DX11_V215_FORCE_TREX_DX11_RUNTIME
static HMODULE gDx11BridgeForcedTrexDxgiV215 = nullptr;
static HMODULE gDx11BridgeForcedTrexD3D11V215 = nullptr;

static bool Dx11BridgeForceTrexRuntimePathExistsV215(const wchar_t* path) {
  const DWORD attrs = GetFileAttributesW(path);
  return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
}

static bool Dx11BridgeForceGetTrexDirV215(wchar_t* out, DWORD cap) {
  if (!out || cap == 0) return false;
  out[0] = 0;

  wchar_t envTrex[MAX_PATH] = {};
  DWORD got = GetEnvironmentVariableW(L"DXVK_REMIX_TREX_DIR", envTrex, _countof(envTrex));
  if (got > 0 && got < _countof(envTrex)) {
    lstrcpynW(out, envTrex, cap);
  } else {
    wchar_t exePath[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, exePath, _countof(exePath)) == 0) {
      return false;
    }
    wchar_t* slash = wcsrchr(exePath, L'\\');
    if (!slash) {
      return false;
    }
    slash[1] = 0;
    lstrcpynW(out, exePath, cap);
  }

  size_t len = wcslen(out);
  if (len > 0 && out[len - 1] != L'\\' && len + 2 < cap) {
    out[len] = L'\\';
    out[len + 1] = 0;
  }

  return out[0] != 0;
}

static bool Dx11BridgeServerForceLoadTrexDx11RuntimeV215() {
  wchar_t mode[64] = {};
  if (GetEnvironmentVariableW(L"DX11_BRIDGE_MODE", mode, _countof(mode)) == 0) {
    return true;
  }

  wchar_t trexDir[MAX_PATH] = {};
  if (!Dx11BridgeForceGetTrexDirV215(trexDir, _countof(trexDir))) {
    Logger::err("DX11_V215_FORCE_TREX_DX11_RUNTIME: FAILED: could not resolve .trex directory.");
    return false;
  }

  SetDllDirectoryW(trexDir);

  wchar_t dxgiPath[MAX_PATH] = {};
  wchar_t d3d11Path[MAX_PATH] = {};
  wcscpy_s(dxgiPath, trexDir);
  wcscat_s(dxgiPath, L"dxgi.dll");
  wcscpy_s(d3d11Path, trexDir);
  wcscat_s(d3d11Path, L"d3d11.dll");

  if (!Dx11BridgeForceTrexRuntimePathExistsV215(dxgiPath)) {
    Logger::err("DX11_V215_FORCE_TREX_DX11_RUNTIME: FAILED: .trex\\dxgi.dll is missing.");
    return false;
  }

  if (!Dx11BridgeForceTrexRuntimePathExistsV215(d3d11Path)) {
    Logger::err("DX11_V215_FORCE_TREX_DX11_RUNTIME: FAILED: .trex\\d3d11.dll is missing.");
    return false;
  }

  gDx11BridgeForcedTrexDxgiV215 = LoadLibraryExW(dxgiPath, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
  if (!gDx11BridgeForcedTrexDxgiV215) {
    Logger::err("DX11_V215_FORCE_TREX_DX11_RUNTIME: FAILED: LoadLibraryExW(.trex\\dxgi.dll) failed.");
    return false;
  }

  Logger::info("DX11_V215_FORCE_TREX_DX11_RUNTIME: forced .trex\\dxgi.dll loaded into NvRemixBridge.exe.");

  gDx11BridgeForcedTrexD3D11V215 = LoadLibraryExW(d3d11Path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
  if (!gDx11BridgeForcedTrexD3D11V215) {
    Logger::err("DX11_V215_FORCE_TREX_DX11_RUNTIME: FAILED: LoadLibraryExW(.trex\\d3d11.dll) failed.");
    return false;
  }

  Logger::info("DX11_V215_FORCE_TREX_DX11_RUNTIME: forced .trex\\d3d11.dll loaded into NvRemixBridge.exe.");

  FARPROC createFactory = GetProcAddress(gDx11BridgeForcedTrexDxgiV215, "CreateDXGIFactory");
  FARPROC createFactory1 = GetProcAddress(gDx11BridgeForcedTrexDxgiV215, "CreateDXGIFactory1");
  FARPROC createFactory2 = GetProcAddress(gDx11BridgeForcedTrexDxgiV215, "CreateDXGIFactory2");
  FARPROC createDevice = GetProcAddress(gDx11BridgeForcedTrexD3D11V215, "D3D11CreateDevice");
  FARPROC createDeviceAndSwapChain = GetProcAddress(gDx11BridgeForcedTrexD3D11V215, "D3D11CreateDeviceAndSwapChain");

  if (!createDevice) {
    Logger::err("DX11_V215_FORCE_TREX_DX11_RUNTIME: FAILED: forced .trex\\d3d11.dll lacks D3D11CreateDevice.");
    return false;
  }

  Logger::info("DX11_V215_FORCE_TREX_DX11_RUNTIME: .trex\\d3d11.dll exports D3D11CreateDevice.");

  if (createDeviceAndSwapChain) {
    Logger::info("DX11_V215_FORCE_TREX_DX11_RUNTIME: .trex\\d3d11.dll exports D3D11CreateDeviceAndSwapChain.");
  } else {
    Logger::warn("DX11_V215_FORCE_TREX_DX11_RUNTIME: .trex\\d3d11.dll lacks D3D11CreateDeviceAndSwapChain.");
  }

  if (createFactory || createFactory1 || createFactory2) {
    Logger::info("DX11_V215_FORCE_TREX_DX11_RUNTIME: .trex\\dxgi.dll exports DXGI factory entrypoints.");
  } else {
    Logger::warn("DX11_V215_FORCE_TREX_DX11_RUNTIME: .trex\\dxgi.dll lacks DXGI factory exports.");
  }

  SetEnvironmentVariableW(L"DX11_BRIDGE_TREX_D3D11_FORCED", L"1");
  SetEnvironmentVariableW(L"DX11_BRIDGE_TREX_DXGI_FORCED", L"1");
  return true;
}
#endif

#ifndef DX11_V216_SERVER_REMIX_INITIALIZER
#define DX11_V216_SERVER_REMIX_INITIALIZER
static HMODULE gDx11BridgeTrexDxgiV216 = nullptr;
static HMODULE gDx11BridgeTrexD3D11V216 = nullptr;

static void Dx11BridgeServerInitializeTrexRemixRuntimeV216() {
  wchar_t mode[64] = {};
  GetEnvironmentVariableW(L"DX11_BRIDGE_MODE", mode, _countof(mode));

  wchar_t exePath[MAX_PATH] = {};
  if (GetModuleFileNameW(nullptr, exePath, _countof(exePath)) == 0) {
    Logger::warn("DX11_V216_SERVER_REMIX_INITIALIZER: could not resolve NvRemixBridge.exe path.");
    return;
  }

  wchar_t* slash = wcsrchr(exePath, L'\\');
  if (!slash) {
    Logger::warn("DX11_V216_SERVER_REMIX_INITIALIZER: could not resolve .trex folder.");
    return;
  }
  slash[1] = 0;

  SetDllDirectoryW(exePath);
  SetEnvironmentVariableW(L"DXVK_REMIX_TREX_DIR", exePath);
  SetEnvironmentVariableW(L"DXVK_REMIX_BRIDGE_SERVER_HOSTS_RUNTIME", L"1");

  wchar_t dxgiPath[MAX_PATH] = {};
  wchar_t d3d11Path[MAX_PATH] = {};
  wcscpy_s(dxgiPath, exePath);
  wcscat_s(dxgiPath, L"dxgi.dll");
  wcscpy_s(d3d11Path, exePath);
  wcscat_s(d3d11Path, L"d3d11.dll");

  gDx11BridgeTrexDxgiV216 = LoadLibraryW(dxgiPath);
  if (gDx11BridgeTrexDxgiV216) {
    Logger::info("DX11_V216_SERVER_REMIX_INITIALIZER: loaded .trex dxgi.dll into NvRemixBridge.exe.");
    FARPROC f0 = GetProcAddress(gDx11BridgeTrexDxgiV216, "CreateDXGIFactory");
    FARPROC f1 = GetProcAddress(gDx11BridgeTrexDxgiV216, "CreateDXGIFactory1");
    FARPROC f2 = GetProcAddress(gDx11BridgeTrexDxgiV216, "CreateDXGIFactory2");
    if (f0 || f1 || f2) {
      Logger::info("DX11_V216_SERVER_REMIX_INITIALIZER: .trex dxgi.dll exports DXGI factory entrypoints.");
    } else {
      Logger::warn("DX11_V216_SERVER_REMIX_INITIALIZER: .trex dxgi.dll loaded but no DXGI factory exports were found.");
    }
  } else {
    Logger::warn("DX11_V216_SERVER_REMIX_INITIALIZER: failed to load .trex dxgi.dll into NvRemixBridge.exe.");
  }

  gDx11BridgeTrexD3D11V216 = LoadLibraryW(d3d11Path);
  if (gDx11BridgeTrexD3D11V216) {
    Logger::info("DX11_V216_SERVER_REMIX_INITIALIZER: loaded .trex d3d11.dll into NvRemixBridge.exe.");
    FARPROC d0 = GetProcAddress(gDx11BridgeTrexD3D11V216, "D3D11CreateDevice");
    FARPROC d1 = GetProcAddress(gDx11BridgeTrexD3D11V216, "D3D11CreateDeviceAndSwapChain");
    if (d0 || d1) {
      Logger::info("DX11_V216_SERVER_REMIX_INITIALIZER: .trex d3d11.dll exports D3D11 device entrypoints.");
    } else {
      Logger::warn("DX11_V216_SERVER_REMIX_INITIALIZER: .trex d3d11.dll loaded but no D3D11 entrypoints were found.");
    }
  } else {
    Logger::warn("DX11_V216_SERVER_REMIX_INITIALIZER: failed to load .trex d3d11.dll into NvRemixBridge.exe.");
  }
}
#endif

#ifndef DX11_V216_FORCE_TREX_DX11_RUNTIME
#define DX11_V216_FORCE_TREX_DX11_RUNTIME
static HMODULE gDx11BridgeForcedTrexDxgiV216 = nullptr;
static HMODULE gDx11BridgeForcedTrexD3D11V216 = nullptr;

static bool Dx11BridgeForceTrexRuntimePathExistsV216(const wchar_t* path) {
  const DWORD attrs = GetFileAttributesW(path);
  return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
}

static bool Dx11BridgeForceGetTrexDirV216(wchar_t* out, DWORD cap) {
  if (!out || cap == 0) return false;
  out[0] = 0;

  wchar_t envTrex[MAX_PATH] = {};
  DWORD got = GetEnvironmentVariableW(L"DXVK_REMIX_TREX_DIR", envTrex, _countof(envTrex));
  if (got > 0 && got < _countof(envTrex)) {
    lstrcpynW(out, envTrex, cap);
  } else {
    wchar_t exePath[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, exePath, _countof(exePath)) == 0) {
      return false;
    }
    wchar_t* slash = wcsrchr(exePath, L'\\');
    if (!slash) {
      return false;
    }
    slash[1] = 0;
    lstrcpynW(out, exePath, cap);
  }

  size_t len = wcslen(out);
  if (len > 0 && out[len - 1] != L'\\' && len + 2 < cap) {
    out[len] = L'\\';
    out[len + 1] = 0;
  }

  return out[0] != 0;
}

static bool Dx11BridgeServerForceLoadTrexDx11RuntimeV216() {
  wchar_t mode[64] = {};
  if (GetEnvironmentVariableW(L"DX11_BRIDGE_MODE", mode, _countof(mode)) == 0) {
    return true;
  }

  wchar_t trexDir[MAX_PATH] = {};
  if (!Dx11BridgeForceGetTrexDirV216(trexDir, _countof(trexDir))) {
    Logger::err("DX11_V216_FORCE_TREX_DX11_RUNTIME: FAILED: could not resolve .trex directory.");
    return false;
  }

  SetDllDirectoryW(trexDir);

  wchar_t dxgiPath[MAX_PATH] = {};
  wchar_t d3d11Path[MAX_PATH] = {};
  wcscpy_s(dxgiPath, trexDir);
  wcscat_s(dxgiPath, L"dxgi.dll");
  wcscpy_s(d3d11Path, trexDir);
  wcscat_s(d3d11Path, L"d3d11.dll");

  if (!Dx11BridgeForceTrexRuntimePathExistsV216(dxgiPath)) {
    Logger::err("DX11_V216_FORCE_TREX_DX11_RUNTIME: FAILED: .trex\\dxgi.dll is missing.");
    return false;
  }

  if (!Dx11BridgeForceTrexRuntimePathExistsV216(d3d11Path)) {
    Logger::err("DX11_V216_FORCE_TREX_DX11_RUNTIME: FAILED: .trex\\d3d11.dll is missing.");
    return false;
  }

  gDx11BridgeForcedTrexDxgiV216 = LoadLibraryExW(dxgiPath, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
  if (!gDx11BridgeForcedTrexDxgiV216) {
    Logger::err("DX11_V216_FORCE_TREX_DX11_RUNTIME: FAILED: LoadLibraryExW(.trex\\dxgi.dll) failed.");
    return false;
  }

  Logger::info("DX11_V216_FORCE_TREX_DX11_RUNTIME: forced .trex\\dxgi.dll loaded into NvRemixBridge.exe.");

  gDx11BridgeForcedTrexD3D11V216 = LoadLibraryExW(d3d11Path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
  if (!gDx11BridgeForcedTrexD3D11V216) {
    Logger::err("DX11_V216_FORCE_TREX_DX11_RUNTIME: FAILED: LoadLibraryExW(.trex\\d3d11.dll) failed.");
    return false;
  }

  Logger::info("DX11_V216_FORCE_TREX_DX11_RUNTIME: forced .trex\\d3d11.dll loaded into NvRemixBridge.exe.");

  FARPROC createFactory = GetProcAddress(gDx11BridgeForcedTrexDxgiV216, "CreateDXGIFactory");
  FARPROC createFactory1 = GetProcAddress(gDx11BridgeForcedTrexDxgiV216, "CreateDXGIFactory1");
  FARPROC createFactory2 = GetProcAddress(gDx11BridgeForcedTrexDxgiV216, "CreateDXGIFactory2");
  FARPROC createDevice = GetProcAddress(gDx11BridgeForcedTrexD3D11V216, "D3D11CreateDevice");
  FARPROC createDeviceAndSwapChain = GetProcAddress(gDx11BridgeForcedTrexD3D11V216, "D3D11CreateDeviceAndSwapChain");

  if (!createDevice) {
    Logger::err("DX11_V216_FORCE_TREX_DX11_RUNTIME: FAILED: forced .trex\\d3d11.dll lacks D3D11CreateDevice.");
    return false;
  }

  Logger::info("DX11_V216_FORCE_TREX_DX11_RUNTIME: .trex\\d3d11.dll exports D3D11CreateDevice.");

  if (createDeviceAndSwapChain) {
    Logger::info("DX11_V216_FORCE_TREX_DX11_RUNTIME: .trex\\d3d11.dll exports D3D11CreateDeviceAndSwapChain.");
  } else {
    Logger::warn("DX11_V216_FORCE_TREX_DX11_RUNTIME: .trex\\d3d11.dll lacks D3D11CreateDeviceAndSwapChain.");
  }

  if (createFactory || createFactory1 || createFactory2) {
    Logger::info("DX11_V216_FORCE_TREX_DX11_RUNTIME: .trex\\dxgi.dll exports DXGI factory entrypoints.");
  } else {
    Logger::warn("DX11_V216_FORCE_TREX_DX11_RUNTIME: .trex\\dxgi.dll lacks DXGI factory exports.");
  }

  SetEnvironmentVariableW(L"DX11_BRIDGE_TREX_D3D11_FORCED", L"1");
  SetEnvironmentVariableW(L"DX11_BRIDGE_TREX_DXGI_FORCED", L"1");
  return true;
}
#endif

#ifndef DX11_V218_SERVER_REMIX_INITIALIZER
#define DX11_V218_SERVER_REMIX_INITIALIZER
static HMODULE gDx11BridgeTrexDxgiV218 = nullptr;
static HMODULE gDx11BridgeTrexD3D11V218 = nullptr;

static void Dx11BridgeServerInitializeTrexRemixRuntimeV218() {
  wchar_t mode[64] = {};
  GetEnvironmentVariableW(L"DX11_BRIDGE_MODE", mode, _countof(mode));

  wchar_t exePath[MAX_PATH] = {};
  if (GetModuleFileNameW(nullptr, exePath, _countof(exePath)) == 0) {
    Logger::warn("DX11_V218_SERVER_REMIX_INITIALIZER: could not resolve NvRemixBridge.exe path.");
    return;
  }

  wchar_t* slash = wcsrchr(exePath, L'\\');
  if (!slash) {
    Logger::warn("DX11_V218_SERVER_REMIX_INITIALIZER: could not resolve .trex folder.");
    return;
  }
  slash[1] = 0;

  SetDllDirectoryW(exePath);
  SetEnvironmentVariableW(L"DXVK_REMIX_TREX_DIR", exePath);
  SetEnvironmentVariableW(L"DXVK_REMIX_BRIDGE_SERVER_HOSTS_RUNTIME", L"1");

  wchar_t dxgiPath[MAX_PATH] = {};
  wchar_t d3d11Path[MAX_PATH] = {};
  wcscpy_s(dxgiPath, exePath);
  wcscat_s(dxgiPath, L"dxgi.dll");
  wcscpy_s(d3d11Path, exePath);
  wcscat_s(d3d11Path, L"d3d11.dll");

  gDx11BridgeTrexDxgiV218 = LoadLibraryW(dxgiPath);
  if (gDx11BridgeTrexDxgiV218) {
    Logger::info("DX11_V218_SERVER_REMIX_INITIALIZER: loaded .trex dxgi.dll into NvRemixBridge.exe.");
    FARPROC f0 = GetProcAddress(gDx11BridgeTrexDxgiV218, "CreateDXGIFactory");
    FARPROC f1 = GetProcAddress(gDx11BridgeTrexDxgiV218, "CreateDXGIFactory1");
    FARPROC f2 = GetProcAddress(gDx11BridgeTrexDxgiV218, "CreateDXGIFactory2");
    if (f0 || f1 || f2) {
      Logger::info("DX11_V218_SERVER_REMIX_INITIALIZER: .trex dxgi.dll exports DXGI factory entrypoints.");
    } else {
      Logger::warn("DX11_V218_SERVER_REMIX_INITIALIZER: .trex dxgi.dll loaded but no DXGI factory exports were found.");
    }
  } else {
    Logger::warn("DX11_V218_SERVER_REMIX_INITIALIZER: failed to load .trex dxgi.dll into NvRemixBridge.exe.");
  }

  gDx11BridgeTrexD3D11V218 = LoadLibraryW(d3d11Path);
  if (gDx11BridgeTrexD3D11V218) {
    Logger::info("DX11_V218_SERVER_REMIX_INITIALIZER: loaded .trex d3d11.dll into NvRemixBridge.exe.");
    FARPROC d0 = GetProcAddress(gDx11BridgeTrexD3D11V218, "D3D11CreateDevice");
    FARPROC d1 = GetProcAddress(gDx11BridgeTrexD3D11V218, "D3D11CreateDeviceAndSwapChain");
    if (d0 || d1) {
      Logger::info("DX11_V218_SERVER_REMIX_INITIALIZER: .trex d3d11.dll exports D3D11 device entrypoints.");
    } else {
      Logger::warn("DX11_V218_SERVER_REMIX_INITIALIZER: .trex d3d11.dll loaded but no D3D11 entrypoints were found.");
    }
  } else {
    Logger::warn("DX11_V218_SERVER_REMIX_INITIALIZER: failed to load .trex d3d11.dll into NvRemixBridge.exe.");
  }
}
#endif

#ifndef DX11_V218_FORCE_TREX_DX11_RUNTIME
#define DX11_V218_FORCE_TREX_DX11_RUNTIME
static HMODULE gDx11BridgeForcedTrexDxgiV218 = nullptr;
static HMODULE gDx11BridgeForcedTrexD3D11V218 = nullptr;

static bool Dx11BridgeForceTrexRuntimePathExistsV218(const wchar_t* path) {
  const DWORD attrs = GetFileAttributesW(path);
  return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
}

static bool Dx11BridgeForceGetTrexDirV218(wchar_t* out, DWORD cap) {
  if (!out || cap == 0) return false;
  out[0] = 0;

  wchar_t envTrex[MAX_PATH] = {};
  DWORD got = GetEnvironmentVariableW(L"DXVK_REMIX_TREX_DIR", envTrex, _countof(envTrex));
  if (got > 0 && got < _countof(envTrex)) {
    lstrcpynW(out, envTrex, cap);
  } else {
    wchar_t exePath[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, exePath, _countof(exePath)) == 0) {
      return false;
    }
    wchar_t* slash = wcsrchr(exePath, L'\\');
    if (!slash) {
      return false;
    }
    slash[1] = 0;
    lstrcpynW(out, exePath, cap);
  }

  size_t len = wcslen(out);
  if (len > 0 && out[len - 1] != L'\\' && len + 2 < cap) {
    out[len] = L'\\';
    out[len + 1] = 0;
  }

  return out[0] != 0;
}

static bool Dx11BridgeServerForceLoadTrexDx11RuntimeV218() {
  wchar_t mode[64] = {};
  if (GetEnvironmentVariableW(L"DX11_BRIDGE_MODE", mode, _countof(mode)) == 0) {
    return true;
  }

  wchar_t trexDir[MAX_PATH] = {};
  if (!Dx11BridgeForceGetTrexDirV218(trexDir, _countof(trexDir))) {
    Logger::err("DX11_V218_FORCE_TREX_DX11_RUNTIME: FAILED: could not resolve .trex directory.");
    return false;
  }

  SetDllDirectoryW(trexDir);

  wchar_t dxgiPath[MAX_PATH] = {};
  wchar_t d3d11Path[MAX_PATH] = {};
  wcscpy_s(dxgiPath, trexDir);
  wcscat_s(dxgiPath, L"dxgi.dll");
  wcscpy_s(d3d11Path, trexDir);
  wcscat_s(d3d11Path, L"d3d11.dll");

  if (!Dx11BridgeForceTrexRuntimePathExistsV218(dxgiPath)) {
    Logger::err("DX11_V218_FORCE_TREX_DX11_RUNTIME: FAILED: .trex\\dxgi.dll is missing.");
    return false;
  }

  if (!Dx11BridgeForceTrexRuntimePathExistsV218(d3d11Path)) {
    Logger::err("DX11_V218_FORCE_TREX_DX11_RUNTIME: FAILED: .trex\\d3d11.dll is missing.");
    return false;
  }

  gDx11BridgeForcedTrexDxgiV218 = LoadLibraryExW(dxgiPath, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
  if (!gDx11BridgeForcedTrexDxgiV218) {
    Logger::err("DX11_V218_FORCE_TREX_DX11_RUNTIME: FAILED: LoadLibraryExW(.trex\\dxgi.dll) failed.");
    return false;
  }

  Logger::info("DX11_V218_FORCE_TREX_DX11_RUNTIME: forced .trex\\dxgi.dll loaded into NvRemixBridge.exe.");

  gDx11BridgeForcedTrexD3D11V218 = LoadLibraryExW(d3d11Path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
  if (!gDx11BridgeForcedTrexD3D11V218) {
    Logger::err("DX11_V218_FORCE_TREX_DX11_RUNTIME: FAILED: LoadLibraryExW(.trex\\d3d11.dll) failed.");
    return false;
  }

  Logger::info("DX11_V218_FORCE_TREX_DX11_RUNTIME: forced .trex\\d3d11.dll loaded into NvRemixBridge.exe.");

  FARPROC createFactory = GetProcAddress(gDx11BridgeForcedTrexDxgiV218, "CreateDXGIFactory");
  FARPROC createFactory1 = GetProcAddress(gDx11BridgeForcedTrexDxgiV218, "CreateDXGIFactory1");
  FARPROC createFactory2 = GetProcAddress(gDx11BridgeForcedTrexDxgiV218, "CreateDXGIFactory2");
  FARPROC createDevice = GetProcAddress(gDx11BridgeForcedTrexD3D11V218, "D3D11CreateDevice");
  FARPROC createDeviceAndSwapChain = GetProcAddress(gDx11BridgeForcedTrexD3D11V218, "D3D11CreateDeviceAndSwapChain");

  if (!createDevice) {
    Logger::err("DX11_V218_FORCE_TREX_DX11_RUNTIME: FAILED: forced .trex\\d3d11.dll lacks D3D11CreateDevice.");
    return false;
  }

  Logger::info("DX11_V218_FORCE_TREX_DX11_RUNTIME: .trex\\d3d11.dll exports D3D11CreateDevice.");

  if (createDeviceAndSwapChain) {
    Logger::info("DX11_V218_FORCE_TREX_DX11_RUNTIME: .trex\\d3d11.dll exports D3D11CreateDeviceAndSwapChain.");
  } else {
    Logger::warn("DX11_V218_FORCE_TREX_DX11_RUNTIME: .trex\\d3d11.dll lacks D3D11CreateDeviceAndSwapChain.");
  }

  if (createFactory || createFactory1 || createFactory2) {
    Logger::info("DX11_V218_FORCE_TREX_DX11_RUNTIME: .trex\\dxgi.dll exports DXGI factory entrypoints.");
  } else {
    Logger::warn("DX11_V218_FORCE_TREX_DX11_RUNTIME: .trex\\dxgi.dll lacks DXGI factory exports.");
  }

  SetEnvironmentVariableW(L"DX11_BRIDGE_TREX_D3D11_FORCED", L"1");
  SetEnvironmentVariableW(L"DX11_BRIDGE_TREX_DXGI_FORCED", L"1");
  return true;
}
#endif

#ifndef DX11_V219_SERVER_REMIX_INITIALIZER
#define DX11_V219_SERVER_REMIX_INITIALIZER
static HMODULE gDx11BridgeTrexDxgiV219 = nullptr;
static HMODULE gDx11BridgeTrexD3D11V219 = nullptr;

static void Dx11BridgeServerInitializeTrexRemixRuntimeV219() {
  wchar_t mode[64] = {};
  GetEnvironmentVariableW(L"DX11_BRIDGE_MODE", mode, _countof(mode));

  wchar_t exePath[MAX_PATH] = {};
  if (GetModuleFileNameW(nullptr, exePath, _countof(exePath)) == 0) {
    Logger::warn("DX11_V219_SERVER_REMIX_INITIALIZER: could not resolve NvRemixBridge.exe path.");
    return;
  }

  wchar_t* slash = wcsrchr(exePath, L'\\');
  if (!slash) {
    Logger::warn("DX11_V219_SERVER_REMIX_INITIALIZER: could not resolve .trex folder.");
    return;
  }
  slash[1] = 0;

  SetDllDirectoryW(exePath);
  SetEnvironmentVariableW(L"DXVK_REMIX_TREX_DIR", exePath);
  SetEnvironmentVariableW(L"DXVK_REMIX_BRIDGE_SERVER_HOSTS_RUNTIME", L"1");

  wchar_t dxgiPath[MAX_PATH] = {};
  wchar_t d3d11Path[MAX_PATH] = {};
  wcscpy_s(dxgiPath, exePath);
  wcscat_s(dxgiPath, L"dxgi.dll");
  wcscpy_s(d3d11Path, exePath);
  wcscat_s(d3d11Path, L"d3d11.dll");

  gDx11BridgeTrexDxgiV219 = LoadLibraryW(dxgiPath);
  if (gDx11BridgeTrexDxgiV219) {
    Logger::info("DX11_V219_SERVER_REMIX_INITIALIZER: loaded .trex dxgi.dll into NvRemixBridge.exe.");
    FARPROC f0 = GetProcAddress(gDx11BridgeTrexDxgiV219, "CreateDXGIFactory");
    FARPROC f1 = GetProcAddress(gDx11BridgeTrexDxgiV219, "CreateDXGIFactory1");
    FARPROC f2 = GetProcAddress(gDx11BridgeTrexDxgiV219, "CreateDXGIFactory2");
    if (f0 || f1 || f2) {
      Logger::info("DX11_V219_SERVER_REMIX_INITIALIZER: .trex dxgi.dll exports DXGI factory entrypoints.");
    } else {
      Logger::warn("DX11_V219_SERVER_REMIX_INITIALIZER: .trex dxgi.dll loaded but no DXGI factory exports were found.");
    }
  } else {
    Logger::warn("DX11_V219_SERVER_REMIX_INITIALIZER: failed to load .trex dxgi.dll into NvRemixBridge.exe.");
  }

  gDx11BridgeTrexD3D11V219 = LoadLibraryW(d3d11Path);
  if (gDx11BridgeTrexD3D11V219) {
    Logger::info("DX11_V219_SERVER_REMIX_INITIALIZER: loaded .trex d3d11.dll into NvRemixBridge.exe.");
    FARPROC d0 = GetProcAddress(gDx11BridgeTrexD3D11V219, "D3D11CreateDevice");
    FARPROC d1 = GetProcAddress(gDx11BridgeTrexD3D11V219, "D3D11CreateDeviceAndSwapChain");
    if (d0 || d1) {
      Logger::info("DX11_V219_SERVER_REMIX_INITIALIZER: .trex d3d11.dll exports D3D11 device entrypoints.");
    } else {
      Logger::warn("DX11_V219_SERVER_REMIX_INITIALIZER: .trex d3d11.dll loaded but no D3D11 entrypoints were found.");
    }
  } else {
    Logger::warn("DX11_V219_SERVER_REMIX_INITIALIZER: failed to load .trex d3d11.dll into NvRemixBridge.exe.");
  }
}
#endif

#ifndef DX11_V219_FORCE_TREX_DX11_RUNTIME
#define DX11_V219_FORCE_TREX_DX11_RUNTIME
static HMODULE gDx11BridgeForcedTrexDxgiV219 = nullptr;
static HMODULE gDx11BridgeForcedTrexD3D11V219 = nullptr;

static bool Dx11BridgeForceTrexRuntimePathExistsV219(const wchar_t* path) {
  const DWORD attrs = GetFileAttributesW(path);
  return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
}

static bool Dx11BridgeForceGetTrexDirV219(wchar_t* out, DWORD cap) {
  if (!out || cap == 0) return false;
  out[0] = 0;

  wchar_t envTrex[MAX_PATH] = {};
  DWORD got = GetEnvironmentVariableW(L"DXVK_REMIX_TREX_DIR", envTrex, _countof(envTrex));
  if (got > 0 && got < _countof(envTrex)) {
    lstrcpynW(out, envTrex, cap);
  } else {
    wchar_t exePath[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, exePath, _countof(exePath)) == 0) {
      return false;
    }
    wchar_t* slash = wcsrchr(exePath, L'\\');
    if (!slash) {
      return false;
    }
    slash[1] = 0;
    lstrcpynW(out, exePath, cap);
  }

  size_t len = wcslen(out);
  if (len > 0 && out[len - 1] != L'\\' && len + 2 < cap) {
    out[len] = L'\\';
    out[len + 1] = 0;
  }

  return out[0] != 0;
}

static bool Dx11BridgeServerForceLoadTrexDx11RuntimeV219() {
  wchar_t mode[64] = {};
  if (GetEnvironmentVariableW(L"DX11_BRIDGE_MODE", mode, _countof(mode)) == 0) {
    return true;
  }

  wchar_t trexDir[MAX_PATH] = {};
  if (!Dx11BridgeForceGetTrexDirV219(trexDir, _countof(trexDir))) {
    Logger::err("DX11_V219_FORCE_TREX_DX11_RUNTIME: FAILED: could not resolve .trex directory.");
    return false;
  }

  SetDllDirectoryW(trexDir);

  wchar_t dxgiPath[MAX_PATH] = {};
  wchar_t d3d11Path[MAX_PATH] = {};
  wcscpy_s(dxgiPath, trexDir);
  wcscat_s(dxgiPath, L"dxgi.dll");
  wcscpy_s(d3d11Path, trexDir);
  wcscat_s(d3d11Path, L"d3d11.dll");

  if (!Dx11BridgeForceTrexRuntimePathExistsV219(dxgiPath)) {
    Logger::err("DX11_V219_FORCE_TREX_DX11_RUNTIME: FAILED: .trex\\dxgi.dll is missing.");
    return false;
  }

  if (!Dx11BridgeForceTrexRuntimePathExistsV219(d3d11Path)) {
    Logger::err("DX11_V219_FORCE_TREX_DX11_RUNTIME: FAILED: .trex\\d3d11.dll is missing.");
    return false;
  }

  gDx11BridgeForcedTrexDxgiV219 = LoadLibraryExW(dxgiPath, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
  if (!gDx11BridgeForcedTrexDxgiV219) {
    Logger::err("DX11_V219_FORCE_TREX_DX11_RUNTIME: FAILED: LoadLibraryExW(.trex\\dxgi.dll) failed.");
    return false;
  }

  Logger::info("DX11_V219_FORCE_TREX_DX11_RUNTIME: forced .trex\\dxgi.dll loaded into NvRemixBridge.exe.");

  gDx11BridgeForcedTrexD3D11V219 = LoadLibraryExW(d3d11Path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
  if (!gDx11BridgeForcedTrexD3D11V219) {
    Logger::err("DX11_V219_FORCE_TREX_DX11_RUNTIME: FAILED: LoadLibraryExW(.trex\\d3d11.dll) failed.");
    return false;
  }

  Logger::info("DX11_V219_FORCE_TREX_DX11_RUNTIME: forced .trex\\d3d11.dll loaded into NvRemixBridge.exe.");

  FARPROC createFactory = GetProcAddress(gDx11BridgeForcedTrexDxgiV219, "CreateDXGIFactory");
  FARPROC createFactory1 = GetProcAddress(gDx11BridgeForcedTrexDxgiV219, "CreateDXGIFactory1");
  FARPROC createFactory2 = GetProcAddress(gDx11BridgeForcedTrexDxgiV219, "CreateDXGIFactory2");
  FARPROC createDevice = GetProcAddress(gDx11BridgeForcedTrexD3D11V219, "D3D11CreateDevice");
  FARPROC createDeviceAndSwapChain = GetProcAddress(gDx11BridgeForcedTrexD3D11V219, "D3D11CreateDeviceAndSwapChain");

  if (!createDevice) {
    Logger::err("DX11_V219_FORCE_TREX_DX11_RUNTIME: FAILED: forced .trex\\d3d11.dll lacks D3D11CreateDevice.");
    return false;
  }

  Logger::info("DX11_V219_FORCE_TREX_DX11_RUNTIME: .trex\\d3d11.dll exports D3D11CreateDevice.");

  if (createDeviceAndSwapChain) {
    Logger::info("DX11_V219_FORCE_TREX_DX11_RUNTIME: .trex\\d3d11.dll exports D3D11CreateDeviceAndSwapChain.");
  } else {
    Logger::warn("DX11_V219_FORCE_TREX_DX11_RUNTIME: .trex\\d3d11.dll lacks D3D11CreateDeviceAndSwapChain.");
  }

  if (createFactory || createFactory1 || createFactory2) {
    Logger::info("DX11_V219_FORCE_TREX_DX11_RUNTIME: .trex\\dxgi.dll exports DXGI factory entrypoints.");
  } else {
    Logger::warn("DX11_V219_FORCE_TREX_DX11_RUNTIME: .trex\\dxgi.dll lacks DXGI factory exports.");
  }

  SetEnvironmentVariableW(L"DX11_BRIDGE_TREX_D3D11_FORCED", L"1");
  SetEnvironmentVariableW(L"DX11_BRIDGE_TREX_DXGI_FORCED", L"1");
  return true;
}
#endif
int WINAPI wWinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE hPrevInstance, _In_ PWSTR pCmdLine, _In_ int nCmdShow) {
  if (!Dx11BridgeServerForceLoadTrexDx11RuntimeV219()) {
    Logger::err("DX11_V219_FORCE_TREX_DX11_RUNTIME: server continuing after forced runtime load failure so logs remain visible.");
  }
  Dx11BridgeServerInitializeTrexRemixRuntimeV219();
  if (!Dx11BridgeServerForceLoadTrexDx11RuntimeV218()) {
    Logger::err("DX11_V218_FORCE_TREX_DX11_RUNTIME: server continuing after forced runtime load failure so logs remain visible.");
  }
  Dx11BridgeServerInitializeTrexRemixRuntimeV218();
  if (!Dx11BridgeServerForceLoadTrexDx11RuntimeV216()) {
    Logger::err("DX11_V216_FORCE_TREX_DX11_RUNTIME: server continuing after forced runtime load failure so logs remain visible.");
  }
  Dx11BridgeServerInitializeTrexRemixRuntimeV216();
  if (!Dx11BridgeServerForceLoadTrexDx11RuntimeV215()) {
    Logger::err("DX11_V215_FORCE_TREX_DX11_RUNTIME: server continuing after forced runtime load failure so logs remain visible.");
  }
  Dx11BridgeServerInitializeTrexRemixRuntimeV215();
  if (!Dx11BridgeServerForceLoadTrexDx11RuntimeV214()) {
    Logger::err("DX11_V214_FORCE_TREX_DX11_RUNTIME: server continuing after forced runtime load failure so logs remain visible.");
  }
  Dx11BridgeServerInitializeTrexRemixRuntimeV214();
  if (!Dx11BridgeServerForceLoadTrexDx11RuntimeV213()) {
    Logger::err("DX11_V213_FORCE_TREX_DX11_RUNTIME: server continuing after forced runtime load failure so logs remain visible.");
  }
  Dx11BridgeServerInitializeTrexRemixRuntimeV213();
  if (!Dx11BridgeServerForceLoadTrexDx11RuntimeV212()) {
    Logger::err("DX11_V212_FORCE_TREX_DX11_RUNTIME: server continuing after forced runtime load failure so logs remain visible.");
  }
  Dx11BridgeServerInitializeTrexRemixRuntimeV212();
  if (!Dx11BridgeServerForceLoadTrexDx11RuntimeV211()) {
    Logger::err("DX11_V211_FORCE_TREX_DX11_RUNTIME: server continuing after forced runtime load failure so logs remain visible.");
  }
  Dx11BridgeServerInitializeTrexRemixRuntimeV211();
  if (!Dx11BridgeServerForceLoadTrexDx11RuntimeV210()) {
    Logger::err("DX11_V210_FORCE_TREX_DX11_RUNTIME: server continuing after forced runtime load failure so logs remain visible.");
  }
  Dx11BridgeServerInitializeTrexRemixRuntimeV210();
  if (!Dx11BridgeServerForceLoadTrexDx11RuntimeV209()) {
    Logger::err("DX11_V209_FORCE_TREX_DX11_RUNTIME: server continuing after forced runtime load failure so logs remain visible.");
  }
  Dx11BridgeServerInitializeTrexRemixRuntimeV209();
  if (!Dx11BridgeServerForceLoadTrexDx11RuntimeV204()) {
    Logger::err("DX11_V204_FORCE_TREX_DX11_RUNTIME: server continuing after forced runtime load failure so logs remain visible.");
  }
  Dx11BridgeServerInitializeTrexRemixRuntimeV204();
  if (!Dx11BridgeServerForceLoadTrexDx11RuntimeV203()) {
    Logger::err("DX11_V203_FORCE_TREX_DX11_RUNTIME: server continuing after forced runtime load failure so logs remain visible.");
  }
  Dx11BridgeServerInitializeTrexRemixRuntimeV203();
  if (!Dx11BridgeServerForceLoadTrexDx11RuntimeV201()) {
    Logger::err("DX11_V201_FORCE_TREX_DX11_RUNTIME: server continuing after forced runtime load failure so logs remain visible.");
  }
  Dx11BridgeServerInitializeTrexRemixRuntimeV201();
  if (!Dx11BridgeServerForceLoadTrexDx11RuntimeV200()) {
    Logger::err("DX11_V200_FORCE_TREX_DX11_RUNTIME: server continuing after forced runtime load failure so logs remain visible.");
  }
  Dx11BridgeServerInitializeTrexRemixRuntimeV200();
  if (!Dx11BridgeServerForceLoadTrexDx11RuntimeV198()) {
    Logger::err("DX11_V198_FORCE_TREX_DX11_RUNTIME: server continuing after forced runtime load failure so logs remain visible.");
  }
  Dx11BridgeServerInitializeTrexRemixRuntimeV198();
  if (!Dx11BridgeServerForceLoadTrexDx11RuntimeV197()) {
    Logger::err("DX11_V197_FORCE_TREX_DX11_RUNTIME: server continuing after forced runtime load failure so logs remain visible.");
  }
  Dx11BridgeServerInitializeTrexRemixRuntimeV197();
  if (!Dx11BridgeServerForceLoadTrexDx11RuntimeV196()) {
    Logger::err("DX11_V196_FORCE_TREX_DX11_RUNTIME: server continuing after forced runtime load failure so logs remain visible.");
  }
  Dx11BridgeServerInitializeTrexRemixRuntimeV196();
  if (!Dx11BridgeServerForceLoadTrexDx11RuntimeV195()) {
    Logger::err("DX11_V195_FORCE_TREX_DX11_RUNTIME: server continuing after forced runtime load failure so logs remain visible.");
  }
  Dx11BridgeServerInitializeTrexRemixRuntimeV195();
  if (!Dx11BridgeServerForceLoadTrexDx11RuntimeV193()) {
    Logger::err("DX11_V193_FORCE_TREX_DX11_RUNTIME: server continuing after forced runtime load failure so logs remain visible.");
  }
  Dx11BridgeServerInitializeTrexRemixRuntimeV193();
  if (!Dx11BridgeServerForceLoadTrexDx11RuntimeV191()) {
    Logger::err("DX11_V191_FORCE_TREX_DX11_RUNTIME: server continuing after forced runtime load failure so logs remain visible.");
  }
  Dx11BridgeServerInitializeTrexRemixRuntimeV191();
  if (!Dx11BridgeServerForceLoadTrexDx11RuntimeV190()) {
    Logger::err("DX11_V190_FORCE_TREX_DX11_RUNTIME: server continuing after forced runtime load failure so logs remain visible.");
  }
  Dx11BridgeServerInitializeTrexRemixRuntimeV190();
  if (!Dx11BridgeServerForceLoadTrexDx11RuntimeV187()) {
    Logger::err("DX11_V187_FORCE_TREX_DX11_RUNTIME: server continuing after forced runtime load failure so logs remain visible.");
  }
  Dx11BridgeServerInitializeTrexRemixRuntimeV187();
  if (!Dx11BridgeServerForceLoadTrexDx11RuntimeV186()) {
    Logger::err("DX11_V186_FORCE_TREX_DX11_RUNTIME: server continuing after forced runtime load failure so logs remain visible.");
  }
  Dx11BridgeServerInitializeTrexRemixRuntimeV186();
  if (!Dx11BridgeServerForceLoadTrexDx11RuntimeV180()) {
    Logger::err("DX11_V180_FORCE_TREX_DX11_RUNTIME: server continuing after forced runtime load failure so logs remain visible.");
  }
  Dx11BridgeServerInitializeTrexRemixRuntimeV180();
  gTimeStart = std::chrono::high_resolution_clock::now();
  
  if (!initFileSys()) {
    Logger::err("Failed to initialize rtx filesystem!");
    return 1;
  }

  Config::init(Config::App::Server);
  GlobalOptions::init();
  Logger::init();

  // Always setup exception handler on server
  ExceptionHandler::get().init();

  // Identify yourself
  Logger::info("==================\nNVIDIA RTX Remix Bridge Server\n==================");
  Logger::info(std::string("Version: ") + std::string(BRIDGE_VERSION));
#ifdef _WIN64
  Logger::info("Running in x64 mode!");
#else
  Logger::warn("Running in x86 mode! Are you sure this is what you want? RTX will not work this way, please run the 64-bit server instead!");
#endif

  int argCount = 0; LPWSTR* argList = CommandLineToArgvW(pCmdLine, &argCount); bool dx11FallbackArgListV63 = false; if (argCount < 2 || argList == nullptr) { Logger::warn("DX11 bridge: missing/empty command line arguments; trying DX11_BRIDGE_GUID/DX11_BRIDGE_VERSION fallback."); if (argList) { LocalFree(argList); argList = nullptr; } argList = Dx11BridgeBuildFallbackArgListV63(&argCount); dx11FallbackArgListV63 = true; } if (argCount < 2 || argList == nullptr) { Logger::err("DX11 bridge: server still has no GUID/version after command-line and fallback parsing."); return 1; }
  if (gUniqueIdentifier.setGuid(&argList[0])) {
    Logger::info("Launched server with GUID " + gUniqueIdentifier.toString());
  } else {
    Logger::err("Server was invoked with invalid GUID! Unable to establish bridge, exiting...");
    return 1;
  }
  if (wcscmp(argList[1], BRIDGE_VERSION_W) != 0) {
    Logger::err(format_string("Client (%s) and server (%s) version numbers do not match. Mixed version runtime execution is currently not supported! Exiting...", argList[1], BRIDGE_VERSION));
    return 1;
  }
  LocalFree(argList);

  initModuleBridge();
  initDeviceBridge();

  if (GlobalOptions::getUseSharedHeap()) {
    SharedHeap::init();
  }

  gpPresent = new NamedSemaphore("Present", GlobalOptions::getPresentSemaphoreMaxFrames(), GlobalOptions::getPresentSemaphoreMaxFrames());

  // Initialize our shared client command queue as a Reader.
  // (1) Wait for connection for client.
  Logger::info("Server started up, waiting for connection from client...");
  const auto waitForSynResult = DeviceBridge::waitForCommand(Bridge_Syn, GlobalOptions::getStartupTimeout());
  switch (waitForSynResult) {
  case Result::Timeout:
  {
    Logger::err("Timeout. Connection not established to client application/game.");
    Logger::err("Are you sure a client application/game is running and invoked this application?");
    return 1;
  }
  case Result::Failure:
  {
    Logger::err("Failed to connect to client.");
    return 1;
  }
  }
  const auto synResponse = DeviceBridge::pop_front(); // Get process handle from Syn response
  // Pulling default data sent from client to have the data queue in sync
  {
    PULL_U(uid);
  }
  Logger::info("Registering exit callback in case client exits unexpectedly.");
  RegisterExitCallback(synResponse.pHandle);

  RegisterMessageChannel();

  // (2) Load d3d11.dll, which could be original system, dxvk-remix, or something else...
  wchar_t dx11BridgeModeBuf[16] = {}; const bool dx11BridgeMode = GetEnvironmentVariableW(L"DX11_BRIDGE_MODE", dx11BridgeModeBuf, 16) > 0;
  if (dx11BridgeMode) {
    // DX11_V227_BRIDGE_REMIX_INIT: the DX11 capture bridge streams RemixApi_* scene
    // commands (meshes/materials/lights/instances). The server MUST have a live Remix
    // API interface (remixapi::g_remix) to replay them; otherwise the RemixApi handlers
    // would invoke null function pointers and crash the server (taking the game down with
    // it on the next captured draw). Initialize the Remix runtime here directly, without
    // the client-side exposeRemixApi gate which does not apply to the bridge server host.
    Logger::info("DX11 bridge mode active: initializing Remix runtime for capture->Remix scene streaming.");
    if (!remixapi::g_remix_initialized) {
      // DX11_V228_BRIDGE_REMIX_INIT_DIR_NOT_SYSTEM: the Remix runtime d3d11.dll lives in the
      // .trex DIRECTORY alongside this NvRemixBridge.exe. d3d11.dll is a Windows KnownDLL, so a
      // relative LoadLibraryW(L"d3d11.dll") resolves to System32\d3d11.dll (no remixapi export)
      // instead of the .trex runtime. Build the FULL path from our own module directory so the
      // Remix runtime is loaded from the DIR, not the SYSTEM, KnownDLL.
      wchar_t remixD3D11Path[MAX_PATH] = {};
      if (GetModuleFileNameW(nullptr, remixD3D11Path, _countof(remixD3D11Path)) == 0) {
        Logger::err("DX11 bridge mode: could not resolve NvRemixBridge.exe path for .trex d3d11.dll; aborting Remix init.");
        return 1;
      }
      if (wchar_t* lastSlash = wcsrchr(remixD3D11Path, L'\\')) {
        lastSlash[1] = 0;
      }
      wcscat_s(remixD3D11Path, _countof(remixD3D11Path), L"d3d11.dll");
      Logger::info(format_string("DX11 bridge mode: loading Remix runtime from .trex DIR (not System32): %ls", remixD3D11Path));
      const remixapi_ErrorCode status = remixapi_lib_loadRemixDllAndInitialize(remixD3D11Path, &remixapi::g_remix, &remixapi::g_remix_dll);
      if (status != REMIXAPI_ERROR_CODE_SUCCESS) {
        Logger::err(format_string("DX11 bridge mode: Remix API init failed: %d; capture->Remix rendering will be unavailable (handlers are null-guarded so the server stays alive).", status));
      } else {
        remixapi::g_remix_initialized = true;
        Logger::info("DX11 bridge mode: Remix API initialized for capture->Remix streaming.");
      }
    }
  } else {
    Logger::info("Initializing D3D11...");
    if (!InitializeD3D()) { return 1; }
  }

  // (3) Send ACK to Client. Connection has been established
  Logger::info("Sync request received, sending ACK response...");
  ServerMessage { Commands::Bridge_Ack, (uintptr_t) gpClientMessageChannel->getWorkerThreadId() };

  // (4) Wait for second expected cmd: CONTINUE (ACK v. 2)
  Logger::info("Done! Now waiting for client to consume the response...");
  const auto WaitForContinueResult = DeviceBridge::waitForCommandAndDiscard(Bridge_Continue, GlobalOptions::getStartupTimeout());
  switch (WaitForContinueResult) {
  case Result::Timeout:
  {
    Logger::err("Timeout. Application failed to give go-ahead (CONTINUE) to operate.");
    return 1;
  }
  case Result::Failure:
  {
    Logger::err("Connection could to client application/game could not be finalized.");
    return 1;
  }
  }
  // Pulling default data sent from client to have the data queue in sync
  {
    PULL_U(uid);
  }
  // (5) Ready to listen for incoming commands
  Logger::info("Handshake completed! Now waiting for incoming commands...");

  std::atomic<bool> bSignalDone(false);
  auto moduleCmdProcessingThread = std::thread([&]() {
    processModuleCommandQueue(&bSignalDone);
  });
  // Process device commands
  ProcessDeviceCommandQueue();
  bSignalDone.store(true);
  moduleCmdProcessingThread.join();

  if (!dumpLeakedObjects()) {
    bridge_util::Logger::debug("No leaked objects dicovered at Direct3D module eviction.");
  }

  // Command processing finished, clean up and exit
  Logger::info("Command processing loop finished, cleaning up and exiting...");
  if (ghModule) {
    // Skip unloading the d3d11.dll for now, since it seems to be doing more harm than good
    // especially with other dependencies loaded by dxvk and threads that may deadlock due
    // to being unable to acquire certain locks during unloading.
    //FreeLibrary(ghModule);
    ghModule = nullptr;
  }

  // Clean up client exit callback handler
  if (hWait) {
    // According to MSDN docs INVALID_HANDLE_VALUE means the function
    // waits for all callback functions to complete before returning.
    UnregisterWaitEx(hWait, INVALID_HANDLE_VALUE);
    hWait = NULL;
  }

  Logger::info("Shutdown cleanup successful, exiting now!");

  const auto timeEnd = std::chrono::high_resolution_clock::now();
  std::stringstream uptimeSS;
  uptimeSS << "[Uptime]: ";
  uptimeSS <<
    std::chrono::duration_cast<std::chrono::seconds>(timeEnd - gTimeStart).count();
  uptimeSS << "s";
  Logger::info(uptimeSS.str());

  {
    ServerMessage { Commands::Bridge_Ack };
  }
  return 0;
}