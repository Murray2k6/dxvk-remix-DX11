#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>

#include "d3d11_vk_frontend.h"
#include "d3d11_camera_math.h"
#include "d3d11_engine_profile.h"
#include "d3d11_light_decode.h"
#include "d3d11_rtx.h"

#include "../dxvk/dxvk_format.h"
#include "../dxvk/imgui/dxvk_imgui.h"
#include "../dxvk/rtx_render/rtx_context.h"
#include "../dxvk/rtx_render/rtx_options.h"
#include "../dxvk/rtx_render/rtx_camera.h"
#include "../dxvk/rtx_render/rtx_light_manager.h"
#include "../dxvk/rtx_render/rtx_scene_manager.h"
#include "../dxvk/rtx_render/rtx_terrain_baker.h"
#include "../dxvk/dxvk_scoped_annotation.h"

// Scene input for the DX12 / Vulkan front end: the game's draws of one frame,
// recorded by d3d11_vk_frontend.cpp, become Remix draw calls with the same
// treatment the DX11 path gives its draws (D3D11Rtx::SubmitDraw):
//
//   - geometry is the post-VS clip position the game rasterized, captured by
//     transform feedback and unprojected with the frame's projection, so every
//     engine's transforms, skinning and vertex animation are already applied;
//     its identity is stable across frames and runs, so a mesh keeps its BLAS
//     (refit, not rebuilt) and its hash;
//   - passes: depth-only draws and later passes of the same mesh (depth and
//     light prepasses, ForwardAdd) are not traced twice;
//   - the camera comes from constant-buffer bytes (d3d11_camera_math.h, the
//     D3D11Rtx functions), with the viewport fallback camera otherwise;
//   - materials: albedo, normal map with the game's encoding, roughness /
//     smoothness, metallic, emissive and vertex colour, from the pixel
//     shader's SPIR-V dataflow and debug names (d3d11_vk_spirv.cpp);
//   - categories: every Remix texture-hash category (setupCategoriesForTexture),
//     the texture browser for tagging, and the automatic sky / decal /
//     particle / terrain / water rules of the DX11 path;
//   - lights: the engine light buffers the DX11 path imports
//     (d3d11_light_decode.cpp) and the sun from shadow cascades;
//   - 2D games: the 2D lift (sprites as emissive layers at draw-order depth).

namespace dxvk {

  namespace {

    constexpr uint32_t kMaxCameraScanDraws = 256;

    // Quantized key so equal matrices from different draws vote together.
    uint64_t matrixKey(const Matrix4& m, float quantum) {
      uint64_t key = 1469598103934665603ull;

      for (uint32_t c = 0; c < 4; c++) {
        for (uint32_t r = 0; r < 4; r++) {
          const int64_t q = int64_t(std::llround(double(m[c][r]) / double(quantum)));
          key = (key ^ uint64_t(q)) * 1099511628211ull;
        }
      }

      return key;
    }

    // Same lens: focal scales and depth row within 1 %.
    bool sameLens(const Matrix4& a, const Matrix4& b) {
      auto close = [](float x, float y) {
        return std::abs(x - y) <= 0.01f * std::max(std::abs(x), std::abs(y)) + 1.0e-6f;
      };

      return close(std::abs(a[0][0]), std::abs(b[0][0]))
          && close(std::abs(a[1][1]), std::abs(b[1][1]))
          && close(std::abs(a[2][3]), std::abs(b[2][3]));
    }

    const std::vector<uint8_t>* bytesOf(const D3D11VkDraw& draw, size_t i, size_t minimum = 64) {
      const auto& bytes = draw.bindingBytes[i];
      return (bytes && bytes->size() >= minimum) ? bytes.get() : nullptr;
    }

    bool isDepthFormat(VkFormat format) {
      switch (format) {
        case VK_FORMAT_D16_UNORM:
        case VK_FORMAT_X8_D24_UNORM_PACK32:
        case VK_FORMAT_D32_SFLOAT:
        case VK_FORMAT_S8_UINT:
        case VK_FORMAT_D16_UNORM_S8_UINT:
        case VK_FORMAT_D24_UNORM_S8_UINT:
        case VK_FORMAT_D32_SFLOAT_S8_UINT:
          return true;
        default:
          return false;
      }
    }

    // Two-channel formats engines store tangent-space normals in.
    bool isNormalMapFormat(VkFormat format) {
      switch (format) {
        case VK_FORMAT_BC5_UNORM_BLOCK:
        case VK_FORMAT_BC5_SNORM_BLOCK:
        case VK_FORMAT_R8G8_UNORM:
        case VK_FORMAT_R8G8_SNORM:
        case VK_FORMAT_R16G16_UNORM:
        case VK_FORMAT_R16G16_SNORM:
          return true;
        default:
          return false;
      }
    }

    // Authored colour textures (the auto terrain rule's layer formats).
    bool isColorLayerFormat(VkFormat format) {
      switch (format) {
        case VK_FORMAT_BC1_RGB_UNORM_BLOCK: case VK_FORMAT_BC1_RGB_SRGB_BLOCK:
        case VK_FORMAT_BC1_RGBA_UNORM_BLOCK: case VK_FORMAT_BC1_RGBA_SRGB_BLOCK:
        case VK_FORMAT_BC2_UNORM_BLOCK: case VK_FORMAT_BC2_SRGB_BLOCK:
        case VK_FORMAT_BC3_UNORM_BLOCK: case VK_FORMAT_BC3_SRGB_BLOCK:
        case VK_FORMAT_BC7_UNORM_BLOCK: case VK_FORMAT_BC7_SRGB_BLOCK:
        case VK_FORMAT_R8G8B8A8_SRGB: case VK_FORMAT_B8G8R8A8_SRGB:
          return true;
        default:
          return false;
      }
    }

    // "Already linear on sample": every sRGB format plus float/HDR formats,
    // which the material shader must not gamma-decode again.
    bool isSrgbFormat(VkFormat format) {
      return imageFormatSamplesLinear(format);
    }

    bool pixelStage(const remix_vkfe_binding& b) {
      return (b.stage_mask & (1u << REMIX_VKFE_STAGE_PIXEL)) != 0;
    }

    bool vertexStage(const remix_vkfe_binding& b) {
      return (b.stage_mask & ((1u << REMIX_VKFE_STAGE_VERTEX) | (1u << REMIX_VKFE_STAGE_DOMAIN)
                            | (1u << REMIX_VKFE_STAGE_GEOMETRY))) != 0;
    }

    // An empty reference for a texture wrapTexture refused (destroyed image).
    TextureRef textureRef(Rc<DxvkImageView> view) {
      return view != nullptr ? TextureRef(std::move(view)) : TextureRef();
    }

    bool depthOnly(const D3D11VkDraw& draw) {
      return draw.desc.render_target_count == 0 || !draw.pipeline || draw.pipeline->state.color_write_mask == 0;
    }

    bool additiveBlend(const remix_vkfe_pipeline_desc& s) {
      return s.blend_enable && s.dst_color_blend == VK_BLEND_FACTOR_ONE;
    }

    // Which geometry a draw puts on screen, whatever pass it is in: the
    // vertex and index streams, the draw range and the vertex stage's
    // constants (per-object transforms). Depth prepasses, light prepasses
    // and ForwardAdd passes of one object share it.
    uint64_t meshKey(const D3D11VkDraw& draw) {
      const auto& d = draw.desc;
      XXH64_hash_t h = 0;

      const uint64_t shape[] = {
        uint64_t(d.indexed), uint64_t(d.vertex_or_index_count), uint64_t(d.instance_count),
        uint64_t(d.first_vertex_or_index), uint64_t(int64_t(d.vertex_offset)), uint64_t(d.first_instance),
        uint64_t(d.index_buffer), uint64_t(d.index_offset),
        uint64_t(d.indirect_buffer), uint64_t(d.indirect_offset), uint64_t(d.indirect_draw_count),
      };
      h = XXH3_64bits_withSeed(shape, sizeof(shape), h);

      for (const auto& vb : draw.vertexBuffers) {
        const uint64_t v[] = { vb.binding, uint64_t(vb.buffer), uint64_t(vb.offset) };
        h = XXH3_64bits_withSeed(v, sizeof(v), h);
      }

      // Per-object constants of the vertex stage; the first 4 KiB of each
      // buffer carry the transforms in every engine documented here.
      for (size_t i = 0; i < draw.bindings.size(); i++) {
        if (!vertexStage(draw.bindings[i]))
          continue;

        if (const auto* bytes = bytesOf(draw, i, 1))
          h = XXH3_64bits_withSeed(bytes->data(), std::min<size_t>(bytes->size(), 4096u), h);
      }

      return h ? h : 1;
    }

    // Replacement camera space used with clip.w depth and the 2D lift: LH,
    // Y-up, +Z forward (D3D11Rtx, same settings).
    void selectReplacementProfile() {
      const RtxOptionLayer* derived = RtxOptionLayer::getDerivedLayer();
      RtxOptions::leftHandedCoordinateSystemObject().setDeferred(true, derived);
      RtxOptions::zUpObject().setDeferred(false, derived);
      RtCamera::correctProjectionYFlipObject().setDeferred(
        D3D11Rtx::projectionYFlipOverride() ? D3D11Rtx::projectionYFlip() : false, derived);
    }

    Matrix4 perspectiveFromViewport(float aspect, float fovDegrees) {
      const float yScale = 1.0f / std::tan(fovDegrees * (3.14159265f / 180.0f) * 0.5f);
      const float xScale = yScale / aspect;
      const float nearZ = 0.1f;
      const float farZ = 10000.0f;
      const float q = farZ / (farZ - nearZ);

      return Matrix4(
        Vector4(xScale, 0.0f,   0.0f,       0.0f),
        Vector4(0.0f,   yScale, 0.0f,       0.0f),
        Vector4(0.0f,   0.0f,   q,          1.0f),
        Vector4(0.0f,   0.0f,  -nearZ * q,  0.0f));
    }

    uint32_t textureUiFlags(const Rc<DxvkImageView>& view) {
      uint32_t flags = ImGUI::kTextureFlagsDefault;
      const VkImageUsageFlags usage = view->imageInfo().usage;

      if (usage & (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT))
        flags |= ImGUI::kTextureFlagsRenderTarget;

      return flags;
    }

  }


  // ---------------------------------------------------------------- camera

  D3D11VkFrontendDevice::FrameCamera D3D11VkFrontendDevice::findCamera(
    const std::vector<const D3D11VkDraw*>& sceneDraws,
          VkExtent3D                       sceneExtent) {
    ScopedCpuProfileZone();

    FrameCamera camera;
    auto& st = m_capture;

    // --- Projection: the perspective matrix most scene draws carry. ---
    struct Vote {
      Matrix4  matrix;
      uint32_t votes = 0;
      uint32_t space = 0, slot = 0, kind = 0;
      size_t   offset = 0;
      bool     transposed = false;
    };

    std::map<uint64_t, Vote> projections;
    const size_t scanCount = std::min<size_t>(sceneDraws.size(), kMaxCameraScanDraws);

    for (size_t d = 0; d < scanCount; d++) {
      const D3D11VkDraw& draw = *sceneDraws[d];

      for (size_t b = 0; b < draw.bindings.size(); b++) {
        if (draw.bindings[b].kind == REMIX_VKFE_BINDING_STORAGE_BUFFER)
          continue;

        const auto* bytes = bytesOf(draw, b);

        if (!bytes)
          continue;

        for (size_t off = 0; off + 64 <= bytes->size(); off += 16) {
          const Matrix4 raw = D3D11ReadMatrix(bytes->data(), off, bytes->size());
          const int cls = D3D11ClassifyPerspective(raw);

          if (!cls)
            continue;

          const Matrix4 p = cls == 2 ? transpose(raw) : raw;
          Vote& v = projections[matrixKey(p, 1.0e-4f)];

          if (!v.votes) {
            v.matrix = p;
            v.space = draw.bindings[b].space;
            v.slot = draw.bindings[b].slot;
            v.kind = uint32_t(draw.bindings[b].kind);
            v.offset = off;
            v.transposed = cls == 2;
          }

          v.votes++;
        }
      }
    }

    const Vote* best = nullptr;

    for (const auto& p : projections) {
      if (!best || p.second.votes > best->votes)
        best = &p.second;
    }

    if (best) {
      Matrix4 p = best->matrix;
      camera.jitteredProjection = D3D11CanonicalizeProjection(p, nullptr, nullptr);

      // Remix runs its own temporal jitter.
      p[2][0] = 0.0f;
      p[2][1] = 0.0f;
      camera.projection = D3D11CanonicalizeProjection(p, nullptr, nullptr);
      camera.valid = true;

      st.projSpace = best->space;
      st.projSlot = best->slot;
      st.projKind = best->kind;
      st.projOffset = best->offset;
      st.projTransposed = best->transposed;
    } else {
      // Viewport fallback camera (D3D11Rtx: rtx.fallbackCameraFovDegrees),
      // with geometry reconstructed from clip x, y and w.
      const float aspect = sceneExtent.height ? float(sceneExtent.width) / float(sceneExtent.height) : 16.0f / 9.0f;
      const float fovDegrees = std::max(20.0f, std::min(140.0f, D3D11Rtx::fallbackCameraFovDegrees()));

      camera.projection = perspectiveFromViewport(aspect, fovDegrees);
      camera.jitteredProjection = camera.projection;
      camera.clipUsesWDepth = true;
      camera.valid = true;
      st.statNoCamera++;
      selectReplacementProfile();
      return camera;
    }

    // --- View: the rigid part of the shared view-projection. Per-object
    // world-view-projection matrices also factor, each into a different
    // V * W, so the shared view is the one most draws agree on. ---
    std::map<uint64_t, Vote> views;

    for (size_t d = 0; d < scanCount; d++) {
      const D3D11VkDraw& draw = *sceneDraws[d];

      for (size_t b = 0; b < draw.bindings.size(); b++) {
        if (draw.bindings[b].kind == REMIX_VKFE_BINDING_STORAGE_BUFFER)
          continue;

        const auto* bytes = bytesOf(draw, b);

        if (!bytes)
          continue;

        for (size_t off = 0; off + 64 <= bytes->size(); off += 16) {
          const Matrix4 raw = D3D11ReadMatrix(bytes->data(), off, bytes->size());

          for (int t = 0; t < 2; t++) {
            Matrix4 p, v;

            if (!D3D11FactorViewProjection(t ? transpose(raw) : raw, p, v))
              continue;

            if (!sameLens(p, camera.jitteredProjection))
              continue;

            Vote& vote = views[matrixKey(v, 1.0e-3f)];

            if (!vote.votes)
              vote.matrix = v;

            vote.votes++;
            break;
          }
        }
      }
    }

    const Vote* bestView = nullptr;

    for (const auto& v : views) {
      if (!bestView || v.second.votes > bestView->votes)
        bestView = &v.second;
    }

    // One draw's matrix is not evidence of a shared camera unless the frame
    // only has one draw.
    if (bestView && (bestView->votes >= 2 || scanCount == 1)) {
      camera.view = bestView->matrix;
      camera.haveView = true;
    }

    return camera;
  }


  // ---------------------------------------------------------- terrain bake

  bool D3D11VkFrontendDevice::bakeClipToWorld(const D3D11VkDraw& draw, Matrix4& clipToWorld) const {
    const auto& st = m_capture;

    if (!st.haveProjection || !st.haveView)
      return false;

    // The draw's own view-projection: a matrix in its vertex-stage
    // constants that factors into last frame's lens and a rigid view. Of
    // several (a per-object world-view-projection factors too), the view
    // closest to last frame's camera is the shared one.
    double best = 0.0;
    bool found = false;

    for (size_t b = 0; b < draw.bindings.size(); b++) {
      const auto& binding = draw.bindings[b];

      if (binding.kind == REMIX_VKFE_BINDING_STORAGE_BUFFER || binding.kind == REMIX_VKFE_BINDING_TEXTURE
       || !(binding.stage_mask & (1u << REMIX_VKFE_STAGE_VERTEX)))
        continue;

      const auto* bytes = bytesOf(draw, b);

      if (!bytes)
        continue;

      for (size_t off = 0; off + 64 <= bytes->size(); off += 16) {
        const Matrix4 raw = D3D11ReadMatrix(bytes->data(), off, bytes->size());

        for (int t = 0; t < 2; t++) {
          const Matrix4 m = t ? transpose(raw) : raw;
          Matrix4 p, v;

          if (!D3D11FactorViewProjection(m, p, v) || !sameLens(p, st.jitteredProjection))
            continue;

          double d = 0.0;

          for (uint32_t c = 0; c < 4; c++) {
            for (uint32_t r = 0; r < 4; r++) {
              const double e = double(v[c][r]) - double(st.view[c][r]);
              d += e * e;
            }
          }

          if (!found || d < best) {
            best = d;
            clipToWorld = inverse(m);
            found = true;
          }

          break;
        }
      }
    }

    if (!found)
      return false;

    for (uint32_t c = 0; c < 4; c++) {
      for (uint32_t r = 0; r < 4; r++) {
        if (!std::isfinite(clipToWorld[c][r]))
          return false;
      }
    }

    return true;
  }


  bool D3D11VkFrontendDevice::createBakeImage(BakeImage& out, VkFormat format, VkExtent2D extent) {
    const bool depth = isDepthFormat(format);
    const bool stencil = format == VK_FORMAT_S8_UINT || format == VK_FORMAT_D16_UNORM_S8_UINT
                      || format == VK_FORMAT_D24_UNORM_S8_UINT || format == VK_FORMAT_D32_SFLOAT_S8_UINT;

    // Colour images are UNORM with the game's sRGB format as a view: Remix
    // copies the bytes as the game's pixel shader wrote them, which is the
    // encoding its terrain texture holds.
    VkFormat storage = format;

    if (format == VK_FORMAT_R8G8B8A8_SRGB) storage = VK_FORMAT_R8G8B8A8_UNORM;
    if (format == VK_FORMAT_B8G8R8A8_SRGB) storage = VK_FORMAT_B8G8R8A8_UNORM;

    const VkFormat viewFormats[2] = { storage, format };

    DxvkImageCreateInfo info;
    info.type            = VK_IMAGE_TYPE_2D;
    info.format          = storage;
    info.flags           = storage != format ? VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT : 0;
    info.viewFormatCount = storage != format ? 2u : 0u;
    info.viewFormats     = storage != format ? viewFormats : nullptr;
    info.sampleCount     = VK_SAMPLE_COUNT_1_BIT;
    info.extent          = { extent.width, extent.height, 1 };
    info.numLayers       = 1;
    info.mipLevels       = 1;
    info.usage           = depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT
                                 : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    info.stages          = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT
                         | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
    info.access          = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT
                         | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT
                         | VK_ACCESS_TRANSFER_READ_BIT;
    info.tiling          = VK_IMAGE_TILING_OPTIMAL;
    // The game's queue renders into it, Remix's copies from it.
    info.layout          = VK_IMAGE_LAYOUT_GENERAL;
    info.shared          = VK_TRUE;

    BakeImage next;
    next.image = m_device->createImage(info, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
      DxvkMemoryStats::Category::RTXRenderTarget, "Remix front-end terrain bake");

    if (next.image == nullptr)
      return false;

    const VkImageAspectFlags aspect = depth
      ? VkImageAspectFlags(VK_IMAGE_ASPECT_DEPTH_BIT | (stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0))
      : VkImageAspectFlags(VK_IMAGE_ASPECT_COLOR_BIT);

    DxvkImageViewCreateInfo view;
    view.type      = VK_IMAGE_VIEW_TYPE_2D;
    view.format    = format;
    view.usage     = depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    view.aspect    = aspect;
    view.minLevel  = 0;
    view.numLevels = 1;
    view.minLayer  = 0;
    view.numLayers = 1;
    next.attachmentView = m_device->createImageView(next.image, view);

    if (!depth) {
      view.format = storage;
      view.usage  = VK_IMAGE_USAGE_SAMPLED_BIT;
      next.sampledView = m_device->createImageView(next.image, view);
    }

    next.format = format;
    next.extent = extent;

    if (out.image != nullptr)
      m_retiredBakeImages.emplace_back(m_presentCount, std::move(out));

    out = std::move(next);

    // In GENERAL before the game's queue renders into it.
    {
      auto ctxLock = m_context->LockContext();
      m_context->EmitCs([image = out.image] (DxvkContext* ctx) {
        ctx->initImage(image, image->getAvailableSubresources(), VK_IMAGE_LAYOUT_UNDEFINED);
        ctx->flushCommandList();
      });
      m_context->FlushCsChunk();
    }

    m_context->SynchronizeCsThread(DxvkCsThread::SynchronizeAll);
    m_device->lockSubmission();
    m_device->unlockSubmission();

    // Framebuffers the caller built around older views are stale now.
    m_bakeGeneration++;
    return true;
  }


  const D3D11VkFrontendDevice::BakeImage* D3D11VkFrontendDevice::bakeImage(
          std::map<uint64_t, BakeImage>& images,
          uint64_t                       key,
          VkFormat                       format,
          VkExtent2D                     extent) {
    BakeImage& entry = images[key];

    if (entry.image == nullptr || entry.format != format
     || entry.extent.width != extent.width || entry.extent.height != extent.height) {
      if (!createBakeImage(entry, format, extent)) {
        images.erase(key);
        return nullptr;
      }
    }

    return &entry;
  }


  bool D3D11VkFrontendDevice::prepareBake(D3D11VkDraw& draw, remix_vkfe_draw_capture* capture) {
    const D3D11VkPipeline& pipeline = *draw.pipeline;

    if (!pipeline.bakeSupported || !TerrainBaker::enableBaking())
      return false;

    // Terrain: the auto rule's colour layers (D3D11Rtx auto terrain), or a
    // texture tagged as terrain whose content hash is known.
    const auto& terrainTextures = RtxOptions::terrainTextures();
    uint32_t colourLayers = 0;
    bool tagged = false;

    for (const auto& b : draw.bindings) {
      if (b.kind != REMIX_VKFE_BINDING_TEXTURE || !b.image || !pixelStage(b) || isDepthFormat(b.format))
        continue;

      bool isTarget = false;

      for (uint32_t r = 0; r < draw.desc.render_target_count; r++)
        isTarget |= draw.desc.render_targets[r].image == b.image;

      if (isTarget)
        continue;

      if (isColorLayerFormat(b.format) && b.mip_levels > 1 && b.extent.width >= 64)
        colourLayers++;

      auto t = m_capture.textures.find(b.image);

      if (t != m_capture.textures.end() && t->second.contentHashed && t->second.view != nullptr
       && terrainTextures.count(t->second.view->image()->getHash()))
        tagged = true;
    }

    if (!tagged && !(RtxOptions::dx11AutoTerrainBlend() && colourLayers >= 4u))
      return false;

    Matrix4 clipToWorld;

    if (!bakeClipToWorld(draw, clipToWorld))
      return false;

    // A new slot each frame; the frame's layout is fixed at its first bake
    // (Remix updates the layout while the game records).
    if (m_bakeSlotFrame != m_presentCount) {
      TerrainBaker::ExternalBakeLayout layout;

      if (!m_device->getCommon()->getSceneManager().getTerrainBaker().getExternalBakeLayout(layout))
        return false;

      m_bakeSlotFrame = m_presentCount;
      m_bakeSlot = (m_bakeSlot + 1) % kBakeSlots;

      BakeSlot& slot = m_bakeSlots[m_bakeSlot];
      slot.layout = layout;
      slot.frameBakes.clear();
      slot.cleared.clear();
      slot.bakes = 0;
    }

    BakeSlot& slot = m_bakeSlots[m_bakeSlot];
    const TerrainBaker::ExternalBakeLayout& layout = slot.layout;

    if (slot.bakes >= kBakesPerFrame || layout.numCascades == 0 || layout.numCascades > REMIX_VKFE_MAX_BAKE_CASCADES)
      return false;

    const VkExtent2D extent = { layout.cascadeMapSizeX * kBakeLevelResolution, layout.cascadeMapSizeY * kBakeLevelResolution };
    const auto& s = pipeline.state;
    const VkFormat albedoFormat = s.render_target_formats[pipeline.bakeAlbedoTarget];
    const BakeImage* albedo = nullptr;
    uint32_t views = 0;

    // Attachments in the subpass's order, then depth: the albedo target is
    // this slot's image, the rest are written and never read.
    for (uint32_t r = 0; r < std::min(s.render_target_count, 8u); r++) {
      const VkFormat f = s.render_target_formats[r];

      if (f == VK_FORMAT_UNDEFINED) {
        capture->bake_views[views++] = VK_NULL_HANDLE;
        continue;
      }

      const BakeImage* image = r == pipeline.bakeAlbedoTarget
        ? bakeImage(slot.albedo, uint64_t(f), f, extent)
        : bakeImage(m_bakeScratch, (uint64_t(f) << 8) | r, f, extent);

      if (!image)
        return false;

      if (r == pipeline.bakeAlbedoTarget)
        albedo = image;

      capture->bake_views[views++] = image->attachmentView->handle();
    }

    if (s.depth_format != VK_FORMAT_UNDEFINED) {
      const BakeImage* image = bakeImage(m_bakeScratch, (uint64_t(s.depth_format) << 8) | 0xffu, s.depth_format, extent);

      if (!image)
        return false;

      capture->bake_views[views++] = image->attachmentView->handle();
    }

    if (!albedo || albedo->sampledView == nullptr)
      return false;

    // Per cascade: game clip -> world -> cascade clip, as four rows, and
    // its tile of the grid (Vulkan's y flipped, as TerrainBaker bakes).
    const DxvkBufferSliceHandle matrices = m_bakeMatrices->getSliceHandle();
    uint8_t* mapped = reinterpret_cast<uint8_t*>(m_bakeMatrices->mapPtr(0));
    const VkDeviceSize base = (VkDeviceSize(m_bakeSlot) * kBakesPerFrame + slot.bakes) * REMIX_VKFE_MAX_BAKE_CASCADES * 64u;
    const float level = float(kBakeLevelResolution);

    for (uint32_t c = 0; c < layout.numCascades; c++) {
      const Matrix4 bake = layout.worldToCascadeClip[c] * clipToWorld;
      float rows[16];

      for (uint32_t r = 0; r < 4; r++) {
        rows[r * 4 + 0] = bake[0][r];
        rows[r * 4 + 1] = bake[1][r];
        rows[r * 4 + 2] = bake[2][r];
        rows[r * 4 + 3] = bake[3][r];
      }

      std::memcpy(mapped + base + c * 64u, rows, sizeof(rows));

      const uint32_t x = c % layout.cascadeMapSizeX;
      const uint32_t y = c / layout.cascadeMapSizeX;

      capture->bake_viewports[c]      = { x * level, (y + 1) * level, level, -level, 0.0f, 1.0f };
      capture->bake_scissors[c]       = { { int32_t(x * kBakeLevelResolution), int32_t(y * kBakeLevelResolution) },
                                          { kBakeLevelResolution, kBakeLevelResolution } };
      capture->bake_matrix_offsets[c] = matrices.offset + base + c * 64u;
    }

    // The frame's bake into this image, shared by all its terrain draws.
    auto& frameBake = slot.frameBakes[albedoFormat];

    if (!frameBake) {
      frameBake = std::make_shared<ExternalTerrainBake>();
      frameBake->image                  = albedo->sampledView;
      frameBake->numCascades            = layout.numCascades;
      frameBake->cascadeMapSizeX        = layout.cascadeMapSizeX;
      frameBake->cascadeMapSizeY        = layout.cascadeMapSizeY;
      frameBake->lastCascadeScale       = layout.lastCascadeScale;
      frameBake->worldToCascade0Texture = layout.worldToCascade0Texture;
      frameBake->frame                  = m_presentCount;
    }

    capture->bake               = 1;
    capture->bake_clear         = slot.cleared.count(uint32_t(albedoFormat)) ? 0u : 1u;
    capture->bake_target        = pipeline.bakeAlbedoTarget;
    capture->bake_view_count    = views;
    capture->bake_extent        = extent;
    capture->bake_cascade_count = layout.numCascades;
    capture->bake_matrix_buffer = matrices.handle;
    capture->bake_generation    = m_bakeGeneration;

    draw.bakeSlot = int32_t(m_bakeSlot);
    draw.bake     = frameBake;
    slot.bakes++;
    return true;
  }


  // -------------------------------------------------------------- textures

  Rc<DxvkImageView> D3D11VkFrontendDevice::wrapTexture(const remix_vkfe_binding& binding) {
    auto& st = m_capture;

    // The game destroyed the image after the draw being committed was
    // recorded (GPU-driven draws commit two frames late; loading screens
    // free textures constantly): wrapping the freed handle crashes the driver.
    // The draw keeps its geometry, without this texture.
    auto destroyed = m_destroyedImages.find(binding.image);

    if (destroyed != m_destroyedImages.end() && destroyed->second.seq > m_commitImageSeq)
      return nullptr;

    auto& entry = st.textures[binding.image];

    const bool same = entry.view != nullptr && entry.format == binding.format
      && entry.extent.width == binding.extent.width && entry.extent.height == binding.extent.height
      && entry.mips == binding.mip_levels;

    if (!same) {
      // New image, or the game reused the handle for another one.
      entry = D3D11VkCaptureState::Texture();

      DxvkImageCreateInfo info;
      info.type        = VK_IMAGE_TYPE_2D;
      info.format      = binding.format;
      info.flags       = 0;
      info.sampleCount = VK_SAMPLE_COUNT_1_BIT;
      info.extent      = { binding.extent.width, binding.extent.height, 1 };
      info.numLayers   = std::max(binding.array_layers, 1u);
      info.mipLevels   = std::max(binding.mip_levels, 1u);
      info.usage       = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
      info.stages      = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT
                       | VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR | VK_PIPELINE_STAGE_TRANSFER_BIT;
      info.access      = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT;
      info.tiling      = VK_IMAGE_TILING_OPTIMAL;
      // The game keeps the image in this layout while it is bound for
      // sampling; Remix samples it without transitions.
      info.layout      = binding.layout != VK_IMAGE_LAYOUT_UNDEFINED
                       ? binding.layout : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
      info.shared      = VK_TRUE;

      Rc<DxvkImage> image = m_device->createImageFromVkImage(info, binding.image);

      // Provisional identity until the content hash arrives
      // (processTextureHashes).
      struct { uint64_t image; uint32_t format, width, height, mips; } key = {
        uint64_t(binding.image), uint32_t(binding.format), binding.extent.width, binding.extent.height, info.mipLevels };
      image->setHash(XXH3_64bits(&key, sizeof(key)));

      DxvkImageViewCreateInfo view;
      view.type      = VK_IMAGE_VIEW_TYPE_2D;
      view.format    = binding.format;
      view.usage     = VK_IMAGE_USAGE_SAMPLED_BIT;
      view.aspect    = VK_IMAGE_ASPECT_COLOR_BIT;
      view.minLevel  = 0;
      view.numLevels = info.mipLevels;
      view.minLayer  = 0;
      view.numLayers = 1;

      entry.view   = m_device->createImageView(image, view);
      entry.format = binding.format;
      entry.extent = binding.extent;
      entry.mips   = binding.mip_levels;
    }

    // Content hash: read back one mip of at most 1 MiB (the largest that
    // fits, so small textures hash their full detail) once per texture.
    constexpr uint32_t     kMaxReadbacksPerFrame = 32;
    constexpr VkDeviceSize kMaxReadbackBytes = 1ull << 20;

    // Only images the game (or the layer) created with transfer-source usage
    // can be copied; vkd3d-proton's images always are.
    if (!entry.contentHashed && entry.readback == nullptr && binding.transfer_src
     && st.readbacksThisFrame < kMaxReadbacksPerFrame) {
      const DxvkFormatInfo* fmt = imageFormatInfo(entry.format);
      const uint32_t levels = std::max(entry.mips, 1u);

      if (fmt && fmt->elementSize && !(fmt->aspectMask & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT))) {
        uint32_t level = 0;
        VkDeviceSize bytes = 0;
        VkExtent3D extent = {};

        for (level = 0; level < levels; level++) {
          extent = { std::max(entry.extent.width >> level, 1u), std::max(entry.extent.height >> level, 1u), 1u };
          const VkDeviceSize bw = (extent.width  + fmt->blockSize.width  - 1) / fmt->blockSize.width;
          const VkDeviceSize bh = (extent.height + fmt->blockSize.height - 1) / fmt->blockSize.height;
          bytes = bw * bh * fmt->elementSize;

          if (bytes <= kMaxReadbackBytes)
            break;
        }

        if (level < levels && bytes) {
          DxvkBufferCreateInfo info = {};
          info.size   = bytes;
          info.usage  = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
          info.stages = VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_HOST_BIT;
          info.access = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_HOST_READ_BIT;

          entry.readback = m_device->createBuffer(info,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT,
            DxvkMemoryStats::Category::RTXBuffer, "Remix front-end texture hash readback");
          entry.readbackSize  = bytes;
          entry.readbackLevel = level;
          entry.readbackFrame = m_presentCount;
          st.readbacksThisFrame++;

          const VkImageSubresourceLayers layers = { VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1 };

          m_context->EmitCs([buffer = entry.readback, image = entry.view->image(), layers, extent] (DxvkContext* ctx) {
            ctx->copyImageToBuffer(buffer, 0, 0, 0, image, layers, VkOffset3D(), extent);
            ctx->emitMemoryBarrier(0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
              VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_READ_BIT);
          });
        }
      }
    }

    entry.lastFrame = m_presentCount;
    return entry.view;
  }


  Rc<DxvkImageView> D3D11VkFrontendDevice::wrapColorTexture(const remix_vkfe_binding& binding) {
    Rc<DxvkImageView> view = wrapTexture(binding);

    if (view == nullptr)
      return nullptr;

    // Grey-scale colour maps (R8, BC4) and grey + alpha (R8G8, BC5 used as
    // luminance-alpha): the game's shader reads .r as the colour.
    VkComponentMapping swizzle;
    switch (binding.format) {
      case VK_FORMAT_R8_UNORM:
      case VK_FORMAT_R8_SRGB:
      case VK_FORMAT_R16_UNORM:
      case VK_FORMAT_R16_SFLOAT:
      case VK_FORMAT_BC4_UNORM_BLOCK:
        swizzle = { VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_ONE };
        break;
      case VK_FORMAT_R8G8_UNORM:
      case VK_FORMAT_R8G8_SRGB:
        swizzle = { VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_G };
        break;
      default:
        return view;
    }

    auto& entry = m_capture.textures[binding.image];

    if (entry.colorView == nullptr) {
      DxvkImageViewCreateInfo info = view->info();
      info.swizzle = swizzle;
      entry.colorView = m_device->createImageView(view->image(), info);
    }

    return entry.colorView;
  }


  void D3D11VkFrontendDevice::processTextureHashes() {
    // The copy runs in Remix's submission for the frame it was recorded
    // in; three frames later that submission has completed.
    constexpr uint64_t kLag = 3;

    for (auto& t : m_capture.textures) {
      auto& e = t.second;

      if (e.readback == nullptr || e.readbackFrame + kLag > m_presentCount)
        continue;

      const void* data = e.readback->mapPtr(0);

      if (data) {
        // Format and size are part of the identity: two textures with equal
        // bytes but different layouts are different assets.
        struct { uint32_t format, width, height, level; } key = {
          uint32_t(e.format), e.extent.width, e.extent.height, e.readbackLevel };
        XXH64_hash_t h = XXH3_64bits_withSeed(&key, sizeof(key), 0);
        h = XXH3_64bits_withSeed(data, size_t(e.readbackSize), h);

        if (!h)
          h = 1;

        e.view->image()->setHash(h);
        e.contentHashed = true;
      }

      e.readback = nullptr;
    }
  }


  // ---------------------------------------------------------------- lights

  void D3D11VkFrontendDevice::importLights(const std::vector<const D3D11VkDraw*>& draws, const FrameCamera& camera) {
    ScopedCpuProfileZone();

    if (!RtxOptions::dx11ImportTiledLights() || !camera.valid || camera.clipUsesWDepth)
      return;

    D3D11LightDecodeView view;
    view.viewToWorld    = camera.haveView ? Matrix4d(inverse(camera.view)) : Matrix4d();
    view.cameraWorld    = view.viewToWorld * Vector4d(0.0, 0.0, 0.0, 1.0);
    view.intensityScale = RtxOptions::dx11TiledLightIntensity();
    view.maxLights      = RtxOptions::dx11TiledLightMaxPerFrame();

    const D3D11EngineProfile& engine = GetD3D11EngineProfile();
    D3D11TiledLightLayout tiled = engine.facts->lights;

    if (tiled == D3D11TiledLightLayout::None
     && (engine.family() == D3D11EngineFamily::Unknown || engine.family() == D3D11EngineFamily::Creation))
      tiled = D3D11TiledLightLayout::Creation48;

    std::vector<Dx11LightDesc> lights;
    std::unordered_set<const std::vector<uint8_t>*> seen;
    auto& st = m_capture;

    for (const D3D11VkDraw* draw : draws) {
      for (size_t i = 0; i < draw->bindings.size() && lights.size() < view.maxLights; i++) {
        const auto& b = draw->bindings[i];
        const auto* bytes = bytesOf(*draw, i, 96);

        if (!bytes || !seen.insert(bytes).second)
          continue;

        std::vector<Dx11LightDesc> found;

        // UE's light list is whole 96-byte records (float4 x 6); other
        // storage buffers would only meet the per-entry checks by chance.
        if (engine.family() == D3D11EngineFamily::Unreal && b.kind == REMIX_VKFE_BINDING_STORAGE_BUFFER
         && bytes->size() % 96u == 0)
          D3D11DecodeUnrealLocalLights(bytes->data(), bytes->size(), view, found);
        else if (tiled == D3D11TiledLightLayout::Frostbite96 && b.kind == REMIX_VKFE_BINDING_CONSTANT_BUFFER)
          D3D11DecodeFrostbitePunctualLights(bytes->data(), bytes->size(), view, found);
        else if (tiled != D3D11TiledLightLayout::None && tiled != D3D11TiledLightLayout::Frostbite96
              && b.kind == REMIX_VKFE_BINDING_STORAGE_BUFFER) {
          const uint32_t stride = D3D11TiledLightStride(tiled);

          if (stride && bytes->size() % stride == 0) {
            std::vector<Vector3> sunDirections;
            D3D11DecodeTiledLights(tiled, bytes->data(), bytes->size(), view, found, &sunDirections);

            for (const Vector3& d : sunDirections)
              st.sunVotes.push_back({ d, 1000u });
          }
        }

        // Every light once per frame, however many draws bind its buffer.
        for (const Dx11LightDesc& l : found) {
          if (st.lightKeys.insert(XXH3_64bits(&l, sizeof(l))).second)
            lights.push_back(l);
        }
      }
    }

    if (lights.empty())
      return;

    st.statLights += uint32_t(lights.size());

    m_context->EmitCs([cLights = std::move(lights)](DxvkContext* ctx) {
      static_cast<RtxContext*>(ctx)->addLights(cLights.data(), uint32_t(cLights.size()));
    });
  }


  void D3D11VkFrontendDevice::learnSun(const D3D11VkDraw& draw) {
    // A shadow cascade draw carries the light's orthographic view-projection:
    // its depth row is the sun's direction of travel (D3D11Rtx::
    // LearnSunFromShadowDraw). Per-object matrices vote apart; the shared
    // one wins in applySun.
    for (size_t i = 0; i < draw.bindings.size(); i++) {
      if (!vertexStage(draw.bindings[i]) || draw.bindings[i].kind == REMIX_VKFE_BINDING_STORAGE_BUFFER)
        continue;

      const auto* bytes = bytesOf(draw, i);

      if (!bytes)
        continue;

      for (size_t off = 0; off + 64 <= std::min<size_t>(bytes->size(), 1024u); off += 16) {
        const Matrix4 raw = D3D11ReadMatrix(bytes->data(), off, bytes->size());

        for (int t = 0; t < 2; t++) {
          // rows[r] = the matrix row producing clip component r, in the
          // layout D3D11ReadMatrix returns (16 bytes per row); the transpose
          // covers column-major storage. Orthographic: clip w is 1.
          const Matrix4 rows = t ? transpose(raw) : raw;
          const Vector4 r3 = rows[3];

          if (std::abs(r3.x) + std::abs(r3.y) + std::abs(r3.z) > 1.0e-4f || std::abs(r3.w - 1.0f) > 1.0e-3f)
            continue;

          Vector3 d(rows[2].x, rows[2].y, rows[2].z);
          const float len = length(d);

          if (!std::isfinite(len) || len < 1.0e-8f)
            continue;

          d = d * (1.0f / len);

          // Reversed depth: the depth row points the other way.
          if (draw.pipeline && (draw.pipeline->state.depth_compare == VK_COMPARE_OP_GREATER
                             || draw.pipeline->state.depth_compare == VK_COMPARE_OP_GREATER_OR_EQUAL))
            d = Vector3(0.0f) - d;

          bool voted = false;

          for (auto& vote : m_capture.sunVotes) {
            if (dot(vote.direction, d) > 0.999f) {
              vote.count++;
              voted = true;
              break;
            }
          }

          if (!voted && m_capture.sunVotes.size() < 16u)
            m_capture.sunVotes.push_back({ d, 1u });

          break;
        }
      }
    }
  }


  void D3D11VkFrontendDevice::applySun(const FrameCamera& camera) {
    auto& st = m_capture;
    const D3D11VkCaptureState::SunVote* best = nullptr;

    for (const auto& vote : st.sunVotes) {
      if (!best || vote.count > best->count)
        best = &vote;
    }

    // Only meaningful when the RT world has the game's world axes.
    if (camera.haveView && best && best->count >= 16u
     && (!st.sunValid || dot(best->direction, st.sunDirection) < 0.99996f)) {
      // Light directions are in world space; the vote was in the matrix's
      // space, which for the shared cascade matrix is world space.
      st.sunDirection = best->direction;
      st.sunValid = true;
      // Default layer: never written to the user's config, and a direction
      // the user sets there still wins.
      LightManager::fallbackLightDirectionObject().setDeferred(st.sunDirection, RtxOptionLayer::getDefaultLayer());
    }

    st.sunVotes.clear();
  }


  // ----------------------------------------------------------------- draws

  void D3D11VkFrontendDevice::commitDraw(
    const D3D11VkDraw&                draw,
          uint32_t                    vertexCount,
    const FrameCamera&                camera,
    const DrawContext&                context) {
    const D3D11VkPipeline& pipeline = *draw.pipeline;
    const remix_vkfe_pipeline_desc& state = pipeline.state;
    const D3D11VkCaptureLayout& layout = pipeline.captureLayout;
    auto& st = m_capture;

    if (!vertexCount)
      return;

    // Texture wraps below check against the point this draw was recorded at.
    m_commitImageSeq = draw.imageSeq;

    DrawCallState dcs;
    RasterGeometry& geo = dcs.geometryData;

    const uint32_t stride = pipeline.captureStride;
    const VkDeviceSize bytes = VkDeviceSize(vertexCount) * stride;
    const DxvkBufferSlice slice(draw.captureBuffer, draw.captureOffset, bytes);

    geo.positionBuffer = RasterBuffer(slice, 0, stride, VK_FORMAT_R32G32B32A32_SFLOAT);

    if (layout.texcoord)
      geo.texcoordBuffer = RasterBuffer(slice, layout.texcoordOffset, stride, VK_FORMAT_R32G32_SFLOAT);

    // Vertex colour: the input the pixel shader multiplies a sample with,
    // streamed out beside the position (interleaver packs float RGBA).
    if (layout.color && layout.colorComponents == 4)
      geo.color0Buffer = RasterBuffer(slice, layout.colorOffset, stride, VK_FORMAT_R32G32B32A32_SFLOAT);

    geo.vertexCount = vertexCount;
    geo.indexCount  = 0;
    geo.topology    = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    // Unprojected clip positions keep the rasterizer's winding only up to
    // the projection's handedness; trace both faces (D3D11Rtx raySafeTwoSided).
    geo.cullMode    = VK_CULL_MODE_NONE;
    geo.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;

    geo.postVsPositionIsHomogeneousClip = true;
    geo.boundingBox.invalidate();

    DrawCallTransforms& t = dcs.transformData;

    if (context.lift2D) {
      // 2D lift (D3D11Rtx): NDC (x/w, y/w) onto the plane at this layer's
      // draw-order depth Z, scaled by Z so the synthetic camera projects it
      // back onto the same pixels: position = (x/w * Z / P00, y/w * Z / P11, Z).
      const float z = context.liftDepth;
      Matrix4 clipToPosition;
      clipToPosition[0] = Vector4(z / camera.projection[0][0], 0.0f, 0.0f, 0.0f);
      clipToPosition[1] = Vector4(0.0f, z / camera.projection[1][1], 0.0f, 0.0f);
      clipToPosition[2] = Vector4(0.0f, 0.0f, 0.0f, 0.0f);
      clipToPosition[3] = Vector4(0.0f, 0.0f, z, 1.0f);
      geo.postVsClipToPosition = clipToPosition;
      geo.postVsClipUsesWDepth = false;
    } else {
      geo.postVsClipUsesWDepth = camera.clipUsesWDepth;
      // Unproject with the projection the draw was rasterized with, jitter
      // included, or every vertex moves by the frame's sub-pixel offset.
      geo.postVsClipToPosition = inverse(camera.jitteredProjection);
    }

    // Identity, stable across frames and runs: the pipeline's code hash and
    // the draw's shape and buffer sizes (handles change between runs). The
    // mesh keeps its BLAS and its hash for texture tags and replacements;
    // the positions are refreshed every frame because they are captured
    // in clip space (dynamic), which Remix applies as a refit.
    {
      uint64_t sizes = 0;

      for (const auto& vb : draw.vertexBuffers)
        sizes = sizes * 31u + uint64_t(vb.size == VK_WHOLE_SIZE ? 0 : vb.size) + vb.stride;

      const uint64_t identity[] = {
        pipeline.stableHash, uint64_t(draw.desc.vertex_or_index_count), uint64_t(draw.desc.first_vertex_or_index),
        uint64_t(draw.desc.instance_count), uint64_t(int64_t(draw.desc.vertex_offset)), sizes,
        uint64_t(draw.desc.indexed), uint64_t(vertexCount),
      };

      XXH64_hash_t id = XXH3_64bits(identity, sizeof(identity));

      if (!id)
        id = 0x9e3779b97f4a7c15ull;

      geo.postVsCaptureIdentity = id;
      geo.postVsPositionHashSeed = id;
      geo.hasPostVsPositionHashSeed = true;
      geo.postVsCapturedPositionsDynamic = true;
    }

    // Transforms: clip -> view by the matrix above in the interleaver, view
    // -> world by inverse(V). Without a recovered view the RT world is the
    // current view space (D3D11Rtx cameraRelativeView).
    t.viewToProjection = camera.projection;
    t.usedViewportFallbackProjection = camera.clipUsesWDepth;

    if (camera.haveView && !context.lift2D) {
      t.worldToView   = camera.view;
      t.objectToWorld = inverse(camera.view);
      t.objectToView  = Matrix4();
    } else {
      t.worldToView   = Matrix4();
      t.objectToWorld = Matrix4();
      t.objectToView  = Matrix4();
      t.cameraRelativeView = true;
    }

    t.sanitize();

    // ------------------------------------------------------------ material
    LegacyMaterialData& mat = dcs.materialData;
    const D3D11VkShaderAnalysis& analysis = pipeline.pixelAnalysis;

    // Role of a bound texture from the shader analysis (Vulkan descriptor
    // set / binding / element); nullptr for bindless or unknown.
    auto roleOf = [&](const remix_vkfe_binding& b) -> const D3D11VkTextureRole* {
      for (const auto& r : analysis.textures) {
        if (r.set == b.space && r.binding == b.slot && (r.element == ~0u ? false : r.element == b.array_element))
          return &r;
      }

      return nullptr;
    };

    const remix_vkfe_binding* albedo = nullptr;
    uint64_t albedoScore = 0;
    const remix_vkfe_binding* normal = nullptr;
    uint8_t normalEncoding = 0;
    const remix_vkfe_binding* rough = nullptr;
    uint8_t roughChannel = 0;
    bool roughIsSmoothness = false;
    const remix_vkfe_binding* metallic = nullptr;
    uint8_t metallicChannel = 0;
    const remix_vkfe_binding* emissive = nullptr;
    uint32_t colourLayers = 0;
    bool samplesDepth = false;
    bool samplesEarlierTarget = false;

    for (const auto& b : draw.bindings) {
      if (b.kind != REMIX_VKFE_BINDING_TEXTURE || !b.image || !pixelStage(b))
        continue;

      if (isDepthFormat(b.format)) {
        samplesDepth = true;
        continue;
      }

      bool isTarget = false;

      for (uint32_t r = 0; r < draw.desc.render_target_count; r++)
        isTarget |= draw.desc.render_targets[r].image == b.image;

      if (isTarget)
        continue;

      const bool earlierTarget = context.earlierTargets && context.earlierTargets->count(b.image);
      samplesEarlierTarget |= earlierTarget;

      if (earlierTarget)
        continue;

      if (const D3D11VkTextureRole* role = roleOf(b)) {
        if (role->normal && !normal) {
          normal = &b;
          normalEncoding = role->normalEncoding;

          // CRYENGINE "_ddna": smoothness in the normal map's alpha, as
          // the DX11 path takes it.
          if (role->smoothnessChannel >= 0 && !rough) {
            rough = &b;
            roughChannel = uint8_t(role->smoothnessChannel);
            roughIsSmoothness = true;
          }
          continue;
        }

        // Packed maps (Unity mask map, Unreal ORM) carry roughness or
        // smoothness beside metallic.
        auto takeRoughness = [&]() {
          if (rough)
            return false;

          if (role->smoothnessChannel >= 0) {
            rough = &b;
            roughChannel = uint8_t(role->smoothnessChannel);
            roughIsSmoothness = true;
            return true;
          }

          if (role->roughnessChannel >= 0) {
            rough = &b;
            roughChannel = uint8_t(role->roughnessChannel);
            roughIsSmoothness = false;
            return true;
          }

          return false;
        };

        if (role->metallicChannel >= 0 && !metallic) {
          metallic = &b;
          metallicChannel = uint8_t(role->metallicChannel);
          takeRoughness();
          continue;
        }

        if (takeRoughness())
          continue;

        if (role->emissive && !emissive) {
          emissive = &b;
          continue;
        }

        if (role->albedo && (!albedo || !roleOf(*albedo) || !roleOf(*albedo)->albedo)) {
          albedo = &b;
          albedoScore = ~0ull;
          continue;
        }
      } else if (analysis.sampledNormal && !normal && isNormalMapFormat(b.format)) {
        // The shader unpacks a normal map but the texture cannot be named
        // (bindless heap): the two-channel texture is it.
        normal = &b;
        normalEncoding = analysis.normalEncoding;
        continue;
      }

      if (isNormalMapFormat(b.format))
        continue;

      if (isColorLayerFormat(b.format) && b.mip_levels > 1 && b.extent.width >= 64)
        colourLayers++;

      // Mipped textures are authored assets; full-screen intermediates
      // rarely have mips.
      const uint64_t score = uint64_t(b.extent.width) * b.extent.height * (b.mip_levels > 1 ? 4u : 1u);

      if (score > albedoScore) {
        albedo = &b;
        albedoScore = score;
      }
    }

    // Material textures resolved from the engine's own material data
    // (Starfield's bindless tables) take precedence over shader roles.
    const auto& em = draw.explicitMaterial;
    bool authoredRoughness = false;

    if (em.albedo >= 0) {
      albedo = &draw.bindings[size_t(em.albedo)];
      albedoScore = ~0ull;
    }

    if (em.normal >= 0) {
      normal = &draw.bindings[size_t(em.normal)];
      normalEncoding = em.normalEncoding;
    }

    if (em.roughness >= 0) {
      rough = &draw.bindings[size_t(em.roughness)];
      roughChannel = 0;
      roughIsSmoothness = false;
      authoredRoughness = true;
    }

    if (em.metallic >= 0) {
      metallic = &draw.bindings[size_t(em.metallic)];
      metallicChannel = 0;
    }

    // Signed two-channel normal maps are sampled in [-1, 1] already.
    if (normal && (normalEncoding == 3 || normalEncoding == 0)
     && (normal->format == VK_FORMAT_BC5_SNORM_BLOCK || normal->format == VK_FORMAT_R8G8_SNORM))
      normalEncoding = 6;

    // Every texture the pixel shader samples goes to Remix's texture browser,
    // as on DX11, so it can be tagged with any category.
    for (const auto& b : draw.bindings) {
      if (b.kind == REMIX_VKFE_BINDING_TEXTURE && b.image && pixelStage(b) && !isDepthFormat(b.format)
       && !(context.earlierTargets && context.earlierTargets->count(b.image))) {
        Rc<DxvkImageView> view = wrapTexture(b);
        const XXH64_hash_t hash = view != nullptr ? view->image()->getHash() : 0;

        if (hash)
          ImGUI::AddTexture(hash, view, textureUiFlags(view));
      }
    }

    if (albedo && (layout.texcoord || context.lift2D)) {
      mat.colorTextures[0] = textureRef(wrapColorTexture(*albedo));
      mat.samplers[0] = st.sampler;
      mat.colorTextureIsSrgb = isSrgbFormat(albedo->format);
    }

    if (normal && layout.texcoord) {
      mat.normalTexture = textureRef(wrapTexture(*normal));
      mat.normalEncoding = normalEncoding;
      st.statNormalMaps++;
    }

    if (metallic && layout.texcoord) {
      mat.metallicTexture = textureRef(wrapTexture(*metallic));
      mat.metallicChannel = metallicChannel;
    }

    if (emissive && layout.texcoord && !context.lift2D)
      mat.emissiveTexture = textureRef(wrapTexture(*emissive));

    if (rough && layout.texcoord) {
      mat.roughnessTexture = textureRef(wrapTexture(*rough));
      mat.roughnessChannel = roughChannel;
      mat.roughnessIsSmoothness = roughIsSmoothness;
      mat.roughnessAuthored = authoredRoughness;
    } else if (analysis.smoothnessChannel >= 0 && albedo && layout.texcoord) {
      // Smoothness packed in the albedo texture's channel (alpha, typically).
      mat.roughnessTexture = mat.colorTextures[0];
      mat.roughnessChannel = uint8_t(analysis.smoothnessChannel);
      mat.roughnessIsSmoothness = true;
    }

    if (geo.color0Buffer.defined()) {
      // The shader multiplies the texture by this colour: it is albedo tint,
      // not baked lighting.
      mat.modulateVertexColor = true;
      mat.isVertexColorBakedLighting = false;
    }

    mat.blendMode.enableBlending = state.blend_enable ? VK_TRUE : VK_FALSE;
    mat.blendMode.colorSrcFactor = state.src_color_blend;
    mat.blendMode.colorDstFactor = state.dst_color_blend;
    mat.blendMode.colorBlendOp   = state.color_blend_op;
    mat.blendMode.alphaSrcFactor = state.src_alpha_blend;
    mat.blendMode.alphaDstFactor = state.dst_alpha_blend;
    mat.blendMode.alphaBlendOp   = VK_BLEND_OP_ADD;
    mat.blendMode.writeMask      = state.color_write_mask;
    mat.alphaTestEnabled         = state.alpha_to_coverage != 0;
    mat.isLiftedSprite           = context.lift2D;

    // Refracting water: the pixel shader samples a copy of the scene made
    // earlier this frame and has its own normal map. Remix's animated-water
    // layering replaces the motion the game's shader gave it.
    if (samplesEarlierTarget && mat.normalTexture.isValid() && !context.lift2D) {
      mat.isRefractiveSurface = true;
      dcs.setCategory(InstanceCategories::AnimatedWater, true);
      st.statWater++;
    }

    mat.updateCachedHash();

    // ---------------------------------------------------------- categories
    const bool zEnable = state.depth_test != 0;
    const bool zWriteEnable = state.depth_write != 0;

    dcs.usesVertexShader = true;
    dcs.usesPixelShader  = true;
    dcs.zEnable          = zEnable;
    dcs.zWriteEnable     = zWriteEnable;
    dcs.minZ             = draw.desc.viewport.minDepth;
    dcs.maxZ             = draw.desc.viewport.maxDepth;
    dcs.cameraType       = CameraType::Unknown;
    dcs.drawCallID       = st.drawCallId++;
    dcs.passDescription  = "VkFrontend";

    // Texture-hash categories (sky, UI, decals, particles, terrain, water,
    // ignore, ... - everything tagged in the Remix menu or rtx.conf),
    // keeping the automatic water tag.
    const bool autoWater = dcs.testCategoryFlags(InstanceCategories::AnimatedWater);
    dcs.setupCategoriesForTexture();

    if (autoWater)
      dcs.setCategory(InstanceCategories::AnimatedWater, true);

    if (dcs.testCategoryFlags(InstanceCategories::Ignore)) {
      st.statIgnored++;
      return;
    }

    // Far-plane sky pinned through the viewport depth range
    // (MinDepth == MaxDepth == far), as D3D11Rtx detects it.
    {
      const float minD = draw.desc.viewport.minDepth, maxD = draw.desc.viewport.maxDepth;
      const bool reversed = GetD3D11EngineProfile().facts->depth == D3D11DepthConvention::Reversed
        || state.depth_compare == VK_COMPARE_OP_GREATER || state.depth_compare == VK_COMPARE_OP_GREATER_OR_EQUAL;

      if (zEnable && std::abs(maxD - minD) <= 1.0e-6f
       && ((reversed && minD <= 1.0e-6f) || (!reversed && minD >= 1.0f - 1.0e-6f))) {
        dcs.setCategory(InstanceCategories::Sky, true);
        dcs.skyAutoDetected = true;
        st.statSky++;
      }
    }

    // Splat-blended terrain (D3D11Rtx auto terrain): several colour layers
    // sampled by an opaque, depth-writing draw.
    if (RtxOptions::dx11AutoTerrainBlend() && !context.lift2D && zEnable && zWriteEnable
     && !state.blend_enable && colourLayers >= 4u && !dcs.testCategoryFlags(InstanceCategories::Terrain)) {
      dcs.setCategory(InstanceCategories::Terrain, true);
      st.statTerrain++;
    }

    // A draw the caller baked is terrain whatever tagged it.
    if (draw.baked && draw.bake != nullptr)
      dcs.setCategory(InstanceCategories::Terrain, true);

    // Terrain goes through TerrainBaker with this frame's bake, or with an
    // empty one (registers the mesh, so the next frame's cascades cover it).
    if (dcs.testCategoryFlags(InstanceCategories::Terrain)) {
      static const auto kUnbaked = std::make_shared<const ExternalTerrainBake>();
      dcs.externalTerrainBake = draw.baked && draw.bake != nullptr ? draw.bake : kUnbaked;
    }

    // Automatic decal / particle classification (D3D11Rtx): any manual tag
    // on the texture wins (categories already set).
    if (RtxOptions::dx11AutoClassifyDecalsAndParticles() && dcs.getCategoryFlags().raw() == 0u && !context.lift2D) {
      const bool blending = state.blend_enable != 0;
      const bool additive = additiveBlend(state);
      const float depthBias = state.depth_bias_enable
        ? std::abs(state.depth_bias_constant) + std::abs(state.depth_bias_slope) : 0.0f;
      const bool overlay = zEnable && !zWriteEnable;
      const bool boxSized = draw.desc.vertex_or_index_count <= 36u && !draw.desc.indirect_buffer;
      const bool gbufferDecal = GetD3D11EngineProfile().facts->decalsInGBuffer
        && overlay && blending && !additive && draw.desc.render_target_count >= 2u && !samplesDepth;

      if (overlay && samplesDepth && boxSized && blending && !additive) {
        // A small blended box reading scene depth: a deferred / DBuffer decal.
        dcs.setCategory(InstanceCategories::DecalDynamic, true);
        st.statDecals++;
      } else if (overlay && blending && (depthBias > 0.0f || gbufferDecal) && !additive) {
        dcs.setCategory(InstanceCategories::DecalDynamic, true);
        st.statDecals++;
      } else if (overlay && blending && (additive || samplesDepth || geo.color0Buffer.defined())) {
        dcs.setCategory(InstanceCategories::Particle, true);
        st.statParticles++;
      }
    }

    DrawParameters params;
    params.instanceCount = 1;
    params.vertexCount   = vertexCount;
    params.indexCount    = 0;
    params.firstIndex    = 0;
    params.vertexOffset  = 0;

    m_context->EmitCs([params, dcs](DxvkContext* ctx) mutable {
      static_cast<RtxContext*>(ctx)->commitGeometryToRT(params, dcs);
    });

    st.statCommitted++;
    st.statLifted += context.lift2D ? 1u : 0u;
  }


  void D3D11VkFrontendDevice::processPendingIndirect() {
    ScopedCpuProfileZone();

    // Counters are read once the frame's fence has signalled and Remix's
    // host-visibility barrier for that frame has executed (it runs in the
    // frame's own Remix submission, before that frame's present).
    constexpr uint64_t kMinLag = 2;

    auto it = m_pendingIndirect.begin();

    while (it != m_pendingIndirect.end()) {
      D3D11VkPendingIndirect& p = *it;

      if (p.frame + kMinLag > m_presentCount) {
        ++it;
        continue;
      }

      const uint32_t fenceIndex = uint32_t(p.frame % kFrameFences);
      const bool fenceReused = m_frameFenceFrame[fenceIndex] != p.frame + 1;

      if (!fenceReused && m_device->vkd()->vkGetFenceStatus(m_device->handle(), m_frameFences[fenceIndex]) != VK_SUCCESS) {
        ++it;
        continue;
      }

      const uint32_t writtenBytes = *reinterpret_cast<const uint32_t*>(
        m_counterBuffer->mapPtr(VkDeviceSize(p.draw.counterSlot) * sizeof(uint32_t)));
      const uint32_t stride = p.draw.pipeline->captureStride;
      const VkDeviceSize clamped = std::min<VkDeviceSize>(writtenBytes, p.draw.captureSize);

      // Size this pipeline's next reservations: a capture that filled its
      // range lost vertices, so double it; otherwise keep twice what was
      // written. 64 KiB .. 64 MiB (256 MiB for a generated-commands stream,
      // which can hold a whole scene pass).
      {
        constexpr VkDeviceSize kMinReserve = 64ull << 10;
        const VkDeviceSize kMaxReserve = p.draw.desc.generated_commands ? (256ull << 20) : (64ull << 20);
        const VkDeviceSize next = clamped >= p.draw.captureSize
          ? p.draw.captureSize * 2
          : clamped * 2;
        m_countedReserve[p.draw.pipeline->key] = std::clamp(align(next, VkDeviceSize(256)), kMinReserve, kMaxReserve);
      }

      if (p.draw.desc.generated_commands) {
        if (p.haveCamera)
          commitGeneratedCommands(p, uint32_t(clamped));
      } else {
        uint32_t vertices = uint32_t(clamped / stride);
        vertices -= vertices % 3;

        FrameCamera camera;
        camera.valid              = p.haveCamera;
        camera.clipUsesWDepth     = p.clipUsesWDepth;
        camera.projection         = p.projection;
        camera.jitteredProjection = p.jitteredProjection;
        camera.view               = p.view;
        camera.haveView           = p.haveView;

        if (camera.valid && vertices)
          commitDraw(p.draw, vertices, camera, DrawContext());
      }

      if (p.draw.captureChunk) {
        p.draw.captureChunk->users--;
        p.draw.captureChunk->lastFrame = m_presentCount;
      }

      it = m_pendingIndirect.erase(it);
    }
  }


  // A generated-commands capture (GPU-driven DX12 draws, vkd3d ExecuteIndirect
  // with root constants) of a finished frame: the multi-draw replay wrote the
  // commands' vertices one after the other, so the copied stream gives each
  // command's range; each becomes its own draw, with its own root constants
  // (and, for Starfield, its own material).
  void D3D11VkFrontendDevice::commitGeneratedCommands(D3D11VkPendingIndirect& p, uint32_t writtenBytes) {
    const D3D11VkDraw& base = p.draw;

    if (!base.pipeline || m_argsReadback == nullptr || !base.argsCopySize)
      return;

    const D3D11VkPipeline& pipe = *base.pipeline;
    const uint32_t vertexStride  = pipe.captureStride;
    const uint32_t commandStride = base.desc.indirect_stride;
    const uint32_t argsOffset    = base.desc.indirect_args_offset;
    const uint32_t argsBytes     = base.desc.indexed ? 20u : 16u;

    if (!vertexStride || !commandStride || argsOffset + argsBytes > commandStride)
      return;

    const uint32_t count = *reinterpret_cast<const uint32_t*>(m_argsReadback->mapPtr(base.countCopyOffset));

    // The copy was never recorded (the pass did not end in a way the
    // caller could copy after).
    if (count == kArgsCountSentinel)
      return;

    const uint8_t* stream = reinterpret_cast<const uint8_t*>(m_argsReadback->mapPtr(base.argsCopyOffset));
    const uint32_t commands = std::min({ count, base.desc.indirect_draw_count, uint32_t(base.argsCopySize / commandStride) });

    FrameCamera camera;
    camera.valid              = p.haveCamera;
    camera.clipUsesWDepth     = p.clipUsesWDepth;
    camera.projection         = p.projection;
    camera.jitteredProjection = p.jitteredProjection;
    camera.view               = p.view;
    camera.haveView           = p.haveView;

    // The raw push-constant block the layer appended last: the stream's
    // constants replace bytes in it per command.
    int32_t pushIndex = -1;

    for (size_t i = 0; i < base.bindings.size(); i++) {
      if (base.bindings[i].kind == REMIX_VKFE_BINDING_PUSH_CONSTANTS)
        pushIndex = int32_t(i);
    }

    const remix_vkfe_pipeline_desc& layout = pipe.state;
    VkDeviceSize written = 0;
    uint32_t committed = 0;

    for (uint32_t c = 0; c < commands; c++) {
      const uint8_t* command = stream + size_t(c) * commandStride;
      uint32_t a[5] = {};
      std::memcpy(a, command + argsOffset, argsBytes);

      const uint32_t elements  = a[0];
      const uint32_t instances = a[1];

      // Vertices transform feedback wrote for this command: whole triangles
      // per instance (strips and fans come out as lists).
      uint32_t perInstance = 0;

      switch (layout.topology) {
        case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP:
        case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN:
          perInstance = elements >= 3 ? 3 * (elements - 2) : 0;
          break;
        default:
          perInstance = elements - elements % 3;
          break;
      }

      const uint64_t vertices = uint64_t(perInstance) * instances;
      const VkDeviceSize bytes = VkDeviceSize(vertices) * vertexStride;

      if (!vertices)
        continue;

      // The capture range filled up: later commands were not written.
      if (written + bytes > writtenBytes)
        break;

      D3D11VkDraw draw = base;
      draw.captureOffset    = base.captureOffset + written;
      draw.captureSize      = bytes;
      draw.capturedVertices = uint32_t(vertices);
      draw.desc.vertex_or_index_count = elements;
      draw.desc.instance_count        = instances;
      draw.desc.first_vertex_or_index = a[2];
      draw.desc.vertex_offset         = base.desc.indexed ? int32_t(a[3]) : 0;
      draw.desc.first_instance        = base.desc.indexed ? a[4] : a[3];

      if (pushIndex >= 0 && base.bindingBytes[size_t(pushIndex)]) {
        auto bytesCopy = std::make_shared<std::vector<uint8_t>>(*base.bindingBytes[size_t(pushIndex)]);

        for (uint32_t k = 0; k < std::min(layout.indirect_constant_count, REMIX_VKFE_MAX_INDIRECT_CONSTANTS); k++) {
          const remix_vkfe_indirect_constant& ic = layout.indirect_constants[k];

          if (bytesCopy->size() < ic.push_offset + ic.size)
            bytesCopy->resize(ic.push_offset + ic.size);

          if (ic.command_offset == REMIX_VKFE_INDIRECT_COMMAND_INDEX)
            std::memcpy(bytesCopy->data() + ic.push_offset, &c, sizeof(c));
          else if (ic.command_offset + ic.size <= commandStride)
            std::memcpy(bytesCopy->data() + ic.push_offset, command + ic.command_offset, ic.size);
        }

        draw.bindingBytes[size_t(pushIndex)] = std::move(bytesCopy);
      }

      // Starfield: root constant dword 1 (the second value of the first
      // constant range) is the material index.
      if (m_starfieldMaterials && layout.indirect_constant_count
       && layout.indirect_constants[0].size >= 8
       && layout.indirect_constants[0].command_offset != REMIX_VKFE_INDIRECT_COMMAND_INDEX
       && layout.indirect_constants[0].command_offset + 8 <= commandStride) {
        uint32_t material = 0;
        std::memcpy(&material, command + layout.indirect_constants[0].command_offset + 4, sizeof(material));
        applyMaterialChain(draw, material);
      }

      if (camera.valid)
        commitDraw(draw, uint32_t(vertices), camera, DrawContext());

      written += bytes;
      committed++;
    }

    static uint32_t s_logs = 0;

    if (s_logs < 8) {
      s_logs++;
      Logger::info(str::format("[Remix-VkFrontend][generated] commands=", commands, " of max ", base.desc.indirect_draw_count,
        " committed=", committed, " capturedBytes=", writtenBytes, " usedBytes=", written));
    }
  }


  // Starfield (Creation Engine 2) bindless materials (starfield-decomp
  // MODLOG.md, "Material chain walk"): material index -> CommonMaterialEntry
  // (t9 space2, 288 B; layer 0 index at +116) -> MaterialLayerEntry (t25
  // space2, 40 B; descriptor heap indices: +0 colour, +4 normal, +8 metal,
  // +12 roughness; 0 = none). Layer 0 only: blending further layers needs
  // the blend masks the game's shader applies.
  void D3D11VkFrontendDevice::applyMaterialChain(D3D11VkDraw& draw, uint32_t materialIndex) {
    constexpr size_t kCommonStride = 288;
    constexpr size_t kLayerStride  = 40;

    // Table bytes: copied at record / submit time when the CPU can read
    // them, else the GPU read-back of the frame (onDraw).
    const uint8_t* common = nullptr;
    size_t commonSize = 0;
    const uint8_t* layers = nullptr;
    size_t layersSize = 0;
    const void* heap = nullptr;
    uint32_t heapCount = 0;

    for (size_t i = 0; i < draw.bindings.size(); i++) {
      const auto& b = draw.bindings[i];
      const auto& bytes = draw.bindingBytes[i];

      if (b.kind == REMIX_VKFE_BINDING_STORAGE_BUFFER && b.space == 2 && b.slot == 9 && bytes) {
        common = bytes->data();
        commonSize = bytes->size();
      } else if (b.kind == REMIX_VKFE_BINDING_STORAGE_BUFFER && b.space == 2 && b.slot == 25 && bytes) {
        layers = bytes->data();
        layersSize = bytes->size();
      } else if (b.kind == REMIX_VKFE_BINDING_DESCRIPTOR_HEAP) {
        heap = b.host_data;
        heapCount = uint32_t(b.size);
      }
    }

    if (!common && draw.tableCopySize[0] && m_argsReadback != nullptr) {
      common = reinterpret_cast<const uint8_t*>(m_argsReadback->mapPtr(draw.tableCopyOffset[0]));
      commonSize = size_t(draw.tableCopySize[0]);
    }

    if (!layers && draw.tableCopySize[1] && m_argsReadback != nullptr) {
      layers = reinterpret_cast<const uint8_t*>(m_argsReadback->mapPtr(draw.tableCopyOffset[1]));
      layersSize = size_t(draw.tableCopySize[1]);
    }

    if (!m_heapView) {
      if (HMODULE core = GetModuleHandleA("d3d12core.dll"))
        m_heapView = reinterpret_cast<PFN_remix_vkd3d_heap_view>(GetProcAddress(core, REMIX_VKD3D_HEAP_VIEW_ENTRY_POINT));
    }

    if (!m_describeView) {
      if (HMODULE layer = GetModuleHandleA("remix_vk_layer.dll"))
        m_describeView = reinterpret_cast<PFN_remix_vklayer_describe_view>(GetProcAddress(layer, REMIX_VKLAYER_DESCRIBE_VIEW_ENTRY_POINT));
    }

    auto fail = [&](const char* why) {
      if (m_materialWalkLogs < 8) {
        m_materialWalkLogs++;
        Logger::warn(str::format("[Remix-VkFrontend][starfield] material ", materialIndex, ": ", why));
      }
    };

    if (!common || !layers) { fail("material tables neither CPU-readable nor read back"); return; }
    if (!heap || !heapCount) { fail("no descriptor heap"); return; }
    if (!m_heapView || !m_describeView) { fail("vkd3d / layer exports missing"); return; }

    if ((size_t(materialIndex) + 1) * kCommonStride > commonSize) { fail("index past the copied table"); return; }

    const uint8_t* entry = common + size_t(materialIndex) * kCommonStride;
    uint32_t layer0 = 0;
    std::memcpy(&layer0, entry + 116, sizeof(layer0));

    if ((size_t(layer0) + 1) * kLayerStride > layersSize) { fail("layer index past the copied table"); return; }

    uint32_t indices[4] = {};
    std::memcpy(indices, layers + size_t(layer0) * kLayerStride, sizeof(indices));

    auto resolve = [&](uint32_t index) -> int32_t {
      if (!index)
        return -1;

      VkImageView view = VK_NULL_HANDLE;
      VkImageLayout imageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
      remix_vkfe_binding binding = {};

      if (!m_heapView(heap, heapCount, index, &view, &imageLayout)
       || !m_describeView(m_device->handle(), view, imageLayout, &binding))
        return -1;

      binding.kind       = REMIX_VKFE_BINDING_TEXTURE;
      binding.stage_mask = 1u << REMIX_VKFE_STAGE_PIXEL;
      // Outside every D3D12 register space: these come from the heap.
      binding.space      = 0xfffffffeu;
      binding.slot       = index;
      draw.bindings.push_back(binding);
      draw.bindingBytes.push_back(nullptr);
      return int32_t(draw.bindings.size() - 1);
    };

    auto& mat = draw.explicitMaterial;
    mat.albedo    = resolve(indices[0]);
    mat.normal    = resolve(indices[1]);
    mat.metallic  = resolve(indices[2]);
    mat.roughness = resolve(indices[3]);

    if (mat.normal >= 0) {
      switch (draw.bindings[size_t(mat.normal)].format) {
        case VK_FORMAT_BC5_SNORM_BLOCK: case VK_FORMAT_R8G8_SNORM:
          mat.normalEncoding = 6; break;
        case VK_FORMAT_BC5_UNORM_BLOCK: case VK_FORMAT_R8G8_UNORM:
          mat.normalEncoding = 3; break;
        default:
          mat.normalEncoding = 2; break;
      }
    }

    if (mat.albedo < 0)
      fail("colour texture not resolved");
  }


  void D3D11VkFrontendDevice::captureFrame(std::vector<D3D11VkDraw>& draws) {
    ScopedCpuProfileZone();

    auto& st = m_capture;

    st.statDraws = uint32_t(draws.size());
    st.statCommitted = st.statNoCapture = st.statIndirect = st.statNotScene = st.statNoCamera = 0;
    st.statDepthOnly = st.statDuplicate = st.statIgnored = st.statDecals = st.statParticles = 0;
    st.statSky = st.statWater = st.statTerrain = st.statLifted = st.statLights = st.statNormalMaps = 0;
    st.readbacksThisFrame = 0;
    st.lightKeys.clear();

    if (st.sampler == nullptr) {
      DxvkSamplerCreateInfo info = {};
      info.magFilter      = VK_FILTER_LINEAR;
      info.minFilter      = VK_FILTER_LINEAR;
      info.mipmapMode     = VK_SAMPLER_MIPMAP_MODE_LINEAR;
      info.mipmapLodBias  = 0.0f;
      info.mipmapLodMin   = 0.0f;
      info.mipmapLodMax   = 16.0f;
      info.useAnisotropy  = VK_TRUE;
      info.maxAnisotropy  = 16.0f;
      info.addressModeU   = VK_SAMPLER_ADDRESS_MODE_REPEAT;
      info.addressModeV   = VK_SAMPLER_ADDRESS_MODE_REPEAT;
      info.addressModeW   = VK_SAMPLER_ADDRESS_MODE_REPEAT;
      info.compareToDepth = VK_FALSE;
      info.compareOp      = VK_COMPARE_OP_NEVER;
      info.borderColor    = VkClearColorValue();
      info.usePixelCoord  = VK_FALSE;
      st.sampler = m_device->createSampler(info);
    }

    processTextureHashes();

    // 1. The scene pass: the depth buffer with the largest area, then the
    // most draws. Shadow maps, reflections and post passes use other (or
    // no) depth targets. Shadow cascades feed the sun vote.
    struct DepthUse { uint64_t area = 0; uint32_t draws = 0; VkExtent3D extent = {}; };
    std::unordered_map<VkImage, DepthUse> depthUse;

    for (const auto& draw : draws) {
      const auto& depth = draw.desc.depth;

      if (!depth.image)
        continue;

      auto& use = depthUse[depth.image];
      use.area = uint64_t(depth.extent.width) * depth.extent.height;
      use.extent = depth.extent;
      use.draws++;
    }

    VkImage sceneDepth = VK_NULL_HANDLE;
    DepthUse sceneUse;

    for (const auto& use : depthUse) {
      if (use.second.area > sceneUse.area
       || (use.second.area == sceneUse.area && use.second.draws > sceneUse.draws)) {
        sceneDepth = use.first;
        sceneUse = use.second;
      }
    }

    std::vector<const D3D11VkDraw*> sceneDraws;
    std::vector<const D3D11VkDraw*> swapchainDraws;

    for (const auto& draw : draws) {
      if (sceneDepth && draw.desc.depth.image == sceneDepth) {
        sceneDraws.push_back(&draw);
      } else {
        st.statNotScene++;

        if (draw.desc.depth.image && depthOnly(draw))
          learnSun(draw);

        if (draw.desc.render_target_count && draw.desc.render_targets[0].is_swapchain_image)
          swapchainDraws.push_back(&draw);
      }
    }

    // 2. Camera.
    FrameCamera camera = sceneDraws.empty()
      ? FrameCamera()
      : findCamera(sceneDraws, sceneUse.extent);

    if (camera.valid && !camera.clipUsesWDepth) {
      st.seenPerspective = true;
      st.framesWithoutPerspective = 0;
    } else {
      st.framesWithoutPerspective++;
    }

    st.haveProjection = camera.valid && !camera.clipUsesWDepth;
    st.haveView = camera.haveView;

    if (camera.valid) {
      st.projection = camera.projection;
      st.jitteredProjection = camera.jitteredProjection;
      st.view = camera.view;
    }

    // 2D games (D3D11Rtx 2D lift): a process that has never drawn a
    // perspective scene, after some frames (GameMaker / 2D frameworks at
    // once), traces its back-buffer draws as emissive layers.
    const D3D11EngineFamily family = GetD3D11EngineProfile().family();
    const bool twoDFramework = family == D3D11EngineFamily::GameMaker || family == D3D11EngineFamily::Framework2D;
    const bool lift2D = RtxOptions::dx11Lift2DLayers() && !st.seenPerspective
      && (twoDFramework || st.framesWithoutPerspective >= std::max(RtxOptions::dx11Lift2DMinFrames(), 1u));
    D3D11Rtx::SetLift2DPresentation(lift2D, 1u);

    // 3. Lights and the sun.
    importLights(sceneDraws, camera);
    applySun(camera);

    // 4. Geometry, one draw per mesh: depth-only draws are prepasses or
    // shadows; of the colour passes of one mesh, the depth-EQUAL material
    // pass of a light-prepass engine wins, else the first, and additive
    // repeats (ForwardAdd) are dropped.
    std::unordered_set<VkImage> earlierTargets;
    std::unordered_map<uint64_t, const D3D11VkDraw*> chosen;

    if (!lift2D) {
      for (const D3D11VkDraw* draw : sceneDraws) {
        if (depthOnly(*draw) || !draw->pipeline)
          continue;

        const uint64_t key = meshKey(*draw);
        auto it = chosen.find(key);

        if (it == chosen.end()) {
          chosen[key] = draw;
          continue;
        }

        const bool equalPass = draw->pipeline->state.depth_compare == VK_COMPARE_OP_EQUAL;
        const bool keptEqual = it->second->pipeline->state.depth_compare == VK_COMPARE_OP_EQUAL;

        if (equalPass && !keptEqual)
          it->second = draw;
      }
    }

    // DX12 back buffers are vkd3d-proton's own images, copied to the swap
    // chain at present, so no draw targets the swap chain: the 2D lift then
    // takes the colour target with the largest area and most draws.
    if (lift2D && swapchainDraws.empty()) {
      struct TargetUse { uint64_t area = 0; uint32_t draws = 0; };
      std::unordered_map<VkImage, TargetUse> targets;
      VkImage best = VK_NULL_HANDLE;
      TargetUse bestUse;

      for (const auto& draw : draws) {
        if (!draw.desc.render_target_count)
          continue;

        auto& use = targets[draw.desc.render_targets[0].image];
        use.area = uint64_t(draw.desc.render_targets[0].extent.width) * draw.desc.render_targets[0].extent.height;
        use.draws++;
      }

      for (const auto& t : targets) {
        if (t.second.area > bestUse.area || (t.second.area == bestUse.area && t.second.draws > bestUse.draws)) {
          best = t.first;
          bestUse = t.second;
        }
      }

      for (const auto& draw : draws) {
        if (draw.desc.render_target_count && draw.desc.render_targets[0].image == best)
          swapchainDraws.push_back(&draw);
      }
    }

    // Lifted 2D layers are placed back to front by draw order.
    const std::vector<const D3D11VkDraw*>& layered = lift2D ? swapchainDraws : sceneDraws;
    const float liftNear = 10.0f, liftFar = 100.0f;
    uint32_t layerIndex = 0;

    if (lift2D) {
      // Synthetic camera the layers are placed for (D3D11Rtx::Lift2DProjection).
      VkExtent3D extent = swapchainDraws.empty() ? VkExtent3D{ 16u, 9u, 1u } : swapchainDraws.front()->desc.render_targets[0].extent;
      camera = FrameCamera();
      camera.valid = true;
      camera.projection = perspectiveFromViewport(extent.height ? float(extent.width) / float(extent.height) : 16.0f / 9.0f, 60.0f);
      camera.jitteredProjection = camera.projection;
      selectReplacementProfile();
    }

    for (const D3D11VkDraw* draw : layered) {
      // Images this draw renders to become "earlier targets" for later draws
      // (scene copies sampled by refraction). Recorded before the skip
      // checks, since skipped passes still produce them.
      struct Mark {
        std::unordered_set<VkImage>& set;
        const D3D11VkDraw* d;
        ~Mark() {
          for (uint32_t r = 0; r < d->desc.render_target_count; r++)
            set.insert(d->desc.render_targets[r].image);
        }
      } mark { earlierTargets, draw };

      if (!draw->pipeline || draw->captureBuffer == nullptr) {
        st.statNoCapture++;
        continue;
      }

      if (!lift2D) {
        if (depthOnly(*draw)) {
          st.statDepthOnly++;
          continue;
        }

        auto it = chosen.find(meshKey(*draw));

        if (it == chosen.end() || it->second != draw) {
          st.statDuplicate++;
          continue;
        }
      }

      DrawContext context;
      context.earlierTargets = &earlierTargets;
      context.lift2D = lift2D;
      context.liftDepth = lift2D
        ? liftFar - (liftFar - liftNear) * float(layerIndex++) / float(std::max<size_t>(layered.size(), 1))
        : 0.0f;

      if (draw->counterSlot != ~0u) {
        st.statIndirect++;

        D3D11VkPendingIndirect pending;
        pending.draw               = *draw;
        pending.frame              = m_presentCount;
        pending.haveCamera         = camera.valid;
        pending.clipUsesWDepth     = camera.clipUsesWDepth;
        pending.projection         = camera.projection;
        pending.jitteredProjection = camera.jitteredProjection;
        pending.view               = camera.view;
        pending.haveView           = camera.haveView;

        // Keep the capture memory until the count has been read.
        if (draw->captureChunk)
          draw->captureChunk->users++;

        m_pendingIndirect.push_back(std::move(pending));
        continue;
      }

      if (camera.valid)
        commitDraw(*draw, draw->capturedVertices, camera, context);
    }

    processPendingIndirect();

    // 5. Drop texture wrappers the game stopped using (the VkImage may be
    // destroyed and its handle reused).
    constexpr uint64_t kTextureIdleFrames = 300;

    for (auto it = st.textures.begin(); it != st.textures.end(); ) {
      if (it->second.lastFrame + kTextureIdleFrames < m_presentCount && it->second.readback == nullptr)
        it = st.textures.erase(it);
      else
        ++it;
    }

    if (m_presentCount < 8 || (m_presentCount % 300) == 0) {
      Logger::info(str::format("[Remix-VkFrontend][capture] frame=", m_presentCount,
        " draws=", st.statDraws,
        " committed=", st.statCommitted,
        " depthOnly=", st.statDepthOnly,
        " duplicates=", st.statDuplicate,
        " indirectQueued=", st.statIndirect,
        " pendingIndirect=", m_pendingIndirect.size(),
        " noCapture=", st.statNoCapture,
        " offScene=", st.statNotScene,
        " ignored=", st.statIgnored,
        " decals=", st.statDecals,
        " particles=", st.statParticles,
        " sky=", st.statSky,
        " water=", st.statWater,
        " terrain=", st.statTerrain,
        " lifted=", st.statLifted,
        " normalMaps=", st.statNormalMaps,
        " lights=", st.statLights,
        " sun=", st.sunValid ? 1 : 0,
        " camera=", camera.valid ? (lift2D ? "2d-lift" : (camera.clipUsesWDepth ? "fallback" : (camera.haveView ? "world" : "view-space"))) : "none",
        " textures=", st.textures.size()));
    }
  }

}
