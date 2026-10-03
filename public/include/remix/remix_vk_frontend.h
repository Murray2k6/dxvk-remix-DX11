/*
 * Remix Vulkan front-end interface.
 *
 * Remix's DX11 runtime (d3d11.dll) also path traces games that render
 * through Vulkan. Both go through the Remix Vulkan layer
 * (remix_vk_layer.dll), which calls these hooks from its vkCreateInstance /
 * vkCreateDevice / vkCreateGraphicsPipelines / vkCmdDraw* / vkQueueSubmit /
 * vkQueuePresentKHR entry points:
 *
 *   - Native Vulkan games.
 *   - DX12 games, through vkd3d-proton (Remix's d3d12.dll next to the game),
 *     whose Vulkan stream passes through the layer. Remix's vkd3d patch adds
 *     what only exists at the D3D12 level - root CBV bytes and descriptor
 *     heap SRVs - per draw (see "DX12 draw annotation" below).
 *
 * Remix renders on the game's VkDevice (no second device, no copies). The
 * caller lets Remix extend the instance and device create infos, then hands
 * over the created handles. Every handle stays owned by the game.
 *
 * Plain C, so vkd3d-proton (C, built with MinGW) and the layer can include
 * it. Entry point exported from Remix's d3d11.dll:
 *
 *   remix_vkfe_result remix_vkfe_get_api(uint32_t version, const remix_vkfe_api** api);
 */

#ifndef REMIX_VK_FRONTEND_H
#define REMIX_VK_FRONTEND_H

#include <stdint.h>
#include <vulkan/vulkan.h>

#ifdef __cplusplus
extern "C" {
#endif

#define REMIX_VKFE_VERSION 1u
#define REMIX_VKFE_ENTRY_POINT "remix_vkfe_get_api"

typedef enum remix_vkfe_result {
  REMIX_VKFE_OK = 0,
  /* Remix cannot run here; the caller continues without Remix. The reason
   * is in Remix's log. */
  REMIX_VKFE_UNSUPPORTED = 1,
  REMIX_VKFE_BAD_ARGUMENT = 2,
  REMIX_VKFE_VERSION_MISMATCH = 3,
} remix_vkfe_result;

typedef enum remix_vkfe_frontend {
  REMIX_VKFE_FRONTEND_DX12_VKD3D = 1,
  REMIX_VKFE_FRONTEND_VULKAN_LAYER = 2,
} remix_vkfe_frontend;

/* Opaque Remix objects. */
typedef struct remix_vkfe_instance_t* remix_vkfe_instance;
typedef struct remix_vkfe_device_t*   remix_vkfe_device;
typedef struct remix_vkfe_pending_t*  remix_vkfe_pending;

/* ------------------------------------------------------------------ */
/* Instance                                                             */
/* ------------------------------------------------------------------ */

typedef struct remix_vkfe_instance_request {
  remix_vkfe_frontend          frontend;
  /* The game's (or vkd3d's) create info. */
  const VkInstanceCreateInfo*  create_info;
} remix_vkfe_instance_request;

typedef struct remix_vkfe_instance_plan {
  /* Create info to use instead: extensions Remix needs added, apiVersion
   * raised to at least 1.3. Owned by Remix until finish_instance. */
  const VkInstanceCreateInfo*  create_info;
  remix_vkfe_pending           pending;
} remix_vkfe_instance_plan;

/* ------------------------------------------------------------------ */
/* Device                                                               */
/* ------------------------------------------------------------------ */

typedef struct remix_vkfe_device_request {
  remix_vkfe_instance          instance;
  VkPhysicalDevice             physical_device;
  const VkDeviceCreateInfo*    create_info;
} remix_vkfe_device_request;

typedef struct remix_vkfe_device_plan {
  /* Create info to use instead: Remix's extensions, feature bits and one
   * extra queue per family Remix needs. Valid until finish_device. */
  const VkDeviceCreateInfo*    create_info;
  remix_vkfe_pending           pending;
} remix_vkfe_device_plan;

/* ------------------------------------------------------------------ */
/* Shaders and pipelines                                                */
/* ------------------------------------------------------------------ */

typedef enum remix_vkfe_shader_format {
  REMIX_VKFE_SHADER_DXBC  = 1,  /* D3D12 SM5.1 */
  REMIX_VKFE_SHADER_DXIL  = 2,  /* D3D12 SM6.x (DXBC container with DXIL/PSV0) */
  REMIX_VKFE_SHADER_SPIRV = 3,  /* native Vulkan */
} remix_vkfe_shader_format;

typedef enum remix_vkfe_stage {
  REMIX_VKFE_STAGE_VERTEX   = 0,
  REMIX_VKFE_STAGE_HULL     = 1,
  REMIX_VKFE_STAGE_DOMAIN   = 2,
  REMIX_VKFE_STAGE_GEOMETRY = 3,
  REMIX_VKFE_STAGE_PIXEL    = 4,
  REMIX_VKFE_STAGE_MESH     = 5,
  REMIX_VKFE_STAGE_TASK     = 6,
  REMIX_VKFE_STAGE_COUNT    = 7,
} remix_vkfe_stage;

typedef struct remix_vkfe_shader {
  remix_vkfe_shader_format     format;
  const void*                  code;
  size_t                       size;
  const char*                  entry_point;   /* SPIR-V only; may be NULL */
} remix_vkfe_shader;

typedef struct remix_vkfe_vertex_attribute {
  uint32_t                     location;      /* SPIR-V location / D3D input slot order */
  const char*                  semantic;      /* D3D12 semantic name, NULL for Vulkan */
  uint32_t                     semantic_index;
  uint32_t                     binding;       /* vertex buffer slot */
  VkFormat                     format;
  uint32_t                     offset;
} remix_vkfe_vertex_attribute;

typedef struct remix_vkfe_vertex_binding {
  uint32_t                     binding;
  uint32_t                     stride;        /* 0 = dynamic, taken from the draw */
  uint32_t                     per_instance;
  uint32_t                     divisor;
} remix_vkfe_vertex_binding;

/* A push-constant range an indirect command stream sets per command
 * (vkd3d-proton: a D3D12 ExecuteIndirect command signature's CONSTANT /
 * INCREMENTING_CONSTANT arguments, executed with device-generated
 * commands). */
#define REMIX_VKFE_INDIRECT_COMMAND_INDEX 0xffffffffu
#define REMIX_VKFE_MAX_INDIRECT_CONSTANTS 4u

typedef struct remix_vkfe_indirect_constant {
  uint32_t                     push_offset;     /* bytes into the push constants */
  uint32_t                     size;            /* bytes */
  /* Bytes into each command; REMIX_VKFE_INDIRECT_COMMAND_INDEX: the value is
   * the command's index (D3D12 INCREMENTING_CONSTANT, one 32-bit value). */
  uint32_t                     command_offset;
} remix_vkfe_indirect_constant;

typedef struct remix_vkfe_pipeline_desc {
  /* The game's pipeline handle: VkPipeline, or vkd3d's PSO pointer. Draws
   * name the pipeline by this key. */
  uint64_t                     key;
  remix_vkfe_shader            stages[REMIX_VKFE_STAGE_COUNT];
  uint32_t                     attribute_count;
  const remix_vkfe_vertex_attribute* attributes;
  uint32_t                     binding_count;
  const remix_vkfe_vertex_binding*   bindings;
  /* VK_PRIMITIVE_TOPOLOGY_MAX_ENUM: set per draw (dynamic state); Remix
   * then counts captured vertices on the GPU. */
  VkPrimitiveTopology          topology;
  VkCullModeFlags              cull_mode;
  VkFrontFace                  front_face;
  uint32_t                     depth_test;
  uint32_t                     depth_write;
  VkCompareOp                  depth_compare;
  uint32_t                     blend_enable;  /* render target 0 */
  VkBlendFactor                src_color_blend;
  VkBlendFactor                dst_color_blend;
  uint32_t                     render_target_count;
  VkFormat                     render_target_formats[8];
  VkFormat                     depth_format;
  /* The game's own stream-output declaration (D3D12 SO / Vulkan XFB), if
   * any. Remix adds capture outputs on its own variant. */
  uint32_t                     has_stream_output;
  /* Render target 0 colour write mask (0 = depth-only draw) and the rest of
   * the state Remix's pass and decal rules read. */
  VkColorComponentFlags        color_write_mask;
  uint32_t                     depth_bias_enable;
  float                        depth_bias_constant;
  float                        depth_bias_slope;
  VkBlendOp                    color_blend_op;
  VkBlendFactor                src_alpha_blend;
  VkBlendFactor                dst_alpha_blend;
  uint32_t                     alpha_to_coverage;
  /* Rasterization samples (the terrain bake needs single-sampled targets). */
  VkSampleCountFlagBits        samples;
  /* Non-zero when a dynamic state the terrain bake would have to override
   * and cannot restore is set per draw (cull mode, depth test / write,
   * stencil test, colour blend enable / write mask, vertex input). */
  uint32_t                     bake_blocking_dynamic_state;
  /* Generated-commands capture (see remix_vkfe_indirect_constant): the
   * push-constant ranges a command stream sets for this pipeline's layout,
   * and the stream's command stride. 0 constants: none. */
  uint32_t                     indirect_constant_count;
  remix_vkfe_indirect_constant indirect_constants[REMIX_VKFE_MAX_INDIRECT_CONSTANTS];
  uint32_t                     indirect_stride;
} remix_vkfe_pipeline_desc;

/* ------------------------------------------------------------------ */
/* Geometry capture                                                     */
/* ------------------------------------------------------------------ */

/*
 * Remix captures the positions (and the texture coordinate the pixel
 * shader samples with) that the game's last pre-raster stage writes, the
 * same post-VS capture the DX11 path does with stream output. The caller
 * builds a capture variant of each pipeline and replays captured draws
 * inline, in the game's own command buffer, right after the game's draw:
 *
 *   vkCmdBindPipeline(variant)
 *   vkCmdBindTransformFeedbackBuffersEXT(0, 1, &buffer, &offset, &size)
 *   vkCmdBeginTransformFeedbackEXT(0, 0, NULL, NULL)
 *   <the game's draw call, same arguments>
 *   vkCmdEndTransformFeedbackEXT(0, 0, NULL, NULL)
 *   vkCmdBindPipeline(game's pipeline)
 *
 * Bound descriptors, push constants, vertex/index buffers and dynamic state
 * are the game's, so the variant computes exactly what the game rendered.
 * The variant is the game's pipeline with:
 *   - stage `stage` replaced by the SPIR-V in the plan (transform feedback
 *     decorations added: position at offset 0, texcoord at 16),
 *   - rasterizerDiscardEnable = VK_TRUE.
 * vkd3d-proton passes the SPIR-V it compiled from DXIL/DXBC, so one
 * analysis serves both front ends.
 */
/*
 * Terrain bake. Splat-blended terrain (several colour layers mixed in the
 * pixel shader) is baked the way the DX11 path bakes it: the game's own
 * draw - its shaders and bindings, so its layer blending - is replayed into
 * Remix's top-down terrain cascades, and Remix samples the result. The bake
 * variant is the game's pipeline with:
 *   - the vertex stage replaced by bake_spirv, which multiplies the clip
 *     position by a matrix read from four vec4 inputs at locations
 *     bake_matrix_location .. + 3; the caller feeds them from a vertex
 *     binding of its choice with stride 0 and VK_FORMAT_R32G32B32A32_SFLOAT
 *     attributes at offsets 0, 16, 32, 48;
 *   - cull mode NONE, depth / stencil tests off, blending off, rasterizer
 *     discard off, viewport and scissor dynamic.
 * The caller records it as described at remix_vkfe_draw_capture::bake.
 */
typedef struct remix_vkfe_capture_plan {
  uint32_t                     supported;
  remix_vkfe_stage             stage;
  /* Remix-owned until on_pipeline_destroy. */
  const uint32_t*              spirv;
  size_t                       spirv_size;    /* bytes */
  uint32_t                     stride;        /* bytes per captured vertex */
  /* Terrain bake variant (vertex stage); NULL when the pipeline cannot be
   * baked. Remix-owned until on_pipeline_destroy. */
  const uint32_t*              bake_spirv;
  size_t                       bake_spirv_size;
  uint32_t                     bake_matrix_location;
  /* Generated-commands capture variant (same stage as spirv): the capture
   * variant whose reads of the desc's indirect_constants come from the
   * command stream - command gl_DrawID at the device address the caller
   * pushes, as a little-endian uint64, into the first 8 bytes of
   * indirect_constants[0]'s push range. NULL: such draws are not captured.
   * Remix-owned until on_pipeline_destroy. */
  const uint32_t*              indirect_spirv;
  size_t                       indirect_spirv_size;
} remix_vkfe_capture_plan;

#define REMIX_VKFE_MAX_BAKE_CASCADES 16
#define REMIX_VKFE_MAX_BAKE_VIEWS    9

typedef struct remix_vkfe_draw_capture {
  /* Non-zero: replay the draw into this range as described above. */
  uint32_t                     capture;
  VkBuffer                     buffer;
  VkDeviceSize                 offset;
  VkDeviceSize                 size;
  /* Indirect draws (vertex count known only to the GPU): end the replay
   * with vkCmdEndTransformFeedbackEXT(0, 1, &counter_buffer,
   * &counter_offset) so the byte count is written; Remix reads it once the
   * frame's GPU work has finished. VK_NULL_HANDLE for direct draws. */
  VkBuffer                     counter_buffer;
  VkDeviceSize                 counter_offset;

  /* Terrain bake (see remix_vkfe_capture_plan). Non-zero: after the game's
   * draw, end the render pass instance, and
   *   - begin one on bake_views (the colour attachments of the game's
   *     subpass in order, then its depth attachment if it has one), all in
   *     VK_IMAGE_LAYOUT_GENERAL, loaded and stored, bake_extent in size;
   *   - if bake_clear, clear colour attachment bake_target to zero;
   *   - bind the bake variant and, per cascade c < bake_cascade_count, set
   *     bake_viewports[c] / bake_scissors[c], bind bake_matrix_buffer at
   *     bake_matrix_offsets[c] to the matrix binding, and replay the draw;
   *   - end it, resume the game's render pass instance and restore the
   *     game's pipeline, viewports, scissors and vertex binding.
   * Unsupported by the caller (multi-subpass pass, secondary command
   * buffer): skip it; Remix then treats the draw as unbaked. */
  uint32_t                     bake;
  uint32_t                     bake_clear;
  uint32_t                     bake_target;
  uint32_t                     bake_view_count;
  VkImageView                  bake_views[REMIX_VKFE_MAX_BAKE_VIEWS];
  VkExtent2D                   bake_extent;
  uint32_t                     bake_cascade_count;
  VkViewport                   bake_viewports[REMIX_VKFE_MAX_BAKE_CASCADES];
  VkRect2D                     bake_scissors[REMIX_VKFE_MAX_BAKE_CASCADES];
  VkBuffer                     bake_matrix_buffer;
  VkDeviceSize                 bake_matrix_offsets[REMIX_VKFE_MAX_BAKE_CASCADES];
  /* Changes whenever Remix recreated bake images: objects the caller made
   * around earlier bake_views (framebuffers) must not be used again. */
  uint64_t                     bake_generation;

  /* Generated-commands draws (draw_desc.generated_commands): after the
   * render pass instance the draw is in ends (or at the end of the command
   * buffer), copy args_copy_size bytes of the command stream (from
   * indirect_buffer / indirect_offset) to args_copy_buffer at
   * args_copy_offset, and the 4-byte command count (when the draw has a
   * count buffer) to count_copy_buffer at count_copy_offset; then make the
   * copies visible to the host. Remix splits the capture per command with
   * them once the frame's GPU work has finished. */
  VkBuffer                     args_copy_buffer;
  VkDeviceSize                 args_copy_offset;
  VkDeviceSize                 args_copy_size;
  VkBuffer                     count_copy_buffer;
  VkDeviceSize                 count_copy_offset;

  /* GPU-only buffer bytes Remix reads back (e.g. engine material tables in a
   * default heap): copy each like the command stream above, whether or not
   * the draw is captured. */
  uint32_t                     buffer_copy_count;
  struct {
    VkBuffer                   src;
    VkDeviceSize               src_offset;
    VkDeviceSize               size;
    VkBuffer                   dst;
    VkDeviceSize               dst_offset;
  }                            buffer_copies[2];
} remix_vkfe_draw_capture;

/* ------------------------------------------------------------------ */
/* Draws                                                                */
/* ------------------------------------------------------------------ */

typedef enum remix_vkfe_binding_kind {
  REMIX_VKFE_BINDING_CONSTANT_BUFFER = 1,  /* D3D12 CBV / Vulkan UBO          */
  REMIX_VKFE_BINDING_STORAGE_BUFFER  = 2,  /* D3D12 SRV/UAV buffer / SSBO      */
  REMIX_VKFE_BINDING_TEXTURE         = 3,  /* sampled image                    */
  REMIX_VKFE_BINDING_PUSH_CONSTANTS  = 4,  /* D3D12 root constants / push consts */
  /* The shader-visible CBV/SRV/UAV heap (SM 6.6 ResourceDescriptorHeap[]):
   * host_data is vkd3d's heap handle for remix_vkd3d_heap_view, size the
   * descriptor count. */
  REMIX_VKFE_BINDING_DESCRIPTOR_HEAP = 5,
} remix_vkfe_binding_kind;

typedef struct remix_vkfe_binding {
  remix_vkfe_binding_kind      kind;
  uint32_t                     stage_mask;    /* 1 << remix_vkfe_stage */
  /* D3D12: register and space (b3, space0 -> 3, 0).
   * Vulkan: binding and set. */
  uint32_t                     slot;
  uint32_t                     space;
  /* Element of a descriptor array (Vulkan); 0 for single descriptors and
   * for D3D12 registers (each register is its own slot). */
  uint32_t                     array_element;
  /* Buffers. host_data is set when the bytes are CPU visible (upload heap,
   * host-visible memory, push constants); otherwise Remix reads a GPU copy
   * one frame late. */
  VkBuffer                     buffer;
  VkDeviceSize                 offset;
  VkDeviceSize                 size;
  VkDeviceAddress              address;
  const void*                  host_data;
  /* Textures. */
  VkImage                      image;
  VkImageView                  view;
  VkFormat                     format;
  VkExtent3D                   extent;
  uint32_t                     mip_levels;
  uint32_t                     array_layers;
  VkImageLayout                layout;
  /* The image was created with VK_IMAGE_USAGE_TRANSFER_SRC_BIT (Remix reads
   * one mip back for the texture's content hash). */
  uint32_t                     transfer_src;
  /* host_data's bytes at this address may change between draws of a frame
   * (vkd3d-proton's shadow of a default-heap constant buffer, refilled by
   * each CopyBufferRegion): Remix copies them per draw instead of sharing
   * one copy per address. */
  uint32_t                     host_data_per_draw;
  /* The bytes are in persistently mapped memory the application may write
   * between recording and submission (vulkan_layer_capture.md 1.10: "UBO /
   * push-constant bytes read at vkQueueSubmit"). host_data is NULL here;
   * the caller hands the bytes over at submission (on_submit_bytes). */
  uint32_t                     host_data_at_submit;
} remix_vkfe_binding;

typedef struct remix_vkfe_vertex_buffer {
  uint32_t                     binding;
  VkBuffer                     buffer;
  VkDeviceSize                 offset;
  VkDeviceSize                 size;
  uint32_t                     stride;
  const void*                  host_data;     /* when CPU visible */
} remix_vkfe_vertex_buffer;

typedef struct remix_vkfe_attachment {
  VkImage                      image;
  VkImageView                  view;
  VkFormat                     format;
  VkExtent3D                   extent;
  uint32_t                     base_layer;
  /* Whether this image will be presented (back buffer). */
  uint32_t                     is_swapchain_image;
} remix_vkfe_attachment;

typedef struct remix_vkfe_draw_desc {
  uint64_t                     pipeline_key;
  VkCommandBuffer              command_buffer;
  /* Arguments. For an indirect draw, indirect_buffer is set and count/
   * instance values are read by Remix on the GPU. */
  uint32_t                     indexed;
  uint32_t                     vertex_or_index_count;
  uint32_t                     instance_count;
  uint32_t                     first_vertex_or_index;
  int32_t                      vertex_offset;
  uint32_t                     first_instance;
  VkBuffer                     indirect_buffer;
  VkDeviceSize                 indirect_offset;
  uint32_t                     indirect_draw_count;
  uint32_t                     indirect_stride;
  VkBuffer                     indirect_count_buffer;
  VkDeviceSize                 indirect_count_offset;

  VkBuffer                     index_buffer;
  VkDeviceSize                 index_offset;
  VkIndexType                  index_type;
  const void*                  index_host_data;

  uint32_t                     vertex_buffer_count;
  const remix_vkfe_vertex_buffer* vertex_buffers;

  uint32_t                     binding_count;
  const remix_vkfe_binding*    bindings;

  uint32_t                     render_target_count;
  remix_vkfe_attachment        render_targets[8];
  remix_vkfe_attachment        depth;
  VkViewport                   viewport;
  VkRect2D                     scissor;

  /* Generated-commands draw (remix_vklayer_annotate_indirect): one command
   * per stream entry; indirect_buffer / indirect_offset point at command 0,
   * indirect_stride is the command stride, indirect_draw_count the maximum
   * command count, indirect_address the device address of command 0, and
   * indirect_args_offset the offset of the draw arguments in a command. */
  uint32_t                     generated_commands;
  VkDeviceAddress              indirect_address;
  uint32_t                     indirect_args_offset;
} remix_vkfe_draw_desc;

/* ------------------------------------------------------------------ */
/* Submission and present                                               */
/* ------------------------------------------------------------------ */

typedef struct remix_vkfe_submit_desc {
  VkQueue                      queue;
  uint32_t                     command_buffer_count;
  const VkCommandBuffer*       command_buffers;
} remix_vkfe_submit_desc;

typedef struct remix_vkfe_present_desc {
  /* The queue the game presents on. During on_present the caller holds
   * this queue's external synchronization, so Remix may submit to it. */
  VkQueue                      queue;
  VkSwapchainKHR               swapchain;
  uint32_t                     image_index;
  VkImage                      image;
  VkFormat                     format;
  VkExtent2D                   extent;
  VkColorSpaceKHR              color_space;
  /* Binary semaphores the present waits on, signalled by the game's
   * rendering. */
  uint32_t                     wait_semaphore_count;
  const VkSemaphore*           wait_semaphores;
} remix_vkfe_present_desc;

/*
 * UI preservation. Remix writes its frame over the whole image the game
 * presents, so the game's HUD would be lost. The caller copies the frame as
 * it is right before the UI is drawn - after the last full-screen pass into
 * an image the size of the swap chain - into a Remix-owned snapshot (it
 * splits the render pass there: end, copy, resume with LOAD). At present
 * Remix keeps the game's pixels where the final frame differs from that
 * snapshot (the UI) and its own frame everywhere else.
 */
typedef struct remix_vkfe_ui_snapshot {
  VkImage                      image;
  /* The snapshot stays in this layout; copy into it with this layout. */
  VkImageLayout                layout;
} remix_vkfe_ui_snapshot;

/*
 * UI layer. A HUD drawn before the game's last full-screen pass (FXAA or a
 * sharpen after the UI) is part of the snapshot, so the snapshot cannot
 * separate it. The caller then also replays each UI draw - into an image of
 * the swap chain's size and format, blended, without depth testing, not a
 * full-screen pass - into Remix's layer: split the render pass instance as
 * for the terrain bake, begin one on color_view (and depth_view as the depth
 * attachment when the instance has one), clear colour to zero when `clear`,
 * draw with the UI variant, and resume. The UI variant is the game's
 * pipeline with alpha blending set to src ONE / dst ONE_MINUS_SRC_ALPHA
 * (straight or premultiplied alpha colour blending) or src ZERO / dst ONE
 * (additive), so the layer holds premultiplied UI with its coverage in
 * alpha. A frame with a UI layer composites it over Remix's frame instead
 * of using the snapshot.
 */
typedef struct remix_vkfe_ui_layer {
  VkImageView                  color_view;    /* the target's format, GENERAL */
  VkImageView                  depth_view;    /* scratch, or VK_NULL_HANDLE */
  VkExtent2D                   extent;
  uint32_t                     clear;
  uint64_t                     generation;    /* as bake_generation */
} remix_vkfe_ui_layer;

typedef struct remix_vkfe_present_result {
  /* When Remix wrote the frame: the present must wait on this semaphore
   * instead of wait_semaphores (Remix waited on those). VK_NULL_HANDLE:
   * present as the game asked. */
  VkSemaphore                  wait_semaphore;
} remix_vkfe_present_result;

/* ------------------------------------------------------------------ */
/* API table                                                            */
/* ------------------------------------------------------------------ */

typedef struct remix_vkfe_api {
  uint32_t version;

  /* Instance: plan before vkCreateInstance, finish after it returned
   * (any result). get_instance_proc_addr is the next layer's for the
   * Vulkan layer and the loader's for vkd3d. */
  remix_vkfe_result (*plan_instance)(const remix_vkfe_instance_request* request,
                                     remix_vkfe_instance_plan* plan);
  remix_vkfe_result (*finish_instance)(remix_vkfe_pending pending,
                                       VkResult create_result,
                                       VkInstance instance,
                                       PFN_vkGetInstanceProcAddr get_instance_proc_addr,
                                       remix_vkfe_instance* out_instance);
  void (*destroy_instance)(remix_vkfe_instance instance);

  /* Device: plan before vkCreateDevice, finish after it returned. */
  remix_vkfe_result (*plan_device)(const remix_vkfe_device_request* request,
                                   remix_vkfe_device_plan* plan);
  remix_vkfe_result (*finish_device)(remix_vkfe_pending pending,
                                     VkResult create_result,
                                     VkDevice device,
                                     PFN_vkGetDeviceProcAddr get_device_proc_addr,
                                     remix_vkfe_device* out_device);
  /* Before the game's vkDestroyDevice. Waits for Remix's GPU work. */
  void (*destroy_device)(remix_vkfe_device device);

  /* Swapchain images, so Remix can wrap them. The caller must have added
   * VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT to
   * the swap chain's imageUsage (Remix copies its frame in and out). */
  void (*on_swapchain)(remix_vkfe_device device,
                       VkSwapchainKHR swapchain,
                       const VkSwapchainCreateInfoKHR* create_info,
                       uint32_t image_count,
                       const VkImage* images);
  void (*on_swapchain_destroy)(remix_vkfe_device device, VkSwapchainKHR swapchain);

  /* Graphics pipeline created. Stages are SPIR-V (vkd3d passes what it
   * compiled). plan->supported == 0: no capture variant needed. */
  void (*on_pipeline)(remix_vkfe_device device, const remix_vkfe_pipeline_desc* desc,
                      remix_vkfe_capture_plan* plan);
  void (*on_pipeline_destroy)(remix_vkfe_device device, uint64_t key);

  /* Recorded draw. Called while the game records, before the game's draw
   * is recorded or right after it; Remix copies what it needs, and acts on
   * it when the command buffer is submitted. capture tells the caller
   * whether to replay the draw into Remix's capture buffer. */
  void (*on_draw)(remix_vkfe_device device, const remix_vkfe_draw_desc* desc,
                  remix_vkfe_draw_capture* capture);

  /* Command buffer reset/begin: draws recorded earlier into it are void. */
  void (*on_command_buffer_reset)(remix_vkfe_device device, VkCommandBuffer command_buffer);

  void (*on_submit)(remix_vkfe_device device, const remix_vkfe_submit_desc* desc);

  remix_vkfe_result (*on_present)(remix_vkfe_device device,
                                  const remix_vkfe_present_desc* desc,
                                  remix_vkfe_present_result* result);

  /* Around every vkQueueSubmit / vkQueueSubmit2 / vkQueuePresentKHR /
   * vkQueueBindSparse / vkQueueWaitIdle the game issues. When the GPU had no
   * spare graphics queue, Remix submits to one of the game's queues, and
   * these take the lock DXVK's own submissions hold. Otherwise they return
   * at once. */
  void (*lock_queue)(remix_vkfe_device device, VkQueue queue);
  void (*unlock_queue)(remix_vkfe_device device, VkQueue queue);

  /* Snapshot image for a pre-UI copy of `format` and `extent` (same format
   * as the source, so vkCmdCopyImage applies). REMIX_VKFE_UNSUPPORTED: no
   * snapshot this frame. */
  remix_vkfe_result (*get_ui_snapshot)(remix_vkfe_device device, VkCommandBuffer command_buffer,
                                       VkFormat format, VkExtent2D extent,
                                       remix_vkfe_ui_snapshot* snapshot);
  /* A copy into the snapshot was recorded into command_buffer; it counts
   * for the frame that command buffer is submitted in. */
  void (*on_ui_snapshot)(remix_vkfe_device device, VkCommandBuffer command_buffer);

  /* Before the game's vkDestroyImage: Remix drops what it wrapped around
   * the image, so a handle the driver reuses is never mistaken for it. */
  void (*on_image_destroy)(remix_vkfe_device device, VkImage image);

  /* The terrain bake on_draw asked for was recorded into command_buffer
   * (for the draw last reported on it). Not called when it was skipped. */
  void (*on_bake)(remix_vkfe_device device, VkCommandBuffer command_buffer);

  /* UI layer (remix_vkfe_ui_layer) for a UI draw into a target of
   * color_format and extent, with a depth attachment of depth_format
   * (VK_FORMAT_UNDEFINED: none). REMIX_VKFE_UNSUPPORTED: do not replay. */
  remix_vkfe_result (*get_ui_layer)(remix_vkfe_device device, VkCommandBuffer command_buffer,
                                    VkFormat color_format, VkFormat depth_format, VkExtent2D extent,
                                    remix_vkfe_ui_layer* layer);
  /* replayed != 0: the replay into the layer was recorded into
   * command_buffer. replayed == 0: a UI draw recorded there could not be
   * replayed (secondary command buffer, unsplittable pass, no UI variant);
   * that frame's layer is incomplete and Remix uses the snapshot. */
  void (*on_ui_layer)(remix_vkfe_device device, VkCommandBuffer command_buffer, uint32_t replayed);

  /* Before on_submit: the bytes of a host_data_at_submit binding as they
   * are now. draw_index counts the on_draw calls made for command_buffer
   * since it was last reset; binding_index is the binding's index in that
   * draw. data is valid during the call only. */
  void (*on_submit_bytes)(remix_vkfe_device device, VkCommandBuffer command_buffer,
                          uint32_t draw_index, uint32_t binding_index,
                          const void* data, VkDeviceSize size);
} remix_vkfe_api;

typedef remix_vkfe_result (*PFN_remix_vkfe_get_api)(uint32_t version, const remix_vkfe_api** api);

/* ------------------------------------------------------------------ */
/* DX12 draw annotation (exported by remix_vk_layer.dll)                */
/* ------------------------------------------------------------------ */

/*
 * DX12 games reach Remix through vkd3d-proton and the Remix Vulkan layer:
 * the layer sees vkd3d's pipelines, draws and present like any Vulkan
 * game's. What it cannot see is what exists only at the D3D12 level - root
 * CBV addresses and the descriptor heap. Remix's vkd3d-proton patch calls
 * this right before each draw with the D3D12 bindings of the graphics root
 * signature: constant buffers by (register, space) with CPU pointers where
 * the memory is host visible, and table SRVs as VkImageViews. The layer
 * attaches them to the next draw recorded into command_buffer, in place of
 * the descriptor-set view of vkd3d's bindless heap. Pointers are read during
 * the call only.
 */
#define REMIX_VKLAYER_ANNOTATE_ENTRY_POINT "remix_vklayer_annotate_draw"

typedef void (*PFN_remix_vklayer_annotate_draw)(VkCommandBuffer command_buffer,
                                                const remix_vkfe_binding* bindings,
                                                uint32_t binding_count);

/*
 * GPU-driven DX12 draws: an ExecuteIndirect whose command signature also
 * sets root constants reaches Vulkan as vkCmdExecuteGeneratedCommandsEXT,
 * whose per-command push constants exist only on the GPU (a culling compute
 * pass writes them). vkd3d tells the layer, when it creates such a command
 * signature, which push-constant ranges its commands set for a pipeline
 * layout (note_indirect_layout); the layer then asks Remix for a capture
 * variant that reads them from the stream. Right before each
 * vkCmdExecuteGeneratedCommandsEXT it calls annotate_draw and
 * annotate_indirect; the layer replays the commands with that variant as a
 * multi-draw indirect into Remix's capture buffer.
 */
#define REMIX_VKLAYER_NOTE_INDIRECT_LAYOUT_ENTRY_POINT "remix_vklayer_note_indirect_layout"

typedef void (*PFN_remix_vklayer_note_indirect_layout)(VkPipelineLayout layout, uint32_t stride,
                                                       uint32_t constant_count,
                                                       const remix_vkfe_indirect_constant* constants);

typedef struct remix_vkfe_indirect_annotation {
  VkPipelineLayout             layout;
  VkBuffer                     argument_buffer;
  VkDeviceSize                 argument_offset;   /* command 0, in argument_buffer */
  VkDeviceAddress              argument_address;  /* command 0 */
  uint32_t                     stride;
  uint32_t                     max_command_count;
  VkBuffer                     count_buffer;      /* VK_NULL_HANDLE: max_command_count commands */
  VkDeviceSize                 count_offset;
  uint32_t                     indexed;
  uint32_t                     args_offset;       /* draw arguments in a command */
} remix_vkfe_indirect_annotation;

#define REMIX_VKLAYER_ANNOTATE_INDIRECT_ENTRY_POINT "remix_vklayer_annotate_indirect"

typedef void (*PFN_remix_vklayer_annotate_indirect)(VkCommandBuffer command_buffer,
                                                    const remix_vkfe_indirect_annotation* annotation);

/* The image behind a VkImageView the layer has seen created, as a texture
 * binding (image, format, extent, mips, layers, transfer_src); layout as
 * given. Returns 0 when the view is unknown. Exported by remix_vk_layer.dll. */
#define REMIX_VKLAYER_DESCRIBE_VIEW_ENTRY_POINT "remix_vklayer_describe_view"

typedef uint32_t (*PFN_remix_vklayer_describe_view)(VkDevice device, VkImageView view, VkImageLayout layout,
                                                    remix_vkfe_binding* out);

/* Descriptor `index` of a REMIX_VKFE_BINDING_DESCRIPTOR_HEAP heap: its image
 * view and the layout it is sampled in. Returns 0 when the descriptor is not
 * an image view (or index >= descriptor_count). Exported by Remix's vkd3d
 * d3d12core.dll (where libvkd3d lives). */
#define REMIX_VKD3D_HEAP_VIEW_ENTRY_POINT "remix_vkd3d_heap_view"

typedef uint32_t (*PFN_remix_vkd3d_heap_view)(const void* heap, uint32_t descriptor_count, uint32_t index,
                                              VkImageView* view, VkImageLayout* layout);

#ifdef __cplusplus
}
#endif

#endif /* REMIX_VK_FRONTEND_H */
