#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <remix/remix_vk_frontend.h>

namespace dxvk {

  struct D3D11VkPipeline;

  /**
   * \brief What a pixel shader does with one sampled texture
   *
   * The SPIR-V counterpart of the DX11 path's DXBC texture-decode analysis
   * (D3D11CommonShader::GetTextureDecode): a sample whose result is
   * unpacked with x * 2 - 1 is a tangent-space normal map, the selected
   * components give its encoding; 1 - x on a channel is smoothness; a
   * sample that reaches the colour output is the albedo.
   */
  struct D3D11VkTextureRole {
    uint32_t set      = ~0u;     // DescriptorSet
    uint32_t binding  = ~0u;     // Binding
    uint32_t element  = ~0u;     // array element, ~0u when indexed dynamically
    bool     albedo   = false;
    bool     normal   = false;
    // NormalEncoding values used by LegacyMaterialData::normalEncoding:
    // 2 RGB, 3 XY, 4 DXT5nm (wy), 5 XY swapped.
    uint8_t  normalEncoding = 0;
    int8_t   smoothnessChannel = -1;
    // Roughness used as is (Unreal ORM green, a "roughness" map).
    int8_t   roughnessChannel = -1;
    // Metallic channel: the factor of mix(dielectric F0 ~0.04, albedo, m),
    // or from the name (Unity mask map R, Unreal ORM B, "metallic").
    int8_t   metallicChannel = -1;
    // Emission: added to the lit colour without being lit itself, or
    // named emissive / glow / illum.
    bool     emissive = false;
    // The image variable's debug name (OpName), empty when stripped. Read
    // with the DX11 path's reflection-name rules.
    std::string name;
  };

  struct D3D11VkShaderAnalysis {
    // Input location the samples' coordinates come from (most samples win).
    uint32_t texcoordLocation = ~0u;
    // Input location that multiplies a sample: the vertex colour.
    uint32_t vertexColorLocation = ~0u;
    std::vector<D3D11VkTextureRole> textures;
    // Any normal-map unpack, with the first one's encoding: used when the
    // texture behind it cannot be named (bindless heaps, vkd3d-proton).
    bool     sampledNormal = false;
    uint8_t  normalEncoding = 0;
    int8_t   smoothnessChannel = -1;
  };

  D3D11VkShaderAnalysis D3D11VkAnalyzePixelShader(const uint32_t* code, size_t words);

  /**
   * \brief Capture layout of a patched pre-raster stage
   */
  struct D3D11VkCaptureLayout {
    uint32_t stride = 0;
    bool     texcoord = false;
    uint32_t texcoordOffset = 0;
    bool     color = false;
    uint32_t colorOffset = 0;
    uint32_t colorComponents = 0;
  };

  /**
   * \brief Adds capture decorations to a pre-raster stage
   *
   * Transform feedback decorations (SPIR-V 1.0+ rules: XfbBuffer/XfbStride
   * on the variable, Offset on the variable or block member, Capability
   * TransformFeedback, ExecutionMode Xfb): gl_Position at byte 0, then the
   * output at texcoordLocation, then the output at colorLocation.
   * \returns \c false when the module cannot be patched (no position
   *    output, existing transform feedback, unsupported layout)
   */
  bool D3D11VkPatchSpirvForCapture(
    const uint32_t*               code,
          size_t                  words,
          uint32_t                texcoordLocation,
          uint32_t                colorLocation,
          std::vector<uint32_t>&  patched,
          D3D11VkCaptureLayout&   layout);

  /**
   * \brief Builds the terrain bake vertex stage
   *
   * Before every return of the entry point, the clip position is replaced
   * by M * position, M's rows read from four new vec4 inputs at
   * matrixLocation .. + 3 (past the highest input location in use).
   * \returns \c false for anything but a vertex shader with a float4
   *    position, or when the inputs would pass location 15
   */
  bool D3D11VkPatchSpirvForBake(
    const uint32_t*               code,
          size_t                  words,
          std::vector<uint32_t>&  patched,
          uint32_t&               matrixLocation);

  /**
   * \brief Builds the generated-commands capture vertex stage
   *
   * From the capture vertex stage: every load of push-constant bytes inside
   * one of the command stream's constant ranges reads them from the stream
   * instead - command gl_DrawID (DrawIndex) at the uint64 device address
   * held in the first 8 bytes of constants[0]'s range, offset by
   * gl_DrawID x stride (remix_vkfe_capture_plan::indirect_spirv).
   * \returns \c false for anything but a vertex shader, or push-constant
   *    access the rewrite cannot follow inside the ranges
   */
  bool D3D11VkPatchSpirvForIndirect(
    const uint32_t*                     code,
          size_t                        words,
    const remix_vkfe_indirect_constant* constants,
          uint32_t                      constantCount,
          uint32_t                      stride,
          std::vector<uint32_t>&        patched);

  /**
   * \brief Builds a pipeline's capture variant and material analysis
   *
   * Sets pipeline.captureSupported, the capture fields and
   * pipeline.pixelAnalysis.
   */
  void D3D11VkBuildCapturePlan(D3D11VkPipeline& pipeline);

}
