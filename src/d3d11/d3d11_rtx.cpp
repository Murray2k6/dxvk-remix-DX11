#include "d3d11_rtx.h"

// Include dxvk_device.h before any rtx headers so that dxvk_buffer.h and
// sibling headers (included bare by rtx_utils.h) are already in the TU.
#include "../dxvk/dxvk_device.h"

#include "d3d11_context.h"
#include "d3d11_buffer.h"
#include "d3d11_input_layout.h"
#include "d3d11_device.h"
#include "d3d11_view_srv.h"
#include "d3d11_sampler.h"
#include "d3d11_depth_stencil.h"
#include "d3d11_blend.h"
#include "d3d11_rasterizer.h"

#include "../dxvk/imgui/dxvk_imgui.h"
#include "../dxvk/rtx_render/rtx_context.h"
#include "../dxvk/rtx_render/rtx_options.h"
#include "../dxvk/rtx_render/rtx_camera.h"
#include "../dxvk/rtx_render/rtx_camera_manager.h"
#include "../dxvk/rtx_render/rtx_scene_manager.h"
#include "../dxvk/rtx_render/rtx_light_manager.h"
#include "../dxvk/rtx_render/rtx_matrix_helpers.h"

#include <cstring>
#include <cmath>
#include <algorithm>
#include <array>
#include <limits>
#include <vector>

namespace dxvk {

  namespace {

    bool isRenderDocAttached() {
      return ::GetModuleHandleW(L"renderdoc.dll") != nullptr;
    }

  }

  static uint32_t getTextureUiFeatureFlagsForView(const Rc<DxvkImageView>& imageView) {
    uint32_t textureFeatureFlags = ImGUI::kTextureFlagsDefault;

    const VkImageUsageFlags usage = imageView->imageInfo().usage;
    if ((usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) != 0 ||
        (usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0) {
      textureFeatureFlags |= ImGUI::kTextureFlagsRenderTarget;
    }

    return textureFeatureFlags;
  }

  // Map D3D11_BLEND → VkBlendFactor.  Mirrors D3D11BlendState::DecodeBlendFactor
  // but kept local to avoid exposing internal statics.
  static VkBlendFactor mapD3D11Blend(D3D11_BLEND b, bool isAlpha) {
    switch (b) {
      case D3D11_BLEND_ZERO:              return VK_BLEND_FACTOR_ZERO;
      case D3D11_BLEND_ONE:               return VK_BLEND_FACTOR_ONE;
      case D3D11_BLEND_SRC_COLOR:         return VK_BLEND_FACTOR_SRC_COLOR;
      case D3D11_BLEND_INV_SRC_COLOR:     return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
      case D3D11_BLEND_SRC_ALPHA:         return VK_BLEND_FACTOR_SRC_ALPHA;
      case D3D11_BLEND_INV_SRC_ALPHA:     return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
      case D3D11_BLEND_DEST_ALPHA:        return VK_BLEND_FACTOR_DST_ALPHA;
      case D3D11_BLEND_INV_DEST_ALPHA:    return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
      case D3D11_BLEND_DEST_COLOR:        return VK_BLEND_FACTOR_DST_COLOR;
      case D3D11_BLEND_INV_DEST_COLOR:    return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
      case D3D11_BLEND_SRC_ALPHA_SAT:     return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
      case D3D11_BLEND_BLEND_FACTOR:      return isAlpha ? VK_BLEND_FACTOR_CONSTANT_ALPHA : VK_BLEND_FACTOR_CONSTANT_COLOR;
      case D3D11_BLEND_INV_BLEND_FACTOR:  return isAlpha ? VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA : VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
      case D3D11_BLEND_SRC1_COLOR:        return VK_BLEND_FACTOR_SRC1_COLOR;
      case D3D11_BLEND_INV_SRC1_COLOR:    return VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR;
      case D3D11_BLEND_SRC1_ALPHA:        return VK_BLEND_FACTOR_SRC1_ALPHA;
      case D3D11_BLEND_INV_SRC1_ALPHA:    return VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA;
      default:                            return VK_BLEND_FACTOR_ONE;
    }
  }

  // Map D3D11_BLEND_OP → VkBlendOp.
  static VkBlendOp mapD3D11BlendOp(D3D11_BLEND_OP op) {
    switch (op) {
      case D3D11_BLEND_OP_ADD:          return VK_BLEND_OP_ADD;
      case D3D11_BLEND_OP_SUBTRACT:     return VK_BLEND_OP_SUBTRACT;
      case D3D11_BLEND_OP_REV_SUBTRACT: return VK_BLEND_OP_REVERSE_SUBTRACT;
      case D3D11_BLEND_OP_MIN:          return VK_BLEND_OP_MIN;
      case D3D11_BLEND_OP_MAX:          return VK_BLEND_OP_MAX;
      default:                          return VK_BLEND_OP_ADD;
    }
  }

  D3D11Rtx::D3D11Rtx(D3D11DeviceContext* pContext)
    : m_context(pContext) {}

  void D3D11Rtx::ClearMaterialTextures(LegacyMaterialData& mat) const {
    for (uint32_t i = 0; i < LegacyMaterialData::kMaxSupportedTextures; ++i) {
      mat.colorTextures[i] = TextureRef {};
      mat.samplers[i] = nullptr;
      mat.colorTextureSlot[i] = kInvalidResourceSlot;
    }

    mat.updateCachedHash();
  }

  Rc<DxvkSampler> D3D11Rtx::getDefaultSampler() const {
    if (m_defaultSampler == nullptr) {
      // D3D11 spec default: linear min/mag/mip, clamp UVW, no compare, no aniso
      DxvkSamplerCreateInfo info;
      info.magFilter      = VK_FILTER_LINEAR;
      info.minFilter      = VK_FILTER_LINEAR;
      info.mipmapMode     = VK_SAMPLER_MIPMAP_MODE_LINEAR;
      info.mipmapLodBias  = 0.0f;
      info.mipmapLodMin   = -1000.0f;
      info.mipmapLodMax   =  1000.0f;
      info.useAnisotropy  = VK_FALSE;
      info.maxAnisotropy  = 1.0f;
      info.addressModeU   = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
      info.addressModeV   = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
      info.addressModeW   = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
      info.compareToDepth = VK_FALSE;
      info.compareOp      = VK_COMPARE_OP_NEVER;
      info.borderColor    = VkClearColorValue{};
      info.usePixelCoord  = VK_FALSE;
      m_defaultSampler = m_context->m_device->createSampler(info);
    }
    return m_defaultSampler;
  }

  void D3D11Rtx::Initialize() {
    // Scale geometry workers to available cores (min 2, max 6).
    // D3D11 games typically have high draw call counts, so more workers pay off.
    const uint32_t cores = std::max(2u, std::thread::hardware_concurrency());
    const uint32_t workers = std::min(std::max(cores / 2, 2u), 6u);
    m_pGeometryWorkers = std::make_unique<GeometryProcessor>(workers, "d3d11-geometry");

    // --- D3D11 sensible defaults (Default layer = lowest priority) ---
    // Written to the Default layer so rtx.conf, user.conf, and all other
    // config layers override them naturally.  Without this, setDeferred()
    // writes to the Derived layer (priority 5) which stomps rtx.conf (priority 3)
    // and makes per-game config files useless.
    const RtxOptionLayer* defaults = RtxOptionLayer::getDefaultLayer();

    // Do not force a fused world-view convention globally.
    // The D3D11 path already scans cbuffers for separate projection, view,
    // and world matrices on a per-draw basis, which is the only engine-
    // agnostic behavior that works across mixed D3D11 renderers.
    // Games that truly provide fused world/view transforms can still opt in
    // explicitly via rtx.fusedWorldViewMode, but separate-matrix engines
    // should not be coerced into View mode by default.
    RtxOptions::fusedWorldViewModeObject().setDeferred(FusedWorldViewMode::None, defaults);

    // Anti-culling: D3D11 engines aggressively frustum-cull objects before
    // issuing draw calls.  Without anti-culling, off-screen objects vanish
    // from reflections, shadows, and GI.
    RtxOptions::AntiCulling::Object::enableObject().setDeferred(true, defaults);
    RtxOptions::AntiCulling::Object::enableHighPrecisionAntiCullingObject().setDeferred(true, defaults);
    RtxOptions::AntiCulling::Object::numObjectsToKeepObject().setDeferred(20000u, defaults);
    RtxOptions::AntiCulling::Object::fovScaleObject().setDeferred(2.0f, defaults);
    RtxOptions::AntiCulling::Object::farPlaneScaleObject().setDeferred(10.0f, defaults);
    RtxOptions::AntiCulling::Light::enableObject().setDeferred(true, defaults);

    // Use incoming vertex buffers directly (skip copy to staging → saves VRAM + bandwidth).
    RtxOptions::useBuffersDirectlyObject().setDeferred(true, defaults);

    // --- Fallback lighting ---
    // D3D11 has no legacy lighting API — all lighting is shader-driven,
    // so Remix never receives explicit light definitions from the application.
    // Force the fallback light to Always so the scene is lit even if there are
    // no Remix USD light assets placed yet.  Use a bright distant light that
    // produces reasonable illumination for most indoor/outdoor scenes.
    LightManager::fallbackLightModeObject().setDeferred(LightManager::FallbackLightMode::Always, defaults);
    LightManager::fallbackLightTypeObject().setDeferred(LightManager::FallbackLightType::Distant, defaults);
    LightManager::fallbackLightRadianceObject().setDeferred(Vector3(4.0f, 4.0f, 4.0f), defaults);
    LightManager::fallbackLightDirectionObject().setDeferred(Vector3(-0.3f, -1.0f, 0.5f), defaults);
    LightManager::fallbackLightAngleObject().setDeferred(5.0f, defaults);

  }

  void D3D11Rtx::OnDraw(UINT vertexCount, UINT startVertex) {
    SubmitDraw(false, vertexCount, startVertex, 0);
  }

  void D3D11Rtx::OnDrawIndexed(UINT indexCount, UINT startIndex, INT baseVertex) {
    SubmitDraw(true, indexCount, startIndex, baseVertex);
  }

  void D3D11Rtx::OnDrawInstanced(UINT vertexCountPerInstance, UINT instanceCount, UINT startVertex, UINT startInstance) {
    SubmitInstancedDraw(false, vertexCountPerInstance, startVertex, 0, instanceCount, startInstance);
  }

  void D3D11Rtx::OnDrawIndexedInstanced(UINT indexCountPerInstance, UINT instanceCount, UINT startIndex, INT baseVertex, UINT startInstance) {
    SubmitInstancedDraw(true, indexCountPerInstance, startIndex, baseVertex, instanceCount, startInstance);
  }

  void D3D11Rtx::ResetCommandListState() {
    m_drawCallID = 0;
    m_drawsSinceFlush = 0;
    m_submitRejectStats = {};
  }

  void D3D11Rtx::SubmitInstancedDraw(bool indexed, UINT count, UINT start, INT base,
                                      UINT instanceCount, UINT startInstance) {
    if (instanceCount <= 1) {
      SubmitDraw(indexed, count, start, base);
      return;
    }

    // Find per-instance float4 rows in the input layout that form a world matrix.
    // Engines encode this as 3 or 4 consecutive float4 elements with per-instance step rate,
    // using semantics like INSTANCETRANSFORM, WORLD, I, INST, or TEXCOORD at high indices.
    auto* layout = m_context->m_state.ia.inputLayout.ptr();
    if (!layout) {
      SubmitDraw(indexed, count, start, base);
      return;
    }

    const auto& semantics = layout->GetRtxSemantics();

    struct Float4Row {
      uint32_t inputSlot;
      uint32_t byteOffset;
    };

    std::vector<Float4Row> instRows;
    uint32_t instSlot = UINT32_MAX;

    for (const auto& s : semantics) {
      if (!s.perInstance) continue;
      if (s.format != VK_FORMAT_R32G32B32A32_SFLOAT) continue;

      // Accept any per-instance float4 — most engines use INSTANCETRANSFORM, WORLD, I, INST,
      // or repurpose TEXCOORD with high indices. The key signal is per-instance + float4.
      if (instSlot == UINT32_MAX)
        instSlot = s.inputSlot;

      // Only collect rows from the same input slot.
      if (s.inputSlot != instSlot) continue;
      instRows.push_back({s.inputSlot, s.byteOffset});
    }

    if (instRows.size() < 3) {
      // No instance transform found — submit once without instance data.
      // This handles instancing used for non-transform data (colors, etc.)
      static uint32_t sNoInstXformLog = 0;
      if (sNoInstXformLog < 3) {
        ++sNoInstXformLog;
        Logger::info(str::format("[D3D11Rtx] Instanced draw (", instanceCount,
                                 " instances) has no per-instance transform (", instRows.size(),
                                 " float4 rows). Submitting single draw."));
      }
      SubmitDraw(indexed, count, start, base);
      return;
    }

    // Read the instance buffer
    const auto& vb = m_context->m_state.ia.vertexBuffers[instSlot];
    if (vb.buffer == nullptr) {
      SubmitDraw(indexed, count, start, base);
      return;
    }

    DxvkBufferSlice instBufSlice = vb.buffer->GetBufferSlice(vb.offset);
    const uint32_t instStride = vb.stride;
    const size_t instBufLen = instBufSlice.length();
    if (instStride == 0) {
      SubmitDraw(indexed, count, start, base);
      return;
    }

    // Cap to avoid excessive submission — configurable via rtx.maxInstanceSubmissions
    const UINT maxInstances = std::min(instanceCount, RtxOptions::maxInstanceSubmissions());

    static uint32_t sInstLog = 0;
    if (sInstLog < 3) {
      ++sInstLog;
      Logger::info(str::format("[D3D11Rtx] Instanced draw: ", instanceCount,
                               " instances, ", instRows.size(), " float4 rows in slot ",
                               instSlot, ", stride=", instStride));
    }

    auto sampleInstanceIndex = [&](UINT sampleIndex) {
      if (maxInstances <= 1 || instanceCount <= maxInstances)
        return startInstance + sampleIndex;

      return startInstance + UINT((uint64_t(sampleIndex) * uint64_t(instanceCount - 1))
        / uint64_t(maxInstances - 1));
    };

    for (UINT i = 0; i < maxInstances; ++i) {
      UINT instIdx = sampleInstanceIndex(i);
      size_t instOffset = static_cast<size_t>(instIdx) * instStride;

      // Read 3 or 4 float4 rows to build a world matrix.
      // Row layout: each row is at instOffset + row.byteOffset within the instance buffer.
      float rows[4][4] = {};
      bool valid = true;

      for (size_t r = 0; r < std::min<size_t>(instRows.size(), 4); ++r) {
        size_t rowOff = instOffset + instRows[r].byteOffset;
        if (rowOff + 16 > instBufLen) { valid = false; break; }
        const void* ptr = instBufSlice.mapPtr(rowOff);
        if (!ptr) { valid = false; break; }
        std::memcpy(rows[r], ptr, 16);
        for (int c = 0; c < 4; ++c) {
          if (!std::isfinite(rows[r][c])) { valid = false; break; }
        }
        if (!valid) break;
      }

      if (!valid) continue;

      // If only 3 rows, the 4th row is (0,0,0,1) — affine transform.
      if (instRows.size() == 3) {
        rows[3][0] = 0.f; rows[3][1] = 0.f; rows[3][2] = 0.f; rows[3][3] = 1.f;
      }

      Matrix4 instMatrix(
        Vector4(rows[0][0], rows[0][1], rows[0][2], rows[0][3]),
        Vector4(rows[1][0], rows[1][1], rows[1][2], rows[1][3]),
        Vector4(rows[2][0], rows[2][1], rows[2][2], rows[2][3]),
        Vector4(rows[3][0], rows[3][1], rows[3][2], rows[3][3]));

      SubmitDraw(indexed, count, start, base, &instMatrix);
    }
  }

  // Read a row-major float4x4 from a mapped cbuffer.  Returns identity on bounds violation
  // or if any element is NaN/Inf (corrupt GPU memory, emulator artifacts, etc.).
  static Matrix4 readCbMatrix(const uint8_t* ptr, size_t offset, size_t bufSize) {
    if (offset + 64 > bufSize)
      return Matrix4();
    float raw[4][4];
    std::memcpy(raw, ptr + offset, 64);
    for (int r = 0; r < 4; ++r)
      for (int c = 0; c < 4; ++c)
        if (!std::isfinite(raw[r][c]))
          return Matrix4();
    return Matrix4(
      Vector4(raw[0][0], raw[0][1], raw[0][2], raw[0][3]),
      Vector4(raw[1][0], raw[1][1], raw[1][2], raw[1][3]),
      Vector4(raw[2][0], raw[2][1], raw[2][2], raw[2][3]),
      Vector4(raw[3][0], raw[3][1], raw[3][2], raw[3][3]));
  }

  struct SkinningConstantBufferSnapshot {
    uint32_t slot = UINT32_MAX;
    std::vector<uint8_t> data;
  };

  static float decodeFloat16(uint16_t value) {
    const uint32_t sign = (value & 0x8000u) << 16;
    uint32_t exponent = (value >> 10) & 0x1fu;
    uint32_t mantissa = value & 0x03ffu;

    uint32_t decoded = 0;
    if (exponent == 0) {
      if (mantissa == 0) {
        decoded = sign;
      } else {
        exponent = 127 - 15 + 1;
        while ((mantissa & 0x0400u) == 0) {
          mantissa <<= 1;
          --exponent;
        }
        mantissa &= 0x03ffu;
        decoded = sign | (exponent << 23) | (mantissa << 13);
      }
    } else if (exponent == 0x1fu) {
      decoded = sign | 0x7f800000u | (mantissa << 13);
    } else {
      decoded = sign | ((exponent + (127 - 15)) << 23) | (mantissa << 13);
    }

    float result = 0.0f;
    std::memcpy(&result, &decoded, sizeof(result));
    return result;
  }

  static bool decodeBlendWeights(const uint8_t* src, VkFormat format, float outWeights[4], uint32_t& outComponentCount) {
    outComponentCount = 0;
    std::fill(outWeights, outWeights + 4, 0.0f);

    switch (format) {
      case VK_FORMAT_R32_SFLOAT: {
        const float* values = reinterpret_cast<const float*>(src);
        outWeights[0] = values[0];
        outComponentCount = 1;
      } break;
      case VK_FORMAT_R32G32_SFLOAT: {
        const float* values = reinterpret_cast<const float*>(src);
        outWeights[0] = values[0];
        outWeights[1] = values[1];
        outComponentCount = 2;
      } break;
      case VK_FORMAT_R32G32B32_SFLOAT: {
        const float* values = reinterpret_cast<const float*>(src);
        outWeights[0] = values[0];
        outWeights[1] = values[1];
        outWeights[2] = values[2];
        outComponentCount = 3;
      } break;
      case VK_FORMAT_R32G32B32A32_SFLOAT: {
        const float* values = reinterpret_cast<const float*>(src);
        outWeights[0] = values[0];
        outWeights[1] = values[1];
        outWeights[2] = values[2];
        outWeights[3] = values[3];
        outComponentCount = 4;
      } break;
      case VK_FORMAT_R16_SFLOAT: {
        const uint16_t* values = reinterpret_cast<const uint16_t*>(src);
        outWeights[0] = decodeFloat16(values[0]);
        outComponentCount = 1;
      } break;
      case VK_FORMAT_R16G16_SFLOAT: {
        const uint16_t* values = reinterpret_cast<const uint16_t*>(src);
        outWeights[0] = decodeFloat16(values[0]);
        outWeights[1] = decodeFloat16(values[1]);
        outComponentCount = 2;
      } break;
      case VK_FORMAT_R16G16B16A16_SFLOAT: {
        const uint16_t* values = reinterpret_cast<const uint16_t*>(src);
        outWeights[0] = decodeFloat16(values[0]);
        outWeights[1] = decodeFloat16(values[1]);
        outWeights[2] = decodeFloat16(values[2]);
        outWeights[3] = decodeFloat16(values[3]);
        outComponentCount = 4;
      } break;
      case VK_FORMAT_R8_UNORM: {
        outWeights[0] = src[0] / 255.0f;
        outComponentCount = 1;
      } break;
      case VK_FORMAT_R8G8_UNORM: {
        outWeights[0] = src[0] / 255.0f;
        outWeights[1] = src[1] / 255.0f;
        outComponentCount = 2;
      } break;
      case VK_FORMAT_R8G8B8A8_UNORM: {
        outWeights[0] = src[0] / 255.0f;
        outWeights[1] = src[1] / 255.0f;
        outWeights[2] = src[2] / 255.0f;
        outWeights[3] = src[3] / 255.0f;
        outComponentCount = 4;
      } break;
      case VK_FORMAT_R16_UNORM: {
        const uint16_t* values = reinterpret_cast<const uint16_t*>(src);
        outWeights[0] = values[0] / 65535.0f;
        outComponentCount = 1;
      } break;
      case VK_FORMAT_R16G16_UNORM: {
        const uint16_t* values = reinterpret_cast<const uint16_t*>(src);
        outWeights[0] = values[0] / 65535.0f;
        outWeights[1] = values[1] / 65535.0f;
        outComponentCount = 2;
      } break;
      case VK_FORMAT_R16G16B16A16_UNORM: {
        const uint16_t* values = reinterpret_cast<const uint16_t*>(src);
        outWeights[0] = values[0] / 65535.0f;
        outWeights[1] = values[1] / 65535.0f;
        outWeights[2] = values[2] / 65535.0f;
        outWeights[3] = values[3] / 65535.0f;
        outComponentCount = 4;
      } break;
      default:
        return false;
    }

    for (uint32_t i = 0; i < outComponentCount; ++i) {
      if (!std::isfinite(outWeights[i]))
        return false;
      outWeights[i] = std::clamp(outWeights[i], 0.0f, 1.0f);
    }

    return outComponentCount > 0;
  }

  static bool decodeBlendIndices(const uint8_t* src, VkFormat format, uint32_t outIndices[4], uint32_t& outComponentCount) {
    outComponentCount = 0;
    std::fill(outIndices, outIndices + 4, 0u);

    switch (format) {
      case VK_FORMAT_R8_UINT:
      case VK_FORMAT_R8_USCALED:
        outIndices[0] = src[0];
        outComponentCount = 1;
        break;
      case VK_FORMAT_R8G8_UINT:
      case VK_FORMAT_R8G8_USCALED:
        outIndices[0] = src[0];
        outIndices[1] = src[1];
        outComponentCount = 2;
        break;
      case VK_FORMAT_R8G8B8A8_UINT:
      case VK_FORMAT_R8G8B8A8_USCALED:
        outIndices[0] = src[0];
        outIndices[1] = src[1];
        outIndices[2] = src[2];
        outIndices[3] = src[3];
        outComponentCount = 4;
        break;
      case VK_FORMAT_R16_UINT: {
        const uint16_t* values = reinterpret_cast<const uint16_t*>(src);
        outIndices[0] = values[0];
        outComponentCount = 1;
      } break;
      case VK_FORMAT_R16G16_UINT: {
        const uint16_t* values = reinterpret_cast<const uint16_t*>(src);
        outIndices[0] = values[0];
        outIndices[1] = values[1];
        outComponentCount = 2;
      } break;
      case VK_FORMAT_R16G16B16A16_UINT: {
        const uint16_t* values = reinterpret_cast<const uint16_t*>(src);
        outIndices[0] = values[0];
        outIndices[1] = values[1];
        outIndices[2] = values[2];
        outIndices[3] = values[3];
        outComponentCount = 4;
      } break;
      case VK_FORMAT_R32_UINT: {
        const uint32_t* values = reinterpret_cast<const uint32_t*>(src);
        outIndices[0] = values[0];
        outComponentCount = 1;
      } break;
      case VK_FORMAT_R32G32_UINT: {
        const uint32_t* values = reinterpret_cast<const uint32_t*>(src);
        outIndices[0] = values[0];
        outIndices[1] = values[1];
        outComponentCount = 2;
      } break;
      case VK_FORMAT_R32G32B32_UINT: {
        const uint32_t* values = reinterpret_cast<const uint32_t*>(src);
        outIndices[0] = values[0];
        outIndices[1] = values[1];
        outIndices[2] = values[2];
        outComponentCount = 3;
      } break;
      case VK_FORMAT_R32G32B32A32_UINT: {
        const uint32_t* values = reinterpret_cast<const uint32_t*>(src);
        outIndices[0] = values[0];
        outIndices[1] = values[1];
        outIndices[2] = values[2];
        outIndices[3] = values[3];
        outComponentCount = 4;
      } break;
      default:
        return false;
    }

    return outComponentCount > 0;
  }

  static VkFormat normalizedBlendWeightFormat(uint32_t explicitWeightCount) {
    switch (explicitWeightCount) {
      case 1: return VK_FORMAT_R32_SFLOAT;
      case 2: return VK_FORMAT_R32G32_SFLOAT;
      case 3: return VK_FORMAT_R32G32B32_SFLOAT;
      default: return VK_FORMAT_UNDEFINED;
    }
  }

  static bool isSkinningMatrix(const Matrix4& m) {
    if (std::abs(m[3][3] - 1.0f) > 0.05f)
      return false;
    if (std::abs(m[0][3]) > 0.05f || std::abs(m[1][3]) > 0.05f || std::abs(m[2][3]) > 0.05f)
      return false;

    for (int row = 0; row < 4; ++row) {
      for (int col = 0; col < 4; ++col) {
        if (!std::isfinite(m[row][col]))
          return false;
      }
    }

    return true;
  }

  static Rc<DxvkBuffer> createHostVisibleHelperBuffer(const Rc<DxvkDevice>& device, VkDeviceSize size, const char* name) {
    DxvkBufferCreateInfo info;
    info.size = size;
    info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    info.stages = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
    info.access = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT;

    return device->createBuffer(
      info,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
      DxvkMemoryStats::Category::RTXBuffer,
      name);
  }

  static bool semanticNameStartsWith(const D3D11RtxSemantic& semantic, const char* prefix) {
    return std::strncmp(semantic.name, prefix, std::strlen(prefix)) == 0;
  }

  static bool isFloatSemantic(const D3D11RtxSemantic& semantic) {
    return semantic.componentType == DxbcScalarType::Float32;
  }

  static bool isSupportedTexcoordFormat(VkFormat format) {
    return format == VK_FORMAT_R32G32B32A32_SFLOAT
        || format == VK_FORMAT_R32G32B32_SFLOAT
        || format == VK_FORMAT_R32G32_SFLOAT
      || format == VK_FORMAT_R16G16_SFLOAT
      || format == VK_FORMAT_R16G16B16A16_SFLOAT
      || format == VK_FORMAT_R8G8_UNORM
      || format == VK_FORMAT_R8G8_SNORM
      || format == VK_FORMAT_R8G8B8A8_UNORM
      || format == VK_FORMAT_R8G8B8A8_SNORM
      || format == VK_FORMAT_R16G16_UNORM
      || format == VK_FORMAT_R16G16_SNORM
      || format == VK_FORMAT_R16G16B16A16_UNORM
      || format == VK_FORMAT_R16G16B16A16_SNORM;
  }

  static bool isPositionFormat(VkFormat format) {
    return format == VK_FORMAT_R32G32_SFLOAT
        || format == VK_FORMAT_R32G32B32_SFLOAT
        || format == VK_FORMAT_R32G32B32A32_SFLOAT
        || format == static_cast<VkFormat>(97);
  }

  static bool isNormalFormat(VkFormat format) {
    return format == VK_FORMAT_R8G8B8A8_UNORM
        || format == VK_FORMAT_R32G32B32_SFLOAT
        || format == VK_FORMAT_R32G32B32A32_SFLOAT
        || format == VK_FORMAT_R32G32_SFLOAT
        || format == VK_FORMAT_R16G16_SFLOAT
        || format == static_cast<VkFormat>(65);
  }

  static bool isColorFormat(VkFormat format) {
    return format == VK_FORMAT_B8G8R8A8_UNORM
        || format == VK_FORMAT_R8G8B8A8_UNORM
        || format == VK_FORMAT_R32G32B32A32_SFLOAT;
  }

  static bool isBlendWeightFormat(VkFormat format) {
    switch (format) {
      case VK_FORMAT_R32_SFLOAT:
      case VK_FORMAT_R32G32_SFLOAT:
      case VK_FORMAT_R32G32B32_SFLOAT:
      case VK_FORMAT_R32G32B32A32_SFLOAT:
      case VK_FORMAT_R16_SFLOAT:
      case VK_FORMAT_R16G16_SFLOAT:
      case VK_FORMAT_R16G16B16A16_SFLOAT:
      case VK_FORMAT_R8_UNORM:
      case VK_FORMAT_R8G8_UNORM:
      case VK_FORMAT_R8G8B8A8_UNORM:
      case VK_FORMAT_R16_UNORM:
      case VK_FORMAT_R16G16_UNORM:
      case VK_FORMAT_R16G16B16A16_UNORM:
        return true;
      default:
        return false;
    }
  }

  static bool isBlendIndexFormat(VkFormat format) {
    switch (format) {
      case VK_FORMAT_R8_UINT:
      case VK_FORMAT_R8_USCALED:
      case VK_FORMAT_R8G8_UINT:
      case VK_FORMAT_R8G8_USCALED:
      case VK_FORMAT_R8G8B8A8_UINT:
      case VK_FORMAT_R8G8B8A8_USCALED:
      case VK_FORMAT_R16_UINT:
      case VK_FORMAT_R16G16_UINT:
      case VK_FORMAT_R16G16B16A16_UINT:
      case VK_FORMAT_R32_UINT:
      case VK_FORMAT_R32G32_UINT:
      case VK_FORMAT_R32G32B32_UINT:
      case VK_FORMAT_R32G32B32A32_UINT:
        return true;
      default:
        return false;
    }
  }

  static int scorePositionSemantic(const D3D11RtxSemantic& semantic) {
    if (semantic.perInstance || semantic.systemValue != DxbcSystemValue::None || !isPositionFormat(semantic.format))
      return std::numeric_limits<int>::min();

    int score = 0;
    if (semanticNameStartsWith(semantic, "POSITION"))
      score += 1000;
    else if (semanticNameStartsWith(semantic, "ATTRIBUTE"))
      score += 120;
    else if (semanticNameStartsWith(semantic, "TEXCOORD")) {
      if (!isFloatSemantic(semantic) || semantic.componentCount < 3)
        return std::numeric_limits<int>::min();

      score += 20;
    }
    else if (semanticNameStartsWith(semantic, "COLOR")
          || semanticNameStartsWith(semantic, "NORMAL")
          || semanticNameStartsWith(semantic, "BLEND"))
      return std::numeric_limits<int>::min();

    if (isFloatSemantic(semantic))
      score += 120;
    if (semantic.componentCount >= 3)
      score += 140;
    else if (semantic.componentCount == 2)
      score += 40;

    if (semantic.index == 0)
      score += 80;
    if (semantic.registerId == 0)
      score += 60;

    switch (semantic.format) {
      case VK_FORMAT_R32G32B32_SFLOAT:    score += 240; break;
      case VK_FORMAT_R32G32B32A32_SFLOAT: score += 200; break;
      case static_cast<VkFormat>(97):     score += 180; break;
      case VK_FORMAT_R32G32_SFLOAT:       score += 60; break;
      default: break;
    }

    return score;
  }

  static int scoreTexcoordSemantic(const D3D11RtxSemantic& semantic) {
    if (semantic.perInstance || semantic.systemValue != DxbcSystemValue::None || !isSupportedTexcoordFormat(semantic.format) || semantic.componentCount < 2)
      return std::numeric_limits<int>::min();

    int score = 0;
    if (semanticNameStartsWith(semantic, "TEXCOORD") || semanticNameStartsWith(semantic, "UV"))
      score += 1000;
    else if (semanticNameStartsWith(semantic, "ATTRIBUTE"))
      score += 140;
    else if (semanticNameStartsWith(semantic, "COLOR")) {
      if (!isFloatSemantic(semantic))
        return std::numeric_limits<int>::min();

      score += 20;
    }
    else if (semanticNameStartsWith(semantic, "POSITION")
          || semanticNameStartsWith(semantic, "NORMAL")
          || semanticNameStartsWith(semantic, "BLEND"))
      return std::numeric_limits<int>::min();

    if (isFloatSemantic(semantic))
      score += 100;
    if (semantic.componentCount == 2)
      score += 180;
    else if (semantic.componentCount == 3)
      score += 120;
    else
      score += 80;

    if (semantic.index == 0)
      score += 70;
    else if (semantic.index == 1)
      score += 40;

    if (semantic.registerId == 0)
      score -= 30;
    else
      score += 20;

    switch (semantic.format) {
      case VK_FORMAT_R32G32_SFLOAT:          score += 260; break;
      case VK_FORMAT_R16G16_SFLOAT:          score += 220; break;
      case VK_FORMAT_R32G32B32_SFLOAT:       score += 170; break;
      case VK_FORMAT_R32G32B32A32_SFLOAT:    score += 140; break;
      case VK_FORMAT_R16G16B16A16_SFLOAT:    score += 130; break;
      default: break;
    }

    return score;
  }

  static int scoreNormalSemantic(const D3D11RtxSemantic& semantic) {
    if (semantic.perInstance || semantic.systemValue != DxbcSystemValue::None || !isNormalFormat(semantic.format))
      return std::numeric_limits<int>::min();

    int score = 0;
    if (semanticNameStartsWith(semantic, "NORMAL"))
      score += 1000;
    else if (semanticNameStartsWith(semantic, "ATTRIBUTE"))
      score += 100;
    else if (semanticNameStartsWith(semantic, "POSITION")
          || semanticNameStartsWith(semantic, "TEXCOORD")
          || semanticNameStartsWith(semantic, "COLOR")
          || semanticNameStartsWith(semantic, "BLEND"))
      return std::numeric_limits<int>::min();

    if (semantic.componentCount >= 3)
      score += 140;
    else if (semantic.componentCount == 2)
      score += 40;

    switch (semantic.format) {
      case VK_FORMAT_R8G8B8A8_UNORM:      score += 220; break;
      case static_cast<VkFormat>(65):     score += 220; break;
      case VK_FORMAT_R32G32B32_SFLOAT:    score += 180; break;
      case VK_FORMAT_R32G32B32A32_SFLOAT: score += 150; break;
      case VK_FORMAT_R32G32_SFLOAT:       score += 90; break;
      case VK_FORMAT_R16G16_SFLOAT:       score += 80; break;
      default: break;
    }

    return score;
  }

  static int scoreColorSemantic(const D3D11RtxSemantic& semantic) {
    if (semantic.perInstance || semantic.systemValue != DxbcSystemValue::None || !isColorFormat(semantic.format))
      return std::numeric_limits<int>::min();

    int score = 0;
    if (semanticNameStartsWith(semantic, "COLOR"))
      score += 1000;
    else if (semanticNameStartsWith(semantic, "ATTRIBUTE"))
      score += 80;
    else if (semanticNameStartsWith(semantic, "POSITION")
          || semanticNameStartsWith(semantic, "TEXCOORD")
          || semanticNameStartsWith(semantic, "NORMAL")
          || semanticNameStartsWith(semantic, "BLEND"))
      return std::numeric_limits<int>::min();

    switch (semantic.format) {
      case VK_FORMAT_B8G8R8A8_UNORM:      score += 240; break;
      case VK_FORMAT_R8G8B8A8_UNORM:      score += 220; break;
      case VK_FORMAT_R32G32B32A32_SFLOAT: score += 90; break;
      default: break;
    }

    return score;
  }

  static int scoreBlendWeightSemantic(const D3D11RtxSemantic& semantic) {
    if (semantic.perInstance || semantic.systemValue != DxbcSystemValue::None || !isBlendWeightFormat(semantic.format))
      return std::numeric_limits<int>::min();

    int score = 0;
    if (semanticNameStartsWith(semantic, "BLENDWEIGHT"))
      score += 1000;
    else if (semanticNameStartsWith(semantic, "ATTRIBUTE"))
      score += 60;
    else if (semanticNameStartsWith(semantic, "POSITION")
          || semanticNameStartsWith(semantic, "TEXCOORD")
          || semanticNameStartsWith(semantic, "NORMAL")
          || semanticNameStartsWith(semantic, "COLOR")
          || semanticNameStartsWith(semantic, "BLENDINDICES"))
      return std::numeric_limits<int>::min();

    if (semantic.componentCount >= 1)
      score += 40;

    return score;
  }

  static int scoreBlendIndexSemantic(const D3D11RtxSemantic& semantic) {
    if (semantic.perInstance || semantic.systemValue != DxbcSystemValue::None || !isBlendIndexFormat(semantic.format))
      return std::numeric_limits<int>::min();

    int score = 0;
    if (semanticNameStartsWith(semantic, "BLENDINDICES"))
      score += 1000;
    else if (semanticNameStartsWith(semantic, "ATTRIBUTE"))
      score += 60;
    else if (semanticNameStartsWith(semantic, "POSITION")
          || semanticNameStartsWith(semantic, "TEXCOORD")
          || semanticNameStartsWith(semantic, "NORMAL")
          || semanticNameStartsWith(semantic, "COLOR")
          || semanticNameStartsWith(semantic, "BLENDWEIGHT"))
      return std::numeric_limits<int>::min();

    if (semantic.componentCount >= 1)
      score += 40;

    return score;
  }

  template <typename ScoreFn>
  static const D3D11RtxSemantic* selectBestSemantic(const std::vector<D3D11RtxSemantic>& semantics,
                                                    ScoreFn&& scoreFn,
                                                    std::initializer_list<const D3D11RtxSemantic*> excluded = {}) {
    const D3D11RtxSemantic* best = nullptr;
    int bestScore = std::numeric_limits<int>::min();

    for (const auto& semantic : semantics) {
      bool skip = false;
      for (const D3D11RtxSemantic* used : excluded) {
        if (used == &semantic) {
          skip = true;
          break;
        }
      }

      if (skip)
        continue;

      const int score = scoreFn(semantic);
      if (score > bestScore) {
        best = &semantic;
        bestScore = score;
      }
    }

    return bestScore > 0 ? best : nullptr;
  }

  // Detect a perspective projection matrix in either memory layout.
  //
  // Row-major layout used by many D3D renderers:
  //   m[0] = [±Sx, 0,   0,    0  ]
  //   m[1] = [0,  ±Sy,  0,    0  ]
  //   m[2] = [Jx,  Jy,  Q,   ±1 ]  ← perspective-divide at m[2][3]
  //   m[3] = [0,   0,   Wz,   0  ]
  //
  // Column-major matrices read back as row-major:
  //   m[0] = [±Sx, 0,   0,    0  ]
  //   m[1] = [0,  ±Sy,  0,    0  ]
  //   m[2] = [Jx,  Jy,  Q,   Wz ]  ← m[2][3] = nearPlane or 0
  //   m[3] = [0,   0,  ±1,    0  ]  ← perspective-divide at m[3][2]
  //
  // Returns: 0 = not perspective, 1 = row-major, 2 = column-major-as-row.
  static int classifyPerspective(const Matrix4& m) {
    constexpr float kTol = 0.02f;
    constexpr float kJitterTol = 0.35f;

    for (int row = 0; row < 4; ++row) {
      for (int col = 0; col < 4; ++col) {
        if (!std::isfinite(m[row][col]))
          return 0;
      }
    }

    // Shared: rows 0-1 keep the scale terms on the diagonal with no w component.
    // Off-center projection jitter lives in different cells depending on layout,
    // so do not reject m[0][2] / m[1][2] until we know which convention we have.
    if (std::abs(m[0][1]) > kTol || std::abs(m[0][3]) > kTol) return 0;
    if (std::abs(m[1][0]) > kTol || std::abs(m[1][3]) > kTol) return 0;
    if (std::abs(m[0][0]) < 0.1f || std::abs(m[1][1]) < 0.1f) return 0;

    // Row-major check: m[2][3] ≈ ±1, m[3][3] ≈ 0.
    const bool r23 = std::abs(std::abs(m[2][3]) - 1.0f) < kTol;
    const bool r33z = std::abs(m[3][3]) < kTol;
    if (r23 && r33z) {
      if (std::abs(m[0][2]) > kTol || std::abs(m[1][2]) > kTol) return 0;
      if (std::abs(m[3][0]) > kTol || std::abs(m[3][1]) > kTol) return 0;
      return 1;
    }

    // Column-major-as-row check: m[3][2] ≈ ±1, m[3][3] ≈ 0.
    const bool c32 = std::abs(std::abs(m[3][2]) - 1.0f) < kTol;
    const bool c33z = std::abs(m[3][3]) < kTol;
    if (c32 && c33z) {
      // Column-major projections transpose the off-center
      // terms into m[0][2] / m[1][2] when read as row-major.
      if (std::abs(m[0][2]) > kJitterTol || std::abs(m[1][2]) > kJitterTol) return 0;
      if (std::abs(m[2][0]) > kTol || std::abs(m[2][1]) > kTol) return 0;
      if (std::abs(m[3][0]) > kTol || std::abs(m[3][1]) > kTol) return 0;
      return 2;
    }

    return 0;
  }

  static bool isFiniteMatrix(const Matrix4& m) {
    for (int row = 0; row < 4; ++row) {
      for (int col = 0; col < 4; ++col) {
        if (!std::isfinite(m[row][col]))
          return false;
      }
    }
    return true;
  }

  static bool isAffineMatrix(const Matrix4& m) {
    if (!isFiniteMatrix(m))
      return false;
    if (std::abs(m[3][3] - 1.0f) > 0.01f)
      return false;
    if (std::abs(m[0][3]) > 0.01f || std::abs(m[1][3]) > 0.01f || std::abs(m[2][3]) > 0.01f)
      return false;
    return true;
  }

  static Matrix4 canonicalizeProjectionOrientation(const Matrix4& projection,
                                                   bool* flippedX = nullptr,
                                                   bool* flippedY = nullptr) {
    Matrix4 normalized = projection;

    const bool didFlipX = normalized[0][0] < 0.0f;
    const bool didFlipY = normalized[1][1] < 0.0f;

    if (didFlipX) {
      normalized[0][0] = -normalized[0][0];
      normalized[2][0] = -normalized[2][0];
    }

    if (didFlipY) {
      normalized[1][1] = -normalized[1][1];
      normalized[2][1] = -normalized[2][1];
    }

    if (flippedX)
      *flippedX = didFlipX;
    if (flippedY)
      *flippedY = didFlipY;

    return normalized;
  }

  // Return true if m looks like a camera view matrix (rigid-body: rotation + translation).
  // Expects row-major convention (or column-major already transposed by the caller).
  // The upper-left 3×3 should be approximately orthonormal and the last column [0,0,0,1].
  static bool isViewMatrix(const Matrix4& m) {
    // Row 3 must be [*, *, *, 1] (affine).
    if (std::abs(m[3][3] - 1.0f) > 0.01f) return false;
    // Columns 0-2 of rows 0-2 should have unit length (orthonormal rotation).
    for (int col = 0; col < 3; ++col) {
      float lenSq = m[0][col] * m[0][col] + m[1][col] * m[1][col] + m[2][col] * m[2][col];
      if (std::abs(lenSq - 1.0f) > 0.1f) return false;
    }
    // m[0][3], m[1][3], m[2][3] should be 0 (no perspective warp).
    if (std::abs(m[0][3]) > 0.01f || std::abs(m[1][3]) > 0.01f || std::abs(m[2][3]) > 0.01f)
      return false;
    // Reject identity — identity means "no view transform" which is not useful.
    if (isIdentityExact(m)) return false;
    return true;
  }

  static bool canSafelyInvertAffineViewCandidate(const Matrix4& candidate) {
    if (!isAffineMatrix(candidate))
      return false;

    const double det = determinant(candidate);
    return std::isfinite(det) && std::abs(det) >= 1e-10;
  }

  static bool resolveViewMatrixCandidate(const Matrix4& candidate, Matrix4& outView) {
    if (isViewMatrix(candidate)) {
      outView = candidate;
      return true;
    }

    if (!canSafelyInvertAffineViewCandidate(candidate))
      return false;

    Matrix4 inverseCandidate = inverseAffine(candidate);
    if (!isFiniteMatrix(inverseCandidate) || !isViewMatrix(inverseCandidate))
      return false;

    outView = inverseCandidate;
    return true;
  }

  DrawCallTransforms D3D11Rtx::ExtractTransforms() {
    DrawCallTransforms transforms;
    bool projectionWasFlippedY = false;

    // Maximum bytes to scan per cbuffer. Projection/view/world matrices are
    // always in the first few hundred bytes of a cbuffer — capping the scan
    // prevents multi-second stalls on emulators that pack all constants into
    // a single 64KB+ UBO (Xenia, Yuzu, RPCS3, Citra).
    static constexpr size_t kMaxScanBytes = 8192;  // 128 matrices

    // Compute the scannable byte range for a cbuffer binding: the intersection
    // of the bound range (constantOffset..constantOffset+constantCount) with
    // the buffer allocation, capped to kMaxScanBytes from the start of the range.
    auto cbRange = [](const D3D11ConstantBufferBinding& cb) -> std::pair<size_t, size_t> {
      const size_t bufSize = cb.buffer->Desc()->ByteWidth;
      const size_t base    = static_cast<size_t>(cb.constantOffset) * 16;
      if (base >= bufSize)
        return { 0, 0 };
      size_t end;
      if (cb.constantCount > 0)
        end = std::min(base + static_cast<size_t>(cb.constantCount) * 16, bufSize);
      else
        end = bufSize;
      if (end - base > kMaxScanBytes)
        end = base + kMaxScanBytes;
      return { base, end };
    };

    // Some engines store matrices transposed in memory;
    // transposing after read normalizes them to row-major for all our checks.
    auto readMatrix = [this](const uint8_t* ptr, size_t offset, size_t bufSize) -> Matrix4 {
      Matrix4 m = readCbMatrix(ptr, offset, bufSize);
      return m_columnMajor ? transpose(m) : m;
    };

    // Viewport and render-target size are the most reliable camera references
    // for emulators and dynamic-resolution games. The host window can change
    // independently from the actual scene resolution, so client/output extents
    // should only be fallback hints instead of the primary aspect source.
    float viewportAspect = 0.0f;
    {
      const auto& vp = m_context->m_state.rs.viewports[0];
      if (vp.Height > 0.0f)
        viewportAspect = vp.Width / vp.Height;
    }
    float renderTargetWidth = 0.0f;
    float renderTargetHeight = 0.0f;
    float renderTargetAspect = 0.0f;
    if (auto* rtv = m_context->m_state.om.renderTargetViews[0].ptr()) {
      Rc<DxvkImageView> rtvView = rtv->GetImageView();
      if (rtvView != nullptr) {
        const VkExtent3D targetExtent = rtvView->image()->info().extent;
        if (targetExtent.width > 0 && targetExtent.height > 0) {
          renderTargetWidth = float(targetExtent.width);
          renderTargetHeight = float(targetExtent.height);
          renderTargetAspect = renderTargetWidth / renderTargetHeight;
        }
      }
    }
    float clientAspect = 0.0f;
    if (m_lastClientExtent.width > 0u && m_lastClientExtent.height > 0u)
      clientAspect = float(m_lastClientExtent.width) / float(m_lastClientExtent.height);
    float outputAspect = 0.0f;
    if (m_lastOutputExtent.width > 0u && m_lastOutputExtent.height > 0u)
      outputAspect = float(m_lastOutputExtent.width) / float(m_lastOutputExtent.height);
    const float projectionReferenceAspect = viewportAspect > 0.0f
      ? viewportAspect
      : (renderTargetAspect > 0.0f
        ? renderTargetAspect
        : (outputAspect > 0.0f
          ? outputAspect
          : clientAspect));
    const float fallbackReferenceAspect = viewportAspect > 0.0f
      ? viewportAspect
      : (renderTargetAspect > 0.0f
        ? renderTargetAspect
        : (outputAspect > 0.0f
          ? outputAspect
          : clientAspect));

    // Score a perspective projection: higher = more likely main game camera.
    // Shadow maps have square aspect, cubemaps have 90° FOV, tool cameras
    // have extreme FOV — all score lower than a typical game camera.
    auto scorePerspective = [projectionReferenceAspect](const Matrix4& proj) -> float {
      const Matrix4 scoredProj = canonicalizeProjectionOrientation(proj);
      float score = 1.0f;
      DecomposeProjectionParams dpp;
      decomposeProjection(scoredProj, dpp);
      // Guard against degenerate decomposition (NaN/Inf from near-singular matrices).
      if (!std::isfinite(dpp.fov) || !std::isfinite(dpp.aspectRatio) || !std::isfinite(dpp.nearPlane))
        return score;
      float fovDeg = dpp.fov * (180.0f / 3.14159265f);
      if (fovDeg >= 30.0f && fovDeg <= 120.0f)
        score += 2.0f;
      else if (fovDeg >= 15.0f && fovDeg <= 150.0f)
        score += 1.0f;
      if (projectionReferenceAspect > 0.0f) {
        float diff = std::abs(std::abs(dpp.aspectRatio) - projectionReferenceAspect);
        if (diff < 0.15f)
          score += 2.0f;
        else if (diff < 0.5f)
          score += 1.0f;
      }
      if (dpp.nearPlane > 0.001f && dpp.nearPlane < 100.0f)
        score += 1.0f;
      if (proj[0][0] < 0.0f)
        score -= 0.5f;
      if (proj[1][1] < 0.0f)
        score -= 1.0f;
      return score;
    };

    // All shader stages to scan for camera matrices.
    // VS is most common; emulators (Dolphin, PCSX2, Xenia, Citra) and some
    // deferred renderers put camera matrices in GS, DS, or PS cbuffers.
    const D3D11ConstantBufferBindings* stageCbs[] = {
      &m_context->m_state.vs.constantBuffers,
      &m_context->m_state.gs.constantBuffers,
      &m_context->m_state.ds.constantBuffers,
      &m_context->m_state.ps.constantBuffers,
    };
    static constexpr int kNumStages = 4;
    static const char* kStageNames[] = { "VS", "GS", "DS", "PS" };

    // Scan one stage's cbuffers for the best-scoring perspective matrix.
    // classifyPerspective detects both row-major and column-major-as-row
    // layouts in a single pass, so no separate transpose pass is needed.
    auto scanStageForProj = [&](int stageIdx,
        uint32_t& outSlot, size_t& outOff, float& outScore,
        Matrix4& outMat, bool& outColMajor) -> bool
    {
      bool found = false;
      const auto& cbs = *stageCbs[stageIdx];
      for (uint32_t slot = 0; slot < D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT; ++slot) {
        const auto& cb = cbs[slot];
        if (cb.buffer == nullptr) continue;
        const auto mapped = cb.buffer->GetMappedSlice();
        const uint8_t* ptr = reinterpret_cast<const uint8_t*>(mapped.mapPtr);
        if (!ptr) continue;
        const size_t bufSize = cb.buffer->Desc()->ByteWidth;
        auto [base, end] = cbRange(cb);
        for (size_t off = base; off + 64 <= end; off += 16) {
          Matrix4 m = readCbMatrix(ptr, off, bufSize);
          int cls = classifyPerspective(m);
          if (cls == 0) continue;
          // Column-major-as-row (cls==2): transpose to row-major for scoring/use.
          const bool isCol = (cls == 2);
          Matrix4 normalized = isCol ? transpose(m) : m;
          float s = scorePerspective(normalized);
          if (s > outScore) {
            outSlot     = slot;
            outOff      = off;
            outScore    = s;
            outMat      = normalized;
            outColMajor = isCol;
            found       = true;
          }
        }
      }
      return found;
    };

    uint32_t projSlot   = m_projSlot;
    size_t   projOffset = m_projOffset;
    int      projStage  = m_projStage;

    // --- PROJECTION: first-draw scan (cache miss) ---
    // Single pass across all stages — classifyPerspective handles both layouts.
    if (projSlot == UINT32_MAX) {
      float bestScore = 0.0f;
      Matrix4 bestMat;
      uint32_t bestSlot = UINT32_MAX;
      size_t bestOff = SIZE_MAX;
      int bestStage = -1;
      bool bestCol = false;

      for (int si = 0; si < kNumStages; ++si) {
        uint32_t ts = UINT32_MAX; size_t to = SIZE_MAX;
        float tsc = bestScore; Matrix4 tm; bool tc = false;
        if (scanStageForProj(si, ts, to, tsc, tm, tc) && tsc > bestScore) {
          bestScore = tsc;
          bestSlot = ts; bestOff = to; bestStage = si; bestMat = tm;
          bestCol = tc;
        }
      }

      if (bestSlot != UINT32_MAX) {
        projSlot   = bestSlot;
        projOffset = bestOff;
        projStage  = bestStage;
        m_projSlot   = bestSlot;
        m_projOffset = bestOff;
        m_projStage  = bestStage;
        m_columnMajor = bestCol;
      }
    }

    // --- PROJECTION: validate cached location, re-scan on stale ---
    if (projSlot != UINT32_MAX && projStage >= 0 && projStage < kNumStages) {
      const auto& cbs = *stageCbs[projStage];
      const auto& cb = cbs[projSlot];
      Matrix4 proj;
      bool valid = false;
      if (cb.buffer != nullptr) {
        const auto mapped = cb.buffer->GetMappedSlice();
        const uint8_t* ptr = reinterpret_cast<const uint8_t*>(mapped.mapPtr);
        if (ptr) {
          Matrix4 raw = readCbMatrix(ptr, projOffset, cb.buffer->Desc()->ByteWidth);
          int cls = classifyPerspective(raw);
          if (cls > 0) {
            proj = (cls == 2) ? transpose(raw) : raw;
            valid = true;
          }
        }
      }

      if (!valid && projSlot == m_projSlot && projStage == m_projStage) {
        // Cached location is stale (different pass). Re-scan all stages.
        projSlot = UINT32_MAX;
        float bestScore = 0.0f;
        for (int si = 0; si < kNumStages; ++si) {
          uint32_t ts = UINT32_MAX; size_t to = SIZE_MAX;
          float tsc = bestScore; Matrix4 tm; bool tc = false;
          if (scanStageForProj(si, ts, to, tsc, tm, tc)) {
            projSlot = ts; projOffset = to; projStage = si;
            proj = tm; bestScore = tsc;
          }
        }
      }

      if (projSlot != UINT32_MAX) {
        // Strip TAA jitter — Remix does its own TAA.
        proj[2][0] = 0.0f;
        proj[2][1] = 0.0f;

        bool flippedX = false;
        bool flippedY = false;
        proj = canonicalizeProjectionOrientation(proj, &flippedX, &flippedY);
        projectionWasFlippedY = flippedY;
        if (flippedX || flippedY) {
          static uint32_t sProjectionCanonicalizeLogCount = 0;
          if (sProjectionCanonicalizeLogCount < 8) {
            ++sProjectionCanonicalizeLogCount;
            Logger::info(str::format(
              "[D3D11Rtx] Canonicalized projection orientation:",
              flippedX ? " flipX" : "",
              flippedY ? " flipY" : "",
              " stage=",
              kStageNames[projStage],
              " slot=",
              projSlot,
              " off=",
              projOffset));
          }
        }

        transforms.viewToProjection = proj;
      }
    }

    // --- FALLBACK PROJECTION ---
    // If no perspective matrix was found in any cbuffer, synthesize one from
    // the viewport. This keeps path tracing viable for games, emulators, and
    // engines that never expose a clean projection cbuffer. Large scene
    // viewports are accepted even when letterboxed or offset; only tiny helper
    // and HUD-style viewports are rejected here.
    if (projSlot == UINT32_MAX) {
      const auto& vp = m_context->m_state.rs.viewports[0];
      if (vp.Width > 0.0f && vp.Height > 0.0f) {
        float targetWidth = vp.Width;
        float targetHeight = vp.Height;
        if (renderTargetWidth > 0.0f && renderTargetHeight > 0.0f) {
          targetWidth = renderTargetWidth;
          targetHeight = renderTargetHeight;
        } else if (m_lastOutputExtent.width > 0u && m_lastOutputExtent.height > 0u) {
          targetWidth = float(m_lastOutputExtent.width);
          targetHeight = float(m_lastOutputExtent.height);
        } else if (m_lastClientExtent.width > 0u && m_lastClientExtent.height > 0u) {
          targetWidth = float(m_lastClientExtent.width);
          targetHeight = float(m_lastClientExtent.height);
        }

        const float targetArea = std::max(targetWidth * targetHeight, 1.0f);
        const float viewportArea = vp.Width * vp.Height;
        const float coverage = std::min(viewportArea, targetArea) / std::max(viewportArea, targetArea);
        const float candidateAspect = vp.Width / vp.Height;
        const float viewportCenterX = vp.TopLeftX + vp.Width * 0.5f;
        const float viewportCenterY = vp.TopLeftY + vp.Height * 0.5f;
        const float targetCenterX = targetWidth * 0.5f;
        const float targetCenterY = targetHeight * 0.5f;
        const float normalizedCenterOffsetX = targetWidth > 0.0f
          ? std::abs(viewportCenterX - targetCenterX) / targetWidth
          : 0.0f;
        const float normalizedCenterOffsetY = targetHeight > 0.0f
          ? std::abs(viewportCenterY - targetCenterY) / targetHeight
          : 0.0f;
        const bool nearOrigin = std::abs(vp.TopLeftX) <= 4.0f && std::abs(vp.TopLeftY) <= 4.0f;
        const bool usableViewport = std::isfinite(vp.Width)
                                 && std::isfinite(vp.Height)
                                 && vp.Width >= 8.0f
                                 && vp.Height >= 8.0f;
        const bool plausibleSceneAspect = std::isfinite(candidateAspect)
                                       && candidateAspect >= 0.4f
                                       && candidateAspect <= 5.0f;
        const bool centeredViewport = normalizedCenterOffsetX <= 0.18f && normalizedCenterOffsetY <= 0.18f;
        const bool coversMostOfTarget = coverage >= 0.6f;
        const bool coversMeaningfulArea = coverage >= 0.2f;

        if (usableViewport && plausibleSceneAspect && (coversMostOfTarget || (coversMeaningfulArea && centeredViewport) || nearOrigin)) {
          const float aspect = candidateAspect > 0.0f ? candidateAspect : fallbackReferenceAspect;
          const float fovY   = 60.0f * (3.14159265f / 180.0f);
          const float nearZ  = 0.1f;
          const float farZ   = 10000.0f;
          const float yScale = 1.0f / std::tan(fovY * 0.5f);
          const float xScale = yScale / aspect;
          const float Q      = farZ / (farZ - nearZ);
          transforms.viewToProjection = Matrix4(
            Vector4(xScale, 0.0f,   0.0f,         0.0f),
            Vector4(0.0f,   yScale, 0.0f,         0.0f),
            Vector4(0.0f,   0.0f,   Q,            1.0f),
            Vector4(0.0f,   0.0f,  -nearZ * Q,    0.0f));
          transforms.usedViewportFallbackProjection = true;
          static bool s_fallbackLogged = false;
          if (!s_fallbackLogged) {
            s_fallbackLogged = true;
            Logger::info(str::format(
              "[D3D11Rtx] No projection found in cbuffers — using viewport fallback (",
              "x=", vp.TopLeftX,
              " y=", vp.TopLeftY,
              vp.Width, "x", vp.Height,
              " aspect=", aspect,
              " coverage=", coverage,
              " centered=", centeredViewport ? 1 : 0,
              " client=", m_lastClientExtent.width, "x", m_lastClientExtent.height,
              " output=", targetWidth, "x", targetHeight,
              ")"));
          }
        } else {
          static bool s_fallbackRejectedLogged = false;
          if (!s_fallbackRejectedLogged) {
            s_fallbackRejectedLogged = true;
            Logger::info(str::format(
              "[D3D11Rtx] No projection found in cbuffers — skipping viewport fallback for implausible scene viewport (",
              "x=", vp.TopLeftX,
              " y=", vp.TopLeftY,
              " w=", vp.Width,
              " h=", vp.Height,
              " coverage=", coverage,
              " centered=", centeredViewport ? 1 : 0,
              " client=", m_lastClientExtent.width, "x", m_lastClientExtent.height,
              " aspect=", candidateAspect,
              ")"));
          }
        }
      }
    }

    // --- VIEW MATRIX ---
    // Cached fast path: re-read from previously discovered location.
    // Only rescan when the cached location is invalid or doesn't contain
    // a view matrix anymore (shader change, different render pass).
    bool viewCacheHit = false;
    if (m_viewSlot != UINT32_MAX && m_viewStage >= 0 && m_viewStage < kNumStages) {
      const auto& cb = (*stageCbs[m_viewStage])[m_viewSlot];
      if (cb.buffer != nullptr) {
        const auto mapped = cb.buffer->GetMappedSlice();
        const uint8_t* ptr = reinterpret_cast<const uint8_t*>(mapped.mapPtr);
        if (ptr) {
          Matrix4 c = readMatrix(ptr, m_viewOffset, cb.buffer->Desc()->ByteWidth);
          Matrix4 resolvedView;
          if (resolveViewMatrixCandidate(c, resolvedView)) {
            transforms.worldToView = resolvedView;
            viewCacheHit = true;
          }
        }
      }
    }

    // Full scan fallback — same logic as before, but caches the result.
    if (!viewCacheHit && projSlot != UINT32_MAX) {
      if (projStage >= 0 && projStage < kNumStages) {
        const auto& cb = (*stageCbs[projStage])[projSlot];
        if (cb.buffer != nullptr) {
          const auto mapped = cb.buffer->GetMappedSlice();
          const uint8_t* ptr = reinterpret_cast<const uint8_t*>(mapped.mapPtr);
          if (ptr) {
            const size_t bufSize = cb.buffer->Desc()->ByteWidth;
            if (projOffset >= 64) {
              Matrix4 c = readMatrix(ptr, projOffset - 64, bufSize);
              Matrix4 resolvedView;
              if (resolveViewMatrixCandidate(c, resolvedView)) {
                transforms.worldToView = resolvedView;
                m_viewStage = projStage; m_viewSlot = projSlot; m_viewOffset = projOffset - 64;
              }
            }
            if (isIdentityExact(transforms.worldToView)) {
              auto [vBase, vEnd] = cbRange(cb);
              for (size_t off = vBase; off + 64 <= vEnd; off += 16) {
                if (off >= projOffset && off < projOffset + 64) continue;
                Matrix4 c = readMatrix(ptr, off, bufSize);
                Matrix4 resolvedView;
                if (resolveViewMatrixCandidate(c, resolvedView)) {
                  transforms.worldToView = resolvedView;
                  m_viewStage = projStage; m_viewSlot = projSlot; m_viewOffset = off;
                  break;
                }
              }
            }
          }
        }
      }

      // Cross-stage fallback: scan all stages' cbuffers for a view matrix.
      if (isIdentityExact(transforms.worldToView)) {
        for (int si = 0; si < kNumStages && isIdentityExact(transforms.worldToView); ++si) {
          const auto& cbs = *stageCbs[si];
          for (uint32_t slot = 0; slot < D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT; ++slot) {
            if (si == projStage && slot == projSlot) continue;
            const auto& cb = cbs[slot];
            if (cb.buffer == nullptr) continue;
            const auto mapped = cb.buffer->GetMappedSlice();
            const uint8_t* ptr = reinterpret_cast<const uint8_t*>(mapped.mapPtr);
            if (!ptr) continue;
            const size_t bufSize = cb.buffer->Desc()->ByteWidth;
            auto [csBase, csEnd] = cbRange(cb);
            for (size_t off = csBase; off + 64 <= csEnd; off += 16) {
              Matrix4 c = readMatrix(ptr, off, bufSize);
              Matrix4 resolvedView;
              if (resolveViewMatrixCandidate(c, resolvedView)) {
                transforms.worldToView = resolvedView;
                m_viewStage = si; m_viewSlot = slot; m_viewOffset = off;
                break;
              }
            }
            if (!isIdentityExact(transforms.worldToView)) break;
          }
        }
      }

      // Convention fallback: if no view matrix was found, the column-major
      // detection may be wrong (ambiguous when near plane ≈ 1). Retry with
      // the opposite convention, but only for the projection cbuffer.
      if (isIdentityExact(transforms.worldToView) && projStage >= 0 && projStage < kNumStages) {
        const auto& cb = (*stageCbs[projStage])[projSlot];
        if (cb.buffer != nullptr) {
          const auto mapped = cb.buffer->GetMappedSlice();
          const uint8_t* ptr = reinterpret_cast<const uint8_t*>(mapped.mapPtr);
          if (ptr) {
            const size_t bufSize = cb.buffer->Desc()->ByteWidth;
            auto [fbBase, fbEnd] = cbRange(cb);
            for (size_t off = fbBase; off + 64 <= fbEnd; off += 16) {
              if (off >= projOffset && off < projOffset + 64) continue;
              Matrix4 raw = readCbMatrix(ptr, off, bufSize);
              Matrix4 flipped = m_columnMajor ? raw : transpose(raw);
              Matrix4 resolvedView;
              if (resolveViewMatrixCandidate(flipped, resolvedView)) {
                transforms.worldToView = resolvedView;
                m_viewStage = projStage; m_viewSlot = projSlot; m_viewOffset = off;
                m_columnMajor = !m_columnMajor;
                break;
              }
            }
          }
        }
      }
    }

    // When using fallback projection (projSlot == UINT32_MAX), still search
    // all stages for a view matrix so the camera position is correct.
    if (!viewCacheHit && projSlot == UINT32_MAX && isIdentityExact(transforms.worldToView)) {
      for (int si = 0; si < kNumStages && isIdentityExact(transforms.worldToView); ++si) {
        const auto& cbs = *stageCbs[si];
        for (uint32_t slot = 0; slot < D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT; ++slot) {
          const auto& cb = cbs[slot];
          if (cb.buffer == nullptr) continue;
          const auto mapped = cb.buffer->GetMappedSlice();
          const uint8_t* ptr = reinterpret_cast<const uint8_t*>(mapped.mapPtr);
          if (!ptr) continue;
          const size_t bufSize = cb.buffer->Desc()->ByteWidth;
          auto [csBase, csEnd] = cbRange(cb);
          for (size_t off = csBase; off + 64 <= csEnd; off += 16) {
            Matrix4 c = readMatrix(ptr, off, bufSize);
            Matrix4 resolvedView;
            if (resolveViewMatrixCandidate(c, resolvedView)) {
              transforms.worldToView = resolvedView;
              m_viewStage = si; m_viewSlot = slot; m_viewOffset = off;
              break;
            }
          }
          if (!isIdentityExact(transforms.worldToView)) break;
        }
      }
    }

    // --- VIEW MATRIX: ViewProj decomposition fallback ---
    // Many engines store a pre-multiplied ViewProj (= View * Proj) instead
    // of separate View and Projection matrices.  When we found a valid P but
    // no standalone view matrix, check: for each matrix M in cbuffers, does
    //   V_candidate = M * inverse(P)
    // yield a valid view?  If so, M is ViewProj and V_candidate is our view.
    if (isIdentityExact(transforms.worldToView) && projSlot != UINT32_MAX) {
      Matrix4 projInv = inverse(transforms.viewToProjection);
      // Sanity: inverse succeeded (non-degenerate projection).
      bool invOk = std::isfinite(projInv[0][0]) && std::isfinite(projInv[1][1])
                && std::isfinite(projInv[2][2]) && std::isfinite(projInv[3][3]);
      if (invOk) {
        for (int si = 0; si < kNumStages && isIdentityExact(transforms.worldToView); ++si) {
          const auto& cbs = *stageCbs[si];
          for (uint32_t slot = 0; slot < D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT; ++slot) {
            const auto& cb = cbs[slot];
            if (cb.buffer == nullptr) continue;
            const auto mapped = cb.buffer->GetMappedSlice();
            const uint8_t* ptr = reinterpret_cast<const uint8_t*>(mapped.mapPtr);
            if (!ptr) continue;
            const size_t bufSize = cb.buffer->Desc()->ByteWidth;
            auto [csBase, csEnd] = cbRange(cb);
            for (size_t off = csBase; off + 64 <= csEnd; off += 16) {
              if (si == projStage && slot == projSlot && off == projOffset) continue;
              Matrix4 M = readMatrix(ptr, off, bufSize);
              if (isIdentityExact(M)) continue;
              Matrix4 V = M * projInv;
              Matrix4 resolvedView;
              if (resolveViewMatrixCandidate(V, resolvedView)) {
                transforms.worldToView = resolvedView;
                m_viewStage = si; m_viewSlot = slot; m_viewOffset = off;
                static bool s_vpLogged = false;
                if (!s_vpLogged) {
                  s_vpLogged = true;
                  Logger::info(str::format(
                    "[D3D11Rtx] View derived from ViewProj decomposition: stage=",
                    kStageNames[si], " slot=", slot, " off=", off));
                }
                break;
              }
            }
            if (!isIdentityExact(transforms.worldToView)) break;
          }
        }
      }
    }

    // --- AXIS AUTO-DETECTION (camera-backed projection-derived) ---
    // Only learn handedness/Y-flip from draws where we recovered both a
    // plausible projection and a plausible view matrix. This avoids locking
    // the session to helper, shadow, or other non-scene projections.
    if (projSlot != UINT32_MAX && !isIdentityExact(transforms.worldToView)) {
      const bool canVote = !m_yFlipSettled || !m_lhSettled;

      if (canVote) {
        m_axisDetected = true;

        const Matrix4& projection = transforms.viewToProjection;

        m_yFlipVotes += projectionWasFlippedY ? 1 : -1;
        if (!m_yFlipSettled && std::abs(m_yFlipVotes) >= kVoteThreshold) {
          m_yFlipSettled = true;
          const bool yFlip = m_yFlipVotes > 0;
          RtCamera::correctProjectionYFlipObject().setDeferred(yFlip);
        }

        DecomposeProjectionParams dpp;
        decomposeProjection(projection, dpp);
        if (std::isfinite(dpp.fov) && std::isfinite(dpp.aspectRatio)) {
          bool hasExplicitHandedness = false;
          bool isLeftHanded = dpp.isLHS;

          if (std::abs(std::abs(projection[2][3]) - 1.0f) < 0.02f) {
            hasExplicitHandedness = true;
            isLeftHanded = projection[2][3] > 0.0f;
          } else if (std::abs(std::abs(projection[3][2]) - 1.0f) < 0.02f) {
            hasExplicitHandedness = true;
            isLeftHanded = projection[3][2] > 0.0f;
          }

          m_lhVotes += isLeftHanded ? 1 : -1;
          if (!m_lhSettled && std::abs(m_lhVotes) >= kVoteThreshold) {
            m_lhSettled = true;
            const bool isLH = m_lhVotes > 0;
            RtxOptions::leftHandedCoordinateSystemObject().setDeferred(isLH);

            static uint32_t sHandednessLogCount = 0;
            if (hasExplicitHandedness && sHandednessLogCount < 4) {
              ++sHandednessLogCount;
              Logger::info(str::format(
                "[D3D11Rtx] Handedness vote from projection structure: ",
                isLH ? "LH" : "RH",
                " m23=",
                projection[2][3],
                " m32=",
                projection[3][2]));
            }
          }
        }
      }
    }

    // --- Z-UP / Y-UP AUTO-DETECTION (view-matrix-derived) ---
    // In a Y-up world, the view matrix "up" column (col 1) has its largest
    // component in row 1 (Y). In a Z-up world, column 1's largest component
    // is in row 2 (Z). Vote on each valid view matrix and settle via threshold.
    if (!isIdentityExact(transforms.worldToView)) {
      if (!m_zUpSettled) {
        const float absY = std::abs(transforms.worldToView[1][1]);
        const float absZ = std::abs(transforms.worldToView[2][1]);
        // Only vote when there's a clear winner (avoid ambiguous 45° views)
        if (std::abs(absZ - absY) > 0.3f) {
          m_zUpVotes += (absZ > absY) ? 1 : -1;
          if (!m_zUpSettled && std::abs(m_zUpVotes) >= kVoteThreshold) {
            m_zUpSettled = true;
            const bool zUp = m_zUpVotes > 0;
            RtxOptions::zUpObject().setDeferred(zUp);
          }
        }
      }

      // Log settled axis conventions once.
      if (m_zUpSettled && m_yFlipSettled && m_lhSettled && !m_axisLogged) {
        m_axisLogged = true;
        Logger::info(str::format("[D3D11Rtx] Axis detection settled: ",
          m_lhVotes > 0 ? "LH" : "RH",
          m_yFlipVotes > 0 ? " Y-flipped" : "",
          m_zUpVotes > 0 ? " Z-up" : " Y-up",
          m_columnMajor ? " col-major" : " row-major",
          " (proj stage=", kStageNames[std::max(0, m_projStage)],
          " slot=", m_projSlot, " off=", m_projOffset, ")"));
      }
    }

    // --- CAMERA POSITION SMOOTHING ---
    // The view matrix encodes camera position in its translation row (row 3).
    // Floating-point rounding in cbuffer reads causes sub-pixel jitter between
    // draws/frames. Apply exponential moving average on the position to dampen
    // this without introducing visible lag. The rotation (upper 3x3) is left
    // untouched — rotation jitter is rare and smoothing it causes ghosting.
    //
    // D3D row-major view matrix layout:
    //   [R00 R01 R02  0]    pos = -R^T * t
    //   [R10 R11 R12  0]    where t = (V[3][0], V[3][1], V[3][2])
    //   [R20 R21 R22  0]
    //   [tx  ty  tz   1]
    if (!isIdentityExact(transforms.worldToView)) {
      const auto& V = transforms.worldToView;
      // Camera world position: pos = -R^T * t for view matrix V = [R | 0; t | 1]
      Vector3 t(V[3][0], V[3][1], V[3][2]);
      Vector3 camPos(
        -(V[0][0] * t.x + V[1][0] * t.y + V[2][0] * t.z),
        -(V[0][1] * t.x + V[1][1] * t.y + V[2][1] * t.z),
        -(V[0][2] * t.x + V[1][2] * t.y + V[2][2] * t.z));

      constexpr float kSmoothAlpha = 0.8f; // 0 = full smooth (laggy), 1 = no smooth (jittery)
      constexpr float kTeleportThreshold = 5.0f; // snap on large jumps (cutscene, teleport)

      if (m_hasPrevCamPos) {
        Vector3 delta = camPos - m_smoothedCamPos;
        float distSq = delta.x * delta.x + delta.y * delta.y + delta.z * delta.z;
        if (distSq < kTeleportThreshold * kTeleportThreshold) {
          m_smoothedCamPos = Vector3(
            m_smoothedCamPos.x + kSmoothAlpha * (camPos.x - m_smoothedCamPos.x),
            m_smoothedCamPos.y + kSmoothAlpha * (camPos.y - m_smoothedCamPos.y),
            m_smoothedCamPos.z + kSmoothAlpha * (camPos.z - m_smoothedCamPos.z));
        } else {
          m_smoothedCamPos = camPos;
        }
      } else {
        m_smoothedCamPos = camPos;
        m_hasPrevCamPos = true;
      }

      // Reconstruct translation row from smoothed position: t = -R * smoothPos
      transforms.worldToView[3][0] = -(V[0][0] * m_smoothedCamPos.x + V[0][1] * m_smoothedCamPos.y + V[0][2] * m_smoothedCamPos.z);
      transforms.worldToView[3][1] = -(V[1][0] * m_smoothedCamPos.x + V[1][1] * m_smoothedCamPos.y + V[1][2] * m_smoothedCamPos.z);
      transforms.worldToView[3][2] = -(V[2][0] * m_smoothedCamPos.x + V[2][1] * m_smoothedCamPos.y + V[2][2] * m_smoothedCamPos.z);
    }

    // --- WORLD MATRIX ---
    // Object-to-world transform, changes every draw call but usually lives
    // at a fixed (stage, slot, offset) within the same shader program.
    // Unlike the old code that only read offset 0, we scan the full cbuffer
    // to handle engines that pack [View|Proj|World] in a single CB.
    //
    // Candidate filter: affine, non-identity, not perspective, not the
    // already-identified view or projection, reasonable scale factors.
    // We compare against the found view by position (stage/slot/offset),
    // NOT by structural isViewMatrix() — the latter rejects unit-scale
    // world matrices which are the majority of game transforms.
    if (RtxOptions::useCBufferWorldMatrices()) {
      auto isWorldCandidate = [&](const Matrix4& m) -> bool {
        if (isIdentityExact(m)) return false;
        if (classifyPerspective(m) != 0) return false;
        for (int row = 0; row < 4; ++row) {
          for (int col = 0; col < 4; ++col) {
            if (!std::isfinite(m[row][col])) return false;
          }
        }
        // Affine: last column = [0, 0, 0, 1]
        if (std::abs(m[3][3] - 1.0f) > 0.01f) return false;
        if (std::abs(m[0][3]) > 0.01f || std::abs(m[1][3]) > 0.01f || std::abs(m[2][3]) > 0.01f)
          return false;
        // Reasonable scale: each column's squared length in [0.0001, 1e6]
        Vector3 normalizedAxes[3];
        for (int col = 0; col < 3; ++col) {
          float lenSq = m[0][col] * m[0][col] + m[1][col] * m[1][col] + m[2][col] * m[2][col];
          if (lenSq < 0.0001f || lenSq > 1e6f) return false;
          const float invLen = 1.0f / std::sqrt(lenSq);
          normalizedAxes[col] = Vector3(m[0][col] * invLen, m[1][col] * invLen, m[2][col] * invLen);
        }

        // World matrices are usually rotation * scale + translation. Reject heavily
        // sheared affine matrices so we don't accidentally pick unrelated cbuffer data.
        if (std::abs(dot(normalizedAxes[0], normalizedAxes[1])) > 0.35f
         || std::abs(dot(normalizedAxes[0], normalizedAxes[2])) > 0.35f
         || std::abs(dot(normalizedAxes[1], normalizedAxes[2])) > 0.35f) {
          return false;
        }

        return true;
      };

      auto isAffineObjectTransform = [&](const Matrix4& m) -> bool {
        if (isIdentityExact(m)) return false;
        if (classifyPerspective(m) != 0) return false;
        for (int row = 0; row < 4; ++row) {
          for (int col = 0; col < 4; ++col) {
            if (!std::isfinite(m[row][col])) return false;
          }
        }
        if (std::abs(m[3][3] - 1.0f) > 0.01f) return false;
        if (std::abs(m[0][3]) > 0.01f || std::abs(m[1][3]) > 0.01f || std::abs(m[2][3]) > 0.01f)
          return false;
        return true;
      };

      auto scoreWorldCandidate = [&](int stageIdx, uint32_t slot, size_t off, const Matrix4& candidate) -> float {
        float score = 0.0f;

        if (stageIdx == 0)
          score += 2.0f;
        if (stageIdx == projStage)
          score += 2.0f;
        if (slot == projSlot)
          score += 1.0f;
        if (projStage == 0 && projSlot != UINT32_MAX && slot == projSlot + 1)
          score += 4.0f;
        if (stageIdx == m_worldStage && slot == m_worldSlot && off == m_worldOffset)
          score += 3.0f;

        if (projOffset != SIZE_MAX) {
          const size_t distance = off > projOffset ? off - projOffset : projOffset - off;
          if (distance <= 128)
            score += 1.0f;
        }

        if (!isIdentityExact(transforms.worldToView)) {
          Matrix4 candidateObjectToView = transforms.worldToView * candidate;
          if (isAffineObjectTransform(candidateObjectToView))
            score += 2.0f;
        }

        const Vector3 translation(candidate[3][0], candidate[3][1], candidate[3][2]);
        const float translationLenSq = dot(translation, translation);
        if (translationLenSq > 1e-6f)
          score += 0.5f;

        return score;
      };

      bool found = false;
      float bestRawWorldScore = -1.0e30f;
      Matrix4 bestRawWorldCandidate;
      int bestRawWorldStage = -1;
      uint32_t bestRawWorldSlot = UINT32_MAX;
      size_t bestRawWorldOffset = SIZE_MAX;

      auto considerRawWorldCandidate = [&](int stageIdx, uint32_t slot, size_t off, const Matrix4& candidate) {
        const float score = scoreWorldCandidate(stageIdx, slot, off, candidate);
        if (score > bestRawWorldScore) {
          bestRawWorldScore = score;
          bestRawWorldCandidate = candidate;
          bestRawWorldStage = stageIdx;
          bestRawWorldSlot = slot;
          bestRawWorldOffset = off;
        }
      };

      auto tryWorldAt = [&](int stageIdx, uint32_t slot, size_t offset) -> bool {
        if (stageIdx < 0 || stageIdx >= kNumStages) return false;
        if (slot >= D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT) return false;
        const auto& cb = (*stageCbs[stageIdx])[slot];
        if (cb.buffer == nullptr) return false;
        const auto mapped = cb.buffer->GetMappedSlice();
        const uint8_t* ptr = reinterpret_cast<const uint8_t*>(mapped.mapPtr);
        if (!ptr || offset + 64 > cb.buffer->Desc()->ByteWidth) return false;
        Matrix4 candidate = readMatrix(ptr, offset, cb.buffer->Desc()->ByteWidth);
        if (!isWorldCandidate(candidate)) return false;
        considerRawWorldCandidate(stageIdx, slot, offset, candidate);
        return true;
      };

      auto scanWorldCb = [&](int stageIdx, uint32_t slot) -> bool {
        if (slot >= D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT) return false;
        const auto& cb = (*stageCbs[stageIdx])[slot];
        if (cb.buffer == nullptr) return false;
        const auto mapped = cb.buffer->GetMappedSlice();
        const uint8_t* ptr = reinterpret_cast<const uint8_t*>(mapped.mapPtr);
        if (!ptr) return false;
        const size_t bufSize = cb.buffer->Desc()->ByteWidth;
        auto [scanBase, scanEnd] = cbRange(cb);
        bool sawCandidate = false;
        for (size_t off = scanBase; off + 64 <= scanEnd; off += 16) {
          if (stageIdx == projStage && slot == projSlot && off == projOffset) continue;
          if (stageIdx == m_viewStage && slot == m_viewSlot && off == m_viewOffset) continue;
          Matrix4 candidate = readMatrix(ptr, off, bufSize);
          if (!isWorldCandidate(candidate)) continue;
          considerRawWorldCandidate(stageIdx, slot, off, candidate);
          sawCandidate = true;
        }
        return sawCandidate;
      };

      bool cachedWorldHit = false;
      if (m_worldSlot != UINT32_MAX && m_worldOffset != SIZE_MAX) {
        cachedWorldHit = tryWorldAt(m_worldStage, m_worldSlot, m_worldOffset);
        if (cachedWorldHit && bestRawWorldSlot != UINT32_MAX) {
          transforms.objectToWorld = bestRawWorldCandidate;
          found = true;
        }
      }

      if (!cachedWorldHit) {
        // Prefer commonly used locations first, but do not stop there.
        if (projSlot != UINT32_MAX && projStage >= 0)
          scanWorldCb(projStage, projSlot);

        if (projSlot != UINT32_MAX && projStage == 0
            && projSlot + 1 < D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT)
          scanWorldCb(0, projSlot + 1);

        float bestDerivedWorldScore = -1.0e30f;
        Matrix4 bestDerivedWorldCandidate;
        int bestDerivedWorldStage = -1;
        uint32_t bestDerivedWorldSlot = UINT32_MAX;
        size_t bestDerivedWorldOffset = SIZE_MAX;

        // Some engines provide object-to-view (model-view) matrices but no standalone
        // world matrix. Recover objectToWorld by stripping the current view transform.
        if (!isIdentityExact(transforms.worldToView)) {
          Matrix4 viewInv = inverse(transforms.worldToView);
          bool invOk = true;
          for (int row = 0; row < 4 && invOk; ++row) {
            for (int col = 0; col < 4; ++col) {
              if (!std::isfinite(viewInv[row][col])) {
                invOk = false;
                break;
              }
            }
          }

          if (invOk) {
            for (int si = 0; si < kNumStages; ++si) {
              const auto& cbs = *stageCbs[si];
              for (uint32_t slot = 0; slot < D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT; ++slot) {
                const auto& cb = cbs[slot];
                if (cb.buffer == nullptr) continue;
                const auto mapped = cb.buffer->GetMappedSlice();
                const uint8_t* ptr = reinterpret_cast<const uint8_t*>(mapped.mapPtr);
                if (!ptr) continue;
                const size_t bufSize = cb.buffer->Desc()->ByteWidth;
                auto [scanBase, scanEnd] = cbRange(cb);
                for (size_t off = scanBase; off + 64 <= scanEnd; off += 16) {
                  if (si == projStage && slot == projSlot && off == projOffset) continue;
                  if (si == m_viewStage && slot == m_viewSlot && off == m_viewOffset) continue;

                  Matrix4 candidateObjectToView = readMatrix(ptr, off, bufSize);
                  if (!isAffineObjectTransform(candidateObjectToView)) continue;

                  Matrix4 candidateObjectToWorld = viewInv * candidateObjectToView;
                  if (!isWorldCandidate(candidateObjectToWorld)) continue;

                  const float score = scoreWorldCandidate(si, slot, off, candidateObjectToWorld) + 2.5f;
                  if (score > bestDerivedWorldScore) {
                    bestDerivedWorldScore = score;
                    bestDerivedWorldCandidate = candidateObjectToWorld;
                    bestDerivedWorldStage = si;
                    bestDerivedWorldSlot = slot;
                    bestDerivedWorldOffset = off;
                  }
                }
              }
            }
          }
        }

        // Full scan: all VS cbuffers, then other stages.
        for (uint32_t s = 0; s < D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT; ++s) {
          if (projStage == 0 && s == projSlot) continue;
          if (projStage == 0 && projSlot != UINT32_MAX && s == projSlot + 1) continue;
          scanWorldCb(0, s);
        }
        for (int si = 1; si < kNumStages; ++si) {
          for (uint32_t s = 0; s < D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT; ++s) {
            if (si == projStage && s == projSlot) continue;
            scanWorldCb(si, s);
          }
        }

        static bool s_worldLogged = false;
        if (bestDerivedWorldSlot != UINT32_MAX && bestDerivedWorldScore >= bestRawWorldScore) {
          transforms.objectToWorld = bestDerivedWorldCandidate;
          m_worldStage = bestDerivedWorldStage;
          m_worldSlot = bestDerivedWorldSlot;
          m_worldOffset = bestDerivedWorldOffset;
          found = true;

          static bool s_objectViewLogged = false;
          if (!s_objectViewLogged) {
            s_objectViewLogged = true;
            Logger::info(str::format("[D3D11Rtx] World matrix derived from object-to-view: stage=",
              kStageNames[bestDerivedWorldStage], " slot=", bestDerivedWorldSlot, " off=", bestDerivedWorldOffset));
          }
        } else if (bestRawWorldSlot != UINT32_MAX) {
          transforms.objectToWorld = bestRawWorldCandidate;
          m_worldStage = bestRawWorldStage;
          m_worldSlot = bestRawWorldSlot;
          m_worldOffset = bestRawWorldOffset;
          found = true;

          if (!s_worldLogged) {
            s_worldLogged = true;
            Logger::info(str::format("[D3D11Rtx] World matrix found: stage=",
              kStageNames[m_worldStage], " slot=", m_worldSlot, " off=", m_worldOffset));
          }
        }
      }
    }

    transforms.objectToView = transforms.objectToWorld;
    if (!isIdentityExact(transforms.worldToView))
      transforms.objectToView = transforms.worldToView * transforms.objectToWorld;

    transforms.sanitize();

    // Log camera discovery once.
    static bool s_cameraLogged = false;
    if (projSlot != UINT32_MAX && !s_cameraLogged) {
      s_cameraLogged = true;
      const auto& p = transforms.viewToProjection;
      const bool hasView  = !isIdentityExact(transforms.worldToView);
      const bool hasWorld = !isIdentityExact(transforms.objectToWorld);
      Logger::info(str::format(
        "[D3D11Rtx] Camera found: proj stage=", kStageNames[projStage],
        " slot=", projSlot, " off=", projOffset,
        " diag=(", p[0][0], ",", p[1][1], ",", p[2][2], ")",
        " m[2][3]=", p[2][3],
        m_columnMajor ? " [column-major]" : " [row-major]",
        " view=", hasView ? "yes" : "NO",
        " world=", hasWorld ? "yes" : "NO"));
    }

    return transforms;
  }

  Future<GeometryHashes> D3D11Rtx::ComputeGeometryHashes(
      const RasterGeometry& geo, uint32_t vertexCount,
      uint32_t hashStartVertex, uint32_t hashVertexCount) const {

    const void* posData = geo.positionBuffer.mapPtr(geo.positionBuffer.offsetFromSlice());
    const void* tcData  = geo.texcoordBuffer.defined()
                        ? geo.texcoordBuffer.mapPtr(geo.texcoordBuffer.offsetFromSlice())
                        : nullptr;
    const void* idxData = geo.indexBuffer.defined() ? geo.indexBuffer.mapPtr(0) : nullptr;

    // D3D11 dynamic buffers can be discarded (Map WRITE_DISCARD) at any time,
    // which recycles the physical slice backing our raw pointers.  Pin each
    // buffer with incRef + acquire(Read) so the allocator won't reuse the
    // memory while the hash worker is reading it.  The lambda releases them.
    DxvkBuffer* posBuf = geo.positionBuffer.buffer().ptr();
    DxvkBuffer* tcBuf  = geo.texcoordBuffer.defined() ? geo.texcoordBuffer.buffer().ptr() : nullptr;
    DxvkBuffer* idxBuf = geo.indexBuffer.defined()    ? geo.indexBuffer.buffer().ptr()    : nullptr;

    if (posBuf) { posBuf->incRef(); posBuf->acquire(DxvkAccess::Read); }
    if (tcBuf)  { tcBuf->incRef();  tcBuf->acquire(DxvkAccess::Read);  }
    if (idxBuf) { idxBuf->incRef(); idxBuf->acquire(DxvkAccess::Read); }

    const uint32_t posStride = geo.positionBuffer.stride();
    const uint32_t tcStride  = geo.texcoordBuffer.defined() ? geo.texcoordBuffer.stride() : 0u;
    const uint32_t idxStride = geo.indexBuffer.defined()    ? geo.indexBuffer.stride()    : 0u;
    const uint32_t indexType = static_cast<uint32_t>(geo.indexBuffer.indexType());
    const uint32_t topology  = static_cast<uint32_t>(geo.topology);

    const uint32_t posOffset = geo.positionBuffer.offsetFromSlice();

    const XXH64_hash_t descHash   = hashGeometryDescriptor(geo.indexCount, vertexCount, indexType, topology);
    const XXH64_hash_t layoutHash = hashVertexLayout(geo);

    // Compute the safe byte range available for position and texcoord data.
    // Buffer pins guarantee the memory won't be recycled, but we must still
    // clamp to the actual buffer extent to avoid reading past the allocation.
    const size_t posLength = geo.positionBuffer.length();
    const size_t tcLength  = geo.texcoordBuffer.defined() ? geo.texcoordBuffer.length() : 0;
    const size_t idxLength = geo.indexBuffer.defined()    ? geo.indexBuffer.length()    : 0;

    auto future = m_pGeometryWorkers->Schedule([posData, tcData, idxData,
                                         posBuf, tcBuf, idxBuf,
                                         posStride, tcStride, idxStride,
                                         posLength, tcLength, idxLength,
                                         vertexCount, indexCount = geo.indexCount,
                                         posOffset,
                                         hashStartVertex, hashVertexCount,
                                         descHash, layoutHash]() -> GeometryHashes {
      GeometryHashes hashes;
      hashes[HashComponents::GeometryDescriptor] = descHash;
      hashes[HashComponents::VertexLayout]       = layoutHash;

      if (posData && posStride > 0) {
        // Hash only the drawn subrange [hashStartVertex, hashStartVertex + hashVertexCount).
        // Clamp to actual buffer length to prevent OOB reads on shared/dynamic VBs.
        const size_t startByte = static_cast<size_t>(hashStartVertex) * posStride;
        size_t posBytes = static_cast<size_t>(hashVertexCount) * posStride;
        if (startByte >= posLength) {
          posBytes = 0;
        } else if (startByte + posBytes > posLength) {
          posBytes = posLength - startByte;
        }
        if (posBytes > 0) {
          const auto* posBase = static_cast<const uint8_t*>(posData) + startByte;
          hashes[HashComponents::VertexPosition] =
            XXH3_64bits_withSeed(posBase, posBytes, static_cast<XXH64_hash_t>(hashStartVertex));
        } else {
          hashes[HashComponents::VertexPosition] =
            XXH3_64bits(&posOffset, sizeof(posOffset));
        }

        if (tcData && tcStride > 0) {
          const size_t tcStartByte = static_cast<size_t>(hashStartVertex) * tcStride;
          size_t tcBytes = static_cast<size_t>(hashVertexCount) * tcStride;
          if (tcStartByte >= tcLength) {
            tcBytes = 0;
          } else if (tcStartByte + tcBytes > tcLength) {
            tcBytes = tcLength - tcStartByte;
          }
          if (tcBytes > 0) {
            const auto* tcBase = static_cast<const uint8_t*>(tcData) + tcStartByte;
            // Use a more robust hash for texture coordinates
            // Include vertex count to ensure different geometries with same TC data hash differently
            XXH64_hash_t tcHash = XXH3_64bits(tcBase, tcBytes);
            tcHash = XXH3_64bits_withSeed(&hashStartVertex, sizeof(hashStartVertex), tcHash);
            tcHash = XXH3_64bits_withSeed(&vertexCount, sizeof(vertexCount), tcHash);
            hashes[HashComponents::VertexTexcoord] = tcHash;
          }
        }
        if (idxData && idxStride > 0) {
           const size_t idxBytes = static_cast<size_t>(std::min(indexCount, kMaxHashedIndices)) * idxStride;
          // Use a more robust hash for indices
          // Include vertex count to ensure different geometries with same index data hash differently
          XXH64_hash_t idxHash = hashContiguousMemory(idxData, std::min(idxBytes, idxLength));
          idxHash = XXH3_64bits_withSeed(&vertexCount, sizeof(vertexCount), idxHash);
          hashes[HashComponents::Indices] = idxHash;
        }
      } else {
        // GPU-only buffer: stable identity hash from buffer address and offset.
        // Use a more robust hash that includes buffer size information
        XXH64_hash_t posHash = XXH3_64bits(&posBuf, sizeof(posBuf));
        posHash = XXH3_64bits_withSeed(&posOffset, sizeof(posOffset), posHash);
        posHash = XXH3_64bits_withSeed(&vertexCount, sizeof(vertexCount), posHash);
        hashes[HashComponents::VertexPosition] = posHash;
      }

      hashes.precombine();

      // Release buffer pins — allow slice recycling again.
      if (posBuf) { posBuf->release(DxvkAccess::Read); posBuf->decRef(); }
      if (tcBuf)  { tcBuf->release(DxvkAccess::Read);  tcBuf->decRef();  }
      if (idxBuf) { idxBuf->release(DxvkAccess::Read); idxBuf->decRef(); }

      return hashes;
    });

    // If the worker queue was full, the lambda never runs — release pins now
    // to prevent a VRAM leak (incRef/acquire above would never be undone).
    if (!future.valid()) {
      if (posBuf) { posBuf->release(DxvkAccess::Read); posBuf->decRef(); }
      if (tcBuf)  { tcBuf->release(DxvkAccess::Read);  tcBuf->decRef();  }
      if (idxBuf) { idxBuf->release(DxvkAccess::Read); idxBuf->decRef(); }
    }

    return future;
  }

  void D3D11Rtx::FillMaterialData(LegacyMaterialData& mat) const {
    const auto& ps = m_context->m_state.ps;
    uint32_t textureID = 0;

    static uint32_t s_logCount = 0;
    const bool doLog = (s_logCount < 10);

    auto isBlockCompressed = [](DXGI_FORMAT fmt) -> bool {
      return (fmt >= DXGI_FORMAT_BC1_TYPELESS && fmt <= DXGI_FORMAT_BC1_UNORM_SRGB)
          || (fmt >= DXGI_FORMAT_BC2_TYPELESS && fmt <= DXGI_FORMAT_BC2_UNORM_SRGB)
          || (fmt >= DXGI_FORMAT_BC3_TYPELESS && fmt <= DXGI_FORMAT_BC3_UNORM_SRGB)
          || (fmt >= DXGI_FORMAT_BC4_TYPELESS && fmt <= DXGI_FORMAT_BC4_SNORM)
          || (fmt >= DXGI_FORMAT_BC5_TYPELESS && fmt <= DXGI_FORMAT_BC5_SNORM)
          || (fmt >= DXGI_FORMAT_BC6H_TYPELESS && fmt <= DXGI_FORMAT_BC7_UNORM_SRGB);
    };

    auto isLargeTexture = [](const VkExtent3D& extent) -> bool {
      return extent.width >= 512 || extent.height >= 512;
    };

    // Collect currently-bound render target images AND their dimensions.
    // Only reject SRVs that point to images actively bound as RTs.
    // VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT is set on most D3D11 textures
    // (engines create them with BIND_RENDER_TARGET for mip gen, dynamic
    // updates, etc.), so the flag alone is NOT a reliable RT indicator.
    const auto& omState = m_context->m_state.om;
    std::array<DxvkImage*, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT> boundRTImages = {};
    uint32_t rtWidth = 0, rtHeight = 0;
    for (uint32_t rt = 0; rt < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++rt) {
      auto* rtv = omState.renderTargetViews[rt].ptr();
      if (rtv) {
        Rc<DxvkImageView> rtvView = rtv->GetImageView();
        if (rtvView != nullptr) {
          boundRTImages[rt] = rtvView->image().ptr();
          if (rt == 0) {
            rtWidth  = rtvView->image()->info().extent.width;
            rtHeight = rtvView->image()->info().extent.height;
          }
        }
      }
    }

    // First pass: find the top-scoring texture candidates without heap allocation.
    // We only need kMaxSupportedTextures (2) winners — a full sort is unnecessary.
    static constexpr uint32_t kMaxPicks = LegacyMaterialData::kMaxSupportedTextures;
    struct TexPick {
      uint32_t slot = UINT32_MAX;
      Rc<DxvkImageView> view;
      int score = INT32_MIN;
      bool isCurrentRT = false;
      bool likelyIntermediate = false;
    };
    TexPick picks[kMaxPicks];
    uint32_t pickCount = 0;
    int worstPickScore = INT32_MIN;
    uint32_t worstPickIdx = 0;

    for (uint32_t slot = 0; slot < D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT; ++slot) {
      D3D11ShaderResourceView* srv = ps.shaderResources.views[slot].ptr();
      if (!srv) continue;
      if (srv->GetResourceType() != D3D11_RESOURCE_DIMENSION_TEXTURE2D) continue;

      Rc<DxvkImageView> view = srv->GetImageView();
      if (view == nullptr) continue;

      const auto& imgInfo = view->image()->info();
      D3D11_SHADER_RESOURCE_VIEW_DESC1 srvDesc = {};
      srv->GetDesc1(&srvDesc);
      const D3D11_COMMON_RESOURCE_DESC resourceDesc = srv->GetResourceDesc();
      const DXGI_FORMAT fmt = srvDesc.Format;
      const bool bc = isBlockCompressed(fmt);

      // Reject Texture2DArray SRVs that cover multiple slices — each slice is a separate
      // game texture that hashes identically, causing surfaces to appear "smashed together".
      // Single-slice array views are safe: getImageHash() mixes in the layer index.
      if (srvDesc.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2DARRAY
          && srvDesc.Texture2DArray.ArraySize > 1)
        continue;
      const bool hasMips = imgInfo.mipLevels > 1;
      const bool hasHazardBindFlags = srv->TestHazards() != FALSE;
      const bool isSingleMipLargeTexture = !hasMips && !bc && isLargeTexture(imgInfo.extent);
      const bool likelyIntermediate = hasHazardBindFlags && isSingleMipLargeTexture;

      DxvkImage* srvImage = view->image().ptr();
      bool isCurrentRT = false;
      for (uint32_t rt = 0; rt < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++rt) {
        if (boundRTImages[rt] == srvImage) { isCurrentRT = true; break; }
      }

      // Skip tiny dummy textures (1x1 default white/black).
      if (imgInfo.extent.width <= 2 && imgInfo.extent.height <= 2)
        continue;

      // Check if texture dimensions match current render target (likely GBuffer/intermediate).
      const bool matchesRT = (rtWidth > 0 && rtHeight > 0
        && imgInfo.extent.width == rtWidth && imgInfo.extent.height == rtHeight);

      int score = 0;
      if (bc)                       score += 10;  // Block-compressed = always content
      if (hasMips)                  score += 5;   // Mipmapped = likely content
      if (!matchesRT)               score += 3;   // Different size from RT = likely content
      if (!isCurrentRT)             score += 2;   // Not actively rendering to it
      score += std::max(0, 16 - (int)slot);       // Prefer lower slots (albedo first)

      // Global demotion for likely intermediate surfaces.
      // Hazard-capable resources are often postprocess, scene color, video, or other transient targets.
      // Many real material textures are BC-compressed and mipmapped, so only apply the strong penalty
      // when the texture also looks like a large single-mip intermediate.
      if (hasHazardBindFlags)
        score -= isSingleMipLargeTexture ? 16 : 6;

      // Large uncompressed single-mip textures are disproportionately likely to be transient scene/video data.
      if (isSingleMipLargeTexture)
        score -= 8;

      // Resources created primarily for RT/UAV work should lose to ordinary sampled textures whenever possible.
      if ((resourceDesc.BindFlags & (D3D11_BIND_RENDER_TARGET | D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_DEPTH_STENCIL)) != 0)
        score -= 4;

      // Currently bound as active RT → negative score (only use as absolute last resort)
      if (isCurrentRT) score = -10;

      // Sampler address mode: WRAP/MIRROR indicates a tiling world texture (strong positive signal).
      // CLAMP/BORDER indicates an atlas, render target, or postprocess input (negative signal).
      if (slot < D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT) {
        D3D11SamplerState* samp = ps.samplers[slot];
        if (samp != nullptr) {
          D3D11_SAMPLER_DESC sampDesc = {};
          samp->GetDesc(&sampDesc);
          const bool uWrap = (sampDesc.AddressU == D3D11_TEXTURE_ADDRESS_WRAP
                           || sampDesc.AddressU == D3D11_TEXTURE_ADDRESS_MIRROR);
          const bool vWrap = (sampDesc.AddressV == D3D11_TEXTURE_ADDRESS_WRAP
                           || sampDesc.AddressV == D3D11_TEXTURE_ADDRESS_MIRROR);
          if (uWrap && vWrap)       score += 4;   // Tiling = world geometry texture
          else if (!uWrap && !vWrap) score -= 2;  // Clamped = likely atlas/postprocess
          
          // Engine-specific sampler fixes for texture corruption
          // Some engines use non-standard sampler settings that cause texture corruption
          if (RtxOptions::enableUnrealTextureFixes()) {
            // Unreal Engine sometimes uses non-standard sampler settings
            // Apply additional filtering to prevent texture corruption
            if (sampDesc.Filter == D3D11_FILTER_MIN_MAG_MIP_LINEAR ||
                sampDesc.Filter == D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT) {
              // Unreal often uses linear filtering which can cause texture corruption
              // Apply additional checks to prevent corruption
              score += 2;  // Slight boost for Unreal textures
            }
          }
          
          if (RtxOptions::enableSource2Fixes()) {
            // Source 2 engine has specific sampler requirements
            // Apply fixes for Source 2 texture handling
            if (sampDesc.AddressU == D3D11_TEXTURE_ADDRESS_CLAMP ||
                sampDesc.AddressV == D3D11_TEXTURE_ADDRESS_CLAMP) {
              // Source 2 often uses clamp addressing
              score += 1;  // Slight boost for Source 2 textures
            }
          }
        }
      }

      if (doLog) {
        Logger::info(str::format("[D3D11Rtx] FillMaterialData tex candidate: slot=", slot,
          " fmt=", (uint32_t)fmt,
          " w=", imgInfo.extent.width, " h=", imgInfo.extent.height,
          " mips=", imgInfo.mipLevels,
          " score=", score,
          bc ? " [BC]" : "",
          hasMips ? " [MIPS]" : "",
          hasHazardBindFlags ? " [HAZARD]" : "",
          isSingleMipLargeTexture ? " [SINGLE-MIP-LARGE]" : "",
          likelyIntermediate ? " [LIKELY-INTERMEDIATE]" : "",
          isCurrentRT ? " [BOUND-RT]" : "",
          matchesRT ? " [RT-SIZED]" : ""));
      }

      // Insert into top-N picks (sorted descending by score, no heap alloc).
      if (pickCount < kMaxPicks) {
        picks[pickCount] = { slot, std::move(view), score, isCurrentRT, likelyIntermediate };
        ++pickCount;
        if (pickCount == kMaxPicks) {
          // Find worst to know which slot to evict next.
          worstPickScore = picks[0].score;
          worstPickIdx = 0;
          for (uint32_t p = 1; p < kMaxPicks; ++p) {
            if (picks[p].score < worstPickScore) {
              worstPickScore = picks[p].score;
              worstPickIdx = p;
            }
          }
        }
      } else if (score > worstPickScore) {
        picks[worstPickIdx] = { slot, std::move(view), score, isCurrentRT, likelyIntermediate };
        // Re-find worst.
        worstPickScore = picks[0].score;
        worstPickIdx = 0;
        for (uint32_t p = 1; p < kMaxPicks; ++p) {
          if (picks[p].score < worstPickScore) {
            worstPickScore = picks[p].score;
            worstPickIdx = p;
          }
        }
      }
    }

    // Sort the picks descending by score (at most kMaxPicks = 2 elements).
    if (pickCount == 2 && picks[0].score < picks[1].score)
      std::swap(picks[0], picks[1]);

    // Assign up to maxTextures picks, skipping active RTs if better options exist.
    const uint32_t maxTextures = RtxOptions::ignoreSecondaryTextures()
                                ? 1u : kMaxPicks;
    bool pickedAny = false;
    bool anyPositive = (pickCount > 0 && picks[0].score > 0);
    for (uint32_t p = 0; p < pickCount && textureID < maxTextures; ++p) {
      auto& c = picks[p];
      if (c.isCurrentRT && anyPositive)
        continue;

      mat.colorTextures[textureID] = TextureRef(std::move(c.view));
      mat.colorTextureSlot[textureID] = c.slot;

      if (c.slot < D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT) {
        D3D11SamplerState* samp = ps.samplers[c.slot];
        mat.samplers[textureID] = samp ? samp->GetDXVKSampler() : getDefaultSampler();
      } else {
        mat.samplers[textureID] = getDefaultSampler();
      }

      pickedAny = true;
      ++textureID;
    }

    // Last resort: pick the best candidate even if it's an active RT.
    if (!pickedAny && pickCount > 0) {
      auto& c = picks[0];
      // If the only remaining candidate still looks like a transient intermediate,
      // prefer leaving the material untextured over flooding the browser with garbage/video surfaces.
      if (!c.likelyIntermediate) {
        mat.colorTextures[0] = TextureRef(std::move(c.view));
        mat.colorTextureSlot[0] = c.slot;
        if (c.slot < D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT) {
          D3D11SamplerState* samp = ps.samplers[c.slot];
          mat.samplers[0] = samp ? samp->GetDXVKSampler() : getDefaultSampler();
        } else {
          mat.samplers[0] = getDefaultSampler();
        }
        textureID = 1;
      } else {
        mat.colorTextureSlot[0] = kInvalidResourceSlot;
      }
    }

    if (doLog && pickCount > 0) {
      Logger::info(str::format("[D3D11Rtx] FillMaterialData draw #", s_logCount,
        " picked ", textureID, " of ", pickCount, " candidate(s)"));
      ++s_logCount;
    }

    for (uint32_t textureIndex = 0; textureIndex < textureID; ++textureIndex) {
      const Rc<DxvkImageView> imageView = mat.colorTextures[textureIndex].getImageViewRc();
      const XXH64_hash_t textureHash = mat.colorTextures[textureIndex].getImageHash();
      if (imageView != nullptr && textureHash != 0) {
        ImGUI::AddTexture(textureHash, imageView, getTextureUiFeatureFlagsForView(imageView));
      }
    }

    // Material defaults for the Remix legacy material pipeline.
    // D3D11 bakes blending/alpha into immutable state objects — we extract
    // what we can from BlendState and DepthStencilState below.
    mat.textureColorArg1Source  = RtTextureArgSource::Texture;
    mat.textureColorArg2Source  = RtTextureArgSource::None;
    mat.textureColorOperation   = DxvkRtTextureOperation::Modulate;
    mat.textureAlphaArg1Source  = RtTextureArgSource::Texture;
    mat.textureAlphaArg2Source  = RtTextureArgSource::None;
    mat.textureAlphaOperation   = DxvkRtTextureOperation::SelectArg1;
    mat.tFactor                 = 0xFFFFFFFF;  // Opaque white
    mat.diffuseColorSource      = RtTextureArgSource::None;
    mat.specularColorSource     = RtTextureArgSource::None;

    // --- Blend state ---
    D3D11BlendState* blendState = m_context->m_state.om.cbState;
    if (blendState) {
      D3D11_BLEND_DESC1 blendDesc;
      blendState->GetDesc1(&blendDesc);
      const auto& rt0 = blendDesc.RenderTarget[0];

      mat.blendMode.enableBlending = rt0.BlendEnable;
      mat.blendMode.colorSrcFactor = mapD3D11Blend(rt0.SrcBlend, false);
      mat.blendMode.colorDstFactor = mapD3D11Blend(rt0.DestBlend, false);
      mat.blendMode.colorBlendOp   = mapD3D11BlendOp(rt0.BlendOp);
      mat.blendMode.alphaSrcFactor = mapD3D11Blend(rt0.SrcBlendAlpha, true);
      mat.blendMode.alphaDstFactor = mapD3D11Blend(rt0.DestBlendAlpha, true);
      mat.blendMode.alphaBlendOp   = mapD3D11BlendOp(rt0.BlendOpAlpha);
      mat.blendMode.writeMask      = rt0.RenderTargetWriteMask;

      // AlphaToCoverage = D3D11's cutout transparency (foliage, fences, hair).
      if (blendDesc.AlphaToCoverageEnable) {
        mat.alphaTestEnabled       = true;
        mat.alphaTestCompareOp     = VK_COMPARE_OP_GREATER;
        mat.alphaTestReferenceValue = 128;
      }
    }

    // --- Alpha test from depth-stencil state ---
    // Some engines use stencil ops to simulate alpha test; detect write-mask-zero
    // with stencil as a proxy for "discard if alpha < ref".
    D3D11DepthStencilState* dsState = m_context->m_state.om.dsState;
    if (dsState && !mat.alphaTestEnabled) {
      D3D11_DEPTH_STENCIL_DESC dsDesc;
      dsState->GetDesc(&dsDesc);
      if (dsDesc.StencilEnable && dsDesc.FrontFace.StencilFunc == D3D11_COMPARISON_LESS) {
        mat.alphaTestEnabled        = true;
        mat.alphaTestCompareOp      = VK_COMPARE_OP_GREATER;
        mat.alphaTestReferenceValue  = dsDesc.StencilReadMask;
      }
    }

    mat.updateCachedHash();
  }

  void D3D11Rtx::SubmitDraw(bool indexed,
                             UINT count,
                             UINT start,
                             INT  base,
                             const Matrix4* instanceTransform) {
    if (m_pGeometryWorkers == nullptr) {
      const bool isDeferredContext = m_context->GetType() == D3D11_DEVICE_CONTEXT_DEFERRED;
      const uint32_t cores = std::max(2u, std::thread::hardware_concurrency());
      const uint32_t workers = isDeferredContext
        ? 1u
        : std::min(std::max(cores / 2, 2u), 6u);
      m_pGeometryWorkers = std::make_unique<GeometryProcessor>(workers,
        isDeferredContext ? "d3d11-deferred-geometry" : "d3d11-geometry");

      if (isDeferredContext) {
        static uint32_t s_deferredGeometryInitLogCount = 0;
        if (s_deferredGeometryInitLogCount < 8) {
          ++s_deferredGeometryInitLogCount;
          Logger::info(str::format("[D3D11Rtx] Enabled deferred-context RTX submission with ", workers, " worker(s)"));
        }
      }
    }

    ++m_submitRejectStats.total;

    // Throttle: don't exceed the worker ring buffer capacity.
    // Beyond this point new futures would overwrite in-flight ones → corrupt hashes.
    if (m_drawCallID >= kMaxConcurrentDraws) {
      ++m_submitRejectStats.queueOverflow;
      return;
    }

    // --- Cheap pre-filters: discard draws that cannot contribute to raytracing ---

    // Only triangle topologies are raytraceable. Skip points, lines, patch lists, etc.
    // This check is first: it costs a single comparison before any other state is read.
    const D3D11_PRIMITIVE_TOPOLOGY d3dTopology = m_context->m_state.ia.primitiveTopology;
    if (d3dTopology != D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST &&
        d3dTopology != D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP) {
      ++m_submitRejectStats.nonTriangleTopology;
      return;
    }

    // Skip depth-only passes: no pixel shader means depth prepass or shadow map.
    // Most engines draw opaque geometry twice — once for depth prepass (PS == null)
    // and once for the color pass (PS != null) with the same vertices.
    if (m_context->m_state.ps.shader == nullptr) {
      ++m_submitRejectStats.noPixelShader;
      return;
    }

    // Skip draws with no color render target (shadow maps, depth-only, auxiliary passes).
    if (m_context->m_state.om.renderTargetViews[0].ptr() == nullptr) {
      ++m_submitRejectStats.noRenderTarget;
      return;
    }

    // Skip trivially small draws (< 3 elements = 0 triangles).
    if (count < 3) {
      ++m_submitRejectStats.trivialDraw;
      return;
    }

    // Read actual depth/stencil state from the OM — don't hardcode.
    bool zEnable = true;
    bool zWriteEnable = true;
    bool stencilEnabled = false;
    D3D11DepthStencilState* dsState = m_context->m_state.om.dsState;
    if (dsState) {
      D3D11_DEPTH_STENCIL_DESC dsDesc;
      dsState->GetDesc(&dsDesc);
      zEnable         = dsDesc.DepthEnable != FALSE;
      zWriteEnable    = dsDesc.DepthWriteMask != D3D11_DEPTH_WRITE_MASK_ZERO;
      stencilEnabled  = dsDesc.StencilEnable != FALSE;
    }

    // Skip fullscreen quad / postprocess draws: depth disabled + 6 or fewer
    // elements (a fullscreen triangle or quad) + no depth write.
    // Only skip if BOTH depth test and write are off — some engines do
    // "depth off, write on" for sky or "depth on, write off" for decals.
    if (!zEnable && !zWriteEnable && count <= 6) {
      ++m_submitRejectStats.fullscreenPostFx;
      return;
    }

    D3D11InputLayout* layout = m_context->m_state.ia.inputLayout.ptr();
    if (!layout) {
      ++m_submitRejectStats.noInputLayout;
      return;
    }

    const auto& semantics = layout->GetRtxSemantics();

    if (semantics.empty()) {
      ++m_submitRejectStats.noSemantics;
      return;
    }

    const D3D11RtxSemantic* posSem = selectBestSemantic(semantics, scorePositionSemantic);
    const D3D11RtxSemantic* tcSem  = selectBestSemantic(semantics, scoreTexcoordSemantic, { posSem });
    const D3D11RtxSemantic* nrmSem = selectBestSemantic(semantics, scoreNormalSemantic, { posSem, tcSem });
    const D3D11RtxSemantic* colSem = selectBestSemantic(semantics, scoreColorSemantic, { posSem, tcSem, nrmSem });
    const D3D11RtxSemantic* bwSem  = selectBestSemantic(semantics, scoreBlendWeightSemantic, { posSem, tcSem, nrmSem, colSem });
    const D3D11RtxSemantic* biSem  = selectBestSemantic(semantics, scoreBlendIndexSemantic, { posSem, tcSem, nrmSem, colSem, bwSem });

    if (!posSem) {
      ++m_submitRejectStats.noPositionSemantic;
      return;
    }

    // Skip 2D UI/HUD draws: if position is R32G32_SFLOAT it is in screen/clip space,
    // not world space, and cannot be raytraced.
    if (posSem->format == VK_FORMAT_R32G32_SFLOAT) {
      ++m_submitRejectStats.position2D;
      return;
    }

    auto makeVertexBuffer = [&](const D3D11RtxSemantic* sem) -> RasterBuffer {
      if (!sem)
        return RasterBuffer();
      const auto& vb = m_context->m_state.ia.vertexBuffers[sem->inputSlot];
      if (vb.buffer == nullptr)
        return RasterBuffer();
      DxvkBufferSlice slice = vb.buffer->GetBufferSlice(vb.offset);
      return RasterBuffer(slice, sem->byteOffset, vb.stride, sem->format);
    };

    RasterBuffer posBuffer = makeVertexBuffer(posSem);
    if (!posBuffer.defined()) {
      ++m_submitRejectStats.noPositionBuffer;
      return;
    }

    // Normal buffer: only submit if enabled and the interleaver can convert.
    // Supported: R16G16_SFLOAT(83), R32G32_SFLOAT(103), R32G32B32_SFLOAT(106),
    // R32G32B32A32_SFLOAT(109), R8G8B8A8_UNORM(37), A2B10G10R10_SNORM(65).
    // D3D11 normals are often R16G16B16A16_SFLOAT(97) or R16G16B16A16_SNORM(98)
    // which the interleaver rejects.  Remix regenerates normals when absent.
    RasterBuffer nrmBuffer;
    if (nrmSem && RtxOptions::useInputAssemblerNormals()) {
      VkFormat nf = nrmSem->format;
      if (nf == VK_FORMAT_R8G8B8A8_UNORM
       || nf == VK_FORMAT_R32G32B32_SFLOAT
       || nf == VK_FORMAT_R32G32B32A32_SFLOAT
       || nf == VK_FORMAT_R32G32_SFLOAT
       || nf == VK_FORMAT_R16G16_SFLOAT
       || nf == static_cast<VkFormat>(65)) {  // A2B10G10R10_SNORM_PACK32
        nrmBuffer = makeVertexBuffer(nrmSem);
      }
    }
    RasterBuffer tcBuffer  = makeVertexBuffer(tcSem);

    RasterBuffer skinWeightBuffer;
    RasterBuffer skinIndexBuffer;
    uint32_t skinBonesPerVertex = 0;

    // Color0: the interleaver converts BGRA and RGBA packed-byte formats.
    // Both B8G8R8A8_UNORM (D3D9 D3DCOLOR) and R8G8B8A8_UNORM (D3D11) are
    // supported — the interleaver swaps R/B for RGBA.  Float vertex color
    // formats are not supported; Remix defaults to white when color0 is absent.
    RasterBuffer colBuffer;
    if (colSem && (colSem->format == VK_FORMAT_B8G8R8A8_UNORM
                || colSem->format == VK_FORMAT_R8G8B8A8_UNORM)) {
      colBuffer = makeVertexBuffer(colSem);
    }

    RasterBuffer idxBuffer;
    if (indexed) {
      const auto& ib = m_context->m_state.ia.indexBuffer;
      if (ib.buffer == nullptr) {
        ++m_submitRejectStats.noIndexBuffer;
        return;
      }
      VkIndexType idxType = (ib.format == DXGI_FORMAT_R32_UINT)
                          ? VK_INDEX_TYPE_UINT32
                          : VK_INDEX_TYPE_UINT16;
      uint32_t idxStride = (idxType == VK_INDEX_TYPE_UINT32) ? 4 : 2;
      idxBuffer = RasterBuffer(ib.buffer->GetBufferSlice(ib.offset), 0, idxStride, idxType);
    }

    VkPrimitiveTopology vkTopology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    switch (m_context->m_state.ia.primitiveTopology) {
      case D3D11_PRIMITIVE_TOPOLOGY_POINTLIST:     vkTopology = VK_PRIMITIVE_TOPOLOGY_POINT_LIST;     break;
      case D3D11_PRIMITIVE_TOPOLOGY_LINELIST:      vkTopology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST;      break;
      case D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP:     vkTopology = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;     break;
      case D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST:  vkTopology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;  break;
      case D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP: vkTopology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP; break;
      default: break;
    }

    RasterGeometry geo;
    geo.topology       = vkTopology;
    geo.frontFace      = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    geo.positionBuffer = posBuffer;
    geo.normalBuffer   = nrmBuffer;
    geo.texcoordBuffer = tcBuffer;
    geo.color0Buffer   = colBuffer;
    geo.blendWeightBuffer = skinWeightBuffer;
    geo.blendIndicesBuffer = skinIndexBuffer;
    geo.numBonesPerVertex = skinBonesPerVertex;
    geo.indexBuffer    = idxBuffer;
    geo.indexCount     = indexed ? count : 0;

    // Read cull mode from the immutable ID3D11RasterizerState object.
    // Default: no culling (safe fallback when no state is bound).
    geo.cullMode = VK_CULL_MODE_NONE;
    D3D11RasterizerState* rsState = m_context->m_state.rs.state;
    if (rsState) {
      const auto* rsDesc = rsState->Desc();
      switch (rsDesc->CullMode) {
        case D3D11_CULL_NONE:  geo.cullMode = VK_CULL_MODE_NONE;      break;
        case D3D11_CULL_FRONT: geo.cullMode = VK_CULL_MODE_FRONT_BIT; break;
        case D3D11_CULL_BACK:  geo.cullMode = VK_CULL_MODE_BACK_BIT;  break;
      }
      geo.frontFace = rsDesc->FrontCounterClockwise
        ? VK_FRONT_FACE_COUNTER_CLOCKWISE
        : VK_FRONT_FACE_CLOCKWISE;
    }

    // Compute vertex count — must cover the highest vertex index accessed by
    // this draw so Remix doesn't read out of bounds when building BLAS.
    // geo.vertexCount is the buffer capacity; the hash uses a tighter subrange.
    const uint32_t maxVBVertices = posBuffer.stride() > 0
      ? static_cast<uint32_t>(posBuffer.length() / posBuffer.stride())
      : count;
    uint32_t drawVertexCount;
    uint32_t hashStart, hashCount;
    if (!indexed) {
      // Non-indexed Draw(count, start): vertices [start, start+count) accessed.
      drawVertexCount = std::min(start + count, maxVBVertices);
      hashStart = std::min(start, maxVBVertices);
      hashCount = std::min(count, maxVBVertices - hashStart);
    } else {
      // Indexed DrawIndexed(indexCount, startIndex, base): vertex = index + base.
      // Without scanning the IB we don't know max(index), so use base + indexCount
      // as a conservative upper bound.
      const uint32_t baseU = static_cast<uint32_t>(std::max(base, 0));
      drawVertexCount = std::min(baseU + count, maxVBVertices);
      hashStart = std::min(baseU, maxVBVertices);
      hashCount = std::min(count, maxVBVertices - hashStart);
    }
    if (drawVertexCount == 0)
      drawVertexCount = count;
    if (hashCount == 0)
      hashCount = std::min(count, maxVBVertices);
    hashCount = std::min(hashCount, kMaxHashedVertices);
    geo.vertexCount = drawVertexCount;

    if (nrmBuffer.defined() && bwSem && biSem) {
      RasterBuffer nativeWeightBuffer = makeVertexBuffer(bwSem);
      RasterBuffer nativeIndexBuffer = makeVertexBuffer(biSem);

      if (nativeWeightBuffer.defined() && nativeIndexBuffer.defined()) {
        const uint8_t* weightBase = reinterpret_cast<const uint8_t*>(nativeWeightBuffer.mapPtr(nativeWeightBuffer.offsetFromSlice()));
        const uint8_t* indexBase = reinterpret_cast<const uint8_t*>(nativeIndexBuffer.mapPtr(nativeIndexBuffer.offsetFromSlice()));

        if (weightBase != nullptr && indexBase != nullptr && nativeWeightBuffer.stride() > 0 && nativeIndexBuffer.stride() > 0) {
          float sourceWeights[4] = {};
          uint32_t sourceWeightCount = 0;
          uint32_t sourceIndices[4] = {};
          uint32_t sourceIndexCount = 0;

          if (decodeBlendWeights(weightBase, nativeWeightBuffer.vertexFormat(), sourceWeights, sourceWeightCount)
           && decodeBlendIndices(indexBase, nativeIndexBuffer.vertexFormat(), sourceIndices, sourceIndexCount)) {
            const uint32_t configuredMaxBones = std::min<uint32_t>(4u, RtxOptions::limitedBonesPerVertex());
            skinBonesPerVertex = std::min({ sourceIndexCount, sourceWeightCount + 1u, configuredMaxBones });

            if (skinBonesPerVertex >= 2) {
              const uint32_t explicitWeightCount = skinBonesPerVertex - 1;
              const VkFormat normalizedWeightFormat = normalizedBlendWeightFormat(explicitWeightCount);

              if (normalizedWeightFormat != VK_FORMAT_UNDEFINED) {
                const VkDeviceSize weightBufferSize = VkDeviceSize(explicitWeightCount) * VkDeviceSize(drawVertexCount) * sizeof(float);
                const VkDeviceSize indexBufferSize = VkDeviceSize(drawVertexCount) * sizeof(uint32_t);

                Rc<DxvkBuffer> normalizedWeightBuffer = createHostVisibleHelperBuffer(m_context->m_device, weightBufferSize, "d3d11 skinning weights");
                Rc<DxvkBuffer> normalizedIndexBuffer = createHostVisibleHelperBuffer(m_context->m_device, indexBufferSize, "d3d11 skinning indices");

                if (normalizedWeightBuffer != nullptr && normalizedIndexBuffer != nullptr) {
                  float* dstWeights = reinterpret_cast<float*>(normalizedWeightBuffer->mapPtr(0));
                  uint8_t* dstIndices = reinterpret_cast<uint8_t*>(normalizedIndexBuffer->mapPtr(0));
                  bool normalizedOk = dstWeights != nullptr && dstIndices != nullptr;

                  for (uint32_t vertex = 0; normalizedOk && vertex < drawVertexCount; ++vertex) {
                    const uint8_t* srcWeights = reinterpret_cast<const uint8_t*>(nativeWeightBuffer.mapPtr(nativeWeightBuffer.offsetFromSlice() + size_t(vertex) * nativeWeightBuffer.stride()));
                    const uint8_t* srcIndices = reinterpret_cast<const uint8_t*>(nativeIndexBuffer.mapPtr(nativeIndexBuffer.offsetFromSlice() + size_t(vertex) * nativeIndexBuffer.stride()));
                    if (srcWeights == nullptr || srcIndices == nullptr) {
                      normalizedOk = false;
                      break;
                    }

                    float decodedWeights[4] = {};
                    uint32_t decodedWeightCount = 0;
                    uint32_t decodedIndices[4] = {};
                    uint32_t decodedIndexCount = 0;
                    if (!decodeBlendWeights(srcWeights, nativeWeightBuffer.vertexFormat(), decodedWeights, decodedWeightCount)
                     || !decodeBlendIndices(srcIndices, nativeIndexBuffer.vertexFormat(), decodedIndices, decodedIndexCount)) {
                      normalizedOk = false;
                      break;
                    }

                    if (decodedWeightCount + 1 < skinBonesPerVertex || decodedIndexCount < skinBonesPerVertex) {
                      normalizedOk = false;
                      break;
                    }

                    float explicitSum = 0.0f;
                    for (uint32_t bone = 0; bone < explicitWeightCount; ++bone) {
                      explicitSum += decodedWeights[bone];
                    }
                    if (explicitSum > 1.0f && explicitSum > 0.0f) {
                      const float invSum = 1.0f / explicitSum;
                      for (uint32_t bone = 0; bone < explicitWeightCount; ++bone) {
                        decodedWeights[bone] *= invSum;
                      }
                    }

                    for (uint32_t bone = 0; bone < explicitWeightCount; ++bone) {
                      dstWeights[vertex * explicitWeightCount + bone] = decodedWeights[bone];
                    }

                    std::array<uint8_t, 4> packedIndices = { 0, 0, 0, 0 };
                    for (uint32_t bone = 0; bone < skinBonesPerVertex; ++bone) {
                      if (decodedIndices[bone] > 255u) {
                        normalizedOk = false;
                        break;
                      }
                      packedIndices[bone] = uint8_t(decodedIndices[bone]);
                    }

                    if (!normalizedOk)
                      break;

                    std::memcpy(dstIndices + size_t(vertex) * sizeof(uint32_t), packedIndices.data(), sizeof(uint32_t));
                  }

                  if (normalizedOk) {
                    skinWeightBuffer = RasterBuffer(
                      DxvkBufferSlice { normalizedWeightBuffer, 0, weightBufferSize },
                      0,
                      explicitWeightCount * sizeof(float),
                      normalizedWeightFormat);
                    skinIndexBuffer = RasterBuffer(
                      DxvkBufferSlice { normalizedIndexBuffer, 0, indexBufferSize },
                      0,
                      sizeof(uint32_t),
                      VK_FORMAT_R8G8B8A8_USCALED);
                  } else {
                    skinBonesPerVertex = 0;
                  }
                }
              }
            }
          }
        }
      }
    }

    geo.blendWeightBuffer = skinWeightBuffer;
    geo.blendIndicesBuffer = skinIndexBuffer;
    geo.numBonesPerVertex = skinBonesPerVertex;

    geo.futureGeometryHashes = ComputeGeometryHashes(geo, drawVertexCount,
                                                     hashStart, hashCount);
    if (!geo.futureGeometryHashes.valid()) {
      ++m_submitRejectStats.geometryHashScheduleFailed;
      return;
    }

    Future<SkinningData> futureSkinningData;
    if (geo.blendWeightBuffer.defined() && geo.blendIndicesBuffer.defined() && geo.numBonesPerVertex >= 2) {
      static constexpr size_t kMaxSkinningScanBytes = 8192;
      auto cbRange = [](const D3D11ConstantBufferBinding& cb) -> std::pair<size_t, size_t> {
        const size_t bufSize = cb.buffer->Desc()->ByteWidth;
        const size_t base = size_t(cb.constantOffset) * 16;
        if (base >= bufSize)
          return { 0, 0 };
        size_t end = cb.constantCount > 0
          ? std::min(base + size_t(cb.constantCount) * 16, bufSize)
          : bufSize;
        if (end - base > kMaxSkinningScanBytes)
          end = base + kMaxSkinningScanBytes;
        return { base, end };
      };

      std::vector<SkinningConstantBufferSnapshot> skinningCbuffers;
      skinningCbuffers.reserve(D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT);
      for (uint32_t slot = 0; slot < D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT; ++slot) {
        const auto& cb = m_context->m_state.vs.constantBuffers[slot];
        if (cb.buffer == nullptr)
          continue;

        const auto mapped = cb.buffer->GetMappedSlice();
        const uint8_t* ptr = reinterpret_cast<const uint8_t*>(mapped.mapPtr);
        if (ptr == nullptr)
          continue;

        auto [baseOffset, endOffset] = cbRange(cb);
        if (endOffset <= baseOffset || endOffset - baseOffset < 128)
          continue;

        SkinningConstantBufferSnapshot snapshot;
        snapshot.slot = slot;
        snapshot.data.resize(endOffset - baseOffset);
        std::memcpy(snapshot.data.data(), ptr + baseOffset, snapshot.data.size());
        skinningCbuffers.push_back(std::move(snapshot));
      }

      if (!skinningCbuffers.empty()) {
        const RasterBuffer weightBuffer = geo.blendWeightBuffer;
        const RasterBuffer indexBufferForSkinning = geo.blendIndicesBuffer;
        const uint32_t bonesPerVertex = geo.numBonesPerVertex;
        const bool columnMajorSkinning = m_columnMajor;

        futureSkinningData = m_pGeometryWorkers->Schedule([
          weightBuffer,
          indexBufferForSkinning,
          drawVertexCount,
          bonesPerVertex,
          columnMajorSkinning,
          cbufferSnapshots = std::move(skinningCbuffers)
        ]() mutable -> SkinningData {
          SkinningData skinningData;

          const float* weightData = reinterpret_cast<const float*>(weightBuffer.mapPtr(weightBuffer.offsetFromSlice()));
          const uint8_t* indexData = reinterpret_cast<const uint8_t*>(indexBufferForSkinning.mapPtr(indexBufferForSkinning.offsetFromSlice()));
          if (weightData == nullptr || indexData == nullptr || bonesPerVertex < 2)
            return skinningData;

          const uint32_t explicitWeightCount = bonesPerVertex - 1;
          const uint32_t weightStride = weightBuffer.stride() / sizeof(float);
          const uint32_t indexStride = indexBufferForSkinning.stride();
          if (weightStride < explicitWeightCount || indexStride < bonesPerVertex)
            return skinningData;

          std::array<bool, 256> usedBoneMask = {};
          uint32_t minBoneIndex = 255u;
          uint32_t maxBoneIndex = 0u;
          std::vector<uint32_t> usedBoneIndices;
          usedBoneIndices.reserve(32);

          const uint32_t sampledVertexCount = std::min(drawVertexCount, kMaxSkinningVerticesToScan);
          auto sampleVertexIndex = [&](uint32_t sampleIndex) {
            if (sampledVertexCount <= 1 || drawVertexCount <= 1)
              return 0u;

            return uint32_t((uint64_t(sampleIndex) * uint64_t(drawVertexCount - 1))
              / uint64_t(sampledVertexCount - 1));
          };

          for (uint32_t sampleIndex = 0; sampleIndex < sampledVertexCount; ++sampleIndex) {
            const uint32_t vertex = sampleVertexIndex(sampleIndex);
            const float* vertexWeights = weightData + size_t(vertex) * weightStride;
            const uint8_t* vertexIndices = indexData + size_t(vertex) * indexStride;

            float explicitSum = 0.0f;
            for (uint32_t bone = 0; bone < explicitWeightCount; ++bone) {
              const float weight = vertexWeights[bone];
              if (!std::isfinite(weight))
                return SkinningData {};
              explicitSum += std::clamp(weight, 0.0f, 1.0f);
            }

            for (uint32_t bone = 0; bone < bonesPerVertex; ++bone) {
              const float weight = bone < explicitWeightCount
                ? std::clamp(vertexWeights[bone], 0.0f, 1.0f)
                : std::max(0.0f, 1.0f - explicitSum);
              if (weight <= 1.0e-5f)
                continue;

              const uint32_t index = vertexIndices[bone];
              if (!usedBoneMask[index]) {
                usedBoneMask[index] = true;
                usedBoneIndices.push_back(index);
                minBoneIndex = std::min(minBoneIndex, index);
                maxBoneIndex = std::max(maxBoneIndex, index);
              }
            }
          }

          if (usedBoneIndices.empty())
            return skinningData;

          auto scorePalette = [&](const SkinningConstantBufferSnapshot& snapshot, size_t startOffset, bool transposeMatrix) -> float {
            float score = 0.0f;
            uint32_t validCount = 0;
            uint32_t nonIdentityCount = 0;
            const size_t sampleCount = std::min<size_t>(usedBoneIndices.size(), 16);

            for (size_t i = 0; i < sampleCount; ++i) {
              const uint32_t boneIndex = usedBoneIndices[i];
              const size_t matrixOffset = startOffset + size_t(boneIndex) * 64;
              if (matrixOffset + 64 > snapshot.data.size())
                return -1.0e30f;

              Matrix4 matrix = readCbMatrix(snapshot.data.data(), matrixOffset, snapshot.data.size());
              if (transposeMatrix)
                matrix = transpose(matrix);
              if (!isSkinningMatrix(matrix))
                return -1.0e30f;

              ++validCount;
              if (!isIdentityExact(matrix))
                ++nonIdentityCount;
            }

            if (validCount == 0)
              return -1.0e30f;

            score += validCount * 4.0f;
            score += nonIdentityCount * 2.0f;
            score -= float(startOffset) / 256.0f;
            score -= float(snapshot.slot) * 0.5f;

            if (nonIdentityCount == 0)
              score -= 6.0f;

            return score;
          };

          const SkinningConstantBufferSnapshot* bestSnapshot = nullptr;
          size_t bestStartOffset = 0;
          bool bestTranspose = false;
          float bestScore = -1.0e30f;

          for (const auto& snapshot : cbufferSnapshots) {
            const size_t requiredBytes = (size_t(maxBoneIndex) + 1) * 64;
            if (snapshot.data.size() < requiredBytes)
              continue;

            for (size_t startOffset = 0; startOffset + requiredBytes <= snapshot.data.size(); startOffset += 16) {
              const float rowMajorScore = scorePalette(snapshot, startOffset, false);
              if (rowMajorScore > bestScore) {
                bestScore = rowMajorScore;
                bestSnapshot = &snapshot;
                bestStartOffset = startOffset;
                bestTranspose = false;
              }

              const float columnMajorScore = scorePalette(snapshot, startOffset, true);
              if (columnMajorScore > bestScore) {
                bestScore = columnMajorScore;
                bestSnapshot = &snapshot;
                bestStartOffset = startOffset;
                bestTranspose = true;
              }
            }
          }

          if (bestSnapshot == nullptr || bestScore < 4.0f)
            return skinningData;

          skinningData.numBonesPerVertex = bonesPerVertex;
          skinningData.minBoneIndex = minBoneIndex;
          skinningData.numBones = maxBoneIndex + 1;
          skinningData.pBoneMatrices.resize(skinningData.numBones, Matrix4());

          for (uint32_t boneIndex = 0; boneIndex < skinningData.numBones; ++boneIndex) {
            const size_t matrixOffset = bestStartOffset + size_t(boneIndex) * 64;
            if (matrixOffset + 64 > bestSnapshot->data.size())
              break;

            Matrix4 matrix = readCbMatrix(bestSnapshot->data.data(), matrixOffset, bestSnapshot->data.size());
            if (bestTranspose)
              matrix = transpose(matrix);
            if (!isSkinningMatrix(matrix))
              matrix = Matrix4();
            skinningData.pBoneMatrices[boneIndex] = matrix;
          }

          skinningData.computeHash();
          return skinningData;
        });
      }
    }

    DrawCallState dcs;
    dcs.geometryData     = geo;
    dcs.transformData    = ExtractTransforms();
    dcs.futureSkinningData = futureSkinningData;

    // Apply per-instance world transform when submitting instanced draws.
    if (instanceTransform) {
      dcs.transformData.objectToWorld = *instanceTransform;
      // Recompute objectToView with the per-instance world matrix.
      dcs.transformData.objectToView = dcs.transformData.objectToWorld;
      if (!isIdentityExact(dcs.transformData.worldToView))
        dcs.transformData.objectToView = dcs.transformData.worldToView * dcs.transformData.objectToWorld;
    }

    // Let processCameraData() classify the camera from the matrices.
    // Hardcoding Main would bypass Remix's sky/portal/shadow detection.
    dcs.cameraType       = CameraType::Unknown;
    dcs.usesVertexShader = (m_context->m_state.vs.shader != nullptr);
    dcs.usesPixelShader  = (m_context->m_state.ps.shader != nullptr);

    // D3D11 shaders are always SM 4.0+.
    if (dcs.usesVertexShader)
      dcs.vertexShaderInfo = ShaderProgramInfo{4, 0};
    if (dcs.usesPixelShader)
      dcs.pixelShaderInfo = ShaderProgramInfo{4, 0};
    dcs.zWriteEnable     = zWriteEnable;
    dcs.zEnable          = zEnable;
    dcs.stencilEnabled   = stencilEnabled;
    dcs.drawCallID       = m_drawCallID++;

    // Viewport depth range from D3D11_VIEWPORT.MinDepth / MaxDepth.
    {
      const auto& vp = m_context->m_state.rs.viewports[0];
      dcs.minZ = std::clamp(vp.MinDepth, 0.0f, 1.0f);
      dcs.maxZ = std::clamp(vp.MaxDepth, 0.0f, 1.0f);
    }

    // D3D11 has no legacy fog — engines bake fog into shaders.
    // FogState defaults to mode=0 (none), which is correct.

    const auto isLikelyScreenSpaceCompositePass = [&]() {
      if (!dcs.transformData.usedViewportFallbackProjection)
        return false;

      if (!isIdentityExact(dcs.transformData.objectToWorld)
       || !isIdentityExact(dcs.transformData.worldToView))
        return false;

      const bool likelyFullscreenPrimitive = count <= 12;
      const bool likelyScreenSpaceDepthState = !zEnable || !zWriteEnable;
      if (!likelyFullscreenPrimitive && !likelyScreenSpaceDepthState)
        return false;

      // Fullscreen triangles/quads with only a synthesized camera and no
      // object/view transform are almost always post-process or UI composite
      // passes rather than stable scene geometry.
      // Reject these even if the bound textures are not exact RT aliases.
      const auto& omState = m_context->m_state.om;
      std::array<DxvkImage*, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT> boundRTImages = {};
      uint32_t rtWidth = 0;
      uint32_t rtHeight = 0;
      for (uint32_t rt = 0; rt < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++rt) {
        auto* rtv = omState.renderTargetViews[rt].ptr();
        if (!rtv)
          continue;

        Rc<DxvkImageView> rtvView = rtv->GetImageView();
        if (rtvView == nullptr)
          continue;

        boundRTImages[rt] = rtvView->image().ptr();
        if (rt == 0) {
          rtWidth = rtvView->image()->info().extent.width;
          rtHeight = rtvView->image()->info().extent.height;
        }
      }

      auto isBlockCompressed = [](DXGI_FORMAT fmt) {
        return (fmt >= DXGI_FORMAT_BC1_TYPELESS && fmt <= DXGI_FORMAT_BC1_UNORM_SRGB)
            || (fmt >= DXGI_FORMAT_BC2_TYPELESS && fmt <= DXGI_FORMAT_BC2_UNORM_SRGB)
            || (fmt >= DXGI_FORMAT_BC3_TYPELESS && fmt <= DXGI_FORMAT_BC3_UNORM_SRGB)
            || (fmt >= DXGI_FORMAT_BC4_TYPELESS && fmt <= DXGI_FORMAT_BC4_SNORM)
            || (fmt >= DXGI_FORMAT_BC5_TYPELESS && fmt <= DXGI_FORMAT_BC5_SNORM)
            || (fmt >= DXGI_FORMAT_BC6H_TYPELESS && fmt <= DXGI_FORMAT_BC7_UNORM_SRGB);
      };

      uint32_t candidateCount = 0;
      uint32_t rtSizedCount = 0;
      uint32_t contentLikeCount = 0;

      for (uint32_t slot = 0; slot < D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT; ++slot) {
        D3D11ShaderResourceView* srv = m_context->m_state.ps.shaderResources.views[slot].ptr();
        if (!srv || srv->GetResourceType() != D3D11_RESOURCE_DIMENSION_TEXTURE2D)
          continue;

        Rc<DxvkImageView> view = srv->GetImageView();
        if (view == nullptr)
          continue;

        const auto& imgInfo = view->image()->info();
        if (imgInfo.extent.width <= 2 && imgInfo.extent.height <= 2)
          continue;

        ++candidateCount;

        D3D11_SHADER_RESOURCE_VIEW_DESC1 srvDesc = {};
        srv->GetDesc1(&srvDesc);

        const bool matchesRT = rtWidth > 0 && rtHeight > 0
          && imgInfo.extent.width == rtWidth
          && imgInfo.extent.height == rtHeight;
        const bool hasMips = imgInfo.mipLevels > 1;
        const bool bc = isBlockCompressed(srvDesc.Format);

        bool isCurrentRT = false;
        for (DxvkImage* boundRT : boundRTImages) {
          if (boundRT == view->image().ptr()) {
            isCurrentRT = true;
            break;
          }
        }

        if (matchesRT || isCurrentRT)
          ++rtSizedCount;
        if (bc || hasMips || (!matchesRT && !isCurrentRT))
          ++contentLikeCount;
      }

      if (candidateCount == 0)
        return false;

      return rtSizedCount == candidateCount && contentLikeCount == 0;
    };

    const auto isLikelyScreenSpaceUiPass = [&]() {
      if (!dcs.transformData.usedViewportFallbackProjection)
        return false;

      if (!isIdentityExact(dcs.transformData.objectToWorld)
       || !isIdentityExact(dcs.transformData.worldToView))
        return false;

      if (count > 12)
        return false;

      const auto& omState = m_context->m_state.om;
      std::array<DxvkImage*, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT> boundRTImages = {};
      uint32_t rtWidth = 0;
      uint32_t rtHeight = 0;
      for (uint32_t rt = 0; rt < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++rt) {
        auto* rtv = omState.renderTargetViews[rt].ptr();
        if (!rtv)
          continue;

        Rc<DxvkImageView> rtvView = rtv->GetImageView();
        if (rtvView == nullptr)
          continue;

        boundRTImages[rt] = rtvView->image().ptr();
        if (rt == 0) {
          rtWidth = rtvView->image()->info().extent.width;
          rtHeight = rtvView->image()->info().extent.height;
        }
      }

      auto isBlockCompressed = [](DXGI_FORMAT fmt) {
        return (fmt >= DXGI_FORMAT_BC1_TYPELESS && fmt <= DXGI_FORMAT_BC1_UNORM_SRGB)
            || (fmt >= DXGI_FORMAT_BC2_TYPELESS && fmt <= DXGI_FORMAT_BC2_UNORM_SRGB)
            || (fmt >= DXGI_FORMAT_BC3_TYPELESS && fmt <= DXGI_FORMAT_BC3_UNORM_SRGB)
            || (fmt >= DXGI_FORMAT_BC4_TYPELESS && fmt <= DXGI_FORMAT_BC4_SNORM)
            || (fmt >= DXGI_FORMAT_BC5_TYPELESS && fmt <= DXGI_FORMAT_BC5_SNORM)
            || (fmt >= DXGI_FORMAT_BC6H_TYPELESS && fmt <= DXGI_FORMAT_BC7_UNORM_SRGB);
      };

      uint32_t candidateCount = 0;
      uint32_t uiLikeCount = 0;

      for (uint32_t slot = 0; slot < D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT; ++slot) {
        D3D11ShaderResourceView* srv = m_context->m_state.ps.shaderResources.views[slot].ptr();
        if (!srv || srv->GetResourceType() != D3D11_RESOURCE_DIMENSION_TEXTURE2D)
          continue;

        Rc<DxvkImageView> view = srv->GetImageView();
        if (view == nullptr)
          continue;

        const auto& imgInfo = view->image()->info();
        if (imgInfo.extent.width <= 2 && imgInfo.extent.height <= 2)
          continue;

        ++candidateCount;

        D3D11_SHADER_RESOURCE_VIEW_DESC1 srvDesc = {};
        srv->GetDesc1(&srvDesc);

        const bool matchesRT = rtWidth > 0 && rtHeight > 0
          && imgInfo.extent.width == rtWidth
          && imgInfo.extent.height == rtHeight;
        const bool hasMips = imgInfo.mipLevels > 1;
        const bool bc = isBlockCompressed(srvDesc.Format);

        bool isCurrentRT = false;
        for (DxvkImage* boundRT : boundRTImages) {
          if (boundRT == view->image().ptr()) {
            isCurrentRT = true;
            break;
          }
        }

        if (matchesRT || isCurrentRT)
          return false;

        D3D11SamplerState* samp = m_context->m_state.ps.samplers[slot];
        bool clampSampler = true;
        if (samp != nullptr) {
          D3D11_SAMPLER_DESC sampDesc = {};
          samp->GetDesc(&sampDesc);
          auto isWrapMode = [](D3D11_TEXTURE_ADDRESS_MODE mode) {
            return mode == D3D11_TEXTURE_ADDRESS_WRAP || mode == D3D11_TEXTURE_ADDRESS_MIRROR;
          };
          clampSampler = !isWrapMode(sampDesc.AddressU) && !isWrapMode(sampDesc.AddressV);
        }

        if (!hasMips && !bc && clampSampler)
          ++uiLikeCount;
      }

      return candidateCount > 0 && uiLikeCount == candidateCount;
    };

    const bool renderDocAttached = isRenderDocAttached();
    auto& sceneManager = m_context->m_device->getCommon()->getSceneManager();
    const auto& cameraManager = sceneManager.getCameraManager();
    const bool hasStableSceneCamera = cameraManager.isCameraValid(CameraType::Main)
      || cameraManager.getLastSetCameraType() != CameraType::Unknown;
    const bool allowViewportFallbackScreenSpaceReject = !renderDocAttached
      && dcs.transformData.usedViewportFallbackProjection
      && hasStableSceneCamera
      && (m_submitRejectStats.accepted > 0 || sceneManager.isPreviousFrameSceneAvailable());

    if (allowViewportFallbackScreenSpaceReject && isLikelyScreenSpaceCompositePass()) {
      ++m_submitRejectStats.compositeSkip;
      static uint32_t sScreenSpaceCompositeSkipLogCount = 0;
      if (sScreenSpaceCompositeSkipLogCount < 8) {
        ++sScreenSpaceCompositeSkipLogCount;
        Logger::info(str::format(
          "[D3D11Rtx] Skipping screen-space composite pass: viewport fallback camera + identity transforms + RT-sized textures (count=",
          count,
          ", zEnable=",
          zEnable ? 1 : 0,
          ", zWrite=",
          zWriteEnable ? 1 : 0,
          ")"));
      }
      return;
    }

    if (allowViewportFallbackScreenSpaceReject && isLikelyScreenSpaceUiPass()) {
      ++m_submitRejectStats.screenSpaceUiSkip;
      static uint32_t sScreenSpaceUiSkipLogCount = 0;
      if (sScreenSpaceUiSkipLogCount < 8) {
        ++sScreenSpaceUiSkipLogCount;
        Logger::info(str::format(
          "[D3D11Rtx] Skipping screen-space UI pass: viewport fallback camera + identity transforms + atlas-style textures (count=",
          count,
          ", zEnable=",
          zEnable ? 1 : 0,
          ", zWrite=",
          zWriteEnable ? 1 : 0,
          ")"));
      }
      return;
    }

    if (renderDocAttached && dcs.transformData.usedViewportFallbackProjection) {
      static uint32_t sRenderDocFallbackBypassLogCount = 0;
      if (sRenderDocFallbackBypassLogCount < 8) {
        ++sRenderDocFallbackBypassLogCount;
        Logger::info(str::format(
          "[D3D11Rtx] RenderDoc detected - bypassing viewport-fallback screen-space rejection (count=",
          count,
          ", zEnable=",
          zEnable ? 1 : 0,
          ", zWrite=",
          zWriteEnable ? 1 : 0,
          ")"));
      }
    }

    // Register this context as the active rendering context so the primary
    // swap chain routes EndFrame/OnPresent through us, not a video-playback
    // device that happened to present first.
    // Do this only after rejecting obvious composite passes so skipped draws
    // do not pay the material/texture selection cost.
    FillMaterialData(dcs.materialData);

    if (!geo.texcoordBuffer.defined() && dcs.materialData.usesTexture()) {
      ++m_submitRejectStats.noTexcoordLayout;
      return;
    }

    ++m_submitRejectStats.accepted;

    DrawParameters params;
    params.instanceCount = 1;
    params.vertexCount   = indexed ? 0 : count;
    params.indexCount    = indexed ? count : 0;
    params.firstIndex    = indexed ? start : 0;
    params.vertexOffset  = indexed ? static_cast<uint32_t>(std::max(base, 0)) : start;

    m_context->EmitCs([params, dcs](DxvkContext* ctx) mutable {
      static_cast<RtxContext*>(ctx)->commitGeometryToRT(params, dcs);
    });

    // CPU-GPU pacing: flush the CS chunk periodically so the GPU can start
    // processing draw batches while the CPU is still recording.  Without
    // this, the CPU can race thousands of draws ahead, bloating memory with
    // buffered DrawCallState objects and causing the GPU to stall at end-of-
    // frame when it has to process the entire backlog at once.
    if (++m_drawsSinceFlush >= kDrawsPerFlush) {
      m_drawsSinceFlush = 0;
      m_context->FlushCsChunk();
    }
  }

  void D3D11Rtx::EndFrame(const Rc<DxvkImage>& backbuffer, VkExtent2D clientExtent) {
    auto noteResizeTransition = [this](VkExtent2D newExtent, VkExtent2D& trackedExtent) {
      if (newExtent.width == 0u || newExtent.height == 0u)
        return;

      if (trackedExtent.width != 0u && trackedExtent.height != 0u
       && (trackedExtent.width != newExtent.width || trackedExtent.height != newExtent.height)) {
        m_resizeTransitionFramesRemaining = kResizeCameraGraceFrames;
      }

      trackedExtent = newExtent;
    };

    if (clientExtent.width > 0u && clientExtent.height > 0u) {
      noteResizeTransition(clientExtent, m_lastClientExtent);
    }

    if (backbuffer != nullptr) {
      const VkExtent3D extent = backbuffer->info().extent;
      if (extent.width > 0u && extent.height > 0u) {
        noteResizeTransition({ extent.width, extent.height }, m_lastOutputExtent);
      }
    }

    const uint32_t draws = m_drawCallID;
    static uint32_t s_endFrameLogCount = 0;
    static uint32_t s_submitSummaryLogCount = 0;
    if (s_endFrameLogCount < 8) {
      ++s_endFrameLogCount;
      Logger::info(str::format("[D3D11Rtx] EndFrame: draws=", draws,
        " backbuffer=", backbuffer != nullptr ? 1 : 0));
    }
    if (s_submitSummaryLogCount < 24 && m_submitRejectStats.total > draws) {
      ++s_submitSummaryLogCount;
      Logger::info(str::format(
        "[D3D11Rtx] Submit summary: total=", m_submitRejectStats.total,
        " accepted=", m_submitRejectStats.accepted,
        " overflow=", m_submitRejectStats.queueOverflow,
        " nonTriangle=", m_submitRejectStats.nonTriangleTopology,
        " noPS=", m_submitRejectStats.noPixelShader,
        " noRT=", m_submitRejectStats.noRenderTarget,
        " trivial=", m_submitRejectStats.trivialDraw,
        " fullscreen=", m_submitRejectStats.fullscreenPostFx,
        " noLayout=", m_submitRejectStats.noInputLayout,
        " noSemantics=", m_submitRejectStats.noSemantics,
        " noTexcoord=", m_submitRejectStats.noTexcoordLayout,
        " noPosSem=", m_submitRejectStats.noPositionSemantic,
        " pos2D=", m_submitRejectStats.position2D,
        " noPosBuffer=", m_submitRejectStats.noPositionBuffer,
        " noIB=", m_submitRejectStats.noIndexBuffer,
        " composite=", m_submitRejectStats.compositeSkip,
        " ui=", m_submitRejectStats.screenSpaceUiSkip,
        " hashFail=", m_submitRejectStats.geometryHashScheduleFailed));
    }

    ResetCommandListState();
    // Projection cache (m_projSlot, m_projOffset, m_projStage, m_columnMajor)
    // is NOT reset — the validation path at the start of ExtractTransforms
    // re-reads and re-scans only when the cached location becomes stale.
    // Keep the world-matrix cache for the same reason: modern games can have
    // thousands of draws per frame, and rescanning all cbuffers on every frame
    // creates unnecessary CPU pressure. The world-cache fast path still
    // validates the cached location every draw and falls back to a full rescan
    // automatically when the shader layout changes.
    ++m_axisDetectFrame;

    const bool allowResizeCameraCarryover = m_resizeTransitionFramesRemaining > 0;
    m_context->EmitCs([backbuffer, draws, allowResizeCameraCarryover](DxvkContext* ctx) {
      RtxContext* rtx = static_cast<RtxContext*>(ctx);
      const uint32_t fid = rtx->getDevice()->getCurrentFrameId();
      bool camValid = rtx->getSceneManager().getCamera().isValid(fid);
      if (!camValid && allowResizeCameraCarryover) {
        auto& cameraManager = rtx->getSceneManager().getCameraManager();
        auto& mainCamera = cameraManager.getCamera(CameraType::Main);
        const uint32_t lastUpdateFrame = mainCamera.getLastUpdateFrame();

        if (lastUpdateFrame != uint32_t(-1)
         && fid > lastUpdateFrame
         && fid - lastUpdateFrame <= D3D11Rtx::kResizeCameraGraceFrames) {
          cameraManager.processExternalCamera(
            CameraType::Main,
            Matrix4 { mainCamera.getWorldToView(false) },
            Matrix4 { mainCamera.getViewToProjection() });
          camValid = true;

          static uint32_t sResizeCameraCarryoverLogCount = 0;
          if (sResizeCameraCarryoverLogCount < 8) {
            ++sResizeCameraCarryoverLogCount;
            Logger::info(str::format(
              "[D3D11Rtx] Carrying forward last valid main camera across resize transition: frameId=",
              fid,
              " lastUpdate=",
              lastUpdateFrame));
          }
        }
      }
      if (fid < 32 || (fid < 512 && (fid % 64) == 0)) {
        Logger::info(str::format("[D3D11Rtx] CS endFrame: frameId=", fid,
          " draws=", draws, " camValid=", camValid ? 1 : 0));
      }
      rtx->endFrame(0, backbuffer, true);
    });

    if (m_resizeTransitionFramesRemaining > 0)
      --m_resizeTransitionFramesRemaining;
  }

  void D3D11Rtx::OnPresent(const Rc<DxvkImage>& swapchainImage, VkExtent2D clientExtent) {
    auto noteResizeTransition = [this](VkExtent2D newExtent, VkExtent2D& trackedExtent) {
      if (newExtent.width == 0u || newExtent.height == 0u)
        return;

      if (trackedExtent.width != 0u && trackedExtent.height != 0u
       && (trackedExtent.width != newExtent.width || trackedExtent.height != newExtent.height)) {
        m_resizeTransitionFramesRemaining = std::max(m_resizeTransitionFramesRemaining, kResizeCameraGraceFrames);
      }

      trackedExtent = newExtent;
    };

    if (clientExtent.width > 0u && clientExtent.height > 0u) {
      noteResizeTransition(clientExtent, m_lastClientExtent);
    }

    if (swapchainImage != nullptr) {
      const VkExtent3D extent = swapchainImage->info().extent;
      if (extent.width > 0u && extent.height > 0u) {
        noteResizeTransition({ extent.width, extent.height }, m_lastOutputExtent);
      }
    }

    m_context->EmitCs([swapchainImage](DxvkContext* ctx) {
      RtxContext* rtx = static_cast<RtxContext*>(ctx);
      rtx->onPresent(swapchainImage);
    });
  }

}
