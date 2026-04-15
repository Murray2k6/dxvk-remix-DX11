#pragma once

#include "d3d11_include.h"

#include "../dxvk/rtx_render/rtx_types.h"
#include "../dxvk/rtx_render/rtx_hashing.h"
#include "../dxvk/rtx_render/rtx_materials.h"
#include "../dxvk/rtx_render/rtx_utils.h"
#include "../dxvk/dxvk_buffer.h"
#include "../util/util_matrix.h"
#include "../util/util_threadpool.h"

namespace dxvk {

  class D3D11DeviceContext;

  class D3D11Rtx {
  public:
    explicit D3D11Rtx(D3D11DeviceContext* pContext);

    void Initialize();
    void OnDraw(UINT vertexCount, UINT startVertex);
    void OnDrawIndexed(UINT indexCount, UINT startIndex, INT baseVertex);
    void OnDrawInstanced(UINT vertexCountPerInstance, UINT instanceCount, UINT startVertex, UINT startInstance);
    void OnDrawIndexedInstanced(UINT indexCountPerInstance, UINT instanceCount, UINT startIndex, INT baseVertex, UINT startInstance);
    void ResetCommandListState();

    // Must be called with the context lock held.
    // EndFrame runs the RT pipeline writing output into backbuffer (called BEFORE recording the blit).
    void EndFrame(const Rc<DxvkImage>& backbuffer, VkExtent2D clientExtent = { 0u, 0u });
    // OnPresent registers the swapchain present image (called AFTER recording the blit).
    void OnPresent(const Rc<DxvkImage>& swapchainImage, VkExtent2D clientExtent = { 0u, 0u });

    uint32_t getDrawCallID() const { return m_drawCallID; }

  private:
    static constexpr uint32_t kMaxConcurrentDraws = 6 * 1024;
    using GeometryProcessor = WorkerThreadPool<kMaxConcurrentDraws>;

    D3D11DeviceContext*                  m_context;
    std::unique_ptr<GeometryProcessor>   m_pGeometryWorkers;
    uint32_t                             m_drawCallID = 0;

    // Cached projection cbuffer location — found on first draw with a perspective
    // matrix and reused for the rest of the frame. Reset to invalid in EndFrame.
    uint32_t                             m_projSlot   = UINT32_MAX;
    size_t                               m_projOffset = SIZE_MAX;
    int                                  m_projStage  = -1;
    // true when the engine stores matrices in column-major order (Unity, Godot).
    // Detected during the projection scan — all subsequent reads are transposed.
    bool                                 m_columnMajor = false;

    // Cached view matrix cbuffer location — mirrors projection caching.
    // Once a valid view matrix is found at (stage, slot, offset), subsequent
    // draws re-read from the same location instead of rescanning.
    uint32_t                             m_viewSlot   = UINT32_MAX;
    size_t                               m_viewOffset = SIZE_MAX;
    int                                  m_viewStage  = -1;

    // Cached world matrix cbuffer location — reduces per-draw scanning.
    // World matrices change every draw but often live at the same (stage, slot, offset).
    uint32_t                             m_worldSlot   = UINT32_MAX;
    int                                  m_worldStage  = -1;
    size_t                               m_worldOffset = SIZE_MAX;

    // Smoothed camera position — exponential moving average dampens
    // micro-jitter from floating-point rounding in cbuffer matrix extraction.
    Vector3                              m_smoothedCamPos = Vector3(0.0f);
    bool                                 m_hasPrevCamPos  = false;

    // Most recent real output size seen at EndFrame or OnPresent. Use this to
    // keep viewport fallback aligned with the actual swapchain instead of
    // transient intermediate render targets.
    VkExtent2D                           m_lastOutputExtent = { 0u, 0u };

    // Most recent HWND client size. Prefer this over the internal present image
    // extent for viewport fallback so emulators and dynamic-resolution games
    // follow the user-visible window size.
    VkExtent2D                           m_lastClientExtent = { 0u, 0u };

    // Axis convention auto-detection — voting system accumulates evidence
    // from projection and view matrices, then settles once confident.
    // Re-checks during warmup to correct boot/loading screen misdetections.
    bool                                 m_axisDetected = false;
    bool                                 m_axisLogged   = false;
    uint32_t                             m_axisDetectFrame = 0;

    // Voting counters for Z-up vs Y-up and LH vs RH.
    // Accumulate votes over multiple frames, settle when |votes| >= threshold.
    int                                  m_zUpVotes     = 0;  // positive = Z-up, negative = Y-up
    int                                  m_lhVotes      = 0;  // positive = LH, negative = RH
    int                                  m_yFlipVotes   = 0;  // positive = flipped, negative = normal
    bool                                 m_zUpSettled    = false;
    bool                                 m_lhSettled     = false;
    bool                                 m_yFlipSettled  = false;
    static constexpr int kVoteThreshold  = 5; // votes needed to settle
    mutable Rc<DxvkSampler>              m_defaultSampler;

    // CPU-GPU pacing: flush the CS chunk every N draws to prevent the CPU
    // from queuing unbounded work while the GPU is still on a prior batch.
    // Without this, frame latency spikes and memory pressure builds from
    // thousands of buffered DrawCallState objects.
    static constexpr uint32_t kDrawsPerFlush = 256;
    // Bound per-draw frontend analysis so large meshes do not dominate DX11
    // CPU time when a real scene first appears.
    static constexpr uint32_t kMaxHashedVertices = 1024;
    static constexpr uint32_t kMaxHashedIndices = 4096;
    static constexpr uint32_t kMaxSkinningVerticesToScan = 1024;
    static constexpr uint32_t kResizeCameraGraceFrames = 16;

    struct SubmitRejectStats {
      uint32_t total = 0;
      uint32_t accepted = 0;
      uint32_t queueOverflow = 0;
      uint32_t nonTriangleTopology = 0;
      uint32_t noPixelShader = 0;
      uint32_t noRenderTarget = 0;
      uint32_t trivialDraw = 0;
      uint32_t fullscreenPostFx = 0;
      uint32_t noInputLayout = 0;
      uint32_t noSemantics = 0;
      uint32_t noPositionSemantic = 0;
      uint32_t noTexcoordLayout = 0;
      uint32_t position2D = 0;
      uint32_t noPositionBuffer = 0;
      uint32_t noIndexBuffer = 0;
      uint32_t compositeSkip = 0;
      uint32_t screenSpaceUiSkip = 0;
      uint32_t geometryHashScheduleFailed = 0;
    };

    SubmitRejectStats                    m_submitRejectStats;
    uint32_t                             m_drawsSinceFlush = 0;
    uint32_t                             m_resizeTransitionFramesRemaining = 0;

    Rc<DxvkSampler> getDefaultSampler() const;
    void SubmitDraw(bool indexed, UINT count, UINT start, INT base,
                    const Matrix4* instanceTransform = nullptr);
    void SubmitInstancedDraw(bool indexed, UINT count, UINT start, INT base,
                             UINT instanceCount, UINT startInstance);
    DrawCallTransforms ExtractTransforms();
    Future<GeometryHashes> ComputeGeometryHashes(const RasterGeometry& geo,
                                                 uint32_t vertexCount,
                                                 uint32_t hashStartVertex,
                                                 uint32_t hashVertexCount) const;
    void ClearMaterialTextures(LegacyMaterialData& mat) const;
    void FillMaterialData(LegacyMaterialData& mat) const;
  };

}
