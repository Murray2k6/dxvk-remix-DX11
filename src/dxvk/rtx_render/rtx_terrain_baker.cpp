#include "../../dxbc/dxbc_util.h"
#include "rtx/dx11/dx11_material_fog_state.h"
/*
* Copyright (c) 2023-2024, NVIDIA CORPORATION. All rights reserved.
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

#include "rtx_terrain_baker.h"
// DX11_V225: complete type for the fixed-function-equivalent VS constant block.
#include "../../d3d11/d3d11_fixed_function.h"

#include "dxvk_device.h"
#include "../tracy/Tracy.hpp"
#include "dxvk_scoped_annotation.h"
#include "rtx_imgui.h"
#include "rtx_option.h"
#include "rtx_texture.h"
#include "rtx_texture_manager.h"

#include "../d3d11/d3d11_state.h"
#include "../d3d11/d3d11_spec_constants.h"
#include "../../d3d11/d3d11_rtx.h"
#include "../../d3d11/d3d11_resource_slot.h"
#include "../../d3d11/d3d11_caps.h"

namespace {
  // By default, a value of 1.f will have 0 displacement.
  const float kDefaultNeutralHeight = 1.f;
}

namespace dxvk {

  uint32_t getMipLevels(ReplacementMaterialTextureType::Enum textureType, const VkExtent3D& extent) {
    switch (textureType) {
    case ReplacementMaterialTextureType::Height:
      // height maps need to be created with mip levels for quad tree POM support.
      return static_cast<uint32_t>(std::floor(std::log2(std::max(extent.width, extent.height))));
      break;

    default:
      return 1;
      break;
    }
  }

  VkFormat getTextureFormat(ReplacementMaterialTextureType::Enum textureType) {
    switch (textureType) {
    case ReplacementMaterialTextureType::Normal:
    case ReplacementMaterialTextureType::Tangent:
      return VK_FORMAT_R8G8B8A8_SNORM;
      break;

    case ReplacementMaterialTextureType::AlbedoOpacity:
    case ReplacementMaterialTextureType::Emissive:
      return VK_FORMAT_R8G8B8A8_UNORM;
      break;

      // R16
    case ReplacementMaterialTextureType::Height:
    case ReplacementMaterialTextureType::Roughness:
    case ReplacementMaterialTextureType::Metallic:
      return VK_FORMAT_R8_UNORM;
      break;

    default:
      assert(0);
      return VK_FORMAT_UNDEFINED;
      break;
    }
  }

  VkClearColorValue TerrainBaker::getClearColor(ReplacementMaterialTextureType::Enum textureType) {
    const float prevFrameTotalHeight = m_prevFrameMaxDisplaceIn + m_prevFrameMaxDisplaceOut;
    float neutral_height = prevFrameTotalHeight != 0.f ? m_prevFrameMaxDisplaceIn / prevFrameTotalHeight : kDefaultNeutralHeight;
    switch (textureType) {
    case ReplacementMaterialTextureType::Height:
      // height maps should be cleared to neutral_height, which keeps the displaced surface identical to the original surface.
      // The height texture should be single channel, so only the first value actually matters.
      return { neutral_height, neutral_height, neutral_height, neutral_height };
      break;

    default:
      return { 0.0f, 0.0f, 0.0f, 0.0f };
      break;
    }
  }

  // The two gates below came from the legacy shader-object path, where the baker could
  // only drive replacement PBR textures through a shader-model-1 pixel shader. The DX11
  // capture layer never populated the field they read, so both have always resolved to
  // the "shader model <= 1" branch. Keep that behavior explicit rather than switching to
  // the real DXBC shader model (always 4.0+ in D3D11), which would silently disable PS
  // replacement support and start erroring on every terrain draw. Revisit alongside
  // REMIX-2223 if the baker gains real SM4+ handling.
  static constexpr uint32_t kLegacyProgrammablePsMajorVersion = 0;

  bool TerrainBaker::isPSReplacementSupportEnabled(const DrawCallState& drawCallState) {
    if (drawCallState.usesPixelShader) {
      return Material::replacementSupportInPS() &&
             Material::replacementSupportInPS_programmableShaders() &&
             kLegacyProgrammablePsMajorVersion <= 1;
    } else {
      return Material::replacementSupportInPS() && Material::replacementSupportInPS_fixedFunction();
    }
  }

  // Gathers available textures from a replacement material and 
  // runs a compute shader to convert them into a compatible format for baking
  bool TerrainBaker::gatherAndPreprocessReplacementTextures(Rc<RtxContext> ctx,
                                                            const DrawCallState& drawCallState,
                                                            OpaqueMaterialData* replacementMaterial,
                                                            std::vector<RtxGeometryUtils::TextureConversionInfo>& replacementTextures) {
    if (!replacementMaterial) {
      return false;
    }

    SceneManager& sceneManager = ctx->getSceneManager();
    Resources& resourceManager = ctx->getResourceManager();
    const bool hasTexcoords = drawCallState.hasTextureCoordinates();
    // We're going to use this to create a modified sampler for textures.
    DxvkSampler* pOriginalSampler = drawCallState.getMaterialData().getSampler().ptr();
    Rc<DxvkContext> dxvkCtx = ctx;

    // Opacity texture is currently required for blending to work. 
    // Scenarios where blending does not require a colorOpacity texture or 
    // replacement material is using a colorOpacity constant are not currently supported
    if (!replacementMaterial->getAlbedoOpacityTexture().isValid()) {
      ONCE(Logger::warn(str::format("[RTX Texture Baker] Replacement material for ", drawCallState.getMaterialData().getHash(), " does not have a color opacity texture.",
                                    " This scenario is not currently supported by the texture baker. Ignoring the replacement material.")));
      return false;
    }

    if (!drawCallState.getMaterialData().getColorTexture2().isValid()) {
      ONCE(Logger::warn(str::format("[RTX Texture Baker] Legacy material for ", drawCallState.getMaterialData().getHash(), " has a second color texture.",
                                    "Only single texture legacy materials are supported. Ignoring the second color texture.")));
    }

    // Ensures a texture stays in VidMem
    auto trackAndFinalizeTexture = [&](TextureRef& texture) {
      uint32_t unusedTextureIndex;
      sceneManager.trackTexture(texture, unusedTextureIndex, hasTexcoords);
    };

    // Track the source albedo opacity texture to keep it in VidMem as it's needed for baking
    trackAndFinalizeTexture(replacementMaterial->getAlbedoOpacityTexture());

    const DxvkImageCreateInfo& aoImageInfo = replacementMaterial->getAlbedoOpacityTexture().getImageView()->imageInfo();

    // Returns a scaled down the extent that fits within the max resolution constraint preserving the aspect ratio (barring float to integer conversion errors)
    auto calculateScaledResolution2D = [&](VkExtent3D extent, const uint32_t maxResolutionPerDimension) {
      const float scalingFactor = 
        std::min(
          1.f,     // Don't scale up the input dimensions
          1 / std::max(
            extent.width / static_cast<float>(maxResolutionPerDimension),
            extent.height / static_cast<float>(maxResolutionPerDimension)));

      extent.width = static_cast<uint32_t>(extent.width * scalingFactor);
      extent.height = static_cast<uint32_t>(extent.height * scalingFactor);

      return extent;
    };

    auto addValidTexture = [&](TextureRef& texture, ReplacementMaterialTextureType::Enum textureType) {

      if (!texture.isValid()) {
        return;
      }

      // Track the source material texture to keep it in VidMem while it's being used for baking.
      // This needs to be done prior to checking for having valid views 
      // since the views are not created until the texture is promoted
      trackAndFinalizeTexture(texture);

      if (!texture.getImageView()) {
        return;
      }

      RtxGeometryUtils::TextureConversionInfo& conversionInfo = replacementTextures.emplace_back();
      conversionInfo.type = textureType;
      conversionInfo.sourceTexture = &texture;
      
      if (textureType == ReplacementMaterialTextureType::Height) {
        // Normalize the displaceIn and displaceOut to the previous frame's displacement range.
        const float prevFrameTotalHeight = m_prevFrameMaxDisplaceIn + m_prevFrameMaxDisplaceOut;
        const float materialTotalHeight = replacementMaterial->getDisplaceIn() + replacementMaterial->getDisplaceOut();

        conversionInfo.scale = prevFrameTotalHeight <= 0.f ? 0.f : materialTotalHeight / prevFrameTotalHeight;
        // We want to subtract the original neutral displacement, then scale the values, then add the new neutral displacement.
        conversionInfo.offset = -1.f * (materialTotalHeight == 0.f ? kDefaultNeutralHeight : (replacementMaterial->getDisplaceIn() / materialTotalHeight));
        m_currFrameMaxDisplaceIn = std::max(m_currFrameMaxDisplaceIn, replacementMaterial->getDisplaceIn());
        m_currFrameMaxDisplaceOut = std::max(m_currFrameMaxDisplaceOut, replacementMaterial->getDisplaceOut());
      }

      if (isPSReplacementSupportEnabled(drawCallState)) {
        conversionInfo.targetTexture = TextureRef(texture.getImageView());
      } else {
        const DxvkImageCreateInfo& imageInfo = texture.getImageView()->imageInfo();
        const VkExtent3D& extent = imageInfo.extent;

        const VkExtent3D adjustedExtent = calculateScaledResolution2D(extent, Material::maxResolutionToUseForReplacementMaterials());

        TextureKey textureKey;
        textureKey.width = adjustedExtent.width;
        textureKey.height = adjustedExtent.height;
        textureKey.textureType = textureType;
        XXH64_hash_t textureKeyHash = textureKey.calculateHash();

        auto textureIter = m_stagingTextureCache.find(textureKeyHash);

        // Staging texture must be 4 channel as the 4th channel will contain opacity
        VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;

        if (textureType == ReplacementMaterialTextureType::Normal ||
            textureType == ReplacementMaterialTextureType::Tangent) {
          format = VK_FORMAT_R8G8B8A8_SNORM;
        }

        // No matching cached texture found, create a new one
        if (textureIter == m_stagingTextureCache.end()) {
          textureIter =
            m_stagingTextureCache.emplace(
              textureKeyHash,
              Resources::createImageResource(dxvkCtx, "terrain baking: staging replacement texture", adjustedExtent,
                                             format, 1, VK_IMAGE_TYPE_2D, VK_IMAGE_VIEW_TYPE_2D, 0)).first;
        }

        conversionInfo.targetTexture = TextureRef(textureIter->second.view);

        // Track lifetime of the resource now since targetTexture object is about to get destroyed
        ctx->getCommandList()->trackResource<DxvkAccess::Write>(textureIter->second.image);
      }
    };

    // Gather all replacement textures that need to be preprocessed
    replacementTextures.reserve(ReplacementMaterialTextureType::Count);

    if (Material::bakeSecondaryPBRTextures()) {
      addValidTexture(replacementMaterial->getNormalTexture(), ReplacementMaterialTextureType::Normal);
      addValidTexture(replacementMaterial->getTangentTexture(), ReplacementMaterialTextureType::Tangent);
      addValidTexture(replacementMaterial->getHeightTexture(), ReplacementMaterialTextureType::Height);
      addValidTexture(replacementMaterial->getRoughnessTexture(), ReplacementMaterialTextureType::Roughness);
      addValidTexture(replacementMaterial->getMetallicTexture(), ReplacementMaterialTextureType::Metallic);
      addValidTexture(replacementMaterial->getEmissiveColorTexture(), ReplacementMaterialTextureType::Emissive);


      if (!isPSReplacementSupportEnabled(drawCallState)) {
        // Pre-process textures to be compatible with baking
        ctx->getCommonObjects()->metaGeometryUtils().decodeAndAddOpacity(ctx, replacementMaterial->getAlbedoOpacityTexture(), replacementTextures);
      }
    }

    // Add the remaining albedo opacity which does not needed to be preprocessed to the texture list for baking.
    RtxGeometryUtils::TextureConversionInfo& conversionInfo = replacementTextures.emplace_back();
    conversionInfo.type = ReplacementMaterialTextureType::AlbedoOpacity;
    conversionInfo.sourceTexture = nullptr;
    conversionInfo.targetTexture = replacementMaterial->getAlbedoOpacityTexture();

    // Move albedo opacity to the front of the baking queue as the baking aborts if baking of albedo opacity texture fails
    if (replacementTextures.size() > 1) {
      std::swap(replacementTextures.front(), replacementTextures.back());
    }

    return true;
  }

  bool TerrainBaker::bakeDrawCall(Rc<RtxContext> ctx,
                                  const DxvkContextState& dxvkCtxState,
                                  DxvkRaytracingInstanceState& rtState,
                                  const DrawParameters& drawParams,
                                  const DrawCallState& drawCallState,
                                  OpaqueMaterialData* replacementMaterial,
                                  Matrix4& textureTransformOut) {

    ScopedGpuProfileZone(ctx, "Terrain Baker: Bake Draw Call");

    // DX11 bake (documentation/engine_knowledge/METHODS.md, Terrain layer
    // blending): the game's own draw - its VS, PS and every bound resource,
    // so its layer blending - is replayed into the terrain cascades. The DXBC
    // bake hook (DxbcCompiler::emitBakeTransform) moves SV_Position from the
    // game camera into each cascade's top-down camera.
    (void) rtState;
    (void) drawParams;

    // Baked by the DX12 / Vulkan front end in the game's command buffer.
    if (drawCallState.externalTerrainBake != nullptr)
      return bakeExternal(ctx, dxvkCtxState, drawCallState, textureTransformOut);

    const DrawCallState::GameDraw& game = drawCallState.gameDraw;
    const RasterGeometry& geo = drawCallState.getGeometryData();
    if (!game.valid || game.count == 0) {
      ONCE(Logger::warn("[RTX Terrain Baker] Indirect terrain draws cannot be replayed for baking; rendering them as regular path-traced geometry."));
      return false;
    }
    if (geo.postVsClipUsesWDepth) {
      // Viewport-fallback capture rebuilds positions from clip.w, which is
      // not a linear map of clip space; the bake matrix cannot express it.
      ONCE(Logger::warn("[RTX Terrain Baker] Terrain draw captured with a viewport-fallback camera; it cannot be baked."));
      return false;
    }
    if (replacementMaterial != nullptr)
      ONCE(Logger::info("[RTX Terrain Baker] Replacement terrain materials are not baked on DX11; the game's own blended albedo is baked."));

    // Register mesh and preprocess state for baking for this frame
    registerTerrainMesh(ctx, dxvkCtxState, drawCallState);

    if (!debugDisableBinding()) {
      textureTransformOut = m_bakingParams.viewToCascade0TextureSpace;
    }

    if (debugDisableBaking()) {
      const bool isBaked =
        (debugDisableBinding() ? false : true) &&
        getTerrainTexture(ReplacementMaterialTextureType::AlbedoOpacity).view != nullptr;
      if (isBaked)
        updateMaterialData(ctx);
      return isBaked;
    }

    // Clip space of this draw -> the scene space its RT instance lives in.
    // Captured draws carry the exact clip-to-position map; draws placed with
    // the game's own world matrix use the game camera's inverse.
    const DrawCallTransforms& transforms = drawCallState.getTransformData();
    const Matrix4 clipToWorld = geo.postVsPositionIsHomogeneousClip
      ? transforms.objectToWorld * geo.postVsClipToPosition
      : inverse(transforms.viewToProjection * transforms.worldToView);
    for (uint32_t c = 0; c < 4; ++c)
      for (uint32_t r = 0; r < 4; ++r)
        if (!std::isfinite(clipToWorld[c][r]))
          return false;

    RtxTextureManager& textureManger = ctx->getCommonObjects()->getTextureManager();
    const RtxMipmap::Resource& terrainResource = getTerrainTexture(ctx, textureManger, ReplacementMaterialTextureType::AlbedoOpacity,
      m_bakingParams.cascadeMapResolution.width, m_bakingParams.cascadeMapResolution.height);
    const Rc<DxvkImageView>& terrainTextureView = terrainResource.views.empty() ? terrainResource.view : terrainResource.views[0];
    if (terrainTextureView == nullptr) {
      ONCE(Logger::err("[RTX Terrain Baker] Failed to retrieve the albedo terrain texture; skipping baking."));
      return false;
    }

    // The game PS's albedo output. A deferred G-buffer pass writes several
    // targets; albedo is the first four-channel 8-bit colour target. Other
    // outputs go to unbound attachments and are dropped.
    uint32_t albedoIndex = UINT32_MAX;
    for (uint32_t i = 0; i < MaxNumRenderTargets; ++i) {
      const Rc<DxvkImageView>& view = dxvkCtxState.om.renderTargets.color[i].view;
      if (view == nullptr)
        continue;
      const VkFormat fmt = view->info().format;
      if (fmt == VK_FORMAT_R8G8B8A8_UNORM || fmt == VK_FORMAT_R8G8B8A8_SRGB
       || fmt == VK_FORMAT_B8G8R8A8_UNORM || fmt == VK_FORMAT_B8G8R8A8_SRGB) {
        albedoIndex = i;
        break;
      }
    }
    // A forward pass writes lit colour into an HDR target: baking it would put
    // the game's lighting into the albedo the path tracer lights again.
    if (albedoIndex == UINT32_MAX) {
      ONCE(Logger::info("[RTX Terrain Baker] Terrain pass has no 8-bit albedo target (forward/HDR output); not baking it."));
      return false;
    }

    // Re-bake only when the cascades or the draw changed (METHODS.md,
    // Terrain: "Re-bake only when cascades scroll"): a draw baked last frame
    // with the same camera, geometry and material, under the same cascade
    // layout, is still in the cascade map. Every kRebakeRefreshFrames frames
    // it is baked again anyway, so textures streamed in at a higher mip
    // reach the bake.
    {
      constexpr uint32_t kRebakeRefreshFrames = 30;
      const uint32_t frameIndex = ctx->getDevice()->getCurrentFrameId();
      const XXH64_hash_t geometryHash = geo.hashes[HashComponents::VertexPosition];
      const XXH64_hash_t materialHash = drawCallState.getMaterialData().getHash();
      XXH64_hash_t drawKey = XXH3_64bits(&clipToWorld, sizeof(clipToWorld));
      drawKey = XXH3_64bits_withSeed(&geometryHash, sizeof(geometryHash), drawKey);
      drawKey = XXH3_64bits_withSeed(&materialHash, sizeof(materialHash), drawKey);
      m_bakedDrawsThisFrame.insert(drawKey);

      const bool unchanged = m_bakingParamsUnchanged && !clearTerrainBeforeBaking()
        && (frameIndex % kRebakeRefreshFrames) != 0
        && m_bakedDrawsLastFrame.count(drawKey) != 0
        && getTerrainTexture(ReplacementMaterialTextureType::AlbedoOpacity).view != nullptr;

      if (unchanged) {
        ++m_bakeSkippedThisFrame;
        m_materialTextures[ReplacementMaterialTextureType::AlbedoOpacity].markAsBaked();
        updateMaterialData(ctx);
        return true;
      }
    }

    if (m_bakeTransformBuffer == nullptr) {
      DxvkBufferCreateInfo info;
      info.size = sizeof(Vector4) * 4;
      info.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
      info.stages = VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_TESSELLATION_EVALUATION_SHADER_BIT;
      info.access = VK_ACCESS_UNIFORM_READ_BIT;
      m_bakeTransformBuffer = ctx->getDevice()->createBuffer(info,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        DxvkMemoryStats::Category::RTXBuffer, "Terrain baker DX11 bake transform");
    }
    const uint32_t vsSlot = computeConstantBufferBinding(DxbcProgramType::VertexShader, 15);
    const uint32_t dsSlot = computeConstantBufferBinding(DxbcProgramType::DomainShader, 15);

    // Save state
    const uint32_t prevViewportCount = dxvkCtxState.gp.state.rs.viewportCount();
    const DxvkViewportState prevViewportState = dxvkCtxState.vp;
    const DxvkRenderTargets prevRenderTargets = dxvkCtxState.om.renderTargets;
    const DxvkScInfo prevSpecConstantsInfo = ctx->getSpecConstantsInfo(VK_PIPELINE_BIND_POINT_GRAPHICS);
    const DxvkRsInfo& ri = dxvkCtxState.gp.state.rs;
    DxvkRasterizerState prevRasterizerState;
    prevRasterizerState.depthClipEnable = ri.depthClipEnable();
    prevRasterizerState.depthBiasEnable = ri.depthBiasEnable();
    prevRasterizerState.polygonMode = ri.polygonMode();
    prevRasterizerState.cullMode = ri.cullMode();
    prevRasterizerState.frontFace = ri.frontFace();
    prevRasterizerState.sampleCount = ri.sampleCount();
    prevRasterizerState.conservativeMode = ri.conservativeMode();

    // The top-down camera and flipped viewport can reverse winding.
    DxvkRasterizerState bakeRs = prevRasterizerState;
    bakeRs.cullMode = VK_CULL_MODE_NONE;
    ctx->setRasterizerState(bakeRs);
    ctx->setSpecConstant(VK_PIPELINE_BIND_POINT_GRAPHICS, D3D11SpecConstantId::CustomVertexTransformEnabled, true);

    DxvkRenderTargets terrainRt;
    terrainRt.color[albedoIndex].view = terrainTextureView;
    terrainRt.color[albedoIndex].layout = VK_IMAGE_LAYOUT_GENERAL;
    ctx->bindRenderTargets(terrainRt);
    m_materialTextures[ReplacementMaterialTextureType::AlbedoOpacity].markAsBaked();

    ctx->bindResourceBuffer(vsSlot, DxvkBufferSlice(m_bakeTransformBuffer));
    ctx->bindResourceBuffer(dsSlot, DxvkBufferSlice(m_bakeTransformBuffer));

    // Screen-space inputs (METHODS.md, Terrain: "Neutralise screen-space
    // inputs (shadow mask, AO, fog)"): an image the frame rendered earlier,
    // sampled at screen positions, means nothing in the top-down bake. White
    // stands for lit and unoccluded. Only 2D colour targets: authored
    // textures, arrays and depth keep their bindings.
    std::vector<std::pair<uint32_t, Rc<DxvkImageView>>> neutralised;
    {
      const Rc<DxvkImageView> white = ctx->getResourceManager().getWhiteTexture(ctx);
      for (uint32_t i = 0; white != nullptr && i < D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT; ++i) {
        const uint32_t slot = computeSrvBinding(DxbcProgramType::PixelShader, i);
        const Rc<DxvkImageView> view = ctx->getShaderResourceSlot(slot).imageView;
        if (view == nullptr || view->info().type != VK_IMAGE_VIEW_TYPE_2D
         || (view->imageInfo().usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) == 0)
          continue;
        // Integer targets (material IDs) are read with integer views.
        const DxvkFormatInfo* formatInfo = imageFormatInfo(view->info().format);
        if (formatInfo == nullptr || formatInfo->flags.any(DxvkFormatFlag::SampledUInt, DxvkFormatFlag::SampledSInt))
          continue;
        neutralised.emplace_back(slot, view);
        ctx->bindResourceView(slot, white, nullptr);
      }
    }

    const float2 levelResolution = float2 {
      static_cast<float>(m_bakingParams.cascadeLevelResolution.width),
      static_cast<float>(m_bakingParams.cascadeLevelResolution.height)
    };

    // Render into all cascade levels, tiled left to right, top to bottom.
    for (uint32_t iCascade = 0; iCascade < m_bakingParams.numCascades; iCascade++) {
      Vector2i cascade2DIndex;
      cascade2DIndex.y = iCascade / m_bakingParams.cascadeMapSize.x;
      cascade2DIndex.x = iCascade - cascade2DIndex.y * m_bakingParams.cascadeMapSize.x;

      // Clip space <-1, 1> to screen space <0, resolution>, Vulkan's y flipped.
      VkViewport viewport {
        cascade2DIndex.x * levelResolution.x,
        (cascade2DIndex.y + 1) * levelResolution.y,
        levelResolution.x,
        -levelResolution.y,
        0.f, 1.f
      };
      const VkOffset2D cascadeOffset = VkOffset2D {
        static_cast<int>(cascade2DIndex.x * m_bakingParams.cascadeLevelResolution.width),
        static_cast<int>(cascade2DIndex.y * m_bakingParams.cascadeLevelResolution.height) };
      const VkRect2D scissor = { cascadeOffset, m_bakingParams.cascadeLevelResolution };
      ctx->setViewports(1, &viewport, &scissor);

      // Game clip -> scene -> cascade camera, uploaded as four rows.
      const Matrix4 bake = m_bakingParams.bakingCameraOrthoProjection[iCascade] * m_bakingParams.sceneView * clipToWorld;
      DxvkBufferSliceHandle slice = m_bakeTransformBuffer->allocSlice();
      ctx->invalidateBuffer(m_bakeTransformBuffer, slice);
      Vector4* rows = static_cast<Vector4*>(slice.mapPtr);
      for (uint32_t r = 0; r < 4; ++r)
        rows[r] = Vector4(bake[0][r], bake[1][r], bake[2][r], bake[3][r]);

      if (game.indexed) {
        ctx->DxvkContext::drawIndexed(game.count, game.instanceCount, game.start, game.base, game.firstInstance);
      } else {
        ctx->DxvkContext::draw(game.count, game.instanceCount, game.start, game.firstInstance);
      }
    }

    // Restore state
    for (const auto& n : neutralised)
      ctx->bindResourceView(n.first, n.second, nullptr);
    ctx->bindResourceBuffer(vsSlot, DxvkBufferSlice());
    ctx->bindResourceBuffer(dsSlot, DxvkBufferSlice());
    ctx->setViewports(prevViewportCount, prevViewportState.viewports.data(), prevViewportState.scissorRects.data());
    ctx->bindRenderTargets(prevRenderTargets);
    ctx->setSpecConstantsInfo(VK_PIPELINE_BIND_POINT_GRAPHICS, prevSpecConstantsInfo);
    ctx->setRasterizerState(prevRasterizerState);

    updateMaterialData(ctx);
    return true;
  }

  bool TerrainBaker::getExternalBakeLayout(ExternalBakeLayout& out) const {
    std::lock_guard lock(m_externalMutex);

    if (!m_externalLayoutValid)
      return false;

    out = m_externalLayout;
    return true;
  }

  bool TerrainBaker::bakeExternal(Rc<RtxContext> ctx,
                                  const DxvkContextState& dxvkCtxState,
                                  const DrawCallState& drawCallState,
                                  Matrix4& textureTransformOut) {
    ScopedGpuProfileZone(ctx, "Terrain Baker: External Bake");

    const ExternalTerrainBake& bake = *drawCallState.externalTerrainBake;

    // This frame's parameters (and the terrain BBOX the next layout uses).
    registerTerrainMesh(ctx, dxvkCtxState, drawCallState);

    if (bake.image == nullptr || bake.numCascades == 0 || bake.numCascades > 16
     || bake.cascadeMapSizeX * bake.cascadeMapSizeY < bake.numCascades)
      return false;

    // The bake used the layout of an earlier frame. The cascade selection in
    // the ray tracing shaders must use the same one, so it replaces this
    // frame's for the terrain (the front end has no bakes of its own here).
    m_bakingParams.numCascades      = bake.numCascades;
    m_bakingParams.cascadeMapSize.x = bake.cascadeMapSizeX;
    m_bakingParams.cascadeMapSize.y = bake.cascadeMapSizeY;
    m_bakingParams.lastCascadeScale = bake.lastCascadeScale;
    calculateCascadeMapResolution(ctx->getDevice());

    RtxTextureManager& textureManager = ctx->getCommonObjects()->getTextureManager();
    const RtxMipmap::Resource& terrainResource = getTerrainTexture(ctx, textureManager, ReplacementMaterialTextureType::AlbedoOpacity,
      m_bakingParams.cascadeMapResolution.width, m_bakingParams.cascadeMapResolution.height);
    const Rc<DxvkImageView>& terrainTextureView = terrainResource.views.empty() ? terrainResource.view : terrainResource.views[0];

    if (terrainTextureView == nullptr) {
      ONCE(Logger::err("[RTX Terrain Baker] Failed to retrieve the albedo terrain texture; skipping the external bake."));
      return false;
    }

    // One copy per frame: every terrain draw of the frame baked into the
    // same image. The grids match tile for tile, so one scaled blit moves
    // each cascade level into its place.
    const uint32_t currentFrameIndex = ctx->getDevice()->getCurrentFrameId();

    if (m_externalCopiedFrame != currentFrameIndex || m_externalCopied != bake.image) {
      const Rc<DxvkImage>& src = bake.image->image();
      const Rc<DxvkImage>& dst = terrainTextureView->image();
      const VkExtent3D srcExtent = src->info().extent;
      const VkExtent3D dstExtent = dst->info().extent;

      VkImageBlit region = {};
      region.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
      region.srcOffsets[1]  = { int32_t(srcExtent.width), int32_t(srcExtent.height), 1 };
      region.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
      region.dstOffsets[1]  = { int32_t(dstExtent.width), int32_t(dstExtent.height), 1 };

      const VkComponentMapping identity = {
        VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
        VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY };

      ctx->blitImage(dst, identity, src, identity, region, VK_FILTER_LINEAR);

      m_externalCopied = bake.image;
      m_externalCopiedFrame = currentFrameIndex;
    }

    m_materialTextures[ReplacementMaterialTextureType::AlbedoOpacity].markAsBaked();

    if (!debugDisableBinding()) {
      const RtCamera& camera = ctx->getSceneManager().getCamera();
      textureTransformOut = bake.worldToCascade0Texture * camera.getViewToWorld();
    }

    updateMaterialData(ctx);
    return true;
  }

  void TerrainBaker::updateMaterialData(Rc<RtxContext> ctx) {
    if (m_hasInitializedMaterialDataThisFrame && !m_needsMaterialDataUpdate) {
      return;
    }

    // We're going to use this to create a modified sampler for terrain textures.
    // Terrain textures have only mip 0, so use nearest for mip filtering
    if (!m_terrainSampler.ptr()) {
      Resources& resourceManager = ctx->getResourceManager();
      m_terrainSampler = resourceManager.getSampler(VK_FILTER_LINEAR, VK_SAMPLER_MIPMAP_MODE_NEAREST, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);
    }
    
    auto createTextureRef = [&](ReplacementMaterialTextureType::Enum textureType) {
      return m_materialTextures[textureType].isBaked()
        ? TextureRef(m_materialTextures[textureType].texture.view)
        : TextureRef();
    };

    // Create a material with the baked material textures
    m_materialData.emplace(OpaqueMaterialData(
      createTextureRef(ReplacementMaterialTextureType::AlbedoOpacity),
      createTextureRef(ReplacementMaterialTextureType::Normal),
      createTextureRef(ReplacementMaterialTextureType::Tangent),
      createTextureRef(ReplacementMaterialTextureType::Height),
      createTextureRef(ReplacementMaterialTextureType::Roughness),
      createTextureRef(ReplacementMaterialTextureType::Metallic),
      createTextureRef(ReplacementMaterialTextureType::Emissive),
      TextureRef(), TextureRef(), TextureRef(), TextureRef(), TextureRef(), // SSS textures
      Material::Properties::roughnessAnisotropy(),
      Material::Properties::emissiveIntensity(),
      Vector3(1, 1, 1), // AlbedoConstant - unused since the AlbedoOpacity texture must be always present for baking
      1.f, // OpacityConstant - unused since the AlbedoOpacity texture must be always present for baking
      Material::Properties::roughnessConstant(),
      Material::Properties::metallicConstant(),
      Material::Properties::emissiveColorConstant(),
      Material::Properties::enableEmission(),
      // Setting expected constant values. Baked terrain should not need to have other values for the below material parameters set
      1, 1, 0, /* spriteSheet* */
      false, // LegacyMaterialDefaults::enableThinFilm(),
      false, // LegacyMaterialDefaults::alphaIsThinFilmThickness(),
      0.f,
      false, // Set to false for now, otherwise the baked terrain is not fully opaque - opaqueMaterialDefaults.UseLegacyAlphaState
      false, // OpaqueMaterialDefaults::BlendEnabled,
      BlendType::kAlpha,
      false, // OpaqueMaterialDefaults::InvertedBlend,
      AlphaTestType::kAlways,
      0,//OpaqueMaterialDefaults::AlphaReferenceValue;
      // Using the previous frame's displaceIn/Out because all current frame draw calls are normalized to the previous frame's max.
      m_prevFrameMaxDisplaceIn / Material::Properties::displaceInFactor(),  // OpaqueMaterialDefaults::DisplaceIn
      m_prevFrameMaxDisplaceOut / Material::Properties::displaceInFactor(),  // OpaqueMaterialDefaults::DisplaceOut
      Vector3(),  // OpaqueMaterialDefaults::subsurfaceTransmittanceColor
      0.0f,  // OpaqueMaterialDefaults::subsurfaceMeasurementDistance
      Vector3(),  // OpaqueMaterialDefaults::subsurfaceSingleScatteringAlbedo
      0.0f, // OpaqueMaterialDefaults::subsurfaceVolumetricAnisotropy
      false, // OpaqueMaterialDefaults::subsurfaceDiffusionProfile
      Vector3(),  // OpaqueMaterialDefaults::subsurfaceRadius
      0.0f, // OpaqueMaterialDefaults::subsurfaceRadiusScale
      0.0f, // OpaqueMaterialDefaults::subsurfaceMaxSampleRadius
      uint8_t(0), // NormalEncoding: baked terrain normals are decoded by the baker path
      // NOTE: The terrain defines it's own sampler, and these are the modes it uses.
      lss::Mdl::Filter::Linear,
      lss::Mdl::WrapMode::Clamp, // U
      lss::Mdl::WrapMode::Clamp  // V
    ));

    m_hasInitializedMaterialDataThisFrame = true;
    m_needsMaterialDataUpdate = false;
  }

  const RtxMipmap::Resource& TerrainBaker::getTerrainTexture(ReplacementMaterialTextureType::Enum textureType) const {
    return m_materialTextures[textureType].texture;
  }

  const RtxMipmap::Resource& TerrainBaker::getTerrainTexture(
    Rc<DxvkContext> ctx, 
    RtxTextureManager& textureManager, 
    ReplacementMaterialTextureType::Enum textureType, 
    uint32_t width, 
    uint32_t height) {
    VkExtent3D resolution = { width, height, 1 };

    RtxMipmap::Resource& texture = m_materialTextures[static_cast<uint32_t>(textureType)].texture;

    // Recreate the texture
    if (!texture.isValid() ||
        texture.image->info().extent != resolution) {

      // WAR (REMIX-1557) to force release previous terrain texture reference from texture cache since it doesn't do it automatically resulting in a leak
      if (texture.isValid()) {
        TextureRef textureRef = TextureRef(texture.view);
        textureManager.releaseTexture(textureRef);

        if (texture.views.size() > 0) {
          for (Rc<DxvkImageView>& view : texture.views) {
            auto viewRef = TextureRef(view);
            textureManager.releaseTexture(viewRef);
          }
        }
      }

      texture = RtxMipmap::createResource(
        ctx, "baked terrain texture", resolution, getTextureFormat(textureType), VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, getClearColor(textureType), getMipLevels(textureType, resolution));

      m_needsMaterialDataUpdate = true;
    }

    return texture;
  }

  const Rc<DxvkSampler>& TerrainBaker::getTerrainSampler() const {
    return m_terrainSampler;
  }

  const MaterialData* TerrainBaker::getMaterialData() const {
    if (m_materialData.has_value()) {
      return &(*m_materialData);
    }

    return nullptr;
  }

  TerrainArgs TerrainBaker::getTerrainArgs() const {

    TerrainArgs args;

    args.cascadeMapSize = m_bakingParams.cascadeMapSize;
    args.rcpCascadeMapSize.x = 1.f / args.cascadeMapSize.x;
    args.rcpCascadeMapSize.y = 1.f / args.cascadeMapSize.y;

    args.maxCascadeLevel = m_bakingParams.numCascades - 1;
    if (m_materialData.has_value()) {
      args.displaceIn = m_materialData->getOpaqueMaterialData().getDisplaceIn();
    } else {
      args.displaceIn = 0.0f;
    }
    args.lastCascadeScale = m_bakingParams.lastCascadeScale;

    return args;
  }

  void TerrainBaker::showImguiSettings() const {

    constexpr ImGuiSliderFlags sliderFlags = ImGuiSliderFlags_AlwaysClamp;

    {
      RemixGui::Checkbox("Use Terrain Bounding Box", &cascadeMap.useTerrainBBOXObject());
      RemixGui::Checkbox("Clear Terrain Textures Before Terrain Baking", &clearTerrainBeforeBakingObject());

      if (RemixGui::CollapsingHeader("Material")) {
        ImGui::Indent();

        const bool isPSReplacementSupportEnabled = Material::replacementSupportInPS_fixedFunction() || Material::replacementSupportInPS_programmableShaders();
        ImGui::BeginDisabled(!isPSReplacementSupportEnabled);
        RemixGui::Checkbox("Replacements Support in PS", &Material::replacementSupportInPSObject());
        ImGui::EndDisabled();

        RemixGui::Checkbox("Bake Replacement Materials", &Material::bakeReplacementMaterialsObject());
        RemixGui::Checkbox("Bake Secondary PBR Textures", &Material::bakeSecondaryPBRTexturesObject());
        RemixGui::DragInt("Max Resolution (except for colorOpacity)", &Material::maxResolutionToUseForReplacementMaterialsObject(), 1.f, 1, 16384);

        if (RemixGui::CollapsingHeader("Properties")) {
          ImGui::Indent();

          RemixGui::ColorEdit3("Emissive Color", &Material::Properties::emissiveColorConstantObject());
          RemixGui::Checkbox("Enable Emission", &Material::Properties::enableEmissionObject());
          RemixGui::DragFloat("Emissive Intensity", &Material::Properties::emissiveIntensityObject(), 0.01f, 0.f, FLT_MAX, "%.3f", sliderFlags);
          RemixGui::DragFloat("Roughness", &Material::Properties::roughnessConstantObject(), 0.01f, 0.f, 1.f, "%.3f", sliderFlags);
          RemixGui::DragFloat("Metallic", &Material::Properties::metallicConstantObject(), 0.01f, 0.f, 1.f, "%.3f", sliderFlags);
          RemixGui::DragFloat("Anisotropy", &Material::Properties::roughnessAnisotropyObject(), 0.01f, -1.0f, 1.f, "%.3f", sliderFlags);
          
          ImGui::Text("\nDisplacement Settings");
          RemixGui::DragFloat("Displacement Factor", &Material::Properties::displaceInFactorObject(), 0.01f, 0.01f, 100.f, "%.3f", sliderFlags);
          ImGui::Text("Calculate the lowest safe Displacement Factor for the current \n"
                      "scene.  Mod creators should run this in scenes across the \n"
                      "game, and use the highest returned value.  See Displacement \n"
                      "Factor's tooltip for more info.");
          if (ImGui::Button("Calculate Scene's Optimal Displacement Factor")) {
            m_calculateDisplaceInFactorNextFrame = true;
          }

          ImGui::Unindent();
        }
        ImGui::Unindent();
      }

      if (RemixGui::CollapsingHeader("Cascade Map")) {
        ImGui::Indent();

        RemixGui::DragFloat("Cascade Map's Default Half Width [meters]", &cascadeMap.defaultHalfWidthObject(), 1.f, 0.1f, 10000.f);
        RemixGui::DragFloat("Cascade Map's Default Height [meters]", &cascadeMap.defaultHeightObject(), 1.f, 0.1f, 10000.f);
        RemixGui::DragFloat("First Cascade Level's Half Width [meters]", &cascadeMap.levelHalfWidthObject(), 1.f, 0.1f, 10000.f);

        RemixGui::DragInt("Max Cascade Levels", &cascadeMap.maxLevelsObject(), 1.f, 1, 16);
        RemixGui::DragInt("Texture Resolution Per Cascade Level", &cascadeMap.levelResolutionObject(), 8.f, 1, 32 * 1024);
        RemixGui::Checkbox("Expand Last Cascade Level", &cascadeMap.expandLastCascadeObject());

        if (RemixGui::CollapsingHeader("Statistics")) {
          ImGui::Indent();
        
          ImGui::Text("Cascade Levels: %u", m_bakingParams.numCascades);
          ImGui::Text("Cascade Level Resolution: %u, %u", m_bakingParams.cascadeLevelResolution.width, m_bakingParams.cascadeLevelResolution.height);
          ImGui::Text("Cascade Map Resolution: %u, %u", m_bakingParams.cascadeMapResolution.width, m_bakingParams.cascadeMapResolution.height);
        
          ImGui::Unindent();
        }

        ImGui::Unindent();
      }

      RemixGui::Checkbox("Debug: Disable Baking", &debugDisableBakingObject());
      RemixGui::Checkbox("Debug: Disable Binding", &debugDisableBindingObject());
    }
  }

  void TerrainBaker::calculateTerrainBBOX(const uint32_t currentFrameIndex) {
    m_bakedTerrainBBOX.invalidate();

    // Find the union of all terrain mesh BBOXes
    if (m_terrainMeshBBOXes.size() > 0) {
      for (auto& meshBBOX : m_terrainMeshBBOXes) {
        m_bakedTerrainBBOX.unionWith(meshBBOX.calculateAABBInWorldSpace());
      }
      m_terrainMeshBBOXes.clear();
      m_terrainBBOXFrameIndex = currentFrameIndex;
    }
  }

  void TerrainBaker::onFrameEnd(Rc<DxvkContext> ctx) {
    RtxTextureManager& textureManager = ctx->getCommonObjects()->getTextureManager();
    const uint32_t currentFrameIndex = ctx->getDevice()->getCurrentFrameId();

    if (TerrainBaker::needsTerrainBaking()) {
      // Expects the mesh BBOXes to be calculated by this point
      calculateTerrainBBOX(currentFrameIndex);
    }

    m_hasInitializedMaterialDataThisFrame = false;

    for (BakedTexture& texture : m_materialTextures) {
      texture.onFrameEnd(ctx);
    }

    if (m_calculatingDisplaceInFactor) {
      m_calculatingDisplaceInFactor = false;
      Material::Properties::displaceInFactor.setDeferred(m_calculatedDisplaceInFactor);
    }
    m_calculatingDisplaceInFactor = m_calculateDisplaceInFactorNextFrame;
    m_calculateDisplaceInFactorNextFrame = false;

    m_prevFrameMaxDisplaceIn = m_currFrameMaxDisplaceIn;
    m_currFrameMaxDisplaceIn = 0.f;

    m_prevFrameMaxDisplaceOut = m_currFrameMaxDisplaceOut;
    m_currFrameMaxDisplaceOut = 0.f;

    m_stagingTextureCache.clear();

    // Destroy material data every frame so as not keep texture references around.
    // Material data gets recreated every frame on baking
    m_materialData.reset();
  }

  void TerrainBaker::prepareSceneData(Rc<RtxContext> ctx) {
    if (TerrainBaker::needsTerrainBaking()) {
      // update the height mipmap
      if (m_materialTextures[ReplacementMaterialTextureType::Height].texture.isValid()) {
        ScopedGpuProfileZone(ctx, "Terrain Height Mip Map");
        RtxMipmap::updateMipmap(ctx, m_materialTextures[ReplacementMaterialTextureType::Height].texture, MipmapMethod::Maximum);
      }
    }
  }

  void TerrainBaker::BakedTexture::onFrameEnd(Rc<DxvkContext> ctx) {

    auto releaseTexture = [&](RtxMipmap::Resource& texture) {
      if (!texture.isValid()) {
        return;
      }

      RtxTextureManager& textureManager = ctx->getCommonObjects()->getTextureManager();

      // WAR (REMIX-1557) to force release terrain texture reference from texture cache since it doesn't do it automatically resulting in a leak
      TextureRef textureRef = TextureRef(texture.view);
      textureManager.releaseTexture(textureRef);
      
      if (texture.views.size() > 0) {
        for (Rc<DxvkImageView>& view : texture.views) {
          auto viewRef = TextureRef(view);
          textureManager.releaseTexture(viewRef);
        }
      }
      texture.reset();
    };

    // Retain textures when baking is disabled as they are not being refreshed and can still be used
    if (!debugDisableBaking()) {
      if (numFramesToRetain > 0) {
        numFramesToRetain--;
      }
    }

    // Release the texture if it has not been baked to recently
    if (numFramesToRetain == 0) {
      releaseTexture(texture);
    }
  }

  void TerrainBaker::updateTextureFormat(const DxvkContextState& dxvkCtxState) {
    DxvkRenderTargets currentRenderTargets = dxvkCtxState.om.renderTargets;

    // External bakes (DX12 / Vulkan front end) arrive with no render target
    // bound in Remix's own context.
    if (currentRenderTargets.color[0].view == nullptr)
      return;

    VkFormat terrainRtColorFormat = currentRenderTargets.color[0].view->image()->info().format;
    VkFormat terrainSrgbColorFormat = TextureUtils::toSRGB(terrainRtColorFormat);

    // RT shaders expect the textures in sRGB format but but as linear targets
    if (terrainRtColorFormat == terrainSrgbColorFormat) {
      ONCE(Logger::warn(str::format("[RTX Terrain Baker] Terrain render target is of sRGB format ", terrainRtColorFormat, ". Instead, it is expected to be of linear format.")));
    }
  }

  void TerrainBaker::clearMaterialTexture(Rc<DxvkContext> ctx, ReplacementMaterialTextureType::Enum textureType) {
    Resources::Resource& texture = m_materialTextures[textureType].texture;

    VkImageSubresourceRange subRange = {};
    subRange.layerCount = 1;
    subRange.levelCount = 1;
    subRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;

    ctx->clearColorImage(texture.image, getClearColor(textureType), subRange);
  }

  TerrainBaker::AxisAlignedBoundingBoxLink::AxisAlignedBoundingBoxLink(const DrawCallState& drawCallState)
    : aabbObjectSpace(drawCallState.getGeometryData().boundingBox)
    , objectToWorld(drawCallState.getTransformData().objectToWorld) {
  }

  AxisAlignedBoundingBox TerrainBaker::AxisAlignedBoundingBoxLink::calculateAABBInWorldSpace() {
    AxisAlignedBoundingBox aabb;
    aabb.minPos = (objectToWorld * Vector4(aabbObjectSpace.minPos, 1.f)).xyz();
    aabb.maxPos = (objectToWorld * Vector4(aabbObjectSpace.maxPos, 1.f)).xyz();
    return aabb;
  }

  bool TerrainBaker::needsTerrainBaking() {
    return enableBaking() && RtxOptions::terrainTextures().size() > 0;
  }

  void TerrainBaker::onFrameBegin(Rc<RtxContext> ctx, const DxvkContextState& dxvkCtxState) {
    Resources& resourceManager = ctx->getResourceManager();
    RtxTextureManager& textureManger = ctx->getCommonObjects()->getTextureManager();

    // Force material data update every frame to pick up any material parameter changes
    m_needsMaterialDataUpdate = true;

    // Draws baked in the last terrain frame (re-bake skipping, bakeDrawCall).
    ProfilerPlotValueI64("terrain bakes skipped", int64_t(m_bakeSkippedThisFrame));
    m_bakedDrawsLastFrame = std::move(m_bakedDrawsThisFrame);
    m_bakedDrawsThisFrame.clear();
    m_bakeSkippedThisFrame = 0;

    updateTextureFormat(dxvkCtxState);
    calculateBakingParameters(ctx, dxvkCtxState);

    // Clear terrain textures
    if (clearTerrainBeforeBaking() && !debugDisableBaking()) {
      for (uint32_t i = 0; i < ReplacementMaterialTextureType::Count; i++) {
        if (m_materialTextures[i].texture.isValid()) {
          clearMaterialTexture(ctx, static_cast<ReplacementMaterialTextureType::Enum>(i));
        }
      }
    }
  }

  void TerrainBaker::registerTerrainMesh(Rc<RtxContext> ctx, const DxvkContextState& dxvkCtxState, const DrawCallState& drawCallState) {
    const uint32_t currentFrameIndex = ctx->getDevice()->getCurrentFrameId();

    // This is the first call in a frame, set up baking state for the new frame
    if (m_bakingParams.frameIndex != currentFrameIndex) {
      onFrameBegin(ctx, dxvkCtxState);
    }

    if (cascadeMap.useTerrainBBOX()) { 
      m_terrainMeshBBOXes.emplace_back(AxisAlignedBoundingBoxLink(drawCallState));
    }
  }

  void TerrainBaker::calculateCascadeMapResolution(const Rc<DxvkDevice>& device) {
    // ToDo: switch to using vkGetPhysicalDeviceImageFormatProperties which may allow larger dimensions for a given image config
    const VkPhysicalDeviceLimits& limits = device->adapter()->deviceProperties().limits;
    const uint32_t maxDimension = limits.maxImageDimension2D;

    m_bakingParams.cascadeLevelResolution = VkExtent2D { cascadeMap.levelResolution(), cascadeMap.levelResolution() };

    // Calculate cascade map resolution
    m_bakingParams.cascadeMapResolution.width = m_bakingParams.cascadeMapSize.x * m_bakingParams.cascadeLevelResolution.width;
    m_bakingParams.cascadeMapResolution.height = m_bakingParams.cascadeMapSize.y * m_bakingParams.cascadeLevelResolution.height;

    // Ensure the texture resolution fits within device limits
    if (m_bakingParams.cascadeMapResolution.width > maxDimension || m_bakingParams.cascadeMapResolution.height > maxDimension) {
      const float2 downscale = {
        static_cast<float>(maxDimension) / m_bakingParams.cascadeMapResolution.width,
        static_cast<float>(maxDimension) / m_bakingParams.cascadeMapResolution.height };

      VkExtent2D prevCascadeMapResolution = m_bakingParams.cascadeMapResolution;

      m_bakingParams.cascadeLevelResolution.width = static_cast<uint32_t>(floor(downscale.x * m_bakingParams.cascadeMapResolution.width) / m_bakingParams.cascadeMapSize.x);
      m_bakingParams.cascadeLevelResolution.height = static_cast<uint32_t>(floor(downscale.y * m_bakingParams.cascadeMapResolution.height) / m_bakingParams.cascadeMapSize.y);

      m_bakingParams.cascadeMapResolution.width = m_bakingParams.cascadeLevelResolution.width * m_bakingParams.cascadeMapSize.x;
      m_bakingParams.cascadeMapResolution.height = m_bakingParams.cascadeLevelResolution.height * m_bakingParams.cascadeMapSize.y;

      ONCE(Logger::warn(str::format("[RTX Terrain Baker] Requested terrain cascade map resolution {", prevCascadeMapResolution.width, ", ", prevCascadeMapResolution.height, "} is outside the device limits {", maxDimension, ", ", maxDimension, "}. Reducing the cascade map resolution to {", m_bakingParams.cascadeMapResolution.width, ", ", m_bakingParams.cascadeMapResolution.height, "}.")));
    }
  }

  void TerrainBaker::calculateBakingParameters(Rc<RtxContext> ctx, const DxvkContextState& dxvkCtxState) {

    SceneManager& sceneManager = ctx->getSceneManager();
    Resources& resourceManager = ctx->getResourceManager();
    const RtCamera& camera = sceneManager.getCamera();
    const uint32_t currentFrameIndex = ctx->getDevice()->getCurrentFrameId();
    const float metersToWorldUnitScale = RtxOptions::getMeterToWorldUnitScale();

    // The previous layout, to tell whether the cascades moved this frame.
    const Matrix4 prevSceneView = m_bakingParams.sceneView;
    const std::vector<Matrix4> prevOrtho = m_bakingParams.bakingCameraOrthoProjection;
    const uint32_t prevNumCascades = m_bakingParams.numCascades;
    const VkExtent2D prevMapResolution = m_bakingParams.cascadeMapResolution;

    m_bakingParams.frameIndex = currentFrameIndex;

    const bool terrainBBOXIsValid = m_bakedTerrainBBOX.isValid();
    const float epsilon = 0.01f;      // Epsilon to ensure distances are greater or equal

    const float terrainHeight =
      terrainBBOXIsValid
      ? SceneManager::worldToSceneOrientedVector(m_bakedTerrainBBOX.maxPos - m_bakedTerrainBBOX.minPos).z
      : metersToWorldUnitScale * cascadeMap.defaultHeight();

    const float cameraRelativeTerrainHeight =
      terrainBBOXIsValid
      ? SceneManager::worldToSceneOrientedVector(m_bakedTerrainBBOX.maxPos - camera.getPosition()).z
      : metersToWorldUnitScale * cascadeMap.defaultHeight() / 2; // Assume camera is in the middle of terrain's height span

    // Constants set to what makes generally should make sense
    // Offset zFar by zNear to match the baking camera position being offset by it.
    // Offset by 1.f in case terrainHeight is zero (i.e. it's planar)
    const float zNear = 0.01f;
    const float zFar = (terrainHeight + 1.f) * (1 + epsilon) + zNear;

    // Compute the relative half width of the cascade map around the camera
    float cascadeMapHalfWidth = metersToWorldUnitScale * cascadeMap.defaultHalfWidth();
    if (terrainBBOXIsValid) {
      // Add offset for all terrain samples to be within the baked terrain texture
      const float halfTexelOffset = 10;       // ToDo: calculate an exact value

      // Compute bbox relative to the camera
      AxisAlignedBoundingBox cameraRelativeTerrainBBOX = {
        m_bakedTerrainBBOX.minPos - camera.getPosition() - Vector3{ halfTexelOffset },
        m_bakedTerrainBBOX.maxPos - camera.getPosition() + Vector3{ halfTexelOffset }
      };

      // Convert the bbox to scene space
      cameraRelativeTerrainBBOX.minPos = SceneManager::worldToSceneOrientedVector(cameraRelativeTerrainBBOX.minPos);
      cameraRelativeTerrainBBOX.maxPos = SceneManager::worldToSceneOrientedVector(cameraRelativeTerrainBBOX.maxPos);

      // Calculate a half width of a cascade map around camera that covers the terrain's BBOX
      cascadeMapHalfWidth =
        std::max(std::max(std::abs(cameraRelativeTerrainBBOX.maxPos.x), std::abs(cameraRelativeTerrainBBOX.minPos.x)),
                 std::max(std::abs(cameraRelativeTerrainBBOX.maxPos.y), std::abs(cameraRelativeTerrainBBOX.minPos.y)));
    }

    // Construct a scene oriented view
    Matrix4 sceneView;
    {
      const Vector3 up = SceneManager::getSceneUp();
      const Vector3 forward = SceneManager::getSceneForward();
      const Vector3 right = SceneManager::calculateSceneRight();

      // Set baking camera position just above the terrain
      // Offset by zNear so that zNear doesn't clip the terrain
      // Offset by epsilon so that it doesn't clip top of the terrain
      const Vector3 bakingCameraPosition = cameraRelativeTerrainHeight >= 0.f
        ? camera.getPosition() + (cameraRelativeTerrainHeight * (1 + epsilon) + zNear) * up
        : camera.getPosition() + (cameraRelativeTerrainHeight * (1 - epsilon) - zNear) * up;

      const Vector3 translation = Vector3(
        dot(right, -bakingCameraPosition),
        dot(forward, -bakingCameraPosition),
        dot(up, -bakingCameraPosition));

      sceneView[0] = Vector4(right.x, forward.x, up.x, 0.f);
      sceneView[1] = Vector4(right.y, forward.y, up.y, 0.f);
      sceneView[2] = Vector4(right.z, forward.z, up.z, 0.f);
      sceneView[3] = Vector4(translation.x, translation.y, translation.z, 1.f);
    }

    m_bakingParams.sceneView = sceneView;
    m_bakingParams.inverseSceneView = inverse(sceneView);

    // Number of cascades required to cover the whole bbox
    const uint32_t numRequiredCascades = 
      1 + static_cast<uint32_t>(ceil(log2(std::max(1.f, cascadeMapHalfWidth / (metersToWorldUnitScale * cascadeMap.levelHalfWidth())))));

    // Number of cascades actually used
    m_bakingParams.numCascades = std::min(cascadeMap.maxLevels(), numRequiredCascades);

    // If there isn't enough cascades to cover the terrain radius, expand the last cascade to cover the cascade map's span
    const bool isLastCascadeExpanded = m_bakingParams.numCascades != numRequiredCascades;
    m_bakingParams.lastCascadeScale = 1.f;

    m_bakingParams.cascadeMapSize.x = static_cast<uint32_t>(ceilf(sqrtf(static_cast<float>(m_bakingParams.numCascades))));
    m_bakingParams.cascadeMapSize.y = static_cast<uint32_t>(ceilf(static_cast<float>(m_bakingParams.numCascades) / m_bakingParams.cascadeMapSize.x));

    m_bakingParams.bakingCameraOrthoProjection.resize(m_bakingParams.numCascades);

    // Calculate cascade map resolution
    calculateCascadeMapResolution(ctx->getDevice());

    const float2 float2CascadeLevelResolution = float2 {
      static_cast<float>(m_bakingParams.cascadeLevelResolution.width),
      static_cast<float>(m_bakingParams.cascadeLevelResolution.height)
    };

    // Calculate params for each cascade level.
    // The levels are tiled left to right top to bottom in the combined render target texture
    for (uint32_t iCascade = 0; iCascade < m_bakingParams.numCascades; iCascade++) {

      Vector2i cascade2DIndex;
      cascade2DIndex.y = iCascade / m_bakingParams.cascadeMapSize.x;
      cascade2DIndex.x = iCascade - cascade2DIndex.y * m_bakingParams.cascadeMapSize.x;

      // Set viewport which maps clip space <-1, 1> to screen space <0, resolution>.
      // Accounts for inverted y coordinate in Vulkan
      VkViewport viewport = VkViewport {
        cascade2DIndex.x * float2CascadeLevelResolution.x,
        (cascade2DIndex.y + 1) * float2CascadeLevelResolution.y,
        float2CascadeLevelResolution.x,
        -float2CascadeLevelResolution.y,
        0.f, 1.f
      };

      VkOffset2D cascadeOffset = VkOffset2D {
        static_cast<int>(cascade2DIndex.x * m_bakingParams.cascadeLevelResolution.width),
        static_cast<int>(cascade2DIndex.y * m_bakingParams.cascadeLevelResolution.height) };

      // Set scissor window which clips the screen space
      VkRect2D scissor = VkRect2D { cascadeOffset, m_bakingParams.cascadeLevelResolution };

      // Half width of the cascade level
      float halfWidth = metersToWorldUnitScale * cascadeMap.levelHalfWidth() * pow(2, iCascade);

      // Expand the last cascade level if necessary
      const bool isLastCascade = iCascade == m_bakingParams.numCascades - 1;
      if (isLastCascade && isLastCascadeExpanded && cascadeMap.expandLastCascade()) {
        // Note: 1st cascade is naturally expanded by matching the projection to the expanded range, rather than applying 
        // expansion scale if it is to be expanded. But for pedantic purposes we set the scale to 1 here anyway
        m_bakingParams.lastCascadeScale = iCascade > 0 ? cascadeMapHalfWidth / halfWidth : 1.f;
        halfWidth = cascadeMapHalfWidth;
      }

      // Setup orthographic projection top-down that maps <-halfWidth, halfWidth> around camera to <0, 1> in clip space
      float4x4& newProjection = *reinterpret_cast<float4x4*>(&m_bakingParams.bakingCameraOrthoProjection[iCascade]);
      newProjection.SetupByOrthoProjection(-halfWidth, halfWidth, -halfWidth, halfWidth, zNear, zFar);

      if (iCascade == 0) {
        // Convert from clip space <-1, 1> to <0, 1> and flip y coordinate for Vulkan
        const Matrix4 textureOffset = Matrix4(Vector4(.5f, 0, 0, 0),
                                              Vector4(0, -.5f, 0, 0),
                                              Vector4(0, 0, 1, 0),
                                              Vector4(.5f, .5f, 0, 1));

        m_bakingParams.viewToCascade0TextureSpace = textureOffset * m_bakingParams.bakingCameraOrthoProjection[iCascade] * sceneView * camera.getViewToWorld();
      }
    }

    // Same layout as last frame: bakes done then are still valid.
    m_bakingParamsUnchanged = prevNumCascades == m_bakingParams.numCascades
      && prevMapResolution.width == m_bakingParams.cascadeMapResolution.width
      && prevMapResolution.height == m_bakingParams.cascadeMapResolution.height
      && std::memcmp(&prevSceneView, &m_bakingParams.sceneView, sizeof(Matrix4)) == 0
      && prevOrtho.size() == m_bakingParams.bakingCameraOrthoProjection.size()
      && (prevOrtho.empty() || std::memcmp(prevOrtho.data(), m_bakingParams.bakingCameraOrthoProjection.data(),
                                           prevOrtho.size() * sizeof(Matrix4)) == 0);

    // The same cascades in world terms, for the front end's bakes of the
    // next frame (getExternalBakeLayout).
    {
      const Matrix4 textureOffset = Matrix4(Vector4(.5f, 0, 0, 0),
                                            Vector4(0, -.5f, 0, 0),
                                            Vector4(0, 0, 1, 0),
                                            Vector4(.5f, .5f, 0, 1));

      std::lock_guard lock(m_externalMutex);
      m_externalLayout.numCascades      = std::min(m_bakingParams.numCascades, 16u);
      m_externalLayout.cascadeMapSizeX  = m_bakingParams.cascadeMapSize.x;
      m_externalLayout.cascadeMapSizeY  = m_bakingParams.cascadeMapSize.y;
      m_externalLayout.lastCascadeScale = m_bakingParams.lastCascadeScale;

      for (uint32_t c = 0; c < m_externalLayout.numCascades; c++)
        m_externalLayout.worldToCascadeClip[c] = m_bakingParams.bakingCameraOrthoProjection[c] * sceneView;

      m_externalLayout.worldToCascade0Texture = textureOffset * m_bakingParams.bakingCameraOrthoProjection[0] * sceneView;
      m_externalLayoutValid = true;
    }
  }
}