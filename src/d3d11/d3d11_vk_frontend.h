#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// DXVK headers first: they define VK_USE_PLATFORM_WIN32_KHR before
// vulkan.h is included, which the C header would otherwise include without.
#include "d3d11_device.h"
#include "d3d11_context_imm.h"
#include "d3d11_vk_spirv.h"

#include <remix/remix_vk_frontend.h>

#include "../dxvk/dxvk_device_import.h"
#include "../dxvk/rtx_render/rtx_terrain_baker.h"
#include "../util/util_matrix.h"

#include <map>

namespace dxvk {

  /**
   * \brief A pipeline the game created, as Remix needs to see it
   *
   * Shader bytecode and vertex layout are copied: the caller's memory is
   * gone after the hook returns.
   */
  struct D3D11VkPipeline {
    uint64_t                                  key = 0;
    struct Stage {
      remix_vkfe_shader_format                format = remix_vkfe_shader_format(0);
      std::vector<uint8_t>                    code;
      std::string                             entryPoint;
    };
    Stage                                     stages[REMIX_VKFE_STAGE_COUNT];
    std::vector<remix_vkfe_vertex_attribute>  attributes;
    std::vector<std::string>                  semantics;
    std::vector<remix_vkfe_vertex_binding>    bindings;
    remix_vkfe_pipeline_desc                  state = {};

    // Capture variant (d3d11_vk_spirv.cpp): the last pre-raster stage with
    // transform feedback decorations on position and the sampled texcoord.
    bool                                      captureSupported = false;
    remix_vkfe_stage                          captureStage = REMIX_VKFE_STAGE_VERTEX;
    std::vector<uint32_t>                     captureSpirv;
    uint32_t                                  captureStride = 0;
    bool                                      captureHasTexcoord = false;
    // Tessellation / geometry shader: vertex count comes from the XFB
    // counter, as for indirect draws.
    bool                                      captureCountedOnGpu = false;
    D3D11VkCaptureLayout                      captureLayout;

    // Pixel shader texture roles, texcoord and vertex-colour inputs.
    D3D11VkShaderAnalysis                     pixelAnalysis;

    // Identity that survives across runs (pipeline handles do not): hash of
    // the stages' code and the fixed-function state Remix reads.
    uint64_t                                  stableHash = 0;
    // Input topology of the captured stream: XFB writes primitives as
    // lists, so strips and fans come out expanded.
    VkPrimitiveTopology                       capturedTopology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    // Terrain bake variant (remix_vkfe_capture_plan::bake_spirv): vertex
    // stage moving the clip position into a cascade, and the colour target
    // the pixel shader writes albedo to (first 8-bit RGBA target).
    bool                                      bakeSupported = false;
    std::vector<uint32_t>                     bakeSpirv;
    uint32_t                                  bakeMatrixLocation = 0;
    uint32_t                                  bakeAlbedoTarget = 0;

    // Generated-commands capture variant (remix_vkfe_capture_plan::
    // indirect_spirv), for the command layout in state.indirect_*.
    std::vector<uint32_t>                     indirectSpirv;
  };


  /**
   * \brief Device-local memory the game's command buffers capture into
   *
   * Command buffers are recorded before they are submitted and may be
   * submitted again later, so a chunk stays allocated while any command
   * buffer that captures into it is alive, and for a few frames after.
   */
  struct D3D11VkCaptureChunk {
    Rc<DxvkBuffer>                            buffer;
    VkDeviceSize                              used = 0;
    uint32_t                                  users = 0;
    uint64_t                                  lastFrame = 0;
  };


  /**
   * \brief A draw the game recorded, with the bytes Remix reads later
   *
   * Constant-buffer and push-constant bytes are copied at record time:
   * D3D12 and Vulkan apps write them before recording the draw and may
   * reuse the memory once the GPU is done, which can be before Remix
   * processes the submission.
   */
  struct D3D11VkDraw {
    remix_vkfe_draw_desc                      desc = {};
    std::vector<remix_vkfe_vertex_buffer>     vertexBuffers;
    std::vector<remix_vkfe_binding>           bindings;
    // Parallel to bindings; null when not copied. Shared between the draws
    // of a frame that bind the same memory.
    std::vector<std::shared_ptr<const std::vector<uint8_t>>> bindingBytes;

    std::shared_ptr<D3D11VkPipeline>          pipeline;

    // Where this draw's post-VS vertices land, when captured.
    Rc<DxvkBuffer>                            captureBuffer;
    VkDeviceSize                              captureOffset = 0;
    VkDeviceSize                              captureSize = 0;
    uint32_t                                  capturedVertices = 0;
    // Indirect: vertex count comes from the XFB byte counter.
    D3D11VkCaptureChunk*                      captureChunk = nullptr;
    uint32_t                                  counterSlot = ~0u;

    // Terrain bake asked for (bakeSlot) and recorded by the caller (baked,
    // on_bake), with the cascade layout it used.
    int32_t                                   bakeSlot = -1;
    bool                                      baked = false;
    std::shared_ptr<const ExternalTerrainBake> bake;

    // Generated commands (desc.generated_commands): where the copy of the
    // command stream and of the command count land (host-visible readback,
    // remix_vkfe_draw_capture::args_copy_buffer).
    VkDeviceSize                              argsCopyOffset = 0;
    VkDeviceSize                              argsCopySize = 0;
    VkDeviceSize                              countCopyOffset = 0;
    // GPU read-backs of engine material tables the CPU cannot read (in the
    // same ring): Starfield's CommonMaterialEntry [0] and MaterialLayerEntry
    // [1] tables. Size 0: none.
    VkDeviceSize                              tableCopyOffset[2] = {};
    VkDeviceSize                              tableCopySize[2] = {};

    // Material textures resolved from engine material data (bindless
    // engines, e.g. Starfield's material tables): indices into bindings,
    // used in place of the pixel shader's texture roles. -1: none.
    struct ExplicitMaterial {
      int32_t                                 albedo = -1;
      int32_t                                 normal = -1;
      int32_t                                 roughness = -1;
      int32_t                                 metallic = -1;
      uint8_t                                 normalEncoding = 0;
    }                                         explicitMaterial;
  };


  /**
   * \brief An indirect draw waiting for its GPU-written vertex count
   */
  struct D3D11VkPendingIndirect {
    D3D11VkDraw                               draw;
    uint64_t                                  frame = 0;
    // Camera of the frame the vertices were captured in.
    bool                                      haveCamera = false;
    bool                                      clipUsesWDepth = false;
    Matrix4                                   projection;
    Matrix4                                   jitteredProjection;
    Matrix4                                   view;
    bool                                      haveView = false;
  };


  /**
   * \brief State of the capture code (d3d11_vk_capture.cpp) across frames
   */
  struct D3D11VkCaptureState {
    Rc<DxvkSampler>                                   sampler;

    // Game images wrapped as DXVK images, by VkImage. A wrapper never owns
    // the VkImage. Entries unused for a while are dropped, and a handle the
    // game reused for an image with other properties is re-wrapped.
    struct Texture {
      Rc<DxvkImageView>                               view;
      // One- and two-channel colour textures as albedo: grey (rrr1) or
      // grey + alpha (rrrg), so they do not read as red / red-green.
      Rc<DxvkImageView>                               colorView;
      VkFormat                                        format = VK_FORMAT_UNDEFINED;
      VkExtent3D                                      extent = { 0u, 0u, 0u };
      uint32_t                                        mips = 0;
      uint64_t                                        lastFrame = 0;
      // Content hash: one mip is read back once and hashed, so texture
      // categories and replacements tagged by hash hold across runs. Until
      // it arrives the image carries a per-handle hash.
      bool                                            contentHashed = false;
      Rc<DxvkBuffer>                                  readback;
      VkDeviceSize                                    readbackSize = 0;
      uint32_t                                        readbackLevel = 0;
      uint64_t                                        readbackFrame = 0;
    };
    std::unordered_map<VkImage, Texture>              textures;
    // Read-backs started this frame (bounded so a level load does not stall).
    uint32_t                                          readbacksThisFrame = 0;

    // Shadow-cascade sun votes (orthographic depth-only draws), as D3D11Rtx.
    struct SunVote { Vector3 direction; uint32_t count; };
    std::vector<SunVote>                              sunVotes;
    bool                                              sunValid = false;
    Vector3                                           sunDirection = Vector3(0.0f);

    // 2D games: frames without a perspective scene (D3D11Rtx 2D lift).
    uint32_t                                          framesWithoutPerspective = 0;
    bool                                              seenPerspective = false;

    // Light identities already submitted this frame.
    std::unordered_set<XXH64_hash_t>                  lightKeys;

    // Camera of the last frame that had one.
    bool                                              haveProjection = false;
    bool                                              haveView = false;
    Matrix4                                           projection;
    Matrix4                                           jitteredProjection;
    Matrix4                                           view;
    // Where the projection was found: binding (space, slot, kind) and
    // byte offset. Tried first next frame.
    uint32_t                                          projSpace = ~0u;
    uint32_t                                          projSlot = ~0u;
    uint32_t                                          projKind = 0;
    size_t                                            projOffset = 0;
    bool                                              projTransposed = false;

    uint32_t                                          drawCallId = 0;

    // Per-frame counters, logged periodically.
    uint32_t                                          statDraws = 0;
    uint32_t                                          statCommitted = 0;
    uint32_t                                          statNoCapture = 0;
    uint32_t                                          statIndirect = 0;
    uint32_t                                          statNotScene = 0;
    uint32_t                                          statNoCamera = 0;
    uint32_t                                          statDepthOnly = 0;
    uint32_t                                          statDuplicate = 0;
    uint32_t                                          statIgnored = 0;
    uint32_t                                          statDecals = 0;
    uint32_t                                          statParticles = 0;
    uint32_t                                          statSky = 0;
    uint32_t                                          statWater = 0;
    uint32_t                                          statTerrain = 0;
    uint32_t                                          statLifted = 0;
    uint32_t                                          statLights = 0;
    uint32_t                                          statNormalMaps = 0;
  };


  /**
   * \brief Swap chain state for one game swap chain
   */
  struct D3D11VkSwapchain {
    VkFormat                                  format = VK_FORMAT_UNDEFINED;
    VkExtent2D                                extent = { 0u, 0u };
    VkImageUsageFlags                         usage = 0;
    std::vector<Rc<DxvkImage>>                images;
    // Remix renders into this image: it holds a copy of the game's frame
    // (for UI and anything Remix leaves raster-composited) and is copied
    // back into the swap chain image afterwards.
    Rc<DxvkImage>                             backbuffer;
    // UI composite: the game's presented frame, and the composite output.
    Rc<DxvkImage>                             finalCopy;
    Rc<DxvkImage>                             uiOut;
    // Per swap chain image: game rendering done -> Remix; Remix done -> present.
    std::vector<VkSemaphore>                  gameDone;
    std::vector<VkSemaphore>                  remixDone;
  };


  /**
   * \brief Remix on a game's VkDevice (DX12 through vkd3d-proton, or Vulkan)
   *
   * Owns a D3D11 device created on the imported DxvkDevice, so the DX11
   * runtime (D3D11Rtx, RtxContext, the Remix API) runs unchanged on the
   * game's device. Frame boundaries come from on_present.
   */
  class D3D11VkFrontendDevice {

  public:

    D3D11VkFrontendDevice(
            remix_vkfe_frontend frontend,
      const Rc<DxvkInstance>&   instance,
      const Rc<DxvkAdapter>&    adapter,
      const Rc<DxvkDevice>&     device,
            bool                sharesGameQueue);

    // remix_vkfe_api::lock_queue / unlock_queue
    void lockQueue(VkQueue queue);
    void unlockQueue(VkQueue queue);

    ~D3D11VkFrontendDevice();

    const Rc<DxvkDevice>& device() const {
      return m_device;
    }

    D3D11ImmediateContext* context() const {
      return m_context;
    }

    void onSwapchain(
            VkSwapchainKHR              swapchain,
      const VkSwapchainCreateInfoKHR*   info,
            uint32_t                    imageCount,
      const VkImage*                    images);

    void onSwapchainDestroy(VkSwapchainKHR swapchain);

    void onPipeline(
      const remix_vkfe_pipeline_desc*   desc,
            remix_vkfe_capture_plan*    plan);

    void onPipelineDestroy(uint64_t key);

    void onDraw(
      const remix_vkfe_draw_desc*       desc,
            remix_vkfe_draw_capture*    capture);

    void onCommandBufferReset(VkCommandBuffer commandBuffer);

    void onSubmitBytes(
            VkCommandBuffer             commandBuffer,
            uint32_t                    drawIndex,
            uint32_t                    bindingIndex,
      const void*                       data,
            VkDeviceSize                size);

    void onSubmit(const remix_vkfe_submit_desc* desc);

    remix_vkfe_result getUiSnapshot(
            VkCommandBuffer             commandBuffer,
            VkFormat                    format,
            VkExtent2D                  extent,
            remix_vkfe_ui_snapshot*     snapshot);

    void onUiSnapshot(VkCommandBuffer commandBuffer);

    void onImageDestroy(VkImage image);

    void onBake(VkCommandBuffer commandBuffer);

    remix_vkfe_result getUiLayer(
            VkCommandBuffer             commandBuffer,
            VkFormat                    colorFormat,
            VkFormat                    depthFormat,
            VkExtent2D                  extent,
            remix_vkfe_ui_layer*        layer);

    void onUiLayer(VkCommandBuffer commandBuffer, bool replayed);

    remix_vkfe_result onPresent(
      const remix_vkfe_present_desc*    desc,
            remix_vkfe_present_result*  result);

  private:

    remix_vkfe_frontend               m_frontend;
    bool                              m_sharesGameQueue = false;
    Rc<DxvkInstance>                  m_instance;
    Rc<DxvkAdapter>                   m_adapter;
    Rc<DxvkDevice>                    m_device;

    Com<D3D11DXGIDevice>              m_dxgiDevice;
    Com<ID3D11Device>                 m_d3d11Device;
    Com<ID3D11DeviceContext>          m_d3d11Context;
    D3D11ImmediateContext*            m_context = nullptr;

    std::mutex                        m_mutex;
    std::unordered_map<VkSwapchainKHR, D3D11VkSwapchain>             m_swapchains;
    std::unordered_map<uint64_t, std::shared_ptr<D3D11VkPipeline>>   m_pipelines;
    std::unordered_map<VkCommandBuffer, std::vector<D3D11VkDraw>>    m_recorded;

    std::vector<std::unique_ptr<D3D11VkCaptureChunk>>                m_captureChunks;
    D3D11VkCaptureChunk*                                             m_captureCurrent = nullptr;
    std::unordered_map<VkCommandBuffer, std::vector<D3D11VkCaptureChunk*>> m_captureUsers;
    VkDeviceSize                                                     m_captureBytesThisFrame = 0;

    D3D11VkCaptureState                                              m_capture;

    // XFB byte counters of indirect captures: host-visible, one uint32 per
    // slot, used as a ring.
    static constexpr uint32_t                                        kCounterSlots = 65536;
    Rc<DxvkBuffer>                                                   m_counterBuffer;
    uint32_t                                                         m_counterNext = 0;

    // Command-stream copies of generated-commands draws (remix_vkfe_draw_
    // capture::args_copy_buffer): host-visible, used as a ring. A slot is
    // read a few frames after it was written, long before the ring wraps.
    static constexpr VkDeviceSize                                    kArgsReadbackSize = 64ull << 20;
    static constexpr uint32_t                                        kArgsCountSentinel = 0xffffffffu;
    Rc<DxvkBuffer>                                                   m_argsReadback;
    VkDeviceSize                                                     m_argsNext = 0;

    // Starfield (Creation Engine 2): bindless materials, walked from the
    // per-draw material index (starfield-decomp MODLOG.md, "Material chain
    // walk"); the vkd3d / layer exports that resolve heap indices.
    bool                                                             m_starfieldMaterials = false;
    // Material tables read back this frame, by (buffer, offset): ring slot.
    std::map<std::pair<VkBuffer, VkDeviceSize>, std::pair<VkDeviceSize, VkDeviceSize>> m_tableCopies;
    uint64_t                                                         m_tableCopiesFrame = ~0ull;
    PFN_remix_vkd3d_heap_view                                        m_heapView = nullptr;
    PFN_remix_vklayer_describe_view                                  m_describeView = nullptr;
    uint32_t                                                         m_materialWalkLogs = 0;

    // Fence per frame, signalled on the game's queue after the frame's
    // rendering (onPresent step 1). Indirect captures of frame F are read
    // when fence F has signalled.
    static constexpr uint32_t                                        kFrameFences = 8;
    VkFence                                                          m_frameFences[kFrameFences] = {};
    uint64_t                                                         m_frameFenceFrame[kFrameFences] = {};
    std::vector<D3D11VkPendingIndirect>                              m_pendingIndirect;

    // Pre-UI snapshots (remix_vkfe_ui_snapshot): a ring, so the game can
    // write the next frame's while Remix composites with this one.
    struct UiSnapshot {
      Rc<DxvkImage>                                                  image;
      Rc<DxvkImageView>                                              view;
      VkFormat                                                       format = VK_FORMAT_UNDEFINED;
      VkExtent2D                                                     extent = { 0u, 0u };
    };
    static constexpr uint32_t                                        kUiSnapshotSlots = 3;
    UiSnapshot                                                       m_uiSnapshots[kUiSnapshotSlots];
    // Command buffers holding a snapshot copy, and the slot they copy to.
    std::unordered_map<VkCommandBuffer, uint32_t>                    m_uiSnapshotCommands;
    int32_t                                                          m_frameUiSnapshot = -1;
    uint32_t                                                         m_uiSnapshotSlot = 0;
    uint64_t                                                         m_uiSnapshotSlotFrame = ~0ull;
    Rc<DxvkShader>                                                   m_uiCompositeVs;
    Rc<DxvkShader>                                                   m_uiCompositeFs;
    Rc<DxvkShader>                                                   m_uiLayerFs;

    // Remix's frame in `target`, the game's in `finalCopy`: keeps the
    // game's pixels where its UI differs from the pre-UI snapshot.
    void compositeUi(D3D11VkSwapchain& swapchain, const UiSnapshot& snapshot);

    void drawComposite(D3D11VkSwapchain& swapchain, const Rc<DxvkShader>& fs,
                       const Rc<DxvkImageView>& a, const Rc<DxvkImageView>& b, const Rc<DxvkImageView>& c);

    // Terrain bakes (remix_vkfe_draw_capture::bake). The game's command
    // buffers bake into Remix's cascade images: the albedo target in a ring
    // of slots like the UI snapshots (Remix copies a frame's while the game
    // may bake the next), the attachments nobody reads shared. Cascade
    // levels are kBakeLevelResolution square; Remix scales them into its
    // cascade map.
    static constexpr uint32_t                                        kBakeLevelResolution = 1024;
    static constexpr uint32_t                                        kBakeSlots = 3;
    static constexpr uint32_t                                        kBakesPerFrame = 64;
    struct BakeImage {
      Rc<DxvkImage>                                                  image;
      Rc<DxvkImageView>                                              attachmentView;  // the game's format
      Rc<DxvkImageView>                                              sampledView;     // UNORM: bytes as written
      VkFormat                                                       format = VK_FORMAT_UNDEFINED;
      VkExtent2D                                                     extent = { 0u, 0u };
    };
    struct BakeSlot {
      std::map<uint64_t, BakeImage>                                  albedo;   // by format
      // This frame's layout (fixed at its first bake) and bakes, by albedo
      // format; formats whose image was cleared this frame.
      TerrainBaker::ExternalBakeLayout                               layout;
      std::map<VkFormat, std::shared_ptr<ExternalTerrainBake>>       frameBakes;
      std::unordered_set<uint32_t>                                   cleared;
      uint32_t                                                       bakes = 0;
    };
    BakeSlot                                                         m_bakeSlots[kBakeSlots];
    uint32_t                                                         m_bakeSlot = 0;
    uint64_t                                                         m_bakeSlotFrame = ~0ull;
    std::map<uint64_t, BakeImage>                                    m_bakeScratch;
    uint64_t                                                         m_bakeGeneration = 1;
    // Cascade matrices, host visible: slot, bake, cascade -> 64 bytes.
    Rc<DxvkBuffer>                                                   m_bakeMatrices;
    // Replaced bake / UI layer images, kept while the game's command
    // buffers in flight may still render into them (Remix does not see the
    // game's queue), with the frame they were replaced in.
    std::vector<std::pair<uint64_t, BakeImage>>                      m_retiredBakeImages;

    // UI layers (remix_vkfe_ui_layer): a ring like the snapshots, with a
    // scratch depth image per format. A slot is cleared by the frame's
    // first replay into it.
    struct UiLayer {
      BakeImage                                                      color;
      bool                                                           cleared = false;
    };
    UiLayer                                                          m_uiLayers[kUiSnapshotSlots];
    std::map<uint64_t, BakeImage>                                    m_uiLayerDepth;
    std::unordered_map<VkCommandBuffer, uint32_t>                    m_uiLayerCommands;
    // Command buffers with a UI draw the caller could not replay.
    std::unordered_set<VkCommandBuffer>                              m_uiLayerIncomplete;
    int32_t                                                          m_frameUiLayer = -1;
    bool                                                             m_frameUiLayerIncomplete = false;
    uint32_t                                                         m_uiLayerSlot = 0;
    uint64_t                                                         m_uiLayerSlotFrame = ~0ull;

    // Remix's frame with the UI layer over it.
    void compositeUiLayer(D3D11VkSwapchain& swapchain, const UiLayer& layer);

    // Sets up the bake of a terrain draw. Caller holds m_mutex.
    bool prepareBake(D3D11VkDraw& draw, remix_vkfe_draw_capture* capture);

    // Game clip space -> world for this draw: its own view-projection,
    // found the way findCamera finds the frame's.
    bool bakeClipToWorld(const D3D11VkDraw& draw, Matrix4& clipToWorld) const;

    const BakeImage* bakeImage(std::map<uint64_t, BakeImage>& images, uint64_t key, VkFormat format, VkExtent2D extent);
    bool createBakeImage(BakeImage& out, VkFormat format, VkExtent2D extent);

    // Capture bytes to reserve for a GPU-counted draw, per pipeline: learned
    // from the XFB counters (twice the last count, doubled when a capture
    // filled its range).
    std::unordered_map<uint64_t, VkDeviceSize>                       m_countedReserve;

    // Draws of the current frame in submission order, consumed by the
    // capture code (d3d11_vk_capture.cpp) at present.
    std::vector<D3D11VkDraw>          m_frameDraws;

    // Bytes copied from host-visible buffers this frame, by address: the
    // per-view constants one engine binds to thousands of draws are copied
    // once.
    std::unordered_map<const void*, std::shared_ptr<const std::vector<uint8_t>>> m_frameBytes;

    // Bytes of mapped-memory bindings read at submission (onSubmitBytes),
    // shared within one submission.
    std::unordered_map<const void*, std::shared_ptr<const std::vector<uint8_t>>> m_submitBytes;

    uint64_t                          m_presentCount = 0;

    void destroySwapchain(D3D11VkSwapchain& swapchain);

    VkSemaphore createSemaphore();

    // A frame whose Remix work failed after the game's present semaphores
    // were consumed: make sure remixDone is signalled anyway.
    void forwardFrame(
            VkQueue                     queue,
            VkSemaphore                 gameDone,
            VkSemaphore                 remixDone,
            bool                        waitEmitted,
            bool                        signalEmitted);

    // Reserves capture memory for a command buffer. Caller holds m_mutex.
    bool allocateCapture(
            VkCommandBuffer             commandBuffer,
            VkDeviceSize                size,
            Rc<DxvkBuffer>&             buffer,
            VkDeviceSize&               offset);

    void releaseCaptureUsers(VkCommandBuffer commandBuffer);

    // d3d11_vk_capture.cpp: turns the frame's draws into Remix scene input
    // (camera, meshes, materials, lights) before EndFrame.
    void captureFrame(std::vector<D3D11VkDraw>& draws);

    struct FrameCamera {
      bool    valid = false;
      bool    clipUsesWDepth = false;
      bool    haveView = false;
      Matrix4 projection;          // jitter removed, canonical orientation
      Matrix4 jitteredProjection;  // as rasterized, canonical orientation
      Matrix4 view;
    };

    FrameCamera findCamera(const std::vector<const D3D11VkDraw*>& sceneDraws, VkExtent3D sceneExtent);

    // What a frame tells the per-draw rules (water, 2D lift).
    struct DrawContext {
      // Images rendered to earlier in the frame (a texture sampled after
      // being a target: scene copies, refraction).
      const std::unordered_set<VkImage>* earlierTargets = nullptr;
      bool     lift2D = false;
      float    liftDepth = 0.0f;
    };

    void commitDraw(
      const D3D11VkDraw&                draw,
            uint32_t                    vertexCount,
      const FrameCamera&                camera,
      const DrawContext&                context);

    Rc<DxvkImageView> wrapTexture(const remix_vkfe_binding& binding);
    Rc<DxvkImageView> wrapColorTexture(const remix_vkfe_binding& binding);

    void processTextureHashes();

    void importLights(const std::vector<const D3D11VkDraw*>& draws, const FrameCamera& camera);

    void learnSun(const D3D11VkDraw& depthOnlyDraw);

    void applySun(const FrameCamera& camera);

    void processPendingIndirect();
    // A generated-commands capture of a finished frame, split per command.
    void commitGeneratedCommands(D3D11VkPendingIndirect& pending, uint32_t writtenBytes);
    // Starfield: material textures of one command's material index.
    void applyMaterialChain(D3D11VkDraw& draw, uint32_t materialIndex);
    bool allocateArgsCopy(VkDeviceSize size, VkDeviceSize& offset);
    VkDeviceSize storageCopyLimit(const remix_vkfe_binding& b, VkDeviceSize defaultLimit) const;

  };

}
