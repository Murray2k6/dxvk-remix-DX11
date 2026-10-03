#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "rtx/dx11/dx11_light_state.h"

#include "d3d11_engine_profile.h"

#include "../util/util_matrix.h"
#include "../util/util_vector.h"

namespace dxvk {

  // Engine light records decoded from raw buffer bytes. Shared by the D3D11
  // runtime (D3D11Rtx) and the DX12 / Vulkan front end (d3d11_vk_capture.cpp),
  // which read the same engines' buffers through different APIs. Layouts and
  // sources: documentation/engine_knowledge (unreal.md, cryengine_frostbite.md,
  // unity.md, creation_source2_gamemaker.md, dx12_unreal_unity.md).

  struct D3D11LightDecodeView {
    Matrix4d viewToWorld;        // view space -> world (Creation48 positions)
    Vector4d cameraWorld;        // eye in world (camera-relative layouts)
    float    intensityScale = 1.0f;
    uint32_t maxLights = 0;
  };

  // Light identity hashes come from positions; rounding keeps a light stable
  // frame to frame despite float noise in the camera transform.
  inline float D3D11SnapLightCoordinate(double v) {
    return float(std::round(v * 2.0) * 0.5);
  }

  // Spot cone from the angle-scale form shared by Frostbite and HDRP
  // ("Moving Frostbite to PBR"): scale = 1 / (cosInner - cosOuter),
  // offset = -cosOuter * scale. Returns false for point lights (scale 0).
  bool D3D11SpotFromAngleScale(float angleScale, float angleOffset, float& innerAngle, float& outerAngle);

  // UE4.2x/UE5 ForwardLocalLightBuffer (LightGridCommon.ush): float4 x 6 per
  // local light, translated-world (camera-relative) positions. Returns the
  // number decoded; 0 when the bytes are not a light buffer.
  uint32_t D3D11DecodeUnrealLocalLights(
    const uint8_t*                bytes,
          size_t                  size,
    const D3D11LightDecodeView&   view,
          std::vector<Dx11LightDesc>& out);

  // Frostbite 3 cbPunctualLightInfo: up to 128 BaseLightInfo of 96 B,
  // camera-relative. Fewer than 2 plausible entries is not a light array (0).
  uint32_t D3D11DecodeFrostbitePunctualLights(
    const uint8_t*                bytes,
          size_t                  size,
    const D3D11LightDecodeView&   view,
          std::vector<Dx11LightDesc>& out);

  // Katana Engine (Koei Tecmo) clustered lights (bitsquid_phyre_katana_ego.md;
  // DOA6 deferred lighting PS c8589d63590629a9, DarkStarSword/3d-fixes):
  //   tCllLightPositions   Buffer<float4>: world position xyz, radius w
  //                        (the PS compares |p - lightPos|^2 with w^2, p
  //                        rebuilt in world space through mP2W);
  //   tCllLightAttributes  Buffer<float3>, 3 per light: [3i] colour,
  //                        [3i+1] falloff coefficients, [3i+2] IES axis.
  // attributeStride is the attribute element size in bytes (12 for
  // R32G32B32_FLOAT). Point lights; spot lights use separate buffers.
  uint32_t D3D11DecodeKatanaClusterLights(
    const uint8_t*                positions,
          size_t                  positionsSize,
    const uint8_t*                attributes,
          size_t                  attributesSize,
          uint32_t                attributeStride,
    const D3D11LightDecodeView&   view,
          std::vector<Dx11LightDesc>& out);

  // Katana clustered spot lights, same shader: tCllSptLightAttributes
  // Buffer<float3>, 5 per light: [5i] world position, [5i+1] cone axis,
  // [5i+2] colour, [5i+3] falloff, [5i+4] (range^2, cone offset, cone scale)
  // with cone = saturate(dot(toLight, axis) * scale + offset).
  uint32_t D3D11DecodeKatanaClusterSpotLights(
    const uint8_t*                attributes,
          size_t                  attributesSize,
          uint32_t                attributeStride,
    const D3D11LightDecodeView&   view,
          std::vector<Dx11LightDesc>& out);

  // Tiled structured-buffer layouts (Creation48, CryEngine208, Hdrp224).
  uint32_t D3D11TiledLightStride(D3D11TiledLightLayout layout);
  bool     D3D11TiledLightRequiresDynamic(D3D11TiledLightLayout layout);

  // sunDirections receives CRYENGINE's sun entries (direction of travel).
  // samples / sampleCount receive up to 3 raw entries for logging.
  uint32_t D3D11DecodeTiledLights(
          D3D11TiledLightLayout   layout,
    const uint8_t*                bytes,
          size_t                  size,
    const D3D11LightDecodeView&   view,
          std::vector<Dx11LightDesc>& out,
          std::vector<Vector3>*   sunDirections = nullptr,
          float                 (*samples)[12] = nullptr,
          uint32_t*               sampleCount = nullptr);

}
