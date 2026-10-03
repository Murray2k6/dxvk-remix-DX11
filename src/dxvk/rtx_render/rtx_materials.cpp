/*
* Copyright (c) 2022, NVIDIA CORPORATION. All rights reserved.
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

#include "rtx_materials.h"

#include <algorithm>

#include "rtx_options.h"

namespace dxvk {

bool getEnableDiffuseLayerOverrideHack() {
  return TranslucentMaterialOptions::enableDiffuseLayerOverride();
}

float getEmissiveIntensity() {
  return RtxOptions::emissiveIntensity();
}

float getDisplacementFactor() {
  return RtxOptions::Displacement::displacementFactor();
}

float getDisplacementInFactor() {
  return RtxOptions::Displacement::displacementFactor() * RtxOptions::Displacement::displacementInFactor();
}

float getDisplacementOutFactor() {
  return RtxOptions::Displacement::displacementFactor() * RtxOptions::Displacement::displacementOutFactor();
}

dxvk::OpaqueMaterialData LegacyMaterialData::createDefault() {
  OpaqueMaterialData opaqueMat;
  opaqueMat.setAnisotropyConstant(LegacyMaterialDefaults::anisotropy());
  opaqueMat.setEmissiveIntensity(LegacyMaterialDefaults::emissiveIntensity());
  opaqueMat.setAlbedoConstant(LegacyMaterialDefaults::albedoConstant());
  opaqueMat.setOpacityConstant(LegacyMaterialDefaults::opacityConstant());
  opaqueMat.setRoughnessConstant(LegacyMaterialDefaults::roughnessConstant());
  opaqueMat.setMetallicConstant(LegacyMaterialDefaults::metallicConstant());
  opaqueMat.setEmissiveColorConstant(LegacyMaterialDefaults::emissiveColorConstant());
  opaqueMat.setEnableEmission(LegacyMaterialDefaults::enableEmissive());
  opaqueMat.setEnableThinFilm(LegacyMaterialDefaults::enableThinFilm());
  opaqueMat.setAlphaIsThinFilmThickness(LegacyMaterialDefaults::alphaIsThinFilmThickness());
  opaqueMat.setThinFilmThicknessConstant(LegacyMaterialDefaults::thinFilmThicknessConstant());
  return opaqueMat;
}

template<> OpaqueMaterialData LegacyMaterialData::as() const {
  // Legacy materials have parameters that can directly carry over onto the opaque material.
  const OpaqueMaterialData defaultLegacyOpaqueMaterial = createDefault();
  // Copy off the defaults, and make dynamic adjustments for the remaining params from this legacy material
  OpaqueMaterialData opaqueMat(defaultLegacyOpaqueMaterial);
  if (LegacyMaterialDefaults::useAlbedoTextureIfPresent()) {
    opaqueMat.setAlbedoOpacityTexture(getColorTexture());
  }
  // Constant-color materials carry their color in shader constant registers rather than a
  // texture; without this they render plain white. Opacity stays at the default - the
  // vector's w component rarely holds opacity.
  if (hasConstantAlbedo && !getColorTexture().isValid()) {
    const Vector3 clampedAlbedo(
      std::clamp(constantAlbedo.x, 0.0f, 1.0f),
      std::clamp(constantAlbedo.y, 0.0f, 1.0f),
      std::clamp(constantAlbedo.z, 0.0f, 1.0f));
    opaqueMat.setAlbedoConstant(clampedAlbedo);
  }
  if (getColorTexture2().isValid()) {
    opaqueMat.setSecondaryTexture(getColorTexture2());
  }
  if (roughnessTexture.isValid())
    opaqueMat.setRoughnessTexture(roughnessTexture);
  if (metallicTexture.isValid())
    opaqueMat.setMetallicTexture(metallicTexture);
  // Legacy envmap: the metallic constant carries the reflection strength / 4
  // (the material range is [0, 1]); the shader scales the mask channel by 4x it.
  if (untintedReflection) {
    opaqueMat.setMetallicConstant(std::clamp(reflectionStrength * 0.25f, 0.0f, 1.0f));
    // Forward envmaps sample the cube at full detail: a glossy surface, not
    // the legacy default roughness, unless the game's smoothness map is known.
    if (!roughnessTexture.isValid())
      opaqueMat.setRoughnessConstant(0.2f);
  }
  // A game emissive/glow map: emission as the game authored it.
  if (emissiveTexture.isValid() && !isLiftedSprite) {
    opaqueMat.setEmissiveColorTexture(emissiveTexture);
    opaqueMat.setEmissiveIntensity(1.0f);
    opaqueMat.setEnableEmission(true);
  }
  // Lifted 2D sprite: emission = the sprite's colour (texture or constant,
  // tinted by vertex colour in the shader); the instance is matte, so this is
  // all it contributes.
  if (isLiftedSprite) {
    if (getColorTexture().isValid())
      opaqueMat.setEmissiveColorTexture(getColorTexture());
    else if (hasConstantAlbedo)
      opaqueMat.setEmissiveColorConstant(Vector3(
        std::clamp(constantAlbedo.x, 0.0f, 1.0f),
        std::clamp(constantAlbedo.y, 0.0f, 1.0f),
        std::clamp(constantAlbedo.z, 0.0f, 1.0f)));
    else
      opaqueMat.setEmissiveColorConstant(Vector3(1.0f));
    opaqueMat.setEmissiveIntensity(1.0f);
    opaqueMat.setEnableEmission(true);
  }
  // Game normal map captured with the draw (DX11 layer): Remix decodes it by
  // its encoding instead of as an octahedral asset.
  // NormalEncoding byte: bits 0-2 normal encoding, 3-4 roughness channel,
  // 5 roughness map is smoothness, 6-7 metallic channel.
  uint8_t packedEncoding = 0;
  if (normalTexture.isValid() && normalEncoding != 0) {
    opaqueMat.setNormalTexture(normalTexture);
    packedEncoding = uint8_t(normalEncoding & 7u);
  }
  packedEncoding |= uint8_t((roughnessChannel & 3u) << 3);
  packedEncoding |= uint8_t(roughnessIsSmoothness ? (1u << 5) : 0u);
  packedEncoding |= uint8_t((metallicChannel & 3u) << 6);
  opaqueMat.setNormalEncoding(packedEncoding);
  // Indicate that we have an exact sampler to use on this material, directly from game
  if (getSampler().ptr()) {
    opaqueMat.setSamplerOverride(getSampler());
  }
  // Ignore colormap alpha of legacy texture if tagged as 'ignoreAlphaOnTextures' 
  bool ignoreAlphaChannel = LegacyMaterialDefaults::ignoreAlphaChannel();
  if (!ignoreAlphaChannel) {
    ignoreAlphaChannel = lookupHash(RtxOptions::ignoreAlphaOnTextures(), getHash());
  }
  opaqueMat.setIgnoreAlphaChannel(ignoreAlphaChannel);
  return opaqueMat;
}

template<> TranslucentMaterialData LegacyMaterialData::as() const {
  TranslucentMaterialData transluscentMat;
  if (getSampler().ptr()) {
    transluscentMat.setSamplerOverride(getSampler());
  }
  // DX11 water: the game's wave normal map, decoded by its NormalEncoding
  // (passed by SceneManager::createSurfaceMaterial when the map is unchanged).
  if (normalTexture.isValid() && normalEncoding != 0) {
    transluscentMat.setNormalTexture(normalTexture);
  }
  // A single-frame sheet: animated-water layering scrolls the normal map
  // within the sheet, and a 0x0 sheet divides by zero.
  transluscentMat.setSpriteSheetRows(1);
  transluscentMat.setSpriteSheetCols(1);
  transluscentMat.setSpriteSheetFPS(0);
  return transluscentMat;
}

template<> RayPortalMaterialData LegacyMaterialData::as() const {
  RayPortalMaterialData portalMat;
  portalMat.getMaskTexture() = getColorTexture();
  portalMat.getMaskTexture2() = getColorTexture2();
  portalMat.setEnableEmission(true);
  portalMat.setEmissiveIntensity(1.f);
  portalMat.setSpriteSheetCols(1);
  portalMat.setSpriteSheetRows(1);
  if (getSampler().ptr()) {
    portalMat.setSamplerOverride(getSampler());
  }
  return portalMat;
}


} // namespace dxvk
