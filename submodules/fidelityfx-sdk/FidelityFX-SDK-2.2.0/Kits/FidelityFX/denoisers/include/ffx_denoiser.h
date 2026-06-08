// This file is part of the FidelityFX SDK.
//
// Copyright (C) 2026 Advanced Micro Devices, Inc.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files(the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and /or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions :
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.

#pragma once

//------------------------------------------------------------------------------
// FFX Includes
//------------------------------------------------------------------------------

// ffxApiMessage
// ffxConfigureDescHeader
// ffxCreateContextDescHeader
// ffxDispatchDescHeader
// ffxQueryDescHeader
#include "../../api/include/ffx_api.h"
// FfxApiDimensions2D
// FfxApiFloatCoords2D
// FfxApiFloatCoords3D
// FfxApiResource
#include "../../api/include/ffx_api_types.h"

//------------------------------------------------------------------------------
// External Includes
//------------------------------------------------------------------------------

// uint32_t
// uint64_t
#include <stdint.h>

//------------------------------------------------------------------------------
// FFX Denoiser Defines
//------------------------------------------------------------------------------

#define FFX_DENOISER_VERSION_MAJOR 1
#define FFX_DENOISER_VERSION_MINOR 1
#define FFX_DENOISER_VERSION_PATCH 0

#define FFX_DENOISER_MAKE_VERSION(major, minor, patch) (((major) << 22) | ((minor) << 12) | (patch))
#define FFX_DENOISER_VERSION FFX_DENOISER_MAKE_VERSION(FFX_DENOISER_VERSION_MAJOR, FFX_DENOISER_VERSION_MINOR, FFX_DENOISER_VERSION_PATCH)

//------------------------------------------------------------------------------
// FFX Denoiser Declarations
//------------------------------------------------------------------------------

#ifdef __cplusplus
extern "C"
{
#endif // __cplusplus

//------------------------------------------------------------------------------
// FFX Denoiser Descriptions: Create Context
//------------------------------------------------------------------------------

#define FFX_API_CREATE_CONTEXT_DESC_TYPE_DENOISER FFX_API_MAKE_EFFECT_SUB_ID(FFX_API_EFFECT_ID_DENOISER, 0x01)
typedef struct ffxCreateContextDescDenoiser
{
    ffxCreateContextDescHeader header;
    uint32_t                   version;                     ///< The version of the API the application was built against. This must be set to <c>FFX_DENOISER_VERSION</c>.
    struct FfxApiDimensions2D  maxRenderSize;               ///< The maximum size that rendering will be performed at.
    ffxApiMessage              fpMessage;                   ///< A pointer to a function that can receive messages from the runtime. May be null.
    uint32_t                   mode;                        ///< An entry of <c>FfxApiDenoiserMode</c> that selects the number of signals to denoise.
    uint32_t                   flags;                       ///< Zero or a combination of values from <c>FfxApiCreateContextDenoiserFlags</c>.
} ffxCreateContextDescDenoiser;

//------------------------------------------------------------------------------
// FFX Denoiser Descriptions: Configure
//------------------------------------------------------------------------------

#define FFX_API_CONFIGURE_DESC_TYPE_DENOISER_KEYVALUE FFX_API_MAKE_EFFECT_SUB_ID(FFX_API_EFFECT_ID_DENOISER, 0x08)
typedef struct ffxConfigureDescDenoiserKeyValue
{
    ffxConfigureDescHeader header;                          ///< Header descriptor, use type FFX_API_CONFIGURE_DESC_TYPE_DENOISER_KEYVALUE.
    uint64_t               key;                             ///< Configuration key, member of the FfxApiConfigureDenoiserKey enumeration.
    uint64_t               count;                           ///< The number of elements to configure.
    const void*            data;                            ///< Pointer to an array containing the elements to configure.
} ffxConfigureDescDenoiserKeyValue;

//------------------------------------------------------------------------------
// FFX Denoiser Descriptions: Dispatch
//------------------------------------------------------------------------------

typedef struct FfxApiDenoiserSignal
{
    struct FfxApiResource input;                            ///< Input signal to be denoised.
    struct FfxApiResource output;                           ///< Resulting denoised signal.

    uint32_t _reserved[2];                                  ///< Reserved for future use.
} FfxApiDenoiserSignal;

#define FFX_API_DISPATCH_DESC_TYPE_DENOISER FFX_API_MAKE_EFFECT_SUB_ID(FFX_API_EFFECT_ID_DENOISER, 0x02)
typedef struct ffxDispatchDescDenoiser
{
    ffxDispatchDescHeader      header;
    void*                      commandList;                 ///< Command list to record upscaling rendering commands into.
                               
    struct FfxApiResource      linearDepth;                 ///< R:   Absolute linear depth values for the current frame (<c>abs(CurrentLinearDepth)</c>).
    struct FfxApiResource      motionVectors;               ///< RG:  2D motion vectors (<c>PreviousUV - CurrentUV</c>), B: Absolute linear depth delta (<c>abs(PreviousLinearDepth) - abs(CurrentLinearDepth)</c>).
    struct FfxApiResource      normals;                     ///< RG:  Octahedrally encoded normals, B: Linear Roughness, A: Material Type - See docs for more info, 
    struct FfxApiResource      specularAlbedo;              ///< RGB: Specular albedo - sqrt encoding assumed unless <c>FFX_DENOISER_DISPATCH_NON_GAMMA_ALBEDO</c> is provided. <c>sqrt(SpecularAlbedo)</c>
    struct FfxApiResource      diffuseAlbedo;               ///< RGB: Diffuse albedo (e.g., <c>BaseColor * (1 - Metalness)</c>) - sqrt encoding assumed unless <c>FFX_DENOISER_DISPATCH_NON_GAMMA_ALBEDO</c> is provided. <c>sqrt(DiffuseAlbedo)</c>
                                                            
    struct FfxApiFloatCoords3D motionVectorScale;           ///< RG:  The scale factor for transforming the 2D motion vectors into UV space. For 2D motion vectors computed as <c>PreviousUV - CurrentUV</c>, use <c>{ .x = +1.0f, .y = +1.0f }</c>. For 2D motion vectors computed as <c>PreviousNDC - CurrentNDC</c>, use <c>{ .x = +0.5f, .y = -0.5f }</c>. B: The scale factor for transforming the linear depth delta. For linear depth deltas computed as <c>abs(PreviousLinearDepth) - abs(CurrentLinearDepth)</c>, use <c>{ .z = +1.0f }</c>.
    struct FfxApiFloatCoords2D jitterOffsets;               ///< The subpixel jitter offset applied to the camera projection. (Expressed in screen pixels)
                                                            
    struct FfxApiFloatCoords3D cameraPositionDelta;         ///< The position delta of the camera since last frame (PreviousPosition - CurrentPosition).
    struct FfxApiFloatCoords3D cameraRight;                 ///< The right (left) vector of the camera in world space if using a right-handed (left-handed) coordinate system.
    struct FfxApiFloatCoords3D cameraUp;                    ///< The up vector of the camera in world space.
    struct FfxApiFloatCoords3D cameraForward;               ///< The forward vector of the camera in world space (i.e., the direction the camera is looking).
    float                      cameraAspectRatio;           ///< The aspect ratio of the camera.
    float                      cameraNear;                  ///< The view z distance to the near plane of the camera.
    float                      cameraFar;                   ///< The view z distance to the far plane of the camera.
    float                      cameraFovAngleVertical;      ///< The vertical field of view of the camera. (Expressed in radians)
                                                            
    struct FfxApiDimensions2D  renderSize;                  ///< The resolution that was used for rendering the input resources.
    float                      deltaTime;                   ///< The time, in milliseconds, since the last frame was rendered.
    uint32_t                   frameIndex;                  ///< The index of the current frame.
                                                            
    uint32_t                   flags;                       ///< Zero or a combination of values from <c>FfxApiDispatchDenoiserFlags</c>.
} ffxDispatchDescDenoiser;

#define FFX_API_DENOISER_DEBUG_VIEW_MAX_VIEWPORTS 12        ///< The maximum number of viewports that can be visualized.

#define FFX_API_DISPATCH_DESC_DEBUG_VIEW_TYPE_DENOISER FFX_API_MAKE_EFFECT_SUB_ID(FFX_API_EFFECT_ID_DENOISER, 0x0c)
typedef struct ffxDispatchDescDenoiserDebugView
{
    ffxDispatchDescHeader                header;
    struct FfxApiResource                output;            ///< Target output resource for debug visualization.
    struct FfxApiDimensions2D            outputSize;        ///< The resolution of the output resource.
    uint32_t                             mode;              ///< An entry of <c>FfxApiDenoiserDebugViewMode</c> that selects the mode used for visualization.
    uint32_t                             viewportIndex;     ///< The index of the viewport to visualize when <c>FFX_API_DENOISER_DEBUG_VIEW_MODE_FULLSCREEN_VIEWPORT</c> mode is active. Clamped between 0 and <c>FFX_API_DENOISER_DEBUG_VIEW_MAX_VIEWPORTS - 1</c>.
} ffxDispatchDescDenoiserDebugView;

#define FFX_API_DISPATCH_DESC_INPUT_DOMINANT_LIGHT_TYPE_DENOISER FFX_API_MAKE_EFFECT_SUB_ID(FFX_API_EFFECT_ID_DENOISER, 0x07)
typedef struct ffxDispatchDescDenoiserInputDominantLight    ///< Requires <c>FFX_DENOISER_ENABLE_DOMINANT_LIGHT</c> to be set in the create flags.
{
    ffxDispatchDescHeader       header;
    struct FfxApiDenoiserSignal dominantLightVisibility;    ///< Dominant light visibility signal in/out, input should be described as distance to occluder (0 -> FP16_MAX).
    struct FfxApiFloatCoords3D  dominantLightDirection;     ///< Dominant light direction. (from light source to target) 
    struct FfxApiFloatCoords3D  dominantLightEmission;      ///< Dominant light emission. (i.e. <c>LightColor * LightIntensity</c>)
} ffxDispatchDescDenoiserInputDominantLight;

#define FFX_API_DISPATCH_DESC_INPUT_4_SIGNALS_TYPE_DENOISER FFX_API_MAKE_EFFECT_SUB_ID(FFX_API_EFFECT_ID_DENOISER, 0x03)
typedef struct ffxDispatchDescDenoiserInput4Signals         ///< Requires FFX_DENOISER_MODE_4_SIGNALS to be set in the create flags.
{
    ffxDispatchDescHeader       header;
    struct FfxApiDenoiserSignal indirectSpecularRadiance;   ///< RGB: Indirect specular radiance signal in/out, A: Specular ray length (in-only).
    struct FfxApiDenoiserSignal indirectDiffuseRadiance;    ///< RGB: Indirect diffuse radiance signal in/out.
    struct FfxApiDenoiserSignal directSpecularRadiance;     ///< RGB: Direct specular radiance signal in/out.
    struct FfxApiDenoiserSignal directDiffuseRadiance;      ///< RGB: Direct diffuse radiance signal in/out.
} ffxDispatchDescDenoiserInput4Signals;

#define FFX_API_DISPATCH_DESC_INPUT_2_SIGNALS_TYPE_DENOISER FFX_API_MAKE_EFFECT_SUB_ID(FFX_API_EFFECT_ID_DENOISER, 0x04)
typedef struct ffxDispatchDescDenoiserInput2Signals         ///< Requires FFX_DENOISER_MODE_2_SIGNALS to be set in the create flags.
{
    ffxDispatchDescHeader       header;
    struct FfxApiDenoiserSignal specularRadiance;           ///< RGB: Specular radiance signal in/out, A: Specular ray length (in-only).
    struct FfxApiDenoiserSignal diffuseRadiance;            ///< RGB: Diffuse radiance signal in/out.
} ffxDispatchDescDenoiserInput2Signals;

#define FFX_API_DISPATCH_DESC_INPUT_1_SIGNAL_TYPE_DENOISER FFX_API_MAKE_EFFECT_SUB_ID(FFX_API_EFFECT_ID_DENOISER, 0x05)
typedef struct ffxDispatchDescDenoiserInput1Signal          ///< Requires FFX_DENOISER_MODE_1_SIGNAL to be set in the create flags.
{
    ffxDispatchDescHeader       header;
    struct FfxApiDenoiserSignal radiance;                   ///< RGB: Composited radiance signal in/out, A: Specular ray length (in-only).
    struct FfxApiResource       fusedAlbedo;                ///< RGB: max(specularAlbedo, diffuseAlbedo) - sqrt encoding assumed unless <c>FFX_DENOISER_DISPATCH_NON_GAMMA_ALBEDO</c> is provided. <c>sqrt(FusedAlbedo)</c>
} ffxDispatchDescDenoiserInput1Signal;

//------------------------------------------------------------------------------
// FFX Denoiser Descriptions: Query
//------------------------------------------------------------------------------

#define FFX_API_QUERY_DESC_TYPE_DENOISER_GET_DEFAULT_KEYVALUE FFX_API_MAKE_EFFECT_SUB_ID(FFX_API_EFFECT_ID_DENOISER, 0x0b)
typedef struct ffxQueryDescDenoiserGetDefaultKeyValue
{
    ffxQueryDescHeader     header;                          ///< Header descriptor, use type FFX_API_QUERY_DESC_TYPE_DENOISER_GET_DEFAULT_KEYVALUE.
    uint64_t               key;                             ///< Configuration key, member of the FfxApiConfigureDenoiserKey enumeration.
    uint64_t               count;                           ///< The number of elements to query.
    void*                  data;                            ///< Pointer to an array for storing the querried elements.
} ffxQueryDescDenoiserGetDefaultKeyValue;

#define FFX_API_QUERY_DESC_TYPE_DENOISER_GPU_MEMORY_USAGE FFX_API_MAKE_EFFECT_SUB_ID(FFX_API_EFFECT_ID_DENOISER, 0x09)
typedef struct ffxQueryDescDenoiserGetGPUMemoryUsage        ///< If a valid FfxContext is passed into ffx::Query with this structure, current context usage will be reported. If no context is passed, the memory usage will be estimated based on the provided parameters.
{
    ffxQueryDescHeader        header;
    void*                     device;                       ///< For DX12: pointer to ID3D12Device.
    struct FfxApiDimensions2D maxRenderSize;                ///< Maximum size that rendering will be performed at.
    uint32_t                  mode;                         ///< An entry of <c>FfxApiDenoiserMode</c> that selects the number of signals to denoise.
    uint32_t                  flags;                        ///< Zero or a combination of values from <c>FfxApiCreateContextDenoiserFlags</c>.

    struct FfxApiEffectMemoryUsage* gpuMemoryUsage;         ///< A pointer to a <c>FfxApiEffectMemoryUsage</c> structure that will hold the GPU memory usage.
} ffxQueryDescDenoiserGetGPUMemoryUsage;

#define FFX_API_QUERY_DESC_TYPE_DENOISER_GET_VERSION FFX_API_MAKE_EFFECT_SUB_ID(FFX_API_EFFECT_ID_DENOISER, 0x0a)
typedef struct ffxQueryDescDenoiserGetVersion               ///< If a valid FfxContext is passed into ffx::Query with this structure, info based on current context will be reported. If no context is passed, the info will be evaluated based on the provided parameters.
{
    ffxConfigureDescHeader  header;
    void*                   device;                         ///< For DX12: pointer to ID3D12Device.

    uint32_t*               major;                          ///< A pointer to a <c>uint32_t</c> variable that will hold the major version number.
    uint32_t*               minor;                          ///< A pointer to a <c>uint32_t</c> variable that will hold the minor version number.
    uint32_t*               patch;                          ///< A pointer to a <c>uint32_t</c> variable that will hold the patch version number.
} ffxQueryDescDenoiserGetVersion;

//------------------------------------------------------------------------------
// FFX Denoiser Enums
//------------------------------------------------------------------------------

typedef enum FfxApiCreateContextDenoiserFlags
{
    FFX_DENOISER_ENABLE_DEBUGGING      = (1 << 0),          ///< A bit indicating that debug features may be enabled, memory consumption may increase.
    FFX_DENOISER_ENABLE_DOMINANT_LIGHT = (1 << 1),          ///< A bit indicating that dominant light visibility denoising should be enabled. This requires the dominant light direction and emission to be provided.
} FfxApiCreateContextDenoiserFlags;

typedef enum FfxApiConfigureDenoiserKey
{
    FFX_API_CONFIGURE_DENOISER_KEY_CROSS_BILATERAL_NORMAL_STRENGTH = 1, ///< Override the strength of the cross bilateral normal term. A single float scalar.
    FFX_API_CONFIGURE_DENOISER_KEY_STABILITY_BIAS                  = 2, ///< Override the bias of the temporal accumulation to be more stable but less responsive. A single float scalar.
    FFX_API_CONFIGURE_DENOISER_KEY_MAX_RADIANCE                    = 3, ///< Override the maximum radiance value. A single float scalar.
    FFX_API_CONFIGURE_DENOISER_KEY_RADIANCE_CLIP_STD_K             = 4, ///< Override the standard deviation K value used for radiance clipping. A single float scalar.
    FFX_API_CONFIGURE_DENOISER_KEY_GAUSSIAN_KERNEL_RELAXATION      = 5, ///< Override the Gaussian kernel relaxation factor. A single float scalar.
    FFX_API_CONFIGURE_DENOISER_KEY_DISOCCLUSION_THRESHOLD          = 6, ///< Override the discocclusion threshold used for depth comparisons during temporal reprojection. A single float scalar.
} FfxApiConfigureDenoiserKey;

typedef enum FfxApiDenoiserDebugViewMode
{
    FFX_API_DENOISER_DEBUG_VIEW_MODE_OVERVIEW            = 0,
    FFX_API_DENOISER_DEBUG_VIEW_MODE_FULLSCREEN_VIEWPORT = 1,
} FfxApiDenoiserDebugViewMode;

typedef enum FfxApiDenoiserMode
{
    FFX_DENOISER_MODE_4_SIGNALS = 0,                        ///< The denoiser expects 4 split radiance signals as input to perform denoising on. (Direct Specular, Direct Diffuse, Indirect Specular, Indirect Diffuse)
    FFX_DENOISER_MODE_2_SIGNALS = 1,                        ///< The denoiser expects 2 fused radiance signals as input to perform denoising on. (Specular, Diffuse)
    FFX_DENOISER_MODE_1_SIGNAL  = 2,                        ///< The denoiser expects 1 fused radiance signal as input to perform denoising on. (Composited)
} FfxApiDenoiserMode;

typedef enum FfxApiDispatchDenoiserFlags
{
    FFX_DENOISER_DISPATCH_RESET            = (1 << 0),      ///< A bit indicating that the we need to reset history accumulation.
    FFX_DENOISER_DISPATCH_NON_GAMMA_ALBEDO = (1 << 1),      ///< A bit indicating that the input albedo textures are not gamma encoded.
} FfxApiDispatchDenoiserFlags;

#ifdef __cplusplus
}
#endif // __cplusplus
