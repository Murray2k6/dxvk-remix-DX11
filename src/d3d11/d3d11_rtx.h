#pragma once
#if defined(_MSC_VER)
#pragma warning(disable: 4099) // DX11_V219_SINGLE_AUTHORITATIVE_BUFFER_COOKIE_METHODS
#endif
#if defined(_MSC_VER)
#pragma warning(disable: 4099) // DX11_V218_BRUTE_FORCE_BUFFER_COOKIE_SINGLE_METHODS
#endif
#if defined(_MSC_VER)
#pragma warning(disable: 4099) // DX11_V216_IDENTIFY_AND_FIX_UNDECLARED_CONTENT_COOKIE
#endif
#if defined(_MSC_VER)
#pragma warning(disable: 4099) // DX11_V215_GDI_NO_D3DUKMDT_AND_BUFFER_COOKIE_FIX
#endif
#if defined(_MSC_VER)
#pragma warning(disable: 4099) // DX11_V214_D3D11_FEATURE_QUERY_AND_GDI_FORMAT_FIX
#endif
#if defined(_MSC_VER)
#pragma warning(disable: 4099) // DX11_V213_FIX_WX_C4099_C4146
#endif
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

    // DX11_V225: DX11 capture-layer options exposed in the RTX Remix "Game Setup"
    // menu. RTX_OPTION generates both the value accessor (e.g. useVertexCapture())
    // and the option-object accessor (useVertexCaptureObject()) the UI binds to.
    RTX_OPTION("rtx", bool, useVertexCapture, true, "Capture programmable vertex-shader output so such draws can be ray traced / terrain baked.");
    RTX_OPTION("rtx", bool, useVertexCapturedNormals, true, "Use normals produced by vertex capture instead of input-assembler normals.");
    RTX_OPTION("rtx", bool, useWorldMatricesForShaders, true, "Use captured world/view matrices when reconstructing transforms for programmable-shader draws.");
    RTX_OPTION("rtx", bool, allowCubemaps, false, "Allow cubemap render targets/draws to be considered for ray tracing.");
    RTX_OPTION("rtx", bool, orthographicIsUI, true, "Treat draws with an orthographic projection as UI and skip them from the ray traced scene.");
    RTX_OPTION("rtx", float, integerTexcoordScale, 1.0f / 2048.0f, "Scale applied when decoding fixed-point integer (SINT/UINT) texcoord vertex formats to floating point UVs. Engines that store UVs as 16-bit integers use an engine-specific divisor; 1/2048 fits common fixed-point conventions (Saints Row IV: TEXCOORD0 = R16G16_SINT). Adjust per game in rtx.conf if textures tile incorrectly.");
    RTX_OPTION("rtx", float, fallbackCameraFovDegrees, 60.0f, "Vertical field of view (degrees) of the synthesized fallback camera used when no projection matrix is found in any constant buffer. Tune per game in rtx.conf when the traced image looks zoomed relative to the raster view. Clamped to [20, 140].");

    void Initialize();
    void OnDraw(UINT vertexCount, UINT startVertex);
    void OnDrawIndexed(UINT indexCount, UINT startIndex, INT baseVertex);
    void OnDrawInstanced(UINT vertexCountPerInstance, UINT instanceCount, UINT startVertex, UINT startInstance);
    void OnDrawIndexedInstanced(UINT indexCountPerInstance, UINT instanceCount, UINT startIndex, INT baseVertex, UINT startInstance);
    void ResetCommandListState();

    // Must be called with the context lock held.
    // EndFrame runs the RT pipeline writing output into backbuffer (called BEFORE recording the blit).
    void EndFrame(const Rc<DxvkImage>& backbuffer, VkExtent2D remixViewportExtent = { 0u, 0u });
    // OnPresent registers the swapchain present image (called AFTER recording the blit).
    void OnPresent(const Rc<DxvkImage>& swapchainImage, VkExtent2D remixViewportExtent = { 0u, 0u });

    uint32_t getDrawCallID() const { return m_drawCallID; }
    uint32_t getAcceptedSceneDrawCount() const;

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
    bool                                 m_viewColumnMajor = false;
    // DX11_V260_PRECISE_CAMERA: the cached view location was confirmed by
    // matching candidateView x rawProjection against a stored ViewProj block
    // in the same cbuffer - decisive, because rigid-body checks alone cannot
    // tell the camera view from shadow-light views, mirror cameras, bone
    // matrices, or a stored camera-to-world.
    bool                                 m_viewConfirmed = false;
    // The confirmed location stores camera-to-world; invert on each re-read.
    bool                                 m_viewInverted = false;
    // Throttles late-session confirmation attempts to once per frame.
    uint32_t                             m_lastViewConfirmFrame = UINT32_MAX;

    // Cached world matrix cbuffer location — reduces per-draw scanning.
    // World matrices change every draw but often live at the same (stage, slot, offset).
    uint32_t                             m_worldSlot   = UINT32_MAX;
    int                                  m_worldStage  = -1;
    size_t                               m_worldOffset = SIZE_MAX;

    // Smoothed camera position — exponential moving average dampens
    // micro-jitter from floating-point rounding in cbuffer matrix extraction.
    Vector3                              m_smoothedCamPos = Vector3(0.0f);
    bool                                 m_hasPrevCamPos  = false;

    // Sole source of truth for resize transition detection. Only changes to
    // this extent trigger `m_resizeTransitionFramesRemaining` and
    // `resetScreenResolution`.
    VkExtent2D                           m_lastOutputExtent = { 0u, 0u };

    // Used only for viewport fallback. This is the Remix-owned output extent
    // (backbuffer or present image), not the game HWND client rect.
    VkExtent2D                           m_lastRemixViewportExtent = { 0u, 0u };

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
    static constexpr uint32_t kDrawsPerFlush = 512;
    // Bound per-draw frontend analysis so large meshes do not dominate DX11
    // CPU time when a real scene first appears.
    static constexpr uint32_t kMaxHashedVertices = 1024;
    static constexpr uint32_t kMaxHashedIndices = 4096;
    static constexpr uint32_t kMaxSkinningVerticesToScan = 1024;
    static constexpr uint32_t kResizeCameraGraceFrames = 45;
    static constexpr uint32_t kSceneCameraGraceFrames = 90;

    struct SubmitRejectStats {
      uint32_t total = 0;
      uint32_t accepted = 0;
      uint32_t sceneAccepted = 0;
      uint32_t realSceneAccepted = 0;
      uint32_t sceneCandidates = 0;
      uint32_t significanceCulled = 0;
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
      uint32_t texcoordGenerated = 0;
      // DX11_V280: no-TEXCOORD-layout draws whose UVs were recovered from the
      // vertex shader's output via the stream-out capture replay.
      uint32_t texcoordCaptured = 0;
      uint32_t position2D = 0;
      uint32_t noPositionBuffer = 0;
      uint32_t noIndexBuffer = 0;
      uint32_t compositeSkip = 0;
      uint32_t screenSpaceUiSkip = 0;
      uint32_t screenSpaceGarbageSkip = 0;
      uint32_t geometryHashScheduleFailed = 0;
      uint32_t forceInjectionIdle = 0;
      // DX11_V249: draws dropped because their position stream cannot be made
      // safe for the interleaver/BLAS (unconvertible format, all-garbage data,
      // or an index range whose vertex extent cannot be bounded).
      uint32_t positionFormatRejected = 0;
      uint32_t poisonedPositions = 0;
      uint32_t vertexRangeRejected = 0;
      // DX11_V277: depth-only draws (prepass/shadow re-renders) excluded so
      // geometry never enters the RT scene as stacked coincident copies.
      uint32_t depthOnlySkipped = 0;
      // DX11_V281: wireframe-fill draws (debug/editor overlays) excluded -
      // their triangles are lines on screen, not solid RT surfaces.
      uint32_t wireframeSkipped = 0;
    };

    SubmitRejectStats                    m_submitRejectStats;
    bool                                 m_hasSeenRealSceneProjection = false;
    uint32_t                             m_drawsSinceFlush = 0;
    uint32_t                             m_resizeTransitionFramesRemaining = 0;

    // Persistence escape hatch for the occluding-helper-window heuristic.
    // If the same "occluding" extent keeps arriving, the window genuinely
    // became small (user resized below the heuristic floor) and must
    // eventually be accepted instead of being rejected forever. The counter
    // accumulates rejection events (up to ~4 per frame: output + viewport
    // tracker in both EndFrame and OnPresent), so 240 events is roughly
    // 1-2 seconds at 60 fps.
    VkExtent2D                           m_pendingRejectedExtent = { 0u, 0u };
    uint32_t                             m_pendingRejectedExtentCount = 0;
    static constexpr uint32_t kRejectedExtentAcceptEvents = 240;

    // Resize debounce: deferred pipelines bind several RT sizes per frame at
    // scene transitions, which previously re-triggered resize grace every
    // frame (a "resize storm" - 8 consecutive camera carry-forwards observed
    // in Saints Row IV at the scene->loading boundary). A new output extent
    // must persist for this many consecutive observations before it commits.
    VkExtent2D m_pendingResizeExtent = { 0u, 0u };
    uint32_t   m_pendingResizeCount = 0;
    static constexpr uint32_t kResizeDebounceFrames = 3;

    // Frame id of the most recent real cbuffer projection. Lets
    // m_hasSeenRealSceneProjection decay after extended camera absence so
    // menu / loading-screen draws (which rely on viewport fallback) are not
    // permanently blocked once a session has run (Saints Row IV bug 1).
    uint32_t m_lastRealCameraFrameId = 0;

    // Minimum coverage for origin-anchored viewport-fallback acceptance once
    // a stable scene extent exists (loading screens render small origin
    // rects: SR4 uses 600x337 = 31% during loads).
    static constexpr float kMinNearOriginCoverage = 0.25f;

    // forceInjection overflow guard (Saints Row IV open world): with
    // injection forced and no usable scene found in the previous frame, cap
    // geometry submission to a probe window so camera discovery still works
    // but the RT acceleration structure cannot balloon to thousands of junk
    // instances (25k draws -> 6144-instance AS rebuilt every frame, ~1.4 s).
    uint32_t m_prevFrameSceneAccepted = 0;
    uint32_t m_prevFrameRealSceneAccepted = 0;
    static constexpr uint32_t kForceInjectionProbeDraws = 512;

    // UE-style significance manager (rtx.significanceCulling, default OFF).
    // Unreal's Significance Manager spends a fixed budget by importance, not by
    // arrival order. Applied here so that when a frame's scene draws exceed
    // rtx.maxInstanceSubmissions, the draws KEPT are the ones nearest the
    // camera (importance = camera-space depth), instead of whatever the engine
    // happened to submit first - which in a 25k-draw open world is often
    // distant terrain/skybox while the geometry you are looking at is dropped.
    // A multiplicative control loop adjusts a squared-distance threshold toward
    // the budget each frame, so it is temporal (no hard pop) and arms only when
    // real demand exceeds budget. m_significanceMaxDistanceSq == 0 means
    // "no limit / disarmed".
    float    m_significanceMaxDistanceSq = 0.0f;
    uint32_t m_prevFrameSceneCandidates = 0;

    // Single coherent policy for maintaining m_lastOutputExtent and
    // m_lastRemixViewportExtent. Called from both EndFrame and OnPresent so
    // the two paths cannot drift apart again.
    void UpdateTrackedExtents(const Rc<DxvkImage>& outputImage, VkExtent2D remixViewportExtent);

    // Synthetic placeholder textures for untextured draws, keyed by pixel
    // shader instance, so those draws are listable / pickable / taggable in
    // the texture tools. See getOrCreateUntexturedPlaceholder.
    std::unordered_map<const void*, TextureRef> m_untexturedPlaceholders;
    TextureRef getOrCreateUntexturedPlaceholder();

    Rc<DxvkSampler> getDefaultSampler() const;

    // DX11_V280_TEXCOORD_CAPTURE: recover texture coordinates for textured
    // draws whose input layout has no TEXCOORD stream by replaying the draw's
    // vertex range through a stream-out passthrough pipeline (game VS +
    // generated xfb GS, rasterization discarded) and feeding the captured
    // per-vertex UVs to the RT geometry. Engine-agnostic: keyed purely off
    // the VS output signature and device transform-feedback support.
    bool TryCaptureTexcoordsViaStreamOut(DrawCallState& dcs, RasterGeometry& geo,
                                         bool indexed, UINT count, UINT start, INT base);
    uint32_t     m_texcoordCapturesThisFrame = 0;
    VkDeviceSize m_texcoordCaptureBytesThisFrame = 0;

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
