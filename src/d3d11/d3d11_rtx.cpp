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
#include <set>

// DX11_V263_CRASH_FILTER_SAFE: defined in d3d11_main.cpp. Re-installs the
// log-only, chained unhandled-exception filter so a game crash handler
// installed after ours cannot silently eat the crash signature.
void RemixReassertCrashSignatureFilter();

namespace dxvk {

  namespace {

    bool isRenderDocAttached() {
      return ::GetModuleHandleW(L"renderdoc.dll") != nullptr;
    }

    bool shouldInjectD3D11RtxFrame(bool hasBackbuffer,
                                   bool hasGameSceneDraws,
                                   bool hasValidCamera,
                                   bool previousSceneAvailable) {
      if (!hasBackbuffer)
        return false;

      // Escape hatch: some games never produce a camera that passes the
      // validity gates below, which permanently blocks both path tracing and
      // the Remix UI (the UI renders inside the injected composite). When
      // rtx.dx11.forceInjection is enabled in dxvk.conf, inject every frame
      // that has a backbuffer.
      //
      // DX11_V253_MENU_PASSTHROUGH: even with forceInjection, a frame with no
      // scene draws, no camera and no prior scene is a pure-UI frame (title
      // screen / menu / loading built from screen-space quads). Injecting
      // replaces it with an empty composite - the "menu renders black" bug.
      // Pass such frames through so the game's own raster shows, EXCEPT while
      // the Remix menu is open, since that menu renders inside the composite.
      if (RtxOptions::forceInjection()) {
        const bool remixUiOpen = RtxOptions::showUI() != UIType::None;
        if (!hasGameSceneDraws && !hasValidCamera && !previousSceneAvailable && !remixUiOpen)
          return false;
        return true;
      }

      // First-time RTX injection needs a real scene camera. Otherwise loading
      // screens, menus, and weak viewport-fallback candidates can replace the
      // game frame with a black Remix composite. Previous scenes may only carry
      // when the current game frame still has a valid camera.
      return hasValidCamera && hasGameSceneDraws && (hasGameSceneDraws || previousSceneAvailable); // DX11_V124_CAMERA_ARTIFACT_STABILITY: do not inject fallback-only bootstrap/menu composite frames
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

  // Map D3D11_BLEND â†’ VkBlendFactor.  Mirrors D3D11BlendState::DecodeBlendFactor
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

  // Map D3D11_BLEND_OP â†’ VkBlendOp.
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

  uint32_t D3D11Rtx::getAcceptedSceneDrawCount() const {
    if (m_submitRejectStats.realSceneAccepted > 0) {
      return m_submitRejectStats.realSceneAccepted;
    }

    return m_hasSeenRealSceneProjection ? 0u : m_submitRejectStats.sceneAccepted;
  }

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
    // DX11-only games do not always create the DXVK Vulkan instance path that
    // normally initializes RTX options. Do it here before any DX11 defaults or
    // UI settings touch RtxOption layers.
    RtxOptions::Create();

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

    // --- Graphics preset: Custom by default ---
    // The Auto/High/Medium/Low presets populate the Quality Presets layer
    // (priority 0xFFFFFFFF) with values for every UserSetting-flagged option.
    // That layer is stronger than the User Settings layer (0xFFFFFFFE) and the
    // RtxConf layer (3), so any toggle the user makes in the menu lands in a
    // weaker layer and is immediately shadowed by the preset.  Observed
    // symptom: every checkbox and dropdown reverts as soon as it is changed.
    //
    // Forcing Custom keeps the Quality layer empty, letting User and RtxConf
    // writes win the resolve.  Games that want a preset can still set
    // rtx.graphicsPreset explicitly in their rtx.conf; that value lives in a
    // stronger layer (3) and overrides this Default-layer value.
    RtxOptions::graphicsPresetObject().setDeferred(GraphicsPreset::Custom, defaults);

    // Universal source-level default. Manufacturer upscalers remain selectable
    // in the UI/config, but first launch should not vendor-force DLSS/XeSS/FSR.
    RtxOptions::upscalerTypeObject().setDeferred(UpscalerType::TAAU, defaults);

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

    // Use incoming vertex buffers directly where safe (device-local geometry).
    // NOTE: host-visible/renameable (D3D11 dynamic) buffers are ALWAYS
    // snapshotted at submit regardless of this option - see
    // DX11_V250_DYNAMIC_BUFFER_SNAPSHOT in SubmitDraw. Binding those directly
    // reads a later rename's bytes at EndFrame record time (geometry collapses
    // to a point / turns to garbage).
    RtxOptions::useBuffersDirectlyObject().setDeferred(true, defaults);

    // --- Fallback lighting ---
    // D3D11 has no legacy lighting API â€” all lighting is shader-driven,
    // so Remix never receives explicit light definitions from the application.
    // Force the fallback light to Always so the scene is lit even if there are
    // no Remix USD light assets placed yet. Keep it moderate so it prevents
    // black scenes without blowing captured materials to flat white.
    // Kept at Always per user requirement: DX11 games never provide explicit
    // lights to Remix, and the scene must never go black. If a game with real
    // Remix lights (USD mods) over-brightens, set rtx.fallbackLightMode=1
    // (NoLightsPresent) in that game's rtx.conf instead of changing this default.
    LightManager::fallbackLightModeObject().setDeferred(LightManager::FallbackLightMode::Always, defaults);
    LightManager::fallbackLightTypeObject().setDeferred(LightManager::FallbackLightType::Distant, defaults);
    // DX11_V257_FALLBACK_RADIANCE: the light stays Always-on (hard user
    // requirement - scenes must never go black), but 2.0 radiance stacked on
    // games' own emissive/baked lighting blew scenes out to white. 1.0 keeps
    // everything clearly visible while leaving auto-exposure headroom. Tune
    // per game via rtx.fallbackLightRadiance in rtx.conf if a title reads dim.
    LightManager::fallbackLightRadianceObject().setDeferred(Vector3(1.0f, 1.0f, 1.0f), defaults);
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
      if (s.componentType != DxbcScalarType::Float32 || s.componentCount != 4) continue;

      // Accept any per-instance float4 row Ã¢â‚¬â€ most engines use INSTANCETRANSFORM, WORLD,
      // INSTANCE, I, INST, or repurpose high TEXCOORD registers. Matching on row
      // shape instead of one exact VkFormat catches more real D3D11 layouts.
      if (instSlot == UINT32_MAX)
        instSlot = s.inputSlot;

      if (s.inputSlot != instSlot) continue;
      instRows.push_back({s.inputSlot, s.byteOffset});
    }

    std::sort(instRows.begin(), instRows.end(), [] (const Float4Row& a, const Float4Row& b) {
      return a.byteOffset < b.byteOffset;
    });

    if (instRows.size() < 3) {
      // No instance transform found â€” submit once without instance data.
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

    // Cap to avoid excessive submission â€” configurable via rtx.maxInstanceSubmissions
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

      // If only 3 rows, the 4th row is (0,0,0,1) â€” affine transform.
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
      || format == VK_FORMAT_R16G16B16A16_SNORM
      // Fixed-point integer UVs: decoded to float by the interleaver with
      // rtx.integerTexcoordScale (Saints Row IV: TEXCOORD0 = R16G16_SINT).
      || format == VK_FORMAT_R16G16_SINT
      || format == VK_FORMAT_R16G16_UINT
      // DX11_V269: 32-bit fixed-point UVs, same scale treatment.
      || format == VK_FORMAT_R32G32_SINT
      || format == VK_FORMAT_R32G32_UINT;
  }

  static bool isPositionFormat(VkFormat format) {
    return format == VK_FORMAT_R32G32_SFLOAT
        || format == VK_FORMAT_R32G32B32_SFLOAT
        || format == VK_FORMAT_R32G32B32A32_SFLOAT
        || format == static_cast<VkFormat>(97);
  }

  // Byte size of one position element for the formats isPositionFormat accepts.
  // Used to bounds-check reads when computing the mesh bounding box.
  static uint32_t positionElementBytes(VkFormat format) {
    switch (format) {
      case VK_FORMAT_R32G32B32A32_SFLOAT:   return 16;
      case VK_FORMAT_R32G32B32_SFLOAT:      return 12;
      case VK_FORMAT_R32G32_SFLOAT:         return 8;
      case static_cast<VkFormat>(97):       return 8; // R16G16B16A16_SFLOAT (half4)
      default:                              return 0;
    }
  }

  // Decode an object-space position for bounding-box computation. Mirrors the
  // formats accepted by isPositionFormat. Returns false for unsupported formats
  // or non-finite data, in which case the caller leaves the bbox invalid so the
  // instance is kept (fail-safe: never drop geometry on a decode failure).
  static bool decodePositionForBounds(const uint8_t* src, VkFormat format, float out[3]) {
    switch (format) {
      case VK_FORMAT_R32G32B32_SFLOAT:
      case VK_FORMAT_R32G32B32A32_SFLOAT: {
        const float* f = reinterpret_cast<const float*>(src);
        out[0] = f[0]; out[1] = f[1]; out[2] = f[2];
      } break;
      case VK_FORMAT_R32G32_SFLOAT: {
        const float* f = reinterpret_cast<const float*>(src);
        out[0] = f[0]; out[1] = f[1]; out[2] = 0.0f;
      } break;
      case static_cast<VkFormat>(97): { // R16G16B16A16_SFLOAT (half4)
        const uint16_t* h = reinterpret_cast<const uint16_t*>(src);
        out[0] = decodeFloat16(h[0]); out[1] = decodeFloat16(h[1]); out[2] = decodeFloat16(h[2]);
      } break;
      default:
        return false;
    }
    return std::isfinite(out[0]) && std::isfinite(out[1]) && std::isfinite(out[2]);
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
        || format == VK_FORMAT_R32G32B32A32_SFLOAT
        // DX11_V268_VERTEX_COLOR_FORMATS: half4/unorm16 vertex colors -
        // accepted only under an explicit COLOR semantic name (the scorer
        // rejects them for generic names since these formats also carry
        // normals/tangents in many layouts).
        || format == VK_FORMAT_R16G16B16A16_UNORM
        || format == VK_FORMAT_R16G16B16A16_SFLOAT;
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

    // DX11_V269: 4-component texcoords are real UVs - engines pack two UV
    // sets into one float4 (xy = uv0, zw = uv1). Rejecting them dropped the
    // texcoord entirely (textures with no UVs = flat albedo); accept and use
    // xy, just score below dedicated 2-component streams.

    int score = 0;
    if (semanticNameStartsWith(semantic, "TEXCOORD")
     || semanticNameStartsWith(semantic, "TEX")
     || semanticNameStartsWith(semantic, "UV")
     || semanticNameStartsWith(semantic, "TCOORD")
     || semanticNameStartsWith(semantic, "MAP"))
      score += 1000;
    else if (semanticNameStartsWith(semantic, "ATTRIBUTE"))
      score += 140;
    else if (semanticNameStartsWith(semantic, "COLOR"))
      return std::numeric_limits<int>::min();
    else if (semanticNameStartsWith(semantic, "POSITION")
          || semanticNameStartsWith(semantic, "NORMAL")
          || semanticNameStartsWith(semantic, "BLEND"))
      return std::numeric_limits<int>::min();

    if (isFloatSemantic(semantic))
      score += 100;
    if (semantic.componentCount == 2)
      score += 220;
    else if (semantic.componentCount == 3)
      score += 100;
    else if (semantic.componentCount == 4)
      score += 40;  // DX11_V269: packed uv0+uv1 float4 - xy is used

    if (semantic.index == 0)
      score += 70;
    else if (semantic.index == 1)
      score += 40;

    if (semantic.registerId == 0)
      score -= 20;
    else
      score += 20;

    switch (semantic.format) {
      case VK_FORMAT_R32G32_SFLOAT:          score += 280; break;
      case VK_FORMAT_R16G16_SFLOAT:          score += 240; break;
      case VK_FORMAT_R8G8_UNORM:             score += 180; break;
      case VK_FORMAT_R16G16_UNORM:           score += 160; break;
      case VK_FORMAT_R8G8_SNORM:             score += 120; break;
      case VK_FORMAT_R16G16_SNORM:           score += 110; break;
      case VK_FORMAT_R32G32B32_SFLOAT:       score += 120; break;
      case VK_FORMAT_R16G16B16A16_SFLOAT:    score += 40; break;
      case VK_FORMAT_R32G32B32A32_SFLOAT:    score += 20; break;
      default: break;
    }

    return score;
  }

  static int scoreTexcoordFallbackSemantic(const D3D11RtxSemantic& semantic) {
    if (semantic.perInstance || semantic.systemValue != DxbcSystemValue::None || !isSupportedTexcoordFormat(semantic.format) || semantic.componentCount < 2)
      return std::numeric_limits<int>::min();

    if (semanticNameStartsWith(semantic, "POSITION")
     || semanticNameStartsWith(semantic, "NORMAL")
     || semanticNameStartsWith(semantic, "BLEND")
     || semanticNameStartsWith(semantic, "COLOR"))
      return std::numeric_limits<int>::min();

    int score = 0;
    if (semanticNameStartsWith(semantic, "ATTRIBUTE"))
      score += 220;
    else
      score += 80;

    if (isFloatSemantic(semantic))
      score += 100;

    if (semantic.componentCount == 2)
      score += 240;
    else if (semantic.componentCount == 3)
      score += 100;
    else if (semantic.componentCount == 4)
      score += 40;  // DX11_V269: packed uv0+uv1 float4 - xy is used

    if (semantic.index == 0)
      score += 50;
    else if (semantic.index == 1)
      score += 35;

    if (semantic.registerId == 0)
      score -= 20;
    else
      score += 20;

    switch (semantic.format) {
      case VK_FORMAT_R32G32_SFLOAT:          score += 300; break;
      case VK_FORMAT_R16G16_SFLOAT:          score += 260; break;
      case VK_FORMAT_R8G8_UNORM:             score += 200; break;
      case VK_FORMAT_R16G16_UNORM:           score += 170; break;
      case VK_FORMAT_R8G8_SNORM:             score += 130; break;
      case VK_FORMAT_R16G16_SNORM:           score += 120; break;
      case VK_FORMAT_R32G32B32_SFLOAT:       score += 100; break;
      case VK_FORMAT_R16G16B16A16_SFLOAT:    score += 20; break;
      case VK_FORMAT_R32G32B32A32_SFLOAT:    score += 0; break;
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
    // DX11_V259: tangent-frame streams share the normal formats but are NOT
    // shading normals - picking one bends lighting on every lightmapped mesh.
    // Remix regenerates normals when absent, so rejecting is strictly safer.
    else if (semanticNameStartsWith(semantic, "POSITION")
          || semanticNameStartsWith(semantic, "TEXCOORD")
          || semanticNameStartsWith(semantic, "COLOR")
          || semanticNameStartsWith(semantic, "BLEND")
          || semanticNameStartsWith(semantic, "TANGENT")
          || semanticNameStartsWith(semantic, "BINORMAL")
          || semanticNameStartsWith(semantic, "BITANGENT"))
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

    // DX11_V268: 16-bit-per-channel formats double as normal/tangent storage
    // in many vertex layouts; only an explicit COLOR name may claim them.
    if ((semantic.format == VK_FORMAT_R16G16B16A16_UNORM
      || semantic.format == VK_FORMAT_R16G16B16A16_SFLOAT)
     && !semanticNameStartsWith(semantic, "COLOR"))
      return std::numeric_limits<int>::min();

    int score = 0;
    if (semanticNameStartsWith(semantic, "COLOR"))
      score += 1000;
    else if (semanticNameStartsWith(semantic, "ATTRIBUTE"))
      score += 80;
    // DX11_V259: packed UBYTE4 tangent frames share COLOR0's format - misread
    // as vertex color they tint/darken every surface (worst with
    // vertexColorIsBakedLighting, where they masquerade as baked lighting).
    else if (semanticNameStartsWith(semantic, "POSITION")
          || semanticNameStartsWith(semantic, "TEXCOORD")
          || semanticNameStartsWith(semantic, "NORMAL")
          || semanticNameStartsWith(semantic, "BLEND")
          || semanticNameStartsWith(semantic, "TANGENT")
          || semanticNameStartsWith(semantic, "BINORMAL")
          || semanticNameStartsWith(semantic, "BITANGENT"))
      return std::numeric_limits<int>::min();

    switch (semantic.format) {
      case VK_FORMAT_B8G8R8A8_UNORM:      score += 240; break;
      case VK_FORMAT_R8G8B8A8_UNORM:      score += 220; break;
      case VK_FORMAT_R32G32B32A32_SFLOAT: score += 90; break;
      default: break;
    }

    return score;
  }

  // DX11_V259_SKINNING_NAME_GATE: only explicitly skinning-named semantics may
  // become bone weights/indices. The old heuristics accepted ANY unrecognized
  // semantic name (only POSITION/TEXCOORD/NORMAL/COLOR/BLEND* were excluded),
  // and the accepted formats overlap ordinary vertex data: lightmap UV
  // channels with custom names (LIGHTMAPUV, LM_UV, UV1...) are float2,
  // TANGENT/BINORMAL frames are float4/UBYTE4, and lightmap atlas page
  // indices are UINT - all of which passed as "bone weights"/"bone indices"
  // on static lightmapped world geometry. That flipped numBonesPerVertex >= 2
  // and the skinning path deformed the mesh with garbage "bone matrices"
  // scanned from the constant buffers, smearing broken copies over the scene.
  // The harm is asymmetric: a false positive destroys static geometry, while
  // a false negative merely skips skinning replication for a mesh the game
  // still renders. So: strict name allow-list, no generic-name fallback.
  static int scoreBlendWeightSemantic(const D3D11RtxSemantic& semantic) {
    if (semantic.perInstance || semantic.systemValue != DxbcSystemValue::None || !isBlendWeightFormat(semantic.format))
      return std::numeric_limits<int>::min();

    int score = 0;
    if (semanticNameStartsWith(semantic, "BLENDWEIGHT"))
      score += 1000;
    else if (semanticNameStartsWith(semantic, "BONEWEIGHT")
          || semanticNameStartsWith(semantic, "SKINWEIGHT")
          || semanticNameStartsWith(semantic, "WEIGHT"))
      score += 500;
    else
      return std::numeric_limits<int>::min();

    if (semantic.componentCount >= 1)
      score += 40;

    return score;
  }

  static int scoreBlendIndexSemantic(const D3D11RtxSemantic& semantic) {
    if (semantic.perInstance || semantic.systemValue != DxbcSystemValue::None || !isBlendIndexFormat(semantic.format))
      return std::numeric_limits<int>::min();

    int score = 0;
    if (semanticNameStartsWith(semantic, "BLENDINDICES")
     || semanticNameStartsWith(semantic, "BLENDINDEX"))
      score += 1000;
    else if (semanticNameStartsWith(semantic, "BONEINDICES")
          || semanticNameStartsWith(semantic, "BONEINDEX")
          || semanticNameStartsWith(semantic, "SKININDICES")
          || semanticNameStartsWith(semantic, "SKININDEX")
          || semanticNameStartsWith(semantic, "BONES"))
      score += 500;
    else
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
  //   m[0] = [Â±Sx, 0,   0,    0  ]
  //   m[1] = [0,  Â±Sy,  0,    0  ]
  //   m[2] = [Jx,  Jy,  Q,   Â±1 ]  â† perspective-divide at m[2][3]
  //   m[3] = [0,   0,   Wz,   0  ]
  //
  // Column-major matrices read back as row-major:
  //   m[0] = [Â±Sx, 0,   0,    0  ]
  //   m[1] = [0,  Â±Sy,  0,    0  ]
  //   m[2] = [Jx,  Jy,  Q,   Wz ]  â† m[2][3] = nearPlane or 0
  //   m[3] = [0,   0,  Â±1,    0  ]  â† perspective-divide at m[3][2]
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

    // Row-major check: m[2][3] â‰ˆ Â±1, m[3][3] â‰ˆ 0.
    const bool r23 = std::abs(std::abs(m[2][3]) - 1.0f) < kTol;
    const bool r33z = std::abs(m[3][3]) < kTol;
    if (r23 && r33z) {
      if (std::abs(m[0][2]) > kTol || std::abs(m[1][2]) > kTol) return 0;
      if (std::abs(m[3][0]) > kTol || std::abs(m[3][1]) > kTol) return 0;
      return 1;
    }

    // Column-major-as-row check: m[3][2] â‰ˆ Â±1, m[3][3] â‰ˆ 0.
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
  // The upper-left 3Ã—3 should be approximately orthonormal and the last column [0,0,0,1].
  static bool isViewMatrix(const Matrix4& m) {
    // Row 3 must be [*, *, *, 1] (affine).
    if (std::abs(m[3][3] - 1.0f) > 0.01f) return false;
    // Columns 0-2 of rows 0-2 should have unit length (orthonormal rotation).
    for (int col = 0; col < 3; ++col) {
      float lenSq = m[0][col] * m[0][col] + m[1][col] * m[1][col] + m[2][col] * m[2][col];
      if (std::abs(lenSq - 1.0f) > 0.1f) return false;
    }
    Vector3 axisX(m[0][0], m[1][0], m[2][0]);
    Vector3 axisY(m[0][1], m[1][1], m[2][1]);
    Vector3 axisZ(m[0][2], m[1][2], m[2][2]);
    if (std::abs(dot(axisX, axisY)) > 0.08f
     || std::abs(dot(axisX, axisZ)) > 0.08f
     || std::abs(dot(axisY, axisZ)) > 0.08f) {
      return false;
    }
    const double det = determinant(m);
    if (!std::isfinite(det) || std::abs(std::abs(det) - 1.0) > 0.25)
      return false;
    // m[0][3], m[1][3], m[2][3] should be 0 (no perspective warp).
    if (std::abs(m[0][3]) > 0.01f || std::abs(m[1][3]) > 0.01f || std::abs(m[2][3]) > 0.01f)
      return false;
    // Reject identity â€” identity means "no view transform" which is not useful.
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
    // always in the first few hundred bytes of a cbuffer â€” capping the scan
    // prevents multi-second stalls on emulators that pack all constants into
    // a single 64KB+ UBO (Xenia, Yuzu, RPCS3, Citra).
    static constexpr size_t kFastScanBytes = 8192;   // 128 matrices
    static constexpr size_t kDeepScanBytes = 65536;  // Full D3D11 cbuffer
    const bool needDeepCameraScan = !m_hasSeenRealSceneProjection
      && m_context->m_device->getCurrentFrameId() < 600u;
    const size_t maxScanBytes = needDeepCameraScan ? kDeepScanBytes : kFastScanBytes;

    // Compute the scannable byte range for a cbuffer binding: the intersection
    // of the bound range (constantOffset..constantOffset+constantCount) with
    // the buffer allocation, capped to maxScanBytes from the start of the range.
    auto cbRange = [maxScanBytes](const D3D11ConstantBufferBinding& cb) -> std::pair<size_t, size_t> {
      const size_t bufSize = cb.buffer->Desc()->ByteWidth;
      const size_t base    = static_cast<size_t>(cb.constantOffset) * 16;
      if (base >= bufSize)
        return { 0, 0 };
      size_t end;
      if (cb.constantCount > 0)
        end = std::min(base + static_cast<size_t>(cb.constantCount) * 16, bufSize);
      else
        end = bufSize;
      if (end - base > maxScanBytes)
        end = base + maxScanBytes;
      return { base, end };
    };

    // Some engines store matrices transposed in memory;
    // transposing after read normalizes them to row-major for all our checks.
    auto readMatrixWithConvention = [](const uint8_t* ptr, size_t offset, size_t bufSize, bool columnMajor) -> Matrix4 {
      Matrix4 m = readCbMatrix(ptr, offset, bufSize);
      return columnMajor ? transpose(m) : m;
    };

    auto readMatrix = [this, &readMatrixWithConvention](const uint8_t* ptr, size_t offset, size_t bufSize) -> Matrix4 {
      return readMatrixWithConvention(ptr, offset, bufSize, m_columnMajor);
    };

    auto resolveViewAt = [&](const uint8_t* ptr,
                             size_t offset,
                             size_t bufSize,
                             bool primaryColumnMajor,
                             bool allowOppositeConvention,
                             Matrix4& resolvedView,
                             bool& resolvedColumnMajor) -> bool {
      const Matrix4 primary = readMatrixWithConvention(ptr, offset, bufSize, primaryColumnMajor);
      if (resolveViewMatrixCandidate(primary, resolvedView)) {
        resolvedColumnMajor = primaryColumnMajor;
        return true;
      }

      if (allowOppositeConvention) {
        const bool oppositeColumnMajor = !primaryColumnMajor;
        const Matrix4 opposite = readMatrixWithConvention(ptr, offset, bufSize, oppositeColumnMajor);
        if (resolveViewMatrixCandidate(opposite, resolvedView)) {
          resolvedColumnMajor = oppositeColumnMajor;
          return true;
        }
      }

      return false;
    };

    // Viewport and render-target size are the most reliable camera references
    // for emulators and dynamic-resolution games. The host window can change
    // independently from the actual scene resolution, so client/output extents
    // should only be fallback hints instead of the primary aspect source.
    //
    // If the game has bound zero viewports (some engines leave the RS viewport
    // state dirty across a no-RT clear pass) or more than one viewport (shadow
    // cascade or split-screen passes), treat viewport[0] as a fallback hint
    // only and do not let it drive camera detection â€” otherwise a 256x256
    // cascade viewport would stamp its aspect onto the primary camera and
    // cause path tracing to render the wrong frustum for the main scene.
    float viewportAspect = 0.0f;
    const uint32_t boundViewports = m_context->m_state.rs.numViewports;
    const bool singleSceneViewport = boundViewports == 1;
    if (singleSceneViewport) {
      const auto& vp = m_context->m_state.rs.viewports[0];
      if (vp.Height > 0.0f && std::isfinite(vp.Width) && std::isfinite(vp.Height))
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
    float remixViewportAspect = 0.0f;
    if (m_lastRemixViewportExtent.width > 0u && m_lastRemixViewportExtent.height > 0u)
      remixViewportAspect = float(m_lastRemixViewportExtent.width) / float(m_lastRemixViewportExtent.height);
    float outputAspect = 0.0f;
    if (m_lastOutputExtent.width > 0u && m_lastOutputExtent.height > 0u)
      outputAspect = float(m_lastOutputExtent.width) / float(m_lastOutputExtent.height);
    const float projectionReferenceAspect = viewportAspect > 0.0f
      ? viewportAspect
      : (renderTargetAspect > 0.0f
        ? renderTargetAspect
        : (outputAspect > 0.0f
          ? outputAspect
          : remixViewportAspect));
    const float fallbackReferenceAspect = viewportAspect > 0.0f
      ? viewportAspect
      : (renderTargetAspect > 0.0f
        ? renderTargetAspect
        : (outputAspect > 0.0f
          ? outputAspect
          : remixViewportAspect));

    // Score a perspective projection: higher = more likely main game camera.
    // Shadow maps have square aspect, cubemaps have 90Â° FOV, tool cameras
    // have extreme FOV â€” all score lower than a typical game camera.
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

    // Raster shader stages to scan for camera matrices.
    // VS is most common; emulators (Dolphin, PCSX2, Xenia, Citra) and some
    // deferred renderers put camera matrices in GS, DS, or PS cbuffers.
    const D3D11ConstantBufferBindings* stageCbs[] = {
      &m_context->m_state.vs.constantBuffers,
      &m_context->m_state.hs.constantBuffers,
      &m_context->m_state.gs.constantBuffers,
      &m_context->m_state.ds.constantBuffers,
      &m_context->m_state.ps.constantBuffers,
    };
    static constexpr int kNumStages = 5;
    static const char* kStageNames[] = { "VS", "HS", "GS", "DS", "PS" };

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

    // DX11_V240 TRANSFORM DIAGNOSTIC: the path tracer renders black on all GPUs because we extract
    // only the projection (view/world stay identity), so geometry collapses to object space and the
    // RT camera is wrong. Dump every distinct candidate 4x4 matrix in the VS/GS/etc cbuffers (each
    // stage+slot+offset logged once) so we can see where the game stores world / view / projection
    // and fix the extraction. Captures both the menu and the 3D-scene matrices as they first appear.
    {
      // DX11_V267_LOG_CLEANUP: this dump diagnosed the camera-extraction bugs
      // (V240..V260, all fixed). It cost a per-draw cbuffer scan and 64 log
      // lines every session for an issue that no longer exists. Now opt-in:
      // set DXVK_REMIX_MTXDUMP=1 when debugging a new game's matrices.
      static const bool s_mtxDumpEnabled = env::getEnvVar("DXVK_REMIX_MTXDUMP") == "1";
      static std::set<uint64_t> s_dumpedMatrixLocs;
      static uint32_t s_dumpLogged = 0;
      if (s_mtxDumpEnabled && s_dumpLogged < 64) {
        for (int si = 0; si < kNumStages; ++si) {
          const auto& cbsD = *stageCbs[si];
          for (uint32_t slot = 0; slot < D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT; ++slot) {
            const auto& cbD = cbsD[slot];
            if (cbD.buffer == nullptr) continue;
            const auto mappedD = cbD.buffer->GetMappedSlice();
            const uint8_t* ptrD = reinterpret_cast<const uint8_t*>(mappedD.mapPtr);
            if (!ptrD) continue;
            const size_t bufSizeD = cbD.buffer->Desc()->ByteWidth;
            auto [baseD, endD] = cbRange(cbD);
            for (size_t off = baseD; off + 64 <= endD && s_dumpLogged < 64; off += 16) {
              Matrix4 m = readCbMatrix(ptrD, off, bufSizeD);
              if (isIdentityExact(m)) continue;
              const bool rowAffine = std::abs(m[0][3]) < 0.01f && std::abs(m[1][3]) < 0.01f && std::abs(m[2][3]) < 0.01f && std::abs(m[3][3] - 1.0f) < 0.01f;
              const bool colAffine = std::abs(m[3][0]) < 0.01f && std::abs(m[3][1]) < 0.01f && std::abs(m[3][2]) < 0.01f && std::abs(m[3][3] - 1.0f) < 0.01f;
              const bool persp     = classifyPerspective(m) != 0;
              if (!rowAffine && !colAffine && !persp) continue;
              const uint64_t key = (uint64_t(si) << 40) | (uint64_t(slot) << 32) | uint64_t(off);
              if (!s_dumpedMatrixLocs.insert(key).second) continue;
              ++s_dumpLogged;
              Logger::info(str::format("[D3D11Rtx][mtxdump] stage=", kStageNames[si], " slot=", slot, " off=", off,
                (persp ? " PERSP" : ""), (rowAffine ? " rowAff" : ""), (colAffine ? " colAff" : ""),
                " r0=", m[0][0], ",", m[0][1], ",", m[0][2], ",", m[0][3],
                " r1=", m[1][0], ",", m[1][1], ",", m[1][2], ",", m[1][3],
                " r2=", m[2][0], ",", m[2][1], ",", m[2][2], ",", m[2][3],
                " r3=", m[3][0], ",", m[3][1], ",", m[3][2], ",", m[3][3]));
            }
          }
        }
      }
    }

    // --- PROJECTION: first-draw scan (cache miss) ---
    // Single pass across all stages â€” classifyPerspective handles both layouts.
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

    // DX11_V260_PRECISE_CAMERA: the projection exactly as the engine stored it
    // (convention-normalized, but BEFORE jitter strip and orientation
    // canonicalization). Compositions against engine-stored ViewProj blocks
    // must use these bytes: the engine multiplied with the original matrix,
    // so inverting/composing the canonicalized one is off by the jitter terms
    // and, worse, by a whole axis flip when canonicalization fired - a flipped
    // "view" still passes the rigid-body test and mirrors the camera.
    Matrix4 rawProjNormalized;
    bool haveRawProjNormalized = false;

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
        // Cached location is stale (different pass). Re-scan all stages and
        // persist the winner back to the member cache â€” otherwise we would
        // redo this full multi-stage scan for every subsequent draw.
        projSlot = UINT32_MAX;
        float bestScore = 0.0f;
        bool bestCol = false;
        for (int si = 0; si < kNumStages; ++si) {
          uint32_t ts = UINT32_MAX; size_t to = SIZE_MAX;
          float tsc = bestScore; Matrix4 tm; bool tc = false;
          if (scanStageForProj(si, ts, to, tsc, tm, tc)) {
            projSlot = ts; projOffset = to; projStage = si;
            proj = tm; bestScore = tsc; bestCol = tc;
          }
        }

        if (projSlot != UINT32_MAX) {
          m_projSlot    = projSlot;
          m_projOffset  = projOffset;
          m_projStage   = projStage;
          m_columnMajor = bestCol;
        } else {
          // Nothing found â€” drop the stale cache so the next frame's
          // first-draw scan path runs instead of this re-scan path.
          m_projSlot   = UINT32_MAX;
          m_projOffset = SIZE_MAX;
          m_projStage  = -1;
        }
      }

      if (projSlot != UINT32_MAX) {
        rawProjNormalized = proj;
        haveRawProjNormalized = true;

        // Strip TAA jitter â€” Remix does its own TAA.
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
    //
    // Only synthesise a fallback projection when exactly one viewport is
    // bound.  Shadow cascade / cube face / split-screen passes bind multiple
    // viewports and must never drive the main camera.
    if (projSlot == UINT32_MAX && singleSceneViewport) {
      const auto& vp = m_context->m_state.rs.viewports[0];
      if (vp.Width > 0.0f && vp.Height > 0.0f) {
        float targetWidth = vp.Width;
        float targetHeight = vp.Height;
        bool haveStableSceneExtent = false;
        if (m_lastRemixViewportExtent.width > 0u && m_lastRemixViewportExtent.height > 0u) {
          targetWidth = float(m_lastRemixViewportExtent.width);
          targetHeight = float(m_lastRemixViewportExtent.height);
          haveStableSceneExtent = true;
        } else if (m_lastOutputExtent.width > 0u && m_lastOutputExtent.height > 0u) {
          targetWidth = float(m_lastOutputExtent.width);
          targetHeight = float(m_lastOutputExtent.height);
          haveStableSceneExtent = true;
        } else if (renderTargetWidth > 0.0f && renderTargetHeight > 0.0f) {
          targetWidth = renderTargetWidth;
          targetHeight = renderTargetHeight;
        }

        const float targetArea = std::max(targetWidth * targetHeight, 1.0f);
        const float viewportArea = vp.Width * vp.Height;
        const float coverage = std::min(viewportArea, targetArea) / std::max(viewportArea, targetArea);
        const float widthCoverage = targetWidth > 0.0f ? vp.Width / targetWidth : 0.0f;
        const float heightCoverage = targetHeight > 0.0f ? vp.Height / targetHeight : 0.0f;
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

        // Aspect proximity to the output target is the strongest scene
        // signal we have: HUD strips, square shadow targets and cube faces
        // all have wildly different aspects from the output, while scene
        // viewports - scaled, anamorphic, or loading-screen sized - track it.
        const float targetAspectEarly = targetHeight > 0.0f ? targetWidth / targetHeight : 0.0f;
        const bool aspectNearTarget10 = targetAspectEarly > 0.0f
          && std::abs(candidateAspect - targetAspectEarly) <= 0.10f * targetAspectEarly;

        // A strip is small in one dimension AND aspect-divergent. A 31%
        // uniformly-scaled loading viewport is not a strip even though one
        // coverage dips below the floor (SR4 loads at 600x337 = 31%).
        const bool stripViewport = (widthCoverage < 0.35f || heightCoverage < 0.35f)
                                && !aspectNearTarget10;

        // Capped above: an oversized square depth target (2048x2048 against
        // 1080p) "covers most of the target" numerically but is not a scene.
        const bool coversMostOfTarget = widthCoverage >= 0.80f && heightCoverage >= 0.80f
                                     && widthCoverage <= 1.05f && heightCoverage <= 1.05f;
        const bool coversSceneLikeExtent = widthCoverage >= 0.55f && heightCoverage >= 0.55f;
        const bool coversMeaningfulArea = coverage >= 0.2f;

        // Internal render-scale detection. Many engines render the 3D scene
        // into a top-left-anchored sub-rectangle of the output target and
        // upscale during post (Saints Row IV uses a fixed 62.5%; dynamic
        // resolution systems roam 50-100%). The signature is a near-origin
        // viewport with UNIFORM width/height coverage whose aspect matches
        // the target aspect. These are scene viewports, not HUD strips, and
        // must drive the fallback projection even though their center is
        // offset from the target center (a 62.5% origin-anchored viewport
        // has a normalized center offset of 0.1875 - just past the centered
        // threshold). Shadow passes stay rejected: a square 1024x1024 pass
        // against a 16:10 target fails both the uniformity and the aspect
        // match.
        const float targetAspect = targetHeight > 0.0f ? targetWidth / targetHeight : 0.0f;
        const bool uniformScale = std::abs(widthCoverage - heightCoverage)
                               <= 0.05f * std::max(widthCoverage, heightCoverage);
        const bool aspectMatchesTarget = targetAspect > 0.0f
                                      && std::abs(candidateAspect - targetAspect) <= 0.05f * targetAspect;
        // Upper bound 2.05 admits supersampled scene targets (SSAA renders
        // at up to 2x per axis); uniformity + aspect match keep shadow
        // targets out regardless.
        const bool renderScaleViewport = nearOrigin
                                      && uniformScale
                                      && aspectMatchesTarget
                                      && widthCoverage >= 0.35f
                                      && widthCoverage <= 2.05f;

        // Sub-native render targets anchored at the origin: engines that
        // render at 55-85% of the output without centering (Saints Row IV's
        // fixed 62.5% among them). Uniformity is NOT required here, unlike
        // renderScaleViewport, so anamorphic internal targets also pass.
        const bool subNativeOriginViewport = coversSceneLikeExtent && nearOrigin
                                          && aspectNearTarget10;

        // Loading screens render small origin-anchored rects (SR4: 600x337,
        // 31% of output) after the scene extent has stabilized, which the
        // unstable-only nearOrigin path below cannot accept. Allow them when
        // the aspect still matches the output - that keeps square shadow
        // passes (aspect 1.0 against a widescreen target) rejected.
        const bool nearOriginSceneAspect =
             nearOrigin
          && widthCoverage >= kMinNearOriginCoverage
          && heightCoverage >= kMinNearOriginCoverage
          && targetAspect > 0.0f
          && std::abs(candidateAspect - targetAspect) <= 0.10f * targetAspect;

        const bool acceptViewportFallback =
             usableViewport
          && plausibleSceneAspect
          && !stripViewport
          && (
               coversMostOfTarget
            || renderScaleViewport
            || subNativeOriginViewport
            || nearOriginSceneAspect
            || (coversSceneLikeExtent && centeredViewport)
            || (!haveStableSceneExtent && (coversMeaningfulArea && centeredViewport))
            || (!haveStableSceneExtent && nearOrigin)
             );

        if (acceptViewportFallback) {
          const float aspect = candidateAspect > 0.0f ? candidateAspect : fallbackReferenceAspect;
          // DX11_V260: per-game tunable (rtx.fallbackCameraFovDegrees) - a
          // fixed guess can never match every engine, and a wrong FOV makes
          // the traced image zoom-mismatch the raster view.
          const float fovDegrees = std::max(20.0f, std::min(140.0f, fallbackCameraFovDegrees()));
          const float fovY   = fovDegrees * (3.14159265f / 180.0f);
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
              "[D3D11Rtx] No projection found in cbuffers â€” using viewport fallback (",
              "x=", vp.TopLeftX,
              " y=", vp.TopLeftY,
              " w=", vp.Width,
              " h=", vp.Height,
              " aspect=", aspect,
              " coverage=", coverage,
              " widthCov=", widthCoverage,
              " heightCov=", heightCoverage,
              " centered=", centeredViewport ? 1 : 0,
              " remixViewport=", m_lastRemixViewportExtent.width, "x", m_lastRemixViewportExtent.height,
              " output=", targetWidth, "x", targetHeight,
              ")"));
          }
        } else {
          static bool s_fallbackRejectedLogged = false;
          if (!s_fallbackRejectedLogged) {
            s_fallbackRejectedLogged = true;
            Logger::info(str::format(
              "[D3D11Rtx] No projection found in cbuffers â€” skipping viewport fallback for implausible scene viewport (",
              "x=", vp.TopLeftX,
              " y=", vp.TopLeftY,
              " w=", vp.Width,
              " h=", vp.Height,
              " coverage=", coverage,
              " widthCov=", widthCoverage,
              " heightCov=", heightCoverage,
              " centered=", centeredViewport ? 1 : 0,
              " remixViewport=", m_lastRemixViewportExtent.width, "x", m_lastRemixViewportExtent.height,
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
          Matrix4 c = readMatrixWithConvention(ptr, m_viewOffset, cb.buffer->Desc()->ByteWidth, m_viewColumnMajor);
          Matrix4 resolvedView;
          if (resolveViewMatrixCandidate(c, resolvedView)) {
            // DX11_V260_PRECISE_CAMERA: a confirmed camera-to-world location
            // stores the inverse of the view - flip it back on every re-read.
            if (m_viewInverted) {
              const Matrix4 inv = inverseAffine(resolvedView);
              if (isFiniteMatrix(inv)) {
                transforms.worldToView = inv;
                viewCacheHit = true;
              }
            } else {
              transforms.worldToView = resolvedView;
              viewCacheHit = true;
            }
          }
        }
      }
    }

    // --- VIEW CONFIRMATION AGAINST A STORED VIEWPROJ (DX11_V260_PRECISE_CAMERA) ---
    // The rigid-body test alone cannot tell the main camera view from shadow-
    // light views, mirror/reflection cameras, bone matrices, or a stored
    // camera-to-world (an inverse view is exactly as rigid). Engines routinely
    // upload View, Projection AND their ViewProj product in the same cbuffer,
    // which gives a decisive test: only the true view composed with the RAW
    // projection reproduces the stored ViewProj. On a match, lock the location
    // (m_viewConfirmed) so the heuristic scans can never displace it, and
    // remember whether the stored matrix needs inversion. Both composition
    // orders and both matrix conventions are tried, so this is layout-proof.
    if (!m_viewConfirmed && haveRawProjNormalized
     && projSlot != UINT32_MAX && projStage >= 0 && projStage < kNumStages) {
      const uint32_t curFrame = m_context->m_device->getCurrentFrameId();
      // STRICTLY once per frame. The first version ran on every draw for the
      // session's first 3600 frames; multiplied by in-game draw counts
      // (hundreds+) that ground gameplay to seconds per frame the moment the
      // player left the menu - "ray tracing freezes the game". One bounded
      // attempt per frame confirms within seconds on engines that store a
      // ViewProj and costs a fixed sliver on engines that never do.
      const bool mayAttempt = m_lastViewConfirmFrame != curFrame;
      const auto& cb = (*stageCbs[projStage])[projSlot];
      if (mayAttempt && cb.buffer != nullptr) {
        m_lastViewConfirmFrame = curFrame;
        const auto mapped = cb.buffer->GetMappedSlice();
        const uint8_t* ptr = reinterpret_cast<const uint8_t*>(mapped.mapPtr);
        if (ptr) {
          const size_t bufSize = cb.buffer->Desc()->ByteWidth;
          auto [cfBase, cfEndFull] = cbRange(cb);
          // Camera blocks live in the first few KB of a camera cbuffer;
          // never pay the emulator-sized deep-scan window here.
          const size_t cfEnd = std::min(cfEndFull, cfBase + size_t(8192));

          auto matricesNearlyEqual = [](const Matrix4& a, const Matrix4& b) -> bool {
            float maxRef = 1.0f;
            float maxDiff = 0.0f;
            for (int r = 0; r < 4; ++r) {
              for (int c = 0; c < 4; ++c) {
                if (!std::isfinite(a[r][c]) || !std::isfinite(b[r][c]))
                  return false;
                maxRef = std::max(maxRef, std::abs(b[r][c]));
                maxDiff = std::max(maxDiff, std::abs(a[r][c] - b[r][c]));
              }
            }
            return maxDiff <= 0.02f * maxRef;
          };

          // Pass 1: rigid candidates from this cbuffer, plus each candidate's
          // inverse (the stored matrix may be camera-to-world). Capped.
          struct ViewCandidate {
            Matrix4 view;
            size_t offset;
            bool columnMajor;
            bool inverted;
          };
          ViewCandidate cands[8];
          uint32_t candCount = 0;
          for (size_t off = cfBase; off + 64 <= cfEnd && candCount + 2 <= 8; off += 16) {
            if (off == projOffset) continue;
            Matrix4 resolvedView;
            bool resolvedColumnMajor = false;
            if (!resolveViewAt(ptr, off, bufSize, m_columnMajor, true, resolvedView, resolvedColumnMajor))
              continue;
            cands[candCount++] = { resolvedView, off, resolvedColumnMajor, false };
            const Matrix4 inv = inverseAffine(resolvedView);
            if (isFiniteMatrix(inv))
              cands[candCount++] = { inv, off, resolvedColumnMajor, true };
          }

          // Pass 2: ViewProj-shaped blocks (finite, non-affine, not a pure
          // projection, enough non-zero structure to be a real composition),
          // capped - then match candidates against ONLY those. This keeps the
          // multiply count fixed instead of offsets x candidates.
          struct VpBlock {
            Matrix4 m;
            size_t offset;
          };
          VpBlock vps[12];
          uint32_t vpCount = 0;
          for (size_t off = cfBase; off + 64 <= cfEnd && candCount > 0 && vpCount < 12; off += 16) {
            if (off == projOffset) continue;
            for (int convIdx = 0; convIdx < 2 && vpCount < 12; ++convIdx) {
              const Matrix4 stored = readMatrixWithConvention(
                ptr, off, bufSize, convIdx == 0 ? m_columnMajor : !m_columnMajor);
              if (!isFiniteMatrix(stored) || isAffineMatrix(stored) || classifyPerspective(stored) != 0)
                continue;
              uint32_t nonZero = 0;
              for (int r = 0; r < 4; ++r)
                for (int c = 0; c < 4; ++c)
                  nonZero += stored[r][c] != 0.0f ? 1u : 0u;
              if (nonZero < 8)
                continue;  // padding / vectors / mostly-zero garbage
              vps[vpCount++] = { stored, off };
            }
          }

          bool locked = false;
          for (uint32_t vi = 0; vi < vpCount && !locked; ++vi) {
            const Matrix4& stored = vps[vi].m;
            const size_t off = vps[vi].offset;
              for (uint32_t ci = 0; ci < candCount && !locked; ++ci) {
                if (cands[ci].offset == off) continue;
                if (matricesNearlyEqual(cands[ci].view * rawProjNormalized, stored)
                 || matricesNearlyEqual(rawProjNormalized * cands[ci].view, stored)) {
                  transforms.worldToView = cands[ci].view;
                  m_viewStage = projStage;
                  m_viewSlot = projSlot;
                  m_viewOffset = cands[ci].offset;
                  m_viewColumnMajor = cands[ci].columnMajor;
                  m_viewInverted = cands[ci].inverted;
                  m_viewConfirmed = true;
                  viewCacheHit = true;
                  locked = true;
                  static bool s_viewConfirmedLogged = false;
                  if (!s_viewConfirmedLogged) {
                    s_viewConfirmedLogged = true;
                    Logger::info(str::format(
                      "[D3D11Rtx] View matrix CONFIRMED against stored ViewProj: stage=",
                      kStageNames[projStage], " slot=", projSlot,
                      " viewOff=", cands[ci].offset, " vpOff=", off,
                      cands[ci].inverted ? " [stored as camera-to-world]" : "",
                      cands[ci].columnMajor ? " [column-major]" : " [row-major]"));
                  }
                }
              }
          }
        }
      }
    }

    // Full scan fallback â€” same logic as before, but caches the result.
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
                m_viewColumnMajor = m_columnMajor;
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
                  m_viewColumnMajor = m_columnMajor;
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
                m_viewColumnMajor = m_columnMajor;
                break;
              }
            }
            if (!isIdentityExact(transforms.worldToView)) break;
          }
        }
      }

      // Convention fallback: if no view matrix was found, the column-major
      // detection may be wrong (ambiguous when near plane â‰ˆ 1). Retry with
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
                m_viewColumnMajor = !m_columnMajor;
                m_columnMajor = !m_columnMajor;
                break;
              }
            }
          }
        }
      }

      // Mixed-layout fallback: some engines compile one shader with row-major
      // matrices and another with column-major matrices, or pack camera data in
      // a different stage from projection. Retry both conventions across all
      // raster stages before giving up on the frame's view matrix.
      if (isIdentityExact(transforms.worldToView)) {
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
              Matrix4 resolvedView;
              bool resolvedColumnMajor = m_columnMajor;
              if (resolveViewAt(ptr, off, bufSize, m_columnMajor, true, resolvedView, resolvedColumnMajor)) {
                transforms.worldToView = resolvedView;
                m_viewStage = si; m_viewSlot = slot; m_viewOffset = off;
                m_viewColumnMajor = resolvedColumnMajor;

                static uint32_t sMixedViewLayoutLogCount = 0;
                if (resolvedColumnMajor != m_columnMajor && sMixedViewLayoutLogCount < 8) {
                  ++sMixedViewLayoutLogCount;
                  Logger::info(str::format(
                    "[D3D11Rtx] View matrix recovered with mixed row/column-major layout: stage=",
                    kStageNames[si],
                    " slot=",
                    slot,
                    " off=",
                    off));
                }
                break;
              }
            }
            if (!isIdentityExact(transforms.worldToView)) break;
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
            Matrix4 resolvedView;
            bool resolvedColumnMajor = m_columnMajor;
            if (resolveViewAt(ptr, off, bufSize, m_columnMajor, true, resolvedView, resolvedColumnMajor)) {
              transforms.worldToView = resolvedView;
              m_viewStage = si; m_viewSlot = slot; m_viewOffset = off;
              m_viewColumnMajor = resolvedColumnMajor;
              break;
            }
          }
          if (!isIdentityExact(transforms.worldToView)) break;
        }
      }
    }

    // --- VIEW MATRIX: full scan of the projection's own cbuffer ---
    // DX11_V256_VIEW_IN_PROJ_CBUFFER: engines commonly pack the whole camera
    // block [Proj | View | inverses | ...] into ONE cbuffer, with the view at
    // an arbitrary offset (Saints Row IV: proj at slot 2 off 0, column-major
    // view at off 352). The broad view scan above only runs when NO projection
    // was found, so such views were missed entirely (log: "view=NO") and the
    // RT camera sat at the origin. Scan every offset of the projection's
    // cbuffer, both matrix conventions, skipping the projection itself.
    if (isIdentityExact(transforms.worldToView)
     && projSlot != UINT32_MAX && projStage >= 0 && projStage < kNumStages) {
      const auto& cb = (*stageCbs[projStage])[projSlot];
      if (cb.buffer != nullptr) {
        const auto mapped = cb.buffer->GetMappedSlice();
        const uint8_t* ptr = reinterpret_cast<const uint8_t*>(mapped.mapPtr);
        if (ptr) {
          const size_t bufSize = cb.buffer->Desc()->ByteWidth;
          auto [scanBase, scanEnd] = cbRange(cb);
          for (size_t off = scanBase; off + 64 <= scanEnd; off += 16) {
            if (off == projOffset) continue;
            Matrix4 resolvedView;
            bool resolvedColumnMajor = false;
            if (resolveViewAt(ptr, off, bufSize, m_columnMajor, true, resolvedView, resolvedColumnMajor)) {
              transforms.worldToView = resolvedView;
              m_viewStage = projStage;
              m_viewSlot = projSlot;
              m_viewOffset = off;
              m_viewColumnMajor = resolvedColumnMajor;
              static bool s_projCbViewLogged = false;
              if (!s_projCbViewLogged) {
                s_projCbViewLogged = true;
                Logger::info(str::format("[D3D11Rtx] View matrix found in projection cbuffer: stage=",
                  kStageNames[projStage], " slot=", projSlot, " off=", off,
                  resolvedColumnMajor ? " [column-major]" : " [row-major]"));
              }
              break;
            }
          }
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
      // DX11_V260_PRECISE_CAMERA: invert the projection AS THE ENGINE STORED
      // IT. The engine built its ViewProj with the original matrix; inverting
      // the jitter-stripped, orientation-canonicalized copy is off by the
      // jitter terms and - when canonicalization flipped an axis - produces a
      // mirrored "view" that still passes the rigid-body test.
      Matrix4 projInv = inverse(haveRawProjNormalized ? rawProjNormalized
                                                      : transforms.viewToProjection);
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
              const bool matrixColumnMajorOptions[] = { m_columnMajor, !m_columnMajor };
              for (bool matrixColumnMajor : matrixColumnMajorOptions) {
                Matrix4 M = readMatrixWithConvention(ptr, off, bufSize, matrixColumnMajor);
                if (isIdentityExact(M)) continue;

                const Matrix4 viewProjOrders[] = {
                  M * projInv,
                  projInv * M,
                };
                for (uint32_t order = 0; order < 2; ++order) {
                  Matrix4 resolvedView;
                  if (resolveViewMatrixCandidate(viewProjOrders[order], resolvedView)) {
                    transforms.worldToView = resolvedView;
                    m_viewStage = si; m_viewSlot = slot; m_viewOffset = off;
                    m_viewColumnMajor = matrixColumnMajor;
                    static bool s_vpLogged = false;
                    if (!s_vpLogged) {
                      s_vpLogged = true;
                      Logger::info(str::format(
                        "[D3D11Rtx] View derived from ViewProj decomposition: stage=",
                        kStageNames[si],
                        " slot=",
                        slot,
                        " off=",
                        off,
                        " order=",
                        order == 0 ? "ViewProj*InvProj" : "InvProj*ViewProj",
                        matrixColumnMajor != m_columnMajor ? " mixed-layout" : ""));
                    }
                    break;
                  }
                }
                if (!isIdentityExact(transforms.worldToView)) break;
              }
              if (!isIdentityExact(transforms.worldToView)) break;
            }
            if (!isIdentityExact(transforms.worldToView)) break;
          }
        }
      }
    }

    // DX11_V260_PRECISE_CAMERA: if a heuristic scan just cached a fresh view
    // location (viewCacheHit false but a view was found), it was a direct,
    // unconfirmed read - only the confirmation pass may set the inverted
    // flag, and a re-discovered location must re-earn confirmed status.
    if (!viewCacheHit && !isIdentityExact(transforms.worldToView)) {
      m_viewConfirmed = false;
      m_viewInverted = false;
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
          RtCamera::correctProjectionYFlipObject().setDeferred(yFlip, RtxOptionLayer::getDerivedLayer());
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
            RtxOptions::leftHandedCoordinateSystemObject().setDeferred(isLH, RtxOptionLayer::getDerivedLayer());

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
        // Only vote when there's a clear winner (avoid ambiguous 45Â° views)
        if (std::abs(absZ - absY) > 0.3f) {
          m_zUpVotes += (absZ > absY) ? 1 : -1;
          if (!m_zUpSettled && std::abs(m_zUpVotes) >= kVoteThreshold) {
            m_zUpSettled = true;
            const bool zUp = m_zUpVotes > 0;
            RtxOptions::zUpObject().setDeferred(zUp, RtxOptionLayer::getDerivedLayer());
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
    // untouched â€” rotation jitter is rare and smoothing it causes ghosting.
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
    // NOT by structural isViewMatrix() â€” the latter rejects unit-scale
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
        " viewConfirmed=", m_viewConfirmed ? "yes" : "no",
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

    // Content-derived identity for CPU-unreadable buffers (set at creation
    // from initial data). Stable across runs and GPU vendors, unlike the
    // pointer-based fallback below.
    const uint64_t posCookie = posBuf ? posBuf->contentCookie() : 0ull;

    auto future = m_pGeometryWorkers->Schedule([posData, tcData, idxData,
                                         posBuf, tcBuf, idxBuf,
                                         posStride, tcStride, idxStride,
                                         posLength, tcLength, idxLength,
                                         vertexCount, indexCount = geo.indexCount,
                                         posOffset, posCookie,
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
        // GPU-only buffer the CPU cannot read. Prefer the content cookie
        // (hashed from the buffer's initial data at creation): it is the
        // same value every run on every GPU vendor. The pointer-based
        // fallback below only triggers for buffers created without initial
        // data and filled purely on the GPU; its hashes are randomized by
        // ASLR each run and can collide when the allocator recycles
        // addresses - the "garbled hash" failure mode.
        if (posCookie != 0ull) {
          XXH64_hash_t posHash = XXH3_64bits(&posCookie, sizeof(posCookie));
          posHash = XXH3_64bits_withSeed(&posOffset, sizeof(posOffset), posHash);
          posHash = XXH3_64bits_withSeed(&vertexCount, sizeof(vertexCount), posHash);
          hashes[HashComponents::VertexPosition] = posHash;
        } else {
          XXH64_hash_t posHash = XXH3_64bits(&posBuf, sizeof(posBuf));
          posHash = XXH3_64bits_withSeed(&posOffset, sizeof(posOffset), posHash);
          posHash = XXH3_64bits_withSeed(&vertexCount, sizeof(vertexCount), posHash);
          hashes[HashComponents::VertexPosition] = posHash;
        }
      }

      hashes.precombine();

      // Release buffer pins â€” allow slice recycling again.
      if (posBuf) { posBuf->release(DxvkAccess::Read); posBuf->decRef(); }
      if (tcBuf)  { tcBuf->release(DxvkAccess::Read);  tcBuf->decRef();  }
      if (idxBuf) { idxBuf->release(DxvkAccess::Read); idxBuf->decRef(); }

      return hashes;
    });

    // If the worker queue was full, the lambda never runs â€” release pins now
    // to prevent a VRAM leak (incRef/acquire above would never be undone).
    if (!future.valid()) {
      if (posBuf) { posBuf->release(DxvkAccess::Read); posBuf->decRef(); }
      if (tcBuf)  { tcBuf->release(DxvkAccess::Read);  tcBuf->decRef();  }
      if (idxBuf) { idxBuf->release(DxvkAccess::Read); idxBuf->decRef(); }
    }

    return future;
  }

  TextureRef D3D11Rtx::getOrCreateUntexturedPlaceholder() {
    const auto& ps = m_context->m_state.ps;
    const void* cacheKey = ps.shader.ptr();

    auto it = m_untexturedPlaceholders.find(cacheKey);
    if (it != m_untexturedPlaceholders.end())
      return it->second;

    // Hash from the pixel shader's content-derived key so the placeholder
    // hash is identical across runs and machines (required for rtx.conf
    // tags to persist); shaderless draws share a single fixed identity.
    XXH64_hash_t hash;
    if (ps.shader != nullptr) {
      const std::string keyName = ps.shader->GetCommonShader()->GetShader()->getShaderKey().toString();
      hash = XXH3_64bits(keyName.data(), keyName.size());
    } else {
      static const char kFixedKey[] = "remix-dx11-untextured";
      hash = XXH3_64bits(kFixedKey, sizeof(kFixedKey) - 1);
    }
    if (hash == 0ull)
      hash = 1ull;

    DxvkImageCreateInfo info = {};
    info.type        = VK_IMAGE_TYPE_2D;
    info.format      = VK_FORMAT_R8G8B8A8_UNORM;
    info.flags       = 0;
    info.sampleCount = VK_SAMPLE_COUNT_1_BIT;
    info.extent      = { 4u, 4u, 1u };
    info.numLayers   = 1;
    info.mipLevels   = 1;
    info.usage       = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    info.stages      = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT
                     | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT
                     | VK_PIPELINE_STAGE_TRANSFER_BIT;
    info.access      = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    info.tiling      = VK_IMAGE_TILING_OPTIMAL;
    info.layout      = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    Rc<DxvkImage> image = m_context->m_device->createImage(
      info, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
      DxvkMemoryStats::Category::AppTexture, "remix untextured placeholder");
    image->setHash(hash);

    DxvkImageViewCreateInfo viewInfo = {};
    viewInfo.type      = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format    = info.format;
    viewInfo.usage     = VK_IMAGE_USAGE_SAMPLED_BIT;
    viewInfo.aspect    = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.minLevel  = 0;
    viewInfo.numLevels = 1;
    viewInfo.minLayer  = 0;
    viewInfo.numLayers = 1;

    Rc<DxvkImageView> view = m_context->m_device->createImageView(image, viewInfo);

    // Solid white at full alpha: Modulate(white, x) == x, so attaching the
    // placeholder cannot change the rendered result of any combiner path.
    m_context->EmitCs([cImage = image](DxvkContext* ctx) {
      // Neutral 70% gray rather than pure white: a full-albedo surface is
      // non-physical and lets path-traced light bounce without loss - rooms
      // full of untextured geometry blow out to white. Gray keeps the
      // modulate identity close (slightly darkens vertex-colored untextured
      // surfaces) while keeping bounce energy sane.
      static const uint32_t kGrayPixels[16] = {
        0xFFB4B4B4u, 0xFFB4B4B4u, 0xFFB4B4B4u, 0xFFB4B4B4u,
        0xFFB4B4B4u, 0xFFB4B4B4u, 0xFFB4B4B4u, 0xFFB4B4B4u,
        0xFFB4B4B4u, 0xFFB4B4B4u, 0xFFB4B4B4u, 0xFFB4B4B4u,
        0xFFB4B4B4u, 0xFFB4B4B4u, 0xFFB4B4B4u, 0xFFB4B4B4u };
      ctx->updateImage(cImage,
        VkImageSubresourceLayers { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        VkOffset3D { 0, 0, 0 },
        VkExtent3D { 4u, 4u, 1u },
        (void*) kGrayPixels, 4u * 4u, 4u * 4u * 4u);
    });

    TextureRef ref(view);
    m_untexturedPlaceholders.emplace(cacheKey, ref);
    return ref;
  }

  void D3D11Rtx::FillMaterialData(LegacyMaterialData& mat) const {
    const auto& ps = m_context->m_state.ps;
    uint32_t textureID = 0;

    static uint32_t s_logCount = 0;
    const bool doLog = (s_logCount < 10);

    auto isColorBlockCompressed = [](DXGI_FORMAT fmt) -> bool {
      return (fmt >= DXGI_FORMAT_BC1_TYPELESS && fmt <= DXGI_FORMAT_BC1_UNORM_SRGB)
          || (fmt >= DXGI_FORMAT_BC2_TYPELESS && fmt <= DXGI_FORMAT_BC2_UNORM_SRGB)
          || (fmt >= DXGI_FORMAT_BC3_TYPELESS && fmt <= DXGI_FORMAT_BC3_UNORM_SRGB)
          || (fmt >= DXGI_FORMAT_BC7_TYPELESS && fmt <= DXGI_FORMAT_BC7_UNORM_SRGB);
    };

    auto isDataBlockCompressed = [](DXGI_FORMAT fmt) -> bool {
      return (fmt >= DXGI_FORMAT_BC4_TYPELESS && fmt <= DXGI_FORMAT_BC4_SNORM)
          || (fmt >= DXGI_FORMAT_BC5_TYPELESS && fmt <= DXGI_FORMAT_BC5_SNORM)
          || (fmt >= DXGI_FORMAT_BC6H_TYPELESS && fmt <= DXGI_FORMAT_BC6H_SF16);
    };

    auto isBlockCompressed = [&](DXGI_FORMAT fmt) -> bool {
      return isColorBlockCompressed(fmt) || isDataBlockCompressed(fmt);
    };

    auto isLikelyAlbedoFormat = [&](DXGI_FORMAT fmt) -> bool {
      if (isColorBlockCompressed(fmt))
        return true;

      switch (fmt) {
        // Note: A8_UNORM is deliberately absent. Alpha-only textures are
        // font/UI atlases, not albedo; treating them as albedo let a
        // 2880x1088 glyph atlas win material selection and tile glyph
        // noise across world geometry whenever every other candidate was
        // rejected (observed in Sunset Overdrive).
        case DXGI_FORMAT_R8_UNORM:
        case DXGI_FORMAT_R8G8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        case DXGI_FORMAT_B8G8R8X8_UNORM:
        case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
        case DXGI_FORMAT_B5G6R5_UNORM:
        case DXGI_FORMAT_B5G5R5A1_UNORM:
        case DXGI_FORMAT_B4G4R4A4_UNORM:
          return true;
        default:
          return false;
      }
    };

    auto isStrongAlbedoFormat = [&](DXGI_FORMAT fmt) -> bool {
      if (isColorBlockCompressed(fmt))
        return true;

      switch (fmt) {
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        case DXGI_FORMAT_B8G8R8X8_UNORM:
        case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
        case DXGI_FORMAT_B5G6R5_UNORM:
        case DXGI_FORMAT_B5G5R5A1_UNORM:
        case DXGI_FORMAT_B4G4R4A4_UNORM:
          return true;
        default:
          return false;
      }
    };

    auto isLikelyDataOrSceneColorFormat = [&](DXGI_FORMAT fmt) -> bool {
      if (isDataBlockCompressed(fmt))
        return true;

      switch (fmt) {
        case DXGI_FORMAT_R10G10B10A2_UNORM:
        case DXGI_FORMAT_R10G10B10A2_UINT:
        case DXGI_FORMAT_R11G11B10_FLOAT:
        case DXGI_FORMAT_R16_FLOAT:
        case DXGI_FORMAT_R16G16_FLOAT:
        case DXGI_FORMAT_R16G16B16A16_FLOAT:
        case DXGI_FORMAT_R32_FLOAT:
        case DXGI_FORMAT_R32G32_FLOAT:
        case DXGI_FORMAT_R32G32B32_FLOAT:
        case DXGI_FORMAT_R32G32B32A32_FLOAT:
        case DXGI_FORMAT_R8_UINT:
        case DXGI_FORMAT_R8_SINT:
        case DXGI_FORMAT_R8G8_UINT:
        case DXGI_FORMAT_R8G8_SINT:
        case DXGI_FORMAT_R8G8B8A8_UINT:
        case DXGI_FORMAT_R8G8B8A8_SINT:
        case DXGI_FORMAT_R16_UINT:
        case DXGI_FORMAT_R16_SINT:
        case DXGI_FORMAT_R16G16_UINT:
        case DXGI_FORMAT_R16G16_SINT:
        case DXGI_FORMAT_R16G16B16A16_UINT:
        case DXGI_FORMAT_R16G16B16A16_SINT:
        case DXGI_FORMAT_R32_UINT:
        case DXGI_FORMAT_R32_SINT:
        case DXGI_FORMAT_R32G32_UINT:
        case DXGI_FORMAT_R32G32_SINT:
        case DXGI_FORMAT_R32G32B32_UINT:
        case DXGI_FORMAT_R32G32B32_SINT:
        case DXGI_FORMAT_R32G32B32A32_UINT:
        case DXGI_FORMAT_R32G32B32A32_SINT:
        case DXGI_FORMAT_R16_TYPELESS:
        case DXGI_FORMAT_R24G8_TYPELESS:
        case DXGI_FORMAT_R32_TYPELESS:
        case DXGI_FORMAT_R32G8X24_TYPELESS:
        case DXGI_FORMAT_D16_UNORM:
        case DXGI_FORMAT_D24_UNORM_S8_UINT:
        case DXGI_FORMAT_D32_FLOAT:
        case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
        case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
        case DXGI_FORMAT_X24_TYPELESS_G8_UINT:
        case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:
        case DXGI_FORMAT_X32_TYPELESS_G8X24_UINT:
        case DXGI_FORMAT_R8G8_B8G8_UNORM:
        case DXGI_FORMAT_G8R8_G8B8_UNORM:
          return true;
        default:
          return false;
      }
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
    // We only need kMaxSupportedTextures (2) winners â€” a full sort is unnecessary.
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

    auto registerRemixTextureCandidate = [](const Rc<DxvkImageView>& imageView) {
      if (imageView == nullptr)
        return;

      TextureRef previewRef(imageView);
      const XXH64_hash_t textureHash = previewRef.getImageHash();
      if (textureHash != 0) {
        ImGUI::AddTexture(textureHash, imageView, getTextureUiFeatureFlagsForView(imageView));
      }
    };

    for (uint32_t slot = 0; slot < D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT; ++slot) {
      D3D11ShaderResourceView* srv = ps.shaderResources.views[slot].ptr();
      if (!srv) continue;
      if (srv->GetResourceType() != D3D11_RESOURCE_DIMENSION_TEXTURE2D) continue;

      Rc<DxvkImageView> view = srv->GetImageView();
      if (view == nullptr) continue;

      const auto& imgInfo = view->image()->info();
      const auto& viewInfo = view->info();
      D3D11_SHADER_RESOURCE_VIEW_DESC1 srvDesc = {};
      srv->GetDesc1(&srvDesc);
      const D3D11_COMMON_RESOURCE_DESC resourceDesc = srv->GetResourceDesc();
      const DXGI_FORMAT fmt = srvDesc.Format;
      const bool bc = isBlockCompressed(fmt);
      const bool colorBc = isColorBlockCompressed(fmt);
      const bool dataOrSceneFormat = isLikelyDataOrSceneColorFormat(fmt);
      const bool albedoFormat = isLikelyAlbedoFormat(fmt);
      const bool strongAlbedoFormat = isStrongAlbedoFormat(fmt);
      const bool texture2DView = srvDesc.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2D;
      const bool singleSliceTexture2DArrayView = srvDesc.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2DARRAY
        && srvDesc.Texture2DArray.ArraySize == 1;
      const bool materialViewDimension = texture2DView || singleSliceTexture2DArrayView;
      const bool multisampledView = srvDesc.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2DMS
        || srvDesc.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2DMSARRAY
        || imgInfo.sampleCount != VK_SAMPLE_COUNT_1_BIT;

      // Reject Texture2DArray SRVs that cover multiple slices â€” each slice is a separate
      // game texture that hashes identically, causing surfaces to appear "smashed together".
      // Single-slice array views are safe: getImageHash() mixes in the layer index.
      if (srvDesc.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2DARRAY
          && srvDesc.Texture2DArray.ArraySize > 1)
        continue;
      const bool hasMips = viewInfo.numLevels > 1 || (viewInfo.numLevels == 0 && imgInfo.mipLevels > 1);
      const bool hasHazardBindFlags = srv->TestHazards() != FALSE;
      const bool hasRtBind = (resourceDesc.BindFlags & D3D11_BIND_RENDER_TARGET) != 0;
      const bool hasUavBind = (resourceDesc.BindFlags & D3D11_BIND_UNORDERED_ACCESS) != 0;
      const bool hasDepthBind = (resourceDesc.BindFlags & D3D11_BIND_DEPTH_STENCIL) != 0;
      const bool isSingleMipLargeTexture = !hasMips && !bc && isLargeTexture(imgInfo.extent);

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
      const bool rtSizedIntermediate = matchesRT
        && (hasHazardBindFlags || isSingleMipLargeTexture || dataOrSceneFormat);
      const bool likelyIntermediate = multisampledView
        || isCurrentRT
        || rtSizedIntermediate
        || ((hasRtBind || hasUavBind || hasDepthBind) && isSingleMipLargeTexture && !strongAlbedoFormat);
      const bool rejectTextureBrowserCandidate = !materialViewDimension
        || multisampledView
        || isCurrentRT
        || rtSizedIntermediate
        || (hasDepthBind && !hasMips)
        || ((hasRtBind || hasUavBind) && isSingleMipLargeTexture && !bc);
      const bool rejectMaterialCandidate = rejectTextureBrowserCandidate
        || likelyIntermediate
        || !albedoFormat
        || dataOrSceneFormat;

      int score = 0;
      if (colorBc)                  score += 14;  // Color BC = strong material signal.
      else if (bc)                  score -= 10;  // BC4/BC5/BC6 are masks/normals/HDR, not albedo.
      if (strongAlbedoFormat)       score += 8;
      else if (albedoFormat)        score += 2;
      if (hasMips)                  score += 5;   // Mipmapped = likely content
      if (!matchesRT)               score += 3;   // Different size from RT = likely content
      if (!isCurrentRT)             score += 2;   // Not actively rendering to it
      score += std::max(0, 16 - (int)slot);       // Prefer lower slots (albedo first)

      if (dataOrSceneFormat)
        score -= 24;
      if (!materialViewDimension)
        score -= 16;
      if (multisampledView)
        score -= 32;

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

      // Currently bound as active RT â†’ negative score (only use as absolute last resort)
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
            if (strongAlbedoFormat && hasMips && !matchesRT && !hasUavBind)
              score += 3;
            if (dataOrSceneFormat || likelyIntermediate)
              score -= 16;
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

      const bool likelyNormalLikeTexture =
        !colorBc &&
        !strongAlbedoFormat &&
        !dataOrSceneFormat &&
        materialViewDimension &&
        !multisampledView &&
        !matchesRT &&
        !hasUavBind &&
        (fmt == VK_FORMAT_R8G8B8A8_UNORM
          || fmt == VK_FORMAT_R8G8_UNORM
          || fmt == VK_FORMAT_R8G8_SNORM
          || fmt == VK_FORMAT_R16G16_UNORM
          || fmt == VK_FORMAT_R16G16_SNORM
          || fmt == VK_FORMAT_R16G16_SFLOAT
          || fmt == static_cast<VkFormat>(65));

      const bool likelyAtlasOrHelperTexture =
        !bc &&
        !colorBc &&
        !hasMips &&
        materialViewDimension &&
        (imgInfo.extent.width <= 256 || imgInfo.extent.height <= 256);

      if (likelyNormalLikeTexture)
        score -= 18;
      if (likelyAtlasOrHelperTexture)
        score -= 10;

      if (doLog) {
        Logger::info(str::format("[D3D11Rtx] FillMaterialData tex candidate: slot=", slot,
          " fmt=", (uint32_t)fmt,
          " w=", imgInfo.extent.width, " h=", imgInfo.extent.height,
          " mips=", imgInfo.mipLevels,
          " viewMips=", viewInfo.numLevels,
          " score=", score,
          bc ? " [BC]" : "",
          colorBc ? " [COLOR-BC]" : "",
          dataOrSceneFormat ? " [DATA/SCENE-FMT]" : "",
          albedoFormat ? " [ALBEDO-FMT]" : "",
          hasMips ? " [MIPS]" : "",
          hasHazardBindFlags ? " [HAZARD]" : "",
          hasRtBind ? " [RT-BIND]" : "",
          hasUavBind ? " [UAV-BIND]" : "",
          hasDepthBind ? " [DEPTH-BIND]" : "",
          !materialViewDimension ? " [NON-2D-MATERIAL-VIEW]" : "",
          multisampledView ? " [MSAA]" : "",
          isSingleMipLargeTexture ? " [SINGLE-MIP-LARGE]" : "",
          likelyIntermediate ? " [LIKELY-INTERMEDIATE]" : "",
          isCurrentRT ? " [BOUND-RT]" : "",
          matchesRT ? " [RT-SIZED]" : "",
          rejectTextureBrowserCandidate ? " [REJECT-BROWSER]" : "",
          rejectMaterialCandidate ? " [REJECT-MATERIAL]" : ""));
      }

      // The legacy material can only bind a small number of color textures,
      // but Remix tooling still needs to see every safe game material texture
      // encountered by the draw stream.
      const bool safeForTextureBrowser =
        !rejectTextureBrowserCandidate &&
        (hasMips || bc || albedoFormat) &&
        !likelyNormalLikeTexture &&
        !likelyAtlasOrHelperTexture;

      if (safeForTextureBrowser)
        registerRemixTextureCandidate(view);

      if (rejectMaterialCandidate)
        continue;

      // Keep low-confidence non-albedo candidates out of the legacy material path.
      // This prevents small helper textures and likely normal/packed textures from
      // being merged into the two legacy color slots.
      if (score < 8 && !strongAlbedoFormat && !colorBc)
        continue;

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

    // Untextured draws are invisible to every texture tool: no hash in the
    // browser, nothing for viewport object picking to hit, nothing to tag as
    // ignore/UI/sky. With ray tracing on, an untextured surface that fills
    // the screen is therefore impossible to select or remove from inside the
    // UI. Attach a tiny solid-white placeholder whose hash derives from the
    // pixel shader identity: white modulates to the same visual result as
    // the untextured path, the hash is stable across runs and GPUs, and the
    // draw becomes a first-class taggable citizen (including ignore-tags
    // that remove it entirely).
    // Depth-only prepass and shadow draws bind no color target; attaching a
    // placeholder there turned invisible prepass geometry into solid albedo
    // surfaces inside the path-traced scene (observed as a blown-out white
    // screen in Unity titles, whose prepass draws have zero texture
    // candidates). Only color-writing draws get the placeholder.
    const bool writesColor = m_context->m_state.om.renderTargetViews[0].ptr() != nullptr;
    if (textureID == 0 && writesColor && RtxOptions::taggableUntexturedDraws()) {
      TextureRef placeholder = const_cast<D3D11Rtx*>(this)->getOrCreateUntexturedPlaceholder();
      if (placeholder.getImageViewRc() != nullptr) {
        mat.colorTextures[0] = std::move(placeholder);
        mat.colorTextureSlot[0] = kInvalidResourceSlot;
        mat.samplers[0] = getDefaultSampler();
        textureID = 1;
      }
    }

    if (doLog) {
      Logger::info(str::format("[D3D11Rtx] FillMaterialData draw #", s_logCount,
        " picked ", textureID, " of ", pickCount, " candidate(s)"));
      // Count every logged draw, not just draws that picked a texture.
      // Previously the counter only advanced when pickCount > 0, so in
      // deferred engines where most draws reject all candidates the 10-draw
      // cap never engaged and the per-draw candidate logging ran forever --
      // tens of thousands of str::format + log writes on the draw hot path
      // (a measurable CPU bottleneck and 30k+ line logs).
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
    // D3D11 bakes blending/alpha into immutable state objects â€” we extract
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

    // forceInjection overflow guard: when injection is forced but the
    // previous frame produced zero scene instances, only the first
    // kForceInjectionProbeDraws draws are considered (enough for camera and
    // scene discovery - SR4 finds its camera at drawCallID 41). Everything
    // past the window is rejected before geometry processing, so the
    // Remix UI and composite stay alive while the acceleration structure
    // stays empty instead of rebuilding 6k junk instances per frame.
    if (RtxOptions::forceInjection()
     && m_prevFrameSceneAccepted == 0
     && m_prevFrameRealSceneAccepted == 0
     && m_submitRejectStats.total > kForceInjectionProbeDraws) {
      ++m_submitRejectStats.forceInjectionIdle;
      return;
    }

    // Throttle: don't exceed the worker ring buffer capacity.
    // Beyond this point new futures would overwrite in-flight ones â†’ corrupt hashes.
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
    // Most engines draw opaque geometry twice â€” once for depth prepass (PS == null)
    // and once for the color pass (PS != null) with the same vertices.
    if (m_context->m_state.ps.shader == nullptr) {
      ++m_submitRejectStats.noPixelShader;
      return;
    }

    const auto& omState = m_context->m_state.om;
    const bool hasColorRenderTarget = std::any_of(
      omState.renderTargetViews.begin(),
      omState.renderTargetViews.end(),
      [](const auto& rtv) { return rtv.ptr() != nullptr; });
    const bool hasDepthStencilTarget = omState.depthStencilView.ptr() != nullptr;

    // Skip draws with no output target at all. Depth-only draws with a pixel
    // shader can still carry real world geometry and material textures, so
    // feed those to Remix instead of tying visibility to raster color output.
    if (!hasColorRenderTarget && !hasDepthStencilTarget) {
      ++m_submitRejectStats.noRenderTarget;
      return;
    }

    // Skip trivially small draws (< 3 elements = 0 triangles).
    if (count < 3) {
      ++m_submitRejectStats.trivialDraw;
      return;
    }

    // Read actual depth/stencil state from the OM â€” don't hardcode.
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
    // Only skip if BOTH depth test and write are off â€” some engines do
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
    if (!tcSem)
      tcSem = selectBestSemantic(semantics, scoreTexcoordFallbackSemantic, { posSem });

    if (tcSem
     && !semanticNameStartsWith(*tcSem, "TEXCOORD")
     && !semanticNameStartsWith(*tcSem, "TEX")
     && !semanticNameStartsWith(*tcSem, "UV")
     && !semanticNameStartsWith(*tcSem, "TCOORD")
     && !semanticNameStartsWith(*tcSem, "MAP")) {
      static uint32_t sTexcoordDiscoverLogCount = 0;
      if (sTexcoordDiscoverLogCount < 32) {
        ++sTexcoordDiscoverLogCount;
        Logger::info(str::format(
          "[D3D11Rtx] Selected fallback TEXCOORD semantic: ",
          tcSem->name,
          tcSem->index,
          " fmt=",
          static_cast<uint32_t>(tcSem->format),
          " comps=",
          tcSem->componentCount,
          " reg=",
          tcSem->registerId));
      }
    }

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
    //
    // Caveat: some engines emit billboard/sprite geometry as R32G32_SFLOAT quads
    // and expand them into 3D inside the vertex shader using the camera basis.
    // Those draws are valid 3D content and participate in world-space lighting,
    // so they depth-test against the scene. Only reject 2D-position draws that
    // ALSO have depth testing off â€” which is the unambiguous HUD / overlay case.
    if (posSem->format == VK_FORMAT_R32G32_SFLOAT && !zEnable) {
      ++m_submitRejectStats.position2D;
      return;
    }

    // D3D11 draws address the vertex buffer through BaseVertexLocation (indexed
    // draws) or StartVertexLocation (non-indexed draws), and indexed draws read
    // the index buffer starting at StartIndexLocation. Remix does none of this:
    // it reads indices from the start of the bound slice and fetches vertices by
    // the raw index value (no base added) - see RtxGeometryUtils::cacheIndexData
    // OnGPU and RasterGeometry::printDebugInfo. So the base/start offsets must be
    // folded into the buffer slices here. Without it, engines that pack many
    // sub-meshes into one shared vertex/index buffer (the DX11 norm) resolve
    // every non-zero-base draw to the wrong vertices, which makes the geometry
    // collapse toward one point and stretch into spikes when ray traced.
    const int64_t vertexStartIndex = indexed ? int64_t(base) : int64_t(start);

    auto makeVertexBuffer = [&](const D3D11RtxSemantic* sem) -> RasterBuffer {
      if (!sem)
        return RasterBuffer();
      const auto& vb = m_context->m_state.ia.vertexBuffers[sem->inputSlot];
      if (vb.buffer == nullptr)
        return RasterBuffer();
      // Advance the slice to the first vertex this draw touches. Per-slot stride
      // is used so multi-stream layouts stay correct. A negative BaseVertexLocation
      // that would move the slice before the buffer start cannot be represented,
      // so skip the attribute (the draw is dropped when this is the position).
      const int64_t sliceOffset = int64_t(vb.offset) + vertexStartIndex * int64_t(vb.stride);
      if (sliceOffset < 0)
        return RasterBuffer();
      DxvkBufferSlice slice = vb.buffer->GetBufferSlice(static_cast<VkDeviceSize>(sliceOffset));
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

    // Color0: the interleaver's uint path accepts ONLY B8G8R8A8_UNORM, but the
    // capture admits the formats games actually declare for COLOR0 and the
    // DX11_V268 normalization below converts them all into that layout:
    // packed bytes (BGRA/RGBA), float4 (very common in modern engines - was
    // silently dropped, washing baked lighting/tinting out to white), half4
    // and unorm16 (COLOR-named only; those formats double as normal/tangent
    // storage under other names).
    RasterBuffer colBuffer;
    if (colSem) {
      const VkFormat cf = colSem->format;
      const bool packedByteColor = cf == VK_FORMAT_B8G8R8A8_UNORM
                                || cf == VK_FORMAT_R8G8B8A8_UNORM;
      const bool wideColor = cf == VK_FORMAT_R32G32B32A32_SFLOAT
                          || ((cf == VK_FORMAT_R16G16B16A16_UNORM
                            || cf == VK_FORMAT_R16G16B16A16_SFLOAT)
                           && semanticNameStartsWith(*colSem, "COLOR"));
      if (packedByteColor || wideColor) {
        colBuffer = makeVertexBuffer(colSem);
      }
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
      // Skip StartIndexLocation indices so element 0 of the slice is the first
      // index this draw consumes (Remix always reads from the slice start).
      const VkDeviceSize idxSliceOffset = VkDeviceSize(ib.offset) + VkDeviceSize(start) * idxStride;
      idxBuffer = RasterBuffer(ib.buffer->GetBufferSlice(idxSliceOffset), 0, idxStride, idxType);
      if (!idxBuffer.defined()) {
        ++m_submitRejectStats.noIndexBuffer;
        return;
      }
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

    // Compute vertex count â€” must cover the highest vertex index accessed by
    // this draw so Remix doesn't read out of bounds when building the BLAS.
    // The position slice now starts at the draw's first vertex (base/start folded
    // in above), so all counts below are relative to that origin.
    const uint32_t maxVBVertices = posBuffer.stride() > 0
      ? static_cast<uint32_t>(posBuffer.length() / posBuffer.stride())
      : count;
    uint32_t drawVertexCount;
    uint32_t hashStart = 0;
    uint32_t hashCount;
    if (!indexed) {
      // Non-indexed: relative vertices [0, count) after the start offset.
      drawVertexCount = std::min(count, maxVBVertices);
      hashCount = drawVertexCount;
    } else {
      // Indexed: index values are relative to the base vertex, so the highest
      // one referenced determines how many vertices Remix must copy. Scan the
      // (CPU-visible) index range for the exact maximum; fall back to the whole
      // remaining vertex buffer when the indices can't be read here or the draw
      // is too large to scan cheaply on the submit thread.
      uint32_t maxIndexPlusOne = 0;
      const uint32_t idxStrideBytes = std::max(idxBuffer.stride(), 1u);
      const void* idxScan = idxBuffer.defined() ? idxBuffer.mapPtr(0) : nullptr;
      const uint32_t idxAvail = idxBuffer.defined()
        ? static_cast<uint32_t>(idxBuffer.length() / idxStrideBytes)
        : 0u;
      const uint32_t scanCount = std::min(count, idxAvail);
      static constexpr uint32_t kMaxIndexScan = 4u << 20; // cap submit-thread work
      if (idxScan && scanCount > 0 && scanCount <= kMaxIndexScan) {
        if (idxBuffer.indexType() == VK_INDEX_TYPE_UINT32) {
          const uint32_t* ip = static_cast<const uint32_t*>(idxScan);
          for (uint32_t i = 0; i < scanCount; ++i)
            maxIndexPlusOne = std::max(maxIndexPlusOne, ip[i] + 1u);
        } else {
          const uint16_t* ip = static_cast<const uint16_t*>(idxScan);
          for (uint32_t i = 0; i < scanCount; ++i)
            maxIndexPlusOne = std::max(maxIndexPlusOne, uint32_t(ip[i]) + 1u);
        }
      }
      if (maxIndexPlusOne > 0) {
        // Exact maximum known - size the vertex range to it.
        drawVertexCount = std::min(maxIndexPlusOne, maxVBVertices);
      } else {
        // Index data not CPU-readable (or draw too large to scan). Index values
        // may reference ANY vertex in the remaining buffer: many engines bake
        // absolute offsets into the indices instead of using BaseVertexLocation,
        // so clamping to the index count would leave indices pointing past the
        // vertex range Remix copies - out-of-bounds fetches that render as
        // exploded triangle spikes. Cover the whole remaining buffer, bounded to
        // keep the interleave allocation sane; beyond that the draw cannot be
        // made safe, so drop it rather than corrupt the scene.
        static constexpr uint32_t kMaxUnknownRangeVertices = 4u << 20;
        if (maxVBVertices > kMaxUnknownRangeVertices) {
          ++m_submitRejectStats.vertexRangeRejected;
          return;
        }
        drawVertexCount = maxVBVertices;
      }
      hashCount = std::min(drawVertexCount, count);
    }
    if (drawVertexCount == 0)
      drawVertexCount = std::min(count, maxVBVertices > 0 ? maxVBVertices : count);
    if (hashCount == 0)
      hashCount = std::min(count, maxVBVertices);
    hashCount = std::min(hashCount, kMaxHashedVertices);
    geo.vertexCount = drawVertexCount;

    // DX11_V250_DYNAMIC_BUFFER_SNAPSHOT: DrawCallStates are queued and the RT
    // scene is recorded at EndFrame, but D3D11 dynamic buffers are renamed
    // (Map WRITE_DISCARD) many times per frame. A directly-bound slice resolves
    // to the physical backing CURRENT AT RECORD TIME - i.e. the bytes of a
    // LATER draw or a recycled slice - so every dynamic-buffer mesh reads
    // someone else's vertices or zeros. Zeros collapse the mesh to a single
    // point; foreign data renders as garbage. The rtx.useBuffersDirectly=false
    // copy path was never ported from D3D11 (the option has no consumer in this
    // fork), so implement the snapshot here: host-visible (renameable) sources
    // are copied at submit time, while their contents are the ones this draw
    // actually used. Device-local buffers cannot be CPU-renamed and stay
    // zero-copy. Interleaved layouts sharing one buffer are copied once.
    {
      struct SnapEntry {
        DxvkBuffer*  src = nullptr;
        VkDeviceSize srcOffset = 0;
        uint32_t     stride = 0;
        Rc<DxvkBuffer> copy;
        VkDeviceSize bytes = 0;
      };
      SnapEntry snapEntries[4];
      uint32_t snapCount = 0;
      static constexpr VkDeviceSize kMaxSnapshotBytes = 32ull << 20;

      auto snapshotVertexBuffer = [&](RasterBuffer& buf) {
        if (!buf.defined() || buf.stride() == 0 || drawVertexCount == 0)
          return;
        const void* srcMap = buf.mapPtr(0);
        if (srcMap == nullptr)
          return; // device-local: not renameable, safe to bind directly

        DxvkBuffer* srcBuf = buf.buffer().ptr();
        const VkDeviceSize srcOffset = buf.offset();
        const uint32_t stride = buf.stride();

        // Reuse a snapshot already taken for this (buffer, slice, stride) -
        // interleaved layouts share one buffer across several attributes.
        for (uint32_t i = 0; i < snapCount; ++i) {
          if (snapEntries[i].src == srcBuf && snapEntries[i].srcOffset == srcOffset && snapEntries[i].stride == stride) {
            buf = RasterBuffer(DxvkBufferSlice(snapEntries[i].copy, 0, snapEntries[i].bytes),
                               buf.offsetFromSlice(), stride, buf.vertexFormat());
            return;
          }
        }

        // Cover every byte the draw can address: full stride per vertex plus a
        // small margin for exotic layouts whose attribute offset exceeds the
        // stride, clamped to the actual slice extent.
        const VkDeviceSize wanted = VkDeviceSize(drawVertexCount) * stride + 256u;
        const VkDeviceSize bytes = std::min<VkDeviceSize>(wanted, buf.length());
        if (bytes == 0 || bytes > kMaxSnapshotBytes) {
          static uint32_t sSnapshotSkipLog = 0;
          if (bytes > kMaxSnapshotBytes && sSnapshotSkipLog < 4) {
            ++sSnapshotSkipLog;
            Logger::info(str::format("[D3D11Rtx] Dynamic vertex snapshot skipped (", bytes >> 20, " MiB exceeds cap); binding directly."));
          }
          return;
        }

        Rc<DxvkBuffer> copy = createHostVisibleHelperBuffer(m_context->m_device, bytes, "d3d11 rtx dynamic vb snapshot");
        void* dst = copy != nullptr ? copy->mapPtr(0) : nullptr;
        if (dst == nullptr)
          return;
        std::memcpy(dst, srcMap, size_t(bytes));

        if (snapCount < 4)
          snapEntries[snapCount++] = { srcBuf, srcOffset, stride, copy, bytes };
        buf = RasterBuffer(DxvkBufferSlice(copy, 0, bytes), buf.offsetFromSlice(), stride, buf.vertexFormat());
      };

      snapshotVertexBuffer(posBuffer);
      snapshotVertexBuffer(nrmBuffer);
      snapshotVertexBuffer(tcBuffer);
      snapshotVertexBuffer(colBuffer);
      geo.positionBuffer = posBuffer;
      geo.normalBuffer   = nrmBuffer;
      geo.texcoordBuffer = tcBuffer;
      geo.color0Buffer   = colBuffer;

      if (indexed && idxBuffer.defined()) {
        const void* srcMap = idxBuffer.mapPtr(0);
        if (srcMap != nullptr) {
          const uint32_t idxStrideBytesSnap = std::max(idxBuffer.stride(), 1u);
          const VkDeviceSize wanted = VkDeviceSize(count) * idxStrideBytesSnap;
          const VkDeviceSize bytes = std::min<VkDeviceSize>(wanted, idxBuffer.length());
          if (bytes > 0 && bytes <= kMaxSnapshotBytes) {
            Rc<DxvkBuffer> copy = createHostVisibleHelperBuffer(m_context->m_device, bytes, "d3d11 rtx dynamic ib snapshot");
            void* dst = copy != nullptr ? copy->mapPtr(0) : nullptr;
            if (dst != nullptr) {
              std::memcpy(dst, srcMap, size_t(bytes));
              idxBuffer = RasterBuffer(DxvkBufferSlice(copy, 0, bytes), 0, idxBuffer.stride(), idxBuffer.indexType());
              geo.indexBuffer = idxBuffer;
            }
          }
        }
      }
    }

    // DX11_V249_INTERLEAVER_FORMAT_NORMALIZATION: the interleaver (interleave_
    // geometry.h) decodes exactly six formats: R16G16_SFLOAT, R32G32_SFLOAT,
    // R32G32B32_SFLOAT, R32G32B32A32_SFLOAT, R8G8B8A8_UNORM and
    // A2B10G10R10_SNORM_PACK32. The capture accept-lists were wider, which
    // produced two whole bug classes:
    //  - POSITION in R16G16B16A16_SFLOAT (fmt 97, common in optimized engines):
    //    the interleaver refuses the whole geometry and leaves the interleaved
    //    vertex output garbage -> exploded triangle spikes.
    //  - TEXCOORD in half4 / 8-bit / 16-bit (u)norm: the interleaver skips the
    //    texcoord channel -> meshes bind textures with no UVs -> corrupt/flat
    //    texturing.
    // Normalize both CPU-side into interleaver-native helper buffers here, so
    // every submitted mesh is decodable by construction.
    {
      auto interleaverSupportsFloatFormat = [](VkFormat f) -> bool {
        switch (f) {
          case VK_FORMAT_R16G16_SFLOAT:
          case VK_FORMAT_R32G32_SFLOAT:
          case VK_FORMAT_R32G32B32_SFLOAT:
          case VK_FORMAT_R32G32B32A32_SFLOAT:
          case VK_FORMAT_R8G8B8A8_UNORM:
          case static_cast<VkFormat>(65): // A2B10G10R10_SNORM_PACK32
            return true;
          default:
            return false;
        }
      };

      static constexpr uint32_t kMaxFormatConvertVertices = 1u << 20;

      // --- POSITION ---
      if (!interleaverSupportsFloatFormat(geo.positionBuffer.vertexFormat())) {
        bool positionConverted = false;
        const VkFormat srcFmt = geo.positionBuffer.vertexFormat();
        const uint8_t* srcBase = reinterpret_cast<const uint8_t*>(
          geo.positionBuffer.mapPtr(geo.positionBuffer.offsetFromSlice()));
        const uint32_t srcStride = geo.positionBuffer.stride();
        const uint32_t srcSliceOff = geo.positionBuffer.offsetFromSlice();
        const size_t srcLen = geo.positionBuffer.length() > srcSliceOff
          ? geo.positionBuffer.length() - srcSliceOff
          : 0;

        if (srcFmt == static_cast<VkFormat>(97) // R16G16B16A16_SFLOAT
         && srcBase != nullptr && srcStride > 0
         && drawVertexCount > 0 && drawVertexCount <= kMaxFormatConvertVertices) {
          const VkDeviceSize dstSize = VkDeviceSize(drawVertexCount) * 12u;
          Rc<DxvkBuffer> dst = createHostVisibleHelperBuffer(m_context->m_device, dstSize, "d3d11 rtx positions f16->f32");
          float* out = dst != nullptr ? reinterpret_cast<float*>(dst->mapPtr(0)) : nullptr;
          if (out != nullptr) {
            for (uint32_t v = 0; v < drawVertexCount; ++v) {
              const size_t off = size_t(v) * srcStride;
              float x = 0.f, y = 0.f, z = 0.f;
              if (off + 8 <= srcLen) {
                const uint16_t* h = reinterpret_cast<const uint16_t*>(srcBase + off);
                x = decodeFloat16(h[0]); y = decodeFloat16(h[1]); z = decodeFloat16(h[2]);
                if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) {
                  x = y = z = 0.f; // neutralize poisoned elements instead of exploding
                }
              }
              out[v * 3 + 0] = x; out[v * 3 + 1] = y; out[v * 3 + 2] = z;
            }
            geo.positionBuffer = RasterBuffer(DxvkBufferSlice(dst, 0, dstSize), 0, 12u, VK_FORMAT_R32G32B32_SFLOAT);
            posBuffer = geo.positionBuffer;
            positionConverted = true;
          }
        }

        if (!positionConverted) {
          // The interleaver would refuse this geometry and output garbage -
          // dropping the draw is strictly better than an exploded mesh.
          ++m_submitRejectStats.positionFormatRejected;
          return;
        }
      }

      // --- TEXCOORD ---
      if (geo.texcoordBuffer.defined()
       && !interleaverSupportsFloatFormat(geo.texcoordBuffer.vertexFormat())) {
        bool texcoordConverted = false;
        const VkFormat srcFmt = geo.texcoordBuffer.vertexFormat();
        const uint8_t* srcBase = reinterpret_cast<const uint8_t*>(
          geo.texcoordBuffer.mapPtr(geo.texcoordBuffer.offsetFromSlice()));
        const uint32_t srcStride = geo.texcoordBuffer.stride();
        const uint32_t srcSliceOff = geo.texcoordBuffer.offsetFromSlice();
        const size_t srcLen = geo.texcoordBuffer.length() > srcSliceOff
          ? geo.texcoordBuffer.length() - srcSliceOff
          : 0;

        // Decode a UV pair for the formats with well-defined semantics.
        // Integer formats use rtx.integerTexcoordScale - fixed-point UVs with an
        // engine-specific divisor (Saints Row IV: TEXCOORD0 = R16G16_SINT).
        const float intUvScale = integerTexcoordScale();
        auto decodeUv = [srcFmt, intUvScale](const uint8_t* src, float& u, float& v) -> bool {
          switch (srcFmt) {
            case VK_FORMAT_R16G16_SINT: {
              const int16_t* s = reinterpret_cast<const int16_t*>(src);
              u = s[0] * intUvScale; v = s[1] * intUvScale;
            } return true;
            case VK_FORMAT_R16G16_UINT: {
              const uint16_t* s = reinterpret_cast<const uint16_t*>(src);
              u = s[0] * intUvScale; v = s[1] * intUvScale;
            } return true;
            case static_cast<VkFormat>(97): { // R16G16B16A16_SFLOAT -> xy
              const uint16_t* h = reinterpret_cast<const uint16_t*>(src);
              u = decodeFloat16(h[0]); v = decodeFloat16(h[1]);
            } return true;
            case VK_FORMAT_R8G8_UNORM:
              u = src[0] / 255.0f; v = src[1] / 255.0f;
              return true;
            case VK_FORMAT_R8G8_SNORM: {
              const int8_t* s = reinterpret_cast<const int8_t*>(src);
              u = std::max(s[0] / 127.0f, -1.0f); v = std::max(s[1] / 127.0f, -1.0f);
            } return true;
            case VK_FORMAT_R16G16_UNORM:
            case VK_FORMAT_R16G16B16A16_UNORM: { // xy
              const uint16_t* s = reinterpret_cast<const uint16_t*>(src);
              u = s[0] / 65535.0f; v = s[1] / 65535.0f;
            } return true;
            case VK_FORMAT_R16G16_SNORM:
            case VK_FORMAT_R16G16B16A16_SNORM: { // xy
              const int16_t* s = reinterpret_cast<const int16_t*>(src);
              u = std::max(s[0] / 32767.0f, -1.0f); v = std::max(s[1] / 32767.0f, -1.0f);
            } return true;
            // DX11_V269: previously accepted by the capture but never decoded
            // here NOR supported by the interleaver - the channel was dropped
            // and textures rendered with no UVs (flat albedo).
            case VK_FORMAT_R8G8B8A8_SNORM: { // xy
              const int8_t* s = reinterpret_cast<const int8_t*>(src);
              u = std::max(s[0] / 127.0f, -1.0f); v = std::max(s[1] / 127.0f, -1.0f);
            } return true;
            case VK_FORMAT_R32G32_SINT: {
              const int32_t* s = reinterpret_cast<const int32_t*>(src);
              u = s[0] * intUvScale; v = s[1] * intUvScale;
            } return true;
            case VK_FORMAT_R32G32_UINT: {
              const uint32_t* s = reinterpret_cast<const uint32_t*>(src);
              u = s[0] * intUvScale; v = s[1] * intUvScale;
            } return true;
            default:
              return false;
          }
        };

        const uint32_t uvBytes =
          (srcFmt == VK_FORMAT_R8G8_UNORM || srcFmt == VK_FORMAT_R8G8_SNORM) ? 2u :
          (srcFmt == VK_FORMAT_R16G16_UNORM || srcFmt == VK_FORMAT_R16G16_SNORM
           || srcFmt == VK_FORMAT_R16G16_SINT || srcFmt == VK_FORMAT_R16G16_UINT
           || srcFmt == VK_FORMAT_R8G8B8A8_SNORM) ? 4u : 8u;

        float probeU = 0.f, probeV = 0.f;
        if (srcBase != nullptr && srcStride > 0 && srcLen >= uvBytes
         && drawVertexCount > 0 && drawVertexCount <= kMaxFormatConvertVertices
         && decodeUv(srcBase, probeU, probeV)) {
          const VkDeviceSize dstSize = VkDeviceSize(drawVertexCount) * 8u;
          Rc<DxvkBuffer> dst = createHostVisibleHelperBuffer(m_context->m_device, dstSize, "d3d11 rtx texcoords ->f32");
          float* out = dst != nullptr ? reinterpret_cast<float*>(dst->mapPtr(0)) : nullptr;
          if (out != nullptr) {
            for (uint32_t v = 0; v < drawVertexCount; ++v) {
              const size_t off = size_t(v) * srcStride;
              float tu = 0.f, tv = 0.f;
              if (off + uvBytes <= srcLen) {
                decodeUv(srcBase + off, tu, tv);
                if (!std::isfinite(tu) || !std::isfinite(tv)) {
                  tu = tv = 0.f;
                }
              }
              out[v * 2 + 0] = tu; out[v * 2 + 1] = tv;
            }
            geo.texcoordBuffer = RasterBuffer(DxvkBufferSlice(dst, 0, dstSize), 0, 8u, VK_FORMAT_R32G32_SFLOAT);
            tcBuffer = geo.texcoordBuffer;
            texcoordConverted = true;
          }
        }

        if (!texcoordConverted) {
          // Formats without defined decode semantics here (e.g. integer UVs with
          // an engine-specific fixed-point scale): drop the channel explicitly.
          // Remix then uses its no-UV fallback, which renders flat but never
          // corrupts; the interleaver skipping an undecodable channel is the
          // same result reached less predictably.
          geo.texcoordBuffer = RasterBuffer();
          tcBuffer = RasterBuffer();
        }
      }

      // --- COLOR0 ---
      // DX11_V268_VERTEX_COLOR_FORMATS (supersedes the RGBA8-only V252 swap):
      // the interleaver's uint path accepts ONLY B8G8R8A8_UNORM. Convert every
      // other admitted COLOR0 format into that layout - RGBA8 (byte swizzle),
      // float4 (was silently dropped: games that bake lighting/tinting into
      // vertex colors washed out to white), half4 and unorm16. B8G8R8A8 is
      // the D3DCOLOR byte order the Remix shaders decode, matching what the
      // native d3d11 path always fed them.
      if (geo.color0Buffer.defined()
       && geo.color0Buffer.vertexFormat() != VK_FORMAT_B8G8R8A8_UNORM) {
        const VkFormat colFmt = geo.color0Buffer.vertexFormat();
        const uint32_t colElemBytes =
            colFmt == VK_FORMAT_R8G8B8A8_UNORM      ? 4u
          : colFmt == VK_FORMAT_R16G16B16A16_UNORM  ? 8u
          : colFmt == VK_FORMAT_R16G16B16A16_SFLOAT ? 8u
          : colFmt == VK_FORMAT_R32G32B32A32_SFLOAT ? 16u
          : 0u;

        bool colorConverted = false;
        const uint8_t* srcBase = reinterpret_cast<const uint8_t*>(
          geo.color0Buffer.mapPtr(geo.color0Buffer.offsetFromSlice()));
        const uint32_t srcStride = geo.color0Buffer.stride();
        const uint32_t srcSliceOff = geo.color0Buffer.offsetFromSlice();
        const size_t srcLen = geo.color0Buffer.length() > srcSliceOff
          ? geo.color0Buffer.length() - srcSliceOff
          : 0;

        if (colElemBytes != 0 && srcBase != nullptr && srcStride > 0
         && drawVertexCount > 0 && drawVertexCount <= kMaxFormatConvertVertices) {
          const VkDeviceSize dstSize = VkDeviceSize(drawVertexCount) * 4u;
          Rc<DxvkBuffer> dst = createHostVisibleHelperBuffer(m_context->m_device, dstSize, "d3d11 rtx color0 to bgra");
          uint8_t* out = dst != nullptr ? reinterpret_cast<uint8_t*>(dst->mapPtr(0)) : nullptr;
          if (out != nullptr) {
            auto toByte = [](float c) -> uint8_t {
              if (!std::isfinite(c)) c = 1.0f;
              c = c < 0.0f ? 0.0f : (c > 1.0f ? 1.0f : c);
              return static_cast<uint8_t>(c * 255.0f + 0.5f);
            };
            for (uint32_t v = 0; v < drawVertexCount; ++v) {
              const size_t off = size_t(v) * srcStride;
              uint8_t r = 255, g = 255, b = 255, a = 255;
              if (off + colElemBytes <= srcLen) {
                const uint8_t* src = srcBase + off;
                switch (colFmt) {
                  case VK_FORMAT_R8G8B8A8_UNORM:
                    r = src[0]; g = src[1]; b = src[2]; a = src[3];
                    break;
                  case VK_FORMAT_R16G16B16A16_UNORM: {
                    const uint16_t* u = reinterpret_cast<const uint16_t*>(src);
                    r = uint8_t(u[0] >> 8); g = uint8_t(u[1] >> 8);
                    b = uint8_t(u[2] >> 8); a = uint8_t(u[3] >> 8);
                    break;
                  }
                  case VK_FORMAT_R16G16B16A16_SFLOAT: {
                    const uint16_t* h = reinterpret_cast<const uint16_t*>(src);
                    r = toByte(decodeFloat16(h[0])); g = toByte(decodeFloat16(h[1]));
                    b = toByte(decodeFloat16(h[2])); a = toByte(decodeFloat16(h[3]));
                    break;
                  }
                  case VK_FORMAT_R32G32B32A32_SFLOAT: {
                    const float* f = reinterpret_cast<const float*>(src);
                    r = toByte(f[0]); g = toByte(f[1]); b = toByte(f[2]); a = toByte(f[3]);
                    break;
                  }
                  default:
                    break;
                }
              }
              out[v * 4 + 0] = b; out[v * 4 + 1] = g; out[v * 4 + 2] = r; out[v * 4 + 3] = a;
            }
            geo.color0Buffer = RasterBuffer(DxvkBufferSlice(dst, 0, dstSize), 0, 4u, VK_FORMAT_B8G8R8A8_UNORM);
            colBuffer = geo.color0Buffer;
            colorConverted = true;
          }
        }

        if (!colorConverted) {
          // Cannot convert (unmapped/huge/unknown): drop the channel so the
          // interleaver never sees a format it cannot decode. White fallback,
          // never corrupt.
          geo.color0Buffer = RasterBuffer();
          colBuffer = RasterBuffer();
        }
      }
    }

    // Object-space mesh bounding box. The D3D11 capture path never produced one,
    // so every feature that depends on it silently no-oped on all GPUs:
    // GPU Scene (significance culling projects the world bounds vs the sub-pixel
    // threshold) and Anti-Culling (keeps off-screen bounds in the frustum). Both
    // are enabled/available here, so compute the AABB - vendor-agnostic CPU
    // min/max over the drawn vertex range - only when a feature needs it. The
    // instance manager reads geo.boundingBox directly, and finalizeGeometry
    // BoundingBox() leaves it untouched unless a futureBoundingBox was scheduled,
    // so setting it here is sufficient. Fail-safe: an unmapped/unsupported/empty
    // position buffer leaves the bbox invalid, which keeps the instance.
    if (RtxOptions::needsMeshBoundingBox() && posBuffer.stride() > 0 && drawVertexCount > 0) {
      const VkFormat posFmt = posBuffer.vertexFormat();
      const uint32_t elemBytes = positionElementBytes(posFmt);
      const uint8_t* posBase = elemBytes > 0
        ? reinterpret_cast<const uint8_t*>(posBuffer.mapPtr(posBuffer.offsetFromSlice()))
        : nullptr;
      if (posBase != nullptr) {
        const uint32_t stride = posBuffer.stride();
        // posBase points at offsetFromSlice() within the slice, so the readable
        // span from it is length() minus that attribute offset. Bounds-check reads
        // against this (not the full slice length) to avoid running off the buffer.
        const uint32_t posSliceOff = posBuffer.offsetFromSlice();
        const size_t posLen = posBuffer.length() > posSliceOff
          ? posBuffer.length() - posSliceOff
          : 0;
        // Sample evenly across the whole vertex range so the extent is captured
        // without iterating millions of vertices on the submit thread. Kept modest
        // because this runs per accepted draw.
        static constexpr uint32_t kMaxBBoxSampleVertices = 1024u;
        const uint32_t sampleCount = std::min(drawVertexCount, kMaxBBoxSampleVertices);
        float mn[3] = { std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), std::numeric_limits<float>::max() };
        float mx[3] = { -std::numeric_limits<float>::max(), -std::numeric_limits<float>::max(), -std::numeric_limits<float>::max() };
        bool anyValid = false;
        uint32_t sampledVerts = 0;
        for (uint32_t i = 0; i < sampleCount; ++i) {
          const uint32_t v = (sampleCount >= drawVertexCount || sampleCount <= 1)
            ? i
            : static_cast<uint32_t>(uint64_t(i) * uint64_t(drawVertexCount - 1) / uint64_t(sampleCount - 1));
          const size_t byteOff = size_t(v) * stride;
          if (byteOff + elemBytes > posLen)
            continue;
          float p[3];
          ++sampledVerts;
          // Post-normalization the format is always decodable here, so a false
          // return means the values are non-finite. Skip them for the bounds
          // (a mesh may carry unreferenced NaN padding), but count them.
          if (!decodePositionForBounds(posBase + byteOff, posFmt, p))
            continue;
          for (int c = 0; c < 3; ++c) {
            mn[c] = std::min(mn[c], p[c]);
            mx[c] = std::max(mx[c], p[c]);
          }
          anyValid = true;
        }
        // DX11_V249: every sampled position non-finite means the "positions"
        // are not positions at all (wrong stride/slot/offset - reading garbage
        // memory). Feeding them to the BLAS renders exploded spikes and risks a
        // GPU hang, so drop the draw. Requiring ALL samples to be garbage keeps
        // this fail-safe for meshes with sparse NaN padding.
        if (sampledVerts >= 16 && !anyValid) {
          ++m_submitRejectStats.poisonedPositions;
          return;
        }
        if (anyValid) {
          // Bias the sampled extent outward a hair so under-sampling a large mesh
          // can never shrink an object below the sub-pixel cull threshold and drop
          // visible geometry. Symmetric about the centroid.
          for (int c = 0; c < 3; ++c) {
            const float center = 0.5f * (mn[c] + mx[c]);
            const float half = std::max(0.0f, 0.5f * (mx[c] - mn[c])) * 1.05f;
            mn[c] = center - half;
            mx[c] = center + half;
          }
          geo.boundingBox.minPos = Vector3(mn[0], mn[1], mn[2]);
          geo.boundingBox.maxPos = Vector3(mx[0], mx[1], mx[2]);
        }
      }
    }

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
    if (m_context->m_state.rs.numViewports > 0) {
      const auto& vp = m_context->m_state.rs.viewports[0];
      dcs.minZ = std::clamp(vp.MinDepth, 0.0f, 1.0f);
      dcs.maxZ = std::clamp(vp.MaxDepth, 0.0f, 1.0f);
    } else {
      dcs.minZ = 0.0f;
      dcs.maxZ = 1.0f;
    }

    // D3D11 has no legacy fog â€” engines bake fog into shaders.
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
        return likelyFullscreenPrimitive && likelyScreenSpaceDepthState;

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
      || cameraManager.getLastSetCameraType() != CameraType::Unknown
      || cameraManager.hasSeenRealMainCamera();
    const bool viewportFallbackAfterRealCamera =
      dcs.transformData.usedViewportFallbackProjection
      && cameraManager.hasSeenRealMainCamera();
    const bool allowViewportFallbackScreenSpaceReject = !renderDocAttached
      && dcs.transformData.usedViewportFallbackProjection
      && (hasStableSceneCamera
       || viewportFallbackAfterRealCamera
       || m_submitRejectStats.accepted > 0
       || sceneManager.isPreviousFrameSceneAvailable());

    const bool deferViewportFallbackScreenSpaceReject =
      dcs.transformData.usedViewportFallbackProjection
      && !allowViewportFallbackScreenSpaceReject;

    if (deferViewportFallbackScreenSpaceReject) {
      static uint32_t sDeferredFallbackScreenRejectLogCount = 0;
      if (sDeferredFallbackScreenRejectLogCount < 8) {
        ++sDeferredFallbackScreenRejectLogCount;
        Logger::info(str::format(
          "[D3D11Rtx] Deferring viewport-fallback screen-space rejection until a stable scene camera exists (count=",
          count,
          ", zEnable=",
          zEnable ? 1 : 0,
          ", zWrite=",
          zWriteEnable ? 1 : 0,
          ")"));
      }
    }

    if (allowViewportFallbackScreenSpaceReject && isLikelyScreenSpaceCompositePass()) {
      ++m_submitRejectStats.compositeSkip;
      static uint32_t sScreenSpaceCompositeSkipLogCount = 0;
      if (sScreenSpaceCompositeSkipLogCount < 8) {
          ++sScreenSpaceCompositeSkipLogCount;
        Logger::info(str::format(
          "[D3D11Rtx] Skipping screen-space composite pass: viewport fallback camera + identity transforms + RT-sized/empty inputs (count=",
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

    // Launcher / helper-window guard:
    // Many D3D11 launchers present tiny swap chains (211x36, 161x36, 480x420, 10x10, etc.)
    // before the real game scene exists. Running full Remix material categorization and RT
    // submission heuristics on those windows can destabilize startup while providing no useful
    // scene data. Until a stable scene camera or previous scene exists, hard-skip these tiny
    // outputs and wait for the real game viewport.
    const uint32_t activeOutputWidth =
      m_lastRemixViewportExtent.width > 0u ? m_lastRemixViewportExtent.width : m_lastOutputExtent.width;
    const uint32_t activeOutputHeight =
      m_lastRemixViewportExtent.height > 0u ? m_lastRemixViewportExtent.height : m_lastOutputExtent.height;
    const bool launcherSizedOutput =
      activeOutputWidth > 0u && activeOutputHeight > 0u &&
      (activeOutputWidth < 640u || activeOutputHeight < 480u);
    const bool noStableSceneYet =
      !hasStableSceneCamera &&
      !viewportFallbackAfterRealCamera &&
      m_submitRejectStats.accepted == 0 &&
      !sceneManager.isPreviousFrameSceneAvailable();

    if (launcherSizedOutput && noStableSceneYet) {
      static uint32_t sLauncherSizedOutputSkipLogCount = 0;
      if (sLauncherSizedOutputSkipLogCount < 12) {
        ++sLauncherSizedOutputSkipLogCount;
        Logger::info(str::format(
          "[D3D11Rtx] Skipping launcher/helper-window draw before RT submission: output=",
          activeOutputWidth, "x", activeOutputHeight,
          " count=", count,
          " zEnable=", zEnable ? 1 : 0,
          " zWrite=", zWriteEnable ? 1 : 0));
      }
      return;
    }

    // Launcher / helper-window guard:
    // Ignore tiny startup swap chains until a stable scene exists.

    // Register this context as the active rendering context so the primary
    // swap chain routes EndFrame/OnPresent through us, not a video-playback
    // device that happened to present first.
    // Do this only after rejecting obvious composite passes so skipped draws
    // do not pay the material/texture selection cost.
    FillMaterialData(dcs.materialData);

    const uint32_t tinyFallbackPrimitiveCount = dcs.geometryData.calculatePrimitiveCount();
    const bool tinyFallbackHasSceneDepthSignal =
      dcs.zEnable && (dcs.zWriteEnable || dcs.maxZ >= 0.99f);
    const bool tinyFallbackMicroRaster =
      !renderDocAttached &&
      dcs.transformData.usedViewportFallbackProjection &&
      count <= 6u &&
      tinyFallbackPrimitiveCount <= 2u &&
      !tinyFallbackHasSceneDepthSignal;

    if (tinyFallbackMicroRaster) {
      ++m_submitRejectStats.screenSpaceGarbageSkip;
      static uint32_t sTinyFallbackMicroRasterLogCount = 0;
      if (sTinyFallbackMicroRasterLogCount < 16) {
        ++sTinyFallbackMicroRasterLogCount;
        Logger::info(str::format(
          "[D3D11Rtx] Skipping tiny fallback-camera micro-raster draw before RT submission: count=",
          count,
          ", indexed=",
          indexed ? 1 : 0,
          ", primitives=",
          tinyFallbackPrimitiveCount,
          ", zEnable=",
          dcs.zEnable ? 1 : 0,
          ", zWrite=",
          dcs.zWriteEnable ? 1 : 0));
      }
      return;
    }

    uint32_t transientInputCount = 0;
    uint32_t significantInputCount = 0;
    const auto isLikelyTransientScreenSpacePass = [&]() {
      const uint32_t primitiveCount = dcs.geometryData.calculatePrimitiveCount();
      const bool hasSceneDepthSignal = dcs.zEnable
        && (dcs.zWriteEnable || dcs.maxZ >= 0.99f);
      const bool identitySceneSpace =
        isIdentityExact(dcs.transformData.objectToWorld)
        && isIdentityExact(dcs.transformData.worldToView)
        && (isIdentityExact(dcs.transformData.objectToView)
         || dcs.transformData.objectToView == dcs.transformData.objectToWorld);
      const bool viewportFallbackScreenSpace =
        dcs.transformData.usedViewportFallbackProjection
        && identitySceneSpace;

      // A synthesized viewport camera plus identity transforms is a raster
      // composition/UI pass, not a stable camera-space scene for Remix.  This
      // catches startup splash frames that repeatedly fed a rejected fake camera
      // into the RT scene before the actual game camera appeared.
      if (viewportFallbackScreenSpace && !hasSceneDepthSignal && count <= 4096)
        return true;

      const bool smallOrScreenPrimitive = count <= 12 || primitiveCount <= 4;
      const bool weakSceneSignal = !hasSceneDepthSignal || dcs.transformData.usedViewportFallbackProjection;
      if (!smallOrScreenPrimitive || !weakSceneSignal)
        return false;

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

      const uint32_t outputWidth = m_lastOutputExtent.width != 0u
        ? m_lastOutputExtent.width
        : m_lastRemixViewportExtent.width;
      const uint32_t outputHeight = m_lastOutputExtent.height != 0u
        ? m_lastOutputExtent.height
        : m_lastRemixViewportExtent.height;

      auto isBlockCompressed = [](DXGI_FORMAT fmt) {
        return (fmt >= DXGI_FORMAT_BC1_TYPELESS && fmt <= DXGI_FORMAT_BC1_UNORM_SRGB)
            || (fmt >= DXGI_FORMAT_BC2_TYPELESS && fmt <= DXGI_FORMAT_BC2_UNORM_SRGB)
            || (fmt >= DXGI_FORMAT_BC3_TYPELESS && fmt <= DXGI_FORMAT_BC3_UNORM_SRGB)
            || (fmt >= DXGI_FORMAT_BC4_TYPELESS && fmt <= DXGI_FORMAT_BC4_SNORM)
            || (fmt >= DXGI_FORMAT_BC5_TYPELESS && fmt <= DXGI_FORMAT_BC5_SNORM)
            || (fmt >= DXGI_FORMAT_BC6H_TYPELESS && fmt <= DXGI_FORMAT_BC7_UNORM_SRGB);
      };

      auto isDataOrSceneFormat = [](DXGI_FORMAT fmt) {
        switch (fmt) {
          case DXGI_FORMAT_R10G10B10A2_UNORM:
          case DXGI_FORMAT_R10G10B10A2_UINT:
          case DXGI_FORMAT_R11G11B10_FLOAT:
          case DXGI_FORMAT_R16_FLOAT:
          case DXGI_FORMAT_R16G16_FLOAT:
          case DXGI_FORMAT_R16G16B16A16_FLOAT:
          case DXGI_FORMAT_R32_FLOAT:
          case DXGI_FORMAT_R32G32_FLOAT:
          case DXGI_FORMAT_R32G32B32_FLOAT:
          case DXGI_FORMAT_R32G32B32A32_FLOAT:
          case DXGI_FORMAT_R8_UINT:
          case DXGI_FORMAT_R8_SINT:
          case DXGI_FORMAT_R8G8_UINT:
          case DXGI_FORMAT_R8G8_SINT:
          case DXGI_FORMAT_R8G8B8A8_UINT:
          case DXGI_FORMAT_R8G8B8A8_SINT:
          case DXGI_FORMAT_R16_UINT:
          case DXGI_FORMAT_R16_SINT:
          case DXGI_FORMAT_R16G16_UINT:
          case DXGI_FORMAT_R16G16_SINT:
          case DXGI_FORMAT_R16G16B16A16_UINT:
          case DXGI_FORMAT_R16G16B16A16_SINT:
          case DXGI_FORMAT_R32_UINT:
          case DXGI_FORMAT_R32_SINT:
          case DXGI_FORMAT_R32G32_UINT:
          case DXGI_FORMAT_R32G32_SINT:
          case DXGI_FORMAT_R32G32B32_UINT:
          case DXGI_FORMAT_R32G32B32_SINT:
          case DXGI_FORMAT_R32G32B32A32_UINT:
          case DXGI_FORMAT_R32G32B32A32_SINT:
          case DXGI_FORMAT_R16_TYPELESS:
          case DXGI_FORMAT_R24G8_TYPELESS:
          case DXGI_FORMAT_R32_TYPELESS:
          case DXGI_FORMAT_R32G8X24_TYPELESS:
          case DXGI_FORMAT_D16_UNORM:
          case DXGI_FORMAT_D24_UNORM_S8_UINT:
          case DXGI_FORMAT_D32_FLOAT:
          case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
          case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
          case DXGI_FORMAT_X24_TYPELESS_G8_UINT:
          case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:
          case DXGI_FORMAT_X32_TYPELESS_G8X24_UINT:
          case DXGI_FORMAT_R8G8_B8G8_UNORM:
          case DXGI_FORMAT_G8R8_G8B8_UNORM:
            return true;
          default:
            return false;
        }
      };

      auto matchesExtent = [](const VkExtent3D& extent, uint32_t width, uint32_t height) {
        return width != 0u && height != 0u
          && extent.width == width
          && extent.height == height;
      };

      auto matchesHalfExtent = [](const VkExtent3D& extent, uint32_t width, uint32_t height) {
        return width != 0u && height != 0u
          && extent.width * 2u == width
          && extent.height * 2u == height;
      };

      transientInputCount = 0;
      significantInputCount = 0;

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

        ++significantInputCount;

        D3D11_SHADER_RESOURCE_VIEW_DESC1 srvDesc = {};
        srv->GetDesc1(&srvDesc);
        const D3D11_COMMON_RESOURCE_DESC resourceDesc = srv->GetResourceDesc();
        const bool bc = isBlockCompressed(srvDesc.Format);
        const bool dataOrSceneFormat = isDataOrSceneFormat(srvDesc.Format);
        const bool hasHazardBindFlags = srv->TestHazards() != FALSE;
        const bool hasRtBind = (resourceDesc.BindFlags & D3D11_BIND_RENDER_TARGET) != 0;
        const bool hasUavBind = (resourceDesc.BindFlags & D3D11_BIND_UNORDERED_ACCESS) != 0;
        const bool hasDepthBind = (resourceDesc.BindFlags & D3D11_BIND_DEPTH_STENCIL) != 0;
        const bool singleMipLarge = imgInfo.mipLevels <= 1
          && (imgInfo.extent.width >= 512 || imgInfo.extent.height >= 512);
        const bool rtSized = matchesExtent(imgInfo.extent, rtWidth, rtHeight)
          || matchesExtent(imgInfo.extent, outputWidth, outputHeight)
          || matchesHalfExtent(imgInfo.extent, outputWidth, outputHeight);
        const bool multisampledView = srvDesc.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2DMS
          || srvDesc.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2DMSARRAY
          || imgInfo.sampleCount != VK_SAMPLE_COUNT_1_BIT;

        bool isCurrentRT = false;
        for (DxvkImage* boundRT : boundRTImages) {
          if (boundRT == view->image().ptr()) {
            isCurrentRT = true;
            break;
          }
        }

        const bool transientInput =
          isCurrentRT
          || multisampledView
          || (rtSized && (hasHazardBindFlags || hasDepthBind || hasRtBind || hasUavBind || dataOrSceneFormat))
          || (!bc && singleMipLarge && (hasDepthBind || hasRtBind || hasUavBind) && (hasHazardBindFlags || dataOrSceneFormat));

        if (transientInput)
          ++transientInputCount;
      }

      if (significantInputCount == 0)
        return viewportFallbackScreenSpace || (!dcs.materialData.usesTexture() && smallOrScreenPrimitive && !hasSceneDepthSignal);

      return transientInputCount == significantInputCount;
    };

    if (!renderDocAttached && isLikelyTransientScreenSpacePass()) {
      ++m_submitRejectStats.screenSpaceGarbageSkip;
      static uint32_t sScreenSpaceGarbageSkipLogCount = 0;
      if (sScreenSpaceGarbageSkipLogCount < 12) {
        ++sScreenSpaceGarbageSkipLogCount;
        Logger::info(str::format(
          "[D3D11Rtx] Skipping transient screen-space pass before RTX scene submission: count=",
          count,
          " primitives=",
          dcs.geometryData.calculatePrimitiveCount(),
          " zEnable=",
          zEnable ? 1 : 0,
          " zWrite=",
          zWriteEnable ? 1 : 0,
          " fallbackCamera=",
          dcs.transformData.usedViewportFallbackProjection ? 1 : 0,
          " transientInputs=",
          transientInputCount,
          "/",
          significantInputCount));
      }
      return;
    }

    // Vertex-color wiring. N64-era ports and fixed-function-style renderers
    // bake shading - or the entire surface color - into COLOR0: SM64-style
    // characters have untextured, vertex-colored body parts. With arg1
    // hardwired to Texture, untextured draws sampled a nonexistent texture
    // and rendered black. When the draw carries vertex colors: untextured
    // draws select the vertex color directly; textured draws use the classic
    // fixed-function default, Modulate(texture, vertex color).
    if (geo.color0Buffer.defined()) {
      if (!dcs.materialData.usesTexture()) {
        dcs.materialData.textureColorArg1Source = RtTextureArgSource::VertexColor0;
        dcs.materialData.textureColorOperation  = DxvkRtTextureOperation::SelectArg1;
        dcs.materialData.textureAlphaArg1Source = RtTextureArgSource::VertexColor0;
        dcs.materialData.textureAlphaOperation  = DxvkRtTextureOperation::SelectArg1;
      } else {
        dcs.materialData.textureColorArg2Source = RtTextureArgSource::VertexColor0;
        dcs.materialData.textureColorOperation  = DxvkRtTextureOperation::Modulate;
      }
      dcs.materialData.updateCachedHash();
    }

    if (!geo.texcoordBuffer.defined() && dcs.materialData.usesTexture()) {
      ++m_submitRejectStats.noTexcoordLayout;

      const uint32_t primitiveCountNoUv = dcs.geometryData.calculatePrimitiveCount();
      const bool noUvHasSceneDepthSignal = dcs.zEnable
        && (dcs.zWriteEnable || dcs.maxZ >= 0.99f);
      const bool noUvLikelyScreenGarbage =
        dcs.transformData.usedViewportFallbackProjection &&
        !indexed &&
        primitiveCountNoUv <= 8u &&
        !noUvHasSceneDepthSignal;

      if (noUvLikelyScreenGarbage) {
        ++m_submitRejectStats.screenSpaceGarbageSkip;
        return;
      }

      if (dcs.transformData.usedViewportFallbackProjection) {
      ++m_submitRejectStats.screenSpaceGarbageSkip;
      static uint32_t sDx11V124NoUvFallbackSkipLogCount = 0;
      if (sDx11V124NoUvFallbackSkipLogCount < 16) {
        ++sDx11V124NoUvFallbackSkipLogCount;
        Logger::info(str::format(
          "[D3D11Rtx] DX11_V124: skipping textured no-TEXCOORD draw under viewport-fallback camera to prevent white/color flash artifacts (count=",
          count,
          ", indexed=",
          indexed ? 1 : 0,
          ", primitives=",
          primitiveCountNoUv,
          ")"));
      }
      return;
    }

      // DX11_V126 NO-TEXCOORD ALBEDO FIX:
      // These textured draws have no TEXCOORD semantic in the input layout, so
      // the previous code forced TexGenMode::ViewPositions, which synthesizes
      // UVs from each vertex's camera-relative view-space position
      // (surface_interaction.slangh: mul(worldToView, worldPos)). The resulting
      // UVs span enormous ranges across a triangle and swim with every camera
      // move, so the texture is sampled at essentially random texels -> the
      // garbled-smear / black / blown-white surfaces observed in Granny's main
      // geometry (the count=273 draws).
      //
      // There is no correct UV to recover here (the layout genuinely has none),
      // and synthesized UVs can only be wrong. Instead, leave texgen OFF and
      // force a flat neutral albedo through the existing TFactor + SelectArg1
      // path: the shader picks the material's albedo from tFactor (a constant
      // register) and never depends on valid texture coordinates, so the
      // surface renders as solid geometry lit by the path tracer rather than
      // as a garbled texture smear. This is a strict improvement over the
      // swimming-texgen artifact and over simply dropping the draw.
      if (dcs.transformData.texgenMode == TexGenMode::None) {
        // Prefer the draw's own vertex colors when present: they are real
        // per-vertex data that needs no texture coordinates, so they make a
        // faithful flat albedo. Only fall back to the neutral TFactor constant
        // when there are no vertex colors either.
        const RtTextureArgSource albedoSource =
          geo.color0Buffer.defined() ? RtTextureArgSource::VertexColor0
                                     : RtTextureArgSource::TFactor;
        dcs.materialData.textureColorArg1Source  = albedoSource;
        dcs.materialData.textureColorOperation   = DxvkRtTextureOperation::SelectArg1;
        dcs.materialData.textureColorArg2Source  = RtTextureArgSource::None;
        dcs.materialData.textureAlphaArg1Source  = albedoSource;
        dcs.materialData.textureAlphaOperation   = DxvkRtTextureOperation::SelectArg1;
        dcs.materialData.textureAlphaArg2Source  = RtTextureArgSource::None;
        // tFactor is ARGB packed; the shader decodes it as .bgra. 0xffffffff =
        // opaque white (1,1,1,1), a neutral albedo: the surface is shaded purely
        // by the path tracer's lighting/GI with no baked color bias. This is the
        // least surprising default for geometry whose real texture we cannot
        // sample correctly.
        dcs.materialData.tFactor = 0xffffffffu;
        dcs.materialData.updateCachedHash();
        ++m_submitRejectStats.texcoordGenerated;

        static uint32_t sTexcoordFallbackLogCount = 0;
        if (sTexcoordFallbackLogCount < 12) {
          ++sTexcoordFallbackLogCount;
          // Prove the bound texture is still captured/indexed despite the flat
          // albedo override: the image hash is what the Remix dev-menu texture
          // browser and all per-texture settings (ignore/hide/sky/decal/...)
          // key on, so reporting it here confirms the asset pipeline is intact
          // for these no-UV draws. A hash of 0 means FillMaterialData found no
          // usable color texture candidate for this draw (a separate issue).
          const XXH64_hash_t texHash = dcs.materialData.getColorTexture().getImageHash();
          Logger::info(str::format(
            "[D3D11Rtx] Textured draw has no TEXCOORD semantic; using flat albedo (",
            geo.color0Buffer.defined() ? "vertex color" : "TFactor white",
            ") instead of view-position texgen (count=",
            count,
            ", indexed=",
            indexed ? 1 : 0,
            ", fallbackCamera=",
            dcs.transformData.usedViewportFallbackProjection ? 1 : 0,
            ", textureHash=0x", std::hex, texHash, std::dec,
            ", textureCaptured=", texHash != 0 ? 1 : 0,
            ")"));
        }
      }
    }

    const uint32_t tinyRasterPrimitiveCount = dcs.geometryData.calculatePrimitiveCount();
    const bool tinyRasterHasSceneDepthSignal = dcs.zEnable
      && (dcs.zWriteEnable || dcs.maxZ >= 0.99f);
    const bool tinyPostCameraRasterJunk =
      cameraManager.hasSeenRealMainCamera()
      && !dcs.transformData.usedViewportFallbackProjection
      && indexed
      && count <= 6u
      && tinyRasterPrimitiveCount <= 2u
      && !tinyRasterHasSceneDepthSignal;

    if (!renderDocAttached && tinyPostCameraRasterJunk) {
      ++m_submitRejectStats.screenSpaceGarbageSkip;
      static uint32_t sTinyRasterJunkLogCount = 0;
      if (sTinyRasterJunkLogCount < 16) {
        ++sTinyRasterJunkLogCount;
        Logger::info(str::format(
          "[D3D11Rtx] Skipping tiny post-main-camera raster junk draw: count=",
          count,
          ", indexed=",
          indexed ? 1 : 0,
          ", primitives=",
          tinyRasterPrimitiveCount,
          ", zEnable=",
          dcs.zEnable ? 1 : 0,
          ", zWrite=",
          dcs.zWriteEnable ? 1 : 0));
      }
      return;
    }

    ++m_submitRejectStats.accepted;
    {
      const uint32_t primitiveCount = dcs.geometryData.calculatePrimitiveCount();
      const bool hasSceneDepthSignal = dcs.zEnable
        && (dcs.zWriteEnable || dcs.maxZ >= 0.99f);
      const bool hasRealProjection = !dcs.transformData.usedViewportFallbackProjection;
      const bool hasViewOrStrongProjection = !isIdentityExact(dcs.transformData.worldToView)
        || (hasRealProjection && primitiveCount >= 32u);
      const bool strongViewportFallbackScene = dcs.transformData.usedViewportFallbackProjection
        && !m_hasSeenRealSceneProjection
        && !cameraManager.hasSeenRealMainCamera()
        && hasSceneDepthSignal
        && primitiveCount >= 32u;

      if (hasSceneDepthSignal
       && primitiveCount >= 1u
       && ((hasRealProjection && hasViewOrStrongProjection) || strongViewportFallbackScene)) {

        // UE-style significance culling: this draw is a scene candidate. Count
        // it, and if the budgeting loop has armed a distance threshold, drop
        // candidates farther than it so the per-frame budget is spent on the
        // nearest (most important) geometry rather than arrival order. The
        // camera-space depth is column 3, row 2 of objectToView (the object
        // origin's view-space Z); abs() since view Z is negative looking down -Z.
        ++m_submitRejectStats.sceneCandidates;
        if (RtxOptions::significanceCulling() && m_significanceMaxDistanceSq > 0.0f) {
          const float viewZ = dcs.transformData.objectToView[3][2];
          const float distSq = viewZ * viewZ;
          if (distSq > m_significanceMaxDistanceSq) {
            ++m_submitRejectStats.significanceCulled;
            return;
          }
        }

        ++m_submitRejectStats.sceneAccepted;
        if (hasRealProjection && hasViewOrStrongProjection) {
          ++m_submitRejectStats.realSceneAccepted;
          m_hasSeenRealSceneProjection = true;
          m_lastRealCameraFrameId = m_context->m_device->getCurrentFrameId();
        }
      }
    }

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

  void D3D11Rtx::UpdateTrackedExtents(const Rc<DxvkImage>& outputImage, VkExtent2D remixViewportExtent) {
    // Capture the stable previous values BEFORE any mutation. Every
    // comparison below must run against last frame's state — comparing the
    // incoming extent against a tracker this function already overwrote
    // (the bug in the previous EndFrame implementation) makes the
    // "much smaller than stable output" test compare a value against itself.
    const VkExtent2D previousStableOutput   = m_lastOutputExtent;
    const VkExtent2D previousStableViewport = m_lastRemixViewportExtent;

    VkExtent2D outputExtent = { 0u, 0u };
    if (outputImage != nullptr) {
      const VkExtent3D e = outputImage->info().extent;
      if (e.width > 0u && e.height > 0u)
        outputExtent = { e.width, e.height };
    }

    // The Remix-owned output extent is the only fallback for a missing
    // viewport extent. Note this is NOT the inverse promotion the old code
    // did: a valid sub-output viewport (letterboxed scene) is preserved
    // as-is and never silently replaced with the larger output extent,
    // otherwise remixViewportAspect and the viewport-fallback projection
    // would be computed from the wrong rectangle.
    if (remixViewportExtent.width == 0u || remixViewportExtent.height == 0u)
      remixViewportExtent = outputExtent;

    // Heuristic: does this extent look like a small helper/launcher window
    // occluding the real game output (overlay swapchains, splash windows,
    // emulator tool panes) rather than a legitimate resize?
    const auto isOccludingHelperExtent = [&](VkExtent2D candidate) -> bool {
      if (candidate.width == 0u || candidate.height == 0u)
        return false; // empty extents are skipped by applyExtent, not "occluding"

      const bool hadStableViewport =
        previousStableViewport.width >= 640u && previousStableViewport.height >= 480u;
      const bool hadStableOutput =
        previousStableOutput.width >= 640u && previousStableOutput.height >= 480u;

      if (!hadStableViewport && !hadStableOutput)
        return false; // nothing stable to defend yet — accept whatever arrives

      const bool tiny = candidate.width < 640u || candidate.height < 480u;

      const auto muchSmallerThan = [&](VkExtent2D stable) {
        return (uint64_t(candidate.width)  * 10ull < uint64_t(stable.width)  * 7ull)
            || (uint64_t(candidate.height) * 10ull < uint64_t(stable.height) * 7ull);
      };

      const bool muchSmallerThanViewport = hadStableViewport && muchSmallerThan(previousStableViewport);
      const bool muchSmallerThanOutput   = hadStableOutput   && muchSmallerThan(previousStableOutput);

      return tiny || muchSmallerThanViewport || muchSmallerThanOutput;
    };

    // Persistence escape hatch: a rejected extent that keeps arriving is the
    // new reality (the user really did shrink the window below the heuristic
    // floor). Returns true once the same extent has been rejected enough
    // consecutive times that it should be accepted after all.
    const auto rejectedExtentBecamePersistent = [&](VkExtent2D rejected) -> bool {
      if (rejected.width == m_pendingRejectedExtent.width
       && rejected.height == m_pendingRejectedExtent.height) {
        if (++m_pendingRejectedExtentCount >= kRejectedExtentAcceptEvents) {
          m_pendingRejectedExtentCount = 0;
          return true;
        }
      } else {
        m_pendingRejectedExtent = rejected;
        m_pendingRejectedExtentCount = 1;
      }
      return false;
    };

    // driveResizeTransition: per the header contract, only m_lastOutputExtent
    // changes may trigger resize-grace handling.
    const auto applyExtent = [&](VkExtent2D newExtent, VkExtent2D& trackedExtent, bool driveResizeTransition) {
      if (newExtent.width == 0u || newExtent.height == 0u)
        return;

      if (driveResizeTransition
       && trackedExtent.width != 0u && trackedExtent.height != 0u
       && (trackedExtent.width != newExtent.width || trackedExtent.height != newExtent.height)) {
        m_resizeTransitionFramesRemaining = std::max(m_resizeTransitionFramesRemaining, kResizeCameraGraceFrames);
      }

      trackedExtent = newExtent;
    };

    const auto considerExtent = [&](VkExtent2D candidate, VkExtent2D& trackedExtent, bool driveResizeTransition, const char* trackerName) {
      if (candidate.width == 0u || candidate.height == 0u)
        return;

      // During a genuine resize transition new extents flow through freely;
      // outside one, an occluding-helper-looking extent is rejected so a
      // launcher/overlay swapchain cannot clobber the trackers, trigger
      // bogus resize grace every flip-flopped present, or shrink the
      // viewport-fallback projection. Crucially this now protects
      // m_lastOutputExtent too — previously only the viewport tracker was
      // guarded, so a 320x240 helper present poisoned the output extent and
      // kept the resize-carryover camera hack permanently engaged.
      const bool occluding = m_resizeTransitionFramesRemaining == 0
                          && isOccludingHelperExtent(candidate);

      if (occluding && !rejectedExtentBecamePersistent(candidate)) {
        static uint32_t sIgnoredSmallExtentLogCount = 0;
        if (sIgnoredSmallExtentLogCount < 16) {
          ++sIgnoredSmallExtentLogCount;
          Logger::info(str::format(
            "[D3D11Rtx] Ignoring small/occluding ", trackerName, " extent update: new=",
            candidate.width, "x", candidate.height,
            " prevViewport=", previousStableViewport.width, "x", previousStableViewport.height,
            " prevOutput=", previousStableOutput.width, "x", previousStableOutput.height,
            " rejectStreak=", m_pendingRejectedExtentCount));
        }
        return;
      }

      applyExtent(candidate, trackedExtent, driveResizeTransition);
    };

    // Debounce output-extent changes: deferred pipelines bind several RT
    // sizes per frame at scene transitions; committing each one re-armed
    // resize grace every frame (resize storm). A changed extent must repeat
    // kResizeDebounceFrames times consecutively before it commits.
    if (outputExtent.width != 0u && outputExtent.height != 0u
     && m_lastOutputExtent.width != 0u && m_lastOutputExtent.height != 0u
     && (outputExtent.width != m_lastOutputExtent.width || outputExtent.height != m_lastOutputExtent.height)) {
      if (outputExtent.width == m_pendingResizeExtent.width && outputExtent.height == m_pendingResizeExtent.height) {
        ++m_pendingResizeCount;
      } else {
        m_pendingResizeExtent = outputExtent;
        m_pendingResizeCount = 1;
      }
      if (m_pendingResizeCount < kResizeDebounceFrames) {
        outputExtent = { 0u, 0u }; // not yet: skip the output-tracker update this round
      } else {
        m_pendingResizeCount = 0;
      }
    } else {
      m_pendingResizeCount = 0;
    }

    considerExtent(outputExtent,        m_lastOutputExtent,        true,  "output");
    considerExtent(remixViewportExtent, m_lastRemixViewportExtent, false, "Remix viewport");

    // Any accepted frame with non-occluding extents resets the persistence
    // streak so unrelated later rejections start counting from scratch.
    if (outputExtent.width != 0u
     && !(m_resizeTransitionFramesRemaining == 0 && isOccludingHelperExtent(outputExtent))) {
      m_pendingRejectedExtent = { 0u, 0u };
      m_pendingRejectedExtentCount = 0;
    }
  }

  void D3D11Rtx::EndFrame(const Rc<DxvkImage>& backbuffer, VkExtent2D remixViewportExtent) {
    // DX11_V263_CRASH_FILTER_SAFE: games install their own unhandled-exception
    // filter during startup, replacing ours; periodically re-assert so the
    // crash signature is always logged (theirs still runs via the chain).
    {
      static uint32_t s_filterReassertCounter = 0;
      if ((s_filterReassertCounter++ & 255u) == 0u)
        ::RemixReassertCrashSignatureFilter();
    }

    UpdateTrackedExtents(backbuffer, remixViewportExtent);

    // DX11_V255_FULLRES_TARGET_GUARD: the ray tracer's output resolution is the
    // extent of the image injected into (injectRTX sizes everything from
    // targetImage->info().extent). Games create helper/dummy swapchains (2x2
    // observed in Saints Row IV) and small intermediate targets; if one of those
    // ever reaches this point as the injection target, the whole path-traced
    // frame renders at that tiny size instead of the monitor resolution. Never
    // inject into a target dramatically smaller than the established output
    // extent - skip the frame and let the full-resolution primary drive RT.
    if (backbuffer != nullptr
     && m_lastOutputExtent.width > 0u && m_lastOutputExtent.height > 0u) {
      const VkExtent3D targetExtent = backbuffer->info().extent;
      if (targetExtent.width * 2u < m_lastOutputExtent.width
       || targetExtent.height * 2u < m_lastOutputExtent.height) {
        static uint32_t sSmallTargetSkipLog = 0;
        if (sSmallTargetSkipLog < 8) {
          ++sSmallTargetSkipLog;
          Logger::info(str::format("[D3D11Rtx] Skipping RTX injection into undersized target ",
            targetExtent.width, "x", targetExtent.height, " (output is ",
            m_lastOutputExtent.width, "x", m_lastOutputExtent.height, ")"));
        }
        return;
      }
    }

    // Let the real-camera latch decay after extended absence so menu and
    // loading-screen draws (viewport-fallback reliant) are not permanently
    // blocked once a session has run. ~4x the scene grace window.
    if (m_hasSeenRealSceneProjection) {
      const uint32_t currentFrame = m_context->m_device->getCurrentFrameId();
      if (currentFrame > m_lastRealCameraFrameId
       && (currentFrame - m_lastRealCameraFrameId) > kSceneCameraGraceFrames * 4u) {
        m_hasSeenRealSceneProjection = false;
      }
    }

    const uint32_t gameViewportCount = m_context->m_state.rs.numViewports;
    const VkExtent2D singleRemixViewportExtent = m_lastRemixViewportExtent;
    const uint32_t draws = m_drawCallID;
    const uint32_t acceptedDraws = m_submitRejectStats.accepted;
    m_prevFrameSceneAccepted = m_submitRejectStats.sceneAccepted;
    m_prevFrameRealSceneAccepted = m_submitRejectStats.realSceneAccepted;

    // UE-style significance control loop. Adjust the squared-distance threshold
    // toward the instance budget for next frame: if this frame had more scene
    // candidates than the budget, tighten (admit only nearer geometry); if it
    // comfortably fit, relax/disarm so sparse views regain full detail. The
    // step is multiplicative and clamped to +/-40%/frame, so the threshold
    // glides rather than popping. Disarmed (==0) means "no limit".
    if (RtxOptions::significanceCulling()) {
      const uint32_t budget = std::max(RtxOptions::maxInstanceSubmissions(), 1u);
      const uint32_t candidates = m_submitRejectStats.sceneCandidates;
      const float farthestKeptSq = m_significanceMaxDistanceSq;
      if (candidates > budget) {
        // Over budget: tighten. Seed from the current accepted set's implied
        // reach if disarmed, else shrink by the overshoot ratio (capped).
        const float ratio = static_cast<float>(budget) / static_cast<float>(candidates);
        const float shrink = std::max(ratio, 0.6f); // never below 60%/frame
        if (m_significanceMaxDistanceSq <= 0.0f) {
          // First arm: start generous (a large reach) so only the farthest are cut.
          m_significanceMaxDistanceSq = 1.0e12f * shrink;
        } else {
          m_significanceMaxDistanceSq *= shrink;
        }
      } else if (m_significanceMaxDistanceSq > 0.0f) {
        // Within budget: relax by up to 40%/frame; disarm once very large.
        m_significanceMaxDistanceSq *= 1.4f;
        if (m_significanceMaxDistanceSq > 1.0e13f) {
          m_significanceMaxDistanceSq = 0.0f; // disarm: no limit needed
        }
      }
      (void) farthestKeptSq;
    } else {
      m_significanceMaxDistanceSq = 0.0f;
    }
    m_prevFrameSceneCandidates = m_submitRejectStats.sceneCandidates;

    const uint32_t sceneAcceptedDraws = m_submitRejectStats.sceneAccepted;
    const uint32_t realSceneAcceptedDraws = m_submitRejectStats.realSceneAccepted;
    const uint32_t trustedSceneAcceptedDraws = realSceneAcceptedDraws > 0
      ? realSceneAcceptedDraws
      : (m_hasSeenRealSceneProjection ? 0u : sceneAcceptedDraws);
    static uint32_t s_endFrameLogCount = 0;
    static uint32_t s_submitSummaryLogCount = 0;
    if (s_endFrameLogCount < 8) {
      ++s_endFrameLogCount;
      Logger::info(str::format("[D3D11Rtx] EndFrame: draws=", draws,
        " backbuffer=", backbuffer != nullptr ? 1 : 0,
        " remixViewport=", singleRemixViewportExtent.width, "x", singleRemixViewportExtent.height,
        " gameRasterViewports=", gameViewportCount,
        " singleRemixViewport=1"));
    }
    if (gameViewportCount > 1) {
      static uint32_t s_multiViewportLogCount = 0;
      if (s_multiViewportLogCount < 8) {
        ++s_multiViewportLogCount;
        Logger::info(str::format(
          "[D3D11Rtx] Game submitted multiple raster viewports; Remix output remains one viewport and viewport-fallback camera selection stays disabled for this frame. gameRasterViewports=",
          gameViewportCount,
          " remixViewport=", singleRemixViewportExtent.width, "x", singleRemixViewportExtent.height));
      }
    }
    if (s_submitSummaryLogCount < 24 && m_submitRejectStats.total > draws) {
      ++s_submitSummaryLogCount;
      Logger::info(str::format(
        "[D3D11Rtx] Submit summary: total=", m_submitRejectStats.total,
        " forceInjIdle=", m_submitRejectStats.forceInjectionIdle,
        " accepted=", m_submitRejectStats.accepted,
        " scene=", m_submitRejectStats.sceneAccepted,
        " realScene=", m_submitRejectStats.realSceneAccepted,
        " sceneCand=", m_submitRejectStats.sceneCandidates,
        " sigCulled=", m_submitRejectStats.significanceCulled,
        " overflow=", m_submitRejectStats.queueOverflow,
        " nonTriangle=", m_submitRejectStats.nonTriangleTopology,
        " noPS=", m_submitRejectStats.noPixelShader,
        " noRT=", m_submitRejectStats.noRenderTarget,
        " trivial=", m_submitRejectStats.trivialDraw,
        " fullscreen=", m_submitRejectStats.fullscreenPostFx,
        " noLayout=", m_submitRejectStats.noInputLayout,
        " noSemantics=", m_submitRejectStats.noSemantics,
        " noTexcoord=", m_submitRejectStats.noTexcoordLayout,
        " texgen=", m_submitRejectStats.texcoordGenerated,
        " noPosSem=", m_submitRejectStats.noPositionSemantic,
        " pos2D=", m_submitRejectStats.position2D,
        " noPosBuffer=", m_submitRejectStats.noPositionBuffer,
        " noIB=", m_submitRejectStats.noIndexBuffer,
        " composite=", m_submitRejectStats.compositeSkip,
        " ui=", m_submitRejectStats.screenSpaceUiSkip,
        " screenGarbage=", m_submitRejectStats.screenSpaceGarbageSkip,
        " hashFail=", m_submitRejectStats.geometryHashScheduleFailed,
        " posFmtRej=", m_submitRejectStats.positionFormatRejected,
        " posPoison=", m_submitRejectStats.poisonedPositions,
        " vtxRangeRej=", m_submitRejectStats.vertexRangeRejected));
    }

    ResetCommandListState();
    // Projection cache (m_projSlot, m_projOffset, m_projStage, m_columnMajor)
    // is NOT reset â€” the validation path at the start of ExtractTransforms
    // re-reads and re-scans only when the cached location becomes stale.
    // Keep the world-matrix cache for the same reason: modern games can have
    // thousands of draws per frame, and rescanning all cbuffers on every frame
    // creates unnecessary CPU pressure. The world-cache fast path still
    // validates the cached location every draw and falls back to a full rescan
    // automatically when the shader layout changes.
    ++m_axisDetectFrame;

    const bool allowResizeCameraCarryover = m_resizeTransitionFramesRemaining > 0;
    m_context->EmitCs([backbuffer, draws, acceptedDraws, sceneAcceptedDraws, realSceneAcceptedDraws, trustedSceneAcceptedDraws, allowResizeCameraCarryover](DxvkContext* ctx) {
      RtxContext* rtx = static_cast<RtxContext*>(ctx);
      const uint32_t fid = rtx->getDevice()->getCurrentFrameId();
      bool camValid = rtx->getSceneManager().getCamera().isValid(fid);
      const bool allowSceneCameraCarryover = trustedSceneAcceptedDraws > 0 || acceptedDraws > 0;
      if (!camValid && (allowResizeCameraCarryover || allowSceneCameraCarryover)) {
        auto& cameraManager = rtx->getSceneManager().getCameraManager();
        auto& mainCamera = cameraManager.getCamera(CameraType::Main);
        const uint32_t lastUpdateFrame = mainCamera.getLastUpdateFrame();
        const bool lastCameraWasViewportFallback = cameraManager.mainCameraLastUpdateUsedViewportFallback();
        const uint32_t cameraGraceFrames = allowResizeCameraCarryover
          ? D3D11Rtx::kResizeCameraGraceFrames
          : D3D11Rtx::kSceneCameraGraceFrames;

        if (lastUpdateFrame != uint32_t(-1)
         && fid > lastUpdateFrame
         && fid - lastUpdateFrame <= cameraGraceFrames
         && (allowResizeCameraCarryover || allowSceneCameraCarryover || !lastCameraWasViewportFallback)) {
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
          if (!allowResizeCameraCarryover) {
            static uint32_t sSceneCameraCarryoverLogCount = 0;
            if (sSceneCameraCarryoverLogCount < 12) {
              ++sSceneCameraCarryoverLogCount;
              Logger::info(str::format(
                "[D3D11Rtx] Carrying forward last valid main camera across a short scene camera gap: frameId=",
                fid,
                " lastUpdate=",
                lastUpdateFrame,
                " realSceneDraws=",
                realSceneAcceptedDraws,
                " sceneDraws=",
                sceneAcceptedDraws,
                " trustedSceneDraws=",
                trustedSceneAcceptedDraws));
            }
          }
        }
      }
      if (fid < 32 || (fid < 512 && (fid % 64) == 0)) {
        Logger::info(str::format("[D3D11Rtx] CS endFrame: frameId=", fid,
          " draws=", draws, " camValid=", camValid ? 1 : 0));
      }

      const bool hasAcceptedSceneDraws = draws > 0 || acceptedDraws > 0;
      const bool hasGameSceneDraws = trustedSceneAcceptedDraws > 0 || acceptedDraws > 0;
      const bool previousSceneAvailable = rtx->getSceneManager().isPreviousFrameSceneAvailable();
      const bool shouldInjectRtx = shouldInjectD3D11RtxFrame(
            backbuffer != nullptr,
            hasGameSceneDraws,
            camValid || hasAcceptedSceneDraws,
            previousSceneAvailable && hasAcceptedSceneDraws);

      if (!shouldInjectRtx) {
        static uint32_t sStartupPassThroughLogCount = 0;
        if (sStartupPassThroughLogCount < 16) {
          ++sStartupPassThroughLogCount;
          Logger::info(str::format(
            "[D3D11Rtx] Passing through startup/loading frame without RTX injection: frameId=",
            fid,
            " draws=",
            draws,
            " accepted=",
            acceptedDraws,
            " scene=",
            sceneAcceptedDraws,
            " realScene=",
            realSceneAcceptedDraws,
            " trustedScene=",
            trustedSceneAcceptedDraws,
            " camValid=",
            camValid ? 1 : 0,
            " previousScene=",
            previousSceneAvailable ? 1 : 0,
            " backbuffer=",
            backbuffer != nullptr ? 1 : 0));
        }
      }

      rtx->endFrame(0, backbuffer, shouldInjectRtx);
    });

    if (m_resizeTransitionFramesRemaining > 0)
      --m_resizeTransitionFramesRemaining;
  }

  void D3D11Rtx::OnPresent(const Rc<DxvkImage>& swapchainImage, VkExtent2D remixViewportExtent) {
    // Same coherent policy as EndFrame — see UpdateTrackedExtents. The HWND
    // client rect is only an occlusion signal and must not drive the
    // renderer; only the present-image extent may trigger resize handling.
    UpdateTrackedExtents(swapchainImage, remixViewportExtent);

    m_context->EmitCs([swapchainImage](DxvkContext* ctx) {
      RtxContext* rtx = static_cast<RtxContext*>(ctx);
      rtx->onPresent(swapchainImage);
    });
  }

}
