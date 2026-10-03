#pragma once

// Remix Vulkan layer: the native Vulkan front end.
//
// An implicit layer (remix_vk_layer.json). It stays a pure pass-through
// unless the game's folder holds Remix's d3d11.dll (exporting
// remix_vkfe_get_api) and a Remix config, and no anti-cheat. When enabled it:
//   - lets Remix extend vkCreateInstance / vkCreateDevice (API version,
//     ray-tracing extensions, features, one extra queue), then hands Remix
//     the created handles (remix_vk_frontend.h);
//   - tracks the objects a draw refers to (pipelines and their SPIR-V,
//     descriptor sets, buffers and their mapped memory, images, render
//     targets), keyed by raw handle; nothing is wrapped;
//   - reports each draw to Remix and, when Remix asks, replays it inline
//     through a transform feedback variant of the game's pipeline;
//   - adds transfer usage to swap chains and lets Remix write its frame
//     before vkQueuePresentKHR.
// Design and sources: documentation/engine_knowledge/vulkan_layer_capture.md.

#ifndef VK_USE_PLATFORM_WIN32_KHR
#define VK_USE_PLATFORM_WIN32_KHR 1
#endif
// The build passes -DNOMINMAX; redefining it is C4005, an error under werror.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>

#include <remix/remix_vk_frontend.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace remix_vklayer {

  using DispatchKey = void*;

  // Dispatchable handles start with the loader's dispatch table pointer;
  // every child of a device (queues, command buffers) shares it.
  inline DispatchKey keyOf(const void* handle) {
    return *reinterpret_cast<void* const*>(handle);
  }

  void log(const char* fmt, ...);

  // Remix's d3d11.dll API, or null when Remix is not enabled for this
  // process (decided once, at the first vkCreateInstance).
  const remix_vkfe_api* remixApi();

  // ------------------------------------------------------------------------
  // Instance
  // ------------------------------------------------------------------------

  struct InstanceData {
    VkInstance                                      instance = VK_NULL_HANDLE;
    PFN_vkGetInstanceProcAddr                       gipa = nullptr;
    PFN_vkDestroyInstance                           DestroyInstance = nullptr;
    PFN_vkEnumerateDeviceExtensionProperties        EnumerateDeviceExtensionProperties = nullptr;
    PFN_vkGetPhysicalDeviceFeatures2                GetPhysicalDeviceFeatures2 = nullptr;
    PFN_vkGetPhysicalDeviceFeatures2KHR             GetPhysicalDeviceFeatures2KHR = nullptr;
    PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR   GetPhysicalDeviceSurfaceCapabilitiesKHR = nullptr;
    remix_vkfe_instance                             remix = nullptr;
    // The application is vkd3d-proton (a DX12 game), not a native Vulkan game.
    bool                                            vkd3d = false;
  };

  // ------------------------------------------------------------------------
  // Device: next-layer functions the layer calls or forwards
  // ------------------------------------------------------------------------

#define REMIX_VKLAYER_DEVICE_FUNCTIONS(X) \
  X(DestroyDevice) X(GetDeviceQueue) X(GetDeviceQueue2) \
  X(CreateSwapchainKHR) X(DestroySwapchainKHR) X(GetSwapchainImagesKHR) X(QueuePresentKHR) \
  X(QueueSubmit) X(QueueSubmit2) X(QueueSubmit2KHR) X(QueueWaitIdle) X(QueueBindSparse) \
  X(CreateShaderModule) X(DestroyShaderModule) X(CreateGraphicsPipelines) X(DestroyPipeline) \
  X(CreatePipelineLayout) X(DestroyPipelineLayout) \
  X(CreateDescriptorSetLayout) X(DestroyDescriptorSetLayout) \
  X(AllocateDescriptorSets) X(FreeDescriptorSets) X(ResetDescriptorPool) X(DestroyDescriptorPool) \
  X(UpdateDescriptorSets) X(CreateDescriptorUpdateTemplate) X(CreateDescriptorUpdateTemplateKHR) \
  X(DestroyDescriptorUpdateTemplate) X(DestroyDescriptorUpdateTemplateKHR) \
  X(UpdateDescriptorSetWithTemplate) X(UpdateDescriptorSetWithTemplateKHR) \
  X(CreateImage) X(DestroyImage) X(CreateImageView) X(DestroyImageView) \
  X(CreateBuffer) X(DestroyBuffer) X(BindBufferMemory) X(BindBufferMemory2) X(BindBufferMemory2KHR) \
  X(MapMemory) X(UnmapMemory) X(FreeMemory) \
  X(CreateRenderPass) X(CreateRenderPass2) X(CreateRenderPass2KHR) X(DestroyRenderPass) \
  X(CreateFramebuffer) X(DestroyFramebuffer) \
  X(AllocateCommandBuffers) X(FreeCommandBuffers) X(ResetCommandPool) X(DestroyCommandPool) \
  X(BeginCommandBuffer) X(ResetCommandBuffer) X(EndCommandBuffer) X(CmdExecuteGeneratedCommandsEXT) \
  X(CmdBindPipeline) X(CmdBindDescriptorSets) X(CmdPushDescriptorSetKHR) \
  X(CmdPushDescriptorSetWithTemplateKHR) X(CmdPushConstants) \
  X(CmdBindVertexBuffers) X(CmdBindVertexBuffers2) X(CmdBindVertexBuffers2EXT) X(CmdBindIndexBuffer) \
  X(CmdBeginRenderPass) X(CmdBeginRenderPass2) X(CmdBeginRenderPass2KHR) \
  X(CmdEndRenderPass) X(CmdEndRenderPass2) X(CmdEndRenderPass2KHR) \
  X(CmdNextSubpass) X(CmdNextSubpass2) X(CmdNextSubpass2KHR) \
  X(CmdSetRasterizerDiscardEnable) X(CmdSetRasterizerDiscardEnableEXT) \
  X(CmdBeginRendering) X(CmdBeginRenderingKHR) X(CmdEndRendering) X(CmdEndRenderingKHR) \
  X(CmdSetViewport) X(CmdSetScissor) \
  X(CmdSetViewportWithCount) X(CmdSetViewportWithCountEXT) X(CmdSetScissorWithCount) X(CmdSetScissorWithCountEXT) \
  X(CmdClearAttachments) \
  X(CmdDraw) X(CmdDrawIndexed) X(CmdDrawIndirect) X(CmdDrawIndexedIndirect) \
  X(CmdDrawIndirectCount) X(CmdDrawIndexedIndirectCount) \
  X(CmdDrawIndirectCountKHR) X(CmdDrawIndexedIndirectCountKHR) \
  X(CmdExecuteCommands) \
  X(CmdBeginTransformFeedbackEXT) X(CmdEndTransformFeedbackEXT) X(CmdBindTransformFeedbackBuffersEXT) \
  X(CmdPipelineBarrier) X(CmdCopyImage) \
  X(CmdUpdateBuffer) X(CmdCopyBuffer) X(CmdCopyBuffer2) X(CmdCopyBuffer2KHR)

  // ------------------------------------------------------------------------
  // Tracked objects
  // ------------------------------------------------------------------------

  struct ImageInfo {
    VkFormat    format = VK_FORMAT_UNDEFINED;
    VkExtent3D  extent = { 0u, 0u, 0u };
    uint32_t    mips = 1;
    uint32_t    layers = 1;
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
    bool        swapchain = false;
    // Copies from it are allowed (Remix's hash read-back, UI snapshot).
    bool        transferSrc = false;
  };

  struct ViewInfo {
    VkImage     image = VK_NULL_HANDLE;
    VkFormat    format = VK_FORMAT_UNDEFINED;
    uint32_t    mips = 1;
    uint32_t    baseLayer = 0;
  };

  struct BufferInfo {
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize   memoryOffset = 0;
    VkDeviceSize   size = 0;
  };

  // Bytes a device-local buffer was given by vkCmdUpdateBuffer, or by a
  // small vkCmdCopyBuffer from CPU-readable memory, as of recording: what
  // its uniform buffers hold for the draws recorded after. Lazily paged.
  constexpr VkDeviceSize kShadowPageSize = 64ull << 10;

  struct BufferShadow {
    std::unordered_map<VkDeviceSize, std::unique_ptr<uint8_t[]>> pages;
  };

  struct Descriptor {
    VkDescriptorType type = VK_DESCRIPTOR_TYPE_MAX_ENUM;
    VkBuffer         buffer = VK_NULL_HANDLE;
    VkDeviceSize     offset = 0;
    VkDeviceSize     range = 0;
    VkImageView      view = VK_NULL_HANDLE;
    VkImageLayout    layout = VK_IMAGE_LAYOUT_UNDEFINED;
  };

  struct SetLayoutBinding {
    VkDescriptorType   type = VK_DESCRIPTOR_TYPE_MAX_ENUM;
    uint32_t           count = 0;
    VkShaderStageFlags stages = 0;
  };

  struct SetLayoutInfo {
    std::unordered_map<uint32_t, SetLayoutBinding> bindings;
  };

  // Descriptors Remix sees per binding: bindless arrays hold up to hundreds
  // of thousands; the first kMaxArrayDescriptors written ones are kept.
  constexpr uint32_t kMaxArrayDescriptors = 64;

  struct SetInfo {
    std::shared_ptr<SetLayoutInfo>                          layout;
    VkDescriptorPool                                        pool = VK_NULL_HANDLE;
    std::unordered_map<uint32_t, std::vector<Descriptor>>   bindings;
  };

  struct PipelineLayoutInfo {
    std::vector<std::shared_ptr<SetLayoutInfo>>             sets;
  };

  struct RenderPassInfo {
    std::vector<VkFormat> formats;
    std::vector<VkImageLayout> finalLayouts;
    struct Subpass {
      std::vector<uint32_t> colors;
      uint32_t              depth = VK_ATTACHMENT_UNUSED;
    };
    std::vector<Subpass>  subpasses;
    // Single-subpass passes: a copy with every attachment loaded and starting
    // in its final layout, used to resume the pass after the UI snapshot
    // copy (remix_vkfe_ui_snapshot).
    VkRenderPass          resume = VK_NULL_HANDLE;
    // Every attachment is stored at the end, so the instance can be ended
    // early and resumed without losing contents (terrain bake split).
    bool                  allStored = true;
    // Single-subpass passes whose subpass uses every attachment as colour
    // or depth and nothing else: a compatible pass on Remix's bake images
    // (remix_vkfe_draw_capture::bake), everything in GENERAL.
    VkRenderPass          bake = VK_NULL_HANDLE;
  };

  struct FramebufferInfo {
    std::vector<VkImageView> views;
    VkExtent2D               extent = { 0u, 0u };
  };

  // Push-constant ranges a command stream sets per command for a pipeline
  // layout (remix_vklayer_note_indirect_layout).
  struct IndirectLayout {
    uint32_t                     stride = 0;
    uint32_t                     constantCount = 0;
    remix_vkfe_indirect_constant constants[REMIX_VKFE_MAX_INDIRECT_CONSTANTS] = {};

    bool operator == (const IndirectLayout& other) const {
      return stride == other.stride && constantCount == other.constantCount
          && !std::memcmp(constants, other.constants, sizeof(constants));
    }
  };

  struct PipelineInfo {
    uint64_t         key = 0;
    VkPipeline       variant = VK_NULL_HANDLE;   // capture variant, if Remix asked
    bool             blend = false;              // render target 0 blends
    // Rasterizer discard is dynamic state: the replay sets it on and
    // restores the game's value (CommandState::discardEnable).
    bool             dynamicDiscard = false;

    // Terrain bake variant (remix_vkfe_capture_plan::bake_spirv): the
    // vertex binding its matrix comes from, whether the game sets viewports
    // and scissors with count, and binding strides dynamically.
    VkPipeline       bakeVariant = VK_NULL_HANDLE;
    uint32_t         bakeBinding = 0;
    bool             bakeViewportWithCount = false;
    bool             bakeDynamicStride = false;

    // UI layer (remix_vkfe_ui_layer): the pipeline can draw UI (one colour
    // target of a swap chain format, no depth test, single-sampled), and
    // its variant with coverage accumulated in alpha (null when the colour
    // blending has no layer equivalent).
    bool             uiCandidate = false;
    VkPipeline       uiVariant = VK_NULL_HANDLE;

    // Generated-commands capture variant (remix_vkfe_capture_plan::
    // indirect_spirv) and the command layout it was built for.
    VkPipeline       indirectVariant = VK_NULL_HANDLE;
    IndirectLayout   indirectLayout;
  };

  struct TemplateInfo {
    VkDescriptorUpdateTemplateType                  type = VK_DESCRIPTOR_UPDATE_TEMPLATE_TYPE_DESCRIPTOR_SET;
    uint32_t                                        set = 0;
    std::vector<VkDescriptorUpdateTemplateEntry>    entries;
  };

  struct SwapchainInfo {
    std::vector<VkImage> images;
    VkFormat             format = VK_FORMAT_UNDEFINED;
    VkExtent2D           extent = { 0u, 0u };
  };

  // ------------------------------------------------------------------------
  // Command buffer recording state
  // ------------------------------------------------------------------------

  constexpr uint32_t kMaxSets = 32;
  constexpr uint32_t kMaxVertexBuffers = 32;
  constexpr uint32_t kMaxPushConstantBytes = 256;

  struct CommandState {
    VkCommandPool    pool = VK_NULL_HANDLE;
    bool             primary = true;

    // The render pass instance draws are recorded into, kept so the pass can
    // be ended and resumed around the UI snapshot copy.
    enum class Pass { None, RenderPass, DynamicRendering } pass = Pass::None;
    bool             passSplittable = false;
    VkRenderPass     renderPass = VK_NULL_HANDLE;
    VkFramebuffer    framebuffer = VK_NULL_HANDLE;
    VkRect2D         renderArea = {};
    std::vector<VkImageView> imagelessViews;
    // The pass's attachment views and current subpass, so vkCmdNextSubpass
    // reports that subpass's targets.
    std::vector<VkImageView> passViews;
    uint32_t         subpass = 0;
    VkImageLayout    target0FinalLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkRenderingInfo  rendering = {};
    std::vector<VkRenderingAttachmentInfo> renderingColors;
    VkRenderingAttachmentInfo renderingDepth = {};
    VkRenderingAttachmentInfo renderingStencil = {};
    bool             renderingHasDepth = false;
    bool             renderingHasStencil = false;

    VkPipeline       pipeline = VK_NULL_HANDLE;

    struct BoundSet {
      std::shared_ptr<SetInfo>  set;
      std::vector<uint32_t>     dynamicOffsets;
    };
    std::array<BoundSet, kMaxSets> sets;

    uint8_t          push[kMaxPushConstantBytes] = {};
    uint32_t         pushSize = 0;
    VkShaderStageFlags pushStages = 0;

    struct VertexBuffer {
      VkBuffer     buffer = VK_NULL_HANDLE;
      VkDeviceSize offset = 0;
      VkDeviceSize size = VK_WHOLE_SIZE;
      VkDeviceSize stride = 0;
    };
    std::array<VertexBuffer, kMaxVertexBuffers> vertexBuffers;

    VkBuffer         indexBuffer = VK_NULL_HANDLE;
    VkDeviceSize     indexOffset = 0;
    VkIndexType      indexType = VK_INDEX_TYPE_UINT16;

    uint32_t                renderTargetCount = 0;
    remix_vkfe_attachment   renderTargets[8] = {};
    remix_vkfe_attachment   depth = {};

    VkViewport       viewport = {};
    VkRect2D         scissor = {};

    // Every viewport and scissor the game set, restored after a bake.
    static constexpr uint32_t kMaxViewports = 16;
    VkViewport       viewports[kMaxViewports] = {};
    VkRect2D         scissors[kMaxViewports] = {};
    uint32_t         viewportCount = 0;
    uint32_t         scissorCount = 0;
    bool             viewportWithCount = false;
    bool             scissorWithCount = false;

    // Last vkCmdSetRasterizerDiscardEnable value.
    VkBool32         discardEnable = VK_FALSE;

    bool             gameTransformFeedbackActive = false;

    std::vector<VkCommandBuffer> secondaries;

    // D3D12 bindings vkd3d-proton reported for the next draw
    // (remix_vklayer_annotate_draw). Pointers valid until that draw.
    bool                                  annotated = false;
    std::vector<remix_vkfe_binding>       annotation;

    // Mapped-memory bindings read at vkQueueSubmit (host_data_at_submit):
    // the on_draw call index and binding index they belong to.
    struct SubmitRead {
      uint32_t     draw = 0;
      uint32_t     binding = 0;
      VkBuffer     buffer = VK_NULL_HANDLE;
      VkDeviceSize offset = 0;
      VkDeviceSize size = 0;
      // DX12 (vkd3d-proton annotation): the upload-heap pointer itself,
      // mapped for the D3D12 resource's lifetime, which spans the GPU work.
      const void*  host = nullptr;
    };
    uint32_t                              reportedDraws = 0;
    std::vector<SubmitRead>               submitReads;

    // The command stream vkd3d-proton reported for the next
    // vkCmdExecuteGeneratedCommandsEXT (remix_vklayer_annotate_indirect).
    bool                                  indirectAnnotated = false;
    remix_vkfe_indirect_annotation        indirect = {};

    // Command-stream copies for Remix (remix_vkfe_draw_capture::
    // args_copy_buffer), recorded once the render pass instance ends.
    struct ArgsCopy {
      uint32_t     maxCount = 0;     // written as the count without a count buffer
      VkBuffer     src = VK_NULL_HANDLE;
      VkDeviceSize srcOffset = 0;
      VkDeviceSize size = 0;
      VkBuffer     dst = VK_NULL_HANDLE;
      VkDeviceSize dstOffset = 0;
      VkBuffer     countSrc = VK_NULL_HANDLE;
      VkDeviceSize countSrcOffset = 0;
      VkBuffer     countDst = VK_NULL_HANDLE;
      VkDeviceSize countDstOffset = 0;
    };
    std::vector<ArgsCopy>                 argsCopies;
    // The last dynamic-rendering instance ended suspended: it resumes later,
    // and nothing may be recorded until it really ends.
    bool                                  renderingSuspended = false;
  };

  struct DeviceData {
    VkDevice                    device = VK_NULL_HANDLE;
    VkPhysicalDevice            physical = VK_NULL_HANDLE;
    InstanceData*               instance = nullptr;
    PFN_vkGetDeviceProcAddr     gdpa = nullptr;
    remix_vkfe_device           remix = nullptr;

#define REMIX_VKLAYER_DECLARE(name) PFN_vk##name name = nullptr;
    REMIX_VKLAYER_DEVICE_FUNCTIONS(REMIX_VKLAYER_DECLARE)
#undef REMIX_VKLAYER_DECLARE

    std::mutex                                                          mutex;
    std::unordered_map<VkImage, ImageInfo>                              images;
    std::unordered_map<VkImageView, ViewInfo>                           views;
    std::unordered_map<VkBuffer, BufferInfo>                            buffers;
    std::unordered_map<VkBuffer, BufferShadow>                          shadows;
    std::unordered_map<VkDeviceMemory, uint8_t*>                        mapped;      // base pointer (offset 0)
    std::unordered_map<VkDescriptorSetLayout, std::shared_ptr<SetLayoutInfo>> setLayouts;
    std::unordered_map<VkDescriptorSet, std::shared_ptr<SetInfo>>       sets;
    std::unordered_map<VkPipelineLayout, std::shared_ptr<PipelineLayoutInfo>> pipelineLayouts;
    std::unordered_map<VkRenderPass, RenderPassInfo>                    renderPasses;
    std::unordered_map<VkFramebuffer, FramebufferInfo>                  framebuffers;
    std::unordered_map<VkShaderModule, std::vector<uint32_t>>           shaderModules;
    std::unordered_map<VkPipeline, PipelineInfo>                        pipelines;
    std::unordered_map<VkDescriptorUpdateTemplate, TemplateInfo>        templates;
    std::unordered_map<VkSwapchainKHR, SwapchainInfo>                   swapchains;
    std::unordered_map<VkCommandBuffer, std::unique_ptr<CommandState>>  commandBuffers;

    // Framebuffers around Remix's bake images, by render pass and views;
    // dropped when Remix's bake generation changes.
    std::map<std::vector<uint64_t>, VkFramebuffer>                      bakeFramebuffers;
    uint64_t                                                            bakeGeneration = 0;
    // Replaced bake framebuffers and the present count they were retired at.
    std::vector<std::pair<uint64_t, VkFramebuffer>>                     retiredFramebuffers;
    uint64_t                                                            presentCount = 0;

    // UI snapshot state of the frame being recorded (reset at present): a
    // snapshot was taken, and a UI draw followed it - later full-screen
    // passes (present blits, copies) already contain the UI.
    std::atomic<bool>           snapshotTaken = { false };
    std::atomic<bool>           uiStarted = { false };

    bool active() const { return remix != nullptr; }
  };

  // Registries (vklayer_dispatch.cpp).
  InstanceData* findInstance(DispatchKey key);
  InstanceData* findInstanceForPhysicalDevice(VkPhysicalDevice physical);
  DeviceData*   findDevice(DispatchKey key);

  template<typename T>
  DeviceData* deviceOf(T handle) {
    return findDevice(keyOf(handle));
  }

  // vklayer_track.cpp
  // CPU pointer to `size` bytes of a buffer at `offset`: its mapping, or its
  // shadow (perDraw set) when the range lies in one shadow page. Caller
  // holds dev.mutex.
  const uint8_t* cpuBytes(DeviceData& dev, VkBuffer buffer, VkDeviceSize offset, VkDeviceSize& size, bool& perDraw);
  void fillTextureBinding(DeviceData& dev, const Descriptor& d, remix_vkfe_binding& out);
  void fillBufferBinding(DeviceData& dev, const Descriptor& d, uint32_t dynamicOffset, remix_vkfe_binding& out);
  remix_vkfe_attachment attachmentOf(DeviceData& dev, VkImageView view);
  void applyWrites(DeviceData& dev, uint32_t count, const VkWriteDescriptorSet* writes, SetInfo* pushTarget);
  void applyTemplate(DeviceData& dev, const TemplateInfo& t, const void* data, SetInfo& set);

  // vklayer_commands.cpp
  CommandState* commandState(DeviceData& dev, VkCommandBuffer cmd);
  void resetCommandState(DeviceData& dev, VkCommandBuffer cmd);
  void annotateDraw(VkCommandBuffer cmd, const remix_vkfe_binding* bindings, uint32_t count);
  void annotateIndirect(VkCommandBuffer cmd, const remix_vkfe_indirect_annotation* annotation);
  // Generated-commands layouts (process-wide: vkd3d reports them without a
  // device, and pipeline layout handles are unique while alive).
  void noteIndirectLayout(VkPipelineLayout layout, const IndirectLayout& indirect);
  bool findIndirectLayout(VkPipelineLayout layout, IndirectLayout& out);
  uint32_t describeView(VkDevice device, VkImageView view, VkImageLayout layout, remix_vkfe_binding* out);

  // Hooked entry points by name; nullptr if not hooked. deviceHook
  // (vklayer_dispatch.cpp) asks trackHook (vklayer_track.cpp) and
  // commandHook (vklayer_commands.cpp).
  PFN_vkVoidFunction instanceHook(const char* name);
  PFN_vkVoidFunction deviceHook(const char* name);
  PFN_vkVoidFunction trackHook(const char* name);
  PFN_vkVoidFunction commandHook(const char* name);

}
