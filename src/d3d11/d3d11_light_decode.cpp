#include <algorithm>
#include <cmath>
#include <cstring>

#include "d3d11_light_decode.h"

namespace dxvk {

  namespace {

    // Offsets are in bytes inside one structured-buffer element.
    struct TiledLightLayoutDesc {
      D3D11TiledLightLayout id;
      uint32_t stride;
      uint32_t positionOffset;   // float3
      uint32_t radiusOffset;     // float
      uint32_t colorOffset;      // float3, linear
      int32_t  typeOffset;       // uint light type, -1 = none
      bool     viewSpace;        // true: view space; false: camera-relative world
      bool     requireDynamic;   // CPU-written DYNAMIC buffer only
    };

    // Creation (FO4): {flags, viewPos.xyz, radius, color.rgb, shadow[4]}.
    // CRYENGINE 5 STiledLightShadeInfo: {type, resIndex, shadowMaskIndex,
    // stencilID, posRad (camera-relative xyz, radius), attenuation, shadow,
    // color, ...} - TiledShading.cfi / D3DTiledShading.cpp.
    constexpr TiledLightLayoutDesc kTiledLightLayouts[] = {
      { D3D11TiledLightLayout::Creation48,   48u,  4u, 16u, 20u, -1, true,  true  },
      { D3D11TiledLightLayout::CryEngine208, 208u, 16u, 28u, 48u,  0, false, false },
      // Unity HDRP 2021.3/2022.3 LightData (LightDefinition.cs.hlsl): positionRWS 0
      // (camera-relative), angleScale 24, angleOffset 28, forward 32, lightType 48,
      // range 68, color 96; tightly packed structured-buffer element of 224 B.
      { D3D11TiledLightLayout::Hdrp224,      224u,  0u, 68u, 96u, 48, false, false },
    };

    const TiledLightLayoutDesc* findLayout(D3D11TiledLightLayout id) {
      for (const auto& l : kTiledLightLayouts) {
        if (l.id == id)
          return &l;
      }

      return nullptr;
    }

  }


  bool D3D11SpotFromAngleScale(float angleScale, float angleOffset, float& innerAngle, float& outerAngle) {
    if (!std::isfinite(angleScale) || !std::isfinite(angleOffset) || angleScale <= 1.0e-4f)
      return false;
    const float cosOuter = -angleOffset / angleScale;
    const float cosInner = std::min(cosOuter + 1.0f / angleScale, 1.0f);
    if (!(cosOuter > -0.999f && cosOuter < 0.9999f))
      return false;
    outerAngle = 2.0f * std::acos(cosOuter);
    innerAngle = 2.0f * std::acos(std::max(cosInner, cosOuter));
    return true;
  }


  uint32_t D3D11DecodeUnrealLocalLights(
    const uint8_t*                bytes,
          size_t                  size,
    const D3D11LightDecodeView&   view,
          std::vector<Dx11LightDesc>& out) {
    const uint32_t lightCount = uint32_t(size / 96u);
    const size_t first = out.size();
    uint32_t invalid = 0;

    for (uint32_t i = 0; i < lightCount && out.size() - first < view.maxLights; ++i) {
      float f[24];
      std::memcpy(f, bytes + size_t(i) * 96u, sizeof(f));
      const float invRadius = f[3];
      const float maxColor = std::max(f[4], std::max(f[5], f[6]));
      bool valid = std::isfinite(invRadius) && invRadius > 1.0e-6f && invRadius < 1.0f
        && std::isfinite(maxColor) && maxColor > 1.0e-5f && maxColor < 1.0e7f
        && f[4] >= 0.0f && f[5] >= 0.0f && f[6] >= 0.0f;
      for (uint32_t c = 0; c < 3 && valid; ++c)
        valid = std::isfinite(f[c]) && std::abs(f[c]) < 1.0e7f;
      if (!valid) {
        if (++invalid > 4u && out.size() == first)
          break;  // not a light buffer
        continue;
      }
      const float radius = 1.0f / invRadius;
      const float brightness = std::min(std::max(maxColor * view.intensityScale, 1.0e-3f), 1.0e4f);
      const float px = D3D11SnapLightCoordinate(view.cameraWorld.x + f[0]);
      const float py = D3D11SnapLightCoordinate(view.cameraWorld.y + f[1]);
      const float pz = D3D11SnapLightCoordinate(view.cameraWorld.z + f[2]);
      const float cr = f[4] / maxColor, cg = f[5] / maxColor, cb = f[6] / maxColor;
      // SpotAngles.x = cos(outer), .y = 1 / (cos(inner) - cos(outer));
      // point lights carry cos(outer) <= -1.
      const float cosOuter = f[12];
      if (cosOuter > -0.999f && cosOuter < 0.9999f && std::isfinite(f[13]) && f[13] > 0.0f) {
        const float cosInner = std::min(cosOuter + 1.0f / f[13], 1.0f);
        out.push_back(Dx11LightStateApi::makeSpot(px, py, pz, f[8], f[9], f[10], cr, cg, cb,
          2.0f * std::acos(cosInner), 2.0f * std::acos(cosOuter), radius * std::sqrt(brightness)));
      } else {
        out.push_back(Dx11LightStateApi::makePoint(px, py, pz, cr, cg, cb, radius * std::sqrt(brightness)));
      }
    }

    return uint32_t(out.size() - first);
  }


  uint32_t D3D11DecodeKatanaClusterLights(
    const uint8_t*                positions,
          size_t                  positionsSize,
    const uint8_t*                attributes,
          size_t                  attributesSize,
          uint32_t                attributeStride,
    const D3D11LightDecodeView&   view,
          std::vector<Dx11LightDesc>& out) {
    if (!positions || !attributes || attributeStride < 12u)
      return 0;

    const uint32_t lightCount = uint32_t(std::min(positionsSize / 16u, attributesSize / (3u * size_t(attributeStride))));
    const size_t first = out.size();
    uint32_t invalidRun = 0;

    for (uint32_t i = 0; i < lightCount && out.size() - first < view.maxLights; ++i) {
      float p[4], c[3];
      std::memcpy(p, positions + size_t(i) * 16u, sizeof(p));
      std::memcpy(c, attributes + size_t(i) * 3u * attributeStride, sizeof(c));

      const float maxColor = std::max(c[0], std::max(c[1], c[2]));
      bool valid = std::isfinite(p[3]) && p[3] > 1.0e-3f && p[3] < 1.0e6f
        && std::isfinite(maxColor) && maxColor > 1.0e-5f && maxColor < 1.0e7f
        && c[0] >= 0.0f && c[1] >= 0.0f && c[2] >= 0.0f;

      for (uint32_t k = 0; k < 3 && valid; ++k)
        valid = std::isfinite(p[k]) && std::abs(p[k]) < 1.0e7f;

      if (!valid) {
        // Unused tail entries; a buffer that starts with garbage is not a
        // light list.
        if (++invalidRun > 4u && out.size() == first)
          break;
        continue;
      }

      invalidRun = 0;
      const float brightness = std::min(std::max(maxColor * view.intensityScale, 1.0e-3f), 1.0e4f);
      out.push_back(Dx11LightStateApi::makePoint(
        D3D11SnapLightCoordinate(p[0]), D3D11SnapLightCoordinate(p[1]), D3D11SnapLightCoordinate(p[2]),
        c[0] / maxColor, c[1] / maxColor, c[2] / maxColor, p[3] * std::sqrt(brightness)));
    }

    return uint32_t(out.size() - first);
  }


  uint32_t D3D11DecodeKatanaClusterSpotLights(
    const uint8_t*                attributes,
          size_t                  attributesSize,
          uint32_t                attributeStride,
    const D3D11LightDecodeView&   view,
          std::vector<Dx11LightDesc>& out) {
    if (!attributes || attributeStride < 12u)
      return 0;

    const size_t lightBytes = 5u * size_t(attributeStride);
    const uint32_t lightCount = uint32_t(attributesSize / lightBytes);
    const size_t first = out.size();
    uint32_t invalidRun = 0;

    for (uint32_t i = 0; i < lightCount && out.size() - first < view.maxLights; ++i) {
      // [5i] position, [5i+1] cone axis, [5i+2] colour, [5i+3] falloff,
      // [5i+4] (range^2, cone offset, cone scale).
      float e[5][3];
      for (uint32_t k = 0; k < 5; ++k)
        std::memcpy(e[k], attributes + size_t(i) * lightBytes + k * attributeStride, sizeof(e[k]));

      const float rangeSq = e[4][0];
      const float offset = e[4][1];
      const float scale = e[4][2];
      const float axisLen = std::sqrt(e[1][0] * e[1][0] + e[1][1] * e[1][1] + e[1][2] * e[1][2]);
      const float maxColor = std::max(e[2][0], std::max(e[2][1], e[2][2]));
      bool valid = std::isfinite(rangeSq) && rangeSq > 1.0e-6f && rangeSq < 1.0e12f
        && std::isfinite(axisLen) && axisLen > 0.5f && axisLen < 2.0f
        && std::isfinite(scale) && std::isfinite(offset)
        && std::isfinite(maxColor) && maxColor > 1.0e-5f && maxColor < 1.0e7f
        && e[2][0] >= 0.0f && e[2][1] >= 0.0f && e[2][2] >= 0.0f;
      for (uint32_t k = 0; k < 3 && valid; ++k)
        valid = std::isfinite(e[0][k]) && std::abs(e[0][k]) < 1.0e7f;

      // The PS's cone term is saturate(dot(toLight, axis) * scale + offset):
      // the beam runs along -axis for a positive scale, +axis for a negative
      // one, and |scale| / offset are the usual angle-scale pair.
      float inner = 0.0f, outer = 0.0f;
      if (valid)
        valid = D3D11SpotFromAngleScale(std::abs(scale), offset, inner, outer);

      if (!valid) {
        if (++invalidRun > 4u && out.size() == first)
          break;
        continue;
      }

      invalidRun = 0;
      const float dirSign = scale > 0.0f ? -1.0f / axisLen : 1.0f / axisLen;
      const float brightness = std::min(std::max(maxColor * view.intensityScale, 1.0e-3f), 1.0e4f);
      out.push_back(Dx11LightStateApi::makeSpot(
        D3D11SnapLightCoordinate(e[0][0]), D3D11SnapLightCoordinate(e[0][1]), D3D11SnapLightCoordinate(e[0][2]),
        e[1][0] * dirSign, e[1][1] * dirSign, e[1][2] * dirSign,
        e[2][0] / maxColor, e[2][1] / maxColor, e[2][2] / maxColor,
        inner, outer, std::sqrt(rangeSq) * std::sqrt(brightness)));
    }

    return uint32_t(out.size() - first);
  }


  uint32_t D3D11DecodeFrostbitePunctualLights(
    const uint8_t*                bytes,
          size_t                  size,
    const D3D11LightDecodeView&   view,
          std::vector<Dx11LightDesc>& out) {
    constexpr uint32_t kStride = 96u;
    const uint32_t entries = std::min(uint32_t(size / kStride), 128u);
    std::vector<Dx11LightDesc> lights;
    uint32_t invalidRun = 0;

    for (uint32_t i = 0; i < entries && invalidRun < 4u && lights.size() < view.maxLights; ++i) {
      float f[24];
      std::memcpy(f, bytes + size_t(i) * kStride, sizeof(f));
      const float invSqrRadius = f[3];
      const float maxColor = std::max(f[4], std::max(f[5], f[6]));
      const float fwdLen = std::sqrt(f[8] * f[8] + f[9] * f[9] + f[10] * f[10]);
      bool valid = std::isfinite(invSqrRadius) && invSqrRadius > 1.0e-10f && invSqrRadius < 1.0e4f
        && std::isfinite(maxColor) && maxColor > 1.0e-6f && f[4] >= 0.0f && f[5] >= 0.0f && f[6] >= 0.0f
        && std::isfinite(fwdLen) && std::abs(fwdLen - 1.0f) < 0.05f;
      for (uint32_t c = 0; c < 3 && valid; ++c)
        valid = std::isfinite(f[c]) && std::abs(f[c]) < 1.0e6f;
      if (!valid) {
        ++invalidRun;
        continue;
      }
      invalidRun = 0;
      const float radius = 1.0f / std::sqrt(invSqrRadius);
      const float brightness = std::min(std::max(maxColor * view.intensityScale, 1.0e-3f), 1.0e4f);
      const float px = D3D11SnapLightCoordinate(view.cameraWorld.x + f[0]);
      const float py = D3D11SnapLightCoordinate(view.cameraWorld.y + f[1]);
      const float pz = D3D11SnapLightCoordinate(view.cameraWorld.z + f[2]);
      const float cr = f[4] / maxColor, cg = f[5] / maxColor, cbl = f[6] / maxColor;
      float inner = 0.0f, outer = 0.0f;
      if (D3D11SpotFromAngleScale(f[20], f[21], inner, outer)) {
        lights.push_back(Dx11LightStateApi::makeSpot(px, py, pz, f[8] / fwdLen, f[9] / fwdLen, f[10] / fwdLen,
          cr, cg, cbl, inner, outer, radius * std::sqrt(brightness)));
      } else {
        lights.push_back(Dx11LightStateApi::makePoint(px, py, pz, cr, cg, cbl, radius * std::sqrt(brightness)));
      }
    }

    // One plausible entry is not a light array.
    if (lights.size() < 2u)
      return 0;

    out.insert(out.end(), lights.begin(), lights.end());
    return uint32_t(lights.size());
  }


  uint32_t D3D11TiledLightStride(D3D11TiledLightLayout layout) {
    const TiledLightLayoutDesc* l = findLayout(layout);
    return l ? l->stride : 0u;
  }


  bool D3D11TiledLightRequiresDynamic(D3D11TiledLightLayout layout) {
    const TiledLightLayoutDesc* l = findLayout(layout);
    return l && l->requireDynamic;
  }


  uint32_t D3D11DecodeTiledLights(
          D3D11TiledLightLayout   layoutId,
    const uint8_t*                bytes,
          size_t                  size,
    const D3D11LightDecodeView&   view,
          std::vector<Dx11LightDesc>& out,
          std::vector<Vector3>*   sunDirections,
          float                 (*samples)[12],
          uint32_t*               sampleCount) {
    const TiledLightLayoutDesc* layout = findLayout(layoutId);

    if (!layout)
      return 0;

    const uint32_t kStride = layout->stride;
    const uint32_t entryCount = uint32_t(size / kStride);
    const size_t first = out.size();
    uint32_t invalidRun = 0;
    uint32_t sampled = 0;
    const float minRadius = layout->viewSpace ? 8.0f : 0.05f;

    for (uint32_t i = 0; i < entryCount && out.size() - first < view.maxLights && invalidRun < 16u; ++i) {
      const uint8_t* e = bytes + size_t(i) * kStride;
      uint32_t hdrpType = 0;
      if (layout->id == D3D11TiledLightLayout::Hdrp224) {
        // HDRP lightType: 0 directional, 1 point, 2 spot, 3 pyramid and
        // 4 box projector, 5 tube, 6 rectangle, 7 disc. Directional and box
        // lights are not punctual; area lights become points at their centre.
        std::memcpy(&hdrpType, e + layout->typeOffset, 4u);
        if (hdrpType < 1u || hdrpType > 7u || hdrpType == 4u)
          continue;
      } else if (layout->typeOffset >= 0) {
        // CRYENGINE light types: 5 point, 6 projector, 8 area. Probes,
        // ambient volumes and the sun are not punctual lights.
        uint32_t type = 0;
        std::memcpy(&type, e + layout->typeOffset, 4u);
        if (type == 9u) {
          // Sun: posRad is the camera-relative sun position; light travels
          // from it toward the camera.
          float sun[3];
          std::memcpy(sun, e + layout->positionOffset, sizeof(sun));
          const Vector3 toSun(sun[0], sun[1], sun[2]);
          const float sunLen = length(toSun);
          if (sunDirections && std::isfinite(sunLen) && sunLen > 1.0e-3f)
            sunDirections->push_back(toSun * (-1.0f / sunLen));
          continue;
        }
        if (type != 5u && type != 6u && type != 8u)
          continue;
      }
      // f[0] unused, [1..3] position, [4] radius, [5..7] colour.
      float f[12] = {};
      std::memcpy(&f[1], e + layout->positionOffset, 12u);
      std::memcpy(&f[4], e + layout->radiusOffset, 4u);
      std::memcpy(&f[5], e + layout->colorOffset, 12u);
      const float radius = f[4];
      const float maxColor = std::max(f[5], std::max(f[6], f[7]));
      // HDRP colours are physical intensities (candela scale), far above
      // the 0..1000 the other layouts carry.
      const float maxColorLimit = layout->id == D3D11TiledLightLayout::Hdrp224 ? 1.0e6f : 1000.0f;
      bool valid = std::isfinite(radius) && radius >= minRadius && radius <= 50000.0f
        && std::isfinite(maxColor) && maxColor > 1.0e-5f && maxColor < maxColorLimit
        && f[5] >= 0.0f && f[6] >= 0.0f && f[7] >= 0.0f;
      for (uint32_t c = 1; c < 4 && valid; ++c)
        valid = std::isfinite(f[c]) && std::abs(f[c]) < 1.0e6f;
      if (!valid) {
        ++invalidRun;
        continue;
      }
      invalidRun = 0;
      if (samples && sampled < 3u)
        std::memcpy(samples[sampled++], f, sizeof(f));

      const Vector4d world = layout->viewSpace
        ? view.viewToWorld * Vector4d(f[1], f[2], f[3], 1.0)
        : Vector4d(view.cameraWorld.x + f[1], view.cameraWorld.y + f[2], view.cameraWorld.z + f[3], 1.0);
      // The legacy light conversion derives radiance from the range alone
      // (radiance grows with range squared) and only normalises the colour,
      // so a dim and a bright light of equal reach came out identical.
      // Folding the game's brightness into the range makes radiance scale
      // linearly with it.
      const float brightness = std::min(std::max(maxColor * view.intensityScale, 1.0e-3f), 1.0e4f);
      // CRYENGINE projector (type 6): projectorMatrix (offset 80, row-major,
      // used as mul(M, pos)) is ViewProj * ScaleBias(0.5) of the light
      // frustum (CShadowUtils::GetProjectiveTexGen, FOV = 2 * frustum
      // angle). Row 3 is the depth row, so its xyz is the spot axis; the
      // part of row 0 across that axis is 0.5 / tan(half angle) times it.
      bool spot = false;
      Vector3 spotDir;
      float outerAngle = 0.0f;
      if (layout->id == D3D11TiledLightLayout::CryEngine208) {
        uint32_t type = 0;
        std::memcpy(&type, e, 4u);
        float m[16];
        std::memcpy(m, e + 80u, sizeof(m));
        const Vector3 row0(m[0], m[1], m[2]);
        const Vector3 row3(m[12], m[13], m[14]);
        const float axisLen = length(row3);
        if (type == 6u && std::isfinite(axisLen) && axisLen > 1.0e-6f) {
          spotDir = row3 * (1.0f / axisLen);
          const Vector3 across = row0 - spotDir * dot(row0, spotDir);
          const float acrossLen = length(across);
          const float tanHalf = acrossLen > 1.0e-6f ? 0.5f * axisLen / acrossLen : 0.0f;
          outerAngle = 2.0f * std::atan(tanHalf);
          spot = std::isfinite(outerAngle) && outerAngle > 0.02f && outerAngle < 3.12f;
        }
      }
      if (layout->id == D3D11TiledLightLayout::Hdrp224 && (hdrpType == 2u || hdrpType == 3u)) {
        float g[5];
        std::memcpy(g, e + 24u, sizeof(g));  // angleScale, angleOffset, forward.xyz
        const Vector3 forward(g[2], g[3], g[4]);
        const float fwdLen = length(forward);
        float inner = 0.0f;
        if (std::isfinite(fwdLen) && fwdLen > 0.5f && D3D11SpotFromAngleScale(g[0], g[1], inner, outerAngle)) {
          spot = true;
          spotDir = forward * (1.0f / fwdLen);
        }
      }
      out.push_back(spot
        ? Dx11LightStateApi::makeSpot(
            D3D11SnapLightCoordinate(world.x), D3D11SnapLightCoordinate(world.y), D3D11SnapLightCoordinate(world.z),
            spotDir.x, spotDir.y, spotDir.z, f[5], f[6], f[7],
            outerAngle * 0.8f, outerAngle, radius * std::sqrt(brightness))
        : Dx11LightStateApi::makePoint(
            D3D11SnapLightCoordinate(world.x), D3D11SnapLightCoordinate(world.y), D3D11SnapLightCoordinate(world.z),
            f[5], f[6], f[7], radius * std::sqrt(brightness)));
    }

    if (sampleCount)
      *sampleCount = sampled;

    return uint32_t(out.size() - first);
  }

}
