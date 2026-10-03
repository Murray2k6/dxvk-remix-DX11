#include "d3d11_rtx.h"

// Include dxvk_device.h before any rtx headers so that dxvk_buffer.h and
// sibling headers (included bare by rtx_utils.h) are already in the TU.
#include "../dxvk/dxvk_device.h"

#include "d3d11_context.h"
#include "d3d11_buffer.h"
#include "d3d11_input_layout.h"
#include "d3d11_device.h"
#include "d3d11_view_srv.h"
#include "d3d11_sampler.h"
#include "d3d11_depth_stencil.h"
#include "d3d11_blend.h"
#include "d3d11_rasterizer.h"
#include "../../include/remix/emulator_draw_abi.h"
#include "d3d11_camera_resolver.h"
#include "d3d11_rtx_index_range.h"
#include "d3d11_engine_profile.h"
#include "d3d11_light_decode.h"
#include "../dxvk/dxvk_scoped_annotation.h"
#include "../util/util_once.h"

#include "../dxvk/imgui/dxvk_imgui.h"
#include "../dxvk/rtx_render/rtx_context.h"
#include "../dxvk/rtx_render/rtx_options.h"
#include "../dxvk/rtx_render/rtx_camera.h"
#include "../dxvk/rtx_render/rtx_camera_manager.h"
#include "../dxvk/rtx_render/rtx_scene_manager.h"
#include "../dxvk/rtx_render/rtx_light_manager.h"
#include "../dxvk/rtx_render/rtx_matrix_helpers.h"
#include "../dxvk/rtx_render/rtx_option_manager.h"
#include "../dxvk/rtx_render/rtx_debug_view.h"
#include "../dxvk/rtx_render/rtx_auto_exposure.h"
#include "../dxvk/rtx_render/rtx_tone_mapping.h"
#include "../util/util_filesys.h"

#include <cstring>
#include <cctype>
#include <chrono>
#include <cmath>
#include <algorithm>
#include <array>
#include <limits>
#include <vector>
#include <set>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <cstdio>
#include <unordered_map>

// DX11_V263_CRASH_FILTER_SAFE: defined in d3d11_main.cpp. Re-installs the
// log-only, chained unhandled-exception filter so a game crash handler
// installed after ours cannot silently eat the crash signature.
void RemixReassertCrashSignatureFilter();

namespace dxvk {

  namespace {

    // First-person/viewmodel passes draw into a reserved slice of the depth
    // range (FO4, Void stencil hands). A viewport with MinDepth == MaxDepth is
    // NOT that - it pins every fragment to one depth (REDengine sky at 0).
    bool isReservedDepthViewport(const D3D11_VIEWPORT& vp) {
      return vp.MaxDepth < 0.5f && (vp.MaxDepth - vp.MinDepth) > 1.0e-6f;
    }

    // Rows (out.c = dot(rows[c], in)) of a shader-proven position matrix,
    // whichever form the shader used: dp4 rows read directly, or mul/mad
    // columns transposed. Same element layout callers always memcpy'd.
    bool readBindingRows(const D3D11PositionTransformMatrixBinding& binding,
                         const D3D11ConstantBufferBinding& cb, Vector4 (&rows)[4]) {
      if (cb.buffer == nullptr)
        return false;
      const uint8_t* ptr = reinterpret_cast<const uint8_t*>(cb.buffer->GetMappedSlice().mapPtr);
      if (ptr == nullptr)
        return false;
      const size_t bufferSize = cb.buffer->Desc()->ByteWidth;
      const size_t base = size_t(cb.constantOffset) * 16u;
      const size_t end = cb.constantCount > 0
        ? std::min(base + size_t(cb.constantCount) * 16u, bufferSize) : bufferSize;
      Vector4 regs[4];
      for (uint32_t i = 0; i < 4; ++i) {
        const uint32_t reg = binding.constantRegisters[i];
        if (reg == UINT32_MAX) {
          // Row form: synthetic `mov w, 1` row. Column form: no translation.
          regs[i] = binding.columns ? Vector4(0.0f, 0.0f, 0.0f, 0.0f) : Vector4(0.0f, 0.0f, 0.0f, 1.0f);
          continue;
        }
        const size_t offset = base + size_t(reg) * 16u;
        if (offset + 16u > end)
          return false;
        std::memcpy(regs[i].data, ptr + offset, 16u);
      }
      for (uint32_t r = 0; r < 4; ++r) {
        rows[r] = binding.columns
          ? Vector4(regs[0][r], regs[1][r], regs[2][r], regs[3][r])
          : regs[r];
        for (uint32_t c = 0; c < 4; ++c)
          if (!std::isfinite(rows[r][c]))
            return false;
      }
      if (binding.columns && binding.affineW)
        rows[3] = Vector4(0.0f, 0.0f, 0.0f, 1.0f);
      return true;
    }

    // Factor a perspective ViewProj (column-vector Matrix4, M[col][row]) into
    // P * V with V rigid. Engines that upload only a ViewProj (CRYENGINE
    // CV_ViewProjZeroMatr, Dying Light, Mad Max, REDengine VS) have no
    // standalone projection for the scan to find. Clip w = s * forward . p,
    // so the w row gives the forward axis; x and y rows minus their forward
    // component give right/up and the focal scales; the z row must be a pure
    // multiple of forward (plus the depth offset). Returns false unless V's
    // rotation comes out orthonormal.
    bool factorViewProjection(const Matrix4& vp, Matrix4& outP, Matrix4& outV) {
      auto row = [&](uint32_t r) { return Vector3(vp[0][r], vp[1][r], vp[2][r]); };
      const Vector3 r3 = row(3);
      const float s = length(r3);
      if (!std::isfinite(s) || s < 1.0e-6f)
        return false;
      const Vector3 f = r3 * (1.0f / s);
      const float tf = vp[3][3] / s;

      auto split = [&](uint32_t r, Vector3& axis, float& scale, float& shear, float& t) {
        const Vector3 v = row(r);
        shear = dot(v, f);
        const Vector3 u = v - f * shear;
        scale = length(u);
        if (!std::isfinite(scale) || scale < 1.0e-6f)
          return false;
        axis = u * (1.0f / scale);
        t = (vp[3][r] - shear * tf) / scale;
        return true;
      };
      Vector3 right, up;
      float sx, sy, kx, ky, tx, ty;
      if (!split(0, right, sx, kx, tx) || !split(1, up, sy, ky, ty))
        return false;
      if (std::abs(dot(right, up)) > 0.02f)
        return false;
      const Vector3 r2 = row(2);
      const float pz = dot(r2, f);
      if (length(r2 - f * pz) > 1.0e-3f * std::max(length(r2), 1.0f))
        return false;

      outV = Matrix4();
      const Vector3 axes[3] = { right, up, f };
      const float trans[3] = { tx, ty, tf };
      for (uint32_t r = 0; r < 3; ++r) {
        outV[0][r] = axes[r].x;
        outV[1][r] = axes[r].y;
        outV[2][r] = axes[r].z;
        outV[3][r] = trans[r];
      }
      outP = Matrix4();
      outP[0][0] = sx; outP[2][0] = kx;
      outP[1][1] = sy; outP[2][1] = ky;
      outP[2][2] = pz; outP[3][2] = vp[3][2] - pz * tf;
      outP[2][3] = s;  outP[3][3] = 0.0f;
      for (uint32_t c = 0; c < 4; ++c)
        for (uint32_t r = 0; r < 4; ++r)
          if (!std::isfinite(outP[c][r]) || !std::isfinite(outV[c][r]))
            return false;
      return true;
    }

    // Geometry forced onto the far plane through the viewport depth range:
    // MinDepth == MaxDepth == far (0 with reversed Z, 1 with standard Z).
    // REDengine draws its sky dome this way (engine_knowledge, group A).
    bool isFarClampedViewport(const D3D11_VIEWPORT& vp, D3D11_COMPARISON_FUNC depthFunc,
                              bool engineReversedZ) {
      if (std::abs(vp.MaxDepth - vp.MinDepth) > 1.0e-6f)
        return false;
      const bool reversed = engineReversedZ
        || depthFunc == D3D11_COMPARISON_GREATER || depthFunc == D3D11_COMPARISON_GREATER_EQUAL;
      return (reversed && vp.MinDepth <= 1.0e-6f) || (!reversed && vp.MinDepth >= 1.0f - 1.0e-6f);
    }

    // Shared by ordinary and instanced capture admission. Individual replay
    // submissions are smaller; this bounds the complete draw's allocation.
    constexpr uint32_t kMaxPositionCaptureVerticesPerDraw = 2u << 20;

    bool isRenderDocAttached() {
      return ::GetModuleHandleW(L"renderdoc.dll") != nullptr;
    }

    bool isPcsx2HostProcess() {
      static const bool result = [] {
        std::string executable = env::getExeNameNoSuffix();
        std::transform(executable.begin(), executable.end(), executable.begin(),
          [](unsigned char c) { return char(std::tolower(c)); });
        return executable == "pcsx2" || executable == "pcsx2-qt"
            || executable.rfind("pcsx2-", 0) == 0;
      }();
      return result;
    }

    // Known emulator front-ends with a D3D11 backend. Their guest scene is
    // rendered into an internal framebuffer and only blitted to the window,
    // so scene classification must treat that internal target differently
    // from a PC game's auxiliary passes. Exe-name gated: PC games can never
    // take these paths.
    bool isKnownEmulatorHostProcess() {
      static const bool result = [] {
        std::string executable = env::getExeNameNoSuffix();
        std::transform(executable.begin(), executable.end(), executable.begin(),
          [](unsigned char c) { return char(std::tolower(c)); });
        return executable == "pcsx2" || executable.rfind("pcsx2-", 0) == 0
            || executable.rfind("dolphin", 0) == 0
            || executable.rfind("duckstation", 0) == 0
            || executable.rfind("ppsspp", 0) == 0;
      }();
      return result;
    }

    bool isPcsx2GsVertexLayout(const std::vector<D3D11RtxSemantic>& semantics) {
      // PCSX2 resources/shaders/dx11/tfx.fx declares the guest GS position as
      //   uint2 p : POSITION0; uint z : POSITION1;
      // These are post-transform 12.4 fixed-point screen XY plus a 32-bit GS
      // depth value, not object/world coordinates. Match the complete pair so
      // native games with an unrelated integer attribute are unaffected.
      bool packedScreenXy = false;
      bool packedScreenZ = false;

      for (const D3D11RtxSemantic& semantic : semantics) {
        if (std::strncmp(semantic.name, "POSITION", 8) != 0
         || semantic.systemValue != DxbcSystemValue::None
         || semantic.perInstance)
          continue;

        const bool integerInput = semantic.componentType == DxbcScalarType::Uint32
                               || semantic.componentType == DxbcScalarType::Sint32;
        if (!integerInput)
          continue;

        if (semantic.index == 0 && semantic.componentCount >= 2)
          packedScreenXy = true;
        else if (semantic.index == 1 && semantic.componentCount >= 1)
          packedScreenZ = true;
      }

      return packedScreenXy && packedScreenZ;
    }

    std::optional<remix::emulator::DrawMetadataV1>
    readEmulatorDrawMetadata(D3D11DeviceContext* context) {
      remix::emulator::DrawMetadataV1 metadata = {};
      UINT size = sizeof(metadata);
      const HRESULT result = context->GetPrivateData(
        remix::emulator::kDrawMetadataGuid, &size, &metadata);
      if (FAILED(result) || size != sizeof(metadata))
        return std::nullopt;
      if (!remix::emulator::validate(metadata)) {
        static uint32_t s_invalidMetadataLogCount = 0;
        if (s_invalidMetadataLogCount++ < 8u) {
          Logger::warn(str::format(
            "[D3D11Rtx][emulator-profile] Rejected invalid emulator draw metadata: hr=0x",
            std::hex, static_cast<uint32_t>(result), std::dec,
            " size=", size,
            " magic=0x", std::hex, metadata.magic, std::dec,
            " abi=", metadata.abiMajor, ".", metadata.abiMinor));
        }
        return std::nullopt;
      }
      return metadata;
    }

    const char* emulatorProviderName(remix::emulator::Provider provider) {
      switch (provider) {
        case remix::emulator::Provider::Pcsx2: return "pcsx2";
        case remix::emulator::Provider::Dolphin: return "dolphin";
        case remix::emulator::Provider::Xenia: return "xenia";
        case remix::emulator::Provider::Ppsspp: return "ppsspp";
        case remix::emulator::Provider::Cemu: return "cemu";
        case remix::emulator::Provider::DuckStation: return "duckstation";
        default: return "unknown";
      }
    }

    bool activateEmulatorProfile(const remix::emulator::DrawMetadataV1& metadata) {
      struct State {
        std::mutex mutex;
        std::string key;
        RtxOptionLayer* layer = nullptr;
      };
      static State state;

      char crcBuffer[9] = { };
      std::snprintf(crcBuffer, sizeof(crcBuffer), "%08X", metadata.gameCrc);
      const std::string crc = crcBuffer;
      const char* providerName = emulatorProviderName(metadata.provider);
      const std::string titleKey = str::format(metadata.gameSerial, "_", crc);
      const std::string profileKey = str::format(providerName, "_", titleKey);

      std::lock_guard<std::mutex> lock(state.mutex);
      if (state.layer != nullptr && state.key == profileKey)
        return true;

      if (state.layer != nullptr) {
        if (state.layer->hasUnsavedChanges())
          state.layer->save();
        RtxOptionLayer::clearRtxConfLayerOverride();
        RtxOptionManager::releaseLayer(state.layer);
        state.layer = nullptr;
        state.key.clear();
        util::RtxFileSys::clearEmulatedGameProfileRoot();
      }

      const std::filesystem::path profileRoot =
        util::RtxFileSys::rootPath() / "rtx-remix" / "emulators" /
        providerName / titleKey;
      // Keep configs beside the host executable, matching normal PC-game
      // Remix deployment. The ID suffix prevents one emulator's titles from
      // overwriting one another while retaining ordinary rtx.* syntax.
      const std::filesystem::path configPath = util::RtxFileSys::rootPath()
        / str::format("rtx.", titleKey, ".conf");
      if (!util::createDirectories(profileRoot))
        return false;

      if (!std::filesystem::exists(configPath)) {
        auto config = util::createDirectoriesAndOpenFile(configPath);
        if (!config)
          return false;
        *config << "# Standard RTX Remix configuration for " << providerName
                << " title " << metadata.gameSerial << " (CRC " << crc << ").\n"
                << "# Texture categories edited in the Remix developer menu and\n"
                << "# exporter-compatible rtx.* settings are saved in this file.\n";
        if (metadata.coordinateSpace ==
              remix::emulator::CoordinateSpace::Pcsx2GsPostTransform) {
          // Post-transform guest positions change with the guest camera every
          // frame; hashing them would give every mesh a new identity whenever
          // the camera moves, breaking tagging, replacements and USD capture.
          // Both hash rules have runtime onChange handlers, so these take
          // effect the moment this per-title layer is activated. BLAS vertex
          // updates are unaffected (rules::VertexDataHash is fixed).
          *config << "\n"
                  << "# Camera-independent mesh identity for post-transform guest geometry.\n"
                  << "rtx.geometryGenerationHashRuleString = texcoords,indices,geometrydescriptor\n"
                  << "rtx.geometryAssetHashRuleString = texcoords,indices,geometrydescriptor\n"
                  << "\n"
                  << "# Synthesized guest camera: set to this game's real vertical FOV so\n"
                  << "# world proportions and the free camera feel correct.\n"
                  << "rtx.emulator.cameraFovDegrees = 60.0\n";
        }
      }

      const std::string layerName = str::format(providerName, " ", titleKey, " rtx.conf");
      state.layer = RtxOptionManager::acquireLayer(
        configPath.string(),
        { kDefaultDynamicRtxOptionLayerPriority, layerName },
        1.0f, 0.1f, false, nullptr);
      if (state.layer == nullptr ||
          !RtxOptionLayer::setRtxConfLayerOverride(state.layer)) {
        RtxOptionManager::releaseLayer(state.layer);
        state.layer = nullptr;
        return false;
      }

      util::RtxFileSys::setEmulatedGameProfileRoot(profileRoot);
      state.key = profileKey;
      Logger::info(str::format(
        "[D3D11Rtx][emulator-profile] Activated authenticated emulator title '",
        profileKey, "': config=", configPath.string(),
        " captures=", (profileRoot / "captures").string(),
        " (standard Remix USD exporter)."));
      return true;
    }

    Matrix4 makeEmulatorProjection(float viewportWidth, float viewportHeight) {
      // DX11_V284_EMULATOR_CAMERA: the projection used to hardcode a 60-degree
      // FOV. It is now per-title tunable (rtx.emulator.* live in the
      // auto-created rtx.<SERIAL>_<CRC>.conf) so each game's real FOV can be
      // dialed in for correct world proportions.
      const float aspect = viewportWidth > 0.0f && viewportHeight > 0.0f
        ? viewportWidth / viewportHeight : 4.0f / 3.0f;
      const float fovDegrees = std::clamp(
        RtxOptions::Emulator::cameraFovDegrees(), 20.0f, 140.0f);
      const float fovY = fovDegrees * (3.14159265f / 180.0f);
      const float nearZ = std::max(RtxOptions::Emulator::cameraNearPlane(), 0.001f);
      const float farZ = std::max(RtxOptions::Emulator::cameraFarPlane(), nearZ * 16.0f);
      const float yScale = 1.0f / std::tan(fovY * 0.5f);
      const float xScale = yScale / aspect;
      const float q = farZ / (farZ - nearZ);
      return Matrix4(
        Vector4(xScale, 0.0f,   0.0f,       0.0f),
        Vector4(0.0f,   yScale, 0.0f,       0.0f),
        Vector4(0.0f,   0.0f,   q,          1.0f),
        Vector4(0.0f,   0.0f,  -nearZ * q, 0.0f));
    }

    Matrix4 matrixFromAbiRows(const float (&rows)[16]) {
      return Matrix4(
        Vector4(rows[0],  rows[1],  rows[2],  rows[3]),
        Vector4(rows[4],  rows[5],  rows[6],  rows[7]),
        Vector4(rows[8],  rows[9],  rows[10], rows[11]),
        Vector4(rows[12], rows[13], rows[14], rows[15]));
    }

    std::optional<remix::emulator::CameraMetadataV1>
    readEmulatorCameraMetadata(D3D11DeviceContext* context) {
      remix::emulator::CameraMetadataV1 metadata = {};
      UINT size = sizeof(metadata);
      const HRESULT result = context->GetPrivateData(
        remix::emulator::kCameraMetadataGuid, &size, &metadata);
      if (FAILED(result) || size != sizeof(metadata))
        return std::nullopt;
      if (!remix::emulator::validateCamera(metadata)) {
        static uint32_t s_invalidCameraLogCount = 0;
        if (s_invalidCameraLogCount++ < 8u) {
          Logger::warn(str::format(
            "[D3D11Rtx][emulator-camera] Rejected invalid emulator camera metadata: magic=0x",
            std::hex, metadata.magic, std::dec,
            " abi=", metadata.abiMajor, ".", metadata.abiMinor,
            " flags=", metadata.flags));
        }
        return std::nullopt;
      }
      for (uint32_t i = 0; i < 16; ++i) {
        if (!std::isfinite(metadata.worldToView[i])
         || !std::isfinite(metadata.viewToProjection[i]))
          return std::nullopt;
      }
      return metadata;
    }

    // DX11_V284_VIEWSPACE_CAMERA: draws that only exist in view space pin the
    // Remix camera to a fixed pose, so any real camera motion reads as the
    // entire world teleporting - broken motion vectors, smeared temporal
    // accumulation and denoising, and no usable free camera. Two independent
    // producers hit this: post-transform emulator draws (PCSX2 GS), and PC
    // games whose geometry can only be captured camera-relative because no
    // world/view matrix was proven (e.g. Skyrim SE's unconfirmed view). This
    // tracker recovers the camera's rigid motion each frame WITHOUT game
    // matrices: meshes are re-identified across frames by a stable
    // camera-independent key, giving exact 1:1 vertex correspondences between
    // the previous and current view-space positions. A Horn quaternion
    // (Kabsch) fit over those correspondences yields the inter-frame rigid
    // transform of the static world, whose inverse is the camera motion;
    // moving objects are rejected as residual outliers. The accumulated pose
    // feeds worldToView/objectToWorld so static geometry stays anchored in a
    // consistent world space, exactly like a native game with a real camera.
    // Each producer owns its own instance - emulator and PC-game camera state
    // are never mixed - and an emulator-published ABI camera block (Dolphin
    // XF registers, a PS2 VU provider) overrides the estimate entirely.
    class ViewSpaceCameraTracker {
    public:
      static constexpr uint32_t kPointsPerMesh = 8u;

      void beginFrame(uint32_t minimumSamplePoints, float maxTranslationPerFrame) {
        m_minimumSamplePoints = std::max(minimumSamplePoints, 9u);
        m_maxTranslationPerFrame = std::max(maxTranslationPerFrame, 1.0f);
        // Delayed readback can skip frames. Preserve the previous sample set
        // until another completed batch is available to solve against it.
        if (m_current.empty())
          return;
        solveAndAccumulate();
        m_previous = std::move(m_current);
        m_current.clear();
      }

      // True once real camera motion has been solved at least once. Until
      // then the pose is just the seed and callers should keep their proven
      // fallback behavior (menus and intro screens have no trackable meshes).
      bool hasConfidentPose() const {
        return m_hasEverSolved;
      }

      void addMeshSample(uint64_t meshKey, const float* viewPositions,
                         uint32_t vertexCount) {
        if (viewPositions == nullptr || vertexCount < 3u
         || m_current.size() >= kMaxTrackedMeshes
         || m_current.find(meshKey) != m_current.end())
          return;

        MeshSample sample = {};
        sample.vertexCount = vertexCount;
        const uint32_t step = std::max(1u, vertexCount / kPointsPerMesh);
        uint32_t stored = 0;
        for (uint32_t vertex = 0; vertex < vertexCount
             && stored < kPointsPerMesh; vertex += step, ++stored) {
          sample.points[stored] = Vector3(
            viewPositions[vertex * 3u + 0u],
            viewPositions[vertex * 3u + 1u],
            viewPositions[vertex * 3u + 2u]);
        }
        sample.pointCount = stored;
        m_current.emplace(meshKey, sample);
      }

      void reset() {
        m_previous.clear();
        m_current.clear();
        m_viewRotation[0] = Vector3(1.0f, 0.0f, 0.0f);
        m_viewRotation[1] = Vector3(0.0f, 1.0f, 0.0f);
        m_viewRotation[2] = Vector3(0.0f, 0.0f, 1.0f);
        m_viewTranslation = Vector3(0.0f, 0.0f, kSeedOffset);
        m_hasEverSolved = false;
      }

      // Row-vector worldToView from the accumulated column-convention pose:
      // rows are the transpose of the rotation, translation sits in row 3.
      Matrix4 worldToView() const {
        return Matrix4(
          Vector4(m_viewRotation[0].x, m_viewRotation[1].x, m_viewRotation[2].x, 0.0f),
          Vector4(m_viewRotation[0].y, m_viewRotation[1].y, m_viewRotation[2].y, 0.0f),
          Vector4(m_viewRotation[0].z, m_viewRotation[1].z, m_viewRotation[2].z, 0.0f),
          Vector4(m_viewTranslation.x, m_viewTranslation.y, m_viewTranslation.z, 1.0f));
      }

      Matrix4 viewToWorld() const {
        // Rigid inverse of the column-convention pose: R' = R^T, t' = -R^T t.
        // Emitted as a row-vector matrix, whose rotation block is (R^T)^T = R.
        const Vector3& r0 = m_viewRotation[0];
        const Vector3& r1 = m_viewRotation[1];
        const Vector3& r2 = m_viewRotation[2];
        const Vector3 invT(
          -(r0.x * m_viewTranslation.x + r1.x * m_viewTranslation.y + r2.x * m_viewTranslation.z),
          -(r0.y * m_viewTranslation.x + r1.y * m_viewTranslation.y + r2.y * m_viewTranslation.z),
          -(r0.z * m_viewTranslation.x + r1.z * m_viewTranslation.y + r2.z * m_viewTranslation.z));
        return Matrix4(
          Vector4(r0.x, r0.y, r0.z, 0.0f),
          Vector4(r1.x, r1.y, r1.z, 0.0f),
          Vector4(r2.x, r2.y, r2.z, 0.0f),
          Vector4(invT.x, invT.y, invT.z, 1.0f));
      }

    private:
      static constexpr size_t kMaxTrackedMeshes = 512;
      static constexpr float kSeedOffset = 0.001f; // non-identity view gate

      struct MeshSample {
        uint32_t vertexCount = 0;
        uint32_t pointCount = 0;
        Vector3 points[kPointsPerMesh];
      };

      struct RigidTransform {
        Vector3 rotation[3]; // column-convention rows of R
        Vector3 translation;
      };

      static Vector3 rotate(const Vector3 (&rotation)[3], const Vector3& p) {
        return Vector3(
          rotation[0].x * p.x + rotation[0].y * p.y + rotation[0].z * p.z,
          rotation[1].x * p.x + rotation[1].y * p.y + rotation[1].z * p.z,
          rotation[2].x * p.x + rotation[2].y * p.y + rotation[2].z * p.z);
      }

      // Horn's closed-form absolute orientation: dominant eigenvector of the
      // 4x4 quaternion matrix built from the covariance, found by shifted
      // power iteration (eigenvalues are bounded by the covariance norm, so
      // the shift makes the maximum eigenvalue strictly dominant).
      static bool solveRigid(const std::vector<Vector3>& from,
                             const std::vector<Vector3>& to,
                             RigidTransform& result) {
        const size_t n = from.size();
        if (n < 3 || to.size() != n)
          return false;

        Vector3 centroidFrom(0.0f, 0.0f, 0.0f), centroidTo(0.0f, 0.0f, 0.0f);
        for (size_t i = 0; i < n; ++i) {
          centroidFrom += from[i];
          centroidTo += to[i];
        }
        const float invN = 1.0f / float(n);
        centroidFrom *= invN;
        centroidTo *= invN;

        float h[3][3] = {};
        for (size_t i = 0; i < n; ++i) {
          const Vector3 a = from[i] - centroidFrom;
          const Vector3 b = to[i] - centroidTo;
          h[0][0] += a.x * b.x; h[0][1] += a.x * b.y; h[0][2] += a.x * b.z;
          h[1][0] += a.y * b.x; h[1][1] += a.y * b.y; h[1][2] += a.y * b.z;
          h[2][0] += a.z * b.x; h[2][1] += a.z * b.y; h[2][2] += a.z * b.z;
        }

        const float traceH = h[0][0] + h[1][1] + h[2][2];
        float norm = 0.0f;
        for (int r = 0; r < 3; ++r)
          for (int c = 0; c < 3; ++c)
            norm += h[r][c] * h[r][c];
        norm = std::sqrt(norm);
        if (!std::isfinite(norm))
          return false;
        if (norm < 1.0e-12f) {
          // Pure translation (degenerate point cloud): identity rotation.
          result.rotation[0] = Vector3(1.0f, 0.0f, 0.0f);
          result.rotation[1] = Vector3(0.0f, 1.0f, 0.0f);
          result.rotation[2] = Vector3(0.0f, 0.0f, 1.0f);
          result.translation = centroidTo - centroidFrom;
          return true;
        }

        float nMat[4][4] = {
          { traceH,          h[1][2] - h[2][1], h[2][0] - h[0][2], h[0][1] - h[1][0] },
          { h[1][2] - h[2][1], h[0][0] - h[1][1] - h[2][2], h[0][1] + h[1][0], h[2][0] + h[0][2] },
          { h[2][0] - h[0][2], h[0][1] + h[1][0], h[1][1] - h[0][0] - h[2][2], h[1][2] + h[2][1] },
          { h[0][1] - h[1][0], h[2][0] + h[0][2], h[1][2] + h[2][1], h[2][2] - h[0][0] - h[1][1] },
        };
        const float shift = 2.0f * norm;
        for (int d = 0; d < 4; ++d)
          nMat[d][d] += shift;

        float quat[4] = { 1.0f, 0.0f, 0.0f, 0.0f };
        for (int iteration = 0; iteration < 96; ++iteration) {
          float next[4] = {};
          for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c)
              next[r] += nMat[r][c] * quat[c];
          const float length = std::sqrt(next[0] * next[0] + next[1] * next[1]
                                       + next[2] * next[2] + next[3] * next[3]);
          if (!(length > 1.0e-20f))
            return false;
          for (int d = 0; d < 4; ++d)
            quat[d] = next[d] / length;
        }

        const float w = quat[0], x = quat[1], y = quat[2], z = quat[3];
        result.rotation[0] = Vector3(
          1.0f - 2.0f * (y * y + z * z), 2.0f * (x * y - w * z), 2.0f * (x * z + w * y));
        result.rotation[1] = Vector3(
          2.0f * (x * y + w * z), 1.0f - 2.0f * (x * x + z * z), 2.0f * (y * z - w * x));
        result.rotation[2] = Vector3(
          2.0f * (x * z - w * y), 2.0f * (y * z + w * x), 1.0f - 2.0f * (x * x + y * y));
        result.translation = centroidTo - rotate(result.rotation, centroidFrom);

        for (int r = 0; r < 3; ++r)
          if (!std::isfinite(result.rotation[r].x) || !std::isfinite(result.rotation[r].y)
           || !std::isfinite(result.rotation[r].z))
            return false;
        return std::isfinite(result.translation.x)
            && std::isfinite(result.translation.y)
            && std::isfinite(result.translation.z);
      }

      void solveAndAccumulate() {
        if (m_previous.empty() || m_current.empty())
          return;

        struct MeshPairs {
          size_t firstPoint;
          size_t pointCount;
          float residual;
        };
        std::vector<Vector3> fromPoints, toPoints;
        std::vector<MeshPairs> meshes;
        for (const auto& [key, current] : m_current) {
          const auto previous = m_previous.find(key);
          if (previous == m_previous.end()
           || previous->second.vertexCount != current.vertexCount
           || previous->second.pointCount != current.pointCount)
            continue;
          meshes.push_back({ fromPoints.size(), current.pointCount, 0.0f });
          for (uint32_t i = 0; i < current.pointCount; ++i) {
            fromPoints.push_back(previous->second.points[i]);
            toPoints.push_back(current.points[i]);
          }
        }

        if (meshes.size() < 3 || fromPoints.size() < m_minimumSamplePoints)
          return;

        RigidTransform contentMotion;
        if (!solveRigid(fromPoints, toPoints, contentMotion))
          return;

        // Reject moving objects: they disagree with the dominant (static
        // world) motion. Drop meshes whose residual exceeds a multiple of the
        // median residual and refit once from the survivors.
        for (auto& mesh : meshes) {
          float residual = 0.0f;
          for (size_t i = 0; i < mesh.pointCount; ++i) {
            const size_t point = mesh.firstPoint + i;
            const Vector3 predicted =
              rotate(contentMotion.rotation, fromPoints[point]) + contentMotion.translation;
            residual += length(predicted - toPoints[point]);
          }
          mesh.residual = residual / float(mesh.pointCount);
        }
        std::vector<float> residuals;
        residuals.reserve(meshes.size());
        for (const auto& mesh : meshes)
          residuals.push_back(mesh.residual);
        std::nth_element(residuals.begin(),
          residuals.begin() + residuals.size() / 2, residuals.end());
        const float medianResidual = residuals[residuals.size() / 2];
        const float residualLimit = std::max(4.0f * medianResidual, 1.0e-3f);

        std::vector<Vector3> inlierFrom, inlierTo;
        size_t inlierMeshes = 0;
        for (const auto& mesh : meshes) {
          if (mesh.residual > residualLimit)
            continue;
          ++inlierMeshes;
          for (size_t i = 0; i < mesh.pointCount; ++i) {
            inlierFrom.push_back(fromPoints[mesh.firstPoint + i]);
            inlierTo.push_back(toPoints[mesh.firstPoint + i]);
          }
        }
        if (inlierMeshes >= 3 && inlierFrom.size() >= m_minimumSamplePoints
         && inlierFrom.size() < fromPoints.size()) {
          RigidTransform refit;
          if (solveRigid(inlierFrom, inlierTo, refit))
            contentMotion = refit;
        }

        // Scene-cut guard: an implausible jump means the content was replaced,
        // not moved. Keep the current pose; world consistency is preserved
        // because geometry and camera continue to use the same accumulated V.
        const float translationMagnitude = length(contentMotion.translation);
        const float maxTranslation = m_maxTranslationPerFrame;
        const float rotationTrace = contentMotion.rotation[0].x
                                  + contentMotion.rotation[1].y
                                  + contentMotion.rotation[2].z;
        // trace = 1 + 2cos(angle); trace < 0 is > ~104 degrees in one frame.
        if (translationMagnitude > maxTranslation || rotationTrace < 0.0f)
          return;

        // Static world content moved by T in view space => the camera moved by
        // T^-1: accumulate V_k = T * V_{k-1} (column convention).
        Vector3 newRotation[3];
        for (int r = 0; r < 3; ++r) {
          newRotation[r] = Vector3(
            contentMotion.rotation[r].x * m_viewRotation[0].x
              + contentMotion.rotation[r].y * m_viewRotation[1].x
              + contentMotion.rotation[r].z * m_viewRotation[2].x,
            contentMotion.rotation[r].x * m_viewRotation[0].y
              + contentMotion.rotation[r].y * m_viewRotation[1].y
              + contentMotion.rotation[r].z * m_viewRotation[2].y,
            contentMotion.rotation[r].x * m_viewRotation[0].z
              + contentMotion.rotation[r].y * m_viewRotation[1].z
              + contentMotion.rotation[r].z * m_viewRotation[2].z);
        }
        const Vector3 newTranslation =
          rotate(contentMotion.rotation, m_viewTranslation) + contentMotion.translation;

        // Renormalize the rotation so numerical error cannot accumulate into
        // shear across thousands of frames (Gram-Schmidt).
        newRotation[0] = normalize(newRotation[0]);
        newRotation[1] = normalize(
          newRotation[1] - newRotation[0] * dot(newRotation[0], newRotation[1]));
        newRotation[2] = cross(newRotation[0], newRotation[1]);

        m_viewRotation[0] = newRotation[0];
        m_viewRotation[1] = newRotation[1];
        m_viewRotation[2] = newRotation[2];
        m_viewTranslation = newTranslation;
        m_hasEverSolved = true;
      }

      std::unordered_map<uint64_t, MeshSample> m_previous;
      std::unordered_map<uint64_t, MeshSample> m_current;
      uint32_t m_minimumSamplePoints = 24u;
      float m_maxTranslationPerFrame = 500.0f;
      bool m_hasEverSolved = false;
      Vector3 m_viewRotation[3] = {
        Vector3(1.0f, 0.0f, 0.0f),
        Vector3(0.0f, 1.0f, 0.0f),
        Vector3(0.0f, 0.0f, 1.0f),
      };
      Vector3 m_viewTranslation = Vector3(0.0f, 0.0f, kSeedOffset);
    };

    // Both producers drive one immediate context from a single app/GS thread,
    // so plain file-scope state is safe here; deferred contexts never reach
    // these paths. The two trackers are deliberately SEPARATE instances:
    // emulator camera state and PC-game camera state must never mix.
    ViewSpaceCameraTracker s_emulatorCamera;
    uint64_t s_emulatorCameraFrameId = ~0ull;
    std::optional<remix::emulator::CameraMetadataV1> s_emulatorPublishedCamera;

    // PC world units vary per engine; Skyrim units are ~1.4 cm so sprinting
    // is ~100 units/frame. 2000 comfortably covers vehicles without letting
    // teleports/scene cuts through.
    constexpr uint32_t kPcCameraMinSamplePoints = 24u;
    constexpr float kPcCameraMaxTranslationPerFrame = 2000.0f;

    // DX11_V319_WORLD_ANCHOR_CAMERA: recover the camera's WORLD POSITION for
    // engines that render camera-relative.
    //
    // Creation Engine (and several other D3D11 engines) subtract the eye
    // position on the CPU: every per-object world matrix is already relative
    // to the camera, and the view matrix in the cbuffer is a pure rotation
    // whose translation column is exactly zero. Remix derives the camera
    // position from that translation, so a zero translation pins the RT camera
    // at the world origin forever while capturedToWorld = inverse(worldToView)
    // stays a pure rotation. Captured vertices then land in a camera-CENTRED,
    // world-ORIENTED frame: the rotation cancels correctly, the player's
    // translation never does, and the whole scene slides past a camera that
    // does not move. Raster alignment still matches the game exactly, because
    // both errors cancel in worldToView * objectToWorld - which is why the only
    // visible symptom is "the geometry moves with the player".
    //
    // The missing translation P is solved from the captured geometry itself.
    // For a static world point p seen in view space as v under the game's view
    // rotation R, the camera-relative offset is
    //     q = R^T * v = p - P
    // so the SAME mesh in two consecutive frames gives
    //     q_prev - q_cur = P_cur - P_prev
    // with the rotation fully cancelled. Unlike a full pose solve this cannot
    // accumulate rotational drift, and it needs no engine-specific knowledge of
    // where the camera position lives in the game's constant buffers.
    //
    // Moving content - NPCs, foliage, the first-person weapon - disagrees with
    // the static world, so the per-axis MEDIAN across all matched meshes is
    // taken rather than the mean: a minority of movers cannot shift it.
    //
    // P is only ever defined up to the arbitrary origin picked at the first
    // solve. That is fine, and is the same freedom any engine has: the camera
    // and every mesh are anchored with the SAME P, so the RT world is
    // self-consistent no matter where it starts.
    class CameraRelativeWorldAnchor {
    public:
      // One camera-relative world offset q = R^T * v for a mesh this frame.
      void addSample(uint64_t meshKey, const Vector3& offset) {
        if (m_current.size() >= kMaxTrackedMeshes)
          return;
        if (!std::isfinite(offset.x) || !std::isfinite(offset.y)
         || !std::isfinite(offset.z))
          return;
        m_current.emplace(meshKey, offset);
      }

      // Solve this frame's translation delta against the previous frame's
      // samples, then advance the window. Called once per frame.
      void endFrame(float maxTranslationPerFrame) {
        // A frame that produced nothing - a menu, a paused scene, or simply an
        // extra call - must not advance the window: replacing the previous
        // samples with an empty set destroys every correspondence and the
        // solve would have to start over. Keeping them means the next frame
        // with real samples pairs against the last frame that had them, and
        // the scene-cut guard below rejects the pairing if too much moved in
        // between.
        if (m_current.empty())
          return;
        solve(std::max(maxTranslationPerFrame, 1.0f));
        m_previous = std::move(m_current);
        m_current.clear();
      }

      // False until real motion has been solved at least once. Callers must
      // keep their existing behavior until then: before the first solve the
      // position is only the seed, and menus/loading screens never produce one.
      bool hasPosition() const { return m_hasSolved; }
      const Vector3& position() const { return m_position; }
      const Vector3& lastDelta() const { return m_lastDelta; }
      uint32_t lastMatchedMeshes() const { return m_lastMatchedMeshes; }

      void reset() {
        m_previous.clear();
        m_current.clear();
        m_position = Vector3(0.0f, 0.0f, 0.0f);
        m_lastDelta = Vector3(0.0f, 0.0f, 0.0f);
        m_lastMatchedMeshes = 0;
        m_hasSolved = false;
      }

    private:
      // A frame drawing thousands of meshes does not need thousands of votes;
      // the median is already stable at a few dozen, and the sampling budget
      // upstream is smaller than this anyway.
      static constexpr size_t kMaxTrackedMeshes = 128;
      // Below this the median stops being a majority vote and a couple of
      // moving objects could carry the whole estimate.
      static constexpr size_t kMinMatchedMeshes = 4;

      static float medianOf(std::vector<float>& values) {
        const size_t middle = values.size() / 2;
        std::nth_element(values.begin(), values.begin() + middle, values.end());
        return values[middle];
      }

      void solve(float maxTranslationPerFrame) {
        m_lastMatchedMeshes = 0;
        if (m_previous.empty() || m_current.empty())
          return;

        std::vector<float> deltaX, deltaY, deltaZ;
        for (const auto& [meshKey, current] : m_current) {
          const auto previous = m_previous.find(meshKey);
          if (previous == m_previous.end())
            continue;
          // q_prev - q_cur == P_cur - P_prev for anything that did not move.
          const Vector3 delta = previous->second - current;
          deltaX.push_back(delta.x);
          deltaY.push_back(delta.y);
          deltaZ.push_back(delta.z);
        }

        if (deltaX.size() < kMinMatchedMeshes)
          return;
        m_lastMatchedMeshes = uint32_t(deltaX.size());

        const Vector3 delta(medianOf(deltaX), medianOf(deltaY), medianOf(deltaZ));
        if (!std::isfinite(delta.x) || !std::isfinite(delta.y)
         || !std::isfinite(delta.z))
          return;

        // A jump this large is a teleport, a cell load or a cut, not motion.
        // Dropping it re-anchors the world where the player arrived instead of
        // dragging the accumulated position across the discontinuity.
        if (length(delta) > maxTranslationPerFrame)
          return;

        m_position += delta;
        m_lastDelta = delta;
        m_hasSolved = true;
      }

      std::unordered_map<uint64_t, Vector3> m_previous;
      std::unordered_map<uint64_t, Vector3> m_current;
      Vector3  m_position = Vector3(0.0f, 0.0f, 0.0f);
      Vector3  m_lastDelta = Vector3(0.0f, 0.0f, 0.0f);
      uint32_t m_lastMatchedMeshes = 0;
      bool     m_hasSolved = false;
    };


    // The rotation-only inverse of a view matrix. Column i of R^T is row i of
    // R, which in this column-major Matrix4 is (m[0][i], m[1][i], m[2][i]).
    // Deliberately NOT inverse(worldToView): once the anchor is applied that
    // matrix carries the very translation being solved for, and differencing
    // offsets that already contain it would cancel the motion being measured.
    Matrix4 viewRotationToWorld(const Matrix4& worldToView) {
      return Matrix4(
        Vector4(worldToView[0][0], worldToView[1][0], worldToView[2][0], 0.0f),
        Vector4(worldToView[0][1], worldToView[1][1], worldToView[2][1], 0.0f),
        Vector4(worldToView[0][2], worldToView[1][2], worldToView[2][2], 0.0f),
        Vector4(0.0f, 0.0f, 0.0f, 1.0f));
    }

    // Mirror of the interleaver's clip -> position reconstruction
    // (interleave_geometry.h) so a CPU sample lands in exactly the space the
    // RT geometry built from the same bytes does.
    bool unprojectCapturedClip(const Matrix4& clipToPosition, bool clipUsesWDepth,
                               const float* clip, Vector3& position) {
      if (!std::isfinite(clip[0]) || !std::isfinite(clip[1])
       || !std::isfinite(clip[2]) || !std::isfinite(clip[3]))
        return false;

      if (clipUsesWDepth) {
        const float invXScale = clipToPosition[0][0];
        const float invYScale = clipToPosition[1][1];
        if (!std::isfinite(invXScale) || !std::isfinite(invYScale)
         || std::abs(clip[3]) < 1.0e-20f)
          return false;
        position = Vector3(clip[0] * invXScale, clip[1] * invYScale, clip[3]);
        return std::isfinite(position.x) && std::isfinite(position.y) && std::isfinite(position.z);
      }

      const Vector4 p = clipToPosition * Vector4(clip[0], clip[1], clip[2], clip[3]);
      if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z)
       || !std::isfinite(p.w) || std::abs(p.w) < 1.0e-20f)
        return false;
      const float invW = 1.0f / p.w;
      position = Vector3(p.x * invW, p.y * invW, p.z * invW);
      return std::isfinite(position.x) && std::isfinite(position.y) && std::isfinite(position.z);
    }

    // DX11_V291_CROSS_CONTEXT_DIAG: Dolphin-style hosts render the guest on a
    // different D3D11 device/context than the one that presents. Per-context
    // counters made those draws invisible ("draws=0, total=1" while the guest
    // is clearly rendering). This process-wide counter appears in the
    // EndFrame log so a single log line proves whether ANY context in the
    // process is submitting draws, and roughly how many.
    std::atomic<uint64_t> s_processWideSubmittedDraws { 0u };

    // Publisher-provided projection when available and invertible by the
    // reconstruction (standard D3D perspective shape), synthesized otherwise.
    Matrix4 effectiveEmulatorProjection(const remix::emulator::DrawMetadataV1& metadata) {
      if (s_emulatorPublishedCamera
       && (s_emulatorPublishedCamera->flags
           & remix::emulator::CameraFlagHasViewToProjection)) {
        const Matrix4 published =
          matrixFromAbiRows(s_emulatorPublishedCamera->viewToProjection);
        if (published[0][0] > 0.0f && published[1][1] > 0.0f
         && published[2][3] == 1.0f && published[2][2] > 0.0f
         && published[3][2] < 0.0f)
          return published;
      }
      return makeEmulatorProjection(metadata.viewportWidth, metadata.viewportHeight);
    }

    bool shouldInjectD3D11RtxFrame(bool hasBackbuffer,
                                   bool hasGameSceneDraws,
                                   bool hasValidCamera,
                                   bool previousSceneAvailable) {
    if (!hasBackbuffer)
      return false;

      // Escape hatch: some games never produce a camera that passes the
      // validity gates below, which permanently blocks both path tracing and
      // the Remix UI (the UI renders inside the injected composite). When
      // rtx.dx11.forceInjection is enabled in dxvk.conf, inject every frame
      // that has a backbuffer.
      //
      // DX11_V253_MENU_PASSTHROUGH: even with forceInjection, a frame with no
      // scene draws, no camera and no prior scene is a pure-UI frame (title
      // screen / menu / loading built from screen-space quads). Injecting
      // replaces it with an empty composite - the "menu renders black" bug.
      // Pass such frames through so the game's own raster shows, EXCEPT while
      // the Remix menu is open, since that menu renders inside the composite.
      // DX11_V274: hasValidCamera now means a REAL (non-identity) view camera.
      // Inject only when the frame can actually be ray traced: the Remix UI is
      // open (it renders inside the composite), OR we have a real camera this
      // frame, OR we are carrying a valid previous scene. forceInjection still
      // bypasses the stricter scene-draw/previous-scene requirements of the
      // default path, but it must NOT inject scene draws without a real camera
      // - that renders black. Camera-less frames pass through to the game's
      // raster so the screen is never black.
      // A camera alone is not a scene: game menus (e.g. Fallout 4's main menu)
      // keep a valid camera while drawing only screen-space UI. Injecting then
      // replaced the menu with an empty grey/black RT frame. Require scene
      // geometry this frame unless the Remix UI is open.
      if (RtxOptions::forceInjection()) {
        const bool remixUiOpen = RtxOptions::showUI() != UIType::None;
        return remixUiOpen
          || (hasGameSceneDraws && (hasValidCamera || previousSceneAvailable));
      }

      // First-time RTX injection needs a real scene camera. Otherwise loading
      // screens, menus, and weak viewport-fallback candidates can replace the
      // game frame with a black Remix composite. Previous scenes may only carry
      // when the current game frame still has a valid camera.
      return hasValidCamera && hasGameSceneDraws; // DX11_V124_CAMERA_ARTIFACT_STABILITY: do not inject fallback-only bootstrap/menu composite frames
    }

  }

  static std::atomic<XXH64_hash_t> s_centerPickHash { 0 };

  static uint32_t getTextureUiFeatureFlagsForView(const Rc<DxvkImageView>& imageView) {
    uint32_t textureFeatureFlags = ImGUI::kTextureFlagsDefault;

    const VkImageUsageFlags usage = imageView->imageInfo().usage;
    if ((usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) != 0 ||
        (usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0) {
      textureFeatureFlags |= ImGUI::kTextureFlagsRenderTarget;
    }

    return textureFeatureFlags;
  }

  // Map D3D11_BLEND â†’ VkBlendFactor.  Mirrors D3D11BlendState::DecodeBlendFactor
  // but kept local to avoid exposing internal statics.
  static VkBlendFactor mapD3D11Blend(D3D11_BLEND b, bool isAlpha) {
    switch (b) {
      case D3D11_BLEND_ZERO:              return VK_BLEND_FACTOR_ZERO;
      case D3D11_BLEND_ONE:               return VK_BLEND_FACTOR_ONE;
      case D3D11_BLEND_SRC_COLOR:         return VK_BLEND_FACTOR_SRC_COLOR;
      case D3D11_BLEND_INV_SRC_COLOR:     return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
      case D3D11_BLEND_SRC_ALPHA:         return VK_BLEND_FACTOR_SRC_ALPHA;
      case D3D11_BLEND_INV_SRC_ALPHA:     return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
      case D3D11_BLEND_DEST_ALPHA:        return VK_BLEND_FACTOR_DST_ALPHA;
      case D3D11_BLEND_INV_DEST_ALPHA:    return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
      case D3D11_BLEND_DEST_COLOR:        return VK_BLEND_FACTOR_DST_COLOR;
      case D3D11_BLEND_INV_DEST_COLOR:    return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
      case D3D11_BLEND_SRC_ALPHA_SAT:     return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
      case D3D11_BLEND_BLEND_FACTOR:      return isAlpha ? VK_BLEND_FACTOR_CONSTANT_ALPHA : VK_BLEND_FACTOR_CONSTANT_COLOR;
      case D3D11_BLEND_INV_BLEND_FACTOR:  return isAlpha ? VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA : VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
      case D3D11_BLEND_SRC1_COLOR:        return VK_BLEND_FACTOR_SRC1_COLOR;
      case D3D11_BLEND_INV_SRC1_COLOR:    return VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR;
      case D3D11_BLEND_SRC1_ALPHA:        return VK_BLEND_FACTOR_SRC1_ALPHA;
      case D3D11_BLEND_INV_SRC1_ALPHA:    return VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA;
      default:                            return VK_BLEND_FACTOR_ONE;
    }
  }

  // Map D3D11_BLEND_OP â†’ VkBlendOp.
  static VkBlendOp mapD3D11BlendOp(D3D11_BLEND_OP op) {
    switch (op) {
      case D3D11_BLEND_OP_ADD:          return VK_BLEND_OP_ADD;
      case D3D11_BLEND_OP_SUBTRACT:     return VK_BLEND_OP_SUBTRACT;
      case D3D11_BLEND_OP_REV_SUBTRACT: return VK_BLEND_OP_REVERSE_SUBTRACT;
      case D3D11_BLEND_OP_MIN:          return VK_BLEND_OP_MIN;
      case D3D11_BLEND_OP_MAX:          return VK_BLEND_OP_MAX;
      default:                          return VK_BLEND_OP_ADD;
    }
  }

  struct D3D11Rtx::CameraTrackingState {
    ViewSpaceCameraTracker viewSpace;
    CameraRelativeWorldAnchor worldAnchor;
  };

  D3D11Rtx::D3D11Rtx(D3D11DeviceContext* pContext)
    : m_context(pContext), m_cameraTrackingState(std::make_unique<CameraTrackingState>()) {}

  D3D11Rtx::~D3D11Rtx() = default;

  uint32_t D3D11Rtx::getAcceptedSceneDrawCount() const {
    if (m_submitRejectStats.realSceneAccepted > 0) {
      return m_submitRejectStats.realSceneAccepted;
    }

    return m_hasSeenRealSceneProjection ? 0u : m_submitRejectStats.sceneAccepted;
  }

  void D3D11Rtx::ClearMaterialTextures(LegacyMaterialData& mat) const {
    for (uint32_t i = 0; i < LegacyMaterialData::kMaxSupportedTextures; ++i) {
      mat.colorTextures[i] = TextureRef {};
      mat.samplers[i] = nullptr;
      mat.colorTextureSlot[i] = kInvalidResourceSlot;
    }

    mat.updateCachedHash();

  }

  Rc<DxvkSampler> D3D11Rtx::getDefaultSampler() const {
    if (m_defaultSampler == nullptr) {
      // D3D11 spec default: linear min/mag/mip, clamp UVW, no compare, no aniso
      DxvkSamplerCreateInfo info;
      info.magFilter      = VK_FILTER_LINEAR;
      info.minFilter      = VK_FILTER_LINEAR;
      info.mipmapMode     = VK_SAMPLER_MIPMAP_MODE_LINEAR;
      info.mipmapLodBias  = 0.0f;
      info.mipmapLodMin   = -1000.0f;
      info.mipmapLodMax   =  1000.0f;
      info.useAnisotropy  = VK_FALSE;
      info.maxAnisotropy  = 1.0f;
      info.addressModeU   = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
      info.addressModeV   = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
      info.addressModeW   = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
      info.compareToDepth = VK_FALSE;
      info.compareOp      = VK_COMPARE_OP_NEVER;
      info.borderColor    = VkClearColorValue{};
      info.usePixelCoord  = VK_FALSE;
      m_defaultSampler = m_context->m_device->createSampler(info);
    }
    return m_defaultSampler;
  }

  void D3D11Rtx::Initialize() {
    // DX11-only games do not always create the DXVK Vulkan instance path that
    // normally initializes RTX options. Do it here before any DX11 defaults or
    // UI settings touch RtxOption layers.
    RtxOptions::Create();

    // Scale geometry workers to available cores (min 2, max 6).
    // D3D11 games typically have high draw call counts, so more workers pay off.
    const uint32_t cores = std::max(2u, std::thread::hardware_concurrency());
    const uint32_t workers = std::min(std::max(cores / 2, 2u), 6u);
    m_pGeometryWorkers = std::make_unique<GeometryProcessor>(workers, "d3d11-geometry");

    // --- D3D11 sensible defaults (Default layer = lowest priority) ---
    // Written to the Default layer so rtx.conf, user.conf, and all other
    // config layers override them naturally.  Without this, setDeferred()
    // writes to the Derived layer (priority 5) which stomps rtx.conf (priority 3)
    // and makes per-game config files useless.
    const RtxOptionLayer* defaults = RtxOptionLayer::getDefaultLayer();

    // --- Graphics preset: Custom by default ---
    // The Auto/High/Medium/Low presets populate the Quality Presets layer
    // (priority 0xFFFFFFFF) with values for every UserSetting-flagged option.
    // That layer is stronger than the User Settings layer (0xFFFFFFFE) and the
    // RtxConf layer (3), so any toggle the user makes in the menu lands in a
    // weaker layer and is immediately shadowed by the preset.  Observed
    // symptom: every checkbox and dropdown reverts as soon as it is changed.
    //
    // Forcing Custom keeps the Quality layer empty, letting User and RtxConf
    // writes win the resolve.  Games that want a preset can still set
    // rtx.graphicsPreset explicitly in their rtx.conf; that value lives in a
    // stronger layer (3) and overrides this Default-layer value.
    RtxOptions::graphicsPresetObject().setDeferred(GraphicsPreset::Custom, defaults);
	RtxOptions::Shader::enableAsyncCompilationObject().setDeferred(true, defaults);

    // Universal source-level default. Manufacturer upscalers remain selectable
    // in the UI/config, but first launch should not vendor-force DLSS/XeSS.
    RtxOptions::upscalerTypeObject().setDeferred(UpscalerType::TAAU, defaults);

    // Do not force a fused world-view convention globally.
    // The D3D11 path already scans cbuffers for separate projection, view,
    // and world matrices on a per-draw basis, which is the only engine-
    // agnostic behavior that works across mixed D3D11 renderers.
    // Games that truly provide fused world/view transforms can still opt in
    // explicitly via rtx.fusedWorldViewMode, but separate-matrix engines
    // should not be coerced into View mode by default.
	RtxOptions::fusedWorldViewModeObject().setDeferred(FusedWorldViewMode::None, defaults);
	
    // Anti-culling: D3D11 engines aggressively frustum-cull objects before
    // issuing draw calls.  Without anti-culling, off-screen objects vanish
    // from reflections, shadows, and GI.
    RtxOptions::AntiCulling::Object::enableObject().setDeferred(true, defaults);
    RtxOptions::AntiCulling::Object::enableHighPrecisionAntiCullingObject().setDeferred(true, defaults);
    // DX11_V290_BOUNDED_RT_SCENE: retain enough off-camera geometry for useful
    // shadows/reflections without allowing a long play session to accumulate a
    // 20k-object, 10x-far-plane acceleration-structure workload. The old DX11
    // defaults repeatedly drove an 8-GiB NVIDIA GPU to its residency ceiling
    // immediately before nvlddmkm Event 153. These are still wider than the
    // raster camera and remain overridable by an explicit user setting.
    RtxOptions::AntiCulling::Object::numObjectsToKeepObject().setDeferred(4096u, defaults);
    RtxOptions::AntiCulling::Object::fovScaleObject().setDeferred(1.5f, defaults);
    RtxOptions::AntiCulling::Object::farPlaneScaleObject().setDeferred(4.0f, defaults);
    RtxOptions::AntiCulling::Light::enableObject().setDeferred(true, defaults);

    // Keep the always-rebuilt merged BLAS small. Large dynamic meshes get an
    // independent BLAS instead of joining a monolithic per-frame build, which
    // gives the driver smaller preemptible pieces of acceleration-structure
    // work and avoids a multi-second watchdog-visible build command.
    RtxOptions::minPrimsInDynamicBLASObject().setDeferred(256u, defaults);
    RtxOptions::maxPrimsInMergedBLASObject().setDeferred(8192u, defaults);

    // Use incoming vertex buffers directly where safe (device-local geometry).
    // NOTE: host-visible/renameable (D3D11 dynamic) buffers are ALWAYS
    // snapshotted at submit regardless of this option - see
    // DX11_V250_DYNAMIC_BUFFER_SNAPSHOT in SubmitDraw. Binding those directly
    // reads a later rename's bytes at EndFrame record time (geometry collapses
    // to a point / turns to garbage).
    RtxOptions::useBuffersDirectlyObject().setDeferred(true, defaults);

    // DX11_V288_STABLE_RT: captured DX11 alpha geometry is dynamic and its
    // material classification can change from draw to draw. Building opacity
    // micromaps for that stream adds a second GPU-side geometry build path and
    // was active immediately before the observed NVIDIA driver reset. Regular
    // any-hit/ray-query alpha testing is fully ray traced and is the robust
    // default; an explicit rtx.conf setting can still opt OMM back in.
    RtxOptions::OpacityMicromap::enableObject().setDeferred(false, defaults);

    // --- Fallback lighting ---
    // D3D11 has no legacy lighting API â€” all lighting is shader-driven,
    // so Remix never receives explicit light definitions from the application.
    // Force the fallback light to Always so the scene is lit even if there are
    // no Remix USD light assets placed yet. Keep it moderate so it prevents
    // black scenes without blowing captured materials to flat white.
    // Kept at Always per user requirement: DX11 games never provide explicit
    // lights to Remix, and the scene must never go black. If a game with real
    // Remix lights (USD mods) over-brightens, set rtx.fallbackLightMode=1
    // (NoLightsPresent) in that game's rtx.conf instead of changing this default.
    LightManager::fallbackLightModeObject().setDeferred(LightManager::FallbackLightMode::Always, defaults);
    LightManager::fallbackLightTypeObject().setDeferred(LightManager::FallbackLightType::Distant, defaults);
    // DX11_V257_FALLBACK_RADIANCE: the light stays Always-on (hard user
    // requirement - scenes must never go black), but 2.0 radiance stacked on
    // games' own emissive/baked lighting blew scenes out to white. 1.0 keeps
    // everything clearly visible while leaving auto-exposure headroom. Tune
    // per game via rtx.fallbackLightRadiance in rtx.conf if a title reads dim.
    LightManager::fallbackLightRadianceObject().setDeferred(Vector3(1.0f, 1.0f, 1.0f), defaults);
    LightManager::fallbackLightDirectionObject().setDeferred(Vector3(-0.3f, -1.0f, 0.5f), defaults);
    LightManager::fallbackLightAngleObject().setDeferred(5.0f, defaults);

    // DX11_V277_SKY_AUTODETECT: DX11 capture never categorized any draw as
    // sky, so rays that MISSED all geometry sampled an EMPTY (BLACK) sky -
    // rotating the camera between geometry and skyless void flickered
    // grey<->black. The upstream auto-detection (rtx_types.cpp: skybox draws
    // render at the camera origin with depth writes off - true in virtually
    // every engine) was simply disabled by default. Enable it so the game's
    // own skybox becomes the RT sky/environment light on any engine.
    RtxOptions::skyAutoDetectObject().setDeferred(
      SkyAutoDetectMode::CameraPositionAndDepthFlags, defaults);

    // DX11_V279_NRD_ON (supersedes the V272 off-default): the "NRD outputs
    // black" diagnosis was wrong about the denoiser - the black frames came
    // from the since-fixed real causes (identity-view camera parking the RT
    // camera at the origin [V274], rays missing into an undetected black sky
    // [V277], and stacked coincident geometry [V276/V277]). NRD's inputs
    // (motion vectors, linear viewZ) are produced by the RT gbuffer pass
    // itself, not by the capture layer, so they exist. With the black causes
    // fixed, the denoiser is REQUIRED for usable path tracing (kept at the
    // engine default: ON) together with the V266 strengthened settings.
    // Escape hatch: DXVK_REMIX_USE_NRD=0 disables it for A/B testing.
    if (env::getEnvVar("DXVK_REMIX_USE_NRD") == "0") {
      RtxOptions::useDenoiser.setDeferred(false);
    }

    // DX11_V279_GLOBAL_TONEMAP: rtx.tonemappingMode defaults to Local - the
    // exposure-histogram tonemapper, which washes out, crushes, or flickers
    // on content with erratic luminance (captured DX11 scenes with fallback
    // lighting are exactly that). Default the DX11 path to the Global filmic
    // tonemapper, which is stable across arbitrary content; per-game opt back
    // into Local via rtx.tonemappingMode=1 in rtx.conf.
    RtxOptions::tonemappingModeObject().setDeferred(TonemappingMode::Global, defaults);
  }

  void D3D11Rtx::BeginNativeRasterDrawRouting() {
    m_allowNativeRasterForCurrentDraw =
      !m_midFrameRtxInjected || m_forceRasterPassThroughThisFrame;
  }

  // Tiled / clustered deferred renderers (Fallout 4, Skyrim SE, many UE4-era
  // engines) never draw their point lights; a compute pass reads the frame's
  // light list from a CPU-written structured buffer and shades every tile.
  // With no draw to recover them from, this runtime had no game lights at all
  // and night scenes were lit by the fallback light alone. The buffer is
  // identified by shape (dynamic, CPU-written, 48-byte elements: flags,
  // view-space position, radius, linear colour, shadow data) and validated per
  // entry, so non-light data in a same-sized buffer is rejected.
  // Sun direction from the game's own shadow cascades. Every engine with a
  // directional shadow renders the scene with an orthographic light
  // ViewProj into a depth-only target; that matrix's depth row is the sun's
  // direction of travel (negated under reversed Z). Only draws whose VS
  // transform is proven (world stage, then ViewProj) are used, so the matrix
  // read is the light's, not an object's.
  void D3D11Rtx::LearnSunFromShadowDraw() {
    if (m_abDisableEngineKnowledge || m_context->m_state.vs.shader == nullptr)
      return;
    const D3D11CommonShader* vs = m_context->m_state.vs.shader->GetCommonShader();
    Vector4 rows[4];
    bool have = false;
    const D3D11CameraRelativeWorldBinding& wb = vs->GetCameraRelativeWorldBinding();
    if (wb.valid && wb.viewProjSlot < D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT) {
      D3D11PositionTransformMatrixBinding vpBinding;
      vpBinding.constantBufferSlot = wb.viewProjSlot;
      for (uint32_t r = 0; r < 4; ++r)
        vpBinding.constantRegisters[r] = wb.viewProjRegister + r;
      have = readBindingRows(vpBinding, m_context->m_state.vs.constantBuffers[wb.viewProjSlot], rows);
    } else if (const D3D11PositionTransformBinding* pb = vs->GetPositionTransformBinding()) {
      if (pb->matrixCount == 2u && pb->matrices[1].constantBufferSlot < D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT)
        have = readBindingRows(pb->matrices[1],
          m_context->m_state.vs.constantBuffers[pb->matrices[1].constantBufferSlot], rows);
    }
    if (!have)
      return;
    // Orthographic: clip w is the constant 1.
    if (std::abs(rows[3].x) + std::abs(rows[3].y) + std::abs(rows[3].z) > 1.0e-4f
     || std::abs(rows[3].w - 1.0f) > 1.0e-3f)
      return;
    Vector3 d(rows[2].x, rows[2].y, rows[2].z);
    const float len = length(d);
    if (!std::isfinite(len) || len < 1.0e-8f)
      return;
    d = d * (1.0f / len);
    if (D3D11DepthStencilState* ds = m_context->m_state.om.dsState) {
      D3D11_DEPTH_STENCIL_DESC desc;
      ds->GetDesc(&desc);
      if (desc.DepthFunc == D3D11_COMPARISON_GREATER || desc.DepthFunc == D3D11_COMPARISON_GREATER_EQUAL)
        d = Vector3(0.0f) - d;
    }
    for (auto& vote : m_sunVotes) {
      if (dot(vote.direction, d) > 0.999f) {
        ++vote.count;
        return;
      }
    }
    if (m_sunVotes.size() < 8u)
      m_sunVotes.push_back({ d, 1u });
  }

  void D3D11Rtx::ApplyLearnedSunDirection() {
    const SunVote* best = nullptr;
    for (const auto& vote : m_sunVotes)
      if (best == nullptr || vote.count > best->count)
        best = &vote;
    // The cascades are the frame's largest orthographic depth pass; a handful
    // of draws (rain occlusion, map captures) must not steer the sun.
    // Only meaningful when the RT world has the game's world axes: an exact
    // eye (camera-relative engines) or a confirmed world-space view.
    const bool gameWorldAxes = m_eyeOffset != SIZE_MAX || (m_viewConfirmed && !m_viewCameraRelative);
    if (gameWorldAxes && best != nullptr && best->count >= 16u
     && (!m_sunDirectionValid || dot(best->direction, m_sunDirection) < 0.99996f)) {
      m_sunDirection = best->direction;
      m_sunDirectionValid = true;
      // Default layer: never written to the user's config, and a direction
      // the user sets there still wins.
      LightManager::fallbackLightDirectionObject().setDeferred(m_sunDirection, RtxOptionLayer::getDefaultLayer());
      static uint32_t s_sunLogs = 0;
      if (s_sunLogs++ < 8u)
        Logger::info(str::format("[D3D11Rtx][sun] direction from shadow cascades: (",
          m_sunDirection.x, ",", m_sunDirection.y, ",", m_sunDirection.z, ") draws=", best->count));
    }
    m_sunVotes.clear();
  }

  // UE4.2x/UE5 ForwardLocalLightBuffer (LightGridCommon.ush): a typed
  // Buffer<float4>, 6 float4 per local light - PositionAndInvRadius
  // (translated world), ColorAndFalloffExponent, DirectionAndShadowMask,
  // SpotAnglesAndSourceRadiusPacked, Tangent..., RectBarnDoor... It holds
  // every local light of the view, bound to forward/translucent PS and the
  // clustered-deferred CS. Positions are camera-relative (translated world).
  bool D3D11Rtx::ImportTypedLightBuffer(const D3D11ShaderResourceBindings& views) {
    ScopedCpuProfileZoneN("D3D11Rtx::ImportTypedLightBuffer");
    const uint32_t frame = m_context->m_device->getCurrentFrameId();
    if (m_tiledLightImportFrame == frame)
      return false;
    for (uint32_t slot = 0; slot < views.views.size(); ++slot) {
      D3D11ShaderResourceView* srv = views.views[slot].ptr();
      if (srv == nullptr || srv->GetResourceType() != D3D11_RESOURCE_DIMENSION_BUFFER)
        continue;
      D3D11_SHADER_RESOURCE_VIEW_DESC viewDesc;
      srv->GetDesc(&viewDesc);
      if (viewDesc.Format != DXGI_FORMAT_R32G32B32A32_FLOAT)
        continue;
      Com<ID3D11Resource> resource;
      srv->GetResource(&resource);
      auto* buffer = static_cast<D3D11Buffer*>(resource.ptr());
      D3D11_BUFFER_DESC desc;
      buffer->GetDesc(&desc);
      if (desc.Usage != D3D11_USAGE_DYNAMIC || (desc.CPUAccessFlags & D3D11_CPU_ACCESS_WRITE) == 0
       || desc.ByteWidth < 96u)
        continue;
      const auto* bytes = reinterpret_cast<const uint8_t*>(buffer->GetMappedSlice().mapPtr);
      if (bytes == nullptr)
        continue;

      const auto& camera = m_context->m_device->getCommon()->getSceneManager()
        .getCameraManager().getCamera(CameraType::Main);
      if (!camera.isValid(frame) && !camera.isValid(frame - 1u))
        return false;
      D3D11LightDecodeView decodeView;
      decodeView.cameraWorld = camera.getViewToWorld(false) * Vector4d(0.0, 0.0, 0.0, 1.0);
      decodeView.intensityScale = RtxOptions::dx11TiledLightIntensity();
      decodeView.maxLights = RtxOptions::dx11TiledLightMaxPerFrame();
      const uint32_t lightCount = desc.ByteWidth / 96u;
      std::vector<Dx11LightDesc> lights;
      if (!D3D11DecodeUnrealLocalLights(bytes, desc.ByteWidth, decodeView, lights))
        continue;
      m_tiledLightImportFrame = frame;
      m_submitRejectStats.tiledLightsImported += uint32_t(lights.size());
      static uint32_t s_typedLightLogs = 0;
      if (s_typedLightLogs++ < 4u)
        Logger::info(str::format("[D3D11Rtx][typed-lights] ", GetD3D11EngineProfile().name(),
          " light buffer t", slot, ": imported ", lights.size(), " of ", lightCount));
      m_context->EmitCs([cLights = std::move(lights)](DxvkContext* ctx) {
        static_cast<RtxContext*>(ctx)->addLights(cLights.data(), uint32_t(cLights.size()));
      });
      return true;
    }
    return false;
  }

  // Katana Engine clustered lights (D3D11DecodeKatanaClusterLights): the
  // deferred lighting pass (a full-screen PS, or its CS form) binds
  // tCllLightPositions / tCllLightAttributes, found by their reflection
  // names, which Katana's shipped shaders keep.
  bool D3D11Rtx::ImportKatanaClusterLights(const D3D11ShaderResourceBindings& views, const D3D11CommonShader* shader) {
    ScopedCpuProfileZoneN("D3D11Rtx::ImportKatanaClusterLights");
    const uint32_t frame = m_context->m_device->getCurrentFrameId();
    if (shader == nullptr || m_tiledLightImportFrame == frame)
      return false;
    const DxbcRdef* rdef = shader->GetReflection();
    if (rdef == nullptr || !rdef->isValid())
      return false;

    uint32_t positionsSlot = UINT32_MAX, attributesSlot = UINT32_MAX, spotSlot = UINT32_MAX;
    for (const auto& binding : rdef->resourceBindings()) {
      if (binding.name == "tCllLightPositions")     positionsSlot = binding.bindPoint;
      if (binding.name == "tCllLightAttributes")    attributesSlot = binding.bindPoint;
      if (binding.name == "tCllSptLightAttributes") spotSlot = binding.bindPoint;
    }
    const bool hasPoints = positionsSlot < views.views.size() && attributesSlot < views.views.size();
    const bool hasSpots = spotSlot < views.views.size();
    if (!hasPoints && !hasSpots)
      return false;

    // CPU-written buffer bytes behind a typed buffer SRV, from its first element.
    auto viewBytes = [](D3D11ShaderResourceView* srv, uint32_t& stride, size_t& size) -> const uint8_t* {
      if (srv == nullptr || srv->GetResourceType() != D3D11_RESOURCE_DIMENSION_BUFFER)
        return nullptr;
      D3D11_SHADER_RESOURCE_VIEW_DESC viewDesc;
      srv->GetDesc(&viewDesc);
      switch (viewDesc.Format) {
        case DXGI_FORMAT_R32G32B32A32_FLOAT: stride = 16u; break;
        case DXGI_FORMAT_R32G32B32_FLOAT:    stride = 12u; break;
        default: return nullptr;
      }
      Com<ID3D11Resource> resource;
      srv->GetResource(&resource);
      auto* buffer = static_cast<D3D11Buffer*>(resource.ptr());
      D3D11_BUFFER_DESC desc;
      buffer->GetDesc(&desc);
      const auto* bytes = reinterpret_cast<const uint8_t*>(buffer->GetMappedSlice().mapPtr);
      const size_t first = size_t(viewDesc.Buffer.FirstElement) * stride;
      if (bytes == nullptr || first >= desc.ByteWidth)
        return nullptr;
      size = std::min(size_t(viewDesc.Buffer.NumElements) * stride, size_t(desc.ByteWidth) - first);
      return bytes + first;
    };

    D3D11LightDecodeView decodeView;
    decodeView.intensityScale = RtxOptions::dx11TiledLightIntensity();
    decodeView.maxLights = RtxOptions::dx11TiledLightMaxPerFrame();

    std::vector<Dx11LightDesc> lights;
    uint32_t points = 0, spots = 0;

    if (hasPoints) {
      uint32_t positionsStride = 0, attributesStride = 0;
      size_t positionsSize = 0, attributesSize = 0;
      const uint8_t* positions = viewBytes(views.views[positionsSlot].ptr(), positionsStride, positionsSize);
      const uint8_t* attributes = viewBytes(views.views[attributesSlot].ptr(), attributesStride, attributesSize);
      if (positions != nullptr && attributes != nullptr && positionsStride == 16u)
        points = D3D11DecodeKatanaClusterLights(positions, positionsSize, attributes, attributesSize,
                                                attributesStride, decodeView, lights);
    }

    if (hasSpots && lights.size() < decodeView.maxLights) {
      uint32_t spotStride = 0;
      size_t spotSize = 0;
      const uint8_t* spotBytes = viewBytes(views.views[spotSlot].ptr(), spotStride, spotSize);
      D3D11LightDecodeView spotView = decodeView;
      spotView.maxLights = decodeView.maxLights - uint32_t(lights.size());
      spots = D3D11DecodeKatanaClusterSpotLights(spotBytes, spotSize, spotStride, spotView, lights);
    }

    if (lights.empty())
      return false;

    m_tiledLightImportFrame = frame;
    m_submitRejectStats.tiledLightsImported += uint32_t(lights.size());
    static uint32_t s_katanaLightLogs = 0;
    if (s_katanaLightLogs++ < 4u)
      Logger::info(str::format("[D3D11Rtx][katana-lights] points t", positionsSlot, "/t", attributesSlot,
        ": ", points, ", spots t", spotSlot, ": ", spots));
    m_context->EmitCs([cLights = std::move(lights)](DxvkContext* ctx) {
      static_cast<RtxContext*>(ctx)->addLights(cLights.data(), uint32_t(cLights.size()));
    });
    return true;
  }

  // Tiled-light layouts and the Frostbite / HDRP spot form live in
  // d3d11_light_decode.cpp, shared with the DX12 / Vulkan front end.

  // Frostbite 3 tiled lighting (cryengine_frostbite.md): cbPunctualLightInfo
  // holds g_lightInfoPunctual[128] of BaseLightInfo, 96 B each: pos 0,
  // invSqrAttenuationRadius 12, color 16, matrixForward 32, angleScale 80,
  // angleOffset 84. Positions are camera-relative (Frostbite renders camera
  // relative); colour is pre-exposed, so only its ratio and the range are used.
  // Skyrim SE forward lights (creation_source2_gamemaker.md; Nukem9
  // BSLightingShader.cpp): the Lighting PS PerGeometry cbuffer (b2) holds
  // NumLightNumShadowLight (c0.x), PointLightPosition[7] (c1-c7: camera-relative
  // xyz, radius in w) and PointLightColor[7] (c8-c14). Each draw carries the
  // lights touching it; the frame's set is deduplicated by position + colour.
  void D3D11Rtx::CollectSkyrimDrawLights() {
    static const bool s_skyrimSE = [] {
      wchar_t path[MAX_PATH] = {};
      GetModuleFileNameW(nullptr, path, MAX_PATH);
      std::wstring exe = std::filesystem::path(path).filename().wstring();
      std::transform(exe.begin(), exe.end(), exe.begin(), ::towlower);
      return exe == L"skyrimse.exe" || exe == L"skyrimvr.exe";
    }();
    if (!s_skyrimSE)
      return;
    const auto& cb = m_context->m_state.ps.constantBuffers[2];
    constexpr size_t kBytes = 15u * 16u;
    if (cb.buffer == nullptr || cb.buffer->Desc()->ByteWidth < size_t(cb.constantOffset) * 16u + kBytes)
      return;
    const auto* bytes = reinterpret_cast<const uint8_t*>(cb.buffer->GetMappedSlice().mapPtr);
    if (bytes == nullptr)
      return;
    float h[60];
    std::memcpy(h, bytes + size_t(cb.constantOffset) * 16u, sizeof(h));
    const float count = h[0];
    if (!std::isfinite(count) || count < 1.0f || count > 7.0f || count != std::floor(count))
      return;

    const uint32_t frame = m_context->m_device->getCurrentFrameId();
    const auto& camera = m_context->m_device->getCommon()->getSceneManager()
      .getCameraManager().getCamera(CameraType::Main);
    if (!camera.isValid(frame) && !camera.isValid(frame - 1u))
      return;
    const Vector4d eye = camera.getViewToWorld(false) * Vector4d(0.0, 0.0, 0.0, 1.0);
    const float scale = RtxOptions::dx11TiledLightIntensity();
    for (uint32_t i = 0; i < uint32_t(count) && m_frameDrawLights.size() < RtxOptions::dx11TiledLightMaxPerFrame(); ++i) {
      const float* p = h + 4u + 4u * i;
      const float* c = h + 32u + 4u * i;
      const float maxColor = std::max(c[0], std::max(c[1], c[2]));
      bool valid = std::isfinite(p[3]) && p[3] > 1.0f && p[3] < 1.0e5f
        && std::isfinite(maxColor) && maxColor > 1.0e-4f && maxColor < 100.0f
        && c[0] >= 0.0f && c[1] >= 0.0f && c[2] >= 0.0f;
      for (uint32_t k = 0; k < 3 && valid; ++k)
        valid = std::isfinite(p[k]) && std::abs(p[k]) < 1.0e6f;
      if (!valid)
        continue;
      auto snap = [](double v) { return float(std::round(v * 2.0) * 0.5); };
      const float px = snap(eye.x + p[0]), py = snap(eye.y + p[1]), pz = snap(eye.z + p[2]);
      const int32_t key[6] = { int32_t(px), int32_t(py), int32_t(pz),
        int32_t(c[0] * 255.0f), int32_t(c[1] * 255.0f), int32_t(c[2] * 255.0f) };
      if (!m_frameDrawLightKeys.insert(XXH3_64bits(key, sizeof(key))).second)
        continue;
      const float brightness = std::min(std::max(maxColor * scale, 1.0e-3f), 1.0e4f);
      m_frameDrawLights.push_back(Dx11LightStateApi::makePoint(px, py, pz,
        c[0] / maxColor, c[1] / maxColor, c[2] / maxColor, p[3] * std::sqrt(brightness)));
    }
  }

  bool D3D11Rtx::ImportFrostbitePunctualLights() {
    const auto& cs = m_context->m_state.cs;
    const uint32_t frame = m_context->m_device->getCurrentFrameId();
    constexpr uint32_t kStride = 96u;
    for (uint32_t slot = 0; slot < D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT; ++slot) {
      const auto& cb = cs.constantBuffers[slot];
      if (cb.buffer == nullptr || cb.buffer->Desc()->ByteWidth < cb.constantOffset * 16u + kStride * 8u)
        continue;
      const auto* bytes = reinterpret_cast<const uint8_t*>(cb.buffer->GetMappedSlice().mapPtr);
      if (bytes == nullptr)
        continue;
      bytes += size_t(cb.constantOffset) * 16u;
      const size_t available = cb.buffer->Desc()->ByteWidth - cb.constantOffset * 16u;

      const auto& camera = m_context->m_device->getCommon()->getSceneManager()
        .getCameraManager().getCamera(CameraType::Main);
      if (!camera.isValid(frame) && !camera.isValid(frame - 1u))
        return false;

      D3D11LightDecodeView decodeView;
      decodeView.cameraWorld = camera.getViewToWorld(false) * Vector4d(0.0, 0.0, 0.0, 1.0);
      decodeView.intensityScale = RtxOptions::dx11TiledLightIntensity();
      decodeView.maxLights = RtxOptions::dx11TiledLightMaxPerFrame();

      std::vector<Dx11LightDesc> lights;
      if (!D3D11DecodeFrostbitePunctualLights(bytes, available, decodeView, lights))
        continue;
      m_tiledLightImportFrame = frame;
      m_submitRejectStats.tiledLightsImported += uint32_t(lights.size());
      m_context->EmitCs([cLights = std::move(lights)](DxvkContext* ctx) {
        static_cast<RtxContext*>(ctx)->addLights(cLights.data(), uint32_t(cLights.size()));
      });
      return true;
    }
    return false;
  }

  void D3D11Rtx::OnDispatch() {
    ScopedCpuProfileZoneN("D3D11Rtx::OnDispatch");
    if (!RtxOptions::dx11ImportTiledLights() || GetD3D11EngineProfile().chromiumHelperProcess)
      return;
    const uint32_t frame = m_context->m_device->getCurrentFrameId();
    if (m_tiledLightImportFrame == frame)
      return;

    // The engine's documented layout; engines without one keep the original
    // shape-validated FO4 layout, which rejects non-light data per entry.
    const D3D11EngineProfile& engine = GetD3D11EngineProfile();
    if (!m_abDisableEngineKnowledge
     && engine.family() == D3D11EngineFamily::Unreal
     && ImportTypedLightBuffer(m_context->m_state.cs.shaderResources))
      return;
    if (!m_abDisableEngineKnowledge && engine.family() == D3D11EngineFamily::Katana) {
      if (m_context->m_state.cs.shader != nullptr)
        ImportKatanaClusterLights(m_context->m_state.cs.shaderResources, m_context->m_state.cs.shader->GetCommonShader());
      return;
    }
    D3D11TiledLightLayout wanted = engine.facts->lights;
    if (!m_abDisableEngineKnowledge && wanted == D3D11TiledLightLayout::Frostbite96) {
      ImportFrostbitePunctualLights();
      return;
    }
    if (wanted == D3D11TiledLightLayout::None || m_abDisableEngineKnowledge)
      wanted = engine.family() == D3D11EngineFamily::Unknown || engine.family() == D3D11EngineFamily::Creation
        ? D3D11TiledLightLayout::Creation48 : D3D11TiledLightLayout::None;
    const uint32_t kStride = D3D11TiledLightStride(wanted);
    const bool requireDynamic = D3D11TiledLightRequiresDynamic(wanted);
    if (kStride == 0u)
      return;

    const auto& cs = m_context->m_state.cs;
    for (uint32_t slot = 0; slot < cs.shaderResources.views.size(); ++slot) {
      D3D11ShaderResourceView* srv = cs.shaderResources.views[slot].ptr();
      if (srv == nullptr || srv->GetResourceType() != D3D11_RESOURCE_DIMENSION_BUFFER)
        continue;
      Com<ID3D11Resource> resource;
      srv->GetResource(&resource);
      auto* buffer = static_cast<D3D11Buffer*>(resource.ptr());
      D3D11_BUFFER_DESC desc;
      buffer->GetDesc(&desc);
      if (desc.StructureByteStride != kStride
       || (requireDynamic && (desc.Usage != D3D11_USAGE_DYNAMIC
                           || (desc.CPUAccessFlags & D3D11_CPU_ACCESS_WRITE) == 0))
       || desc.ByteWidth < kStride * (requireDynamic ? 8u : 1u) || (desc.ByteWidth % kStride) != 0u)
        continue;
      const auto* bytes = reinterpret_cast<const uint8_t*>(buffer->GetMappedSlice().mapPtr);
      if (bytes == nullptr) {
        ONCE(Logger::info(str::format("[D3D11Rtx][tiled-lights] ", engine.name(),
          " light buffer (stride ", kStride, ") at t", slot, " is not CPU-visible; lights not imported")));
        continue;
      }

      const auto& camera = m_context->m_device->getCommon()->getSceneManager()
        .getCameraManager().getCamera(CameraType::Main);
      if (!camera.isValid(frame) && !camera.isValid(frame - 1u))
        return;
      D3D11LightDecodeView decodeView;
      decodeView.viewToWorld = camera.getViewToWorld(false);
      decodeView.cameraWorld = decodeView.viewToWorld * Vector4d(0.0, 0.0, 0.0, 1.0);
      decodeView.intensityScale = RtxOptions::dx11TiledLightIntensity();
      decodeView.maxLights = RtxOptions::dx11TiledLightMaxPerFrame();

      const uint32_t entryCount = desc.ByteWidth / kStride;
      std::vector<Dx11LightDesc> lights;
      lights.reserve(64);
      std::vector<Vector3> sunDirections;
      float sample[3][12] = {};
      uint32_t sampled = 0;
      D3D11DecodeTiledLights(wanted, bytes, desc.ByteWidth, decodeView, lights, &sunDirections, sample, &sampled);

      // CRYENGINE's sun entry outweighs the shadow-cascade votes.
      for (const Vector3& sunDir : sunDirections)
        m_sunVotes.push_back({ sunDir, 1000u });

      if (lights.empty())
        continue;

      m_tiledLightImportFrame = frame;
      const size_t importedCount = lights.size();
      m_submitRejectStats.tiledLightsImported += uint32_t(importedCount);
      m_context->EmitCs([cLights = std::move(lights)](DxvkContext* ctx) {
        static_cast<RtxContext*>(ctx)->addLights(cLights.data(), uint32_t(cLights.size()));
      });

      static uint32_t s_lastTiledLog = 0;
      if (frame >= s_lastTiledLog + 600u) {
        s_lastTiledLog = frame;
        const Vector3 forward = camera.getDirection(false);
        std::string raw;
        for (uint32_t s = 0; s < sampled; ++s)
          raw += str::format(" [pos=(", sample[s][1], ",", sample[s][2], ",", sample[s][3], ") r=", sample[s][4],
                             " c=(", sample[s][5], ",", sample[s][6], ",", sample[s][7], ")]");
        Logger::info(str::format("[D3D11Rtx][tiled-lights] frame=", frame, " imported=",
          importedCount, " slot=t", slot, " entries=", entryCount,
          " camFwd=(", forward.x, ",", forward.y, ",", forward.z, ")", raw));
      }
      return;
    }
  }

  bool D3D11Rtx::OnDrawAuto() {
    BeginNativeRasterDrawRouting();
    auto* buffer = m_context->m_state.ia.vertexBuffers[0].buffer.ptr();
    if (buffer != nullptr && buffer->GetSOCounter().defined()) {
      // The stream-output count is GPU-produced and cannot be captured in
      // command order. Leave only this draw out of the RT scene; forcing the
      // whole frame to raster turned every SO-using title into a raster game.
    }
    return m_allowNativeRasterForCurrentDraw;
  }

  bool D3D11Rtx::OnDraw(UINT vertexCount, UINT startVertex) {
    BeginNativeRasterDrawRouting();
    SubmitDraw(false, vertexCount, startVertex, 0);
    return m_allowNativeRasterForCurrentDraw;
  }

  bool D3D11Rtx::OnDrawIndexed(UINT indexCount, UINT startIndex, INT baseVertex) {
    BeginNativeRasterDrawRouting();
    SubmitDraw(true, indexCount, startIndex, baseVertex);
    return m_allowNativeRasterForCurrentDraw;
  }

  bool D3D11Rtx::OnDrawInstanced(UINT vertexCountPerInstance, UINT instanceCount, UINT startVertex, UINT startInstance) {
    BeginNativeRasterDrawRouting();
    SubmitInstancedDraw(false, vertexCountPerInstance, startVertex, 0, instanceCount, startInstance);
    return m_allowNativeRasterForCurrentDraw;
  }

  bool D3D11Rtx::OnDrawIndexedInstanced(UINT indexCountPerInstance, UINT instanceCount, UINT startIndex, INT baseVertex, UINT startInstance) {
    BeginNativeRasterDrawRouting();
    SubmitInstancedDraw(true, indexCountPerInstance, startIndex, baseVertex, instanceCount, startInstance);
    return m_allowNativeRasterForCurrentDraw;
  }

  bool D3D11Rtx::OnDrawInstancedIndirect(ID3D11Buffer* argumentBuffer, UINT argumentOffset) {
    BeginNativeRasterDrawRouting();
    if (argumentBuffer == nullptr)
      return m_allowNativeRasterForCurrentDraw;

    const auto* buffer = static_cast<D3D11Buffer*>(argumentBuffer);
    const size_t byteWidth = buffer->Desc()->ByteWidth;
    if ((argumentOffset & 3u) != 0u || argumentOffset > byteWidth
     || sizeof(D3D11_DRAW_INSTANCED_INDIRECT_ARGS) > byteWidth - argumentOffset)
      return m_allowNativeRasterForCurrentDraw;

    // The arguments are GPU-written (culling, compaction), so they never reach
    // the CPU: capture replays this exact indirect draw on the GPU into a
    // NaN-prefilled buffer of fixed capacity (METHODS.md, indirect draws).
    SubmitIndirectDraw(argumentBuffer, argumentOffset, false);
    return m_allowNativeRasterForCurrentDraw;
  }

  bool D3D11Rtx::OnDrawIndexedInstancedIndirect(ID3D11Buffer* argumentBuffer, UINT argumentOffset) {
    BeginNativeRasterDrawRouting();
    if (argumentBuffer == nullptr)
      return m_allowNativeRasterForCurrentDraw;

    const auto* buffer = static_cast<D3D11Buffer*>(argumentBuffer);
    const size_t byteWidth = buffer->Desc()->ByteWidth;
    if ((argumentOffset & 3u) != 0u || argumentOffset > byteWidth
     || sizeof(D3D11_DRAW_INDEXED_INSTANCED_INDIRECT_ARGS) > byteWidth - argumentOffset)
      return m_allowNativeRasterForCurrentDraw;

    // See OnDrawInstancedIndirect.
    SubmitIndirectDraw(argumentBuffer, argumentOffset, true);
    return m_allowNativeRasterForCurrentDraw;
  }

  // A draw that samples a known offscreen UI target and renders into the
  // presented image is the UI composite: place the path-traced frame right
  // before it so the HUD lands on top. Runs before any draw rejection, since
  // composites are fullscreen passes the scene filters drop.
  bool D3D11Rtx::TryInjectAtUiComposite() {
    if (m_offscreenUiTargets.empty() || m_midFrameRtxInjected || m_abDisableEngineKnowledge
     || m_submitRejectStats.realSceneAccepted == 0u || m_lastBackbufferImage == nullptr)
      return false;
    auto* rtv0 = m_context->m_state.om.renderTargetViews[0].ptr();
    Rc<DxvkImageView> targetView = rtv0 != nullptr ? rtv0->GetImageView() : nullptr;
    if (targetView == nullptr || targetView->image().ptr() != m_lastBackbufferImage)
      return false;
    bool samplesUi = false;
    const auto& views = m_context->m_state.ps.shaderResources.views;
    for (uint32_t slot = 0; slot < views.size() && !samplesUi; ++slot) {
      if (views[slot] == nullptr || views[slot]->GetResourceType() != D3D11_RESOURCE_DIMENSION_TEXTURE2D)
        continue;
      Rc<DxvkImageView> view = views[slot]->GetImageView();
      samplesUi = view != nullptr && std::find(m_offscreenUiTargets.begin(), m_offscreenUiTargets.end(),
                                               view->image().ptr()) != m_offscreenUiTargets.end();
    }
    if (!samplesUi)
      return false;
    Rc<DxvkImage> target = targetView->image();
    m_context->EmitCs([target](DxvkContext* ctx) {
      static_cast<RtxContext*>(ctx)->injectRTX(0, target);
    });
    m_midFrameRtxInjected = true;
    m_rasterUiSeenThisFrame = true;
    m_allowNativeRasterForCurrentDraw = true;
    static uint32_t s_compositeLogs = 0;
    if (s_compositeLogs++ < 8u)
      Logger::info("[D3D11Rtx][ui-layer] queued RTX before the offscreen-UI composite onto the back buffer");
    return true;
  }

  // --- 2D lift (rtx.dx11.lift2DLayers) ---
  // A 2D game has no geometry to trace: every draw is a textured quad in an
  // orthographic or pre-transformed screen space. Each such draw is captured
  // post-VS like any other and its clip position is lifted onto a plane at a
  // depth set by draw order (painter's order: later = nearer). Every plane is
  // scaled by its own depth, so it projects onto exactly the pixels the game
  // rasterized: the path-traced image matches the raster one, and the layers
  // still shadow and light each other (documentation/engine_knowledge/
  // frameworks_2d_web.md, section 12).
  namespace {
    constexpr float    kLift2DNear   = 100.0f;
    constexpr float    kLift2DFar    = 1100.0f;
    constexpr uint32_t kLift2DLayers = 16384u;
    constexpr float    kLift2DFovY   = 1.04719755f;  // 60 degrees; any FOV projects exactly
  }

  // The draw writes an orthographic clip position: the w row of its clip
  // matrix has no x/y/z term. `perspective` is the opposite proof.
  void D3D11Rtx::ClassifyClipProjection(bool& orthographic, bool& perspective) const {
    orthographic = false;
    perspective = false;
    if (m_context->m_state.vs.shader == nullptr)
      return;
    const D3D11CommonShader* vs = m_context->m_state.vs.shader->GetCommonShader();
    const D3D11PositionTransformBinding* binding = vs != nullptr ? vs->GetPositionTransformBinding() : nullptr;
    if (binding == nullptr || !binding->valid || binding->matrixCount < 1u)
      return;
    const D3D11PositionTransformMatrixBinding& clip = binding->matrices[binding->matrixCount - 1u];
    if (clip.constantBufferSlot >= D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT)
      return;
    Vector4 rows[4];
    if (!readBindingRows(clip, m_context->m_state.vs.constantBuffers[clip.constantBufferSlot], rows))
      return;
    const Vector4& w = rows[3];
    if (!std::isfinite(w.x) || !std::isfinite(w.y) || !std::isfinite(w.z) || !std::isfinite(w.w))
      return;
    const float xyz = std::abs(w.x) + std::abs(w.y) + std::abs(w.z);
    perspective = xyz > 1.0e-6f;
    orthographic = !perspective && std::abs(w.w) > 1.0e-6f;
  }

  // Lifted layers are the image the game presents: the back buffer, or the
  // offscreen target the game composites onto it (GameMaker's
  // application_surface, render-to-texture playfields). Other targets
  // (light/shadow surfaces, glyph caches) are inputs, not the scene.
  bool D3D11Rtx::IsLift2DTarget() const {
    auto* rtv = m_context->m_state.om.renderTargetViews[0].ptr();
    Rc<DxvkImageView> view = rtv != nullptr ? rtv->GetImageView() : nullptr;
    if (view == nullptr)
      return false;
    const DxvkImage* image = view->image().ptr();
    if (m_lastBackbufferImage == nullptr)
      return true;  // first frame: nothing to compare against yet
    return image == m_lastBackbufferImage || image == m_lift2DSceneTarget;
  }

  // The draw samples only render targets (a post-process or the composite of
  // an offscreen playfield onto the back buffer). Never lifted: it re-shows
  // the layers already lifted. Returns the largest sampled target.
  const DxvkImage* D3D11Rtx::Lift2DSampledRenderTarget(bool& samplesOnlyRenderTargets) const {
    samplesOnlyRenderTargets = false;
    const DxvkImage* largest = nullptr;
    uint32_t largestArea = 0, sampled = 0, renderTargets = 0;
    const D3D11CommonShader* ps = m_context->m_state.ps.shader != nullptr
      ? m_context->m_state.ps.shader->GetCommonShader() : nullptr;
    const auto& views = m_context->m_state.ps.shaderResources.views;
    for (uint32_t slot = 0; slot < views.size(); ++slot) {
      D3D11ShaderResourceView* srv = views[slot].ptr();
      if (srv == nullptr || srv->GetResourceType() != D3D11_RESOURCE_DIMENSION_TEXTURE2D)
        continue;
      if (ps != nullptr && ps->HasCompleteSampledResourceProfile() && !ps->SamplesResourceSlot(slot))
        continue;
      Rc<DxvkImageView> view = srv->GetImageView();
      if (view == nullptr)
        continue;
      ++sampled;
      if ((srv->GetResourceDesc().BindFlags & D3D11_BIND_RENDER_TARGET) == 0)
        continue;
      ++renderTargets;
      const VkExtent3D e = view->image()->info().extent;
      if (e.width * e.height > largestArea) {
        largestArea = e.width * e.height;
        largest = view->image().ptr();
      }
    }
    samplesOnlyRenderTargets = sampled > 0 && renderTargets == sampled;
    return largest;
  }

  Matrix4 D3D11Rtx::Lift2DProjection() const {
    float aspect = 16.0f / 9.0f;
    auto* rtv = m_context->m_state.om.renderTargetViews[0].ptr();
    Rc<DxvkImageView> view = rtv != nullptr ? rtv->GetImageView() : nullptr;
    if (m_lastBackbufferImage != nullptr) {
      const VkExtent3D e = m_lastBackbufferImage->info().extent;
      aspect = float(e.width) / float(std::max(e.height, 1u));
    } else if (view != nullptr) {
      const VkExtent3D e = view->image()->info().extent;
      aspect = float(e.width) / float(std::max(e.height, 1u));
    }
    // Same form as the viewport fallback camera (LH, +Z forward).
    const float nearZ  = 1.0f;
    const float farZ   = 4.0f * kLift2DFar;
    const float yScale = 1.0f / std::tan(kLift2DFovY * 0.5f);
    const float xScale = yScale / aspect;
    const float Q      = farZ / (farZ - nearZ);
    return Matrix4(
      Vector4(xScale, 0.0f,   0.0f,       0.0f),
      Vector4(0.0f,   yScale, 0.0f,       0.0f),
      Vector4(0.0f,   0.0f,   Q,          1.0f),
      Vector4(0.0f,   0.0f,  -nearZ * Q,  0.0f));
  }

  void D3D11Rtx::SubmitIndirectDraw(ID3D11Buffer* argumentBuffer, UINT argumentOffset, bool indexed) {
    if (!RtxOptions::dx11CaptureIndirectDraws() || m_abDisableEngineKnowledge
     || GetD3D11EngineProfile().chromiumHelperProcess)
      return;
    ScopedCpuProfileZoneN("D3D11Rtx::SubmitIndirectDraw");
    auto* buffer = static_cast<D3D11Buffer*>(argumentBuffer);
    m_indirectReplay.active = true;
    m_indirectReplay.indexed = indexed;
    // The same whole-buffer slice D3D11 binds for its own draw (SetDrawBuffers),
    // so the DXVK binding state stays what the D3D11 layer expects.
    m_indirectReplay.args = buffer->GetBufferSlice();
    m_indirectReplay.offset = argumentOffset;
    m_indirectReplay.identity = XXH3_64bits_withSeed(&argumentOffset, sizeof(argumentOffset),
      uint64_t(reinterpret_cast<uintptr_t>(buffer)));
    // Capacity in vertices; the vertex-pulled admission path supplies the
    // placeholder stream and makes capture the only position source.
    SubmitDraw(false, RtxOptions::dx11IndirectCaptureVertices(), 0, 0, nullptr, 0, 1, true);
    m_indirectReplay.active = false;
  }

  void D3D11Rtx::ResetCommandListState() {
    m_drawCallID = 0;
    m_drawsSinceFlush = 0;
    // DX11_V295_ROTATING_PROBE: remember the frame's draw volume and advance
    // the probe phase so the force-injection discovery window sweeps the
    // whole frame over successive frames (see SubmitDraw).
    m_prevFrameTotalDraws = m_submitRejectStats.total;

    // Re-derive the CS flush interval from the frame that just finished, so the
    // GPU gets roughly csChunkFlushesPerFrame batches to chew on while the CPU
    // records the next frame. A fixed interval starves exactly the frames that
    // should be cheapest: below the interval nothing is ever handed over early.
    {
      const int targetFlushes = csChunkFlushesPerFrame();

      if (targetFlushes <= 0) {
        // Opt-out: keep the historical fixed interval.
        m_drawsPerFlush = kMaxDrawsPerFlush;
      } else if (m_prevFrameTotalDraws == 0u) {
        // No observation yet (first frame, or a frame that captured nothing).
        m_drawsPerFlush = kInitialDrawsPerFlush;
      } else {
        const uint32_t interval = m_prevFrameTotalDraws / uint32_t(targetFlushes);
        m_drawsPerFlush = std::min(kMaxDrawsPerFlush, std::max(kMinDrawsPerFlush, interval));
      }
    }
    ++m_forceInjectionProbePhase;
    m_prevFrameStarvedCaptures = m_starvedCapturesThisFrame;
    m_starvedCapturesThisFrame = 0;

    // Per-frame counters as Tracy plots, so every runtime decision can be read
    // against frame time in a capture (zero cost when Tracy is compiled out).
    {
      const SubmitRejectStats& s = m_submitRejectStats;
      ProfilerPlotValueI64("dx11 draws total", s.total);
      ProfilerPlotValueI64("dx11 draws scene", s.sceneAccepted);
      ProfilerPlotValueI64("dx11 exact world", s.exactWorldTransform);
      ProfilerPlotValueI64("dx11 exact rebased", s.exactWorldRebased);
      ProfilerPlotValueI64("dx11 position captured", s.positionCaptured);
      ProfilerPlotValueI64("dx11 position budget rejected", s.positionCaptureBudgetRejected);
      ProfilerPlotValueI64("dx11 duplicate pass skipped", s.duplicatePassSkipped);
      ProfilerPlotValueI64("dx11 light-prepass geometry skipped", s.lightPrepassGeometrySkipped);
      ProfilerPlotValueI64("dx11 depth-only skipped", s.depthOnlySkipped);
      ProfilerPlotValueI64("dx11 tiled lights imported", s.tiledLightsImported);
      ProfilerPlotValueI64("dx11 vertex-pulled world draws", s.noLayoutWorldCandidate);
      ProfilerPlotValueI64("dx11 vertex-pulled admitted", s.vertexPulledAdmitted);
      ProfilerPlotValueI64("dx11 projected decals", s.projectedDecals);
    }
    // 2D lift: decided for this frame from the frames before it. A process
    // that has drawn a perspective 3D scene never lifts (its orthographic
    // draws are UI). Otherwise lifting starts after dx11Lift2DMinFrames
    // 2D-only frames, or after one in an engine known to draw in 2D, and
    // stays on until a perspective scene appears.
    {
      const SubmitRejectStats& s = m_submitRejectStats;
      ProfilerPlotValueI64("dx11 2d lifted", s.lift2DAccepted);
      ProfilerPlotValueI64("dx11 2d candidates", s.lift2DCandidates);
      if (m_perspectiveSceneThisFrame > 0u)
        m_seenPerspectiveScene = true;
      const bool twoDOnlyFrame = m_perspectiveSceneThisFrame == 0u && s.lift2DCandidates > 0u;
      m_lift2DStreak = twoDOnlyFrame ? m_lift2DStreak + 1u : 0u;
      const D3D11EngineFamily family = GetD3D11EngineProfile().family();
      const bool known2D = family == D3D11EngineFamily::GameMaker || family == D3D11EngineFamily::Framework2D;
      const uint32_t needed = known2D ? 1u : std::max(RtxOptions::dx11Lift2DMinFrames(), 1u);
      const bool wasLifting = m_lift2DFrame;
      m_lift2DFrame = RtxOptions::dx11Lift2DLayers() && !m_abDisableEngineKnowledge
        && !m_seenPerspectiveScene && (m_lift2DStreak >= needed || wasLifting);
      if (m_lift2DFrame != wasLifting) {
        Logger::info(str::format("[D3D11Rtx][2d-lift] ", m_lift2DFrame ? "started" : "stopped",
          " (engine=", GetD3D11EngineProfile().name(), " streak=", m_lift2DStreak,
          " perspectiveSeen=", m_seenPerspectiveScene ? 1 : 0, ")"));
      }
      SetLift2DPresentation(m_lift2DFrame);
      m_perspectiveSceneThisFrame = 0;
      m_lift2DLayer = 0;
      m_prevFrameSceneTargetWidth = m_frameSceneTargetWidth;
      m_frameSceneTargetWidth = 0;

      // Scene units: Remix renders in centimetres (light falloff, ray
      // offsets, volumetrics, atmosphere). Default layer, so a value in the
      // user's config still wins.
      static bool s_sceneScaleApplied = false;
      if (!s_sceneScaleApplied && !m_abDisableEngineKnowledge) {
        s_sceneScaleApplied = true;
        const float unitsPerCm = GetD3D11EngineUnitsPerCentimetre(family);
        if (unitsPerCm > 0.0f) {
          RtxOptions::sceneScaleObject().setDeferred(unitsPerCm, RtxOptionLayer::getDefaultLayer());
          Logger::info(str::format("[D3D11Engine] scene scale ", unitsPerCm, " game units per cm (",
            GetD3D11EngineProfile().name(), ")"));
        }
      }
      m_lift2DSceneTarget = m_lift2DSceneTargetNext;
      m_lift2DSceneTargetNext = nullptr;
    }
    ApplyLearnedSunDirection();
    m_passKeysThisFrame.clear();
    m_meshKeysThisFrame.clear();
    std::swap(m_equalPassKeysPrevFrame, m_equalPassKeysThisFrame);
    m_equalPassKeysThisFrame.clear();
    m_submitRejectStats = {};
    m_rasterUiSeenThisFrame = false;
    m_midFrameRtxInjected = false;
    m_forceRasterPassThroughThisFrame = false;
    m_allowNativeRasterForCurrentDraw = true;

    // Testing aid: while "dx11-raster-compare.flag" exists beside the game
    // executable, frames pass through as the game's own raster image, so the
    // same pose can be screenshotted raster vs path traced. Polled, not per frame.
    {
      static bool s_rasterCompare = false;
      static uint32_t s_rasterComparePoll = 0;
      if ((s_rasterComparePoll++ % 30u) == 0u) {
        s_rasterCompare = std::filesystem::exists("dx11-raster-compare.flag");
        // A/B switch for profiling a fix with and without it in one build.
        m_abDisableExactWorld = std::filesystem::exists("dx11-ab-no-exact-world.flag");
        // A/B switch for the engine-knowledge features (pass dedupe, engine
        // light layouts) so Tracy can compare them in one build.
        m_abDisableEngineKnowledge = std::filesystem::exists("dx11-ab-no-engine-knowledge.flag");
        // Testing aid: "dx11-debugview.flag" holding a DEBUG_VIEW_* index
        // selects that Remix debug view; removing the file turns it off.
        // Leaves the user's config files untouched.
        static uint32_t s_flagDebugView = 0;
        uint32_t wanted = 0;
        if (std::ifstream flag { "dx11-debugview.flag" }) {
          flag >> wanted;
        }
        if (wanted != s_flagDebugView) {
          s_flagDebugView = wanted;
          m_context->m_device->getCommon()->metaDebugView().debugViewIdx.setDeferred(wanted);
          Logger::info(str::format("[D3D11Rtx] debug view set from flag file: ", wanted));
        }
      }
      if (s_rasterCompare)
        m_forceRasterPassThroughThisFrame = true;
    }

    // Chromium helper processes show their own raster frames (see SubmitDraw).
    if (GetD3D11EngineProfile().chromiumHelperProcess)
      m_forceRasterPassThroughThisFrame = true;
  }

  // DX11_V280_TEXCOORD_CAPTURE: engine-agnostic recovery of texture
  // coordinates for textured draws whose input layout carries no usable
  // TEXCOORD stream - the UVs exist only as vertex-shader OUTPUTS (computed
  // from other attributes, instance data, or constants). The draw's vertex
  // range is replayed once through DXVK's stream-output passthrough pipeline
  // (the exact mechanism backing D3D11 CreateGeometryShaderWithStreamOutput):
  //
  //   game VS (unmodified) -> generated point-in/point-out passthrough GS
  //   with one xfb entry (TEXCOORDn.xy, buffer 0, stride 8) and
  //   rasterizedStream = -1, which dxvk turns into rasterizer discard
  //   (dxvk_graphics.cpp keys discard off the GS xfb stream) - the replay
  //   can never write a pixel or a depth value.
  //
  // The replay is a POINT_LIST draw over the draw's vertex range with the
  // game's own IA bindings: every vertex becomes one point primitive, so it
  // is processed exactly once and in order - captured vertex i IS geometry
  // vertex i. That keeps indexed geometry indexed (positions and indices stay
  // the IA-sourced ones; only the texcoord stream is new) and works for
  // strips and lists alike, since the original topology only matters to the
  // rasterizer, which is discarded.
  //
  // This is the D3D11 expression of what Unreal Engine does for ray tracing
  // (RayTracingDynamicGeometryUpdate: a dedicated GPU pass re-evaluates
  // shader-computed vertex data into a buffer the BLAS consumes), with UE's
  // cost policies applied as the per-draw and per-frame budgets below.
  bool D3D11Rtx::TryCaptureTexcoordsViaStreamOut(
      DrawCallState& dcs, RasterGeometry& geo,
      bool indexed, UINT count, UINT start, INT base) {
    ScopedCpuProfileZoneN("D3D11Rtx::TryCaptureTexcoordsViaStreamOut");
    // Kill switch for field diagnosis; capture is otherwise always available.
    static const bool s_disabled = env::getEnvVar("DXVK_REMIX_TEXCOORD_CAPTURE") == "0";
    if (s_disabled)
      return false;

    // Capability gate, not a game gate: transform feedback is present on
    // NVIDIA/AMD/Intel desktop Vulkan drivers (dxvk needs it for D3D11
    // stream output), but verify rather than assume.
    if (!m_context->m_device->features().extTransformFeedback.transformFeedback)
      return false;

    // Structural guards: the replay assumes the VS is the only stage shaping
    // vertices and that the GS slot and SO buffers are free for the capture
    // pipeline. Tessellated / GS-driven / SO-active draws pass through to the
    // existing flat-albedo fallback instead.
    if (m_context->m_state.gs.shader != nullptr
     || m_context->m_state.hs.shader != nullptr
     || m_context->m_state.ds.shader != nullptr)
      return false;

    for (const auto& soTarget : m_context->m_state.so.targets) {
      if (soTarget.buffer != nullptr)
        return false;
    }

    // Simple topologies only: the replay rebinds POINT_LIST and must restore
    // the game's input-assembly state exactly (same mapping as
    // D3D11DeviceContext::ApplyPrimitiveTopology, including strip restart).
    DxvkInputAssemblyState restoreIa = { VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_FALSE, 0 };
    switch (m_context->m_state.ia.primitiveTopology) {
      case D3D_PRIMITIVE_TOPOLOGY_POINTLIST:
        restoreIa = { VK_PRIMITIVE_TOPOLOGY_POINT_LIST, VK_FALSE, 0 };
        break;
      case D3D_PRIMITIVE_TOPOLOGY_LINELIST:
        restoreIa = { VK_PRIMITIVE_TOPOLOGY_LINE_LIST, VK_FALSE, 0 };
        break;
      case D3D_PRIMITIVE_TOPOLOGY_LINESTRIP:
        restoreIa = { VK_PRIMITIVE_TOPOLOGY_LINE_STRIP, VK_TRUE, 0 };
        break;
      case D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST:
        restoreIa = { VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_FALSE, 0 };
        break;
      case D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP:
        restoreIa = { VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP, VK_TRUE, 0 };
        break;
      default:
        return false;
    }

    // UE-style cost budgets (UE distance-limits its dynamic-geometry
    // re-evaluation and caps RT instance counts; the equivalents for a
    // per-draw capture are hard per-draw and per-frame ceilings so a
    // pathological frame degrades to the flat-albedo fallback instead of
    // stalling the GPU).
    // Transform-feedback replays share the graphics queue with BLAS builds and
    // path tracing. Bound them so a scene containing many shader-only UV
    // streams degrades to the existing flat-albedo path instead of creating a
    // long driver submission (observed as nvlddmkm 153/device-lost on an
    // 8-GiB RTX 5060). DX11_V295_CAPTURE_BUDGET: shares the per-title capture
    // budget options so Remix-native titles can capture their full scene.
    const uint32_t kMaxCapturesPerFrame =
      std::max(RtxOptions::captureMaxDrawsPerFrame(), 1);
    const VkDeviceSize kMaxCaptureBytesPerFrame =
      VkDeviceSize(std::max(RtxOptions::captureMaxMiBPerFrame(), 1)) << 20;

    constexpr uint32_t kMaxTexcoordCaptureVerticesPerDraw = 512u << 10;
    const uint32_t vertexCount = geo.vertexCount;
    if (vertexCount == 0 || vertexCount > kMaxTexcoordCaptureVerticesPerDraw)
      return false;

    const VkDeviceSize captureBytes = VkDeviceSize(vertexCount) * 8u;

    if (m_context->m_state.vs.shader == nullptr)
      return false;
    if (isIdentityExact(dcs.transformData.viewToProjection))
      return false;
    const D3D11CommonShader* commonVs = m_context->m_state.vs.shader->GetCommonShader();
    if (commonVs == nullptr || !commonVs->HasTexcoordCaptureCandidate())
      return false;

    // Compiled once per VS on first need; nullptr means the compile failed
    // (logged inside) and this VS will never capture.
    Rc<DxvkShader> captureGs = commonVs->GetTexcoordCaptureShader();
    if (captureGs == nullptr)
      return false;

    // The geometry's vertex slices were folded to begin at the draw's base
    // (indexed) / start (non-indexed) vertex, so replaying from that same
    // first vertex makes captured vertex i correspond exactly to slice
    // vertex i.
    const uint32_t firstVertex = indexed ? uint32_t(std::max(base, 0)) : start;

    // DX11_V285_TEXCOORD_CAPTURE_REUSE: key the persistent capture buffer on
    // the draw identity - the VS (fixes the GS variant and output semantic),
    // the replayed vertex range, and every bound vertex-buffer binding that
    // feeds it. Same identity => same buffer object across frames (stable for
    // the scene manager's per-frame unique-buffer table) and across the
    // multiple passes that re-draw the same mesh within one frame.
    uint64_t cacheKey = uint64_t(reinterpret_cast<uintptr_t>(commonVs));
    auto mixKey = [&cacheKey](uint64_t v) {
      cacheKey ^= v + 0x9e3779b97f4a7c15ull + (cacheKey << 6) + (cacheKey >> 2);
    };
    mixKey(firstVertex);
    mixKey(vertexCount);
    for (uint32_t slot = 0; slot < D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT; ++slot) {
      const auto& vb = m_context->m_state.ia.vertexBuffers[slot];
      if (vb.buffer == nullptr)
        continue;
      mixKey(uint64_t(reinterpret_cast<uintptr_t>(vb.buffer.ptr())));
      mixKey((uint64_t(vb.offset) << 20) | uint64_t(vb.stride) | (uint64_t(slot) << 56));
    }

    const uint32_t curFrame = m_context->m_device->getCurrentFrameId();
    TexcoordCaptureEntry& entry = m_texcoordCaptureCache[cacheKey];
    const bool haveUsableBuffer = entry.buffer != nullptr && entry.capacity >= captureBytes;
    const bool alreadyCapturedThisFrame = haveUsableBuffer && entry.lastCapturedFrame == curFrame;
    entry.lastUsedFrame = curFrame;

    if (!alreadyCapturedThisFrame) {
      // A replay costs GPU time and per-frame budget whether or not the buffer
      // already exists; only allocation is avoided on reuse.
      if (m_texcoordCapturesThisFrame >= kMaxCapturesPerFrame
       || m_texcoordCaptureBytesThisFrame + captureBytes > kMaxCaptureBytesPerFrame) {
        if (entry.buffer == nullptr)
          m_texcoordCaptureCache.erase(cacheKey);
        return false;
      }

      if (!haveUsableBuffer) {
        // Round the capacity up so vertex-count jitter between frames reuses
        // the same allocation instead of replacing it.
        VkDeviceSize capacity = 4096u;
        while (capacity < captureBytes)
          capacity <<= 1;

        DxvkBufferCreateInfo info;
        info.size   = capacity;
        info.usage  = VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_BUFFER_BIT_EXT
                    | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT
                    | VK_BUFFER_USAGE_TRANSFER_SRC_BIT
                    | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
        info.stages = VK_PIPELINE_STAGE_TRANSFORM_FEEDBACK_BIT_EXT
                    | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT
                    | VK_PIPELINE_STAGE_TRANSFER_BIT;
        info.access = VK_ACCESS_TRANSFORM_FEEDBACK_WRITE_BIT_EXT
                    | VK_ACCESS_SHADER_READ_BIT
                    | VK_ACCESS_TRANSFER_READ_BIT;

        Rc<DxvkBuffer> newBuffer = m_context->m_device->createBuffer(
          info, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
          DxvkMemoryStats::Category::RTXBuffer, "dx11 texcoord capture");
        if (newBuffer == nullptr) {
          if (entry.buffer == nullptr)
            m_texcoordCaptureCache.erase(cacheKey);
          return false;
        }
        m_texcoordCaptureCacheBytes += capacity - entry.capacity;
        entry.buffer   = std::move(newBuffer);
        entry.capacity = capacity;
        entry.lastCapturedFrame = ~0u;
      }

      // This is a dedicated device-local transform-feedback allocation. Reuse
      // it and rely on DxvkContext's XFB-write/shader-read barriers; renaming it
      // every frame retired untracked physical slices and caused the same
      // process-memory growth as the position capture path.

      m_context->EmitCs([cGs = std::move(captureGs),
                         cBuf = DxvkBufferSlice(entry.buffer, 0, captureBytes),
                         cCount = vertexCount,
                         cFirst = firstVertex,
                         cRestoreIa = restoreIa](DxvkContext* ctx) {
        const DxvkInputAssemblyState pointIa = { VK_PRIMITIVE_TOPOLOGY_POINT_LIST, VK_FALSE, 0 };
        ctx->bindShader(VK_SHADER_STAGE_GEOMETRY_BIT, cGs);
        ctx->bindXfbBuffer(0, cBuf, DxvkBufferSlice());
        ctx->setInputAssemblyState(pointIa);
        ctx->draw(cCount, 1, cFirst, 0);
        // Restore the game's exact pipeline state within this same command
        // stream position: no GS was bound (guarded above), no SO targets were
        // bound, and the original input assembly is re-applied.
        ctx->bindShader(VK_SHADER_STAGE_GEOMETRY_BIT, nullptr);
        ctx->bindXfbBuffer(0, DxvkBufferSlice(), DxvkBufferSlice());
        ctx->setInputAssemblyState(cRestoreIa);
      });

      entry.lastCapturedFrame = curFrame;
      ++m_texcoordCapturesThisFrame;
      m_texcoordCaptureBytesThisFrame += captureBytes;
    }

    const Rc<DxvkBuffer>& captureBuffer = entry.buffer;

    // Wire the captured stream into BOTH the local geometry (later checks in
    // SubmitDraw read it) and the already-copied DrawCallState payload that
    // actually reaches the RT scene. Geometry hashes were scheduled before
    // this point from IA data only, which is intentional: the capture buffer
    // is GPU-written and unreadable by the CPU hash worker, and IA-only
    // hashing keeps the hash stable per frame/run/GPU.
    const RasterBuffer capturedUvs(
      DxvkBufferSlice(captureBuffer, 0, captureBytes), 0, 8u, VK_FORMAT_R32G32_SFLOAT);
    geo.texcoordBuffer = capturedUvs;
    dcs.geometryData.texcoordBuffer = capturedUvs;

    static uint32_t sTexcoordCaptureLogCount = 0;
    if (sTexcoordCaptureLogCount < 12) {
      ++sTexcoordCaptureLogCount;
      Logger::info(str::format(
        "[D3D11Rtx] V280: captured texcoords via stream-out replay (verts=", vertexCount,
        ", indexed=", indexed ? 1 : 0,
        ", count=", count,
        ")"));
    }
    return true;
  }

  // DX11_V285_TEXCOORD_CAPTURE_REUSE: age out capture buffers whose draw
  // identity has not been seen recently (scene change, mesh unloaded). The
  // hard caps bound the cache on pathological scenes; a wholesale reset is
  // safe because entries are pure allocations - the geometry entries that
  // still reference a buffer keep it alive until they release it, and the
  // next frame simply re-captures what it needs.
  void D3D11Rtx::SweepTexcoordCaptureCache(uint32_t currentFrame) {
    static constexpr uint32_t     kEvictAfterFrames = 600u;      // ~10s at 60fps
    static constexpr size_t       kMaxEntries       = 4096u;
    static constexpr VkDeviceSize kMaxCacheBytes    = 96ull << 20;

    const bool overBudget = m_texcoordCaptureCache.size() > kMaxEntries
                         || m_texcoordCaptureCacheBytes > kMaxCacheBytes;
    if (!overBudget && (currentFrame & 255u) != 0u)
      return;

    for (auto it = m_texcoordCaptureCache.begin(); it != m_texcoordCaptureCache.end();) {
      if (it->second.lastUsedFrame + kEvictAfterFrames < currentFrame) {
        m_texcoordCaptureCacheBytes -= it->second.capacity;
        it = m_texcoordCaptureCache.erase(it);
      } else {
        ++it;
      }
    }

    if (m_texcoordCaptureCache.size() > kMaxEntries
     || m_texcoordCaptureCacheBytes > kMaxCacheBytes) {
      // More live capture identities than the cache admits even after the age
      // sweep - reset wholesale rather than thrash an LRU under pressure.
      m_texcoordCaptureCache.clear();
      m_texcoordCaptureCacheBytes = 0;
      static uint32_t sCacheResetLog = 0;
      if (sCacheResetLog < 4) {
        ++sCacheResetLog;
        Logger::info("[D3D11Rtx] V285: texcoord capture cache reset (over budget)");
      }
    }
  }

  // DX11_V290_POST_VS_POSITION_CAPTURE: replay the game VS and capture its
  // shader-computed pre-projection view position. This fixes the fundamental
  // mismatch in programmable D3D11 games where the IA POSITION is only bind
  // pose/object space while skinning and model/view transforms exist solely in
  // shader code. Feeding the IA stream to Remix produces exploded full-screen
  // quads, black moving rectangles, and a black/noisy G-buffer before lighting.
  bool D3D11Rtx::TryCapturePositionsViaStreamOut(
      DrawCallState& dcs, RasterGeometry& geo,
      bool indexed, UINT count, UINT start, INT base,
      bool hasExternalInstanceTransform,
      UINT replayFirstInstance,
      UINT replayInstanceCount,
      bool requireIndexedFlatten) {
    ScopedCpuProfileZoneN("D3D11Rtx::TryCapturePositionsViaStreamOut");
    static const bool s_disabled = env::getEnvVar("DXVK_REMIX_POSITION_CAPTURE") == "0";
    // Keep the existing developer-menu control authoritative. Previously the
    // checkbox disabled terrain vertex capture but this path ignored it and
    // continued replaying every VS, making diagnosis and safe fallback
    // impossible.
    if (s_disabled || !useVertexCapture())
      return false;

    // Explicit CPU instance transforms have already consumed the application's
    // instance state. Replaying them here would evaluate SV_InstanceID again and
    // apply placement twice. Shader-profile batches, on the other hand, pass the
    // original FirstInstance/InstanceCount and must be evaluated as one draw.
    if (hasExternalInstanceTransform || replayInstanceCount == 0)
      return false;

    if (!m_context->m_device->features().extTransformFeedback.transformFeedback)
      return false;

    // Tessellated draws (HS + DS, no GS): capture the domain shader's
    // SV_Position with a triangle-input capture GS (METHODS.md). The output
    // count is GPU-determined, so the buffer is NaN-prefilled to a capacity
    // and unwritten triangles stay inactive in the BLAS.
    const D3D11CommonShader* commonDs = m_context->m_state.ds.shader != nullptr
      ? m_context->m_state.ds.shader->GetCommonShader() : nullptr;
    const bool tessellated = m_context->m_state.gs.shader == nullptr
      && m_context->m_state.hs.shader != nullptr && commonDs != nullptr
      && commonDs->HasPositionCaptureCandidate() && RtxOptions::dx11CaptureTessellation();
    // Geometry-shader draws: the game's GS, recompiled with XFB, is the
    // capture stage (its triangles are what the rasterizer received).
    const D3D11CommonShader* commonGs = m_context->m_state.gs.shader != nullptr
      ? m_context->m_state.gs.shader->GetCommonShader() : nullptr;
    const bool gsCapture = commonGs != nullptr && commonGs->HasPositionCaptureCandidate()
      && m_context->m_state.hs.shader == nullptr && m_context->m_state.ds.shader == nullptr
      && RtxOptions::dx11CaptureGeometryShaders() && !m_indirectReplay.active;
    if ((m_context->m_state.gs.shader != nullptr && !gsCapture)
     || ((m_context->m_state.hs.shader != nullptr || m_context->m_state.ds.shader != nullptr) && !tessellated))
      return false;

    for (const auto& soTarget : m_context->m_state.so.targets) {
      if (soTarget.buffer != nullptr)
        return false;
    }

    DxvkInputAssemblyState restoreIa = { VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_FALSE, 0 };
    uint32_t patchControlPoints = 0;
    if (tessellated) {
      const uint32_t topology = uint32_t(m_context->m_state.ia.primitiveTopology);
      if (topology < uint32_t(D3D_PRIMITIVE_TOPOLOGY_1_CONTROL_POINT_PATCHLIST)
       || topology > uint32_t(D3D_PRIMITIVE_TOPOLOGY_32_CONTROL_POINT_PATCHLIST))
        return false;
      patchControlPoints = topology - uint32_t(D3D_PRIMITIVE_TOPOLOGY_1_CONTROL_POINT_PATCHLIST) + 1u;
      restoreIa = { VK_PRIMITIVE_TOPOLOGY_PATCH_LIST, VK_FALSE, patchControlPoints };
    } else switch (m_context->m_state.ia.primitiveTopology) {
      case D3D_PRIMITIVE_TOPOLOGY_POINTLIST:
        restoreIa = { VK_PRIMITIVE_TOPOLOGY_POINT_LIST, VK_FALSE, 0 };
        break;
      case D3D_PRIMITIVE_TOPOLOGY_LINELIST:
        restoreIa = { VK_PRIMITIVE_TOPOLOGY_LINE_LIST, VK_FALSE, 0 };
        break;
      case D3D_PRIMITIVE_TOPOLOGY_LINESTRIP:
        restoreIa = { VK_PRIMITIVE_TOPOLOGY_LINE_STRIP, VK_TRUE, 0 };
        break;
      case D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST:
        restoreIa = { VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_FALSE, 0 };
        break;
      case D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP:
        restoreIa = { VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP, VK_TRUE, 0 };
        break;
      default:
        return false;
    }

    // Cold capture and dynamic replay must be amortized.  Treating hundreds of
    // Unreal ring-buffer draws as one frame of mandatory work can keep a single
    // NVIDIA queue submission busy past TDR even when shader compilation is
    // already complete.  Separate lanes guarantee forward progress: existing
    // dynamic meshes consume only the replay lane, while later uncached meshes
    // can still populate the cold lane on subsequent frames.
    // DX11_V295_CAPTURE_BUDGET: the old caps (3 draws / 2 new / 1 replay /
    // 3 MiB) were sized for Unreal ring buffers, but a title whose ENTIRE
    // scene flows through post-VS capture (sm64coopdx and other Remix-native
    // ports draw 50-200 captured meshes per frame, each only a few KiB) could
    // never assemble a scene: two draws captured, everything else fell to the
    // raster layer, and the frame passed through un-path-traced forever. The
    // caps are per-title tunable now with defaults sized for full-scene
    // capture; the byte cap still bounds a single frame's GPU copy work.
    const uint32_t kMaxCapturesPerFrame =
      std::max(RtxOptions::captureMaxDrawsPerFrame(), 1);
    const uint32_t kMaxNewCaptureBuffersPerFrame =
      std::max(RtxOptions::captureMaxNewBuffersPerFrame(), 1);
    const uint32_t kMaxReplayCapturesPerFrame =
      std::max(RtxOptions::captureMaxReplaysPerFrame(), 1);
    const VkDeviceSize kMaxCaptureBytesPerFrame =
      VkDeviceSize(std::max(RtxOptions::captureMaxMiBPerFrame(), 1)) << 20;
    static constexpr size_t       kMaxCacheEntries           = 4096u;
    static constexpr VkDeviceSize kMaxCacheBytes             = 384ull << 20;

    // Device-local index buffers cannot be scanned on the submit thread. The
    // old fallback replayed the entire shared vertex buffer for every indexed
    // sub-draw, even when the draw referenced only one triangle. Besides being
    // incorrect (unrelated vertices entered the BLAS), this turned a few dozen
    // indices into thousands of VS/XFB invocations and eventually stalled the
    // frame. For triangle lists, replay the real indexed point stream on the GPU
    // and use transform feedback's compact output as a non-indexed triangle
    // list. The ordering and duplicates exactly match the application's index
    // sequence; no CPU readback or guessed range is involved.
    const bool multiInstanceCapture = replayInstanceCount > 1u;
    const bool triangleList =
      m_context->m_state.ia.primitiveTopology == D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    // An indexed replay emits vertices in index order.  That compact output is
    // a different vertex domain from the application's original index buffer,
    // even for a single instance: keeping the old index buffer can make a draw
    // with 12 indices address a one-vertex capture and feed out-of-bounds
    // addresses to vkCmdBuildAccelerationStructuresKHR (observed as an NVIDIA
    // TDR in Unreal immediately after entering gameplay).  Independent
    // triangle lists have an exact safe representation: flatten every indexed
    // list into the XFB output and submit it as a non-indexed triangle list.
    // Indexed strips cannot be flattened by merely preserving index order,
    // since strip parity/restart state would be lost; reject that uncommon path
    // rather than constructing an invalid RT geometry domain.
    if (indexed && !triangleList && !tessellated && !gsCapture)
      return false;
    // Indirect replay: an indexed indirect draw is replayed as an indexed
    // point stream (one output vertex per index), which needs a triangle list.
    const bool indirectReplay = m_indirectReplay.active;
    if (indirectReplay && (tessellated || (m_indirectReplay.indexed && !triangleList)))
      return false;

    const bool flattenIndexed = indexed && triangleList && !tessellated && !gsCapture;
    if (requireIndexedFlatten && !flattenIndexed)
      return false;

    // GPU-sized output (tessellation, GS amplification): a budgeted capacity;
    // the real count is only known on the GPU and the excess stays NaN.
    const uint32_t tessPatches = tessellated && patchControlPoints > 0u ? count / patchControlPoints : 0u;
    const uint32_t verticesPerInstance = tessellated
      ? uint32_t(std::min<uint64_t>(uint64_t(tessPatches) * RtxOptions::dx11TessellationTrianglesPerPatch() * 3u,
                                    kMaxPositionCaptureVerticesPerDraw))
      : gsCapture
      ? uint32_t(std::min<uint64_t>(uint64_t(count) * RtxOptions::dx11GeometryShaderCaptureVerticesPerInput(),
                                    kMaxPositionCaptureVerticesPerDraw))
      : (flattenIndexed ? count : geo.vertexCount);
    const uint64_t totalVertexCount =
      uint64_t(verticesPerInstance) * uint64_t(replayInstanceCount);
    if (verticesPerInstance == 0 || totalVertexCount == 0
     || (multiInstanceCapture && !tessellated && !gsCapture && (!triangleList || verticesPerInstance % 3u != 0u))
     || totalVertexCount > kMaxPositionCaptureVerticesPerDraw) {
      return false;
    }
    const uint32_t vertexCount = uint32_t(totalVertexCount);

    if (m_context->m_state.vs.shader == nullptr)
      return false;
    const D3D11CommonShader* commonVs = m_context->m_state.vs.shader->GetCommonShader();
    if (commonVs == nullptr || (!tessellated && !gsCapture && !commonVs->HasPositionCaptureCandidate()))
      return false;
    // The stage whose SV_Position reaches the rasterizer.
    const D3D11CommonShader* captureStage = tessellated ? commonDs : gsCapture ? commonGs : commonVs;

    const bool capturesHomogeneousClip =
      captureStage->IsPositionCaptureHomogeneousClipSpace();
    const uint32_t positionBytes = capturesHomogeneousClip ? 16u : 12u;
    std::string requestedTexcoordName;
    uint32_t requestedTexcoordIndex = 0;
    uint32_t requestedTexcoordComponent = 0;
    const D3D11CommonShader* commonPs =
      m_context->m_state.ps.shader != nullptr
        ? m_context->m_state.ps.shader->GetCommonShader()
        : nullptr;
    const uint32_t colorTextureSlot =
      dcs.materialData.getColorTextureSlot(0);
    const bool hasPsSampledTexcoord = commonPs != nullptr
      && colorTextureSlot < D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT
      && commonPs->GetSampledTexcoordSemantic(
           colorTextureSlot, requestedTexcoordName, requestedTexcoordIndex,
           requestedTexcoordComponent);

    std::string captureTexcoordName;
    uint32_t captureTexcoordIndex = 0;
    uint32_t captureTexcoordComponent = 0;
    // Do not manufacture a position+UV GS variant for an untextured material.
    // A PS can retain sampling dataflow while its color image is unbound (for
    // example an Unreal helper/decal pass). Capturing that unused varying adds
    // no RT material data and, on NVIDIA, one such TEXCOORD1 passthrough variant
    // repeatedly reset the device even though the position-only variant of the
    // same application VS was valid. Untextured geometry still captures exact
    // positions and gets albedo from its real vertex color or TFactor policy.
    // The UV is resolved against the stage that feeds the pixel shader: the
    // VS, or the DS / GS whose outputs the capture records (tessellated
    // terrain and water, GS particles).
    const bool captureIncludesTexcoord = dcs.materialData.usesTexture()
      && hasPsSampledTexcoord
      && captureStage->ResolvePositionCaptureTexcoord(
           requestedTexcoordName, requestedTexcoordIndex,
           requestedTexcoordComponent,
           captureTexcoordName, captureTexcoordIndex,
           captureTexcoordComponent);
    // Vertex colour: the pixel-shader input that multiplies the texture
    // (D3D11CommonShader::GetVertexColorSemantic), when the capture stage
    // writes it. Captured beside position, so flattened, instanced,
    // tessellated and GS draws keep the colour the IA stream cannot give them.
    std::string captureColorName;
    uint32_t captureColorIndex = 0;
    uint32_t captureColorComponents = 0;
    {
      std::string psColorName;
      uint32_t psColorIndex = 0, psColorComponents = 0;
      if (commonPs != nullptr && commonPs->GetVertexColorSemantic(psColorName, psColorIndex, psColorComponents)) {
        const uint32_t written = captureStage->ResolvePositionCaptureColor(psColorName, psColorIndex);
        if (written >= 3u) {
          captureColorName = psColorName;
          captureColorIndex = psColorIndex;
          captureColorComponents = std::min(written, std::max(psColorComponents, 3u));
        }
      }
    }
    const bool captureIncludesColor = captureColorComponents >= 3u;
    const uint32_t captureColorOffset = positionBytes + (captureIncludesTexcoord ? 8u : 0u);

    const uint32_t captureStride = positionBytes
      + (captureIncludesTexcoord ? 8u : 0u)
      + captureColorComponents * 4u;
    const VkDeviceSize captureBytes =
      VkDeviceSize(vertexCount) * captureStride;
    Matrix4 capturedClipToPosition;
    bool capturedClipUsesWDepth = false;
    if (m_lift2DDraw && !capturesHomogeneousClip)
      return false;
    if (m_lift2DDraw) {
      // 2D lift: NDC (x/w, y/w) onto the plane at this layer's depth Z,
      // scaled by Z so the synthetic camera (Lift2DProjection) projects it
      // back onto exactly the same pixels:
      //   position = (x/w * Z / P00, y/w * Z / P11, Z)
      // Linear in clip with the divide by w done by the interleaver.
      const Matrix4& liftProjection = dcs.transformData.viewToProjection;
      const float Z = m_lift2DDepth;
      const float sx = Z / liftProjection[0][0];
      const float sy = Z / liftProjection[1][1];
      capturedClipToPosition[0] = Vector4(sx,   0.0f, 0.0f, 0.0f);
      capturedClipToPosition[1] = Vector4(0.0f, sy,   0.0f, 0.0f);
      capturedClipToPosition[2] = Vector4(0.0f, 0.0f, 0.0f, 0.0f);
      capturedClipToPosition[3] = Vector4(0.0f, 0.0f, Z,    1.0f);
      // Same scene convention as the clip-W replacement camera below: LH,
      // Y-up, +Z forward.
      const RtxOptionLayer* derived = RtxOptionLayer::getDerivedLayer();
      RtxOptions::leftHandedCoordinateSystemObject().setDeferred(true, derived);
      RtxOptions::zUpObject().setDeferred(false, derived);
      RtCamera::correctProjectionYFlipObject().setDeferred(
        projectionYFlipOverride() ? projectionYFlip() : false, derived);
      if (!std::isfinite(sx) || !std::isfinite(sy))
        return false;
    } else if (capturesHomogeneousClip) {
      // Exact SV_Position is already the complete result of the game's vertex
      // transform.  Reconstruct the position directly in the replacement
      // camera's view-space world.  Do NOT also apply objectToView here: that
      // matrix is inferred independently and, when it is merely plausible
      // rather than the exact shader transform, inverse(O2V) turns ordinary
      // meshes into the giant camera-enclosing slabs seen in the RT G-buffer.
      // inverse(P) * clip followed by the homogeneous divide is sufficient and
      // is valid for every game whose raster projection was recovered.
      // Jittered projection of this draw when known (see ExtractTransforms):
      // differs from viewToProjection only by the TAA sub-pixel offset.
      const Matrix4& rasterProjection = m_drawJitteredProjectionValid
        ? m_drawJitteredProjection : dcs.transformData.viewToProjection;
      const Matrix4 inverseProjection = inverse(rasterProjection);
      capturedClipToPosition = inverseProjection;
      // Optimized Unity and other engine shaders frequently expose only a
      // combined object-to-clip transform. A viewport-derived replacement
      // projection is still sufficient because visible perspective vertices
      // carry exact linear camera depth in clip.w. The interleaver uses XYW in
      // this mode and deliberately ignores clip.z, so reversed-Z and unknown
      // game near/far planes cannot turn the reconstructed scene inside out.
      capturedClipUsesWDepth = dcs.transformData.usedViewportFallbackProjection;
      if (capturedClipUsesWDepth) {
        // The XYW reconstruction below defines one complete replacement
        // camera space independent of the source engine: +X right, +Y up and
        // +Z forward (clip.w is positive visible depth).  Keep Remix's scene
        // convention synchronized with that generated geometry.  The older
        // matrix-vote path cannot settle for optimized shaders that expose
        // only a combined object-to-clip transform, leaving Remix at its RH
        // default while the replacement geometry is LH; free-camera motion,
        // culling and orientation then appear mirrored in Unity and any other
        // engine using this exact-capture fallback.
        const RtxOptionLayer* derived = RtxOptionLayer::getDerivedLayer();
        RtxOptions::leftHandedCoordinateSystemObject().setDeferred(true, derived);
        RtxOptions::zUpObject().setDeferred(false, derived);
        const bool replacementYFlip = projectionYFlipOverride()
          ? projectionYFlip()
          : false;
        RtCamera::correctProjectionYFlipObject().setDeferred(replacementYFlip, derived);

        static uint32_t sReplacementAxisProfileLogCount = 0;
        if (sReplacementAxisProfileLogCount < 4u) {
          ++sReplacementAxisProfileLogCount;
          Logger::info(
            str::format(
              "[D3D11Rtx] Exact clip-W replacement profile selected: LH, Y-up, projection Y ",
              replacementYFlip ? "flipped (manual override)" : "unflipped"));
        }
      }
      // Reserved-depth passes. inverse(P) recovers position from clip.z, so
      // every draw must share the world's clip.z encoding. First-person arms
      // and weapons are drawn with their own projection (a closer near plane)
      // into a reserved depth range; through the world's inverse they came
      // out many times too large and wrapped around the camera. For any
      // perspective projection clip.z = a * clip.w + b, and the shader's own
      // clip matrix exposes a and b in its z and w rows.
      // Skinned first-person arms have no single readable clip matrix, so the
      // mapping learned from any readable draw in the reserved range is reused
      // for every draw in it (the pass shares one projection).
      if (!capturedClipUsesWDepth) {
        static float s_worldDepthA = 0.0f, s_worldDepthB = 0.0f;
        static float s_reservedDepthA = 0.0f, s_reservedDepthB = 0.0f;
        const auto& vp = m_context->m_state.rs.viewports[0];
        const bool reservedDepthPass = isReservedDepthViewport(vp);

        float a = 0.0f, b = 0.0f;
        bool ownMapping = false;
        const D3D11PositionTransformBinding* depthBinding = commonVs->GetPositionTransformBinding();
        if (depthBinding != nullptr && depthBinding->matrixCount >= 1u) {
          const D3D11PositionTransformMatrixBinding& clipMatrix =
            depthBinding->matrices[depthBinding->matrixCount - 1u];
          const bool slotValid = clipMatrix.constantBufferSlot < D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT;
          const auto& cb = m_context->m_state.vs.constantBuffers[slotValid ? clipMatrix.constantBufferSlot : 0u];
          Vector4 clipRows[4];
          if (slotValid && readBindingRows(clipMatrix, cb, clipRows)) {
            {
              const float* zRow = clipRows[2].data;
              const float* wRow = clipRows[3].data;
              const float wwDot = wRow[0] * wRow[0] + wRow[1] * wRow[1] + wRow[2] * wRow[2];
              if (std::isfinite(wwDot) && wwDot > 1.0e-12f) {
                a = (zRow[0] * wRow[0] + zRow[1] * wRow[1] + zRow[2] * wRow[2]) / wwDot;
                b = zRow[3] - a * wRow[3];
                float residual = 0.0f;
                for (uint32_t c = 0; c < 3; ++c)
                  residual += std::abs(zRow[c] - a * wRow[c]);
                // Only a perspective z row (a multiple of the w row plus a
                // constant) describes a depth mapping.
                ownMapping = std::isfinite(a) && std::isfinite(b)
                  && std::abs(a) > 1.0e-6f && std::abs(b) > 1.0e-6f
                  && residual <= 1.0e-3f * std::sqrt(wwDot);
              }
            }
          }
        }

        if (ownMapping) {
          if (reservedDepthPass) {
            s_reservedDepthA = a;
            s_reservedDepthB = b;
          } else {
            s_worldDepthA = a;
            s_worldDepthB = b;
          }
        } else if (reservedDepthPass && s_reservedDepthB != 0.0f) {
          a = s_reservedDepthA;
          b = s_reservedDepthB;
        }

        // The world's own encoding is the reference: world draws keep
        // inverse(P) unchanged. A reserved-depth draw has its clip.z
        // re-expressed in the world encoding first. Per vertex
        // 1 = (z - a*w) / b, so
        //   z' = aRef*w + bRef = (bRef/b) z + (aRef - bRef*a/b) w
        // which is linear in clip and folds into the matrix.
        if (reservedDepthPass && b != 0.0f && s_worldDepthB != 0.0f) {
          Matrix4 remap;  // identity
          remap[2][2] = s_worldDepthB / b;
          remap[3][2] = s_worldDepthA - s_worldDepthB * a / b;
          const Matrix4 drawInverse = inverseProjection * remap;
          bool finite = true;
          for (uint32_t column = 0; column < 4; ++column)
            for (uint32_t row = 0; row < 4; ++row)
              finite &= std::isfinite(drawInverse[column][row]);
          if (finite) {
            capturedClipToPosition = drawInverse;
            static uint32_t s_depthMapLogs = 0;
            if (s_depthMapLogs < 12u) {
              ++s_depthMapLogs;
              Logger::info(str::format("[D3D11Rtx] reserved-depth pass remapped to world depth encoding: vs=0x",
                std::hex, commonVs->GetBytecodeHash(), std::dec, " a=", a, " b=", b,
                ownMapping ? " (own)" : " (shared)",
                " worldA=", s_worldDepthA, " worldB=", s_worldDepthB,
                " vpDepth=", vp.MinDepth, "-", vp.MaxDepth));
            }
          }
        }
      }
      for (uint32_t column = 0; column < 4; ++column) {
        for (uint32_t row = 0; row < 4; ++row) {
          if (!std::isfinite(capturedClipToPosition[column][row]))
            return false;
        }
      }
    }

    Rc<DxvkShader> captureGs = captureStage->GetPositionCaptureShader(
      captureTexcoordName, captureTexcoordIndex, captureTexcoordComponent,
      captureColorName, captureColorIndex, captureColorComponents);
    if (captureGs == nullptr)
      return false;

    if (captureIncludesTexcoord) {
      static uint32_t sPsLinkedUvSelectionLogCount = 0;
      if (sPsLinkedUvSelectionLogCount < 64u) {
        ++sPsLinkedUvSelectionLogCount;
        Logger::info(str::format(
          "[D3D11Rtx][uv-link] selected ",
          captureTexcoordName, captureTexcoordIndex,
          " components=", captureTexcoordComponent, "-",
          captureTexcoordComponent + 1u,
          " for color resource t", colorTextureSlot,
          " psProven=", hasPsSampledTexcoord ? 1 : 0,
          " requested=", hasPsSampledTexcoord
            ? str::format(requestedTexcoordName, requestedTexcoordIndex)
            : "none",
          " vs=", commonVs->GetName(),
          " ps=", commonPs != nullptr ? commonPs->GetName() : "none"));
      }
    }

    const D3D11CapturedPositionSpace positionSpace = commonVs->GetPositionCaptureSpace();
    // SV_Position capture has no distinct non-system output to factor against
    // clip space.  Its exact transform dependency is the original POSITION to
    // SV_Position chain.  Using the output-to-clip binding here always returned
    // null for the full-replacement camera path and forced every draw to replay
    // forever.  Pre-projection profile captures still use their proven
    // output-to-clip relationship.
    const D3D11PositionTransformBinding* captureBinding = capturesHomogeneousClip
      ? commonVs->GetPositionTransformBinding()
      : commonVs->GetPositionCaptureClipTransformBinding();
    const bool capturedSkinnedPositions =
      geo.blendWeightBuffer.defined()
      && geo.blendIndicesBuffer.defined()
      && geo.numBonesPerVertex >= 2;

    const uint32_t firstVertex = flattenIndexed
      ? 0u
      : (indexed ? uint32_t(std::max(base, 0)) : start);
    std::array<bool, D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT> captureInputSlots = {};
    std::array<bool, D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT> capturePerInstanceSlots = {};
    std::array<uint32_t, D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT> captureInstanceStepRates = {};
    if (m_context->m_state.ia.inputLayout != nullptr) {
      for (const auto& semantic : m_context->m_state.ia.inputLayout->GetRtxSemantics()) {
        if (semantic.inputSlot < captureInputSlots.size()) {
          captureInputSlots[semantic.inputSlot] = true;
          capturePerInstanceSlots[semantic.inputSlot] |= semantic.perInstance;
          captureInstanceStepRates[semantic.inputSlot] = semantic.instanceStepRate;
        }
      }
    } else {
      // No input layout: the VS cannot read any vertex buffer (it pulls from
      // SRVs by SV_VertexID), so stale bound VBs are not part of the draw.
      captureInputSlots.fill(false);
    }

    // Vertex-pulled draws are distinguished by the SRVs the VS reads, not by
    // vertex buffers; fold them into the contract and storage identities.
    uint64_t vsResourceIdentity = 0;
    if (m_context->m_state.ia.inputLayout == nullptr) {
      const auto& views = m_context->m_state.vs.shaderResources.views;
      for (uint32_t slot = 0; slot < views.size(); ++slot) {
        if (views[slot] == nullptr)
          continue;
        vsResourceIdentity ^= (uint64_t(reinterpret_cast<uintptr_t>(views[slot].ptr())) + slot)
          * 0x9e3779b97f4a7c15ull + (vsResourceIdentity << 6) + (vsResourceIdentity >> 2);
      }
    }

    // Hash only the constant registers that DXBC dataflow proved feed the
    // captured clip position.  This is an engine-independent transform-state
    // profile: unchanged registers mean a rigid mesh's captured view-space
    // vertices are still exact, while camera/object motion changes the hash and
    // schedules one new replay.  It avoids both blind per-frame replay and
    // guesses over unrelated cbuffer matrices.
    uint64_t homogeneousTransformStateIdentity = 0;
    bool hasHomogeneousTransformStateIdentity = false;
    bool usedCompleteShaderStateIdentity = false;
    bool usedShaderDependencyProfile = false;
    if (capturesHomogeneousClip) {
      uint64_t stateHash = 0x5356504f53495449ull; // "SVPOSITI"
      stateHash = XXH3_64bits_withSeed(
        &replayFirstInstance, sizeof(replayFirstInstance), stateHash);
      stateHash = XXH3_64bits_withSeed(
        &replayInstanceCount, sizeof(replayInstanceCount), stateHash);
      bool readable = true;
      const bool hasNarrowTransformBinding = !captureIncludesTexcoord && captureBinding != nullptr
        && captureBinding->matrixCount >= 1u
        && captureBinding->matrixCount <= 2u;

      auto hashBoundConstantRange = [&](const D3D11ConstantBufferBinding& cb,
                                        size_t byteOffset,
                                        size_t byteLength) {
        if (cb.buffer == nullptr)
          return false;
        const auto mapped = cb.buffer->GetMappedSlice();
        const uint8_t* ptr = reinterpret_cast<const uint8_t*>(mapped.mapPtr);
        const size_t bufferSize = cb.buffer->Desc()->ByteWidth;
        const size_t bindingBase = size_t(cb.constantOffset) * 16u;
        const size_t bindingEnd = cb.constantCount > 0
          ? std::min(bindingBase + size_t(cb.constantCount) * 16u, bufferSize)
          : bufferSize;
        const size_t begin = bindingBase + byteOffset;
        const size_t end = begin + byteLength;
        if (ptr == nullptr || begin < bindingBase || end < begin
         || begin >= bindingEnd || end > bindingEnd || end > bufferSize)
          return false;
        stateHash = XXH3_64bits_withSeed(ptr + begin, byteLength, stateHash);
        return true;
      };

      auto isExactProjectionRegister = [&](uint32_t slot, uint32_t shaderRegister) {
        if (captureIncludesTexcoord || m_projStage != 0 || m_projSlot != slot || m_projOffset == SIZE_MAX)
          return false;
        const auto& cb = m_context->m_state.vs.constantBuffers[slot];
        const size_t absoluteRegisterOffset =
          size_t(cb.constantOffset) * 16u + size_t(shaderRegister) * 16u;
        return absoluteRegisterOffset >= m_projOffset
            && absoluteRegisterOffset < m_projOffset + 64u;
      };

      if (hasNarrowTransformBinding) {
        stateHash = XXH3_64bits_withSeed(
          &captureBinding->matrixCount, sizeof(captureBinding->matrixCount), stateHash);
        for (uint32_t matrixIndex = 0;
             matrixIndex < captureBinding->matrixCount && readable;
             ++matrixIndex) {
          const auto& matrixBinding = captureBinding->matrices[matrixIndex];
          if (matrixBinding.constantBufferSlot
              >= D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT) {
            readable = false;
            break;
          }
          const auto& cb = m_context->m_state.vs.constantBuffers[
            matrixBinding.constantBufferSlot];
          for (uint32_t row = 0; row < 4u && readable; ++row) {
            const uint32_t reg = matrixBinding.constantRegisters[row];
            if (reg == UINT32_MAX) {
              static constexpr uint64_t kAffineRow = 0x414646494e45572full;
              stateHash = XXH3_64bits_withSeed(
                &kAffineRow, sizeof(kAffineRow), stateHash);
            } else if (isExactProjectionRegister(
                         matrixBinding.constantBufferSlot, reg)) {
              // Clip coordinates and inverse projection are stored as one
              // cache entry below. Pure projection jitter/FOV changes do not
              // alter the reconstructed view-space mesh and must not replay it.
              static constexpr uint64_t kProjectionRegister =
                0x50524f4a524547ull;
              stateHash = XXH3_64bits_withSeed(
                &kProjectionRegister, sizeof(kProjectionRegister), stateHash);
            } else {
              readable = hashBoundConstantRange(cb, size_t(reg) * 16u, 16u);
            }
          }
        }
      } else {
        const auto& dependencyProfile =
          commonVs->GetConstantBufferDependencyProfile();
        if (dependencyProfile.complete) {
          // This is the general optimized-shader path. The profile is parsed
          // once from actual DXBC operands, so it is game-independent and
          // includes every statically addressed register the shader can read.
          usedShaderDependencyProfile = true;
          for (const auto& dependency : dependencyProfile.dependencies) {
            if (dependency.slot >=
                D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT) {
              readable = false;
              break;
            }

            stateHash = XXH3_64bits_withSeed(
              &dependency.slot, sizeof(dependency.slot), stateHash);
            const auto& cb = m_context->m_state.vs.constantBuffers[
              dependency.slot];
            if (cb.buffer == nullptr) {
              static constexpr uint64_t kUnboundConstantBuffer =
                0x554e424f554e44ull;
              stateHash = XXH3_64bits_withSeed(
                &kUnboundConstantBuffer,
                sizeof(kUnboundConstantBuffer), stateHash);
              continue;
            }

            const size_t bufferSize = cb.buffer->Desc()->ByteWidth;
            const size_t bindingBase = size_t(cb.constantOffset) * 16u;
            const size_t bindingEnd = cb.constantCount > 0
              ? std::min(bindingBase + size_t(cb.constantCount) * 16u, bufferSize)
              : bufferSize;
            if (bindingBase >= bindingEnd) {
              readable = false;
              break;
            }

            if (!dependency.wholeBuffer) {
              if (isExactProjectionRegister(
                    dependency.slot, dependency.constantRegister)) {
                static constexpr uint64_t kProjectionRegister =
                  0x50524f4a524547ull;
                stateHash = XXH3_64bits_withSeed(
                  &kProjectionRegister, sizeof(kProjectionRegister), stateHash);
              } else {
                readable = hashBoundConstantRange(
                  cb, size_t(dependency.constantRegister) * 16u, 16u);
              }
              if (!readable)
                break;
              continue;
            }

            // Dynamic indexing makes the whole visible slot relevant. Split
            // around the exact projection block so TAA jitter remains a
            // camera change, not a geometry/BLAS change.
            const bool projectionInThisBinding =
              !captureIncludesTexcoord && m_projStage == 0 && m_projSlot == dependency.slot
              && m_projOffset != SIZE_MAX
              && m_projOffset >= bindingBase
              && m_projOffset + 64u <= bindingEnd;
            if (!projectionInThisBinding) {
              readable = hashBoundConstantRange(
                cb, 0u, bindingEnd - bindingBase);
            } else {
              const size_t projectionRelative = m_projOffset - bindingBase;
              if (projectionRelative > 0u)
                readable = hashBoundConstantRange(
                  cb, 0u, projectionRelative);
              if (readable && m_projOffset + 64u < bindingEnd) {
                const size_t suffixRelative = projectionRelative + 64u;
                readable = hashBoundConstantRange(
                  cb, suffixRelative, bindingEnd - (m_projOffset + 64u));
              }
            }
            if (!readable)
              break;
          }
        } else {
          // Only a dynamically selected cbuffer SLOT defeats the bounded
          // profile. Preserve the exact conservative fallback for that case.
          usedCompleteShaderStateIdentity = true;
          bool anyConstantBuffer = false;
          for (uint32_t slot = 0;
               slot < D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT && readable;
               ++slot) {
            const auto& cb = m_context->m_state.vs.constantBuffers[slot];
            if (cb.buffer == nullptr)
              continue;
            anyConstantBuffer = true;
            const size_t bufferSize = cb.buffer->Desc()->ByteWidth;
            const size_t bindingBase = size_t(cb.constantOffset) * 16u;
            const size_t bindingEnd = cb.constantCount > 0
              ? std::min(bindingBase + size_t(cb.constantCount) * 16u, bufferSize)
              : bufferSize;
            if (bindingBase >= bindingEnd) {
              readable = false;
              break;
            }
            stateHash = XXH3_64bits_withSeed(&slot, sizeof(slot), stateHash);
            readable = hashBoundConstantRange(
              cb, 0u, bindingEnd - bindingBase);
          }
          readable &= anyConstantBuffer;
        }
      }

      // Bound CPU hashing independently of GPU capture size. Large or GPU-only
      // mutable inputs are replayed on the GPU; they must never be declared
      // unchanged based only on their allocation address.
      constexpr uint64_t kMaxCaptureIdentityBytes = 1ull << 20;
      uint64_t identityBytes = 0;
      rtx::CaptureVertexSpan sourceSpan { firstVertex, verticesPerInstance };
      if (flattenIndexed && readable) {
        const auto& ib = m_context->m_state.ia.indexBuffer;
        const uint64_t indexSize = ib.format == DXGI_FORMAT_R16_UINT ? 2u : 4u;
        const uint64_t begin = uint64_t(ib.offset) + uint64_t(start) * indexSize;
        const uint64_t bytes = uint64_t(count) * indexSize;
        // An immutable index allocation is already a content identity. Only
        // mutable per-vertex inputs require its index range to select bytes
        // for CPU hashing. Avoid rescanning megabytes of immutable indices on
        // every draw of a large static mesh.
        bool needsSourceSpan = false;
        for (uint32_t slot = 0; slot < captureInputSlots.size(); ++slot) {
          const auto& vb = m_context->m_state.ia.vertexBuffers[slot];
          needsSourceSpan |= captureInputSlots[slot] && !capturePerInstanceSlots[slot]
            && vb.buffer != nullptr && vb.buffer->Desc()->Usage != D3D11_USAGE_IMMUTABLE;
        }
        const bool immutableIndices = ib.buffer != nullptr
          && ib.buffer->Desc()->Usage == D3D11_USAGE_IMMUTABLE;
        const bool validIndexRange = ib.buffer != nullptr
          && begin <= ib.buffer->Desc()->ByteWidth
          && bytes <= uint64_t(ib.buffer->Desc()->ByteWidth) - begin;
        const bool useImmutableIdentity = validIndexRange && immutableIndices && !needsSourceSpan;
        const uint8_t* indices = nullptr;
        if (useImmutableIdentity) {
          const uint64_t content = ib.buffer->GetBuffer()->contentCookie();
          stateHash = XXH3_64bits_withSeed(&content, sizeof(content), stateHash);
          stateHash = XXH3_64bits_withSeed(&begin, sizeof(begin), stateHash);
          stateHash = XXH3_64bits_withSeed(&bytes, sizeof(bytes), stateHash);
          stateHash = XXH3_64bits_withSeed(&base, sizeof(base), stateHash);
        }
        else if (validIndexRange
          && bytes <= kMaxCaptureIdentityBytes) {
          const auto usage = ib.buffer->Desc()->Usage;
          if (usage == D3D11_USAGE_IMMUTABLE) {
            indices = static_cast<const uint8_t*>(ib.buffer->GetIndexShadow(begin, bytes));
          } else if (usage == D3D11_USAGE_DYNAMIC) {
            const auto* mapped = static_cast<const uint8_t*>(ib.buffer->GetMappedSlice().mapPtr);
            if (mapped != nullptr)
              indices = mapped + size_t(begin);
          }
        }
        if (!useImmutableIdentity && indices == nullptr) {
          readable = false;
        } else if (!useImmutableIdentity) {
          stateHash = XXH3_64bits_withSeed(indices, size_t(bytes), stateHash);
          identityBytes += bytes;
          sourceSpan = indexSize == 2u
            ? rtx::captureVertexSpan<uint16_t>(indices, count, base)
            : rtx::captureVertexSpan<uint32_t>(indices, count, base);
          readable = sourceSpan.count != 0;
        }
      }

      for (uint32_t slot = 0; slot < captureInputSlots.size() && readable; ++slot) {
        if (!captureInputSlots[slot])
          continue;
        const auto& vb = m_context->m_state.ia.vertexBuffers[slot];
        if (vb.buffer == nullptr || vb.stride == 0)
          continue;
        stateHash = XXH3_64bits_withSeed(&slot, sizeof(slot), stateHash);
        stateHash = XXH3_64bits_withSeed(&vb.stride, sizeof(vb.stride), stateHash);
        stateHash = XXH3_64bits_withSeed(&vb.offset, sizeof(vb.offset), stateHash);
        const uint32_t inputRate = capturePerInstanceSlots[slot] ? 1u : 0u;
        stateHash = XXH3_64bits_withSeed(&inputRate, sizeof(inputRate), stateHash);
        stateHash = XXH3_64bits_withSeed(&captureInstanceStepRates[slot],
          sizeof(captureInstanceStepRates[slot]), stateHash);
        if (vb.buffer->Desc()->Usage == D3D11_USAGE_IMMUTABLE) {
          const uint64_t content = vb.buffer->GetBuffer()->contentCookie();
          stateHash = XXH3_64bits_withSeed(&content, sizeof(content), stateHash);
          continue;
        }
        if (vb.buffer->Desc()->Usage != D3D11_USAGE_DYNAMIC) {
          readable = false;
          break;
        }
        const DxvkBufferSliceHandle mapped = vb.buffer->GetMappedSlice();
        const uint8_t* ptr = reinterpret_cast<const uint8_t*>(mapped.mapPtr);
        const size_t bufferSize = vb.buffer->Desc()->ByteWidth;
        const bool perInstance = capturePerInstanceSlots[slot];
        const uint64_t elementIndex = perInstance ? replayFirstInstance : sourceSpan.first;
        const uint64_t elementCount = perInstance
          ? rtx::captureInstanceElementCount(replayInstanceCount, captureInstanceStepRates[slot])
          : sourceSpan.count;
        const uint64_t begin = uint64_t(vb.offset) + elementIndex * vb.stride;
        const uint64_t byteLength = elementCount * vb.stride;
        if (ptr == nullptr || begin >= bufferSize || byteLength > bufferSize - begin
          || byteLength > kMaxCaptureIdentityBytes - identityBytes) {
          readable = false;
          break;
        }
        stateHash = XXH3_64bits_withSeed(ptr + size_t(begin), size_t(byteLength), stateHash);
        identityBytes += byteLength;
      }

      // D3D11 retains stale SRVs until the application explicitly unbinds
      // them. Treat only a slot proven sampled by this VS as position state;
      // otherwise Unreal's always-bound global resources force every rigid
      // mesh into the dynamic replay lane forever. A dynamically indexed
      // resource profile remains conservative until content hashing exists.
      if (commonVs->HasCompleteSampledResourceProfile()) {
        for (uint32_t slot = 0;
             slot < m_context->m_state.vs.shaderResources.views.size();
             ++slot) {
          if (commonVs->SamplesResourceSlot(slot)
           && m_context->m_state.vs.shaderResources.views[slot] != nullptr) {
            readable = false;
            break;
          }
        }
      } else {
        for (const auto& view : m_context->m_state.vs.shaderResources.views) {
          if (view != nullptr) {
            readable = false;
            break;
          }
        }
      }
      if (readable) {
        homogeneousTransformStateIdentity = stateHash != 0
          ? stateHash : 0x9e3779b97f4a7c15ull;
        hasHomogeneousTransformStateIdentity = true;
      }
    }
    // Homogeneous output is reconstructed into CURRENT view space, so camera
    // and rigid-object motion changes the mesh vertices and requires a replay.
    // This intentionally trades coverage for correctness under the bounded
    // capture budget: an omitted draw cannot occlude the valid scene, while a
    // stale or guessed transform can cover the entire camera with a false wall.
    // The dependency fingerprint above covers homogeneous capture only.
    // A profiled pre-projection output can still deform through any VS input,
    // constant or sampled resource even when skinning was not recognized.
    // Adaptive tessellation changes with the camera: always replay.
    const bool captureMustReplayEveryFrame = !capturesHomogeneousClip || tessellated || indirectReplay || gsCapture
      || capturedSkinnedPositions
      || (capturesHomogeneousClip && !hasHomogeneousTransformStateIdentity);
    // View-space vertices bake placement into the BLAS, so the draw cache must
    // keep separate same-frame BLAS slots for separate instances even when the
    // transform-register state allowed the capture replay itself to be reused.
    const bool capturedDynamicPositions = capturesHomogeneousClip
      || captureMustReplayEveryFrame;
    const Matrix4 originalObjectToWorld = dcs.transformData.objectToWorld;
    const Matrix4 originalObjectToView = dcs.transformData.objectToView;
    const Matrix4 originalWorldToView = dcs.transformData.worldToView;
    const bool homogeneousHasStableGameView = capturesHomogeneousClip
      && !dcs.transformData.cameraRelativeView
      && !isIdentityExact(originalWorldToView);
    bool useWorldAnchoredHomogeneousCapture = false;
    Matrix4 capturedToWorld;
    Matrix4 capturedToView;
    if (capturesHomogeneousClip) {
      // inverse(P) reconstructs current VIEW-space positions from SV_Position.
      // When the game supplied a real view matrix, pair those vertices with
      // the inverse of that exact view so the BLAS occupies a stable world.
      // Leaving objectToWorld and the RT camera both identity made the captured
      // world follow the raster camera and prevented Remix free-camera motion.
      capturedToWorld = homogeneousHasStableGameView
        ? inverse(originalWorldToView)
        : Matrix4();
      capturedToView = Matrix4();
      for (uint32_t column = 0; column < 4; ++column) {
        for (uint32_t row = 0; row < 4; ++row) {
          if (!std::isfinite(capturedToWorld[column][row]))
            return false;
        }
      }
    } else if (positionSpace == D3D11CapturedPositionSpace::World) {
      // The VS output already lives in the game's world coordinate system.
      // Applying inverse(view) here would transform it a second time and is the
      // precise cause of camera-following slabs/black rectangles.
      capturedToWorld = Matrix4();
      capturedToView = dcs.transformData.worldToView;
    } else {
      // A genuine view-space output becomes stable RT world space through the
      // inverse of the exact camera view matrix used for this draw.
      capturedToWorld = inverse(dcs.transformData.worldToView);
      capturedToView = Matrix4();
      for (uint32_t column = 0; column < 4; ++column) {
        for (uint32_t row = 0; row < 4; ++row) {
          if (!std::isfinite(capturedToWorld[column][row]))
            return false;
        }
      }
    }

    // The profile selects which VS output to capture; its world/view label is
    // only a compatibility fallback. Prefer the exact matrix that DXBC
    // dataflow proved transforms this output into SV_Position. Factoring that
    // matrix C against Remix's active projection P yields captured-to-view
    // A = inverse(P) * C. Composing inverse(gameView) * A then recovers the
    // correct object/world transform for view-, world-, and object-space
    // outputs without engine-specific matrix layouts.
    bool usedShaderProvenCaptureTransform = false;
    if (!capturesHomogeneousClip
     && captureBinding != nullptr
     && captureBinding->matrixCount >= 1u
     && captureBinding->matrixCount <= 2u) {
      auto finiteMatrix = [](const Matrix4& matrix) {
        for (uint32_t column = 0; column < 4; ++column) {
          for (uint32_t row = 0; row < 4; ++row) {
            if (!std::isfinite(matrix[column][row]))
              return false;
          }
        }
        return true;
      };
      auto readShaderMatrix = [&](const D3D11PositionTransformMatrixBinding& matrixBinding,
                                  Matrix4& matrix) {
        if (matrixBinding.constantBufferSlot
            >= D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT)
          return false;
        const auto& cb = m_context->m_state.vs.constantBuffers[
          matrixBinding.constantBufferSlot];
        Vector4 rows[4];
        if (!readBindingRows(matrixBinding, cb, rows))
          return false;
        for (uint32_t row = 0; row < 4; ++row)
          matrix[row] = rows[row];
        return finiteMatrix(matrix);
      };
      auto affineScore = [&](const Matrix4& candidate) -> float {
        if (!finiteMatrix(candidate))
          return -1.0e30f;
        const float affineError =
            std::abs(candidate[0][3])
          + std::abs(candidate[1][3])
          + std::abs(candidate[2][3])
          + std::abs(candidate[3][3] - 1.0f);
        if (affineError > 0.03f)
          return -1.0e30f;

        float score = 30.0f - affineError * 500.0f;
        Vector3 axes[3];
        for (uint32_t column = 0; column < 3; ++column) {
          const float lengthSq =
              candidate[0][column] * candidate[0][column]
            + candidate[1][column] * candidate[1][column]
            + candidate[2][column] * candidate[2][column];
          if (!std::isfinite(lengthSq)
           || lengthSq < 1.0e-10f || lengthSq > 1.0e10f)
            return -1.0e30f;
          const float invLength = 1.0f / std::sqrt(lengthSq);
          axes[column] = Vector3(
            candidate[0][column] * invLength,
            candidate[1][column] * invLength,
            candidate[2][column] * invLength);
        }
        const float shear = std::abs(dot(axes[0], axes[1]))
                          + std::abs(dot(axes[0], axes[2]))
                          + std::abs(dot(axes[1], axes[2]));
        if (shear > 1.5f)
          return -1.0e30f;
        return score - shear * 2.0f;
      };

      std::array<Matrix4, 2> shaderMatrices;
      bool readable = true;
      for (uint32_t i = 0; i < captureBinding->matrixCount; ++i)
        readable &= readShaderMatrix(captureBinding->matrices[i], shaderMatrices[i]);

      if (readable) {
        Matrix4 captureToClip;
        bool captureToClipValid = false;
        if (captureBinding->matrixCount == 1u) {
          // DXBC dp4 consumes each cbuffer vector as one mathematical row.
          // Matrix4 stores mathematical columns, so the exact transform is the
          // transpose of the four vectors copied from the cbuffer.
          captureToClip = transpose(shaderMatrices[0]);
          captureToClipValid = finiteMatrix(captureToClip);
        } else {
          const Matrix4 captureFromBase = transpose(shaderMatrices[0]);
          const Matrix4 clipFromBase = transpose(shaderMatrices[1]);
          const Matrix4 inverseCaptureFromBase = inverse(captureFromBase);
          if (finiteMatrix(inverseCaptureFromBase)) {
            // captured = A * base, clip = B * base, therefore
            // clip = B * inverse(A) * captured. Matrix order is proven by the
            // DXBC dp4 dataflow; accepting the reverse order merely because it
            // also looked affine selected a plausible but spatially wrong
            // camera and produced the giant wall/black-rectangle frame.
            captureToClip = clipFromBase * inverseCaptureFromBase;
            captureToClipValid = finiteMatrix(captureToClip);
          }
        }

        const Matrix4 inverseProjection = inverse(dcs.transformData.viewToProjection);
        const Matrix4 inverseView = inverse(dcs.transformData.worldToView);
        if (captureToClipValid
         && finiteMatrix(inverseProjection) && finiteMatrix(inverseView)) {
          const Matrix4 provenCapturedToView = inverseProjection * captureToClip;
          const float provenScore = affineScore(provenCapturedToView);
          if (provenScore > -1.0e20f) {
            const Matrix4 bestCapturedToWorld = inverseView * provenCapturedToView;
            if (finiteMatrix(bestCapturedToWorld)) {
              capturedToView = provenCapturedToView;
              capturedToWorld = bestCapturedToWorld;
              usedShaderProvenCaptureTransform = true;

              static uint32_t sCaptureTransformLogs = 0;
              if (sCaptureTransformLogs++ < 24u) {
                const auto& clipMatrixBinding = captureBinding->matrices[
                  captureBinding->matrixCount - 1u];
                Logger::info(str::format(
                  "[D3D11Rtx] shader-proven captured-to-view: vs=",
                  commonVs->GetName(), " matrices=", captureBinding->matrixCount,
                  " clipCb=", clipMatrixBinding.constantBufferSlot, " clipRegs=",
                  clipMatrixBinding.constantRegisters[0], ",",
                  clipMatrixBinding.constantRegisters[1], ",",
                  clipMatrixBinding.constantRegisters[2], ",",
                  clipMatrixBinding.constantRegisters[3],
                  " factorScore=", provenScore,
                  " toViewTranslation=",
                  provenCapturedToView[3][0], ",",
                  provenCapturedToView[3][1], ",",
                  provenCapturedToView[3][2],
                  " profileSpace=",
                  positionSpace == D3D11CapturedPositionSpace::World
                    ? "world" : "view"));
              }
            }
          }
        }
      }
    }

    // Keep the output identity separate from the capture-buffer storage key.
    // The identity describes the draw contract, not the current allocation
    // backing that contract. Dynamic D3D11 buffers are routinely renamed, so
    // including their object addresses here made one mesh acquire a new BLAS
    // identity every frame and eventually allowed cached material/geometry
    // associations to drift. Original vertex contents are incorporated by
    // RasterGeometry::finalizeGeometryHashes; slot/offset/stride still separate
    // distinct streams that share the same shader and draw range.
    const std::string shaderIdentity = commonVs->GetName();
    uint64_t outputIdentity = XXH3_64bits(
      shaderIdentity.data(), shaderIdentity.size());
    auto mixOutputIdentity = [&outputIdentity](uint64_t v) {
      outputIdentity ^= v + 0x9e3779b97f4a7c15ull
        + (outputIdentity << 6) + (outputIdentity >> 2);
    };
    mixOutputIdentity(firstVertex);
    // The capture allocation may grow and shrink with a live particle or
    // foliage batch, but that does not create a new logical draw contract.
    // Key the contract by the per-instance mesh domain; current FirstInstance
    // and InstanceCount are already part of the transform-state hash below,
    // so they still force an exact replay without manufacturing a new capture
    // buffer and BLAS identity every frame.
    mixOutputIdentity(verticesPerInstance);
    mixOutputIdentity(flattenIndexed ? 0x494e4458464c4154ull : 0x564552544558524eull);
    if (flattenIndexed) {
      mixOutputIdentity(start);
      mixOutputIdentity(static_cast<uint32_t>(base));
      const auto& ib = m_context->m_state.ia.indexBuffer;
      mixOutputIdentity(uint64_t(reinterpret_cast<uintptr_t>(ib.buffer.ptr())));
      mixOutputIdentity(ib.offset);
      mixOutputIdentity(static_cast<uint32_t>(ib.format));
    }
    mixOutputIdentity(positionSpace == D3D11CapturedPositionSpace::World ? 1u : 0u);
    mixOutputIdentity(capturesHomogeneousClip ? 0x434c495034ull : 0x5052455033ull);
    if (captureIncludesTexcoord) {
      mixOutputIdentity(XXH3_64bits(
        captureTexcoordName.data(), captureTexcoordName.size()));
      mixOutputIdentity(captureTexcoordIndex);
      mixOutputIdentity(captureTexcoordComponent);
    }
    if (captureIncludesColor) {
      mixOutputIdentity(XXH3_64bits(captureColorName.data(), captureColorName.size()));
      mixOutputIdentity(captureColorIndex);
      mixOutputIdentity(captureColorComponents);
    }
    mixOutputIdentity(usedShaderProvenCaptureTransform ? 0x50524f56454eull : 0x46414c4c4241434bull);
    mixOutputIdentity(capturedClipUsesWDepth ? 0x574445505448ull : 0x4d4154524958ull);
    if (vsResourceIdentity != 0)
      mixOutputIdentity(vsResourceIdentity);
    for (uint32_t slot = 0; slot < D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT; ++slot) {
      if (!captureInputSlots[slot])
        continue;
      const auto& vb = m_context->m_state.ia.vertexBuffers[slot];
      if (vb.buffer == nullptr)
        continue;
      mixOutputIdentity(slot);
      // Per-instance streams are normally suballocated from one engine ring
      // buffer. Their physical offset changes every frame and is not a mesh
      // identity; the stable draw contract plus same-frame occurrence below
      // distinguishes logical instances without manufacturing new BLAS keys.
      if (!capturePerInstanceSlots[slot])
        mixOutputIdentity(vb.offset);
      mixOutputIdentity(vb.stride);
    }
    // Homogeneous capture is converted back to the mesh's pre-instance space,
    // so placement/camera matrices must not become part of BLAS identity. The
    // original IA content hash is folded in asynchronously below. Legacy
    // pre-projection capture still needs its explicit transform contract.
    const uint64_t captureContractIdentity = capturesHomogeneousClip
      ? outputIdentity
      : XXH3_64bits_withSeed(
          &originalObjectToWorld, sizeof(originalObjectToWorld), outputIdentity);

    // Capture-buffer storage must never be keyed by global draw order or by a
    // physical rename slice. Both change routinely between frames. Start with
    // the exact logical draw contract and logical D3D11 input buffers; repeated
    // occurrences of that same contract receive a small per-contract ordinal
    // below, so same-frame instances cannot overwrite one another.
    uint64_t cacheKey = captureContractIdentity;
    auto mixCacheKey = [&cacheKey](uint64_t value) {
      cacheKey ^= value + 0x9e3779b97f4a7c15ull
        + (cacheKey << 6) + (cacheKey >> 2);
    };
    for (uint32_t slot = 0; slot < D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT; ++slot) {
      if (!captureInputSlots[slot])
        continue;
      const auto& vb = m_context->m_state.ia.vertexBuffers[slot];
      if (vb.buffer == nullptr)
        continue;
      mixCacheKey(slot);
      if (!capturePerInstanceSlots[slot]) {
        mixCacheKey(uint64_t(reinterpret_cast<uintptr_t>(vb.buffer.ptr())));
        mixCacheKey(uint64_t(vb.offset));
      }
      mixCacheKey(uint64_t(vb.stride));
    }
    if (flattenIndexed) {
      const auto& ib = m_context->m_state.ia.indexBuffer;
      mixCacheKey(uint64_t(reinterpret_cast<uintptr_t>(ib.buffer.ptr())));
      mixCacheKey(uint64_t(ib.offset));
      mixCacheKey(uint64_t(static_cast<uint32_t>(ib.format)));
      mixCacheKey(uint64_t(start));
      mixCacheKey(uint64_t(static_cast<uint32_t>(base)));
      mixCacheKey(uint64_t(count));
    }
    // Repeated draws can change shader state without changing their buffers.
    // Keep each occurrence separate, including profiled pre-projection output.
    {
      const uint64_t contractKey = cacheKey;
      const uint32_t occurrence =
        m_positionCaptureOccurrencesThisFrame[contractKey]++;
      mixCacheKey(occurrence);
    }
    if (cacheKey == 0)
      cacheKey = 0x9e3779b97f4a7c15ull;

    const uint32_t curFrame = m_context->m_device->getCurrentFrameId();
    VkDeviceSize desiredCapacity = 4096u;
    while (desiredCapacity < captureBytes)
      desiredCapacity <<= 1;

    auto existing = m_positionCaptureCache.find(cacheKey);
    const VkDeviceSize existingCapacity = existing != m_positionCaptureCache.end()
      ? existing->second.capacity
      : 0u;
    const VkDeviceSize additionalCapacity = desiredCapacity > existingCapacity
      ? desiredCapacity - existingCapacity
      : 0u;

    // Keep this cache strictly bounded. Whole-cache resets were unsafe: they
    // released hundreds of capture buffers while transform-feedback and BLAS
    // commands were still in flight, causing allocation spikes and device loss.
    // Evict only least-recently-used entries not referenced this frame; command
    // list Rc references preserve their physical lifetime until GPU completion.
    while (m_positionCaptureCacheBytes + additionalCapacity > kMaxCacheBytes
        || (existing == m_positionCaptureCache.end()
         && m_positionCaptureCache.size() >= kMaxCacheEntries)) {
      auto oldest = m_positionCaptureCache.end();
      for (auto it = m_positionCaptureCache.begin(); it != m_positionCaptureCache.end(); ++it) {
        if (it->first == cacheKey || it->second.lastUsedFrame == curFrame)
          continue;
        if (oldest == m_positionCaptureCache.end()
         || it->second.lastUsedFrame < oldest->second.lastUsedFrame)
          oldest = it;
      }
      if (oldest == m_positionCaptureCache.end())
        return false;
      m_positionCaptureCacheBytes -= oldest->second.capacity;
      m_positionCaptureCache.erase(oldest);
      ++m_submitRejectStats.posCacheEvicted;
      existing = m_positionCaptureCache.find(cacheKey);
    }

    if (existing == m_positionCaptureCache.end())
      ++m_submitRejectStats.posCacheNew;
    PositionCaptureEntry& entry = existing != m_positionCaptureCache.end()
      ? existing->second
      : m_positionCaptureCache.emplace(cacheKey, PositionCaptureEntry()).first->second;
    if (entry.contractIdentity != captureContractIdentity) {
      if (existing != m_positionCaptureCache.end())
        ++m_submitRejectStats.posCacheContractReset;
      entry.contractIdentity = captureContractIdentity;
      entry.lastCapturedFrame = ~0u;
      entry.hasTransformStateIdentity = false;
      entry.hasCanonicalCapturedToWorld = false;
      entry.hasCapturedClipToPosition = false;
      entry.capturedClipUsesWDepth = false;
      entry.hasCapturedViewRotationToWorld = false;
      entry.capturedVertexCount = 0;
      entry.capturedStride = 0;
    }

    // A rigid mesh captured against a real, persistent world/view camera must
    // not be overwritten whenever the camera moves. Preserve the first captured
    // view coordinate system and its matching inverse-view transform as one
    // canonical object space. Camera-relative replacement-camera captures are
    // classified dynamic above because their virtual world itself moves with
    // the camera; skinned and host-visible/renameable inputs are dynamic too.
    // Legacy pre-projection view-space capture needs a canonical coordinate
    // pair. Exact homogeneous capture reconstructs object space and must keep
    // the CURRENT rigid instance placement; freezing camera-relative O2V here
    // is what created camera-following slabs in the earlier implementation.
    if (!capturedDynamicPositions && !capturesHomogeneousClip) {
      if (!entry.hasCanonicalCapturedToWorld) {
        entry.canonicalCapturedToWorld = capturedToWorld;
        entry.hasCanonicalCapturedToWorld = true;
      }
      capturedToWorld = entry.canonicalCapturedToWorld;
    }

    const bool haveUsableBuffer = entry.buffer != nullptr && entry.capacity >= captureBytes;
    const bool haveReusableCapture = haveUsableBuffer
      && entry.lastCapturedFrame != ~0u
      && entry.capturedVertexCount == vertexCount
      && entry.capturedStride == captureStride
      && (!capturesHomogeneousClip || entry.hasCapturedClipToPosition);
    const bool transformStateMatches = capturesHomogeneousClip
      && hasHomogeneousTransformStateIdentity
      && entry.hasTransformStateIdentity
      && entry.transformStateIdentity == homogeneousTransformStateIdentity;
    const bool captureIsCurrent = haveReusableCapture
      && (captureMustReplayEveryFrame
        ? entry.lastCapturedFrame == curFrame
        : (!capturesHomogeneousClip || transformStateMatches));
    entry.lastUsedFrame = curFrame;

    bool reuseStaleCapture = false;
    // Capture fairness. Camera-relative engines (Fallout 4) change every
    // draw's transform state each frame, so every capture asks for a replay
    // and the draws that come first in the frame used the whole budget on
    // replays, every frame. Draws later in the frame were refused with no
    // earlier capture to fall back on, and were missing from the RT scene
    // permanently (which ones depended on draw order, so switching to first
    // person dropped geometry third person had). A world-anchored capture
    // stores its own clip-to-world pairing, so a static mesh served from a
    // recent capture stays exactly where it was. While draws were starved
    // last frame, such meshes skip the replay and leave the budget to the
    // never-captured draws; skinned meshes always replay.
    if (!captureIsCurrent
     && haveReusableCapture
     && entry.hasCanonicalCapturedToWorld
     && !capturedSkinnedPositions
     // Blended overlays (particles, effects) animate and face the camera;
     // an old capture of them is wrong, so they always replay.
     && !dcs.materialData.blendMode.enableBlending
     // Wind-swayed foliage and other shader-animated meshes change every
     // frame; serving them from an older capture made bushes jump between
     // sway phases (smearing, bogus motion vectors, spikes).
     && !commonVs->AnimatesVertices()
     && m_prevFrameStarvedCaptures > 0u
     && entry.lastCapturedFrame != ~0u
     && curFrame - entry.lastCapturedFrame <= 120u) {
      // A stored capture is placed with the eye position of its own frame.
      // That is exact once the engine's eye position is known; with only the
      // geometry-solved estimate (which lags and under-travels) a capture
      // taken before the camera moved lands off by the estimate's error -
      // walls in front of the camera after walking. Reuse it then only if the
      // camera has not moved since it was taken.
      bool placementStillValid = m_eyeOffset != SIZE_MAX;
      if (!placementStillValid) {
        const Matrix4 currentViewToWorld = inverse(originalWorldToView);
        const Vector3 eyeNow(currentViewToWorld[3][0], currentViewToWorld[3][1], currentViewToWorld[3][2]);
        const Vector3 eyeThen(entry.canonicalCapturedToWorld[3][0], entry.canonicalCapturedToWorld[3][1],
                              entry.canonicalCapturedToWorld[3][2]);
        const float eyeMoved = length(eyeNow - eyeThen);
        placementStillValid = std::isfinite(eyeMoved) && eyeMoved < 4.0f;
      }
      if (placementStillValid) {
        reuseStaleCapture = true;
        ++m_submitRejectStats.posCacheStaleReuse;
      }
    }
    if (!captureIsCurrent && !reuseStaleCapture) {
      const bool needsNewCaptureBuffer = !haveUsableBuffer;
      const bool totalBudgetExhausted =
        m_positionCapturesThisFrame >= kMaxCapturesPerFrame;
      // DX11_V319_CAPTURE_LANE_BORROW: a starved lane may borrow the other
      // lane's UNUSED headroom.
      //
      // The per-class caps are a fairness split, not the watchdog protection.
      // What actually bounds per-frame GPU work is the TOTAL cap, the byte
      // ceiling and the submission boundaries below; the split only decides how
      // that total is shared between first-time captures and replays. When one
      // lane sits idle the split stops being fairness and starts dropping
      // geometry for nothing. Measured in Little Nightmares II at the moment a
      // level populated: "draws=64/128 new=64/64 replay=0/64" - the new-buffer
      // lane full, the replay lane completely unused, HALF the frame's total
      // budget unspent, and 120 draws refused. A refused first-time draw has no
      // previous capture to fall back on, so it disappears from the ray-traced
      // scene for that frame; the scene then streams in over many frames and
      // meshes visibly pop in and out.
      //
      // The effective limit for a lane is its own cap plus whatever the other
      // lane has left unused. The caps are per-title tunable options and are
      // not required to sum to the total, so that headroom is computed rather
      // than assumed; totalBudgetExhausted below is what keeps the sum honest,
      // so borrowing can never raise the total work the watchdog sees. Only the
      // MIX changes: a populating frame may now spend the whole budget on new
      // captures instead of stranding half of it.
      const uint32_t ownLaneUsed = needsNewCaptureBuffer
        ? m_positionNewCaptureBuffersThisFrame
        : m_positionReplayCapturesThisFrame;
      const uint32_t ownLaneCap = needsNewCaptureBuffer
        ? kMaxNewCaptureBuffersPerFrame
        : kMaxReplayCapturesPerFrame;
      const uint32_t otherLaneUsed = needsNewCaptureBuffer
        ? m_positionReplayCapturesThisFrame
        : m_positionNewCaptureBuffersThisFrame;
      const uint32_t otherLaneCap = needsNewCaptureBuffer
        ? kMaxReplayCapturesPerFrame
        : kMaxNewCaptureBuffersPerFrame;
      const uint32_t otherLaneUnused =
        otherLaneCap - std::min(otherLaneUsed, otherLaneCap);
      const bool classBudgetExhausted = ownLaneUsed >= ownLaneCap + otherLaneUnused;

      // A small first-time capture is exempt from the draw-count caps. It has
      // no earlier capture to fall back on, so a refusal removes the mesh from
      // the ray-traced scene and leaves it to the native raster. An Unreal 4
      // level's first frame is ~870 draws in only ~4 MiB of capture: with a
      // 128-draw cap, 739 meshes went to raster and half the scene was
      // rasterized. Such captures are cheap (the game draws the same vertices
      // itself) and the 8-capture submission boundaries already bound the GPU
      // work per submission. The byte cap and a hard ceiling still bound the
      // frame. Replays stay capped: they reuse their last capture when refused.
      static constexpr VkDeviceSize kSmallColdCaptureBytes = 256u << 10;
      static constexpr uint32_t     kColdCaptureCeilingScale = 8u;
      const bool smallColdCapture = needsNewCaptureBuffer
        && captureBytes <= kSmallColdCaptureBytes
        && m_positionCapturesThisFrame < kMaxCapturesPerFrame * kColdCaptureCeilingScale;

      const bool captureBudgetExhausted =
        (!smallColdCapture && (totalBudgetExhausted || classBudgetExhausted))
        || m_positionCaptureBytesThisFrame + captureBytes > kMaxCaptureBytesPerFrame;
      if (captureBudgetExhausted) {
        ++m_submitRejectStats.positionCaptureBudgetRejected;
        static uint32_t sPositionCaptureBudgetLogCount = 0;
        if (sPositionCaptureBudgetLogCount < 24) {
          ++sPositionCaptureBudgetLogCount;
          Logger::warn(str::format(
            "[D3D11Rtx][position-capture] exact capture budget exhausted: draws=",
            m_positionCapturesThisFrame, "/", kMaxCapturesPerFrame,
            " new=", m_positionNewCaptureBuffersThisFrame, "/",
            kMaxNewCaptureBuffersPerFrame,
            " replay=", m_positionReplayCapturesThisFrame, "/",
            kMaxReplayCapturesPerFrame,
            " requestedClass=", needsNewCaptureBuffer ? "new" : "replay",
            " bytesMiB=", m_positionCaptureBytesThisFrame >> 20,
            "/", kMaxCaptureBytesPerFrame >> 20,
            " requestedKiB=", captureBytes >> 10,
            " vertices=", vertexCount,
            " indexed=", indexed ? 1 : 0,
            " count=", count,
            " start=", start,
            " base=", base,
            " cameraRelative=", dcs.transformData.cameraRelativeView ? 1 : 0));
        }
        // A budget-refused draw is pending, not absent: keep the cache entry
        // for next frame's retry. Without a previous capture only this draw is
        // missing this frame. With one, reuse it - its clip-to-position pair is
        // stored in the entry and the hash below is seeded with the frame that
        // produced the bytes, so the BLAS is reused instead of rebuilt.
        // Presenting the whole native frame here sent any scene with more than
        // a budget's worth of moving draws back to rasterization.
        if (!haveReusableCapture) {
          ++m_starvedCapturesThisFrame;
          return false;
        }
        reuseStaleCapture = true;
        ++m_submitRejectStats.posCacheStaleReuse;
      }

      if (!reuseStaleCapture && !haveUsableBuffer) {
        DxvkBufferCreateInfo info;
        info.size   = desiredCapacity;
        info.usage  = VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_BUFFER_BIT_EXT
                    | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT
                    | VK_BUFFER_USAGE_TRANSFER_SRC_BIT
                    | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
        info.stages = VK_PIPELINE_STAGE_TRANSFORM_FEEDBACK_BIT_EXT
                    | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT
                    | VK_PIPELINE_STAGE_TRANSFER_BIT;
        info.access = VK_ACCESS_TRANSFORM_FEEDBACK_WRITE_BIT_EXT
                    | VK_ACCESS_SHADER_READ_BIT
                    | VK_ACCESS_TRANSFER_READ_BIT;

        Rc<DxvkBuffer> newBuffer = m_context->m_device->createBuffer(
          info, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
          DxvkMemoryStats::Category::RTXBuffer, "dx11 post-vs position capture");
        if (newBuffer == nullptr) {
          if (entry.buffer == nullptr)
            m_positionCaptureCache.erase(cacheKey);
          return false;
        }
        m_positionCaptureCacheBytes += desiredCapacity - entry.capacity;
        entry.buffer = std::move(newBuffer);
        entry.capacity = desiredCapacity;
        entry.lastCapturedFrame = ~0u;
        entry.hasCapturedClipToPosition = false;
      }

      if (!reuseStaleCapture) {
      // Transform feedback is the sole writer and the following BLAS build is
      // the consumer. Reuse the dedicated device-local allocation and let
      // DxvkContext insert the write/read barriers. Calling allocSlice() here
      // renamed every camera-relative mesh every frame; those retired physical
      // allocations were not represented by m_positionCaptureCacheBytes and
      // grew process memory until Vulkan reported VK_ERROR_DEVICE_LOST.

      // Transform-feedback replays used to accumulate in the application's
      // current Vulkan submission until the whole frame was injected.  A busy
      // Unreal scene can introduce hundreds of previously unseen meshes at a
      // level transition; the resulting monolithic VS/GS workload exceeded the
      // Windows GPU watchdog before BLAS batching was even reached.  Bound both
      // draw count and vertex work, and create an actual queue submission (not
      // merely a CPU command-stream chunk) after each bounded capture batch.
      // A queue submission per capture was itself pathological: an Unreal
      // frame with several dynamic draws generated capture submissions, then
      // another submission per BLAS, then the path tracer.  On NVIDIA that
      // submission storm preceded Event 153 even though every individual draw
      // was small.  Eight draws is still a strict watchdog boundary, while the
      // vertex ceiling splits a single heavy capture batch sooner.
      static constexpr uint32_t kMaxCaptureDrawsPerSubmission = 8u;
      static constexpr uint64_t kMaxCaptureVerticesPerSubmission = 64u << 10;
      const uint64_t queuedCaptureVertices =
        m_positionCaptureVerticesSinceSubmission + uint64_t(vertexCount);

      // DX11_V302_CAPTURE_BOUNDARY_GATE: the boundary below flushes AND blocks
      // the render thread on waitForResource until the GPU retires the XFB
      // write - a full CPU/GPU round trip. Under FIFO present that round trip
      // costs ~100ms because it queues behind pending presents, and measurement
      // showed a single 2-triangle capture eating an entire frame while a
      // 2000-draw world-load frame cost only ~12ms in total.
      //
      // The guard is meant to stop a capture *storm* from outrunning the GPU,
      // so only engage it once a frame is actually capture-heavy. Light frames
      // hit the every-8-captures rule with no storm to prevent and paid the
      // stall for nothing; the vertex ceiling still splits genuinely heavy
      // batches regardless of count.
      const bool frameIsCaptureHeavy =
        m_positionCapturesThisFrame >= RtxOptions::positionCaptureThrottleMinDrawsPerFrame();
      const bool forceCaptureSubmissionBoundary =
        (frameIsCaptureHeavy
          && ((m_positionCapturesThisFrame + 1u) % kMaxCaptureDrawsPerSubmission) == 0u)
        || queuedCaptureVertices >= kMaxCaptureVerticesPerSubmission;

      // Camera estimators consume completed staging batches asynchronously.
      // Capture submission boundaries bound GPU work without CPU readback waits.
      m_positionCaptureVerticesSinceSubmission = forceCaptureSubmissionBoundary
        ? 0u : queuedCaptureVertices;

      // DX11_V298_CONTRACT_LOG_FLOOD: engines that suballocate dynamic vertex
      // streams mint a fresh contract identity every frame, and the old 4096
      // cap let them flood the log with thousands of per-contract lines
      // (each carrying its texture hash - the reported "texture hash loading
      // floods"). Log only the first few contracts by default; set
      // DXVK_REMIX_CAPTURE_LOG=1 to restore the full diagnostic stream.
      static const size_t kMaxLoggedPositionCaptureContracts =
        env::getEnvVar("DXVK_REMIX_CAPTURE_LOG") == "1" ? 4096u : 16u;
      if (m_positionCaptureContractsLogged.size() < kMaxLoggedPositionCaptureContracts
       && m_positionCaptureContractsLogged.insert(captureContractIdentity).second) {
        bool hasVertexShaderResource = false;
        for (const auto& view : m_context->m_state.vs.shaderResources.views)
          hasVertexShaderResource |= view != nullptr;

        std::string inputBuffers;
        for (uint32_t slot = 0; slot < captureInputSlots.size(); ++slot) {
          if (!captureInputSlots[slot])
            continue;
          const auto& vb = m_context->m_state.ia.vertexBuffers[slot];
          if (vb.buffer == nullptr)
            continue;
          if (!inputBuffers.empty())
            inputBuffers += ";";
          inputBuffers += str::format(
            "s", slot,
            "(bytes=", vb.buffer->Desc()->ByteWidth,
            ",offset=", vb.offset,
            ",stride=", vb.stride,
            ",instance=", capturePerInstanceSlots[slot] ? 1 : 0,
            ")");
        }

        const auto& ib = m_context->m_state.ia.indexBuffer;
        Logger::info(str::format(
          "[D3D11Rtx][position-capture-contract] frame=", curFrame,
          " contract=0x", std::hex, captureContractIdentity,
          " vs=", commonVs->GetName(),
          " ps=", commonPs != nullptr ? commonPs->GetName() : "none",
          " texture=0x", dcs.materialData.getColorTexture().getImageHash(),
          std::dec,
          " drawId=", dcs.drawCallID,
          " topology=", static_cast<uint32_t>(m_context->m_state.ia.primitiveTopology),
          " indexed=", indexed ? 1 : 0,
          " flatten=", flattenIndexed ? 1 : 0,
          " count=", count,
          " start=", start,
          " base=", base,
          " firstVertex=", firstVertex,
          " vertices=", vertexCount,
          " firstInstance=", replayFirstInstance,
          " instances=", replayInstanceCount,
          " captureBytes=", captureBytes,
          " stride=", captureStride,
          " texcoord=", captureIncludesTexcoord
            ? str::format(captureTexcoordName, captureTexcoordIndex,
                "[", captureTexcoordComponent, ":",
                captureTexcoordComponent + 1u, "]")
            : "none",
          " psLinked=", hasPsSampledTexcoord ? 1 : 0,
          " vsSrv=", hasVertexShaderResource ? 1 : 0,
          " zEnable=", dcs.zEnable ? 1 : 0,
          " zWrite=", dcs.zWriteEnable ? 1 : 0,
          " minZ=", dcs.minZ,
          " maxZ=", dcs.maxZ,
          " fallbackCamera=", dcs.transformData.usedViewportFallbackProjection ? 1 : 0,
          " identityWorld=", isIdentityExact(dcs.transformData.objectToWorld) ? 1 : 0,
          " identityView=", isIdentityExact(dcs.transformData.worldToView) ? 1 : 0,
          " cameraRelative=", dcs.transformData.cameraRelativeView ? 1 : 0,
          " dynamic=", capturedDynamicPositions ? 1 : 0,
          " replayEveryFrame=", captureMustReplayEveryFrame ? 1 : 0,
          " stateIdentity=", hasHomogeneousTransformStateIdentity ? 1 : 0,
          " newBuffer=", needsNewCaptureBuffer ? 1 : 0,
          " ibBytes=", ib.buffer != nullptr ? ib.buffer->Desc()->ByteWidth : 0u,
          " ibOffset=", ib.offset,
          " ibFormat=", static_cast<uint32_t>(ib.format),
          " inputs=[", inputBuffers, "]"));
      }

      m_context->EmitCs([cGs = std::move(captureGs),
                         cBuf = DxvkBufferSlice(entry.buffer, 0, captureBytes),
                         cCount = verticesPerInstance,
                         cInstanceCount = replayInstanceCount,
                         cFirst = firstVertex,
                         cRestoreIa = restoreIa,
                         cFirstInstance = replayFirstInstance,
                         cFlattenIndexed = flattenIndexed,
                         cStartIndex = start,
                         cBaseVertex = base,
                         cStride = captureStride,
                         cTessellated = tessellated || gsCapture,
                         cGameGs = gsCapture ? commonGs->GetShader() : Rc<DxvkShader>(),
                         cIndexed = indexed,
                         cDrawCount = count,
                         cIndirect = indirectReplay,
                         cIndirectIndexed = m_indirectReplay.indexed,
                         cIndirectArgs = m_indirectReplay.args,
                         cIndirectOffset = m_indirectReplay.offset,
                         cForceSubmissionBoundary = forceCaptureSubmissionBoundary](DxvkContext* ctx) {
        if (cIndirect) {
          // GPU-driven draw: replay the same indirect arguments as a point
          // stream into a NaN-prefilled buffer; XFB stops at capacity and the
          // unwritten tail stays inactive in the BLAS.
          ctx->clearBuffer(cBuf.buffer(), cBuf.offset(), cBuf.length(), 0x7fc00000u);
          const DxvkInputAssemblyState pointIa = { VK_PRIMITIVE_TOPOLOGY_POINT_LIST, VK_FALSE, 0 };
          ctx->bindShader(VK_SHADER_STAGE_GEOMETRY_BIT, cGs);
          ctx->setInputAssemblyState(pointIa);
          ctx->bindXfbBuffer(0, cBuf, DxvkBufferSlice());
          ctx->bindDrawBuffers(cIndirectArgs, DxvkBufferSlice());
          if (cIndirectIndexed)
            ctx->drawIndexedIndirect(cIndirectOffset, 1, sizeof(VkDrawIndexedIndirectCommand));
          else
            ctx->drawIndirect(cIndirectOffset, 1, sizeof(VkDrawIndirectCommand));
          ctx->bindXfbBuffer(0, DxvkBufferSlice(), DxvkBufferSlice());
          ctx->bindShader(VK_SHADER_STAGE_GEOMETRY_BIT, nullptr);
          ctx->setInputAssemblyState(cRestoreIa);
          if (cForceSubmissionBoundary)
            ctx->DxvkContext::flushCommandList();
          return;
        }
        if (cTessellated) {
          // GPU-sized output: prefill with NaN (inactive triangles), replay the
          // original patch-list draw once through VS/HS/DS + capture GS.
          ctx->clearBuffer(cBuf.buffer(), cBuf.offset(), cBuf.length(), 0x7fc00000u);
          ctx->bindShader(VK_SHADER_STAGE_GEOMETRY_BIT, cGs);
          ctx->setInputAssemblyState(cRestoreIa);
          ctx->bindXfbBuffer(0, cBuf, DxvkBufferSlice());
          if (cIndexed)
            ctx->drawIndexed(cDrawCount, cInstanceCount, cStartIndex, cBaseVertex, cFirstInstance);
          else
            ctx->draw(cDrawCount, cInstanceCount, cFirst, cFirstInstance);
          ctx->bindXfbBuffer(0, DxvkBufferSlice(), DxvkBufferSlice());
          // A captured game GS goes back in place for the game's own draw;
          // the D3D11 layer believes it is still bound.
          ctx->bindShader(VK_SHADER_STAGE_GEOMETRY_BIT, cGameGs);
          if (cForceSubmissionBoundary)
            ctx->DxvkContext::flushCommandList();
          return;
        }
        const DxvkInputAssemblyState pointIa = { VK_PRIMITIVE_TOPOLOGY_POINT_LIST, VK_FALSE, 0 };
        ctx->bindShader(VK_SHADER_STAGE_GEOMETRY_BIT, cGs);
        ctx->setInputAssemblyState(pointIa);
        // Split the vertex/index range, never the instance range. Every replay
        // keeps the original SV_InstanceID and divisor-based IA inputs. Chunk
        // boundaries preserve complete triangles, so the chunk-major output
        // is an equivalent non-indexed triangle list even with instancing.
        constexpr uint32_t kMaxReplayVerticesPerSubmission = 256u << 10;
        const uint32_t chunkLimit = std::max(3u,
          (kMaxReplayVerticesPerSubmission / cInstanceCount / 3u) * 3u);
        VkDeviceSize outputOffset = 0;
        for (uint32_t first = 0; first < cCount;) {
          const uint32_t chunkCount = std::min(chunkLimit, cCount - first);
          const VkDeviceSize chunkBytes = VkDeviceSize(chunkCount) * cInstanceCount * cStride;
          ctx->bindXfbBuffer(0, cBuf.subSlice(outputOffset, chunkBytes), DxvkBufferSlice());
          if (cFlattenIndexed) {
            ctx->drawIndexed(chunkCount, cInstanceCount, cStartIndex + first, cBaseVertex, cFirstInstance);
          } else {
            ctx->draw(chunkCount, cInstanceCount, cFirst + first, cFirstInstance);
          }
          first += chunkCount;
          outputOffset += chunkBytes;
          ctx->bindXfbBuffer(0, DxvkBufferSlice(), DxvkBufferSlice());
          if (first < cCount)
            ctx->DxvkContext::flushCommandList();
        }
        ctx->bindShader(VK_SHADER_STAGE_GEOMETRY_BIT, nullptr);
        ctx->setInputAssemblyState(cRestoreIa);
        if (cForceSubmissionBoundary) {
          // This replay runs before RTX injection, so use the base DXVK flush:
          // it submits the captured buffers and starts a fully dirty command
          // list without invoking RtxContext's end-of-frame sky handling.
          ctx->DxvkContext::flushCommandList();

        }
      });

      if (forceCaptureSubmissionBoundary) {
        static uint32_t sCaptureSubmissionLogs = 0;
        if (sCaptureSubmissionLogs < 32u) {
          ++sCaptureSubmissionLogs;
          Logger::info(str::format(
            "[D3D11Rtx][position-capture] queued watchdog-safe submission boundary: frame=",
            curFrame,
            " captures=", m_positionCapturesThisFrame + 1u,
            " lastVertices=", vertexCount));
        }
      }

      entry.lastCapturedFrame = curFrame;
      entry.capturedVertexCount = vertexCount;
      entry.capturedStride = captureStride;
      if (capturesHomogeneousClip) {
        entry.capturedClipToPosition = capturedClipToPosition;
        entry.hasCapturedClipToPosition = true;
        entry.capturedClipUsesWDepth = capturedClipUsesWDepth;
      }
      if (capturesHomogeneousClip && hasHomogeneousTransformStateIdentity) {
        entry.transformStateIdentity = homogeneousTransformStateIdentity;
        entry.hasTransformStateIdentity = true;
      } else {
        entry.hasTransformStateIdentity = false;
      }
      // Store the inverse-view transform in the same cache entry as the clip
      // buffer and inverse projection. If a bounded replay lane reuses stale
      // bytes, using the current frame's inverse view with an older view-space
      // capture makes geometry translate/rotate with the camera.
      if (capturesHomogeneousClip && homogeneousHasStableGameView) {
        entry.canonicalCapturedToWorld = capturedToWorld;
        entry.hasCanonicalCapturedToWorld = true;
        // DX11_V319_WORLD_ANCHOR_CAMERA: the rotation-only half of the same
        // transform, plus the geometry of these exact bytes. The world anchor
        // has to difference offsets in the game's own camera-relative frame;
        // canonicalCapturedToWorld carries the anchor translation being solved
        // for, so reusing it here would close a feedback loop that cancels the
        // very motion the solve is measuring.
        entry.capturedViewRotationToWorld = viewRotationToWorld(originalWorldToView);
        entry.hasCapturedViewRotationToWorld = true;

        // Temporary diagnostic: game camera forward vs Remix camera forward.
        {
          static uint32_t s_lastYawLogFrame = 0;
          const uint32_t yawFrame = m_context->m_device->getCurrentFrameId();
          if (yawFrame >= s_lastYawLogFrame + 20u) {
            s_lastYawLogFrame = yawFrame;
            const Matrix4& v = originalWorldToView;
            const Vector3 gameForward(v[0][2], v[1][2], v[2][2]);
            const Vector3 gameRight(v[0][0], v[1][0], v[2][0]);
            const auto& camera = m_context->m_device->getCommon()->getSceneManager()
              .getCameraManager().getCamera(CameraType::Main);
            const Vector3 remixForward = camera.getDirection(false);
            const Vector3 remixRight = camera.getRight(false);
            const float det3 =
                v[0][0] * (v[1][1] * v[2][2] - v[2][1] * v[1][2])
              - v[1][0] * (v[0][1] * v[2][2] - v[2][1] * v[0][2])
              + v[2][0] * (v[0][1] * v[1][2] - v[1][1] * v[0][2]);
            Logger::info(str::format("[D3D11Rtx][yaw] frame=", yawFrame,
              " gameFwd=(", gameForward.x, ",", gameForward.y, ",", gameForward.z, ")",
              " gameRight=(", gameRight.x, ",", gameRight.y, ",", gameRight.z, ")",
              " remixFwd=(", remixForward.x, ",", remixForward.y, ",", remixForward.z, ")",
              " remixRight=(", remixRight.x, ",", remixRight.y, ",", remixRight.z, ")",
              " viewDet=", det3));
          }
        }
      } else if (capturesHomogeneousClip) {
        entry.hasCanonicalCapturedToWorld = false;
        entry.hasCapturedViewRotationToWorld = false;
      }
      ++m_positionCapturesThisFrame;
      if (needsNewCaptureBuffer)
        ++m_positionNewCaptureBuffersThisFrame;
      else
        ++m_positionReplayCapturesThisFrame;
      m_positionCaptureBytesThisFrame += captureBytes;
      }
    }

    // DX11_V319_WORLD_ANCHOR_CAMERA: queue a few of this mesh's freshly written
    // vertices for readback next frame. Only a capture that actually happened
    // this frame is sampled - a reused buffer still holds an older camera's
    // bytes and would read as motionless, dragging the median towards zero.
    if (capturesHomogeneousClip
     && m_cameraAnchorViewTranslationFree
     && homogeneousHasStableGameView
     && entry.lastCapturedFrame == curFrame) {
      QueueCameraAnchorSample(entry, cacheKey);
    }

    // First person: the game still draws the player's full third-person body
    // (for shadows and reflections) with the eye inside its head. Path traced,
    // that body is a shell around the camera that smears across the whole
    // view. Skinned meshes in the main depth range are probed (readback next
    // frame); one surrounding the camera is given Remix's player-model
    // category, which hides it from camera rays while keeping its shadow and
    // reflection. First-person arms use the reserved depth range and are
    // never probed.
    {
      const bool skinnedMesh = geo.blendWeightBuffer.defined() && geo.blendIndicesBuffer.defined();
      const bool reservedDepthPass = m_context->m_state.rs.numViewports > 0
        && isReservedDepthViewport(m_context->m_state.rs.viewports[0]);
      if (RtxOptions::dx11DetectPlayerBody() && skinnedMesh && !reservedDepthPass && capturesHomogeneousClip) {
        if (entry.lastCapturedFrame == curFrame && entry.hasCapturedViewRotationToWorld) {
          uint32_t& lastProbe = m_playerProbeLastFrame[cacheKey];
          if (lastProbe == 0u || curFrame >= lastProbe + 15u) {
            lastProbe = curFrame;
            QueueCameraAnchorSample(entry, cacheKey, false, true);
          }
        }
        const auto found = m_playerBodyKeys.find(cacheKey);
        if (found != m_playerBodyKeys.end() && curFrame <= found->second + 60u)
          dcs.setCategory(InstanceCategories::ThirdPersonPlayerModel, true);
      }
      if (m_playerProbeLastFrame.size() > 8192u)
        m_playerProbeLastFrame.clear();
    }

    if (capturesHomogeneousClip) {
      // Never combine clip coordinates from an earlier capture with the
      // current frame's inverse projection. That mismatch is a moving box/
      // overlap around the camera. Projection-only changes deliberately reuse
      // the paired matrix stored with the captured buffer.
      if (!entry.hasCapturedClipToPosition)
        return false;
      // A lifted 2D layer's matrix holds only this frame's layer depth; clip
      // is NDC, so it is valid for any capture of the draw.
      if (!m_lift2DDraw)
        capturedClipToPosition = entry.capturedClipToPosition;
      capturedClipUsesWDepth = entry.capturedClipUsesWDepth;

      if (entry.hasCanonicalCapturedToWorld) {
        capturedToWorld = entry.canonicalCapturedToWorld;
        // The current real camera views the stable world. During a brief
        // camera-extraction gap, retain the exact view paired with the capture
        // rather than dropping back to a camera-relative identity world.
        if (!homogeneousHasStableGameView)
          dcs.transformData.worldToView = inverse(capturedToWorld);
        capturedToView = dcs.transformData.worldToView * capturedToWorld;
        useWorldAnchoredHomogeneousCapture = true;
      }
    }

    const RasterBuffer capturedPositions(
      DxvkBufferSlice(entry.buffer, 0, captureBytes),
      0, captureStride,
      capturesHomogeneousClip
        ? VK_FORMAT_R32G32B32A32_SFLOAT
        : VK_FORMAT_R32G32B32_SFLOAT);
    geo.positionBuffer = capturedPositions;
    dcs.geometryData.positionBuffer = capturedPositions;
    const RasterBuffer capturedTexcoords = captureIncludesTexcoord
      ? RasterBuffer(
          DxvkBufferSlice(entry.buffer, 0, captureBytes),
          positionBytes, captureStride, VK_FORMAT_R32G32_SFLOAT)
      : RasterBuffer();
    if (captureIncludesTexcoord) {
      geo.texcoordBuffer = capturedTexcoords;
      dcs.geometryData.texcoordBuffer = capturedTexcoords;
    }
    // Captured vertex colour (float RGB / RGBA): the interleaver packs it
    // into the BGRA8 word Remix's shaders read. The pixel shader multiplies
    // its texture by this input, so it tints the albedo.
    const RasterBuffer capturedColors = captureIncludesColor
      ? RasterBuffer(
          DxvkBufferSlice(entry.buffer, 0, captureBytes),
          captureColorOffset, captureStride,
          captureColorComponents == 4u ? VK_FORMAT_R32G32B32A32_SFLOAT : VK_FORMAT_R32G32B32_SFLOAT)
      : RasterBuffer();
    if (tessellated || gsCapture) {
      // The captured stream is the rasterizer's triangle list, whatever the
      // draw's input topology (patches, points expanded by a GS).
      geo.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
      dcs.geometryData.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    }
    if (flattenIndexed || multiInstanceCapture || tessellated || gsCapture) {
      // XFB emitted one compact vertex stream containing every selected
      // instance. The original one-instance index/attribute streams cannot be
      // applied to that appended domain. TEXCOORD is the exception: it was
      // emitted beside position by the same replay and already matches it.
      geo.indexBuffer = RasterBuffer();
      geo.indexCount = 0;
      geo.vertexCount = vertexCount;
      geo.color0Buffer = RasterBuffer();
      dcs.geometryData.indexBuffer = RasterBuffer();
      dcs.geometryData.indexCount = 0;
      dcs.geometryData.vertexCount = vertexCount;
      dcs.geometryData.color0Buffer = RasterBuffer();
      if (!captureIncludesTexcoord) {
        geo.texcoordBuffer = RasterBuffer();
        dcs.geometryData.texcoordBuffer = RasterBuffer();
      }
    }
    if (captureIncludesColor) {
      geo.color0Buffer = capturedColors;
      dcs.geometryData.color0Buffer = capturedColors;
      dcs.materialData.modulateVertexColor = true;
      dcs.materialData.isVertexColorBakedLighting = false;
    }
    geo.postVsPositionIsHomogeneousClip = capturesHomogeneousClip;
    dcs.geometryData.postVsPositionIsHomogeneousClip = capturesHomogeneousClip;
    geo.postVsClipUsesWDepth = capturesHomogeneousClip && capturedClipUsesWDepth;
    dcs.geometryData.postVsClipUsesWDepth = capturesHomogeneousClip && capturedClipUsesWDepth;
    geo.postVsCapturedPositionsDynamic = capturedDynamicPositions;
    dcs.geometryData.postVsCapturedPositionsDynamic = capturedDynamicPositions;
    if (capturesHomogeneousClip) {
      geo.postVsClipToPosition = capturedClipToPosition;
      dcs.geometryData.postVsClipToPosition = capturedClipToPosition;
    }

    // The VS has already performed skinning and every object/view transform.
    // Do not run Remix skinning or transform the original object-space normals
    // a second time. Missing normals are regenerated from the captured geometry.
    geo.normalBuffer = RasterBuffer();
    geo.blendWeightBuffer = RasterBuffer();
    geo.blendIndicesBuffer = RasterBuffer();
    geo.numBonesPerVertex = 0;
    geo.boundingBox.invalidate();
    dcs.geometryData.normalBuffer = RasterBuffer();
    dcs.geometryData.blendWeightBuffer = RasterBuffer();
    dcs.geometryData.blendIndicesBuffer = RasterBuffer();
    dcs.geometryData.numBonesPerVertex = 0;
    dcs.geometryData.boundingBox.invalidate();
    dcs.futureSkinningData = Future<SkinningData>();
    dcs.skinningData = SkinningData();

    // Build a camera-independent identity for the captured output. Hashing all
    // VS constant bytes included view/projection matrices, forcing a fresh BLAS
    // for every static object on every camera movement until the process ran out
    // of memory. The original IA position hash is combined later; objectToWorld
    // separates placed instances, while skinned or renameable input is explicitly
    // dynamic because its deformation has already been baked by the captured VS.
    XXH64_hash_t outputHash = captureContractIdentity;
    if (!capturedDynamicPositions && !capturesHomogeneousClip) {
      // If a cache entry is evicted and later recreated under another camera,
      // the new canonical view space must not reuse a BLAS containing vertices
      // from the old one.
      outputHash = XXH3_64bits_withSeed(
        &capturedToWorld, sizeof(capturedToWorld), outputHash);
    }
    if (outputHash == 0)
      outputHash = 0x9e3779b97f4a7c15ull;
    geo.postVsCaptureIdentity = outputHash;
    dcs.geometryData.postVsCaptureIdentity = outputHash;

    // Dynamic output is already animated/renamed before it reaches the capture
    // stream, so update its cached vertices and refit its BLAS every frame.
    // Rigid view-space output instead uses the immutable canonical pair above.
    if (capturesHomogeneousClip && entry.hasTransformStateIdentity) {
      outputHash = XXH3_64bits_withSeed(
        &entry.transformStateIdentity,
        sizeof(entry.transformStateIdentity), outputHash);
    } else if (captureMustReplayEveryFrame) {
      // The buffer can intentionally be reused when its bounded replay lane is
      // full. Seed the content identity with the frame that actually produced
      // these bytes, not the current frame, so stale reuse stays a stable BLAS.
      outputHash = XXH3_64bits_withSeed(
        &entry.lastCapturedFrame, sizeof(entry.lastCapturedFrame), outputHash);
    }
    if (outputHash == 0)
      outputHash = 0x9e3779b97f4a7c15ull;
    geo.postVsPositionHashSeed = outputHash;
    geo.hasPostVsPositionHashSeed = true;
    dcs.geometryData.postVsPositionHashSeed = outputHash;
    dcs.geometryData.hasPostVsPositionHashSeed = true;

    dcs.transformData.objectToWorld = capturedToWorld;
    dcs.transformData.objectToView = capturedToView;
    if (capturesHomogeneousClip) {
      if (!useWorldAnchoredHomogeneousCapture) {
        dcs.transformData.worldToView = Matrix4();

        // Feed the estimator through host-visible staging. The capture itself
        // is device-local, and must never be mapped or waited on here.
        if (RtxOptions::estimateViewSpaceCameraMotion()
         && !isKnownEmulatorHostProcess()
         && !m_viewConfirmed) {
          if (entry.lastCapturedFrame == curFrame)
            QueueCameraAnchorSample(entry, cacheKey, true);

          // DX11_V293_CONFIDENCE_GATE: before the first successful solve the
          // estimated pose is only the seed; applying it changed menu/intro
          // frames (no trackable meshes - e.g. Call of Duty front-ends) away
          // from the proven camera-relative fallback. Keep the original
          // fallback bit-for-bit until real camera motion has been solved.
          if (m_cameraTrackingState->viewSpace.hasConfidentPose()) {
            dcs.transformData.worldToView = m_cameraTrackingState->viewSpace.worldToView();
            dcs.transformData.objectToWorld = m_cameraTrackingState->viewSpace.viewToWorld();
            dcs.transformData.cameraRelativeView = false;
          }
        }
      }
      // A real view anchors captured vertices in stable world space. Only the
      // no-view fallback remains camera-relative; this keeps the fallback safe
      // while allowing normal/free cameras to move independently of geometry.
      if (useWorldAnchoredHomogeneousCapture) {
        dcs.transformData.cameraRelativeView = false;
      } else if (m_viewConfirmed && !m_viewCameraRelative) {
        // A positively confirmed view matrix is a real view, so the captured
        // vertices anchor in world space. The fallback below keys off camera
        // POSE ESTIMATION rather than off whether a view exists, which strands
        // games whose camera the estimator cannot solve - a custom or otherwise
        // non-standard camera never reaches a confident pose.
        //
        // m_viewCameraRelative must be honoured here. When the confirmed view is
        // the camera-relative IDENTITY view, the captured vertices are already in
        // camera space and world space *is* camera space; forcing world anchoring
        // then strips the flag the confirmation just set, and the geometry gets
        // treated as world-placed with no translation - which pins it to the eye
        // and makes it travel with the player.
        dcs.transformData.cameraRelativeView = false;

        static bool sWorldAnchoredByConfirmedViewLogged = false;
        if (!sWorldAnchoredByConfirmedViewLogged) {
          sWorldAnchoredByConfirmedViewLogged = true;
          Logger::info(
            "[D3D11Rtx] Anchoring captured geometry to world space from the confirmed view "
            "matrix (camera pose estimation is not confident, but a real view exists).");
        }
      } else if (!RtxOptions::estimateViewSpaceCameraMotion()
            || isKnownEmulatorHostProcess()
            || !m_cameraTrackingState->viewSpace.hasConfidentPose()) {
        dcs.transformData.cameraRelativeView = true;
      }
      dcs.transformData.exactReplacementCamera = true;

      // Once the game's real view is confirmed, a draw that does not carry it
      // (identity view) belongs to one of the game's other cameras - Pip-Boy,
      // menus, inventory preview, render-to-texture. It must not steer the
      // main RT camera: when such a draw came first in a frame it won the
      // camera's first-touch and the scene was viewed from the origin with an
      // identity rotation (the frame then fell back to raster, and the camera
      // appeared not to update). The draw itself is left untouched.
      if (m_viewConfirmed && !m_viewCameraRelative
       && isIdentityExact(dcs.transformData.worldToView)) {
        dcs.allowMainCameraUpdate = false;
        ++m_submitRejectStats.otherCameraNoSteer;
      }
      // The geometry and camera now form one exact replacement coordinate
      // system. Treat it as a real camera even when the projection was derived
      // from the viewport; the old fallback marker would make EndFrame reject
      // this valid path and leave optimized Unity scenes permanently raster-only.
      dcs.transformData.usedViewportFallbackProjection = false;

    } else {
      dcs.transformData.cameraRelativeView = false;
      dcs.transformData.exactReplacementCamera = false;
    }

    static uint32_t sPositionCaptureLogCount = 0;
    if (sPositionCaptureLogCount < 24) {
      ++sPositionCaptureLogCount;
      Logger::info(str::format(
        "[D3D11Rtx] V290: captured post-VS positions for RT geometry (space=",
        positionSpace == D3D11CapturedPositionSpace::World ? "world" : "view",
        ", verts=",
        vertexCount, ", indexed=", indexed ? 1 : 0,
        ", indexedFlatten=", flattenIndexed ? 1 : 0,
        ", homogeneousClip=", capturesHomogeneousClip ? 1 : 0,
        ", clipWDepth=", capturedClipUsesWDepth ? 1 : 0,
        // DX11_V317_ANCHOR_PROBE: why captured geometry is or is not re-anchored
        // into a stable world. space=view means the vertices go into the RT scene
        // still expressed relative to the camera, so the whole world translates
        // and rotates with the eye - the "geometry moves with camera" symptom.
        //
        // Anchoring needs homogeneousHasStableGameView, which is three ANDed
        // conditions; printing each separately says which one fails instead of
        // leaving it to inference:
        //   stableView   = the composite gate
        //   camRel       = cameraRelativeView (must be 0)
        //   idView       = worldToView is identity (must be 0)
        //   canonWorld   = a canonical captured-to-world was stored on the entry
        //   worldAnchor  = useWorldAnchoredHomogeneousCapture actually taken
        ", stableView=", homogeneousHasStableGameView ? 1 : 0,
        ", camRel=", dcs.transformData.cameraRelativeView ? 1 : 0,
        ", idView=", isIdentityExact(originalWorldToView) ? 1 : 0,
        ", worldAnchor=", useWorldAnchoredHomogeneousCapture ? 1 : 0,
        ", firstInstance=", replayFirstInstance,
        ", instanceCount=", replayInstanceCount,
        ", dynamic=", capturedDynamicPositions ? 1 : 0,
        ", replayEveryFrame=", captureMustReplayEveryFrame ? 1 : 0,
        ", transformState=", hasHomogeneousTransformStateIdentity ? 1 : 0,
        ", transformStateSource=",
        usedCompleteShaderStateIdentity ? "complete" :
          (usedShaderDependencyProfile ? "dxbc-profile" : "proven"),
        ", vs=", commonVs->GetName(),
        ", drawId=", dcs.drawCallID,
        ", count=", count,
        ", start=", start,
        ", base=", base,
        ", projectionDiag=",
        dcs.transformData.viewToProjection[0][0], ",",
        dcs.transformData.viewToProjection[1][1], ",",
        dcs.transformData.viewToProjection[2][2],
        ", objectToViewTranslation=",
        originalObjectToView[3][0], ",",
        originalObjectToView[3][1], ",",
        originalObjectToView[3][2], ")"));
    }
    return true;
  }

  void D3D11Rtx::SweepPositionCaptureCache(uint32_t currentFrame) {
    ScopedCpuProfileZoneN("D3D11Rtx::SweepPositionCaptureCache");
    static constexpr uint32_t     kEvictAfterFrames = 120u;
    static constexpr VkDeviceSize kMaxCacheBytes    = 384ull << 20;

    const bool overBudget = m_positionCaptureCacheBytes > kMaxCacheBytes;
    if (!overBudget && (currentFrame & 63u) != 0u)
      return;

    for (auto it = m_positionCaptureCache.begin(); it != m_positionCaptureCache.end();) {
      if (it->second.lastUsedFrame + kEvictAfterFrames < currentFrame) {
        m_positionCaptureCacheBytes -= it->second.capacity;
        it = m_positionCaptureCache.erase(it);
      } else {
        ++it;
      }
    }

    // Allocation enforces this cap. If accounting ever drifts over it, evict
    // entries individually; never clear buffers that may still be referenced
    // by in-flight capture or BLAS work.
    while (m_positionCaptureCacheBytes > kMaxCacheBytes
        && !m_positionCaptureCache.empty()) {
      auto oldest = m_positionCaptureCache.begin();
      for (auto it = std::next(m_positionCaptureCache.begin());
           it != m_positionCaptureCache.end(); ++it) {
        if (it->second.lastUsedFrame < oldest->second.lastUsedFrame)
          oldest = it;
      }
      m_positionCaptureCacheBytes -= oldest->second.capacity;
      m_positionCaptureCache.erase(oldest);
    }
  }

  void D3D11Rtx::QueueCameraAnchorSample(const PositionCaptureEntry& entry,
                                         uint64_t meshKey, bool viewSpaceCamera,
                                         bool playerProbe) {
    ScopedCpuProfileZoneN("D3D11Rtx::QueueCameraAnchorSample");
    const uint32_t currentFrame = m_context->m_device->getCurrentFrameId();
    if (m_cameraAnchorLastConsumedFrame == currentFrame || entry.buffer == nullptr
     || !entry.hasCapturedClipToPosition || entry.capturedStride < sizeof(float) * 4u
     || entry.capturedVertexCount < 3u || m_context->m_device->getDeviceStatus() != VK_SUCCESS)
      return;

    // Translation-only anchoring needs the game's real view rotation. The
    // full view-space estimator can also use the paired synthetic clip-W space.
    if (!viewSpaceCamera
     && (!entry.hasCapturedViewRotationToWorld || entry.capturedClipUsesWDepth))
      return;

    const uint32_t sampleVertices = std::min(kCameraAnchorSampleVertices, entry.capturedVertexCount);
    const VkDeviceSize sampleBytes = VkDeviceSize(sampleVertices) * entry.capturedStride;
    if (sampleBytes > kCameraAnchorSampleSlotBytes || sampleBytes > entry.capacity)
      return;

    if (m_cameraAnchorWriteIndex < kCameraAnchorBatchCount
     && m_cameraAnchorBatches[m_cameraAnchorWriteIndex].frame != currentFrame)
      SealCameraAnchorSamples();

    if (m_cameraAnchorWriteIndex == kCameraAnchorBatchCount) {
      for (uint32_t index = 0; index < kCameraAnchorBatchCount; ++index) {
        if (m_cameraAnchorBatches[index].requests.empty()) {
          m_cameraAnchorWriteIndex = index;
          break;
        }
      }
      // All batches are queued or executing. Skip this sample rather than
      // overwriting pending GPU copies or serializing the render thread.
      if (m_cameraAnchorWriteIndex == kCameraAnchorBatchCount)
        return;
    }

    auto& batch = m_cameraAnchorBatches[m_cameraAnchorWriteIndex];
    if (batch.requests.size() >= kCameraAnchorMaxSampleMeshes)
      return;
    for (const auto& request : batch.requests) {
      if (request.meshKey == meshKey && request.viewSpaceCamera == viewSpaceCamera
       && request.playerProbe == playerProbe)
        return;
    }

    if (batch.staging == nullptr) {
      DxvkBufferCreateInfo info;
      info.size = VkDeviceSize(kCameraAnchorMaxSampleMeshes) * kCameraAnchorSampleSlotBytes;
      info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
      // copyBuffer publishes transfer writes to these host reads before the
      // command list signals completion. The allocation is host-coherent.
      info.stages = VK_PIPELINE_STAGE_HOST_BIT;
      info.access = VK_ACCESS_HOST_READ_BIT;
      batch.staging = m_context->m_device->createBuffer(
        info, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        DxvkMemoryStats::Category::RTXBuffer, "dx11 camera estimator samples");
      batch.completion = new sync::Fence(0);
      batch.requests.reserve(kCameraAnchorMaxSampleMeshes);
    }
    if (batch.staging == nullptr)
      return;

    if (batch.requests.empty()) {
      batch.frame = currentFrame;
      batch.sequence = ++m_cameraAnchorNextSequence;
      batch.sealed = false;
    }
    const VkDeviceSize dstOffset = VkDeviceSize(batch.requests.size()) * kCameraAnchorSampleSlotBytes;
    m_context->EmitCs([cDst = batch.staging, cDstOffset = dstOffset,
                       cSrc = entry.buffer, cBytes = sampleBytes](DxvkContext* ctx) {
      ctx->copyBuffer(cDst, cDstOffset, cSrc, 0, cBytes);
    });

    CameraAnchorSampleRequest request;
    request.meshKey = meshKey;
    request.viewRotationToWorld = entry.capturedViewRotationToWorld;
    request.clipToPosition = entry.capturedClipToPosition;
    request.clipUsesWDepth = entry.capturedClipUsesWDepth;
    request.viewSpaceCamera = viewSpaceCamera;
    request.playerProbe = playerProbe;
    request.capturedToWorld = entry.canonicalCapturedToWorld;
    request.hasCapturedToWorld = entry.hasCanonicalCapturedToWorld;
    request.vertexCount = sampleVertices;
    request.stride = entry.capturedStride;
    batch.requests.push_back(request);
  }

  void D3D11Rtx::SealCameraAnchorSamples() {
    if (m_cameraAnchorWriteIndex == kCameraAnchorBatchCount)
      return;
    auto& batch = m_cameraAnchorBatches[m_cameraAnchorWriteIndex];
    if (!batch.requests.empty()) {
      // Queued after every copy in this batch. Unlike isInUse(), this cannot
      // report ready before the CS thread has recorded the copy commands.
      m_context->EmitCs([cCompletion = batch.completion, cSequence = batch.sequence](DxvkContext* ctx) {
        ctx->signal(cCompletion, cSequence);
      });
      batch.sealed = true;
    }
    m_cameraAnchorWriteIndex = kCameraAnchorBatchCount;
  }

  void D3D11Rtx::ConsumeCameraAnchorSamples() {
    ScopedCpuProfileZoneN("D3D11Rtx::ConsumeCameraAnchorSamples");
    const uint32_t currentFrame = m_context->m_device->getCurrentFrameId();
    if (m_cameraAnchorLastConsumedFrame == currentFrame)
      return;
    m_cameraAnchorLastConsumedFrame = currentFrame;
    SealCameraAnchorSamples();
    // Queue retirement also signals canceled work after device loss. Such a
    // signal releases resources, but does not establish valid captured bytes.
    if (m_context->m_device->getDeviceStatus() != VK_SUCCESS)
      return;

    // Completed batches are solved in capture order. Never mix samples from
    // several source frames in one fit, even when the GPU retires them together.
    for (uint32_t count = 0; count < kCameraAnchorBatchCount; ++count) {
      CameraAnchorSampleBatch* oldest = nullptr;
      for (auto& batch : m_cameraAnchorBatches) {
        if (batch.sealed && (!oldest || batch.sequence < oldest->sequence))
          oldest = &batch;
      }
      if (!oldest || oldest->completion->value() < oldest->sequence)
        break;

      const uint8_t* base = reinterpret_cast<const uint8_t*>(oldest->staging->mapPtr(0));
      bool addedViewSpaceSamples = false;
      bool addedAnchorSamples = false;
      for (size_t index = 0; base && index < oldest->requests.size(); ++index) {
        const auto& request = oldest->requests[index];
        const uint8_t* slot = base + index * kCameraAnchorSampleSlotBytes;
        float viewSamples[kCameraAnchorSampleVertices * 3u] = {};
        Vector3 offsetSum(0.0f, 0.0f, 0.0f);
        uint32_t sampled = 0;
        // Player-body probe: every sampled vertex must lie in a body-sized
        // column around the eye (world-oriented offsets: horizontal radius,
        // feet below, head at eye height).
        bool insideBodyColumn = true;
        const float bodyRadius = RtxOptions::dx11PlayerBodyRadius();
        const float bodyBelow = RtxOptions::dx11PlayerBodyBelowEye();
        const float bodyAbove = RtxOptions::dx11PlayerBodyAboveEye();
        const bool zUp = RtxOptions::zUp();
        for (uint32_t vertex = 0; vertex < request.vertexCount; ++vertex) {
          float clip[4];
          std::memcpy(clip, slot + size_t(vertex) * request.stride, sizeof(clip));
          Vector3 viewPosition;
          if (!unprojectCapturedClip(request.clipToPosition, request.clipUsesWDepth, clip, viewPosition))
            break;
          viewSamples[sampled * 3u + 0u] = viewPosition.x;
          viewSamples[sampled * 3u + 1u] = viewPosition.y;
          viewSamples[sampled * 3u + 2u] = viewPosition.z;
          if (!request.viewSpaceCamera) {
            const Vector4 offset = request.viewRotationToWorld * Vector4(viewPosition, 1.0f);
            offsetSum += Vector3(offset.x, offset.y, offset.z);
            if (request.playerProbe) {
              const float up = zUp ? offset.z : offset.y;
              const float h0 = offset.x;
              const float h1 = zUp ? offset.y : offset.z;
              insideBodyColumn &= (h0 * h0 + h1 * h1) <= bodyRadius * bodyRadius
                               && up >= -bodyBelow && up <= bodyAbove;
            }
          }
          ++sampled;
        }
        if (sampled != request.vertexCount)
          continue;

        // Temporary diagnostic: the world position the renderer gives one
        // fixed static mesh. If the anchoring is right it never moves.
        if (!request.playerProbe && !request.viewSpaceCamera && request.hasCapturedToWorld && sampled > 0) {
          static uint64_t s_trackedKey = 0;
          static uint32_t s_lastTrackLog = 0;
          if (s_trackedKey == 0)
            s_trackedKey = request.meshKey;
          if (request.meshKey == s_trackedKey && currentFrame >= s_lastTrackLog + 30u) {
            s_lastTrackLog = currentFrame;
            const Vector4 world = request.capturedToWorld
              * Vector4(viewSamples[0], viewSamples[1], viewSamples[2], 1.0f);
            Logger::info(str::format("[D3D11Rtx][anchor-check] frame=", currentFrame, " key=0x", std::hex,
              request.meshKey, std::dec, " world=(", world.x, ",", world.y, ",", world.z, ")",
              " exactEye=", m_eyeOffset != SIZE_MAX ? 1 : 0));
          }
        }

        if (request.playerProbe) {
          if (insideBodyColumn) {
            const bool newlyFound = m_playerBodyKeys.find(request.meshKey) == m_playerBodyKeys.end();
            m_playerBodyKeys[request.meshKey] = currentFrame;
            static uint32_t s_playerBodyLogs = 0;
            if (newlyFound && s_playerBodyLogs < 16u) {
              ++s_playerBodyLogs;
              Logger::info(str::format("[D3D11Rtx] Player body around camera -> ThirdPersonPlayerModel: key=0x",
                std::hex, request.meshKey, std::dec));
            }
          } else {
            m_playerBodyKeys.erase(request.meshKey);
          }
          continue;
        }

        if (request.viewSpaceCamera) {
          if (RtxOptions::estimateViewSpaceCameraMotion() && !isKnownEmulatorHostProcess()) {
            m_cameraTrackingState->viewSpace.addMeshSample(request.meshKey, viewSamples, sampled);
            addedViewSpaceSamples = true;
          }
        } else {
          m_cameraTrackingState->worldAnchor.addSample(request.meshKey, offsetSum / float(sampled));
          addedAnchorSamples = true;
        }
      }
      if (addedViewSpaceSamples)
        m_cameraTrackingState->viewSpace.beginFrame(kPcCameraMinSamplePoints, kPcCameraMaxTranslationPerFrame);
      if (addedAnchorSamples)
        m_cameraTrackingState->worldAnchor.endFrame(kCameraAnchorMaxTranslationPerFrame);

      oldest->requests.clear();
      oldest->sealed = false;
    }
  }

  void D3D11Rtx::SubmitInstancedDraw(bool indexed, UINT count, UINT start, INT base,
                                       UINT instanceCount, UINT startInstance) {
    ScopedCpuProfileZoneN("D3D11Rtx::SubmitInstancedDraw");
    if (instanceCount == 0 || count == 0 || GetD3D11EngineProfile().chromiumHelperProcess)
      return;
    if (instanceCount == 1) {
      SubmitDraw(indexed, count, start, base, nullptr, startInstance, 1u, true);
      return;
    }

    // Unity, Unreal, Godot and many proprietary engines perform instancing in
    // the vertex shader (SV_InstanceID, per-instance IA rows, cbuffers or VS
    // SRVs). The old CPU matrix fit handled only one of those layouts and then
    // disabled post-VS capture, so the most important engine geometry was sent
    // to RTX with guessed transforms or collapsed to one point. Replay the
    // original instance range in bounded GPU batches, preserving the game's
    // real SV_InstanceID and per-instance input streams without creating one
    // CPU capture buffer and one BLAS submission per instance.
    bool canCaptureExactInstances = useVertexCapture()
      && m_context->m_device->features().extTransformFeedback.transformFeedback
      && m_context->m_state.vs.shader != nullptr
      && m_context->m_state.gs.shader == nullptr
      && m_context->m_state.hs.shader == nullptr
      && m_context->m_state.ds.shader == nullptr;
    for (const auto& soTarget : m_context->m_state.so.targets)
      canCaptureExactInstances &= soTarget.buffer == nullptr;
    if (canCaptureExactInstances) {
      const D3D11CommonShader* commonVs =
        m_context->m_state.vs.shader->GetCommonShader();
      canCaptureExactInstances = commonVs != nullptr
        && commonVs->HasPositionCaptureCandidate();
    }

    canCaptureExactInstances &=
      m_context->m_state.ia.primitiveTopology == D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;

    if (canCaptureExactInstances) {
      // Preserve one original instanced draw. Splitting and changing
      // StartInstanceLocation resets SV_InstanceID for each batch and advances
      // divisor-based IA streams by the wrong number of elements. A draw that
      // exceeds the exact capture limits falls through to the per-instance
      // layout path below instead of turning the whole frame into raster.
      static constexpr UINT kMaxExactInstancesPerDraw = 4096u;
      const UINT requestedLimit = std::max(1u, RtxOptions::maxInstanceSubmissions());
      if (instanceCount > std::min(requestedLimit, kMaxExactInstancesPerDraw)
        || uint64_t(instanceCount) * count > kMaxPositionCaptureVerticesPerDraw)
        canCaptureExactInstances = false;
    }

    if (canCaptureExactInstances) {

      static uint32_t sExactInstanceLogCount = 0;
      if (sExactInstanceLogCount++ < 12u) {
        Logger::info(str::format(
          "[D3D11Rtx] Exact shader-profile instancing: sourceInstances=",
          instanceCount, " selected=", instanceCount,
          " batches=1",
          " startInstance=", startInstance,
          " indexed=", indexed ? 1 : 0));
      }

      SubmitDraw(indexed, count, start, base, nullptr, startInstance, instanceCount, true);
      return;
    }

    // Find per-instance float4 rows in the input layout that form a world matrix.
    // Engines encode this as 3 or 4 consecutive float4 elements with per-instance step rate,
    // using semantics like INSTANCETRANSFORM, WORLD, I, INST, or TEXCOORD at high indices.
    auto* layout = m_context->m_state.ia.inputLayout.ptr();
    if (!layout) {
      // Vertex-pulled instancing (SV_InstanceID into SRV records): replay
      // every instance exactly through capture.
      SubmitDraw(indexed, count, start, base, nullptr, startInstance, instanceCount, true);
      return;
    }

    const auto& semantics = layout->GetRtxSemantics();

    struct Float4Row {
      uint32_t inputSlot;
      uint32_t byteOffset;
    };

    std::vector<Float4Row> instRows;
    uint32_t instSlot = UINT32_MAX;

    for (const auto& s : semantics) {
      if (!s.perInstance) continue;
      if (s.componentType != DxbcScalarType::Float32 || s.componentCount != 4) continue;

      // Accept any per-instance float4 row Ã¢â‚¬â€ most engines use INSTANCETRANSFORM, WORLD,
      // INSTANCE, I, INST, or repurpose high TEXCOORD registers. Matching on row
      // shape instead of one exact VkFormat catches more real D3D11 layouts.
      if (instSlot == UINT32_MAX)
        instSlot = s.inputSlot;

      if (s.inputSlot != instSlot) continue;
      instRows.push_back({s.inputSlot, s.byteOffset});
    }

    std::sort(instRows.begin(), instRows.end(), [] (const Float4Row& a, const Float4Row& b) {
      return a.byteOffset < b.byteOffset;
    });

    // Input layouts may expose aliases at the same byte offset (for example,
    // multiple semantic names mapped onto one packed instance field). They
    // are one physical float4, not independent matrix rows. Counting aliases
    // made a 32-byte record appear to contain three or four rows and caused us
    // to read overlapping data as a transform.
    instRows.erase(std::unique(instRows.begin(), instRows.end(),
      [](const Float4Row& a, const Float4Row& b) {
        return a.inputSlot == b.inputSlot && a.byteOffset == b.byteOffset;
      }), instRows.end());

    if (instRows.size() < 3) {
      // No instance transform found â€” submit once without instance data.
      // This handles instancing used for non-transform data (colors, etc.)
      static uint32_t sNoInstXformLog = 0;
      if (sNoInstXformLog < 3) {
        ++sNoInstXformLog;
        Logger::info(str::format("[D3D11Rtx] Instanced draw (", instanceCount,
                                 " instances) has no per-instance transform (", instRows.size(),
                                 " float4 rows). Submitting single draw."));
      }
      SubmitDraw(indexed, count, start, base);
      return;
    }

    // Read the instance buffer
    const auto& vb = m_context->m_state.ia.vertexBuffers[instSlot];
    if (vb.buffer == nullptr) {
      SubmitDraw(indexed, count, start, base);
      return;
    }

    DxvkBufferSlice instBufSlice = vb.buffer->GetBufferSlice(vb.offset);
    const uint32_t instStride = vb.stride;
    const size_t instBufLen = instBufSlice.length();
    if (instStride == 0) {
      SubmitDraw(indexed, count, start, base);
      return;
    }

    // DX11_V285_INSTANCE_ROW_STRIDE_CLAMP: input layouts routinely declare
    // more per-instance float4 elements than the BOUND stream's stride holds
    // (layouts shared across mesh types; observed in Skyrim: 4 declared rows,
    // bound stride 32 = room for only 2). A row whose bytes extend past the
    // stride reads the NEXT instance's data - finite but garbage matrix rows
    // for every instance, i.e. exploded/misplaced instanced geometry and a
    // bloated TLAS. Keep only rows that fit inside one instance record.
    {
      size_t kept = 0;
      for (size_t i = 0; i < instRows.size(); ++i) {
        if (uint64_t(instRows[i].byteOffset) + 16u <= uint64_t(instStride))
          instRows[kept++] = instRows[i];
      }
      if (kept != instRows.size()) {
        static uint32_t sRowClampLog = 0;
        if (sRowClampLog < 3) {
          ++sRowClampLog;
          Logger::info(str::format("[D3D11Rtx] Instanced draw: clamped ", instRows.size() - kept,
                                   " declared per-instance float4 row(s) that exceed the bound stride (",
                                   instStride, " bytes); ", kept, " row(s) remain."));
        }
        instRows.resize(kept);
      }

      // A matrix row sequence is contiguous. Select one maximal run of up to
      // four distinct float4s; unrelated instance colors/parameters elsewhere
      // in the same stream must not be spliced into a transform.
      std::vector<Float4Row> bestRun;
      for (size_t begin = 0; begin < instRows.size(); ++begin) {
        std::vector<Float4Row> run;
        run.push_back(instRows[begin]);
        for (size_t i = begin + 1; i < instRows.size() && run.size() < 4; ++i) {
          if (instRows[i].byteOffset != run.back().byteOffset + 16u)
            break;
          run.push_back(instRows[i]);
        }
        if (run.size() > bestRun.size())
          bestRun = std::move(run);
        if (bestRun.size() == 4)
          break;
      }
      instRows = std::move(bestRun);

      if (instRows.size() < 3) {
        // Not enough in-stride rows for an affine transform - this stream is
        // per-instance data (colors/params), not matrices. One placement.
        SubmitDraw(indexed, count, start, base);
        return;
      }
    }

    // Cap to avoid excessive submission â€” configurable via rtx.maxInstanceSubmissions
    const UINT maxInstances = std::min(instanceCount, RtxOptions::maxInstanceSubmissions());

    static uint32_t sInstLog = 0;
    if (sInstLog < 3) {
      ++sInstLog;
      Logger::info(str::format("[D3D11Rtx] Instanced draw: ", instanceCount,
                               " instances, ", instRows.size(), " float4 rows in slot ",
                               instSlot, ", stride=", instStride));
    }

    auto sampleInstanceIndex = [&](UINT sampleIndex) {
      if (maxInstances <= 1 || instanceCount <= maxInstances)
        return startInstance + sampleIndex;

      return startInstance + UINT((uint64_t(sampleIndex) * uint64_t(instanceCount - 1))
        / uint64_t(maxInstances - 1));
    };

    // Read the per-instance world matrix at a given instance index.
    auto readInstMatrix = [&](UINT instIdx, Matrix4& out) -> bool {
      const size_t instOffset = static_cast<size_t>(instIdx) * instStride;
      float rows[4][4] = {};
      for (size_t r = 0; r < std::min<size_t>(instRows.size(), 4); ++r) {
        const size_t rowOff = instOffset + instRows[r].byteOffset;
        if (rowOff + 16 > instBufLen) return false;
        const void* ptr = instBufSlice.mapPtr(rowOff);
        if (!ptr) return false;
        std::memcpy(rows[r], ptr, 16);
        for (int c = 0; c < 4; ++c)
          if (!std::isfinite(rows[r][c])) return false;
      }
      // If only 3 rows, the 4th row is (0,0,0,1) - affine transform.
      if (instRows.size() == 3) {
        rows[3][0] = 0.f; rows[3][1] = 0.f; rows[3][2] = 0.f; rows[3][3] = 1.f;
      }
      const Matrix4 storedRows(
        Vector4(rows[0][0], rows[0][1], rows[0][2], rows[0][3]),
        Vector4(rows[1][0], rows[1][1], rows[1][2], rows[1][3]),
        Vector4(rows[2][0], rows[2][1], rows[2][2], rows[2][3]),
        Vector4(rows[3][0], rows[3][1], rows[3][2], rows[3][3]));

      auto affineInstanceScore = [](const Matrix4& candidate) {
        for (uint32_t column = 0; column < 4; ++column) {
          for (uint32_t row = 0; row < 4; ++row) {
            if (!std::isfinite(candidate[column][row]))
              return -1.0e30f;
          }
        }
        if (std::abs(candidate[0][3]) > 0.01f
         || std::abs(candidate[1][3]) > 0.01f
         || std::abs(candidate[2][3]) > 0.01f
         || std::abs(candidate[3][3] - 1.0f) > 0.01f)
          return -1.0e30f;

        float score = 0.0f;
        Vector3 axes[3];
        for (uint32_t column = 0; column < 3; ++column) {
          const float lengthSq =
              candidate[0][column] * candidate[0][column]
            + candidate[1][column] * candidate[1][column]
            + candidate[2][column] * candidate[2][column];
          if (!std::isfinite(lengthSq) || lengthSq < 1.0e-8f || lengthSq > 1.0e8f)
            return -1.0e30f;
          const float invLength = 1.0f / std::sqrt(lengthSq);
          axes[column] = Vector3(
            candidate[0][column] * invLength,
            candidate[1][column] * invLength,
            candidate[2][column] * invLength);
        }
        const float shear = std::abs(dot(axes[0], axes[1]))
                          + std::abs(dot(axes[0], axes[2]))
                          + std::abs(dot(axes[1], axes[2]));
        if (shear > 1.5f)
          return -1.0e30f;
        score -= shear;
        return score;
      };

      // D3D instance transforms are conventionally supplied as three/four
      // dot-product rows. Matrix4 stores columns, so the transposed candidate
      // is preferred when both layouts are structurally affine. Four-column
      // input layouts are also accepted through the stored candidate.
      const Matrix4 rowVectorTransform = transpose(storedRows);
      const float rowVectorScore = affineInstanceScore(rowVectorTransform) + 0.01f;
      const float columnVectorScore = affineInstanceScore(storedRows);
      if (rowVectorScore <= -1.0e20f && columnVectorScore <= -1.0e20f)
        return false;
      out = rowVectorScore >= columnVectorScore ? rowVectorTransform : storedRows;
      return true;
    };

    // DX11_V276_NO_STACKED_INSTANCE_COPIES: submitting the whole mesh once per
    // instance record with a (near-)identical per-instance transform stacks N
    // coincident copies of the geometry. Self-intersecting opaque geometry
    // shades PURE BLACK in the path tracer (near-coplanar duplicate surfaces
    // fight and the integrator resolves them to zero) - the "black roads /
    // debris ground / trash-blanket" artifact, which is a GEOMETRY duplication
    // bug, NOT a texture bug. This happens when the per-instance step data is
    // not really distinct spatial placements (misread stride/offset, or
    // instancing used for non-transform data that we mis-fit to a matrix), so
    // every "instance" collapses to one placement. Detect that by sampling the
    // instance transforms up front: if they are all near-identical, this is
    // degenerate instancing - submit the mesh ONE time, not N overlapping
    // copies. Genuine distinct instancing (transforms differ) falls through to
    // the normal per-instance submission below.
    {
      Matrix4 firstMatrix;
      bool haveFirst = false;
      bool anyDistinct = false;
      uint32_t sampledCount = 0;
      uint32_t readFailures = 0;
      float translationMin[3] = {};
      float translationMax[3] = {};
      bool haveBounds = false;
      const UINT sampleN = std::min<UINT>(maxInstances, 16u);
      // Note: the whole sample range is walked even once distinctness is known,
      // because the placement-plausibility test below needs the full spread.
      for (UINT i = 0; i < sampleN; ++i) {
        Matrix4 m;
        if (!readInstMatrix(sampleInstanceIndex(i), m)) {
          ++readFailures;
          continue;
        }
        ++sampledCount;

        // Matrix4 stores columns, and readInstMatrix returns the column-vector
        // form, so the translation is column 3.
        const float translation[3] = { m[3][0], m[3][1], m[3][2] };
        for (int axis = 0; axis < 3; ++axis) {
          if (!haveBounds) {
            translationMin[axis] = translationMax[axis] = translation[axis];
          } else {
            translationMin[axis] = std::min(translationMin[axis], translation[axis]);
            translationMax[axis] = std::max(translationMax[axis], translation[axis]);
          }
        }
        haveBounds = true;

        if (!haveFirst) {
          firstMatrix = m;
          haveFirst = true;
          continue;
        }
        float deviation = 0.0f;
        for (int r = 0; r < 4; ++r)
          for (int c = 0; c < 4; ++c)
            deviation += std::abs(m[r][c] - firstMatrix[r][c]);
        if (deviation > 1.0e-3f)
          anyDistinct = true;
      }

      if (haveFirst && !anyDistinct && sampledCount >= 2) {
        static uint32_t sDegenerateInstLog = 0;
        if (sDegenerateInstLog < 8) {
          ++sDegenerateInstLog;
          Logger::info(str::format(
            "[D3D11Rtx] Degenerate instancing: ", instanceCount,
            " instances share one placement - submitting the mesh ONCE to avoid stacked "
            "coincident copies (self-intersection renders black)."));
        }
        SubmitDraw(indexed, count, start, base, &firstMatrix);
        return;
      }

      // DX11_V299_INSTANCE_STREAM_PLAUSIBILITY: distinctness alone does not
      // prove the fitted float4 run is a transform stream. Per-instance colour
      // and parameter data varies per instance too, so it clears the degenerate
      // test above and then places a mesh copy at every bogus "transform" -
      // geometry scattered through the scene and stacked on the camera, which
      // reads as a solid obstruction blocking the view.
      //
      // This is reachable whenever the row fit is not provably a matrix, most
      // easily when the bound stride truncates a declared 4-row layout down to
      // exactly 3 rows (Skyrim: 4 declared, stride 32) and the surviving run is
      // really colour/params. Two signatures separate that from real placements:
      //
      //   - most sampled instances fail the affine test, so the few that pass
      //     did so by chance, or
      //   - every sampled placement sits inside a sub-unit box at the origin.
      //     Normalised colour/parameter data lives in [0,1], so a transform
      //     fitted to it collapses every copy onto the origin. Genuine
      //     instancing spreads placements across world space, and world units
      //     here are large (this scene's far plane is ~20000).
      if (haveFirst && anyDistinct && sampledCount >= 2) {
        const bool mostlyUnreadable = readFailures > sampledCount;

        bool withinUnitBoxAtOrigin = haveBounds;
        float widestSpread = 0.0f;
        for (int axis = 0; axis < 3; ++axis) {
          widestSpread = std::max(widestSpread, translationMax[axis] - translationMin[axis]);
          if (std::abs(translationMin[axis]) > 1.0f || std::abs(translationMax[axis]) > 1.0f)
            withinUnitBoxAtOrigin = false;
        }
        const bool implausiblePlacements = withinUnitBoxAtOrigin && widestSpread < 1.0f;

        if (mostlyUnreadable || implausiblePlacements) {
          static uint32_t sImplausibleInstLog = 0;
          if (sImplausibleInstLog < 8) {
            ++sImplausibleInstLog;
            Logger::info(str::format(
              "[D3D11Rtx] Instance stream does not describe placements (",
              instRows.size(), " float4 row(s), stride=", instStride,
              ", sampled=", sampledCount, ", affineFailures=", readFailures,
              ", widestSpread=", widestSpread,
              ") - treating it as per-instance data and submitting the mesh ONCE "
              "instead of scattering copies."));
          }
          SubmitDraw(indexed, count, start, base);
          return;
        }
      }
    }

    for (UINT i = 0; i < maxInstances; ++i) {
      Matrix4 instMatrix;
      if (!readInstMatrix(sampleInstanceIndex(i), instMatrix))
        continue;
      SubmitDraw(indexed, count, start, base, &instMatrix);
    }
  }

  // Read a row-major float4x4 from a mapped cbuffer.  Returns identity on bounds violation
  // or if any element is NaN/Inf (corrupt GPU memory, emulator artifacts, etc.).
  static Matrix4 readCbMatrix(const uint8_t* ptr, size_t offset, size_t bufSize) {
    if (offset + 64 > bufSize)
      return Matrix4();
    float raw[4][4];
    std::memcpy(raw, ptr + offset, 64);
    for (int r = 0; r < 4; ++r)
      for (int c = 0; c < 4; ++c)
        if (!std::isfinite(raw[r][c]))
          return Matrix4();
    return Matrix4(
      Vector4(raw[0][0], raw[0][1], raw[0][2], raw[0][3]),
      Vector4(raw[1][0], raw[1][1], raw[1][2], raw[1][3]),
      Vector4(raw[2][0], raw[2][1], raw[2][2], raw[2][3]),
      Vector4(raw[3][0], raw[3][1], raw[3][2], raw[3][3]));
  }

  struct SkinningConstantBufferSnapshot {
    uint32_t slot = UINT32_MAX;
    std::vector<uint8_t> data;
  };

  static float decodeFloat16(uint16_t value) {
    const uint32_t sign = (value & 0x8000u) << 16;
    uint32_t exponent = (value >> 10) & 0x1fu;
    uint32_t mantissa = value & 0x03ffu;

    uint32_t decoded = 0;
    if (exponent == 0) {
      if (mantissa == 0) {
        decoded = sign;
      } else {
        exponent = 127 - 15 + 1;
        while ((mantissa & 0x0400u) == 0) {
          mantissa <<= 1;
          --exponent;
        }
        mantissa &= 0x03ffu;
        decoded = sign | (exponent << 23) | (mantissa << 13);
      }
    } else if (exponent == 0x1fu) {
      decoded = sign | 0x7f800000u | (mantissa << 13);
    } else {
      decoded = sign | ((exponent + (127 - 15)) << 23) | (mantissa << 13);
    }

    float result = 0.0f;
    std::memcpy(&result, &decoded, sizeof(result));
    return result;
  }

  static bool decodeBlendWeights(const uint8_t* src, VkFormat format, float outWeights[4], uint32_t& outComponentCount) {
    outComponentCount = 0;
    std::fill(outWeights, outWeights + 4, 0.0f);

    switch (format) {
      case VK_FORMAT_R32_SFLOAT: {
        const float* values = reinterpret_cast<const float*>(src);
        outWeights[0] = values[0];
        outComponentCount = 1;
      } break;
      case VK_FORMAT_R32G32_SFLOAT: {
        const float* values = reinterpret_cast<const float*>(src);
        outWeights[0] = values[0];
        outWeights[1] = values[1];
        outComponentCount = 2;
      } break;
      case VK_FORMAT_R32G32B32_SFLOAT: {
        const float* values = reinterpret_cast<const float*>(src);
        outWeights[0] = values[0];
        outWeights[1] = values[1];
        outWeights[2] = values[2];
        outComponentCount = 3;
      } break;
      case VK_FORMAT_R32G32B32A32_SFLOAT: {
        const float* values = reinterpret_cast<const float*>(src);
        outWeights[0] = values[0];
        outWeights[1] = values[1];
        outWeights[2] = values[2];
        outWeights[3] = values[3];
        outComponentCount = 4;
      } break;
      case VK_FORMAT_R16_SFLOAT: {
        const uint16_t* values = reinterpret_cast<const uint16_t*>(src);
        outWeights[0] = decodeFloat16(values[0]);
        outComponentCount = 1;
      } break;
      case VK_FORMAT_R16G16_SFLOAT: {
        const uint16_t* values = reinterpret_cast<const uint16_t*>(src);
        outWeights[0] = decodeFloat16(values[0]);
        outWeights[1] = decodeFloat16(values[1]);
        outComponentCount = 2;
      } break;
      case VK_FORMAT_R16G16B16A16_SFLOAT: {
        const uint16_t* values = reinterpret_cast<const uint16_t*>(src);
        outWeights[0] = decodeFloat16(values[0]);
        outWeights[1] = decodeFloat16(values[1]);
        outWeights[2] = decodeFloat16(values[2]);
        outWeights[3] = decodeFloat16(values[3]);
        outComponentCount = 4;
      } break;
      case VK_FORMAT_R8_UNORM: {
        outWeights[0] = src[0] / 255.0f;
        outComponentCount = 1;
      } break;
      case VK_FORMAT_R8G8_UNORM: {
        outWeights[0] = src[0] / 255.0f;
        outWeights[1] = src[1] / 255.0f;
        outComponentCount = 2;
      } break;
      case VK_FORMAT_R8G8B8A8_UNORM: {
        outWeights[0] = src[0] / 255.0f;
        outWeights[1] = src[1] / 255.0f;
        outWeights[2] = src[2] / 255.0f;
        outWeights[3] = src[3] / 255.0f;
        outComponentCount = 4;
      } break;
      case VK_FORMAT_R16_UNORM: {
        const uint16_t* values = reinterpret_cast<const uint16_t*>(src);
        outWeights[0] = values[0] / 65535.0f;
        outComponentCount = 1;
      } break;
      case VK_FORMAT_R16G16_UNORM: {
        const uint16_t* values = reinterpret_cast<const uint16_t*>(src);
        outWeights[0] = values[0] / 65535.0f;
        outWeights[1] = values[1] / 65535.0f;
        outComponentCount = 2;
      } break;
      case VK_FORMAT_R16G16B16A16_UNORM: {
        const uint16_t* values = reinterpret_cast<const uint16_t*>(src);
        outWeights[0] = values[0] / 65535.0f;
        outWeights[1] = values[1] / 65535.0f;
        outWeights[2] = values[2] / 65535.0f;
        outWeights[3] = values[3] / 65535.0f;
        outComponentCount = 4;
      } break;
      default:
        return false;
    }

    for (uint32_t i = 0; i < outComponentCount; ++i) {
      if (!std::isfinite(outWeights[i]))
        return false;
      outWeights[i] = std::clamp(outWeights[i], 0.0f, 1.0f);
    }

    return outComponentCount > 0;
  }

  static bool decodeBlendIndices(const uint8_t* src, VkFormat format, uint32_t outIndices[4], uint32_t& outComponentCount) {
    outComponentCount = 0;
    std::fill(outIndices, outIndices + 4, 0u);

    switch (format) {
      case VK_FORMAT_R8_UINT:
      case VK_FORMAT_R8_USCALED:
        outIndices[0] = src[0];
        outComponentCount = 1;
        break;
      case VK_FORMAT_R8G8_UINT:
      case VK_FORMAT_R8G8_USCALED:
        outIndices[0] = src[0];
        outIndices[1] = src[1];
        outComponentCount = 2;
        break;
      case VK_FORMAT_R8G8B8A8_UINT:
      case VK_FORMAT_R8G8B8A8_USCALED:
        outIndices[0] = src[0];
        outIndices[1] = src[1];
        outIndices[2] = src[2];
        outIndices[3] = src[3];
        outComponentCount = 4;
        break;
      case VK_FORMAT_R16_UINT: {
        const uint16_t* values = reinterpret_cast<const uint16_t*>(src);
        outIndices[0] = values[0];
        outComponentCount = 1;
      } break;
      case VK_FORMAT_R16G16_UINT: {
        const uint16_t* values = reinterpret_cast<const uint16_t*>(src);
        outIndices[0] = values[0];
        outIndices[1] = values[1];
        outComponentCount = 2;
      } break;
      case VK_FORMAT_R16G16B16A16_UINT: {
        const uint16_t* values = reinterpret_cast<const uint16_t*>(src);
        outIndices[0] = values[0];
        outIndices[1] = values[1];
        outIndices[2] = values[2];
        outIndices[3] = values[3];
        outComponentCount = 4;
      } break;
      case VK_FORMAT_R32_UINT: {
        const uint32_t* values = reinterpret_cast<const uint32_t*>(src);
        outIndices[0] = values[0];
        outComponentCount = 1;
      } break;
      case VK_FORMAT_R32G32_UINT: {
        const uint32_t* values = reinterpret_cast<const uint32_t*>(src);
        outIndices[0] = values[0];
        outIndices[1] = values[1];
        outComponentCount = 2;
      } break;
      case VK_FORMAT_R32G32B32_UINT: {
        const uint32_t* values = reinterpret_cast<const uint32_t*>(src);
        outIndices[0] = values[0];
        outIndices[1] = values[1];
        outIndices[2] = values[2];
        outComponentCount = 3;
      } break;
      case VK_FORMAT_R32G32B32A32_UINT: {
        const uint32_t* values = reinterpret_cast<const uint32_t*>(src);
        outIndices[0] = values[0];
        outIndices[1] = values[1];
        outIndices[2] = values[2];
        outIndices[3] = values[3];
        outComponentCount = 4;
      } break;
      default:
        return false;
    }

    return outComponentCount > 0;
  }

  static VkFormat normalizedBlendWeightFormat(uint32_t explicitWeightCount) {
    switch (explicitWeightCount) {
      case 1: return VK_FORMAT_R32_SFLOAT;
      case 2: return VK_FORMAT_R32G32_SFLOAT;
      case 3: return VK_FORMAT_R32G32B32_SFLOAT;
      default: return VK_FORMAT_UNDEFINED;
    }
  }

  static bool isSkinningMatrix(const Matrix4& m) {
    if (std::abs(m[3][3] - 1.0f) > 0.05f)
      return false;
    if (std::abs(m[0][3]) > 0.05f || std::abs(m[1][3]) > 0.05f || std::abs(m[2][3]) > 0.05f)
      return false;

    for (int row = 0; row < 4; ++row) {
      for (int col = 0; col < 4; ++col) {
        if (!std::isfinite(m[row][col]))
          return false;
      }
    }

    return true;
  }

  // DX11_V285_HELPER_BUFFER_POOL: see d3d11_rtx.h. Pool-backed replacement
  // for the old per-draw createBuffer. Every helper keeps its own OBJECT for
  // the duration of its use (per-draw freshness preserved - no renaming, no
  // V250-class staleness); only provably-released objects are reused.
  // DX11_V312_PHASE_TIMERS: accumulate wall time into a per-frame sink.
  //
  // The per-draw timer already proves ONE draw absorbs the whole frame (96ms of
  // a 96ms frame) and that the cost does not scale with that draw's geometry -
  // 6 indices and 186 indices both cost ~96ms. So it is a fixed block, not work.
  // The RTX passes themselves complete in ~2ms per the frame log, so the time is
  // not GPU render cost either. These timers say WHICH call is blocking instead
  // of inferring it.
  namespace {
    struct ScopedPhaseTimer {
      uint64_t& sink;
      std::chrono::high_resolution_clock::time_point start;

      explicit ScopedPhaseTimer(uint64_t& s)
        : sink(s), start(std::chrono::high_resolution_clock::now()) { }

      ~ScopedPhaseTimer() {
        sink += static_cast<uint64_t>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::high_resolution_clock::now() - start).count());
      }
    };
  }

  Rc<DxvkBuffer> D3D11Rtx::AcquireHostVisibleHelperBuffer(VkDeviceSize size, const char* name) {
    ScopedPhaseTimer phaseTimer(m_framePhaseHelperNs);

    // Best fit from the free list: smallest capacity that holds the request,
    // but never a grossly oversized one (waste bound).
    const VkDeviceSize maxAcceptable = std::max<VkDeviceSize>(size * 4u, 8192u);
    size_t best = SIZE_MAX;
    for (size_t i = 0; i < m_helperFree.size(); ++i) {
      const VkDeviceSize cap = m_helperFree[i].capacity;
      if (cap >= size && cap <= maxAcceptable
       && (best == SIZE_MAX || cap < m_helperFree[best].capacity))
        best = i;
    }
    if (best != SIZE_MAX) {
      HelperPoolItem item = m_helperFree[best];
      m_helperFree[best] = m_helperFree.back();
      m_helperFree.pop_back();
      m_helperRetired.push_back(item);
      return item.buffer;
    }

    // Power-of-two capacities so per-frame size jitter reuses pool entries.
    VkDeviceSize capacity = 4096u;
    while (capacity < size)
      capacity <<= 1;

    // Leave deterministic residency headroom for BLAS scratch, swapchain
    // recreation, and the path-tracing targets. 128 MiB is enough to recycle
    // the common dynamic streams without parking the previous 384 MiB cap.
    static constexpr VkDeviceSize kMaxPoolBytes = 128ull << 20;

    // DX11_V298_BOUNDED_HELPER_OVERFLOW: when the pool is full but holds idle
    // free-list entries of the wrong size, retire those (the pool owns their
    // only reference) so the pool re-adapts to the workload's current size mix
    // instead of overflowing into unpooled allocations.
    while (m_helperPoolBytes + capacity > kMaxPoolBytes && !m_helperFree.empty()) {
      m_helperPoolBytes -= m_helperFree.back().capacity;
      m_helperFree.pop_back();
    }

    const bool pooled = m_helperPoolBytes + capacity <= kMaxPoolBytes;
    if (!pooled) {
      // The old behavior allocated UNPOOLED past the cap with no bound at
      // all. On a slow world-load frame (Skyrim SE: 600+ snapshot draws per
      // frame at <1 fps) those unpooled buffers accumulated gigabytes in the
      // RTXBuffer census and ended in VK_ERROR_DEVICE_LOST. Allow a bounded
      // per-frame overflow, then fail the acquisition - every caller handles
      // a null buffer by degrading that one draw instead of leaking.
      static constexpr VkDeviceSize kMaxUnpooledBytesPerFrame = 64ull << 20;
      const uint32_t currentFrame = m_context->m_device->getCurrentFrameId();
      if (m_helperUnpooledFrame != currentFrame) {
        m_helperUnpooledFrame = currentFrame;
        m_helperUnpooledBytesThisFrame = 0;
      }
      if (m_helperUnpooledBytesThisFrame + capacity > kMaxUnpooledBytesPerFrame) {
        static bool s_overflowLogged = false;
        if (!s_overflowLogged) {
          s_overflowLogged = true;
          Logger::warn(str::format(
            "[D3D11Rtx] helper-buffer overflow budget exhausted this frame (",
            (kMaxUnpooledBytesPerFrame >> 20),
            " MiB past the pool); degrading further captures this frame instead of growing VRAM unbounded."));
        }
        return nullptr;
      }
      m_helperUnpooledBytesThisFrame += capacity;
    }

    DxvkBufferCreateInfo info;
    info.size = capacity;
    info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    info.stages = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
    info.access = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT;

    Rc<DxvkBuffer> buffer = m_context->m_device->createBuffer(
      info,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
      DxvkMemoryStats::Category::RTXBuffer,
      name);

    if (buffer != nullptr && pooled) {
      m_helperPoolBytes += capacity;
      m_helperRetired.push_back({ buffer, capacity });
    }
    return buffer;
  }

  const D3D11Rtx::CameraSeed* D3D11Rtx::ResolveCameraSeed() {
    if (m_abDisableEngineKnowledge || m_context->m_state.vs.shader == nullptr)
      return nullptr;
    const D3D11CommonShader* vs = m_context->m_state.vs.shader->GetCommonShader();
    // Keyed by bytecode: a destroyed shader's address can be reused.
    auto cached = m_cameraSeedCache.find(vs->GetBytecodeHash());
    if (cached != m_cameraSeedCache.end())
      return cached->second.valid ? &cached->second : nullptr;

    ScopedCpuProfileZoneN("D3D11Rtx::ResolveCameraSeed");
    CameraSeed seed;
    for (auto& o : seed.offsets)
      o = -1;
    auto has = [&](D3D11CameraField f) { return seed.offsets[size_t(f)] >= 0; };

    // 1. Reflection names (engines that keep RDEF: CRYENGINE, Frostbite,
    //    Dunia, Disrupt, Katana, Fox, Phyre, dev builds of Unity/UE, ...).
    const DxbcRdef* rdef = vs->GetReflection();
    if (rdef != nullptr && rdef->isValid()) {
      size_t ruleCount = 0;
      const D3D11CameraNameRule* rules = GetCameraNameRules(ruleCount);
      for (const auto& cb : rdef->constantBuffers()) {
        uint32_t slot = UINT32_MAX;
        for (const auto& binding : rdef->resourceBindings())
          if (binding.kind == DxbcResourceKind::CBuffer && binding.name == cb.name)
            slot = binding.bindPoint;
        if (slot == UINT32_MAX)
          continue;
        for (const auto& var : cb.variables) {
          for (size_t r = 0; r < ruleCount; ++r) {
            if (var.name != rules[r].name)
              continue;
            const size_t f = size_t(rules[r].field);
            if (seed.offsets[f] >= 0)
              break;
            const bool isEye = rules[r].field == D3D11CameraField::Eye
                            || rules[r].field == D3D11CameraField::NegEye
                            || rules[r].field == D3D11CameraField::EyeTile
                            || rules[r].field == D3D11CameraField::EyeLow;
            if (isEye) {
              if (seed.eyeSlot == UINT32_MAX || seed.eyeSlot == slot) {
                seed.eyeSlot = slot;
                seed.offsets[f] = int32_t(var.offset);
              }
            } else if (seed.slot == UINT32_MAX || seed.slot == slot) {
              seed.slot = slot;
              seed.offsets[f] = int32_t(var.offset);
            }
            break;
          }
        }
      }
      if (seed.slot != UINT32_MAX)
        seed.source = "rdef names";
    }

    // 2. Documented register layout of a reflection-stripped engine.
    if (seed.slot == UINT32_MAX) {
      const D3D11EngineFamily family = GetD3D11EngineProfile().family();
      size_t layoutCount = 0;
      const D3D11CameraRegisterLayout* layouts = GetCameraRegisterLayouts(layoutCount);
      for (size_t i = 0; i < layoutCount; ++i) {
        if (layouts[i].family != family)
          continue;
        seed.slot = uint32_t(layouts[i].slot);
        seed.eyeSlot = layouts[i].eyeSlot >= 0 ? uint32_t(layouts[i].eyeSlot) : seed.slot;
        for (size_t f = 0; f < size_t(D3D11CameraField::Count); ++f)
          seed.offsets[f] = layouts[i].offsets[f];
        seed.source = layouts[i].source;
        break;
      }
    }
    if (seed.eyeSlot == UINT32_MAX)
      seed.eyeSlot = seed.slot;

    seed.valid = seed.slot != UINT32_MAX
      && (has(D3D11CameraField::Proj) || has(D3D11CameraField::ViewProj)
       || has(D3D11CameraField::RelViewProj));
    if (seed.valid) {
      static uint32_t s_seedLogs = 0;
      if (s_seedLogs++ < 16u) {
        std::string fields;
        static const char* kNames[] = { "View", "Proj", "ViewProj", "InvView", "InvViewProj",
                                        "Eye", "NegEye", "RelView", "RelViewProj", "PrevViewProj",
                                        "EyeTile", "EyeLow" };
        static_assert(std::size(kNames) == size_t(D3D11CameraField::Count), "camera field names");
        for (size_t f = 0; f < size_t(D3D11CameraField::Count); ++f)
          if (seed.offsets[f] >= 0)
            fields += str::format(" ", kNames[f], "@", seed.offsets[f]);
        Logger::info(str::format("[D3D11Rtx][camera-seed] vs=", vs->GetName(), " source=", seed.source,
          " cb", seed.slot, " eyeCb", seed.eyeSlot, fields));
      }
    }
    CameraSeed& stored = m_cameraSeedCache[vs->GetBytecodeHash()];
    stored = seed;
    return stored.valid ? &stored : nullptr;
  }

  bool D3D11Rtx::ComputeDrawPassKey(bool indexed, UINT count, UINT start, INT base,
                                    UINT firstInstance,
                                    const RasterBuffer& positionBuffer,
                                    const RasterBuffer& indexBuffer, uint64_t positionIdentity,
                                    uint64_t& key, uint64_t& meshKey) const {
    ScopedCpuProfileZoneN("D3D11Rtx::ComputeDrawPassKey");
    if (m_context->m_state.vs.shader == nullptr || !positionBuffer.defined())
      return false;
    const D3D11CommonShader* vs = m_context->m_state.vs.shader->GetCommonShader();

    uint64_t h = 0x50415353u;
    auto mix = [&h](uint64_t v) { h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2); };
    // Geometry: the exact buffer ranges this draw reads. Physical slices are
    // the right identity here - within one frame a re-draw reads the same ones.
    // Vertex-pulled draws have only a per-draw placeholder stream; their
    // identity (VS, range, pulled SRVs) stands in for it.
    if (positionIdentity != 0) {
      mix(positionIdentity);
    } else {
      mix(uint64_t(reinterpret_cast<uintptr_t>(positionBuffer.buffer().ptr())));
      mix(positionBuffer.offset());
      mix(positionBuffer.stride());
    }
    mix(indexed ? 1u : 0u);
    mix(count);
    mix(start);
    mix(uint64_t(int64_t(base)));
    mix(uint64_t(m_context->m_state.ia.primitiveTopology));
    if (indexed && indexBuffer.defined()) {
      mix(uint64_t(reinterpret_cast<uintptr_t>(indexBuffer.buffer().ptr())));
      mix(indexBuffer.offset());
    }
    // Camera-independent identity of the mesh range, stable across frames.
    meshKey = h;

    // Which object this is: UE GPUScene, Unity DOTS and other instance-stream
    // engines draw the same mesh range many times, distinguished only by
    // StartInstanceLocation or a per-instance vertex stream.
    mix(firstInstance);
    for (uint32_t slot = 0; slot < D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT; ++slot) {
      const auto& vb = m_context->m_state.ia.vertexBuffers[slot];
      if (vb.buffer == nullptr)
        continue;
      mix(slot);
      mix(uint64_t(reinterpret_cast<uintptr_t>(vb.buffer.ptr())));
      mix(vb.offset);
    }

    // Placement: only the constants that feed SV_Position, so per-light or
    // per-pass constants (light matrices, pass flags) do not split a re-draw
    // of the same mesh from its first pass.
    auto hashRegs = [&](uint32_t slot, uint32_t firstReg, uint32_t regCount) {
      if (slot >= D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT)
        return false;
      const auto& cb = m_context->m_state.vs.constantBuffers[slot];
      if (cb.buffer == nullptr)
        return false;
      const auto* ptr = reinterpret_cast<const uint8_t*>(cb.buffer->GetMappedSlice().mapPtr);
      const size_t size = cb.buffer->Desc()->ByteWidth;
      const size_t begin = size_t(cb.constantOffset) * 16u + size_t(firstReg) * 16u;
      const size_t bytes = size_t(regCount) * 16u;
      if (ptr == nullptr || begin + bytes > size)
        return false;
      h = XXH3_64bits_withSeed(ptr + begin, bytes, h);
      return true;
    };

    const D3D11CameraRelativeWorldBinding& world = vs->GetCameraRelativeWorldBinding();
    if (world.valid) {
      return hashRegs(world.worldSlot, world.worldRegister, 3u)
          && hashRegs(world.cameraSlot, world.eyeRegister, 1u)
          && hashRegs(world.viewProjSlot, world.viewProjRegister, 4u)
          && (key = h, true);
    }
    if (const D3D11PositionTransformBinding* binding = vs->GetPositionTransformBinding()) {
      for (uint32_t m = 0; m < binding->matrixCount; ++m) {
        const auto& mb = binding->matrices[m];
        for (uint32_t row = 0; row < 4u; ++row) {
          const uint32_t reg = mb.constantRegisters[row];
          if (reg != UINT32_MAX && !hashRegs(mb.constantBufferSlot, reg, 1u))
            return false;
        }
      }
      key = h;
      return true;
    }
    // No proven transform: hash every constant the shader reads. Conservative -
    // a per-light constant then keeps the passes apart, never merges two meshes.
    const D3D11ConstantBufferDependencyProfile& deps = vs->GetConstantBufferDependencyProfile();
    if (!deps.complete)
      return false;
    // Vertex pulling / per-instance records in SRVs (FO4 precombines read
    // t5..t8) place geometry outside the constants, so the views are identity.
    const auto& vsViews = m_context->m_state.vs.shaderResources.views;
    for (uint32_t slot = 0; slot < vsViews.size(); ++slot) {
      if (vsViews[slot] != nullptr) {
        mix(slot);
        mix(uint64_t(reinterpret_cast<uintptr_t>(vsViews[slot].ptr())));
      }
    }
    for (const auto& dep : deps.dependencies) {
      if (dep.wholeBuffer) {
        if (dep.slot >= D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT)
          return false;
        const auto& cb = m_context->m_state.vs.constantBuffers[dep.slot];
        if (cb.buffer == nullptr)
          return false;
        const size_t size = cb.buffer->Desc()->ByteWidth;
        const size_t base16 = size_t(cb.constantOffset) * 16u;
        const size_t end = cb.constantCount > 0
          ? std::min(base16 + size_t(cb.constantCount) * 16u, size) : size;
        if (end <= base16 || !hashRegs(dep.slot, 0u, uint32_t(std::min<size_t>((end - base16) / 16u, 4096u))))
          return false;
      } else if (!hashRegs(dep.slot, dep.constantRegister, 1u)) {
        return false;
      }
    }
    key = h;
    return true;
  }

  bool D3D11Rtx::RebaseIndexedVertexRange(DrawCallState& dcs, uint32_t indexCount,
                                          const D3D11Buffer* idxShadowSource,
                                          VkDeviceSize idxShadowOffset) {
    ScopedCpuProfileZoneN("D3D11Rtx::RebaseIndexedVertexRange");
    RasterGeometry& geo = dcs.geometryData;
    const RasterBuffer& ib = geo.indexBuffer;
    const bool is32 = ib.indexType() == VK_INDEX_TYPE_UINT32;
    const uint32_t idxStride = is32 ? 4u : 2u;
    if (indexCount == 0 || geo.vertexCount == 0)
      return false;

    const void* src = ib.mapPtr(0);
    if (src == nullptr && idxShadowSource != nullptr)
      src = idxShadowSource->GetIndexShadow(idxShadowOffset, VkDeviceSize(indexCount) * idxStride);
    if (src == nullptr)
      return false;

    const bool restart = geo.topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    const uint32_t restartValue = is32 ? 0xFFFFFFFFu : 0xFFFFu;
    auto readIndex = [&](uint32_t i) {
      if (is32) { uint32_t v; std::memcpy(&v, static_cast<const uint8_t*>(src) + size_t(i) * 4u, 4u); return v; }
      uint16_t v; std::memcpy(&v, static_cast<const uint8_t*>(src) + size_t(i) * 2u, 2u); return uint32_t(v);
    };

    uint32_t minIndex = UINT32_MAX, maxIndex = 0;
    for (uint32_t i = 0; i < indexCount; ++i) {
      const uint32_t v = readIndex(i);
      if (restart && v == restartValue)
        continue;
      minIndex = std::min(minIndex, v);
      maxIndex = std::max(maxIndex, v);
    }
    if (minIndex == UINT32_MAX || maxIndex >= geo.vertexCount)
      return false;
    // Not worth a copy when almost nothing would be saved.
    if (minIndex < 64u) {
      geo.vertexCount = maxIndex + 1u;
      return true;
    }

    const VkDeviceSize idxBytes = VkDeviceSize(indexCount) * idxStride;
    Rc<DxvkBuffer> dst = AcquireHostVisibleHelperBuffer(idxBytes, "d3d11 rtx rebased indices");
    uint8_t* out = dst != nullptr ? static_cast<uint8_t*>(dst->mapPtr(0)) : nullptr;
    if (out == nullptr)
      return false;
    for (uint32_t i = 0; i < indexCount; ++i) {
      const uint32_t v = readIndex(i);
      const uint32_t r = (restart && v == restartValue) ? v : v - minIndex;
      if (is32) std::memcpy(out + size_t(i) * 4u, &r, 4u);
      else { const uint16_t r16 = uint16_t(r); std::memcpy(out + size_t(i) * 2u, &r16, 2u); }
    }

    // Every per-vertex stream moves forward by minIndex elements; streams that
    // share one buffer stay interleaved because each moves by its own stride.
    const uint32_t span = maxIndex - minIndex + 1u;
    auto shift = [&](RasterBuffer& buf) {
      if (!buf.defined() || buf.stride() == 0)
        return;
      const VkDeviceSize skip = VkDeviceSize(minIndex) * buf.stride();
      if (skip >= buf.length())
        return;
      buf = RasterBuffer(buf.subSlice(skip, buf.length() - skip),
                         buf.offsetFromSlice(), buf.stride(), buf.vertexFormat());
    };
    shift(geo.positionBuffer);
    shift(geo.normalBuffer);
    shift(geo.texcoordBuffer);
    shift(geo.color0Buffer);
    shift(geo.blendWeightBuffer);
    shift(geo.blendIndicesBuffer);

    geo.indexBuffer = RasterBuffer(DxvkBufferSlice(dst, 0, idxBytes), 0, idxStride, ib.indexType());
    geo.vertexCount = span;
    ++m_submitRejectStats.exactWorldRebased;
    m_submitRejectStats.exactWorldVerticesSaved += minIndex;
    return true;
  }

  void D3D11Rtx::RecycleHelperBuffers() {
    // A retired buffer is reusable once this pool holds the only reference
    // and the GPU has retired all command lists that touched it.
    for (size_t i = 0; i < m_helperRetired.size();) {
      DxvkBuffer* raw = m_helperRetired[i].buffer.ptr();
      if (raw != nullptr && raw->refCount() == 1 && !raw->isInUse(DxvkAccess::Read)) {
        m_helperFree.push_back(m_helperRetired[i]);
        m_helperRetired[i] = m_helperRetired.back();
        m_helperRetired.pop_back();
      } else {
        ++i;
      }
    }
    // Bound the idle free list so a scene change does not park hundreds of
    // MB forever.
    static constexpr size_t kMaxFreeItems = 2048;
    while (m_helperFree.size() > kMaxFreeItems) {
      m_helperPoolBytes -= m_helperFree.back().capacity;
      m_helperFree.pop_back();
    }
  }

  static bool semanticNameStartsWith(const D3D11RtxSemantic& semantic, const char* prefix) {
    return std::strncmp(semantic.name, prefix, std::strlen(prefix)) == 0;
  }

  static bool isFloatSemantic(const D3D11RtxSemantic& semantic) {
    return semantic.componentType == DxbcScalarType::Float32;
  }

  static bool isSupportedTexcoordFormat(VkFormat format) {
    return format == VK_FORMAT_R32G32B32A32_SFLOAT
        || format == VK_FORMAT_R32G32B32_SFLOAT
        || format == VK_FORMAT_R32G32_SFLOAT
      || format == VK_FORMAT_R16G16_SFLOAT
      || format == VK_FORMAT_R16G16B16A16_SFLOAT
      || format == VK_FORMAT_R8G8_UNORM
      || format == VK_FORMAT_R8G8_SNORM
      || format == VK_FORMAT_R8G8B8A8_UNORM
      || format == VK_FORMAT_R8G8B8A8_SNORM
      || format == VK_FORMAT_R16G16_UNORM
      || format == VK_FORMAT_R16G16_SNORM
      || format == VK_FORMAT_R16G16B16A16_UNORM
      || format == VK_FORMAT_R16G16B16A16_SNORM
      // Fixed-point integer UVs: decoded to float by the interleaver with
      // rtx.integerTexcoordScale (Saints Row IV: TEXCOORD0 = R16G16_SINT).
      || format == VK_FORMAT_R16G16_SINT
      || format == VK_FORMAT_R16G16_UINT
      // DX11_V269: 32-bit fixed-point UVs, same scale treatment.
      || format == VK_FORMAT_R32G32_SINT
      || format == VK_FORMAT_R32G32_UINT;
  }

  static bool isPositionFormat(VkFormat format) {
    return format == VK_FORMAT_R32G32_SFLOAT
        || format == VK_FORMAT_R32G32B32_SFLOAT
        || format == VK_FORMAT_R32G32B32A32_SFLOAT
        || format == static_cast<VkFormat>(97);
  }

  // Byte size of one position element for the formats isPositionFormat accepts.
  // Used to bounds-check reads when computing the mesh bounding box.
  static uint32_t positionElementBytes(VkFormat format) {
    switch (format) {
      case VK_FORMAT_R32G32B32A32_SFLOAT:   return 16;
      case VK_FORMAT_R32G32B32_SFLOAT:      return 12;
      case VK_FORMAT_R32G32_SFLOAT:         return 8;
      case static_cast<VkFormat>(97):       return 8; // R16G16B16A16_SFLOAT (half4)
      default:                              return 0;
    }
  }

  // Decode an object-space position for bounding-box computation. Mirrors the
  // formats accepted by isPositionFormat. Returns false for unsupported formats
  // or non-finite data, in which case the caller leaves the bbox invalid so the
  // instance is kept (fail-safe: never drop geometry on a decode failure).
  static bool decodePositionForBounds(const uint8_t* src, VkFormat format, float out[3]) {
    switch (format) {
      case VK_FORMAT_R32G32B32_SFLOAT:
      case VK_FORMAT_R32G32B32A32_SFLOAT: {
        const float* f = reinterpret_cast<const float*>(src);
        out[0] = f[0]; out[1] = f[1]; out[2] = f[2];
      } break;
      case VK_FORMAT_R32G32_SFLOAT: {
        const float* f = reinterpret_cast<const float*>(src);
        out[0] = f[0]; out[1] = f[1]; out[2] = 0.0f;
      } break;
      case static_cast<VkFormat>(97): { // R16G16B16A16_SFLOAT (half4)
        const uint16_t* h = reinterpret_cast<const uint16_t*>(src);
        out[0] = decodeFloat16(h[0]); out[1] = decodeFloat16(h[1]); out[2] = decodeFloat16(h[2]);
      } break;
      default:
        return false;
    }
    return std::isfinite(out[0]) && std::isfinite(out[1]) && std::isfinite(out[2]);
  }

  static bool isNormalFormat(VkFormat format) {
    return format == VK_FORMAT_R8G8B8A8_UNORM
        || format == VK_FORMAT_R32G32B32_SFLOAT
        || format == VK_FORMAT_R32G32B32A32_SFLOAT
        || format == VK_FORMAT_R32G32_SFLOAT
        || format == VK_FORMAT_R16G16_SFLOAT
        || format == static_cast<VkFormat>(65);
  }

  static bool isColorFormat(VkFormat format) {
    return format == VK_FORMAT_B8G8R8A8_UNORM
        || format == VK_FORMAT_R8G8B8A8_UNORM
        || format == VK_FORMAT_R32G32B32A32_SFLOAT
        // DX11_V268_VERTEX_COLOR_FORMATS: half4/unorm16 vertex colors -
        // accepted only under an explicit COLOR semantic name (the scorer
        // rejects them for generic names since these formats also carry
        // normals/tangents in many layouts).
        || format == VK_FORMAT_R16G16B16A16_UNORM
        || format == VK_FORMAT_R16G16B16A16_SFLOAT;
  }

  static bool isBlendWeightFormat(VkFormat format) {
    switch (format) {
      case VK_FORMAT_R32_SFLOAT:
      case VK_FORMAT_R32G32_SFLOAT:
      case VK_FORMAT_R32G32B32_SFLOAT:
      case VK_FORMAT_R32G32B32A32_SFLOAT:
      case VK_FORMAT_R16_SFLOAT:
      case VK_FORMAT_R16G16_SFLOAT:
      case VK_FORMAT_R16G16B16A16_SFLOAT:
      case VK_FORMAT_R8_UNORM:
      case VK_FORMAT_R8G8_UNORM:
      case VK_FORMAT_R8G8B8A8_UNORM:
      case VK_FORMAT_R16_UNORM:
      case VK_FORMAT_R16G16_UNORM:
      case VK_FORMAT_R16G16B16A16_UNORM:
        return true;
      default:
        return false;
    }
  }

  static bool isBlendIndexFormat(VkFormat format) {
    switch (format) {
      case VK_FORMAT_R8_UINT:
      case VK_FORMAT_R8_USCALED:
      case VK_FORMAT_R8G8_UINT:
      case VK_FORMAT_R8G8_USCALED:
      case VK_FORMAT_R8G8B8A8_UINT:
      case VK_FORMAT_R8G8B8A8_USCALED:
      case VK_FORMAT_R16_UINT:
      case VK_FORMAT_R16G16_UINT:
      case VK_FORMAT_R16G16B16A16_UINT:
      case VK_FORMAT_R32_UINT:
      case VK_FORMAT_R32G32_UINT:
      case VK_FORMAT_R32G32B32_UINT:
      case VK_FORMAT_R32G32B32A32_UINT:
        return true;
      default:
        return false;
    }
  }

  static int scorePositionSemantic(const D3D11RtxSemantic& semantic) {
    if (semantic.perInstance || semantic.systemValue != DxbcSystemValue::None || !isPositionFormat(semantic.format))
      return std::numeric_limits<int>::min();

    int score = 0;
    if (semanticNameStartsWith(semantic, "POSITION"))
      score += 1000;
    else if (semanticNameStartsWith(semantic, "ATTRIBUTE"))
      score += 120;
    else if (semanticNameStartsWith(semantic, "TEXCOORD")) {
      if (!isFloatSemantic(semantic) || semantic.componentCount < 3)
        return std::numeric_limits<int>::min();

      score += 20;
    }
    else if (semanticNameStartsWith(semantic, "COLOR")
          || semanticNameStartsWith(semantic, "NORMAL")
          || semanticNameStartsWith(semantic, "BLEND"))
      return std::numeric_limits<int>::min();

    if (isFloatSemantic(semantic))
      score += 120;
    if (semantic.componentCount >= 3)
      score += 140;
    else if (semantic.componentCount == 2)
      score += 40;

    if (semantic.index == 0)
      score += 80;
    if (semantic.registerId == 0)
      score += 60;

    switch (semantic.format) {
      case VK_FORMAT_R32G32B32_SFLOAT:    score += 240; break;
      case VK_FORMAT_R32G32B32A32_SFLOAT: score += 200; break;
      case static_cast<VkFormat>(97):     score += 180; break;
      case VK_FORMAT_R32G32_SFLOAT:       score += 60; break;
      default: break;
    }

    return score;
  }

  static int scoreTexcoordSemantic(const D3D11RtxSemantic& semantic) {
    if (semantic.perInstance || semantic.systemValue != DxbcSystemValue::None || !isSupportedTexcoordFormat(semantic.format) || semantic.componentCount < 2)
      return std::numeric_limits<int>::min();

    // DX11_V269: 4-component texcoords are real UVs - engines pack two UV
    // sets into one float4 (xy = uv0, zw = uv1). Rejecting them dropped the
    // texcoord entirely (textures with no UVs = flat albedo); accept and use
    // xy, just score below dedicated 2-component streams.

    int score = 0;
    if (semanticNameStartsWith(semantic, "TEXCOORD")
     || semanticNameStartsWith(semantic, "TEX")
     || semanticNameStartsWith(semantic, "UV")
     || semanticNameStartsWith(semantic, "TCOORD")
     || semanticNameStartsWith(semantic, "MAP"))
      score += 1000;
    else if (semanticNameStartsWith(semantic, "ATTRIBUTE"))
      score += 140;
    else if (semanticNameStartsWith(semantic, "COLOR"))
      return std::numeric_limits<int>::min();
    else if (semanticNameStartsWith(semantic, "POSITION")
          || semanticNameStartsWith(semantic, "NORMAL")
          || semanticNameStartsWith(semantic, "BLEND"))
      return std::numeric_limits<int>::min();

    if (isFloatSemantic(semantic))
      score += 100;
    if (semantic.componentCount == 2)
      score += 220;
    else if (semantic.componentCount == 3)
      score += 100;
    else if (semantic.componentCount == 4)
      score += 40;  // DX11_V269: packed uv0+uv1 float4 - xy is used

    if (semantic.index == 0)
      score += 70;
    else if (semantic.index == 1)
      score += 40;

    if (semantic.registerId == 0)
      score -= 20;
    else
      score += 20;

    switch (semantic.format) {
      case VK_FORMAT_R32G32_SFLOAT:          score += 280; break;
      case VK_FORMAT_R16G16_SFLOAT:          score += 240; break;
      case VK_FORMAT_R8G8_UNORM:             score += 180; break;
      case VK_FORMAT_R16G16_UNORM:           score += 160; break;
      case VK_FORMAT_R8G8_SNORM:             score += 120; break;
      case VK_FORMAT_R16G16_SNORM:           score += 110; break;
      case VK_FORMAT_R32G32B32_SFLOAT:       score += 120; break;
      case VK_FORMAT_R16G16B16A16_SFLOAT:    score += 40; break;
      case VK_FORMAT_R32G32B32A32_SFLOAT:    score += 20; break;
      default: break;
    }

    return score;
  }

  static int scoreTexcoordFallbackSemantic(const D3D11RtxSemantic& semantic) {
    if (semantic.perInstance || semantic.systemValue != DxbcSystemValue::None || !isSupportedTexcoordFormat(semantic.format) || semantic.componentCount < 2)
      return std::numeric_limits<int>::min();

    if (semanticNameStartsWith(semantic, "POSITION")
     || semanticNameStartsWith(semantic, "NORMAL")
     || semanticNameStartsWith(semantic, "BLEND")
     || semanticNameStartsWith(semantic, "COLOR"))
      return std::numeric_limits<int>::min();

    int score = 0;
    if (semanticNameStartsWith(semantic, "ATTRIBUTE"))
      score += 220;
    else
      score += 80;

    if (isFloatSemantic(semantic))
      score += 100;

    if (semantic.componentCount == 2)
      score += 240;
    else if (semantic.componentCount == 3)
      score += 100;
    else if (semantic.componentCount == 4)
      score += 40;  // DX11_V269: packed uv0+uv1 float4 - xy is used

    if (semantic.index == 0)
      score += 50;
    else if (semantic.index == 1)
      score += 35;

    if (semantic.registerId == 0)
      score -= 20;
    else
      score += 20;

    switch (semantic.format) {
      case VK_FORMAT_R32G32_SFLOAT:          score += 300; break;
      case VK_FORMAT_R16G16_SFLOAT:          score += 260; break;
      case VK_FORMAT_R8G8_UNORM:             score += 200; break;
      case VK_FORMAT_R16G16_UNORM:           score += 170; break;
      case VK_FORMAT_R8G8_SNORM:             score += 130; break;
      case VK_FORMAT_R16G16_SNORM:           score += 120; break;
      case VK_FORMAT_R32G32B32_SFLOAT:       score += 100; break;
      case VK_FORMAT_R16G16B16A16_SFLOAT:    score += 20; break;
      case VK_FORMAT_R32G32B32A32_SFLOAT:    score += 0; break;
      default: break;
    }

    return score;
  }

  static int scoreNormalSemantic(const D3D11RtxSemantic& semantic) {
    if (semantic.perInstance || semantic.systemValue != DxbcSystemValue::None || !isNormalFormat(semantic.format))
      return std::numeric_limits<int>::min();

    int score = 0;
    if (semanticNameStartsWith(semantic, "NORMAL"))
      score += 1000;
    else if (semanticNameStartsWith(semantic, "ATTRIBUTE"))
      score += 100;
    // DX11_V259: tangent-frame streams share the normal formats but are NOT
    // shading normals - picking one bends lighting on every lightmapped mesh.
    // Remix regenerates normals when absent, so rejecting is strictly safer.
    else if (semanticNameStartsWith(semantic, "POSITION")
          || semanticNameStartsWith(semantic, "TEXCOORD")
          || semanticNameStartsWith(semantic, "COLOR")
          || semanticNameStartsWith(semantic, "BLEND")
          || semanticNameStartsWith(semantic, "TANGENT")
          || semanticNameStartsWith(semantic, "BINORMAL")
          || semanticNameStartsWith(semantic, "BITANGENT"))
      return std::numeric_limits<int>::min();

    if (semantic.componentCount >= 3)
      score += 140;
    else if (semantic.componentCount == 2)
      score += 40;

    switch (semantic.format) {
      case VK_FORMAT_R8G8B8A8_UNORM:      score += 220; break;
      case static_cast<VkFormat>(65):     score += 220; break;
      case VK_FORMAT_R32G32B32_SFLOAT:    score += 180; break;
      case VK_FORMAT_R32G32B32A32_SFLOAT: score += 150; break;
      case VK_FORMAT_R32G32_SFLOAT:       score += 90; break;
      case VK_FORMAT_R16G16_SFLOAT:       score += 80; break;
      default: break;
    }

    return score;
  }

  static int scoreColorSemantic(const D3D11RtxSemantic& semantic) {
    if (semantic.perInstance || semantic.systemValue != DxbcSystemValue::None || !isColorFormat(semantic.format))
      return std::numeric_limits<int>::min();

    // DX11_V268: 16-bit-per-channel formats double as normal/tangent storage
    // in many vertex layouts; only an explicit COLOR name may claim them.
    if ((semantic.format == VK_FORMAT_R16G16B16A16_UNORM
      || semantic.format == VK_FORMAT_R16G16B16A16_SFLOAT)
     && !semanticNameStartsWith(semantic, "COLOR"))
      return std::numeric_limits<int>::min();

    int score = 0;
    if (semanticNameStartsWith(semantic, "COLOR"))
      score += 1000;
    else if (semanticNameStartsWith(semantic, "ATTRIBUTE"))
      score += 80;
    // DX11_V259: packed UBYTE4 tangent frames share COLOR0's format - misread
    // as vertex color they tint/darken every surface (worst with
    // vertexColorIsBakedLighting, where they masquerade as baked lighting).
    else if (semanticNameStartsWith(semantic, "POSITION")
          || semanticNameStartsWith(semantic, "TEXCOORD")
          || semanticNameStartsWith(semantic, "NORMAL")
          || semanticNameStartsWith(semantic, "BLEND")
          || semanticNameStartsWith(semantic, "TANGENT")
          || semanticNameStartsWith(semantic, "BINORMAL")
          || semanticNameStartsWith(semantic, "BITANGENT"))
      return std::numeric_limits<int>::min();

    switch (semantic.format) {
      case VK_FORMAT_B8G8R8A8_UNORM:      score += 240; break;
      case VK_FORMAT_R8G8B8A8_UNORM:      score += 220; break;
      case VK_FORMAT_R32G32B32A32_SFLOAT: score += 90; break;
      default: break;
    }

    return score;
  }

  // DX11_V259_SKINNING_NAME_GATE: only explicitly skinning-named semantics may
  // become bone weights/indices. The old heuristics accepted ANY unrecognized
  // semantic name (only POSITION/TEXCOORD/NORMAL/COLOR/BLEND* were excluded),
  // and the accepted formats overlap ordinary vertex data: lightmap UV
  // channels with custom names (LIGHTMAPUV, LM_UV, UV1...) are float2,
  // TANGENT/BINORMAL frames are float4/UBYTE4, and lightmap atlas page
  // indices are UINT - all of which passed as "bone weights"/"bone indices"
  // on static lightmapped world geometry. That flipped numBonesPerVertex >= 2
  // and the skinning path deformed the mesh with garbage "bone matrices"
  // scanned from the constant buffers, smearing broken copies over the scene.
  // The harm is asymmetric: a false positive destroys static geometry, while
  // a false negative merely skips skinning replication for a mesh the game
  // still renders. So: strict name allow-list, no generic-name fallback.
  static int scoreBlendWeightSemantic(const D3D11RtxSemantic& semantic) {
    if (semantic.perInstance || semantic.systemValue != DxbcSystemValue::None || !isBlendWeightFormat(semantic.format))
      return std::numeric_limits<int>::min();

    int score = 0;
    if (semanticNameStartsWith(semantic, "BLENDWEIGHT"))
      score += 1000;
    else if (semanticNameStartsWith(semantic, "BONEWEIGHT")
          || semanticNameStartsWith(semantic, "SKINWEIGHT")
          || semanticNameStartsWith(semantic, "WEIGHT"))
      score += 500;
    else
      return std::numeric_limits<int>::min();

    if (semantic.componentCount >= 1)
      score += 40;

    return score;
  }

  static int scoreBlendIndexSemantic(const D3D11RtxSemantic& semantic) {
    if (semantic.perInstance || semantic.systemValue != DxbcSystemValue::None || !isBlendIndexFormat(semantic.format))
      return std::numeric_limits<int>::min();

    int score = 0;
    if (semanticNameStartsWith(semantic, "BLENDINDICES")
     || semanticNameStartsWith(semantic, "BLENDINDEX"))
      score += 1000;
    else if (semanticNameStartsWith(semantic, "BONEINDICES")
          || semanticNameStartsWith(semantic, "BONEINDEX")
          || semanticNameStartsWith(semantic, "SKININDICES")
          || semanticNameStartsWith(semantic, "SKININDEX")
          || semanticNameStartsWith(semantic, "BONES"))
      score += 500;
    else
      return std::numeric_limits<int>::min();

    if (semantic.componentCount >= 1)
      score += 40;

    return score;
  }

  template <typename ScoreFn>
  static const D3D11RtxSemantic* selectBestSemantic(const std::vector<D3D11RtxSemantic>& semantics,
                                                    ScoreFn&& scoreFn,
                                                    std::initializer_list<const D3D11RtxSemantic*> excluded = {}) {
    const D3D11RtxSemantic* best = nullptr;
    int bestScore = std::numeric_limits<int>::min();

    for (const auto& semantic : semantics) {
      bool skip = false;
      for (const D3D11RtxSemantic* used : excluded) {
        if (used == &semantic) {
          skip = true;
          break;
        }
      }

      if (skip)
        continue;

      const int score = scoreFn(semantic);
      if (score > bestScore) {
        best = &semantic;
        bestScore = score;
      }
    }

    return bestScore > 0 ? best : nullptr;
  }

  // Detect a perspective projection matrix in either memory layout.
  //
  // Row-major layout used by many D3D renderers:
  //   m[0] = [Â±Sx, 0,   0,    0  ]
  //   m[1] = [0,  Â±Sy,  0,    0  ]
  //   m[2] = [Jx,  Jy,  Q,   Â±1 ]  â† perspective-divide at m[2][3]
  //   m[3] = [0,   0,   Wz,   0  ]
  //
  // Column-major matrices read back as row-major:
  //   m[0] = [Â±Sx, 0,   0,    0  ]
  //   m[1] = [0,  Â±Sy,  0,    0  ]
  //   m[2] = [Jx,  Jy,  Q,   Wz ]  â† m[2][3] = nearPlane or 0
  //   m[3] = [0,   0,  Â±1,    0  ]  â† perspective-divide at m[3][2]
  //
  // Returns: 0 = not perspective, 1 = row-major, 2 = column-major-as-row.
  static int classifyPerspective(const Matrix4& m) {
    constexpr float kTol = 0.02f;
    constexpr float kJitterTol = 0.35f;

    for (int row = 0; row < 4; ++row) {
      for (int col = 0; col < 4; ++col) {
        if (!std::isfinite(m[row][col]))
          return 0;
      }
    }

    // Shared: rows 0-1 keep the scale terms on the diagonal with no w component.
    // Off-center projection jitter lives in different cells depending on layout,
    // so do not reject m[0][2] / m[1][2] until we know which convention we have.
    if (std::abs(m[0][1]) > kTol || std::abs(m[0][3]) > kTol) return 0;
    if (std::abs(m[1][0]) > kTol || std::abs(m[1][3]) > kTol) return 0;
    if (std::abs(m[0][0]) < 0.1f || std::abs(m[1][1]) < 0.1f) return 0;

    // Row-major check: m[2][3] â‰ˆ Â±1, m[3][3] â‰ˆ 0.
    const bool r23 = std::abs(std::abs(m[2][3]) - 1.0f) < kTol;
    const bool r33z = std::abs(m[3][3]) < kTol;
    if (r23 && r33z) {
      if (std::abs(m[0][2]) > kTol || std::abs(m[1][2]) > kTol) return 0;
      if (std::abs(m[3][0]) > kTol || std::abs(m[3][1]) > kTol) return 0;
      return 1;
    }

    // Column-major-as-row check: m[3][2] â‰ˆ Â±1, m[3][3] â‰ˆ 0.
    const bool c32 = std::abs(std::abs(m[3][2]) - 1.0f) < kTol;
    const bool c33z = std::abs(m[3][3]) < kTol;
    if (c32 && c33z) {
      // Column-major projections transpose the off-center
      // terms into m[0][2] / m[1][2] when read as row-major.
      if (std::abs(m[0][2]) > kJitterTol || std::abs(m[1][2]) > kJitterTol) return 0;
      if (std::abs(m[2][0]) > kTol || std::abs(m[2][1]) > kTol) return 0;
      if (std::abs(m[3][0]) > kTol || std::abs(m[3][1]) > kTol) return 0;
      return 2;
    }

    return 0;
  }

  static bool isFiniteMatrix(const Matrix4& m) {
    for (int row = 0; row < 4; ++row) {
      for (int col = 0; col < 4; ++col) {
        if (!std::isfinite(m[row][col]))
          return false;
      }
    }
    return true;
  }

  static bool isAffineMatrix(const Matrix4& m) {
    if (!isFiniteMatrix(m))
      return false;
    if (std::abs(m[3][3] - 1.0f) > 0.01f)
      return false;
    if (std::abs(m[0][3]) > 0.01f || std::abs(m[1][3]) > 0.01f || std::abs(m[2][3]) > 0.01f)
      return false;
    return true;
  }

  static Matrix4 canonicalizeProjectionOrientation(const Matrix4& projection,
                                                   bool* flippedX = nullptr,
                                                   bool* flippedY = nullptr) {
    Matrix4 normalized = projection;

    const bool didFlipX = normalized[0][0] < 0.0f;
    const bool didFlipY = normalized[1][1] < 0.0f;

    if (didFlipX) {
      normalized[0][0] = -normalized[0][0];
      normalized[2][0] = -normalized[2][0];
    }

    if (didFlipY) {
      normalized[1][1] = -normalized[1][1];
      normalized[2][1] = -normalized[2][1];
    }

    if (flippedX)
      *flippedX = didFlipX;
    if (flippedY)
      *flippedY = didFlipY;

    return normalized;
  }

  // Return true if m looks like a camera view matrix (rigid-body: rotation + translation).
  // Expects row-major convention (or column-major already transposed by the caller).
  // The upper-left 3Ã—3 should be approximately orthonormal and the last column [0,0,0,1].
  static bool isViewMatrix(const Matrix4& m) {
    // Row 3 must be [*, *, *, 1] (affine).
    if (std::abs(m[3][3] - 1.0f) > 0.01f) return false;
    // Columns 0-2 of rows 0-2 should have unit length (orthonormal rotation).
    for (int col = 0; col < 3; ++col) {
      float lenSq = m[0][col] * m[0][col] + m[1][col] * m[1][col] + m[2][col] * m[2][col];
      if (std::abs(lenSq - 1.0f) > 0.1f) return false;
    }
    Vector3 axisX(m[0][0], m[1][0], m[2][0]);
    Vector3 axisY(m[0][1], m[1][1], m[2][1]);
    Vector3 axisZ(m[0][2], m[1][2], m[2][2]);
    if (std::abs(dot(axisX, axisY)) > 0.08f
     || std::abs(dot(axisX, axisZ)) > 0.08f
     || std::abs(dot(axisY, axisZ)) > 0.08f) {
      return false;
    }
    const double det = determinant(m);
    if (!std::isfinite(det) || std::abs(std::abs(det) - 1.0) > 0.25)
      return false;
    // m[0][3], m[1][3], m[2][3] should be 0 (no perspective warp).
    if (std::abs(m[0][3]) > 0.01f || std::abs(m[1][3]) > 0.01f || std::abs(m[2][3]) > 0.01f)
      return false;
    // Reject identity â€” identity means "no view transform" which is not useful.
    if (isIdentityExact(m)) return false;
    return true;
  }

  static bool canSafelyInvertAffineViewCandidate(const Matrix4& candidate) {
    if (!isAffineMatrix(candidate))
      return false;

    const double det = determinant(candidate);
    return std::isfinite(det) && std::abs(det) >= 1e-10;
  }

  static bool resolveViewMatrixCandidate(const Matrix4& candidate, Matrix4& outView) {
    if (isViewMatrix(candidate)) {
      outView = candidate;
      return true;
    }

    if (!canSafelyInvertAffineViewCandidate(candidate))
      return false;

    Matrix4 inverseCandidate = inverseAffine(candidate);
    if (!isFiniteMatrix(inverseCandidate) || !isViewMatrix(inverseCandidate))
      return false;

    outView = inverseCandidate;
    return true;
  }

  DrawCallTransforms D3D11Rtx::ExtractTransforms() {
    ScopedCpuProfileZoneN("D3D11Rtx::ExtractTransforms");
    ScopedPhaseTimer phaseTimer(m_framePhaseExtractNs);
    m_drawJitteredProjectionValid = false;

    DrawCallTransforms transforms;
    bool projectionWasFlippedY = false;

    // Maximum bytes to scan per cbuffer. Projection/view/world matrices are
    // always in the first few hundred bytes of a cbuffer â€” capping the scan
    // prevents multi-second stalls on emulators that pack all constants into
    // a single 64KB+ UBO (Xenia, Yuzu, RPCS3, Citra).
    static constexpr size_t kFastScanBytes = 8192;   // 128 matrices
    static constexpr size_t kDeepScanBytes = 65536;  // Full D3D11 cbuffer
    const bool needDeepCameraScan = false; // Disabled for performance: deep scanning causes severe stutter
    const size_t maxScanBytes = needDeepCameraScan ? kDeepScanBytes : kFastScanBytes;

    // Compute the scannable byte range for a cbuffer binding: the intersection
    // of the bound range (constantOffset..constantOffset+constantCount) with
    // the buffer allocation, capped to maxScanBytes from the start of the range.
    auto cbRange = [maxScanBytes](const D3D11ConstantBufferBinding& cb) -> std::pair<size_t, size_t> {
      const size_t bufSize = cb.buffer->Desc()->ByteWidth;
      const size_t base    = static_cast<size_t>(cb.constantOffset) * 16;
      if (base >= bufSize)
        return { 0, 0 };
      size_t end;
      if (cb.constantCount > 0)
        end = std::min(base + static_cast<size_t>(cb.constantCount) * 16, bufSize);
      else
        end = bufSize;
      if (end - base > maxScanBytes)
        end = base + maxScanBytes;
      return { base, end };
    };

    // Some engines store matrices transposed in memory;
    // transposing after read normalizes them to row-major for all our checks.
    auto readMatrixWithConvention = [](const uint8_t* ptr, size_t offset, size_t bufSize, bool columnMajor) -> Matrix4 {
      Matrix4 m = readCbMatrix(ptr, offset, bufSize);
      return columnMajor ? transpose(m) : m;
    };

    auto readMatrix = [this, &readMatrixWithConvention](const uint8_t* ptr, size_t offset, size_t bufSize) -> Matrix4 {
      return readMatrixWithConvention(ptr, offset, bufSize, m_columnMajor);
    };

    auto resolveViewAt = [&](const uint8_t* ptr,
                             size_t offset,
                             size_t bufSize,
                             bool primaryColumnMajor,
                             bool allowOppositeConvention,
                             Matrix4& resolvedView,
                             bool& resolvedColumnMajor) -> bool {
      const Matrix4 primary = readMatrixWithConvention(ptr, offset, bufSize, primaryColumnMajor);
      if (resolveViewMatrixCandidate(primary, resolvedView)) {
        resolvedColumnMajor = primaryColumnMajor;
        return true;
      }

      if (allowOppositeConvention) {
        const bool oppositeColumnMajor = !primaryColumnMajor;
        const Matrix4 opposite = readMatrixWithConvention(ptr, offset, bufSize, oppositeColumnMajor);
        if (resolveViewMatrixCandidate(opposite, resolvedView)) {
          resolvedColumnMajor = oppositeColumnMajor;
          return true;
        }
      }

      return false;
    };

    // Viewport and render-target size are the most reliable camera references
    // for emulators and dynamic-resolution games. The host window can change
    // independently from the actual scene resolution, so client/output extents
    // should only be fallback hints instead of the primary aspect source.
    //
    // If the game has bound zero viewports (some engines leave the RS viewport
    // state dirty across a no-RT clear pass) or more than one viewport (shadow
    // cascade or split-screen passes), treat viewport[0] as a fallback hint
    // only and do not let it drive camera detection â€” otherwise a 256x256
    // cascade viewport would stamp its aspect onto the primary camera and
    // cause path tracing to render the wrong frustum for the main scene.
    float viewportAspect = 0.0f;
    const uint32_t boundViewports = m_context->m_state.rs.numViewports;
    const bool singleSceneViewport = boundViewports == 1;
    if (singleSceneViewport) {
      const auto& vp = m_context->m_state.rs.viewports[0];
      if (vp.Height > 0.0f && std::isfinite(vp.Width) && std::isfinite(vp.Height))
        viewportAspect = vp.Width / vp.Height;
    }
    float renderTargetWidth = 0.0f;
    float renderTargetHeight = 0.0f;
    float renderTargetAspect = 0.0f;
    if (auto* rtv = m_context->m_state.om.renderTargetViews[0].ptr()) {
      Rc<DxvkImageView> rtvView = rtv->GetImageView();
      if (rtvView != nullptr) {
        const VkExtent3D targetExtent = rtvView->image()->info().extent;
        if (targetExtent.width > 0 && targetExtent.height > 0) {
          renderTargetWidth = float(targetExtent.width);
          renderTargetHeight = float(targetExtent.height);
          renderTargetAspect = renderTargetWidth / renderTargetHeight;
        }
      }
    }
    float remixViewportAspect = 0.0f;
    if (m_lastRemixViewportExtent.width > 0u && m_lastRemixViewportExtent.height > 0u)
      remixViewportAspect = float(m_lastRemixViewportExtent.width) / float(m_lastRemixViewportExtent.height);
    float outputAspect = 0.0f;
    if (m_lastOutputExtent.width > 0u && m_lastOutputExtent.height > 0u)
      outputAspect = float(m_lastOutputExtent.width) / float(m_lastOutputExtent.height);
    const float projectionReferenceAspect = viewportAspect > 0.0f
      ? viewportAspect
      : (renderTargetAspect > 0.0f
        ? renderTargetAspect
        : (outputAspect > 0.0f
          ? outputAspect
          : remixViewportAspect));
    const float fallbackReferenceAspect = viewportAspect > 0.0f
      ? viewportAspect
      : (renderTargetAspect > 0.0f
        ? renderTargetAspect
        : (outputAspect > 0.0f
          ? outputAspect
          : remixViewportAspect));

    // Score a perspective projection: higher = more likely main game camera.
    // Shadow maps have square aspect, cubemaps have 90Â° FOV, tool cameras
    // have extreme FOV â€” all score lower than a typical game camera.
    auto scorePerspective = [projectionReferenceAspect](const Matrix4& proj) -> float {
      const Matrix4 scoredProj = canonicalizeProjectionOrientation(proj);
      float score = 1.0f;
      DecomposeProjectionParams dpp;
      decomposeProjection(scoredProj, dpp);
      // Guard against degenerate decomposition (NaN/Inf from near-singular matrices).
      if (!std::isfinite(dpp.fov) || !std::isfinite(dpp.aspectRatio) || !std::isfinite(dpp.nearPlane))
        return score;
      float fovDeg = dpp.fov * (180.0f / 3.14159265f);
      if (fovDeg >= 30.0f && fovDeg <= 120.0f)
        score += 2.0f;
      else if (fovDeg >= 15.0f && fovDeg <= 150.0f)
        score += 1.0f;
      if (projectionReferenceAspect > 0.0f) {
        float diff = std::abs(std::abs(dpp.aspectRatio) - projectionReferenceAspect);
        if (diff < 0.15f)
          score += 2.0f;
        else if (diff < 0.5f)
          score += 1.0f;
      }
      if (dpp.nearPlane > 0.001f && dpp.nearPlane < 100.0f)
        score += 1.0f;
      if (proj[0][0] < 0.0f)
        score -= 0.5f;
      if (proj[1][1] < 0.0f)
        score -= 1.0f;
      return score;
    };

    // Raster shader stages to scan for camera matrices.
    // VS is most common; emulators (Dolphin, PCSX2, Xenia, Citra) and some
    // deferred renderers put camera matrices in GS, DS, or PS cbuffers.
    const D3D11ConstantBufferBindings* stageCbs[] = {
      &m_context->m_state.vs.constantBuffers,
      &m_context->m_state.hs.constantBuffers,
      &m_context->m_state.gs.constantBuffers,
      &m_context->m_state.ds.constantBuffers,
      &m_context->m_state.ps.constantBuffers,
    };
    static constexpr int kNumStages = 5;
    static const char* kStageNames[] = { "VS", "HS", "GS", "DS", "PS" };

    // Scan one stage's cbuffers for the best-scoring perspective matrix.
    // classifyPerspective detects both row-major and column-major-as-row
    // layouts in a single pass, so no separate transpose pass is needed.
    auto scanStageForProj = [&](int stageIdx,
        uint32_t& outSlot, size_t& outOff, float& outScore,
        Matrix4& outMat, bool& outColMajor) -> bool
    {
      bool found = false;
      const auto& cbs = *stageCbs[stageIdx];
      for (uint32_t slot = 0; slot < D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT; ++slot) {
        const auto& cb = cbs[slot];
        if (cb.buffer == nullptr) continue;
        const auto mapped = cb.buffer->GetMappedSlice();
        const uint8_t* ptr = reinterpret_cast<const uint8_t*>(mapped.mapPtr);
        if (!ptr) continue;
        const size_t bufSize = cb.buffer->Desc()->ByteWidth;
        auto [base, end] = cbRange(cb);
        for (size_t off = base; off + 64 <= end; off += 16) {
          Matrix4 m = readCbMatrix(ptr, off, bufSize);
          int cls = classifyPerspective(m);
          if (cls == 0) continue;
          // Column-major-as-row (cls==2): transpose to row-major for scoring/use.
          const bool isCol = (cls == 2);
          Matrix4 normalized = isCol ? transpose(m) : m;
          float s = scorePerspective(normalized);
          if (s > outScore) {
            outSlot     = slot;
            outOff      = off;
            outScore    = s;
            outMat      = normalized;
            outColMajor = isCol;
            found       = true;
          }
        }
      }
      return found;
    };

    uint32_t projSlot   = m_projSlot;
    size_t   projOffset = m_projOffset;
    int      projStage  = m_projStage;

    // DX11_V240 TRANSFORM DIAGNOSTIC: the path tracer renders black on all GPUs because we extract
    // only the projection (view/world stay identity), so geometry collapses to object space and the
    // RT camera is wrong. Dump every distinct candidate 4x4 matrix in the VS/GS/etc cbuffers (each
    // stage+slot+offset logged once) so we can see where the game stores world / view / projection
    // and fix the extraction. Captures both the menu and the 3D-scene matrices as they first appear.
    {
      // DX11_V267_LOG_CLEANUP: this dump diagnosed the camera-extraction bugs
      // (V240..V260, all fixed). It cost a per-draw cbuffer scan and 64 log
      // lines every session for an issue that no longer exists. Now opt-in:
      // set DXVK_REMIX_MTXDUMP=1 when debugging a new game's matrices.
      static const bool s_mtxDumpEnabled = env::getEnvVar("DXVK_REMIX_MTXDUMP") == "1";
      static std::set<uint64_t> s_dumpedMatrixLocs;
      static uint32_t s_dumpLogged = 0;
      // DX11_V286_GAMEPLAY_MATRIX_DUMP: env-free burst armed by EndFrame when a
      // real gameplay scene frame has no resolved camera. Undeduped so the LIVE
      // per-frame values at camera cbuffer locations (e.g. slot 12 off 0 - is it
      // identity in gameplay?) are visible; the env path stays deduped-by-location.
      const uint32_t curFrameDump = m_context->m_device->getCurrentFrameId();
      const bool forcedDump = curFrameDump <= m_forceMatrixDumpUntilFrame
                           && m_forceMatrixDumpLines < 240;
      if ((s_mtxDumpEnabled && s_dumpLogged < 64) || forcedDump) {
        for (int si = 0; si < kNumStages; ++si) {
          const auto& cbsD = *stageCbs[si];
          for (uint32_t slot = 0; slot < D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT; ++slot) {
            const auto& cbD = cbsD[slot];
            if (cbD.buffer == nullptr) continue;
            const auto mappedD = cbD.buffer->GetMappedSlice();
            const uint8_t* ptrD = reinterpret_cast<const uint8_t*>(mappedD.mapPtr);
            if (!ptrD) continue;
            const size_t bufSizeD = cbD.buffer->Desc()->ByteWidth;
            auto [baseD, endD] = cbRange(cbD);
            for (size_t off = baseD; off + 64 <= endD; off += 16) {
              if (forcedDump && m_forceMatrixDumpLines >= 240) break;
              if (!forcedDump && s_dumpLogged >= 64) break;
              Matrix4 m = readCbMatrix(ptrD, off, bufSizeD);
              if (isIdentityExact(m)) continue;
              const bool rowAffine = std::abs(m[0][3]) < 0.01f && std::abs(m[1][3]) < 0.01f && std::abs(m[2][3]) < 0.01f && std::abs(m[3][3] - 1.0f) < 0.01f;
              const bool colAffine = std::abs(m[3][0]) < 0.01f && std::abs(m[3][1]) < 0.01f && std::abs(m[3][2]) < 0.01f && std::abs(m[3][3] - 1.0f) < 0.01f;
              const bool persp     = classifyPerspective(m) != 0;
              if (!rowAffine && !colAffine && !persp) continue;
              if (forcedDump) {
                ++m_forceMatrixDumpLines;
                Logger::info(str::format("[D3D11Rtx][gpdump] fid=", curFrameDump, " stage=", kStageNames[si], " slot=", slot, " off=", off,
                  (persp ? " PERSP" : ""), (rowAffine ? " rowAff" : ""), (colAffine ? " colAff" : ""),
                  " r0=", m[0][0], ",", m[0][1], ",", m[0][2], ",", m[0][3],
                  " r1=", m[1][0], ",", m[1][1], ",", m[1][2], ",", m[1][3],
                  " r2=", m[2][0], ",", m[2][1], ",", m[2][2], ",", m[2][3],
                  " r3=", m[3][0], ",", m[3][1], ",", m[3][2], ",", m[3][3]));
                continue;
              }
              const uint64_t key = (uint64_t(si) << 40) | (uint64_t(slot) << 32) | uint64_t(off);
              if (!s_dumpedMatrixLocs.insert(key).second) continue;
              ++s_dumpLogged;
              Logger::info(str::format("[D3D11Rtx][mtxdump] stage=", kStageNames[si], " slot=", slot, " off=", off,
                (persp ? " PERSP" : ""), (rowAffine ? " rowAff" : ""), (colAffine ? " colAff" : ""),
                " r0=", m[0][0], ",", m[0][1], ",", m[0][2], ",", m[0][3],
                " r1=", m[1][0], ",", m[1][1], ",", m[1][2], ",", m[1][3],
                " r2=", m[2][0], ",", m[2][1], ",", m[2][2], ",", m[2][3],
                " r3=", m[3][0], ",", m[3][1], ",", m[3][2], ",", m[3][3]));
            }
          }
        }
      }
    }

    // --- PROJECTION: engine camera knowledge ---
    // The bound VS's camera constants are known (RDEF names, or the engine's
    // documented register layout). A seeded projection that classifies as a
    // perspective matrix replaces whatever the heuristic scan latched onto -
    // shadow, reflection and previous-frame projections can outscore it.
    const CameraSeed* cameraSeed = ResolveCameraSeed();
    if (cameraSeed != nullptr && cameraSeed->slot < D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT
     && cameraSeed->offsets[size_t(D3D11CameraField::Proj)] >= 0) {
      const auto& seedCb = (*stageCbs[0])[cameraSeed->slot];
      const uint8_t* seedPtr = seedCb.buffer != nullptr
        ? reinterpret_cast<const uint8_t*>(seedCb.buffer->GetMappedSlice().mapPtr) : nullptr;
      if (seedPtr != nullptr) {
        const size_t seedOff = size_t(seedCb.constantOffset) * 16u
          + size_t(cameraSeed->offsets[size_t(D3D11CameraField::Proj)]);
        if (seedOff + 64u <= seedCb.buffer->Desc()->ByteWidth
         && (projSlot != cameraSeed->slot || projOffset != seedOff || projStage != 0)) {
          const int cls = classifyPerspective(readCbMatrix(seedPtr, seedOff, seedCb.buffer->Desc()->ByteWidth));
          if (cls > 0) {
            projSlot = m_projSlot = cameraSeed->slot;
            projOffset = m_projOffset = seedOff;
            projStage = m_projStage = 0;
            m_columnMajor = cls == 2;
            if (m_cameraSeedProjLocks++ < 8u)
              Logger::info(str::format("[D3D11Rtx][camera-seed] projection pinned: ", cameraSeed->source,
                " cb", cameraSeed->slot, " offset=", seedOff, cls == 2 ? " (column-major)" : ""));
          }
        }
      }
    }

    // --- PROJECTION: first-draw scan (cache miss) ---
    // Single pass across all stages â€” classifyPerspective handles both layouts.
    if (projSlot == UINT32_MAX) {
      float bestScore = 0.0f;
      Matrix4 bestMat;
      uint32_t bestSlot = UINT32_MAX;
      size_t bestOff = SIZE_MAX;
      int bestStage = -1;
      bool bestCol = false;

      for (int si = 0; si < kNumStages; ++si) {
        uint32_t ts = UINT32_MAX; size_t to = SIZE_MAX;
        float tsc = bestScore; Matrix4 tm; bool tc = false;
        if (scanStageForProj(si, ts, to, tsc, tm, tc) && tsc > bestScore) {
          bestScore = tsc;
          bestSlot = ts; bestOff = to; bestStage = si; bestMat = tm;
          bestCol = tc;
        }
      }

      if (bestSlot != UINT32_MAX) {
        projSlot   = bestSlot;
        projOffset = bestOff;
        projStage  = bestStage;
        m_projSlot   = bestSlot;
        m_projOffset = bestOff;
        m_projStage  = bestStage;
        m_columnMajor = bestCol;
      }
    }

    // DX11_V260_PRECISE_CAMERA: the projection exactly as the engine stored it
    // (convention-normalized, but BEFORE jitter strip and orientation
    // canonicalization). Compositions against engine-stored ViewProj blocks
    // must use these bytes: the engine multiplied with the original matrix,
    // so inverting/composing the canonicalized one is off by the jitter terms
    // and, worse, by a whole axis flip when canonicalization fired - a flipped
    // "view" still passes the rigid-body test and mirrors the camera.
    Matrix4 rawProjNormalized;
    bool haveRawProjNormalized = false;

    // --- PROJECTION: validate cached location, re-scan on stale ---
    if (projSlot != UINT32_MAX && projStage >= 0 && projStage < kNumStages) {
      const auto& cbs = *stageCbs[projStage];
      const auto& cb = cbs[projSlot];
      Matrix4 proj;
      bool valid = false;
      if (cb.buffer != nullptr) {
        const auto mapped = cb.buffer->GetMappedSlice();
        const uint8_t* ptr = reinterpret_cast<const uint8_t*>(mapped.mapPtr);
        if (ptr) {
          Matrix4 raw = readCbMatrix(ptr, projOffset, cb.buffer->Desc()->ByteWidth);
          int cls = classifyPerspective(raw);
          if (cls > 0) {
            proj = (cls == 2) ? transpose(raw) : raw;
            valid = true;
          }
        }
      }

      if (!valid && projSlot == m_projSlot && projStage == m_projStage) {
        // Cached location is stale (different pass). Re-scan all stages and
        // persist the winner back to the member cache â€” otherwise we would
        // redo this full multi-stage scan for every subsequent draw.
        projSlot = UINT32_MAX;
        float bestScore = 0.0f;
        bool bestCol = false;
        for (int si = 0; si < kNumStages; ++si) {
          uint32_t ts = UINT32_MAX; size_t to = SIZE_MAX;
          float tsc = bestScore; Matrix4 tm; bool tc = false;
          if (scanStageForProj(si, ts, to, tsc, tm, tc)) {
            projSlot = ts; projOffset = to; projStage = si;
            proj = tm; bestScore = tsc; bestCol = tc;
          }
        }

        if (projSlot != UINT32_MAX) {
          m_projSlot    = projSlot;
          m_projOffset  = projOffset;
          m_projStage   = projStage;
          m_columnMajor = bestCol;
        } else {
          // Nothing found â€” drop the stale cache so the next frame's
          // first-draw scan path runs instead of this re-scan path.
          m_projSlot   = UINT32_MAX;
          m_projOffset = SIZE_MAX;
          m_projStage  = -1;
        }
      }

      if (projSlot != UINT32_MAX) {
        rawProjNormalized = proj;
        haveRawProjNormalized = true;

        // The projection this draw was actually rasterized with, jitter
        // included, in the same canonical orientation as the stripped one.
        // Capture unprojects SV_Position with it: the stripped matrix would
        // offset every captured vertex by the frame's sub-pixel jitter.
        {
          bool jfx = false, jfy = false;
          m_drawJitteredProjection = canonicalizeProjectionOrientation(proj, &jfx, &jfy);
          m_drawJitteredProjectionValid = isFiniteMatrix(m_drawJitteredProjection);
        }

        // Strip TAA jitter â€” Remix does its own TAA.
        proj[2][0] = 0.0f;
        proj[2][1] = 0.0f;

        bool flippedX = false;
        bool flippedY = false;
        proj = canonicalizeProjectionOrientation(proj, &flippedX, &flippedY);
        projectionWasFlippedY = flippedY;
        if (flippedX || flippedY) {
          static uint32_t sProjectionCanonicalizeLogCount = 0;
          if (sProjectionCanonicalizeLogCount < 8) {
            ++sProjectionCanonicalizeLogCount;
            Logger::info(str::format(
              "[D3D11Rtx] Canonicalized projection orientation:",
              flippedX ? " flipX" : "",
              flippedY ? " flipY" : "",
              " stage=",
              kStageNames[projStage],
              " slot=",
              projSlot,
              " off=",
              projOffset));
          }
        }

        transforms.viewToProjection = proj;
      }
    }

    // --- VIEWPROJ-ONLY CAMERA (engine knowledge) ---
    // The seed names a ViewProj but no Projection: factor it. A camera-
    // relative ViewProj (translation removed) takes its eye from the seed.
    bool seededViewValid = false;
    Matrix4 seededView;
    if (cameraSeed != nullptr && cameraSeed->offsets[size_t(D3D11CameraField::Proj)] < 0
     && cameraSeed->slot < D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT) {
      const bool relative = cameraSeed->offsets[size_t(D3D11CameraField::ViewProj)] < 0;
      const int32_t vpField = cameraSeed->offsets[size_t(relative ? D3D11CameraField::RelViewProj
                                                                  : D3D11CameraField::ViewProj)];
      const auto& vpCb = (*stageCbs[0])[cameraSeed->slot];
      const uint8_t* vpPtr = vpCb.buffer != nullptr
        ? reinterpret_cast<const uint8_t*>(vpCb.buffer->GetMappedSlice().mapPtr) : nullptr;
      const size_t vpOff = size_t(vpCb.constantOffset) * 16u + size_t(std::max(vpField, 0));
      if (vpField >= 0 && vpPtr != nullptr && vpOff + 64u <= vpCb.buffer->Desc()->ByteWidth) {
        const Matrix4 raw = readCbMatrix(vpPtr, vpOff, vpCb.buffer->Desc()->ByteWidth);
        Matrix4 P, V;
        bool factored = factorViewProjection(raw, P, V);
        if (!factored)
          factored = factorViewProjection(transpose(raw), P, V);
        if (factored && relative) {
          // Absolute eye from the seed: V translation = -R * eye.
          factored = false;
          const int32_t eyeField = cameraSeed->offsets[size_t(D3D11CameraField::Eye)];
          const int32_t negField = cameraSeed->offsets[size_t(D3D11CameraField::NegEye)];
          const int32_t field = eyeField >= 0 ? eyeField : negField;
          if (field >= 0 && cameraSeed->eyeSlot < D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT) {
            const auto& eyeCb = (*stageCbs[0])[cameraSeed->eyeSlot];
            const uint8_t* eyePtr = eyeCb.buffer != nullptr
              ? reinterpret_cast<const uint8_t*>(eyeCb.buffer->GetMappedSlice().mapPtr) : nullptr;
            const size_t eyeOff = size_t(eyeCb.constantOffset) * 16u + size_t(field);
            if (eyePtr != nullptr && eyeOff + 12u <= eyeCb.buffer->Desc()->ByteWidth) {
              float e[3];
              std::memcpy(e, eyePtr + eyeOff, sizeof(e));
              const uint32_t eyeBytes = eyeCb.buffer->Desc()->ByteWidth;
              auto readPart = [&](D3D11CameraField part, float (&out)[3]) {
                const int32_t f = cameraSeed->offsets[size_t(part)];
                const size_t off = size_t(eyeCb.constantOffset) * 16u + size_t(std::max(f, 0));
                if (f < 0 || off + 12u > eyeBytes)
                  return false;
                std::memcpy(out, eyePtr + off, sizeof(out));
                return std::isfinite(out[0]) && std::isfinite(out[1]) && std::isfinite(out[2]);
              };
              // UE 5.4+ DoubleFloat: High + Low, in the base field's convention.
              float low[3];
              if (readPart(D3D11CameraField::EyeLow, low))
                for (uint32_t c = 0; c < 3; ++c) e[c] += low[c];
              if (eyeField < 0) { e[0] = -e[0]; e[1] = -e[1]; e[2] = -e[2]; }
              // UE 5.0-5.3: both relative forms drop the view tile offset.
              float tile[3];
              if (readPart(D3D11CameraField::EyeTile, tile))
                for (uint32_t c = 0; c < 3; ++c) e[c] += tile[c] * 2097152.0f;
              if (std::isfinite(e[0]) && std::isfinite(e[1]) && std::isfinite(e[2])) {
                for (uint32_t r = 0; r < 3; ++r)
                  V[3][r] = -(V[0][r] * e[0] + V[1][r] * e[1] + V[2][r] * e[2]);
                factored = true;
              }
            }
          }
        }
        if (factored) {
          rawProjNormalized = P;
          haveRawProjNormalized = true;
          m_drawJitteredProjection = P;
          m_drawJitteredProjectionValid = true;
          P[2][0] = 0.0f;  // strip TAA jitter, as for scanned projections
          P[2][1] = 0.0f;
          transforms.viewToProjection = P;
          seededView = V;
          seededViewValid = true;
          if (m_cameraSeedVpFactored++ < 8u)
            Logger::info(str::format("[D3D11Rtx][camera-seed] view-projection factored into P*V: ",
              cameraSeed->source, relative ? " (camera-relative, eye from seed)" : "",
              " P00=", P[0][0], " P11=", P[1][1]));
        }
      }
    }

    // --- FALLBACK PROJECTION ---
    // If no perspective matrix was found in any cbuffer, synthesize one from
    // the viewport. This keeps path tracing viable for games, emulators, and
    // engines that never expose a clean projection cbuffer. Large scene
    // viewports are accepted even when letterboxed or offset; only tiny helper
    // and HUD-style viewports are rejected here.
    //
    // Only synthesise a fallback projection when exactly one viewport is
    // bound.  Shadow cascade / cube face / split-screen passes bind multiple
    // viewports and must never drive the main camera.
    if (projSlot == UINT32_MAX && singleSceneViewport && !seededViewValid) {
      const auto& vp = m_context->m_state.rs.viewports[0];
      if (vp.Width > 0.0f && vp.Height > 0.0f) {
        float targetWidth = vp.Width;
        float targetHeight = vp.Height;
        bool haveStableSceneExtent = false;
        if (m_lastRemixViewportExtent.width > 0u && m_lastRemixViewportExtent.height > 0u) {
          targetWidth = float(m_lastRemixViewportExtent.width);
          targetHeight = float(m_lastRemixViewportExtent.height);
          haveStableSceneExtent = true;
        } else if (m_lastOutputExtent.width > 0u && m_lastOutputExtent.height > 0u) {
          targetWidth = float(m_lastOutputExtent.width);
          targetHeight = float(m_lastOutputExtent.height);
          haveStableSceneExtent = true;
        } else if (renderTargetWidth > 0.0f && renderTargetHeight > 0.0f) {
          targetWidth = renderTargetWidth;
          targetHeight = renderTargetHeight;
        }

        const float targetArea = std::max(targetWidth * targetHeight, 1.0f);
        const float viewportArea = vp.Width * vp.Height;
        const float coverage = std::min(viewportArea, targetArea) / std::max(viewportArea, targetArea);
        const float widthCoverage = targetWidth > 0.0f ? vp.Width / targetWidth : 0.0f;
        const float heightCoverage = targetHeight > 0.0f ? vp.Height / targetHeight : 0.0f;
        const float candidateAspect = vp.Width / vp.Height;
        const float viewportCenterX = vp.TopLeftX + vp.Width * 0.5f;
        const float viewportCenterY = vp.TopLeftY + vp.Height * 0.5f;
        const float targetCenterX = targetWidth * 0.5f;
        const float targetCenterY = targetHeight * 0.5f;
        const float normalizedCenterOffsetX = targetWidth > 0.0f
          ? std::abs(viewportCenterX - targetCenterX) / targetWidth
          : 0.0f;
        const float normalizedCenterOffsetY = targetHeight > 0.0f
          ? std::abs(viewportCenterY - targetCenterY) / targetHeight
          : 0.0f;
        const bool nearOrigin = std::abs(vp.TopLeftX) <= 4.0f && std::abs(vp.TopLeftY) <= 4.0f;
        const bool usableViewport = std::isfinite(vp.Width)
                                 && std::isfinite(vp.Height)
                                 && vp.Width >= 8.0f
                                 && vp.Height >= 8.0f;
        const bool plausibleSceneAspect = std::isfinite(candidateAspect)
                                       && candidateAspect >= 0.4f
                                       && candidateAspect <= 5.0f;
        const bool centeredViewport = normalizedCenterOffsetX <= 0.18f && normalizedCenterOffsetY <= 0.18f;

        // Aspect proximity to the output target is the strongest scene
        // signal we have: HUD strips, square shadow targets and cube faces
        // all have wildly different aspects from the output, while scene
        // viewports - scaled, anamorphic, or loading-screen sized - track it.
        const float targetAspectEarly = targetHeight > 0.0f ? targetWidth / targetHeight : 0.0f;
        const bool aspectNearTarget10 = targetAspectEarly > 0.0f
          && std::abs(candidateAspect - targetAspectEarly) <= 0.10f * targetAspectEarly;

        // A strip is small in one dimension AND aspect-divergent. A 31%
        // uniformly-scaled loading viewport is not a strip even though one
        // coverage dips below the floor (SR4 loads at 600x337 = 31%).
        const bool stripViewport = (widthCoverage < 0.35f || heightCoverage < 0.35f)
                                && !aspectNearTarget10;

        // Capped above: an oversized square depth target (2048x2048 against
        // 1080p) "covers most of the target" numerically but is not a scene.
        const bool coversMostOfTarget = widthCoverage >= 0.80f && heightCoverage >= 0.80f
                                     && widthCoverage <= 1.05f && heightCoverage <= 1.05f;
        const bool coversSceneLikeExtent = widthCoverage >= 0.55f && heightCoverage >= 0.55f;
        const bool coversMeaningfulArea = coverage >= 0.2f;

        // Internal render-scale detection. Many engines render the 3D scene
        // into a top-left-anchored sub-rectangle of the output target and
        // upscale during post (Saints Row IV uses a fixed 62.5%; dynamic
        // resolution systems roam 50-100%). The signature is a near-origin
        // viewport with UNIFORM width/height coverage whose aspect matches
        // the target aspect. These are scene viewports, not HUD strips, and
        // must drive the fallback projection even though their center is
        // offset from the target center (a 62.5% origin-anchored viewport
        // has a normalized center offset of 0.1875 - just past the centered
        // threshold). Shadow passes stay rejected: a square 1024x1024 pass
        // against a 16:10 target fails both the uniformity and the aspect
        // match.
        const float targetAspect = targetHeight > 0.0f ? targetWidth / targetHeight : 0.0f;
        const bool uniformScale = std::abs(widthCoverage - heightCoverage)
                               <= 0.05f * std::max(widthCoverage, heightCoverage);
        const bool aspectMatchesTarget = targetAspect > 0.0f
                                      && std::abs(candidateAspect - targetAspect) <= 0.05f * targetAspect;
        // Upper bound 2.05 admits supersampled scene targets (SSAA renders
        // at up to 2x per axis); uniformity + aspect match keep shadow
        // targets out regardless.
        const bool renderScaleViewport = nearOrigin
                                      && uniformScale
                                      && aspectMatchesTarget
                                      && widthCoverage >= 0.35f
                                      && widthCoverage <= 2.05f;

        // Sub-native render targets anchored at the origin: engines that
        // render at 55-85% of the output without centering (Saints Row IV's
        // fixed 62.5% among them). Uniformity is NOT required here, unlike
        // renderScaleViewport, so anamorphic internal targets also pass.
        const bool subNativeOriginViewport = coversSceneLikeExtent && nearOrigin
                                          && aspectNearTarget10;

        // Loading screens render small origin-anchored rects (SR4: 600x337,
        // 31% of output) after the scene extent has stabilized, which the
        // unstable-only nearOrigin path below cannot accept. Allow them when
        // the aspect still matches the output - that keeps square shadow
        // passes (aspect 1.0 against a widescreen target) rejected.
        const bool nearOriginSceneAspect =
             nearOrigin
          && widthCoverage >= kMinNearOriginCoverage
          && heightCoverage >= kMinNearOriginCoverage
          && targetAspect > 0.0f
          && std::abs(candidateAspect - targetAspect) <= 0.10f * targetAspect;

        const bool acceptViewportFallback =
             usableViewport
          && plausibleSceneAspect
          && !stripViewport
          && (
               coversMostOfTarget
            || renderScaleViewport
            || subNativeOriginViewport
            || nearOriginSceneAspect
            || (coversSceneLikeExtent && centeredViewport)
            || (!haveStableSceneExtent && (coversMeaningfulArea && centeredViewport))
            || (!haveStableSceneExtent && nearOrigin)
             );

        if (acceptViewportFallback) {
          const float aspect = candidateAspect > 0.0f ? candidateAspect : fallbackReferenceAspect;
          // DX11_V260: per-game tunable (rtx.fallbackCameraFovDegrees) - a
          // fixed guess can never match every engine, and a wrong FOV makes
          // the traced image zoom-mismatch the raster view.
          const float fovDegrees = std::max(20.0f, std::min(140.0f, fallbackCameraFovDegrees()));
          const float fovY   = fovDegrees * (3.14159265f / 180.0f);
          const float nearZ  = 0.1f;
          const float farZ   = 10000.0f;
          const float yScale = 1.0f / std::tan(fovY * 0.5f);
          const float xScale = yScale / aspect;
          const float Q      = farZ / (farZ - nearZ);
          transforms.viewToProjection = Matrix4(
            Vector4(xScale, 0.0f,   0.0f,         0.0f),
            Vector4(0.0f,   yScale, 0.0f,         0.0f),
            Vector4(0.0f,   0.0f,   Q,            1.0f),
            Vector4(0.0f,   0.0f,  -nearZ * Q,    0.0f));
          transforms.usedViewportFallbackProjection = true;
          static bool s_fallbackLogged = false;
          if (!s_fallbackLogged) {
            s_fallbackLogged = true;
            Logger::info(str::format(
              "[D3D11Rtx] No projection found in cbuffers â€” using viewport fallback (",
              "x=", vp.TopLeftX,
              " y=", vp.TopLeftY,
              " w=", vp.Width,
              " h=", vp.Height,
              " aspect=", aspect,
              " coverage=", coverage,
              " widthCov=", widthCoverage,
              " heightCov=", heightCoverage,
              " centered=", centeredViewport ? 1 : 0,
              " remixViewport=", m_lastRemixViewportExtent.width, "x", m_lastRemixViewportExtent.height,
              " output=", targetWidth, "x", targetHeight,
              ")"));
          }
        } else {
          static bool s_fallbackRejectedLogged = false;
          if (!s_fallbackRejectedLogged) {
            s_fallbackRejectedLogged = true;
            Logger::info(str::format(
              "[D3D11Rtx] No projection found in cbuffers â€” skipping viewport fallback for implausible scene viewport (",
              "x=", vp.TopLeftX,
              " y=", vp.TopLeftY,
              " w=", vp.Width,
              " h=", vp.Height,
              " coverage=", coverage,
              " widthCov=", widthCoverage,
              " heightCov=", heightCoverage,
              " centered=", centeredViewport ? 1 : 0,
              " remixViewport=", m_lastRemixViewportExtent.width, "x", m_lastRemixViewportExtent.height,
              " aspect=", candidateAspect,
              ")"));
          }
        }
      }
    }

    // --- VIEW MATRIX ---
    // Cached fast path: re-read from previously discovered location.
    // Only rescan when the cached location is invalid or doesn't contain
    // a view matrix anymore (shader change, different render pass).
    bool viewCacheHit = false;

    // DX11_V310_REJECT_DROPS_STALE_VIEW: remember the view as it stood BEFORE any
    // camera-relative path could assign to it. The rejection guard further down
    // clears the camera-relative FLAG but used to leave the camera-relative
    // MATRIX in transforms.worldToView, shipping "camera-relative matrix +
    // not-camera-relative flag" - a state that is wrong under either reading and
    // which pins geometry to the eye. Keep the original so the guard can honour
    // its own comment and actually reject the stale camera.
    const Matrix4 worldToViewBeforeCameraRelative = transforms.worldToView;

    if (m_viewSlot != UINT32_MAX && m_viewStage >= 0 && m_viewStage < kNumStages) {
      const auto& cb = (*stageCbs[m_viewStage])[m_viewSlot];
      if (cb.buffer != nullptr) {
        const auto mapped = cb.buffer->GetMappedSlice();
        const uint8_t* ptr = reinterpret_cast<const uint8_t*>(mapped.mapPtr);
        if (ptr) {
          Matrix4 c = readMatrixWithConvention(ptr, m_viewOffset, cb.buffer->Desc()->ByteWidth, m_viewColumnMajor);
          Matrix4 resolvedView;
          const bool cachedCameraRelativeIdentity =
               m_viewCameraRelative
            && m_viewConfirmed
            && m_viewStage == projStage
            && m_viewSlot == projSlot
            && m_viewOffset + 64 == projOffset
            && m_viewColumnMajor == m_columnMajor
            && isIdentityExact(c);
          if (cachedCameraRelativeIdentity) {
            transforms.worldToView = c;
            transforms.cameraRelativeView = true;
            viewCacheHit = true;
          } else if (resolveViewMatrixCandidate(c, resolvedView)) {
            // DX11_V260_PRECISE_CAMERA: a confirmed camera-to-world location
            // stores the inverse of the view - flip it back on every re-read.
            if (m_viewInverted) {
              const Matrix4 inv = inverseAffine(resolvedView);
              if (isFiniteMatrix(inv)) {
                transforms.worldToView = inv;
                viewCacheHit = true;
              }
            } else {
              transforms.worldToView = resolvedView;
              viewCacheHit = true;
            }
          }
        }
      }
    }

    // A view factored from the engine's ViewProj is exact; nothing to scan.
    if (seededViewValid) {
      transforms.worldToView = seededView;
      transforms.cameraRelativeView = false;
      viewCacheHit = true;
    }

    // --- VIEW CONFIRMATION AGAINST A STORED VIEWPROJ (DX11_V260_PRECISE_CAMERA) ---
    // The rigid-body test alone cannot tell the main camera view from shadow-
    // light views, mirror/reflection cameras, bone matrices, or a stored
    // camera-to-world (an inverse view is exactly as rigid). Engines routinely
    // upload View, Projection AND their ViewProj product in the same cbuffer,
    // which gives a decisive test: only the true view composed with the RAW
    // projection reproduces the stored ViewProj. On a match, lock the location
    // (m_viewConfirmed) so the heuristic scans can never displace it, and
    // remember whether the stored matrix needs inversion. Both composition
    // orders and both matrix conventions are tried, so this is layout-proof.
    if (!m_viewConfirmed && haveRawProjNormalized
     && projSlot != UINT32_MAX && projStage >= 0 && projStage < kNumStages) {
      const uint32_t curFrame = m_context->m_device->getCurrentFrameId();
      // STRICTLY once per frame. The first version ran on every draw for the
      // session's first 3600 frames; multiplied by in-game draw counts
      // (hundreds+) that ground gameplay to seconds per frame the moment the
      // player left the menu - "ray tracing freezes the game". One bounded
      // attempt per frame confirms within seconds on engines that store a
      // ViewProj and costs a fixed sliver on engines that never do.
      const bool mayAttempt = m_lastViewConfirmFrame != curFrame;
      const auto& cb = (*stageCbs[projStage])[projSlot];
      if (mayAttempt && cb.buffer != nullptr) {
        m_lastViewConfirmFrame = curFrame;
        const auto mapped = cb.buffer->GetMappedSlice();
        const uint8_t* ptr = reinterpret_cast<const uint8_t*>(mapped.mapPtr);
        if (ptr) {
          const size_t bufSize = cb.buffer->Desc()->ByteWidth;
          auto [cfBase, cfEndFull] = cbRange(cb);
          // Camera blocks live in the first few KB of a camera cbuffer;
          // never pay the emulator-sized deep-scan window here.
          const size_t cfEnd = std::min(cfEndFull, cfBase + size_t(8192));

          auto matricesNearlyEqual = [](const Matrix4& a, const Matrix4& b) -> bool {
            float maxRef = 1.0f;
            float maxDiff = 0.0f;
            for (int r = 0; r < 4; ++r) {
              for (int c = 0; c < 4; ++c) {
                if (!std::isfinite(a[r][c]) || !std::isfinite(b[r][c]))
                  return false;
                maxRef = std::max(maxRef, std::abs(b[r][c]));
                maxDiff = std::max(maxDiff, std::abs(a[r][c] - b[r][c]));
              }
            }
            return maxDiff <= 0.02f * maxRef;
          };

          // Pass 1: rigid candidates from this cbuffer, plus each candidate's
          // inverse (the stored matrix may be camera-to-world). Capped.
          struct ViewCandidate {
            Matrix4 view;
            size_t offset;
            bool columnMajor;
            bool inverted;
          };
          ViewCandidate cands[8];
          uint32_t candCount = 0;
          for (size_t off = cfBase; off + 64 <= cfEnd && candCount + 2 <= 8; off += 16) {
            if (off == projOffset) continue;
            Matrix4 resolvedView;
            bool resolvedColumnMajor = false;
            if (!resolveViewAt(ptr, off, bufSize, m_columnMajor, true, resolvedView, resolvedColumnMajor))
              continue;
            cands[candCount++] = { resolvedView, off, resolvedColumnMajor, false };
            const Matrix4 inv = inverseAffine(resolvedView);
            if (isFiniteMatrix(inv))
              cands[candCount++] = { inv, off, resolvedColumnMajor, true };
          }

          // Pass 2: ViewProj-shaped blocks (finite, non-affine, not a pure
          // projection, enough non-zero structure to be a real composition),
          // capped - then match candidates against ONLY those. This keeps the
          // multiply count fixed instead of offsets x candidates.
          struct VpBlock {
            Matrix4 m;
            size_t offset;
          };
          VpBlock vps[12];
          uint32_t vpCount = 0;
          for (size_t off = cfBase; off + 64 <= cfEnd && candCount > 0 && vpCount < 12; off += 16) {
            if (off == projOffset) continue;
            for (int convIdx = 0; convIdx < 2 && vpCount < 12; ++convIdx) {
              const Matrix4 stored = readMatrixWithConvention(
                ptr, off, bufSize, convIdx == 0 ? m_columnMajor : !m_columnMajor);
              if (!isFiniteMatrix(stored) || isAffineMatrix(stored) || classifyPerspective(stored) != 0)
                continue;
              uint32_t nonZero = 0;
              for (int r = 0; r < 4; ++r)
                for (int c = 0; c < 4; ++c)
                  nonZero += stored[r][c] != 0.0f ? 1u : 0u;
              if (nonZero < 8)
                continue;  // padding / vectors / mostly-zero garbage
              vps[vpCount++] = { stored, off };
            }
          }

          // Camera-relative engines (Fallout 4, Skyrim SE) upload a view with no
          // translation but a ViewProj that still carries the camera position,
          // so the full comparison above never matches and the location stayed
          // unconfirmed. Without translation a view and its transpose (the
          // camera-to-world rotation) are equally rigid, and the unconfirmed
          // scan kept the transposed one: the RT camera turned opposite to the
          // game and view-space captures swung around with the player. Only the
          // rotation-dependent part of the product decides orientation, so
          // compare just that: columns 0-2 of P*V, or rows 0-2 of V*P.
          //
          // The comparison is PER ELEMENT. A tolerance relative to the largest
          // element (the projection's depth terms, or a camera translation in
          // the thousands) accepted a view AND its transpose, and whichever the
          // scan reached first was locked - so the RT camera turned the right
          // way in one session and mirrored in the next. Both orientations are
          // now scored and the location is locked only when one wins clearly.
          auto elementError = [](const Matrix4& a, const Matrix4& b, int rowsOrCols, bool compareColumns) -> float {
            float worst = 0.0f;
            for (int i = 0; i < rowsOrCols; ++i) {
              for (int j = 0; j < 4; ++j) {
                const float av = compareColumns ? a[i][j] : a[j][i];
                const float bv = compareColumns ? b[i][j] : b[j][i];
                if (!std::isfinite(av) || !std::isfinite(bv))
                  return 1.0e30f;
                worst = std::max(worst, std::abs(av - bv) / (0.05f + std::abs(bv)));
              }
            }
            return worst;
          };
          (void)matricesNearlyEqual;

          // Best (lowest) error per candidate over every ViewProj block and
          // both composition orders; the rotation-only forms tolerate a
          // camera position present only in the ViewProj.
          float candError[8];
          bool candRotationOnly[8] = {};
          for (uint32_t ci = 0; ci < candCount; ++ci) {
            candError[ci] = 1.0e30f;
            for (uint32_t vi = 0; vi < vpCount; ++vi) {
              if (cands[ci].offset == vps[vi].offset) continue;
              const Matrix4& stored = vps[vi].m;
              const Matrix4 pv = rawProjNormalized * cands[ci].view;
              const Matrix4 vp = cands[ci].view * rawProjNormalized;
              const float full = std::min(elementError(pv, stored, 4, true), elementError(vp, stored, 4, true));
              const float rot = std::min(elementError(pv, stored, 3, true), elementError(vp, stored, 3, false));
              if (full < candError[ci]) { candError[ci] = full; candRotationOnly[ci] = false; }
              if (rot < candError[ci]) { candError[ci] = rot; candRotationOnly[ci] = true; }
            }
          }

          uint32_t bestCand = UINT32_MAX;
          for (uint32_t ci = 0; ci < candCount; ++ci)
            if (bestCand == UINT32_MAX || candError[ci] < candError[bestCand])
              bestCand = ci;
          // The same storage read the other way (view vs camera-to-world) must
          // lose clearly, or orientation is not established yet.
          float partnerError = 1.0e30f;
          if (bestCand != UINT32_MAX) {
            for (uint32_t ci = 0; ci < candCount; ++ci)
              if (ci != bestCand && cands[ci].offset == cands[bestCand].offset)
                partnerError = std::min(partnerError, candError[ci]);
          }
          const bool unambiguous = bestCand != UINT32_MAX
            && candError[bestCand] <= 0.05f
            && partnerError >= std::max(0.25f, 5.0f * candError[bestCand]);

          static uint32_t s_viewScoreLogs = 0;
          if (candCount > 0 && vpCount > 0 && s_viewScoreLogs < 6u
           && (unambiguous || curFrame > 600u)) {
            ++s_viewScoreLogs;
            std::string scores;
            for (uint32_t ci = 0; ci < candCount; ++ci)
              scores += str::format(" ", cands[ci].offset, cands[ci].inverted ? "i" : "", "=", candError[ci],
                                    candRotationOnly[ci] ? "(rot)" : "");
            Logger::info(str::format("[D3D11Rtx] View orientation scores:", scores,
              unambiguous ? " -> locked" : " -> ambiguous, not locked"));
          }

          bool locked = false;
          if (unambiguous) {
            {
                const uint32_t ci = bestCand;
                const size_t off = cands[ci].offset;
                const bool rotationMatch = candRotationOnly[ci];
                {
                  transforms.worldToView = cands[ci].view;
                  m_viewStage = projStage;
                  m_viewSlot = projSlot;
                  m_viewOffset = cands[ci].offset;
                  m_viewColumnMajor = cands[ci].columnMajor;
                  m_viewInverted = cands[ci].inverted;
                  m_viewConfirmed = true;
                  m_viewCameraRelative = false;
                  viewCacheHit = true;
                  locked = true;
                  static bool s_viewConfirmedLogged = false;
                  if (!s_viewConfirmedLogged) {
                    s_viewConfirmedLogged = true;
                    Logger::info(str::format(
                      "[D3D11Rtx] View matrix CONFIRMED against stored ViewProj: stage=",
                      kStageNames[projStage], " slot=", projSlot,
                      " viewOff=", cands[ci].offset, " vpOff=", off,
                      cands[ci].inverted ? " [stored as camera-to-world]" : "",
                      cands[ci].columnMajor ? " [column-major]" : " [row-major]",
                      rotationMatch ? " [rotation-only: camera-relative view]" : ""));
                  }
                }
              }
          }

          // Report once why confirmation failed, so a game that still cannot
          // confirm its view says which blocks were tried.
          static uint32_t s_viewConfirmMissLogs = 0;
          if (!locked && candCount > 0 && s_viewConfirmMissLogs < 2u && curFrame > 600u) {
            ++s_viewConfirmMissLogs;
            std::string candList, vpList;
            for (uint32_t ci = 0; ci < candCount; ++ci)
              candList += str::format(" ", cands[ci].offset, cands[ci].inverted ? "i" : "", cands[ci].columnMajor ? "c" : "r");
            for (uint32_t vi = 0; vi < vpCount; ++vi)
              vpList += str::format(" ", vps[vi].offset);
            Logger::info(str::format("[D3D11Rtx] View not confirmed: stage=", kStageNames[projStage],
              " slot=", projSlot, " projOff=", projOffset, " viewCandidates=[", candList, " ] viewProjBlocks=[", vpList, " ]"));
          }
        }
      }
    }

    // Full scan fallback â€” same logic as before, but caches the result.
    if (!viewCacheHit && projSlot != UINT32_MAX) {
      if (projStage >= 0 && projStage < kNumStages) {
        const auto& cb = (*stageCbs[projStage])[projSlot];
        if (cb.buffer != nullptr) {
          const auto mapped = cb.buffer->GetMappedSlice();
          const uint8_t* ptr = reinterpret_cast<const uint8_t*>(mapped.mapPtr);
          if (ptr) {
            const size_t bufSize = cb.buffer->Desc()->ByteWidth;
            if (projOffset >= 64) {
              Matrix4 c = readMatrix(ptr, projOffset - 64, bufSize);
              Matrix4 resolvedView;
              if (resolveViewMatrixCandidate(c, resolvedView)) {
                transforms.worldToView = resolvedView;
                m_viewStage = projStage; m_viewSlot = projSlot; m_viewOffset = projOffset - 64;
                m_viewColumnMajor = m_columnMajor;
              }
            }
            if (isIdentityExact(transforms.worldToView)) {
              auto [vBase, vEnd] = cbRange(cb);
              for (size_t off = vBase; off + 64 <= vEnd; off += 16) {
                if (off >= projOffset && off < projOffset + 64) continue;
                Matrix4 c = readMatrix(ptr, off, bufSize);
                Matrix4 resolvedView;
                if (resolveViewMatrixCandidate(c, resolvedView)) {
                  transforms.worldToView = resolvedView;
                  m_viewStage = projStage; m_viewSlot = projSlot; m_viewOffset = off;
                  m_viewColumnMajor = m_columnMajor;
                  break;
                }
              }
            }
          }
        }
      }

      // Cross-stage fallback: scan all stages' cbuffers for a view matrix.
      if (isIdentityExact(transforms.worldToView)) {
        for (int si = 0; si < kNumStages && isIdentityExact(transforms.worldToView); ++si) {
          const auto& cbs = *stageCbs[si];
          for (uint32_t slot = 0; slot < D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT; ++slot) {
            if (si == projStage && slot == projSlot) continue;
            const auto& cb = cbs[slot];
            if (cb.buffer == nullptr) continue;
            const auto mapped = cb.buffer->GetMappedSlice();
            const uint8_t* ptr = reinterpret_cast<const uint8_t*>(mapped.mapPtr);
            if (!ptr) continue;
            const size_t bufSize = cb.buffer->Desc()->ByteWidth;
            auto [csBase, csEnd] = cbRange(cb);
            for (size_t off = csBase; off + 64 <= csEnd; off += 16) {
              Matrix4 c = readMatrix(ptr, off, bufSize);
              Matrix4 resolvedView;
              if (resolveViewMatrixCandidate(c, resolvedView)) {
                transforms.worldToView = resolvedView;
                m_viewStage = si; m_viewSlot = slot; m_viewOffset = off;
                m_viewColumnMajor = m_columnMajor;
                break;
              }
            }
            if (!isIdentityExact(transforms.worldToView)) break;
          }
        }
      }

      // Convention fallback: if no view matrix was found, the column-major
      // detection may be wrong (ambiguous when near plane â‰ˆ 1). Retry with
      // the opposite convention, but only for the projection cbuffer.
      if (isIdentityExact(transforms.worldToView) && projStage >= 0 && projStage < kNumStages) {
        const auto& cb = (*stageCbs[projStage])[projSlot];
        if (cb.buffer != nullptr) {
          const auto mapped = cb.buffer->GetMappedSlice();
          const uint8_t* ptr = reinterpret_cast<const uint8_t*>(mapped.mapPtr);
          if (ptr) {
            const size_t bufSize = cb.buffer->Desc()->ByteWidth;
            auto [fbBase, fbEnd] = cbRange(cb);
            for (size_t off = fbBase; off + 64 <= fbEnd; off += 16) {
              if (off >= projOffset && off < projOffset + 64) continue;
              Matrix4 raw = readCbMatrix(ptr, off, bufSize);
              Matrix4 flipped = m_columnMajor ? raw : transpose(raw);
              Matrix4 resolvedView;
              if (resolveViewMatrixCandidate(flipped, resolvedView)) {
                transforms.worldToView = resolvedView;
                m_viewStage = projStage; m_viewSlot = projSlot; m_viewOffset = off;
                m_viewColumnMajor = !m_columnMajor;
                m_columnMajor = !m_columnMajor;
                break;
              }
            }
          }
        }
      }

      // Mixed-layout fallback: some engines compile one shader with row-major
      // matrices and another with column-major matrices, or pack camera data in
      // a different stage from projection. Retry both conventions across all
      // raster stages before giving up on the frame's view matrix.
      if (isIdentityExact(transforms.worldToView)) {
        for (int si = 0; si < kNumStages && isIdentityExact(transforms.worldToView); ++si) {
          const auto& cbs = *stageCbs[si];
          for (uint32_t slot = 0; slot < D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT; ++slot) {
            const auto& cb = cbs[slot];
            if (cb.buffer == nullptr) continue;
            const auto mapped = cb.buffer->GetMappedSlice();
            const uint8_t* ptr = reinterpret_cast<const uint8_t*>(mapped.mapPtr);
            if (!ptr) continue;
            const size_t bufSize = cb.buffer->Desc()->ByteWidth;
            auto [csBase, csEnd] = cbRange(cb);
            for (size_t off = csBase; off + 64 <= csEnd; off += 16) {
              if (si == projStage && slot == projSlot && off == projOffset) continue;
              Matrix4 resolvedView;
              bool resolvedColumnMajor = m_columnMajor;
              if (resolveViewAt(ptr, off, bufSize, m_columnMajor, true, resolvedView, resolvedColumnMajor)) {
                transforms.worldToView = resolvedView;
                m_viewStage = si; m_viewSlot = slot; m_viewOffset = off;
                m_viewColumnMajor = resolvedColumnMajor;

                static uint32_t sMixedViewLayoutLogCount = 0;
                if (resolvedColumnMajor != m_columnMajor && sMixedViewLayoutLogCount < 8) {
                  ++sMixedViewLayoutLogCount;
                  Logger::info(str::format(
                    "[D3D11Rtx] View matrix recovered with mixed row/column-major layout: stage=",
                    kStageNames[si],
                    " slot=",
                    slot,
                    " off=",
                    off));
                }
                break;
              }
            }
            if (!isIdentityExact(transforms.worldToView)) break;
          }
        }
      }
    }

    // When using fallback projection (projSlot == UINT32_MAX), still search
    // all stages for a view matrix so the camera position is correct.
    if (!viewCacheHit && projSlot == UINT32_MAX && isIdentityExact(transforms.worldToView)) {
      for (int si = 0; si < kNumStages && isIdentityExact(transforms.worldToView); ++si) {
        const auto& cbs = *stageCbs[si];
        for (uint32_t slot = 0; slot < D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT; ++slot) {
          const auto& cb = cbs[slot];
          if (cb.buffer == nullptr) continue;
          const auto mapped = cb.buffer->GetMappedSlice();
          const uint8_t* ptr = reinterpret_cast<const uint8_t*>(mapped.mapPtr);
          if (!ptr) continue;
          const size_t bufSize = cb.buffer->Desc()->ByteWidth;
          auto [csBase, csEnd] = cbRange(cb);
          for (size_t off = csBase; off + 64 <= csEnd; off += 16) {
            Matrix4 resolvedView;
            bool resolvedColumnMajor = m_columnMajor;
            if (resolveViewAt(ptr, off, bufSize, m_columnMajor, true, resolvedView, resolvedColumnMajor)) {
              transforms.worldToView = resolvedView;
              m_viewStage = si; m_viewSlot = slot; m_viewOffset = off;
              m_viewColumnMajor = resolvedColumnMajor;
              break;
            }
          }
          if (!isIdentityExact(transforms.worldToView)) break;
        }
      }
    }

    // --- VIEW MATRIX: full scan of the projection's own cbuffer ---
    // DX11_V256_VIEW_IN_PROJ_CBUFFER: engines commonly pack the whole camera
    // block [Proj | View | inverses | ...] into ONE cbuffer, with the view at
    // an arbitrary offset (Saints Row IV: proj at slot 2 off 0, column-major
    // view at off 352). The broad view scan above only runs when NO projection
    // was found, so such views were missed entirely (log: "view=NO") and the
    // RT camera sat at the origin. Scan every offset of the projection's
    // cbuffer, both matrix conventions, skipping the projection itself.
    if (isIdentityExact(transforms.worldToView)
     && projSlot != UINT32_MAX && projStage >= 0 && projStage < kNumStages) {
      const auto& cb = (*stageCbs[projStage])[projSlot];
      if (cb.buffer != nullptr) {
        const auto mapped = cb.buffer->GetMappedSlice();
        const uint8_t* ptr = reinterpret_cast<const uint8_t*>(mapped.mapPtr);
        if (ptr) {
          const size_t bufSize = cb.buffer->Desc()->ByteWidth;
          auto [scanBase, scanEnd] = cbRange(cb);
          for (size_t off = scanBase; off + 64 <= scanEnd; off += 16) {
            if (off == projOffset) continue;
            Matrix4 resolvedView;
            bool resolvedColumnMajor = false;
            if (resolveViewAt(ptr, off, bufSize, m_columnMajor, true, resolvedView, resolvedColumnMajor)) {
              transforms.worldToView = resolvedView;
              m_viewStage = projStage;
              m_viewSlot = projSlot;
              m_viewOffset = off;
              m_viewColumnMajor = resolvedColumnMajor;
              static bool s_projCbViewLogged = false;
              if (!s_projCbViewLogged) {
                s_projCbViewLogged = true;
                Logger::info(str::format("[D3D11Rtx] View matrix found in projection cbuffer: stage=",
                  kStageNames[projStage], " slot=", projSlot, " off=", off,
                  resolvedColumnMajor ? " [column-major]" : " [row-major]"));
              }
              break;
            }
          }
        }
      }
    }

    // --- VIEW MATRIX: ViewProj decomposition fallback ---
    // Many engines store a pre-multiplied ViewProj (= View * Proj) instead
    // of separate View and Projection matrices.  When we found a valid P but
    // no standalone view matrix, check: for each matrix M in cbuffers, does
    //   V_candidate = M * inverse(P)
    // yield a valid view?  If so, M is ViewProj and V_candidate is our view.
    if (isIdentityExact(transforms.worldToView) && projSlot != UINT32_MAX) {
      // DX11_V260_PRECISE_CAMERA: invert the projection AS THE ENGINE STORED
      // IT. The engine built its ViewProj with the original matrix; inverting
      // the jitter-stripped, orientation-canonicalized copy is off by the
      // jitter terms and - when canonicalization flipped an axis - produces a
      // mirrored "view" that still passes the rigid-body test.
      Matrix4 projInv = inverse(haveRawProjNormalized ? rawProjNormalized
                                                      : transforms.viewToProjection);
      // Sanity: inverse succeeded (non-degenerate projection).
      bool invOk = std::isfinite(projInv[0][0]) && std::isfinite(projInv[1][1])
                && std::isfinite(projInv[2][2]) && std::isfinite(projInv[3][3]);
      if (invOk) {
        for (int si = 0; si < kNumStages && isIdentityExact(transforms.worldToView); ++si) {
          const auto& cbs = *stageCbs[si];
          for (uint32_t slot = 0; slot < D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT; ++slot) {
            const auto& cb = cbs[slot];
            if (cb.buffer == nullptr) continue;
            const auto mapped = cb.buffer->GetMappedSlice();
            const uint8_t* ptr = reinterpret_cast<const uint8_t*>(mapped.mapPtr);
            if (!ptr) continue;
            const size_t bufSize = cb.buffer->Desc()->ByteWidth;
            auto [csBase, csEnd] = cbRange(cb);
            for (size_t off = csBase; off + 64 <= csEnd; off += 16) {
              if (si == projStage && slot == projSlot && off == projOffset) continue;
              const bool matrixColumnMajorOptions[] = { m_columnMajor, !m_columnMajor };
              for (bool matrixColumnMajor : matrixColumnMajorOptions) {
                Matrix4 M = readMatrixWithConvention(ptr, off, bufSize, matrixColumnMajor);
                if (isIdentityExact(M)) continue;

                const Matrix4 viewProjOrders[] = {
                  M * projInv,
                  projInv * M,
                };
                for (uint32_t order = 0; order < 2; ++order) {
                  Matrix4 resolvedView;
                  if (resolveViewMatrixCandidate(viewProjOrders[order], resolvedView)) {
                    transforms.worldToView = resolvedView;
                    m_viewStage = si; m_viewSlot = slot; m_viewOffset = off;
                    m_viewColumnMajor = matrixColumnMajor;
                    static bool s_vpLogged = false;
                    if (!s_vpLogged) {
                      s_vpLogged = true;
                      Logger::info(str::format(
                        "[D3D11Rtx] View derived from ViewProj decomposition: stage=",
                        kStageNames[si],
                        " slot=",
                        slot,
                        " off=",
                        off,
                        " order=",
                        order == 0 ? "ViewProj*InvProj" : "InvProj*ViewProj",
                        matrixColumnMajor != m_columnMajor ? " mixed-layout" : ""));
                    }
                    break;
                  }
                }
                if (!isIdentityExact(transforms.worldToView)) break;
              }
              if (!isIdentityExact(transforms.worldToView)) break;
            }
            if (!isIdentityExact(transforms.worldToView)) break;
          }
        }
      }
    }

    // DX11_V260_PRECISE_CAMERA: if a heuristic scan just cached a fresh view
    // location (viewCacheHit false but a view was found), it was a direct,
    // unconfirmed read - only the confirmation pass may set the inverted
    // flag, and a re-discovered location must re-earn confirmed status.
    if (!viewCacheHit && !isIdentityExact(transforms.worldToView)) {
      m_viewConfirmed = false;
      m_viewCameraRelative = false;
      m_viewInverted = false;
    }

    // DX11_V287_CAMERA_RELATIVE_VIEW: a rigid-matrix scan deliberately rejects
    // identity because identity normally means "view unresolved". Some modern
    // engines, however, upload a camera-relative frame block where object/world
    // coordinates already have the high-precision camera origin removed. Their
    // real CameraView is therefore identity whenever yaw/pitch are zero.
    //
    // Accept that identity only when the surrounding camera block proves it:
    //   [View][Projection][ViewProjection] ... [ViewInverse]
    //     [ViewProjectionInverse][ProjectionInverse]
    // Both the forward and inverse compositions must agree. This distinguishes
    // an intentional camera-relative identity view from padding, an unresolved
    // camera, and repeated projection constants. Skyrim SE's b12 PerFrame block
    // is one example, but the validation is based entirely on matrix coherence.
    bool cameraRelativeBlockValidated = false;
    if (haveRawProjNormalized && !seededViewValid
     && projSlot != UINT32_MAX
     && projStage >= 0 && projStage < kNumStages
     && projOffset >= 64) {
      const auto& cb = (*stageCbs[projStage])[projSlot];
      if (cb.buffer != nullptr) {
        const auto mapped = cb.buffer->GetMappedSlice();
        const uint8_t* ptr = reinterpret_cast<const uint8_t*>(mapped.mapPtr);
        const size_t bufSize = cb.buffer->Desc()->ByteWidth;
        const size_t viewOffset = projOffset - 64;
        const size_t viewProjOffset = projOffset + 64;
        const size_t viewInverseOffset = projOffset + 384;
        const size_t viewProjInverseOffset = projOffset + 448;
        const size_t projInverseOffset = projOffset + 512;

        if (ptr && projInverseOffset + 64 <= bufSize) {
          auto matricesNear = [](const Matrix4& a, const Matrix4& b, float relativeTolerance) -> bool {
            float maxRef = 1.0f;
            float maxDiff = 0.0f;
            for (int r = 0; r < 4; ++r) {
              for (int c = 0; c < 4; ++c) {
                if (!std::isfinite(a[r][c]) || !std::isfinite(b[r][c]))
                  return false;
                maxRef = std::max(maxRef, std::abs(b[r][c]));
                maxDiff = std::max(maxDiff, std::abs(a[r][c] - b[r][c]));
              }
            }
            return maxDiff <= relativeTolerance * maxRef;
          };
          auto nearIdentity = [&matricesNear](const Matrix4& m) -> bool {
            return matricesNear(m, Matrix4(), 1.0e-4f);
          };

          const Matrix4 storedView = readMatrixWithConvention(
            ptr, viewOffset, bufSize, m_columnMajor);
          const Matrix4 storedViewProj = readMatrixWithConvention(
            ptr, viewProjOffset, bufSize, m_columnMajor);
          const Matrix4 storedViewInverse = readMatrixWithConvention(
            ptr, viewInverseOffset, bufSize, m_columnMajor);
          const Matrix4 storedViewProjInverse = readMatrixWithConvention(
            ptr, viewProjInverseOffset, bufSize, m_columnMajor);
          const Matrix4 storedProjInverse = readMatrixWithConvention(
            ptr, projInverseOffset, bufSize, m_columnMajor);
          const Matrix4 calculatedProjInverse = inverse(rawProjNormalized);

          const bool identityViewPair = nearIdentity(storedView)
                                     && nearIdentity(storedViewInverse);
          const bool forwardCoherent =
               matricesNear(rawProjNormalized * storedView, storedViewProj, 0.002f)
            || matricesNear(storedView * rawProjNormalized, storedViewProj, 0.002f);
          const bool inverseCoherent =
               matricesNear(calculatedProjInverse, storedViewProjInverse, 0.002f)
            && matricesNear(calculatedProjInverse, storedProjInverse, 0.002f);

          if (identityViewPair && forwardCoherent && inverseCoherent) {
            cameraRelativeBlockValidated = true;
            transforms.worldToView = storedView;
            transforms.cameraRelativeView = true;
            m_viewStage = projStage;
            m_viewSlot = projSlot;
            m_viewOffset = viewOffset;
            m_viewColumnMajor = m_columnMajor;
            m_viewInverted = false;
            m_viewConfirmed = true;
            m_viewCameraRelative = true;
            viewCacheHit = true;

            static bool sCameraRelativeViewLogged = false;
            if (!sCameraRelativeViewLogged) {
              sCameraRelativeViewLogged = true;
              Logger::info(str::format(
                "[D3D11Rtx] Camera-relative identity view CONFIRMED from coherent camera block: stage=",
                kStageNames[projStage], " slot=", projSlot,
                " viewOff=", viewOffset, " projOff=", projOffset));
            }
          }
        }
      }
    }

    // A cached identity is only a fast-path candidate. If the surrounding
    // matrices stop agreeing (shader/layout/pass change), reject this draw and
    // force a fresh scan on the next one instead of injecting a stale camera.
    if (transforms.cameraRelativeView && !cameraRelativeBlockValidated) {
      transforms.cameraRelativeView = false;
      m_viewCameraRelative = false;
      m_viewConfirmed = false;
      viewCacheHit = false;

      // DX11_V310_REJECT_DROPS_STALE_VIEW: drop the stale camera-relative MATRIX
      // too, not just the flag.
      //
      // cameraRelativeBlockValidated is a per-draw local (reset at the top of
      // every ExtractTransforms), while m_viewCameraRelative persists across
      // draws. So any draw that does not re-validate the coherent camera block -
      // different shader, a pass that does not bind that cbuffer, projOffset < 64
      // - reached here with transforms.worldToView already holding the
      // camera-relative (zero-translation) view assigned above, and cleared only
      // the flag. Downstream then reads "vertices are in world space" alongside a
      // view that has no translation, which places the geometry on the eye.
      // Signature in field logs: cameraRelative=0 with worldToViewT=[-0,-0,-0].
      //
      // Restoring the pre-camera-relative value makes the rejection do what its
      // comment always claimed: reject the stale camera instead of injecting it.
      transforms.worldToView = worldToViewBeforeCameraRelative;

      static uint32_t sStaleCameraRelativeRejectLogCount = 0;
      if (sStaleCameraRelativeRejectLogCount < 8u) {
        ++sStaleCameraRelativeRejectLogCount;
        Logger::warn(
          "[D3D11Rtx] Rejected a stale camera-relative view: the coherent camera block did not "
          "re-validate for this draw, so both the flag and the camera-relative matrix are dropped "
          "(previously only the flag was, leaving geometry anchored to the eye).");
      }
    }

    // --- AXIS AUTO-DETECTION (camera-backed projection-derived) ---
    // Only learn handedness/Y-flip from draws where we recovered both a
    // plausible projection and either a non-identity view matrix or a camera-
    // relative identity view proven by the coherent frame-block checks above.
    // This avoids locking the session to helper, shadow, or other projections.
    const bool yFlipOverrideEnabled = projectionYFlipOverride();
    if (!m_projectionYFlipOverrideInitialized
     || yFlipOverrideEnabled != m_projectionYFlipOverrideWasEnabled) {
      m_projectionYFlipOverrideInitialized = true;
      m_projectionYFlipOverrideWasEnabled = yFlipOverrideEnabled;

      if (!yFlipOverrideEnabled) {
        // Returning to Auto must gather fresh evidence rather than restoring a
        // potentially stale decision made by a loading screen or prior scene.
        m_yFlipVotes = 0;
        m_yFlipSettled = false;
      }

      Logger::info(str::format(
        "[D3D11Rtx][axis] projection Y mode changed: ",
        yFlipOverrideEnabled
          ? (projectionYFlip() ? "manual flip" : "manual normal")
          : "automatic detection"));
    }

    if (yFlipOverrideEnabled) {
      RtCamera::correctProjectionYFlipObject().setDeferred(
        projectionYFlip(), RtxOptionLayer::getDerivedLayer());
    }

    if (projSlot != UINT32_MAX
     && (!isIdentityExact(transforms.worldToView) || transforms.cameraRelativeView)) {
      const bool canVote = (!yFlipOverrideEnabled && !m_yFlipSettled) || !m_lhSettled;

      if (canVote) {
        m_axisDetected = true;

        const Matrix4& projection = transforms.viewToProjection;

        // The projection handed to the camera was already canonicalized to
        // +Y above (canonicalizeProjectionOrientation). Voting
        // correctProjectionYFlip on from the raw sign negated [1][1] a second
        // time in RtCamera and rendered those games upside-down. Only the
        // explicit user override may set that option.
        if (!yFlipOverrideEnabled && !m_yFlipSettled) {
          m_yFlipVotes = projectionWasFlippedY ? 1 : -1;
          m_yFlipSettled = true;
        }

        DecomposeProjectionParams dpp;
        decomposeProjection(projection, dpp);
        if (std::isfinite(dpp.fov) && std::isfinite(dpp.aspectRatio)) {
          bool hasExplicitHandedness = false;
          bool isLeftHanded = dpp.isLHS;

          if (std::abs(std::abs(projection[2][3]) - 1.0f) < 0.02f) {
            hasExplicitHandedness = true;
            isLeftHanded = projection[2][3] > 0.0f;
          } else if (std::abs(std::abs(projection[3][2]) - 1.0f) < 0.02f) {
            hasExplicitHandedness = true;
            isLeftHanded = projection[3][2] > 0.0f;
          }

          m_lhVotes += isLeftHanded ? 1 : -1;
          if (!m_lhSettled && std::abs(m_lhVotes) >= kVoteThreshold) {
            m_lhSettled = true;
            const bool isLH = m_lhVotes > 0;
            RtxOptions::leftHandedCoordinateSystemObject().setDeferred(isLH, RtxOptionLayer::getDerivedLayer());

            static uint32_t sHandednessLogCount = 0;
            if (hasExplicitHandedness && sHandednessLogCount < 4) {
              ++sHandednessLogCount;
              Logger::info(str::format(
                "[D3D11Rtx] Handedness vote from projection structure: ",
                isLH ? "LH" : "RH",
                " m23=",
                projection[2][3],
                " m32=",
                projection[3][2]));
            }
          }
        }
      }
    }

    // --- Z-UP / Y-UP AUTO-DETECTION (view-matrix-derived) ---
    // In a Y-up world, the view matrix "up" column (col 1) has its largest
    // component in row 1 (Y). In a Z-up world, column 1's largest component
    // is in row 2 (Z). Vote on each valid view matrix and settle via threshold.
    if (!isIdentityExact(transforms.worldToView)) {
      if (!m_zUpSettled) {
        const float absY = std::abs(transforms.worldToView[1][1]);
        const float absZ = std::abs(transforms.worldToView[2][1]);
        // Only vote when there's a clear winner (avoid ambiguous 45Â° views)
        if (std::abs(absZ - absY) > 0.3f) {
          m_zUpVotes += (absZ > absY) ? 1 : -1;
          if (!m_zUpSettled && std::abs(m_zUpVotes) >= kVoteThreshold) {
            m_zUpSettled = true;
            const bool zUp = m_zUpVotes > 0;
            RtxOptions::zUpObject().setDeferred(zUp, RtxOptionLayer::getDerivedLayer());
          }
        }
      }

      // Log settled axis conventions once.
      if (m_zUpSettled && m_yFlipSettled && m_lhSettled && !m_axisLogged) {
        m_axisLogged = true;
        Logger::info(str::format("[D3D11Rtx] Axis detection settled: ",
          m_lhVotes > 0 ? "LH" : "RH",
          m_yFlipVotes > 0 ? " Y-flipped(canonicalized)" : "",
          m_zUpVotes > 0 ? " Z-up" : " Y-up",
          m_columnMajor ? " col-major" : " row-major",
          " (proj stage=", kStageNames[std::max(0, m_projStage)],
          " slot=", m_projSlot, " off=", m_projOffset, ")"));
      }
    }

    // Preserve the exact recovered view used by the application's VS. Smoothing
    // this matrix per draw pairs captured clip positions with a different camera,
    // displacing static geometry during motion and creating draw-order-dependent
    // silhouettes. Temporal estimation belongs only to unresolved camera paths.

    // --- WORLD MATRIX ---
    // Object-to-world transform, changes every draw call but usually lives
    // at a fixed (stage, slot, offset) within the same shader program.
    // Unlike the old code that only read offset 0, we scan the full cbuffer
    // to handle engines that pack [View|Proj|World] in a single CB.
    //
    // Candidate filter: affine, non-identity, not perspective, not the
    // already-identified view or projection, reasonable scale factors.
    // We compare against the found view by position (stage/slot/offset),
    // NOT by structural isViewMatrix() â€” the latter rejects unit-scale
    // world matrices which are the majority of game transforms.
    // DX11_V319_WORLD_SCAN_GIVE_UP: stop re-running a scan that this shader has
    // already proven it cannot satisfy.
    //
    // The world-matrix search deliberately caches no LOCATION - a remembered
    // (stage,slot,offset) can point at a bone, light or post matrix the moment
    // the game switches shaders, which is what the comment below guards against.
    // But that means the FULL search runs for every draw: each bound cbuffer,
    // every 16-byte offset up to the scan cap, an affine/shear/scale test per
    // candidate, and then a second sweep for a derived object-to-view matrix.
    // For a game that simply does not expose a world matrix, all of that runs
    // and fails on every single draw for the whole session.
    //
    // Measured (Mine Souls III): draw submission 4.6ms across 503 draws with
    // extract=3.0ms - about two thirds of the frame's submission cost - and
    // Skyrim reports "world=NO", i.e. the search never succeeds there either.
    //
    // What is remembered here is not a location but a property of the SHADER:
    // "this vertex shader's constant buffers contain no world matrix". That is
    // stable for as long as the shader is, and it re-arms automatically the
    // moment a different shader is bound, so it cannot cause the cross-shader
    // mismatch the location cache was removed for. Any successful find clears
    // the shader's miss count immediately.
    const void* worldScanShaderKey =
      static_cast<const void*>(m_context->m_state.vs.shader.ptr());
    const uint32_t worldScanMissLimit = RtxOptions::worldMatrixScanMaxMissesPerShader();
    bool worldScanSuppressed = false;

    if (worldScanMissLimit != 0u && worldScanShaderKey != nullptr) {
      const auto missIt = m_worldScanMissesByShader.find(worldScanShaderKey);
      worldScanSuppressed = missIt != m_worldScanMissesByShader.end()
                         && missIt->second >= worldScanMissLimit;
    }

    if (RtxOptions::useCBufferWorldMatrices() && !worldScanSuppressed) {
 auto isWorldCandidate = [&](const Matrix4& m) -> bool {
        if (isIdentityExact(m)) return false;
        if (classifyPerspective(m) != 0) return false;
        for (int row = 0; row < 4; ++row) {
          for (int col = 0; col < 4; ++col) {
            if (!std::isfinite(m[row][col])) return false;
          }
        }
        // Affine: last column = [0, 0, 0, 1]
        if (std::abs(m[3][3] - 1.0f) > 0.01f) return false;
        if (std::abs(m[0][3]) > 0.01f || std::abs(m[1][3]) > 0.01f || std::abs(m[2][3]) > 0.01f)
          return false;
        // Fix "geometry follows player": Reject the view matrix if it was found but not cached,
        // so it doesn't get misidentified as the world matrix.
        if (!isIdentityExact(transforms.worldToView)) {
          bool isView = true;
          for (int r = 0; r < 4 && isView; ++r) {
            for (int c = 0; c < 4; ++c) {
              if (std::abs(m[r][c] - transforms.worldToView[r][c]) > 1e-4f) {
                isView = false;
                break;
              }
            }
          }
          if (isView) return false;
        }

        // Affine: last column = [0, 0, 0, 1]
        if (std::abs(m[3][3] - 1.0f) > 0.01f) return false;
        if (std::abs(m[0][3]) > 0.01f || std::abs(m[1][3]) > 0.01f || std::abs(m[2][3]) > 0.01f)
          return false;
        // Reasonable scale: each column's squared length in [0.0001, 1e6]
        Vector3 normalizedAxes[3];
        for (int col = 0; col < 3; ++col) {
          float lenSq = m[0][col] * m[0][col] + m[1][col] * m[1][col] + m[2][col] * m[2][col];
          if (lenSq < 0.0001f || lenSq > 1e6f) return false;
          const float invLen = 1.0f / std::sqrt(lenSq);
          normalizedAxes[col] = Vector3(m[0][col] * invLen, m[1][col] * invLen, m[2][col] * invLen);
        }

        // World matrices are usually rotation * scale + translation. Reject heavily
        // sheared affine matrices so we don't accidentally pick unrelated cbuffer data.
        if (std::abs(dot(normalizedAxes[0], normalizedAxes[1])) > 0.35f
         || std::abs(dot(normalizedAxes[0], normalizedAxes[2])) > 0.35f
         || std::abs(dot(normalizedAxes[1], normalizedAxes[2])) > 0.35f) {
          return false;
        }

        return true;
      };

      auto isAffineObjectTransform = [&](const Matrix4& m) -> bool {
        if (isIdentityExact(m)) return false;
        if (classifyPerspective(m) != 0) return false;
        for (int row = 0; row < 4; ++row) {
          for (int col = 0; col < 4; ++col) {
            if (!std::isfinite(m[row][col])) return false;
          }
        }
        if (std::abs(m[3][3] - 1.0f) > 0.01f) return false;
        if (std::abs(m[0][3]) > 0.01f || std::abs(m[1][3]) > 0.01f || std::abs(m[2][3]) > 0.01f)
          return false;
        return true;
      };

      auto scoreWorldCandidate = [&](int stageIdx, uint32_t slot, size_t off, const Matrix4& candidate) -> float {
        float score = 0.0f;

        if (stageIdx == 0)
          score += 2.0f;
        if (stageIdx == projStage)
          score += 2.0f;
        if (slot == projSlot)
          score += 1.0f;
        if (projStage == 0 && projSlot != UINT32_MAX && slot == projSlot + 1)
          score += 4.0f;
        if (projOffset != SIZE_MAX) {
          const size_t distance = off > projOffset ? off - projOffset : projOffset - off;
          if (distance <= 128)
            score += 1.0f;
        }

        if (!isIdentityExact(transforms.worldToView)) {
          Matrix4 candidateObjectToView = transforms.worldToView * candidate;
          if (isAffineObjectTransform(candidateObjectToView))
            score += 2.0f;
        }

        const Vector3 translation(candidate[3][0], candidate[3][1], candidate[3][2]);
        const float translationLenSq = dot(translation, translation);
        if (translationLenSq > 1e-6f)
          score += 0.5f;

        // World + inverse pair: Unity's UnityPerDraw starts with
        // unity_ObjectToWorld followed by unity_WorldToObject (unity.md), and
        // other engines pack per-object matrices the same way. A matrix whose
        // next 64 bytes are its inverse is a proven world matrix, even in
        // stripped shaders whose cbuffer slots the compiler assigned.
        if (stageIdx >= 0 && stageIdx < kNumStages && slot < D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT) {
          const auto& cb = (*stageCbs[stageIdx])[slot];
          const uint8_t* ptr = cb.buffer != nullptr
            ? reinterpret_cast<const uint8_t*>(cb.buffer->GetMappedSlice().mapPtr) : nullptr;
          if (ptr != nullptr && off + 128u <= cb.buffer->Desc()->ByteWidth) {
            const Matrix4 next = readMatrix(ptr, off + 64u, cb.buffer->Desc()->ByteWidth);
            const Matrix4 product = candidate * next;
            float deviation = 0.0f;
            for (int c = 0; c < 4; ++c)
              for (int r = 0; r < 4; ++r)
                deviation += std::abs(product[c][r] - (c == r ? 1.0f : 0.0f));
            if (std::isfinite(deviation) && deviation < 1.0e-2f)
              score += 6.0f;
          }
        }

        return score;
      };

      bool found = false;
      float bestRawWorldScore = -1.0e30f;
      Matrix4 bestRawWorldCandidate;
      int bestRawWorldStage = -1;
      uint32_t bestRawWorldSlot = UINT32_MAX;
      size_t bestRawWorldOffset = SIZE_MAX;

      auto considerRawWorldCandidate = [&](int stageIdx, uint32_t slot, size_t off, const Matrix4& candidate) {
        const float score = scoreWorldCandidate(stageIdx, slot, off, candidate);
        if (score > bestRawWorldScore) {
          bestRawWorldScore = score;
          bestRawWorldCandidate = candidate;
          bestRawWorldStage = stageIdx;
          bestRawWorldSlot = slot;
          bestRawWorldOffset = off;
        }
      };

      auto scanWorldCb = [&](int stageIdx, uint32_t slot) -> bool {
        if (slot >= D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT) return false;
        const auto& cb = (*stageCbs[stageIdx])[slot];
        if (cb.buffer == nullptr) return false;
        const auto mapped = cb.buffer->GetMappedSlice();
        const uint8_t* ptr = reinterpret_cast<const uint8_t*>(mapped.mapPtr);
        if (!ptr) return false;
        const size_t bufSize = cb.buffer->Desc()->ByteWidth;
        auto [scanBase, scanEnd] = cbRange(cb);
        bool sawCandidate = false;
        for (size_t off = scanBase; off + 64 <= scanEnd; off += 16) {
          if (stageIdx == projStage && slot == projSlot && off == projOffset) continue;
          if (stageIdx == m_viewStage && slot == m_viewSlot && off == m_viewOffset) continue;
          Matrix4 candidate = readMatrix(ptr, off, bufSize);
          if (!isWorldCandidate(candidate)) continue;
          considerRawWorldCandidate(stageIdx, slot, off, candidate);
          sawCandidate = true;
        }
        return sawCandidate;
      };

      // Never reuse a world-matrix location across vertex shaders. A global
      // (stage,slot,offset) cache can point at a bone, light, shadow or post
      // matrix as soon as the game changes shaders, producing an invalid scene
      // in every debug view. Rescan and validate the active draw's bindings.
      const bool cachedWorldHit = false;

      if (!cachedWorldHit) {
        // Prefer commonly used locations first, but do not stop there.
        if (projSlot != UINT32_MAX && projStage >= 0)
          scanWorldCb(projStage, projSlot);

        if (projSlot != UINT32_MAX && projStage == 0
            && projSlot + 1 < D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT)
          scanWorldCb(0, projSlot + 1);

        float bestDerivedWorldScore = -1.0e30f;
        Matrix4 bestDerivedWorldCandidate;
        int bestDerivedWorldStage = -1;
        uint32_t bestDerivedWorldSlot = UINT32_MAX;
        size_t bestDerivedWorldOffset = SIZE_MAX;

        // Some engines provide object-to-view (model-view) matrices but no standalone
        // world matrix. Recover objectToWorld by stripping the current view transform.
        if (!isIdentityExact(transforms.worldToView)) {
          Matrix4 viewInv = inverse(transforms.worldToView);
          bool invOk = true;
          for (int row = 0; row < 4 && invOk; ++row) {
            for (int col = 0; col < 4; ++col) {
              if (!std::isfinite(viewInv[row][col])) {
                invOk = false;
                break;
              }
            }
          }

          if (invOk) {
            for (int si = 0; si < kNumStages; ++si) {
              const auto& cbs = *stageCbs[si];
              for (uint32_t slot = 0; slot < D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT; ++slot) {
                const auto& cb = cbs[slot];
                if (cb.buffer == nullptr) continue;
                const auto mapped = cb.buffer->GetMappedSlice();
                const uint8_t* ptr = reinterpret_cast<const uint8_t*>(mapped.mapPtr);
                if (!ptr) continue;
                const size_t bufSize = cb.buffer->Desc()->ByteWidth;
                auto [scanBase, scanEnd] = cbRange(cb);
                for (size_t off = scanBase; off + 64 <= scanEnd; off += 16) {
                  if (si == projStage && slot == projSlot && off == projOffset) continue;
                  if (si == m_viewStage && slot == m_viewSlot && off == m_viewOffset) continue;

                  Matrix4 candidateObjectToView = readMatrix(ptr, off, bufSize);
                  if (!isAffineObjectTransform(candidateObjectToView)) continue;

                  Matrix4 candidateObjectToWorld = viewInv * candidateObjectToView;
                  if (!isWorldCandidate(candidateObjectToWorld)) continue;

                  // No bonus: every affine location is also scored as a raw
                  // world matrix with identical location bonuses. A bonus here
                  // made V^-1*W beat a genuine world W, gluing objects to the eye.
                  const float score = scoreWorldCandidate(si, slot, off, candidateObjectToWorld);
                  if (score > bestDerivedWorldScore) {
                    bestDerivedWorldScore = score;
                    bestDerivedWorldCandidate = candidateObjectToWorld;
                    bestDerivedWorldStage = si;
                    bestDerivedWorldSlot = slot;
                    bestDerivedWorldOffset = off;
                  }
                }
              }
            }
          }
        }

        // Full scan: all VS cbuffers, then other stages.
        for (uint32_t s = 0; s < D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT; ++s) {
          if (projStage == 0 && s == projSlot) continue;
          if (projStage == 0 && projSlot != UINT32_MAX && s == projSlot + 1) continue;
          scanWorldCb(0, s);
        }
        for (int si = 1; si < kNumStages; ++si) {
          for (uint32_t s = 0; s < D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT; ++s) {
            if (si == projStage && s == projSlot) continue;
            scanWorldCb(si, s);
          }
        }

        static bool s_worldLogged = false;
        // Ties go to the raw world matrix; the derived (model-view) reading is
        // used only when it is strictly better or no raw world exists.
        if (bestDerivedWorldSlot != UINT32_MAX
         && (bestRawWorldSlot == UINT32_MAX || bestDerivedWorldScore > bestRawWorldScore)) {
          transforms.objectToWorld = bestDerivedWorldCandidate;
          found = true;

          static bool s_objectViewLogged = false;
          if (!s_objectViewLogged) {
            s_objectViewLogged = true;
            Logger::info(str::format("[D3D11Rtx] World matrix derived from object-to-view: stage=",
              kStageNames[bestDerivedWorldStage], " slot=", bestDerivedWorldSlot, " off=", bestDerivedWorldOffset));
          }
        } else if (bestRawWorldSlot != UINT32_MAX) {
          transforms.objectToWorld = bestRawWorldCandidate;
          found = true;

          if (!s_worldLogged) {
            s_worldLogged = true;
            Logger::info(str::format("[D3D11Rtx] World matrix found: stage=",
              kStageNames[bestRawWorldStage], " slot=", bestRawWorldSlot, " off=", bestRawWorldOffset));
          }
        }
      }

      // DX11_V319_WORLD_SCAN_GIVE_UP: record the outcome for this shader. A
      // success clears the count outright, so a shader that only sometimes binds
      // its world cbuffer is never suppressed on the strength of a few early
      // misses; only an unbroken run of failures reaches the limit.
      if (worldScanMissLimit != 0u && worldScanShaderKey != nullptr) {
        if (found) {
          m_worldScanMissesByShader.erase(worldScanShaderKey);
        } else {
          // Bound the map: shaders are finite per game, but a title that mints
          // them endlessly must not grow this without limit.
          constexpr size_t kMaxTrackedWorldScanShaders = 4096;
          if (m_worldScanMissesByShader.size() < kMaxTrackedWorldScanShaders
           || m_worldScanMissesByShader.count(worldScanShaderKey) != 0) {
            uint32_t& misses = m_worldScanMissesByShader[worldScanShaderKey];
            if (misses < worldScanMissLimit) {
              ++misses;
              if (misses == worldScanMissLimit) {
                static uint32_t sWorldScanGiveUpLogCount = 0;
                if (sWorldScanGiveUpLogCount < 8u) {
                  ++sWorldScanGiveUpLogCount;
                  Logger::info(str::format(
                    "[D3D11Rtx] No world matrix in this vertex shader's constant buffers after ",
                    worldScanMissLimit, " draws; skipping the per-draw search for it. "
                    "(rtx.dx11.worldMatrixScanMaxMissesPerShader=0 disables this.)"));
                }
              }
            }
          }
        }
      }
    }

    transforms.objectToView = transforms.objectToWorld;
    if (!isIdentityExact(transforms.worldToView))
      transforms.objectToView = transforms.worldToView * transforms.objectToWorld;

    // DX11_V291_SHADER_PROVEN_OBJECT_TO_VIEW: generic cbuffer scanning cannot
    // distinguish an object's transform from bone, light, reflection, and
    // post-process matrices. Prefer the exact four constant registers that the
    // bound vertex shader dp4s into SV_Position. Factoring object-to-clip by
    // the already validated projection yields object-to-view without relying
    // on engine names or a per-game layout.
    if (!isIdentityExact(transforms.viewToProjection)
     && m_context->m_state.vs.shader != nullptr) {
      const D3D11CommonShader* commonVs = m_context->m_state.vs.shader->GetCommonShader();
      const D3D11PositionTransformBinding* binding = commonVs != nullptr
        ? commonVs->GetPositionTransformBinding()
        : nullptr;
      if (binding != nullptr
       && binding->matrixCount >= 1u
       && binding->matrixCount <= binding->matrices.size()) {
        std::array<Matrix4, 2> shaderMatrices;
        auto readShaderMatrix = [&](const D3D11PositionTransformMatrixBinding& matrixBinding,
                                    Matrix4& shaderMatrix) {
          if (matrixBinding.constantBufferSlot
              >= D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT)
            return false;

          const auto& cb = m_context->m_state.vs.constantBuffers[matrixBinding.constantBufferSlot];
          // Row form reads registers as rows (a synthetic `mov w, 1` row
          // when only three exist); column form transposes mul/mad columns.
          Vector4 rows[4];
          if (!readBindingRows(matrixBinding, cb, rows))
            return false;
          for (uint32_t row = 0; row < 4; ++row)
            shaderMatrix[row] = rows[row];
          return isFiniteMatrix(shaderMatrix);
        };

        bool matricesReadable = true;
        for (uint32_t i = 0; i < binding->matrixCount; ++i)
          matricesReadable &= readShaderMatrix(binding->matrices[i], shaderMatrices[i]);

        if (matricesReadable) {
          // The binding is proven from DXBC dp4 dataflow: shader register i
          // holds mathematical row i, so the Matrix4 (columns) is the transpose
          // of the copied registers, and a two-stage chain applies A then B
          // (clip = B * A). This matches the capture path. Trying untransposed
          // and reversed orders and keeping whichever looked most affine picked
          // plausible but spatially wrong transforms.
          std::vector<Matrix4> shaderObjectToClipCandidates;
          if (binding->matrixCount == 1u) {
            shaderObjectToClipCandidates.push_back(transpose(shaderMatrices[0]));
          } else {
            shaderObjectToClipCandidates.push_back(
              transpose(shaderMatrices[1]) * transpose(shaderMatrices[0]));
          }

          if (!shaderObjectToClipCandidates.empty()) {
            // Factor against the exact projection Remix will use, including
            // orientation normalization and jitter removal. This guarantees
            // replacementProjection * objectToVirtualWorld reproduces the
            // game's clip transform.
            const Matrix4 replacementProjection = transforms.viewToProjection;
            const Matrix4 inverseProjection = inverse(replacementProjection);
            if (isFiniteMatrix(inverseProjection)) {
              std::vector<Matrix4> candidates;
              candidates.reserve(shaderObjectToClipCandidates.size());
              for (const Matrix4& shaderObjectToClip : shaderObjectToClipCandidates)
                candidates.push_back(inverseProjection * shaderObjectToClip);

              auto affineScore = [](const Matrix4& candidate) -> float {
                if (!isFiniteMatrix(candidate))
                  return -1.0e30f;
                // Remix's canonical matrices multiply column vectors: affine
                // transforms have a [0,0,0,1] final column.
                const float affineError =
                    std::abs(candidate[0][3])
                  + std::abs(candidate[1][3])
                  + std::abs(candidate[2][3])
                  + std::abs(candidate[3][3] - 1.0f);
                if (affineError > 0.02f)
                  return -1.0e30f;

                float score = 20.0f - affineError * 500.0f;
                Vector3 axes[3];
                for (uint32_t column = 0; column < 3; ++column) {
                  const float lengthSq =
                      candidate[0][column] * candidate[0][column]
                    + candidate[1][column] * candidate[1][column]
                    + candidate[2][column] * candidate[2][column];
                  if (!std::isfinite(lengthSq) || lengthSq < 1.0e-8f || lengthSq > 1.0e8f)
                    return -1.0e30f;
                  const float invLength = 1.0f / std::sqrt(lengthSq);
                  axes[column] = Vector3(
                    candidate[0][column] * invLength,
                    candidate[1][column] * invLength,
                    candidate[2][column] * invLength);
                }
                const float shear = std::abs(dot(axes[0], axes[1]))
                                  + std::abs(dot(axes[0], axes[2]))
                                  + std::abs(dot(axes[1], axes[2]));
                if (shear > 1.5f)
                  return -1.0e30f;
                return score - shear * 2.0f;
              };

              float bestScore = -1.0e30f;
              Matrix4 bestObjectToView;
              for (const Matrix4& candidate : candidates) {
                const float score = affineScore(candidate);
                if (score > bestScore) {
                  bestScore = score;
                  bestObjectToView = candidate;
                }
              }

                if (bestScore > -1.0e20f) {
                transforms.objectToView = bestObjectToView;
                // When a real world-space view is confirmed for this session,
                // keep it: objectToWorld = inverse(view) * provenModelView.
                // Collapsing to an identity camera here made the RT world move
                // with the eye (breaking free camera, world-space caches and
                // denoiser history) and mixed two world spaces in one frame,
                // since draws without a proven binding still carry the real view.
                const bool keepConfirmedWorldView = m_viewConfirmed
                  && !m_viewCameraRelative
                  && !transforms.cameraRelativeView
                  && !isIdentityExact(transforms.worldToView)
                  && isFiniteMatrix(transforms.worldToView);
                const Matrix4 viewToWorld = keepConfirmedWorldView
                  ? inverse(transforms.worldToView) : Matrix4();
                if (keepConfirmedWorldView && isFiniteMatrix(viewToWorld)) {
                  transforms.objectToWorld = viewToWorld * bestObjectToView;
                } else {
                  // Full DX11 replacement camera: the RT world is view space,
                  // its camera is identity, and every draw carries the complete
                  // shader-proven model-view transform.
                  transforms.objectToWorld = bestObjectToView;
                  transforms.worldToView = Matrix4();
                  transforms.cameraRelativeView = true;
                }
                // A synthesized projection is trustworthy once an exact
                // shader clip transform factors into a finite affine model-view.
                transforms.usedViewportFallbackProjection = false;

                static uint32_t sShaderTransformLogs = 0;
                if (sShaderTransformLogs < 16u) {
                  ++sShaderTransformLogs;
                  std::string bindingDescription;
                  for (uint32_t matrixIndex = 0;
                       matrixIndex < binding->matrixCount;
                       ++matrixIndex) {
                    const auto& matrixBinding = binding->matrices[matrixIndex];
                    if (!bindingDescription.empty())
                      bindingDescription += " -> ";
                    bindingDescription += str::format("cb=", matrixBinding.constantBufferSlot, " regs=");
                    for (uint32_t row = 0; row < 4; ++row) {
                      if (row != 0)
                        bindingDescription += ",";
                      const uint32_t shaderRegister = matrixBinding.constantRegisters[row];
                      bindingDescription += shaderRegister == UINT32_MAX
                        ? "affine-w"
                        : std::to_string(shaderRegister);
                    }
                  }
                  Logger::info(str::format(
                    "[D3D11Rtx] shader-proven object-to-view: vs=", commonVs->GetName(),
                    " matrices=", binding->matrixCount, " ", bindingDescription,
                    " [replacement view-space camera]"));
                }
              }
            }
          }
        }
      }
    }

    transforms.sanitize();

    // DX11_V285_OFFSCREEN_CAMERA_GATE: classify this draw's color target.
    // Offscreen pre-passes (water reflection, environment cubemaps, mirrors)
    // render with their OWN camera BEFORE the main scene each frame; because
    // RtCamera::update() is first-touch-wins per frame, their camera would
    // otherwise claim the Main camera every frame and the entire path-traced
    // scene renders from the wrong viewpoint. A target counts as the scene
    // target when its extent matches the swapchain output OR the established
    // Remix scene viewport (either exactly-ish, or same aspect at >=50% size -
    // dynamic-resolution/internal-scale main targets stay accepted). Nothing
    // is decided before the output extent is known (first frames: flag stays
    // false, previous behavior).
    if (renderTargetWidth > 0.0f && renderTargetHeight > 0.0f
     && m_lastOutputExtent.width > 0u && m_lastOutputExtent.height > 0u) {
      const float rtAspect = renderTargetWidth / renderTargetHeight;
      auto matchesSceneExtent = [&](VkExtent2D ref) -> bool {
        if (ref.width == 0u || ref.height == 0u)
          return false;
        const float refW = float(ref.width);
        const float refH = float(ref.height);
        if (std::abs(renderTargetWidth - refW) <= 0.15f * refW
         && std::abs(renderTargetHeight - refH) <= 0.15f * refH)
          return true;
        const float refAspect = refW / refH;
        return std::abs(rtAspect - refAspect) <= 0.05f * refAspect
            && renderTargetWidth >= 0.5f * refW;
      };
      transforms.offscreenRenderTarget =
        !matchesSceneExtent(m_lastOutputExtent)
        && !matchesSceneExtent(m_lastRemixViewportExtent);

      // DX11_V286_EMULATOR_INTERNAL_RENDER: emulators draw the guest scene
      // into an internal-resolution framebuffer (EFB/GS/GE target) whose
      // extent and aspect need not match the host window - the window only
      // receives a final blit. The extent gate above classified that ENTIRE
      // guest render as auxiliary, so Remix "saw nothing" in-game on
      // unauthenticated emulators. Inside known emulator processes, a
      // substantial internal target IS the scene: accept it, keeping only
      // genuinely small helper targets (shadow maps, EFB copies, LUTs) on
      // the auxiliary path. Exe-name gated; PC games are unaffected.
      bool emulatorInternalSceneTarget = false;
      if (transforms.offscreenRenderTarget
       && RtxOptions::Emulator::enableIntegration()
       && isKnownEmulatorHostProcess()) {
        const bool substantialTarget =
          renderTargetWidth >= 0.5f * float(m_lastOutputExtent.width)
          || renderTargetHeight >= 0.5f * float(m_lastOutputExtent.height);
        if (substantialTarget) {
          transforms.offscreenRenderTarget = false;
          emulatorInternalSceneTarget = true;
          static uint32_t sEmulatorInternalTargetLogs = 0;
          if (sEmulatorInternalTargetLogs++ < 8u) {
            Logger::info(str::format(
              "[D3D11Rtx] Emulator internal render target accepted as the scene: rt=",
              renderTargetWidth, "x", renderTargetHeight,
              " output=", m_lastOutputExtent.width, "x", m_lastOutputExtent.height));
          }
        }
      }

      // A target can alias the swap-chain-sized resource while using an
      // auxiliary square projection (Unreal scene captures, reflection probes,
      // editor thumbnails). Extent-only routing therefore misses the exact
      // wrong-viewport failure: a 1:1 projection (P11/P00 == 1) can replace a
      // 16:9 main camera and the traced output immediately turns black. Learn
      // that this title has produced an output-compatible projection before
      // enforcing the gate, so engines that intentionally apply aspect outside
      // their projection matrix are not rejected by assumption.
      const float projectionScaleX = std::abs(transforms.viewToProjection[0][0]);
      const float projectionScaleY = std::abs(transforms.viewToProjection[1][1]);
      const VkExtent2D aspectReference =
        m_lastRemixViewportExtent.width > 0u && m_lastRemixViewportExtent.height > 0u
          ? m_lastRemixViewportExtent : m_lastOutputExtent;
      if (projectionScaleX > 1.0e-5f && projectionScaleY > 1.0e-5f
       && aspectReference.width > 0u && aspectReference.height > 0u) {
        const float projectionAspect = projectionScaleY / projectionScaleX;
        const float outputAspect = float(aspectReference.width)
                                 / float(aspectReference.height);
        const float relativeAspectError =
          std::abs(projectionAspect - outputAspect) / outputAspect;
        if (relativeAspectError <= 0.08f) {
          m_hasSeenOutputAspectCompatibleProjection = true;
        } else if (m_hasSeenOutputAspectCompatibleProjection
                && relativeAspectError > 0.15f) {
          transforms.offscreenRenderTarget = true;
          static uint32_t sProjectionAspectGateLogs = 0;
          if (sProjectionAspectGateLogs++ < 16u) {
            Logger::info(str::format(
              "[D3D11Rtx] Classified auxiliary camera by projection/output aspect: projection=",
              projectionAspect, " output=", outputAspect,
              " rt=", renderTargetWidth, "x", renderTargetHeight));
          }
        }
      }

      // Reduced secondary views (half-resolution reflection and environment
      // passes, EGO's env-map pass) keep the output aspect and pass the 50%
      // extent test. Their camera is not the main one, so unprojected into the
      // main camera's space they become ghost geometry. A reduced target is
      // secondary when last frame's scene went into a clearly larger one;
      // a dynamic-resolution main target is itself that largest one.
      if (!transforms.offscreenRenderTarget && !m_abDisableEngineKnowledge
       && m_prevFrameSceneTargetWidth > 0u
       && renderTargetWidth < 0.75f * float(m_lastOutputExtent.width)
       && renderTargetWidth < 0.8f * float(m_prevFrameSceneTargetWidth)) {
        transforms.offscreenRenderTarget = true;
        ++m_submitRejectStats.secondaryViewSkipped;
      }
    }

    // Log camera discovery once.
    static bool s_cameraLogged = false;
    if (projSlot != UINT32_MAX && !s_cameraLogged) {
      s_cameraLogged = true;
      const auto& p = transforms.viewToProjection;
      const bool hasView  = !isIdentityExact(transforms.worldToView);
      const bool hasWorld = !isIdentityExact(transforms.objectToWorld);
      Logger::info(str::format(
        "[D3D11Rtx] Camera found: proj stage=", kStageNames[projStage],
        " slot=", projSlot, " off=", projOffset,
        " diag=(", p[0][0], ",", p[1][1], ",", p[2][2], ")",
        " m[2][3]=", p[2][3],
        m_columnMajor ? " [column-major]" : " [row-major]",
        " view=", transforms.cameraRelativeView ? "camera-relative" : (hasView ? "yes" : "NO"),
        " viewConfirmed=", m_viewConfirmed ? "yes" : "no",
        " world=", hasWorld ? "yes" : "NO"));
    }

    // DX11_V286_GAMEPLAY_MATRIX_DUMP: arm the env-free matrix-dump burst on the
    // exact failing condition, detectable right here with `this` available: a
    // real scene projection was found (m_hasSeenRealSceneProjection rules out
    // the menu/loading) but the view resolved to identity - i.e. the RT camera
    // would sit at the world origin. Setting the window to the next 2 frames
    // makes the [gpdump] block at the top of ExtractTransforms log the live
    // cbuffer matrices for those frames. A short burst budget + cooldown keep
    // it to a handful of bursts total, then it stays silent.
    {
      const uint32_t curFrame = m_context->m_device->getCurrentFrameId();
      const bool viewIsIdentity = isIdentityExact(transforms.worldToView);
      if (m_forceMatrixDumpBursts > 0
       && m_hasSeenRealSceneProjection
       && projSlot != UINT32_MAX
       && !transforms.usedViewportFallbackProjection
       && viewIsIdentity
       && !transforms.cameraRelativeView
       && curFrame > m_forceMatrixDumpUntilFrame + 90u) {
        m_forceMatrixDumpUntilFrame = curFrame + 2u;
        --m_forceMatrixDumpBursts;
        Logger::info(str::format(
          "[D3D11Rtx][gpdump] arming gameplay matrix dump at fid=", curFrame,
          " (projFound + identity view = origin camera); dumping next 2 frames"));
      }
    }

    // DX11_V319_WORLD_ANCHOR_CAMERA: supply the camera translation the game
    // never wrote into its view matrix.
    //
    // A real, non-identity view whose translation column is EXACTLY zero means
    // the engine renders camera-relative: it already subtracted the eye
    // position from every object transform on the CPU, so the only thing left
    // in the view matrix is the rotation. Remix then derives a camera position
    // of -R^T*0 = the world origin and anchors captured geometry with a pure
    // rotation, and the world slides past a camera that never moves.
    //
    // Re-introducing the solved position P on BOTH sides restores a real world
    // without changing anything the game rasterizes: worldToView gains -R*P,
    // objectToWorld gains +P, and objectToView = worldToView * objectToWorld is
    // therefore algebraically unchanged - which is why it is deliberately left
    // exactly as computed above rather than recomposed. That property also
    // makes this safe while P is still converging: a wrong P moves the whole
    // world and its camera together and cannot misalign an individual draw.
    m_cameraAnchorViewTranslationFree = false;
    if (RtxOptions::anchorCameraRelativeWorld()
     && !transforms.cameraRelativeView
     && !isIdentityExact(transforms.worldToView)) {
      // Exact zero, not "small": a real camera that happens to stand near the
      // world origin must not be mistaken for a camera-relative engine.
      constexpr float kZeroTranslationEpsilon = 1.0e-6f;
      const Matrix4 view = transforms.worldToView;
      m_cameraAnchorViewTranslationFree =
           std::abs(view[3][0]) < kZeroTranslationEpsilon
        && std::abs(view[3][1]) < kZeroTranslationEpsilon
        && std::abs(view[3][2]) < kZeroTranslationEpsilon;

      // Engine camera knowledge: the seed names the eye (or PreViewTranslation
      // style -eye) in the projection's cbuffer, so the exact eye is known
      // without a layout signature or movement votes.
      if (m_cameraAnchorViewTranslationFree && m_eyeOffset == SIZE_MAX && cameraSeed != nullptr
       && projStage == 0 && projSlot == cameraSeed->eyeSlot) {
        const int32_t eyeField = cameraSeed->offsets[size_t(D3D11CameraField::Eye)];
        const int32_t negField = cameraSeed->offsets[size_t(D3D11CameraField::NegEye)];
        const int32_t field = eyeField >= 0 ? eyeField : negField;
        const auto& seedEyeCb = (*stageCbs[0])[projSlot];
        const uint8_t* seedEyePtr = seedEyeCb.buffer != nullptr
          ? reinterpret_cast<const uint8_t*>(seedEyeCb.buffer->GetMappedSlice().mapPtr) : nullptr;
        const size_t seedEyeOff = size_t(seedEyeCb.constantOffset) * 16u + size_t(std::max(field, 0));
        // The lock follows one float3 per frame. A UE large-world eye is that
        // float3 only while the view tile is zero (within ~21 km of the
        // origin); a DoubleFloat low part is below a millimetre and ignored.
        bool tileZero = true;
        const int32_t tileField = cameraSeed->offsets[size_t(D3D11CameraField::EyeTile)];
        const size_t tileOff = size_t(seedEyeCb.constantOffset) * 16u + size_t(std::max(tileField, 0));
        if (tileField >= 0 && seedEyePtr != nullptr && tileOff + 12u <= seedEyeCb.buffer->Desc()->ByteWidth) {
          float t[3];
          std::memcpy(t, seedEyePtr + tileOff, sizeof(t));
          tileZero = t[0] == 0.0f && t[1] == 0.0f && t[2] == 0.0f;
        }
        if (tileZero && field >= 0 && seedEyePtr != nullptr && seedEyeOff + 12u <= seedEyeCb.buffer->Desc()->ByteWidth) {
          float e[3];
          std::memcpy(e, seedEyePtr + seedEyeOff, sizeof(e));
          const float sign = eyeField >= 0 ? 1.0f : -1.0f;
          const Vector3 exact(sign * e[0], sign * e[1], sign * e[2]);
          const bool worldEye = std::isfinite(exact.x) && std::isfinite(exact.y) && std::isfinite(exact.z)
            && std::abs(exact.x) + std::abs(exact.y) + std::abs(exact.z) > 1.0f;
          if (worldEye) {
            m_eyeOffset = seedEyeOff;
            m_eyeNegated = eyeField < 0;
            m_eyeSignatureChecked = true;
            m_eyeOriginShift = m_cameraTrackingState->worldAnchor.hasPosition()
              ? m_cameraTrackingState->worldAnchor.position() - exact
              : Vector3(0.0f) - exact;
            for (auto& cached : m_positionCaptureCache) {
              cached.second.lastCapturedFrame = ~0u;
              cached.second.hasCanonicalCapturedToWorld = false;
              cached.second.hasCapturedClipToPosition = false;
              cached.second.hasCapturedViewRotationToWorld = false;
            }
            Logger::info(str::format("[D3D11Rtx][world-anchor] exact eye from engine camera knowledge (",
              cameraSeed->source, ") at cb", projSlot, " offset ", seedEyeOff,
              m_eyeNegated ? " (stored negated)" : "", ": (", exact.x, ",", exact.y, ",", exact.z, ")"));
          }
        }
      }

      // Structural lock (reverse-engineered from Fallout 4's per-frame camera
      // buffer, notes in fo4-decomp/MODLOG.md): View at 0 (rotation only),
      // Projection at 64, ViewProj = P*V at 128, inverse View at 192, with the
      // world eye position as a float4 at 560 (and the previous frame's at
      // 576). A buffer that satisfies these exact relations has this layout,
      // whichever game uploads it, so the eye is known from the first frame
      // instead of being learned from movement.
      if (m_cameraAnchorViewTranslationFree && m_eyeOffset == SIZE_MAX && !m_eyeSignatureChecked
       && projStage >= 0 && projStage < kNumStages && projSlot != UINT32_MAX && projOffset == 64u) {
        const auto& sigCb = (*stageCbs[projStage])[projSlot];
        const uint8_t* sigPtr = sigCb.buffer != nullptr
          ? reinterpret_cast<const uint8_t*>(sigCb.buffer->GetMappedSlice().mapPtr) : nullptr;
        if (sigPtr != nullptr && sigCb.buffer->Desc()->ByteWidth >= 592u) {
          float V[16], P[16], VP[16], IV[16];
          std::memcpy(V, sigPtr + 0, sizeof(V));
          std::memcpy(P, sigPtr + 64, sizeof(P));
          std::memcpy(VP, sigPtr + 128, sizeof(VP));
          std::memcpy(IV, sigPtr + 192, sizeof(IV));
          // Registers are matrix rows: VP = P * V, and IV * V = I (rotation).
          bool matches = std::abs(V[0]) + std::abs(V[1]) + std::abs(V[2]) > 0.5f;
          for (int r = 0; r < 4 && matches; ++r) {
            for (int c = 0; c < 4 && matches; ++c) {
              float pv = 0.0f, iv = 0.0f;
              for (int k = 0; k < 4; ++k) {
                pv += P[r * 4 + k] * V[k * 4 + c];
                iv += IV[r * 4 + k] * V[k * 4 + c];
              }
              matches = std::abs(pv - VP[r * 4 + c]) <= 1.0e-3f * std::max(1.0f, std::abs(VP[r * 4 + c]));
              if (r < 3 && c < 3)
                matches = matches && std::abs(iv - (r == c ? 1.0f : 0.0f)) <= 1.0e-3f;
            }
          }
          float eye[4];
          std::memcpy(eye, sigPtr + 560, sizeof(eye));
          matches = matches && eye[3] == 0.0f
            && std::isfinite(eye[0]) && std::isfinite(eye[1]) && std::isfinite(eye[2]);
          // Menus fill the same buffer with a zero eye; wait for the world so
          // the RT origin lands at the player (keeps coordinates small).
          const bool worldEye = std::abs(eye[0]) + std::abs(eye[1]) + std::abs(eye[2]) > 1.0f;
          if (!worldEye)
            matches = false;
          // Decide once the buffer carries a real view and a world eye.
          if (worldEye && (std::abs(V[0] - 1.0f) > 1.0e-4f || std::abs(V[5] - 1.0f) > 1.0e-4f))
            m_eyeSignatureChecked = true;
          if (matches) {
            m_eyeOffset = 560u;
            const Vector3 exact(eye[0], eye[1], eye[2]);
            // Keep the RT world near the origin for precision, and continuous
            // with an estimate that may already be in use.
            m_eyeOriginShift = m_cameraTrackingState->worldAnchor.hasPosition()
              ? m_cameraTrackingState->worldAnchor.position() - exact
              : -exact;
            for (auto& cached : m_positionCaptureCache) {
              cached.second.lastCapturedFrame = ~0u;
              cached.second.hasCanonicalCapturedToWorld = false;
              cached.second.hasCapturedClipToPosition = false;
              cached.second.hasCapturedViewRotationToWorld = false;
            }
            Logger::info(str::format("[D3D11Rtx][world-anchor] camera constants match the Creation Engine "
              "per-frame layout (view/proj/viewproj/inverse view); exact eye position at offset 560: (",
              exact.x, ",", exact.y, ",", exact.z, ")"));
          }
        }
      }

      if (m_cameraAnchorViewTranslationFree
       && (m_cameraTrackingState->worldAnchor.hasPosition() || m_eyeOffset != SIZE_MAX)) {
        Vector3 cameraPosition = m_cameraTrackingState->worldAnchor.hasPosition()
          ? m_cameraTrackingState->worldAnchor.position() : Vector3(0.0f);

        // Exact eye position. The estimate above is solved from geometry read
        // back a frame or more late, so whenever the camera moves (turning in
        // third person orbits it; first person shifts the eye) meshes captured
        // on different frames were anchored with different errors: smearing,
        // doubled geometry and dark frames that settled only once the camera
        // stopped. Camera-relative engines still keep the eye position in
        // their camera constants for their own shaders. Find the vector that
        // moves exactly like the estimate, then use it, shifted once so the
        // world stays where it was.
        if (projStage >= 0 && projStage < kNumStages && projSlot != UINT32_MAX) {
          const auto& eyeCb = (*stageCbs[projStage])[projSlot];
          const uint8_t* eyePtr = eyeCb.buffer != nullptr
            ? reinterpret_cast<const uint8_t*>(eyeCb.buffer->GetMappedSlice().mapPtr) : nullptr;
          const size_t eyeBytes = eyePtr != nullptr ? std::min<size_t>(eyeCb.buffer->Desc()->ByteWidth, 4096u) : 0u;
          auto readVec = [&](size_t off) {
            float v[3];
            std::memcpy(v, eyePtr + off, sizeof(v));
            return Vector3(v[0], v[1], v[2]);
          };
          const uint32_t eyeFrame = m_context->m_device->getCurrentFrameId();

          if (m_eyeOffset != SIZE_MAX && eyePtr != nullptr && m_eyeOffset + 12u <= eyeBytes) {
            const Vector3 stored = readVec(m_eyeOffset);
            const Vector3 exact = m_eyeNegated ? Vector3(0.0f) - stored : stored;
            if (std::isfinite(exact.x) && std::isfinite(exact.y) && std::isfinite(exact.z))
              cameraPosition = exact + m_eyeOriginShift;
          } else if (eyePtr != nullptr && eyeFrame >= m_eyeLastSampleFrame + 30u) {
            // Discovery: compare each vec3's motion with the estimate's motion
            // over the same 30-frame window. Only real movement votes.
            const size_t count = eyeBytes / 16u;
            if (m_eyeSamples.size() != count) {
              m_eyeSamples.assign(count, Vector3(0.0f));
              m_eyeVotes.assign(count, 0u);
              m_eyeHaveSample = false;
            }
            const Vector3 estimate = m_cameraTrackingState->worldAnchor.position();
            if (m_eyeHaveSample) {
              const Vector3 dEstimate = estimate - m_eyeLastEstimate;
              const float moved = length(dEstimate);
              if (moved > 10.0f) {
                for (size_t k = 0; k < count; ++k) {
                  const Vector3 value = readVec(k * 16u);
                  const Vector3 dValue = value - m_eyeSamples[k];
                  // The estimate lags and under-travels (measured ~20% short
                  // in Fallout 4), so match direction and rough magnitude,
                  // not exact distance: the game's own value is the truth.
                  const float valueMoved = length(dValue);
                  const bool tracks = std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z)
                    && valueMoved > 10.0f
                    && dot(dValue, dEstimate) > 0.9f * valueMoved * moved
                    && valueMoved > 0.5f * moved && valueMoved < 2.0f * moved;
                  m_eyeVotes[k] = tracks ? m_eyeVotes[k] + 1u : 0u;
                  if (m_eyeVotes[k] >= 2u && m_eyeOffset == SIZE_MAX) {
                    m_eyeOffset = k * 16u;
                    m_eyeOriginShift = estimate - value;
                    // Every stored capture was anchored with the (lagging,
                    // biased) estimate. Mixing those with captures anchored by
                    // the exact position misplaces geometry, so discard their
                    // placement and let each mesh be captured again.
                    for (auto& cached : m_positionCaptureCache) {
                      cached.second.lastCapturedFrame = ~0u;
                      cached.second.hasCanonicalCapturedToWorld = false;
                      cached.second.hasCapturedClipToPosition = false;
                      cached.second.hasCapturedViewRotationToWorld = false;
                    }
                    Logger::info(str::format("[D3D11Rtx][world-anchor] exact eye position found in camera constants: stage=",
                      kStageNames[projStage], " slot=", projSlot, " offset=", m_eyeOffset,
                      " value=(", value.x, ",", value.y, ",", value.z, ") - replacing the geometry-solved estimate"));
                  }
                }
              }
            }
            for (size_t k = 0; k < count; ++k)
              m_eyeSamples[k] = readVec(k * 16u);
            m_eyeLastEstimate = estimate;
            m_eyeHaveSample = true;
            m_eyeLastSampleFrame = eyeFrame;
          }
        }

        // t = -R*P for this column-major layout: t_row = -sum_col V[col][row]*P_col.
        for (uint32_t row = 0; row < 3u; ++row) {
          transforms.worldToView[3][row] = -(view[0][row] * cameraPosition.x
                                           + view[1][row] * cameraPosition.y
                                           + view[2][row] * cameraPosition.z);
        }

        // The game's object placements are camera-relative for the same reason
        // the view has no translation, so they need the eye position added back
        // to land in the world the camera now lives in. This also covers the
        // common case where no world matrix could be proven at all and
        // objectToWorld is identity: the vertices are then already the
        // camera-relative world positions and +P is the whole transform.
        transforms.objectToWorld[3][0] += cameraPosition.x;
        transforms.objectToWorld[3][1] += cameraPosition.y;
        transforms.objectToWorld[3][2] += cameraPosition.z;

        // Temporary diagnostic: dump the camera constant buffer next to the
        // estimated eye position, to find where the engine keeps the exact one.
        {
          static uint32_t s_lastCbDumpFrame = 0;
          const uint32_t dumpFrame = m_context->m_device->getCurrentFrameId();
          if (dumpFrame >= s_lastCbDumpFrame + 300u && projStage >= 0 && projStage < kNumStages
           && projSlot != UINT32_MAX) {
            const auto& dumpCb = (*stageCbs[projStage])[projSlot];
            const uint8_t* dumpPtr = dumpCb.buffer != nullptr
              ? reinterpret_cast<const uint8_t*>(dumpCb.buffer->GetMappedSlice().mapPtr) : nullptr;
            if (dumpPtr != nullptr) {
              s_lastCbDumpFrame = dumpFrame;
              const size_t dumpBytes = std::min<size_t>(dumpCb.buffer->Desc()->ByteWidth, 1024u);
              std::string dump;
              for (size_t off = 0; off + 16 <= dumpBytes; off += 16) {
                float v[4];
                std::memcpy(v, dumpPtr + off, sizeof(v));
                dump += str::format(" ", off, ":", v[0], ",", v[1], ",", v[2], ",", v[3]);
              }
              Logger::info(str::format("[D3D11Rtx][camera-cb] frame=", dumpFrame,
                " estimate=(", cameraPosition.x, ",", cameraPosition.y, ",", cameraPosition.z, ")", dump));
            }
          }
        }

        static bool sCameraAnchorLogged = false;
        if (!sCameraAnchorLogged) {
          sCameraAnchorLogged = true;
          Logger::info(str::format(
            "[D3D11Rtx][world-anchor] camera-relative engine detected (view rotation "
            "with zero translation); anchoring the world with a camera position "
            "solved from captured geometry: pos=[",
            cameraPosition.x, ",", cameraPosition.y, ",", cameraPosition.z,
            "] meshes=", m_cameraTrackingState->worldAnchor.lastMatchedMeshes()));
        }
      }
    }

    return transforms;
  }

  Future<GeometryHashes> D3D11Rtx::ComputeGeometryHashes(
      const RasterGeometry& geo, uint32_t vertexCount,
      uint32_t hashStartVertex, uint32_t hashVertexCount) const {

    const void* posData = geo.positionBuffer.mapPtr(geo.positionBuffer.offsetFromSlice());
    const void* tcData  = geo.texcoordBuffer.defined()
                        ? geo.texcoordBuffer.mapPtr(geo.texcoordBuffer.offsetFromSlice())
                        : nullptr;
    const void* idxData = geo.indexBuffer.defined() ? geo.indexBuffer.mapPtr(0) : nullptr;

    // D3D11 dynamic buffers can be discarded (Map WRITE_DISCARD) at any time,
    // which recycles the physical slice backing our raw pointers.  Pin each
    // buffer with incRef + acquire(Read) so the allocator won't reuse the
    // memory while the hash worker is reading it.  The lambda releases them.
    DxvkBuffer* posBuf = geo.positionBuffer.buffer().ptr();
    DxvkBuffer* tcBuf  = geo.texcoordBuffer.defined() ? geo.texcoordBuffer.buffer().ptr() : nullptr;
    DxvkBuffer* idxBuf = geo.indexBuffer.defined()    ? geo.indexBuffer.buffer().ptr()    : nullptr;

    if (posBuf) { posBuf->incRef(); posBuf->acquire(DxvkAccess::Read); }
    if (tcBuf)  { tcBuf->incRef();  tcBuf->acquire(DxvkAccess::Read);  }
    if (idxBuf) { idxBuf->incRef(); idxBuf->acquire(DxvkAccess::Read); }

    const uint32_t posStride = geo.positionBuffer.stride();
    const uint32_t tcStride  = geo.texcoordBuffer.defined() ? geo.texcoordBuffer.stride() : 0u;
    const uint32_t idxStride = geo.indexBuffer.defined()    ? geo.indexBuffer.stride()    : 0u;
    const uint32_t indexType = static_cast<uint32_t>(geo.indexBuffer.indexType());
    const uint32_t topology  = static_cast<uint32_t>(geo.topology);

    const uint32_t posOffset = geo.positionBuffer.offsetFromSlice();

    // DX11_V308_REVERTED: do NOT fold geo.positionBuffer.offset() (the per-draw
    // slice offset) into the hashes below.
    //
    // It looked like the fix for USD captures exporting the scene merged into
    // one mesh - offsetFromSlice is only the attribute's position within a
    // vertex, so distinct objects suballocated from one DEFAULT-usage buffer did
    // share a position hash. But D3D11 dynamic buffers RENAME their backing
    // slice on every Map(WRITE_DISCARD), so the slice offset moves every frame
    // for exactly the buffers this engine uses most. Including it made the
    // vertex hash unstable frame to frame: BlasEntry lookups never matched,
    // "[RTX Geometry Identity] prevented material-only BLAS reuse" flooded the
    // log, every BLAS was rebuilt every frame, the helper-buffer pool overflowed
    // ("overflow budget exhausted") and the device was lost seconds later.
    //
    // Stability is the whole point of the content-cookie scheme below. Any
    // future fix for merged captures must first prove the buffer's slice is
    // stable across frames (static/immutable geometry) before keying on it.

    const XXH64_hash_t descHash   = hashGeometryDescriptor(geo.indexCount, vertexCount, indexType, topology);
    const XXH64_hash_t layoutHash = hashVertexLayout(geo);

    // Compute the safe byte range available for position and texcoord data.
    // Buffer pins guarantee the memory won't be recycled, but we must still
    // clamp to the actual buffer extent to avoid reading past the allocation.
    const size_t posLength = geo.positionBuffer.length();
    const size_t tcLength  = geo.texcoordBuffer.defined() ? geo.texcoordBuffer.length() : 0;
    const size_t idxLength = geo.indexBuffer.defined()    ? geo.indexBuffer.length()    : 0;

    // Content-derived identity for CPU-unreadable buffers (set at creation
    // from initial data). Stable across runs and GPU vendors, unlike the
    // pointer-based fallback below.
    const uint64_t posCookie = posBuf ? posBuf->contentCookie() : 0ull;

    auto future = m_pGeometryWorkers->Schedule([posData, tcData, idxData,
                                         posBuf, tcBuf, idxBuf,
                                         posStride, tcStride, idxStride,
                                         posLength, tcLength, idxLength,
                                         vertexCount, indexCount = geo.indexCount,
                                         posOffset, posCookie,
                                         hashStartVertex, hashVertexCount,
                                         descHash, layoutHash]() -> GeometryHashes {
      GeometryHashes hashes;
      hashes[HashComponents::GeometryDescriptor] = descHash;
      hashes[HashComponents::VertexLayout]       = layoutHash;

      if (posData && posStride > 0) {
        // Hash only the drawn subrange [hashStartVertex, hashStartVertex + hashVertexCount).
        // Clamp to actual buffer length to prevent OOB reads on shared/dynamic VBs.
        const size_t startByte = static_cast<size_t>(hashStartVertex) * posStride;
        size_t posBytes = static_cast<size_t>(hashVertexCount) * posStride;
        if (startByte >= posLength) {
          posBytes = 0;
        } else if (startByte + posBytes > posLength) {
          posBytes = posLength - startByte;
        }
        if (posBytes > 0) {
          const auto* posBase = static_cast<const uint8_t*>(posData) + startByte;
          hashes[HashComponents::VertexPosition] =
            XXH3_64bits_withSeed(posBase, posBytes, static_cast<XXH64_hash_t>(hashStartVertex));
        } else {
          hashes[HashComponents::VertexPosition] =
            XXH3_64bits(&posOffset, sizeof(posOffset));
        }

        if (tcData && tcStride > 0) {
          const size_t tcStartByte = static_cast<size_t>(hashStartVertex) * tcStride;
          size_t tcBytes = static_cast<size_t>(hashVertexCount) * tcStride;
          if (tcStartByte >= tcLength) {
            tcBytes = 0;
          } else if (tcStartByte + tcBytes > tcLength) {
            tcBytes = tcLength - tcStartByte;
          }
          if (tcBytes > 0) {
            const auto* tcBase = static_cast<const uint8_t*>(tcData) + tcStartByte;
            // Use a more robust hash for texture coordinates
            // Include vertex count to ensure different geometries with same TC data hash differently
            XXH64_hash_t tcHash = XXH3_64bits(tcBase, tcBytes);
            tcHash = XXH3_64bits_withSeed(&hashStartVertex, sizeof(hashStartVertex), tcHash);
            tcHash = XXH3_64bits_withSeed(&vertexCount, sizeof(vertexCount), tcHash);
            hashes[HashComponents::VertexTexcoord] = tcHash;
          }
        }
        if (idxData && idxStride > 0) {
           const size_t idxBytes = static_cast<size_t>(std::min(indexCount, kMaxHashedIndices)) * idxStride;
          // Use a more robust hash for indices
          // Include vertex count to ensure different geometries with same index data hash differently
          XXH64_hash_t idxHash = hashContiguousMemory(idxData, std::min(idxBytes, idxLength));
          idxHash = XXH3_64bits_withSeed(&vertexCount, sizeof(vertexCount), idxHash);
          hashes[HashComponents::Indices] = idxHash;
        }
      } else {
        // GPU-only buffer the CPU cannot read. Prefer the content cookie
        // (hashed from the buffer's initial data at creation): it is the
        // same value every run on every GPU vendor. The pointer-based
        // fallback below only triggers for buffers created without initial
        // data and filled purely on the GPU; its hashes are randomized by
        // ASLR each run and can collide when the allocator recycles
        // addresses - the "garbled hash" failure mode.
        if (posCookie != 0ull) {
          XXH64_hash_t posHash = XXH3_64bits(&posCookie, sizeof(posCookie));
          posHash = XXH3_64bits_withSeed(&posOffset, sizeof(posOffset), posHash);
          posHash = XXH3_64bits_withSeed(&vertexCount, sizeof(vertexCount), posHash);
          hashes[HashComponents::VertexPosition] = posHash;
        } else {
          XXH64_hash_t posHash = XXH3_64bits(&posBuf, sizeof(posBuf));
          posHash = XXH3_64bits_withSeed(&posOffset, sizeof(posOffset), posHash);
          posHash = XXH3_64bits_withSeed(&vertexCount, sizeof(vertexCount), posHash);
          hashes[HashComponents::VertexPosition] = posHash;
        }
      }

      hashes.precombine();

      // Release buffer pins â€” allow slice recycling again.
      if (posBuf) { posBuf->release(DxvkAccess::Read); posBuf->decRef(); }
      if (tcBuf)  { tcBuf->release(DxvkAccess::Read);  tcBuf->decRef();  }
      if (idxBuf) { idxBuf->release(DxvkAccess::Read); idxBuf->decRef(); }

      return hashes;
    });

    // If the worker queue was full, the lambda never runs â€” release pins now
    // to prevent a VRAM leak (incRef/acquire above would never be undone).
    if (!future.valid()) {
      if (posBuf) { posBuf->release(DxvkAccess::Read); posBuf->decRef(); }
      if (tcBuf)  { tcBuf->release(DxvkAccess::Read);  tcBuf->decRef();  }
      if (idxBuf) { idxBuf->release(DxvkAccess::Read); idxBuf->decRef(); }
    }

    return future;
  }

  void D3D11Rtx::SetLift2DPresentation(bool lifting, uint32_t source) {
    static std::mutex s_mutex;
    static bool s_sources[2] = {};
    static bool s_locked = false;
    std::lock_guard<std::mutex> guard(s_mutex);

    s_sources[std::min(source, 1u)] = lifting;
    lifting = s_sources[0] || s_sources[1];

    if (lifting == s_locked)
      return;

    s_locked = lifting;
    const RtxOptionLayer* derived = RtxOptionLayer::getDerivedLayer();

    if (lifting) {
      DxvkAutoExposure::enabledObject().setDeferred(false, derived);
      DxvkToneMapping::tonemappingEnabledObject().setDeferred(false, derived);
      DxvkToneMapping::exposureBiasObject().setDeferred(0.0f, derived);
      RtxOptions::tonemappingModeObject().setDeferred(TonemappingMode::Global, derived);
    } else {
      // The values come off the layer again: whatever was in effect before
      // (user config, presets) applies to the 3D game that follows.
      std::lock_guard<std::mutex> lock(RtxOptionImpl::getUpdateMutex());
      DxvkAutoExposure::enabledObject().disableLayerValue(derived);
      DxvkToneMapping::tonemappingEnabledObject().disableLayerValue(derived);
      DxvkToneMapping::exposureBiasObject().disableLayerValue(derived);
      RtxOptions::tonemappingModeObject().disableLayerValue(derived);
    }

    Logger::info(str::format("[D3D11Rtx][2d-lift] presentation ", lifting
      ? "held neutral (auto exposure off, EV 0, no tone curve)" : "released"));
  }

  Rc<DxvkImageView> D3D11Rtx::ToGreyAlbedoView(const Rc<DxvkImageView>& view) const {
    if (view == nullptr)
      return nullptr;

    VkComponentMapping swizzle;
    switch (view->info().format) {
      case VK_FORMAT_R8_UNORM:
      case VK_FORMAT_R8_SRGB:
      case VK_FORMAT_R16_UNORM:
      case VK_FORMAT_R16_SFLOAT:
      case VK_FORMAT_BC4_UNORM_BLOCK:
        swizzle = { VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_ONE };
        break;
      case VK_FORMAT_R8G8_UNORM:
      case VK_FORMAT_R8G8_SRGB:
        swizzle = { VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_G };
        break;
      default:
        return nullptr;
    }

    const uint32_t frame = m_context->m_device->getCurrentFrameId();
    constexpr uint32_t kUnusedFrames = 120;

    if (frame - m_greyAlbedoPruneFrame >= kUnusedFrames) {
      m_greyAlbedoPruneFrame = frame;
      for (auto it = m_greyAlbedoViews.begin(); it != m_greyAlbedoViews.end(); ) {
        if (frame - it->second.lastFrame >= kUnusedFrames)
          it = m_greyAlbedoViews.erase(it);
        else
          ++it;
      }
    }

    auto& entry = m_greyAlbedoViews[view.ptr()];
    if (entry.view == nullptr) {
      DxvkImageViewCreateInfo info = view->info();
      info.swizzle = swizzle;
      entry.source = view;
      entry.view = m_context->m_device->createImageView(view->image(), info);
    }
    entry.lastFrame = frame;
    return entry.view;
  }

  void D3D11Rtx::FillMaterialData(
      LegacyMaterialData& mat,
      XXH64_hash_t primaryTextureHashOverride) const {
    ScopedCpuProfileZoneN("D3D11Rtx::FillMaterialData");
    ScopedPhaseTimer phaseTimer(m_framePhaseMaterialNs);

    const auto& ps = m_context->m_state.ps;
    const D3D11CommonShader* commonPs = ps.shader != nullptr
      ? ps.shader->GetCommonShader()
      : nullptr;
    const auto& vs = m_context->m_state.vs;
    const D3D11CommonShader* commonVs = vs.shader != nullptr
      ? vs.shader->GetCommonShader()
      : nullptr;
    const bool hasCompleteSampledResourceProfile = commonPs != nullptr
      && commonPs->HasCompleteSampledResourceProfile();
    uint32_t textureID = 0;

    static uint32_t s_logCount = 0;
    // Log the first draws, plus the first draw of each distinct pixel shader:
    // the first-10-draws window only ever covered menu quads, never world
    // materials, so texture-selection failures in the world were invisible.
    static std::unordered_set<const void*> s_loggedMaterialShaders;
    bool firstDrawForShader = false;
    if (commonPs != nullptr && s_loggedMaterialShaders.size() < 200u)
      firstDrawForShader = s_loggedMaterialShaders.insert(commonPs).second;
    const bool doLog = (s_logCount < 10) || firstDrawForShader;

    auto isColorBlockCompressed = [](DXGI_FORMAT fmt) -> bool {
      return (fmt >= DXGI_FORMAT_BC1_TYPELESS && fmt <= DXGI_FORMAT_BC1_UNORM_SRGB)
          || (fmt >= DXGI_FORMAT_BC2_TYPELESS && fmt <= DXGI_FORMAT_BC2_UNORM_SRGB)
          || (fmt >= DXGI_FORMAT_BC3_TYPELESS && fmt <= DXGI_FORMAT_BC3_UNORM_SRGB)
          || (fmt >= DXGI_FORMAT_BC7_TYPELESS && fmt <= DXGI_FORMAT_BC7_UNORM_SRGB);
    };

    auto isDataBlockCompressed = [](DXGI_FORMAT fmt) -> bool {
      return (fmt >= DXGI_FORMAT_BC4_TYPELESS && fmt <= DXGI_FORMAT_BC4_SNORM)
          || (fmt >= DXGI_FORMAT_BC5_TYPELESS && fmt <= DXGI_FORMAT_BC5_SNORM)
          || (fmt >= DXGI_FORMAT_BC6H_TYPELESS && fmt <= DXGI_FORMAT_BC6H_SF16);
    };

    auto isBlockCompressed = [&](DXGI_FORMAT fmt) -> bool {
      return isColorBlockCompressed(fmt) || isDataBlockCompressed(fmt);
    };

    auto isLikelyAlbedoFormat = [&](DXGI_FORMAT fmt) -> bool {
      if (isColorBlockCompressed(fmt))
        return true;

      switch (fmt) {
        // Note: A8_UNORM is deliberately absent. Alpha-only textures are
        // font/UI atlases, not albedo; treating them as albedo let a
        // 2880x1088 glyph atlas win material selection and tile glyph
        // noise across world geometry whenever every other candidate was
        // rejected (observed in Sunset Overdrive).
        case DXGI_FORMAT_R8_UNORM:
        case DXGI_FORMAT_R8G8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        case DXGI_FORMAT_B8G8R8X8_UNORM:
        case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
        case DXGI_FORMAT_B5G6R5_UNORM:
        case DXGI_FORMAT_B5G5R5A1_UNORM:
        case DXGI_FORMAT_B4G4R4A4_UNORM:
          return true;
        default:
          return false;
      }
    };

    auto isStrongAlbedoFormat = [&](DXGI_FORMAT fmt) -> bool {
      if (isColorBlockCompressed(fmt))
        return true;

      switch (fmt) {
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        case DXGI_FORMAT_B8G8R8X8_UNORM:
        case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
        case DXGI_FORMAT_B5G6R5_UNORM:
        case DXGI_FORMAT_B5G5R5A1_UNORM:
        case DXGI_FORMAT_B4G4R4A4_UNORM:
          return true;
        default:
          return false;
      }
    };

    // DX11_V298_SRGB_ALBEDO_DISCRIMINATOR: an SRGB shader-resource view is
    // authored color content by definition - engines gamma-correct albedo and
    // never normal/mask/data maps. Unreal titles bind linear-UNORM normal maps
    // (uncompressed R8G8B8A8 or BC5) alongside SRGB albedo in the same draw;
    // without this signal the normal map could outscore the albedo ("normal
    // maps take over"). Games without SRGB views are unaffected.
    auto isSrgbFormat = [](DXGI_FORMAT fmt) -> bool {
      switch (fmt) {
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
        case DXGI_FORMAT_BC1_UNORM_SRGB:
        case DXGI_FORMAT_BC2_UNORM_SRGB:
        case DXGI_FORMAT_BC3_UNORM_SRGB:
        case DXGI_FORMAT_BC7_UNORM_SRGB:
          return true;
        default:
          return false;
      }
    };

    auto isLikelyDataOrSceneColorFormat = [&](DXGI_FORMAT fmt) -> bool {
      if (isDataBlockCompressed(fmt))
        return true;

      switch (fmt) {
        case DXGI_FORMAT_R10G10B10A2_UNORM:
        case DXGI_FORMAT_R10G10B10A2_UINT:
        case DXGI_FORMAT_R11G11B10_FLOAT:
        case DXGI_FORMAT_R16_FLOAT:
        case DXGI_FORMAT_R16G16_FLOAT:
        case DXGI_FORMAT_R16G16B16A16_FLOAT:
        case DXGI_FORMAT_R32_FLOAT:
        case DXGI_FORMAT_R32G32_FLOAT:
        case DXGI_FORMAT_R32G32B32_FLOAT:
        case DXGI_FORMAT_R32G32B32A32_FLOAT:
        case DXGI_FORMAT_R8_UINT:
        case DXGI_FORMAT_R8_SINT:
        case DXGI_FORMAT_R8G8_UINT:
        case DXGI_FORMAT_R8G8_SINT:
        case DXGI_FORMAT_R8G8B8A8_UINT:
        case DXGI_FORMAT_R8G8B8A8_SINT:
        case DXGI_FORMAT_R16_UINT:
        case DXGI_FORMAT_R16_SINT:
        case DXGI_FORMAT_R16G16_UINT:
        case DXGI_FORMAT_R16G16_SINT:
        case DXGI_FORMAT_R16G16B16A16_UINT:
        case DXGI_FORMAT_R16G16B16A16_SINT:
        case DXGI_FORMAT_R32_UINT:
        case DXGI_FORMAT_R32_SINT:
        case DXGI_FORMAT_R32G32_UINT:
        case DXGI_FORMAT_R32G32_SINT:
        case DXGI_FORMAT_R32G32B32_UINT:
        case DXGI_FORMAT_R32G32B32_SINT:
        case DXGI_FORMAT_R32G32B32A32_UINT:
        case DXGI_FORMAT_R32G32B32A32_SINT:
        case DXGI_FORMAT_R16_TYPELESS:
        case DXGI_FORMAT_R24G8_TYPELESS:
        case DXGI_FORMAT_R32_TYPELESS:
        case DXGI_FORMAT_R32G8X24_TYPELESS:
        case DXGI_FORMAT_D16_UNORM:
        case DXGI_FORMAT_D24_UNORM_S8_UINT:
        case DXGI_FORMAT_D32_FLOAT:
        case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
        case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
        case DXGI_FORMAT_X24_TYPELESS_G8_UINT:
        case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:
        case DXGI_FORMAT_X32_TYPELESS_G8X24_UINT:
        case DXGI_FORMAT_R8G8_B8G8_UNORM:
        case DXGI_FORMAT_G8R8_G8B8_UNORM:
          return true;
        default:
          return false;
      }
    };

    auto isLargeTexture = [](const VkExtent3D& extent) -> bool {
      return extent.width >= 512 || extent.height >= 512;
    };

    // Collect currently-bound render target images AND their dimensions.
    // Only reject SRVs that point to images actively bound as RTs.
    // VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT is set on most D3D11 textures
    // (engines create them with BIND_RENDER_TARGET for mip gen, dynamic
    // updates, etc.), so the flag alone is NOT a reliable RT indicator.
    const auto& omState = m_context->m_state.om;
    std::array<DxvkImage*, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT> boundRTImages = {};
    uint32_t rtWidth = 0, rtHeight = 0;
    for (uint32_t rt = 0; rt < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++rt) {
      auto* rtv = omState.renderTargetViews[rt].ptr();
      if (rtv) {
        Rc<DxvkImageView> rtvView = rtv->GetImageView();
        if (rtvView != nullptr) {
          boundRTImages[rt] = rtvView->image().ptr();
          if (rt == 0) {
            rtWidth  = rtvView->image()->info().extent.width;
            rtHeight = rtvView->image()->info().extent.height;
          }
        }
      }
    }

    // First pass: find the top-scoring texture candidates without heap allocation.
    // We only need kMaxSupportedTextures (2) winners â€” a full sort is unnecessary.
    static constexpr uint32_t kMaxPicks = LegacyMaterialData::kMaxSupportedTextures;
    struct TexPick {
      uint32_t slot = UINT32_MAX;
      Rc<DxvkImageView> view;
      int score = INT32_MIN;
      bool isCurrentRT = false;
      bool likelyIntermediate = false;
    };
    TexPick picks[kMaxPicks];
    uint32_t pickCount = 0;
    int worstPickScore = INT32_MIN;
    uint32_t worstPickIdx = 0;

    auto registerRemixTextureCandidate = [](const Rc<DxvkImageView>& imageView) {
      if (imageView == nullptr)
        return;

      TextureRef previewRef(imageView);
      const XXH64_hash_t textureHash = previewRef.getImageHash();
      if (textureHash != 0) {
        ImGUI::AddTexture(textureHash, imageView, getTextureUiFeatureFlagsForView(imageView));
      }
    };

    for (uint32_t slot = 0; slot < D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT; ++slot) {
      D3D11ShaderResourceView* srv = ps.shaderResources.views[slot].ptr();
      if (!srv) continue;
      // DX11 keeps stale SRVs bound across draws. Selecting from every bound
      // slot associated UI atlases, scene color and unrelated prior materials
      // with otherwise valid world geometry. A complete DXBC profile is
      // authoritative: only slots consumed by an actual sample/gather
      // instruction are material or texture-browser candidates.
      if (hasCompleteSampledResourceProfile
       && !commonPs->SamplesResourceSlot(slot))
        continue;
      if (srv->GetResourceType() != D3D11_RESOURCE_DIMENSION_TEXTURE2D) continue;

      Rc<DxvkImageView> view = srv->GetImageView();
      if (view == nullptr) continue;

      // Bind a sampled image to geometry only when the active PS proves which
      // input components feed this exact resource slot and the active VS
      // proves that it exports that same semantic. This is the stable identity
      // joining texture hashes to geometry across engines; slot order and
      // semantic-name guesses are not. Safe images without this proof remain
      // available in the Remix texture browser below, but cannot corrupt a
      // draw's albedo/hash association.
      std::string sampledSemanticName;
      uint32_t sampledSemanticIndex = 0;
      uint32_t sampledSemanticComponent = 0;
      std::string resolvedSemanticName;
      uint32_t resolvedSemanticIndex = 0;
      uint32_t resolvedSemanticComponent = 0;
      const bool psUvTraced = commonPs != nullptr
        && commonPs->GetSampledTexcoordSemantic(
             slot, sampledSemanticName, sampledSemanticIndex,
             sampledSemanticComponent);
      const bool hasProvenGeometryUvContract =
        psUvTraced
        && commonVs != nullptr
        && commonVs->ResolvePositionCaptureTexcoord(
             sampledSemanticName, sampledSemanticIndex,
             sampledSemanticComponent,
             resolvedSemanticName, resolvedSemanticIndex,
             resolvedSemanticComponent);
      const bool rejectUnprovenGeometryHash =
        hasCompleteSampledResourceProfile && !hasProvenGeometryUvContract;

      const auto& imgInfo = view->image()->info();
      const auto& viewInfo = view->info();
      D3D11_SHADER_RESOURCE_VIEW_DESC1 srvDesc = {};
      srv->GetDesc1(&srvDesc);
      const D3D11_COMMON_RESOURCE_DESC resourceDesc = srv->GetResourceDesc();
      const DXGI_FORMAT fmt = srvDesc.Format;
      const bool bc = isBlockCompressed(fmt);
      const bool colorBc = isColorBlockCompressed(fmt);
      const bool dataOrSceneFormat = isLikelyDataOrSceneColorFormat(fmt);
      const bool albedoFormat = isLikelyAlbedoFormat(fmt);
      const bool strongAlbedoFormat = isStrongAlbedoFormat(fmt);
      const bool texture2DView = srvDesc.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2D;
      const bool singleSliceTexture2DArrayView = srvDesc.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2DARRAY
        && srvDesc.Texture2DArray.ArraySize == 1;
      const bool materialViewDimension = texture2DView || singleSliceTexture2DArrayView;
      const bool multisampledView = srvDesc.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2DMS
        || srvDesc.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2DMSARRAY
        || imgInfo.sampleCount != VK_SAMPLE_COUNT_1_BIT;

      // Reject Texture2DArray SRVs that cover multiple slices â€” each slice is a separate
      // game texture that hashes identically, causing surfaces to appear "smashed together".
      // Single-slice array views are safe: getImageHash() mixes in the layer index.
      if (srvDesc.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2DARRAY
          && srvDesc.Texture2DArray.ArraySize > 1)
        continue;
      const bool hasMips = viewInfo.numLevels > 1 || (viewInfo.numLevels == 0 && imgInfo.mipLevels > 1);
      const bool hasHazardBindFlags = srv->TestHazards() != FALSE;
      const bool hasRtBind = (resourceDesc.BindFlags & D3D11_BIND_RENDER_TARGET) != 0;
      const bool hasUavBind = (resourceDesc.BindFlags & D3D11_BIND_UNORDERED_ACCESS) != 0;
      const bool hasDepthBind = (resourceDesc.BindFlags & D3D11_BIND_DEPTH_STENCIL) != 0;
      const bool isSingleMipLargeTexture = !hasMips && !bc && isLargeTexture(imgInfo.extent);

      DxvkImage* srvImage = view->image().ptr();
      bool isCurrentRT = false;
      for (uint32_t rt = 0; rt < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++rt) {
        if (boundRTImages[rt] == srvImage) { isCurrentRT = true; break; }
      }

      // Skip tiny dummy textures (1x1 default white/black).
      if (imgInfo.extent.width <= 2 && imgInfo.extent.height <= 2)
        continue;

      // Check if texture dimensions match current render target (likely GBuffer/intermediate).
      const bool matchesRT = (rtWidth > 0 && rtHeight > 0
        && imgInfo.extent.width == rtWidth && imgInfo.extent.height == rtHeight);
      const bool rtSizedIntermediate = matchesRT
        && (hasHazardBindFlags || isSingleMipLargeTexture || dataOrSceneFormat);
      // DX11_V273_USE_REAL_ALBEDO: a mipmapped strong-albedo color texture
      // (RGBA8/BGRA8/BC1-3-7) that is NOT the actively-bound render target is
      // an unambiguous real game material texture - the exact thing the user
      // wants on screen. The intermediate/data heuristics below occasionally
      // mis-flag such a texture (e.g. when it happens to match the RT size),
      // which rejected it -> the surface fell back to the gray placeholder or
      // to an unbound (BLACK) albedo. Never reject a clear albedo, so real
      // colors always render. Render targets are excluded (isCurrentRT /
      // matchesRT) and mip presence keeps out untextured RT/video surfaces,
      // so this cannot pull garbage into the material.
      const bool clearAlbedo = strongAlbedoFormat && hasMips && !isCurrentRT && !matchesRT;

      const bool likelyIntermediate = !clearAlbedo && (multisampledView
        || isCurrentRT
        || rtSizedIntermediate
        || ((hasRtBind || hasUavBind || hasDepthBind) && isSingleMipLargeTexture && !strongAlbedoFormat));
      const bool rejectTextureBrowserCandidate = !materialViewDimension
        || multisampledView
        || isCurrentRT
        || rtSizedIntermediate
        || (hasDepthBind && !hasMips)
        || ((hasRtBind || hasUavBind) && isSingleMipLargeTexture && !bc);
      // The shader's own reflection names its textures when it was not
      // stripped. A slot named like a normal/specular/mask/environment map is
      // never the albedo, however colour-like its format (normal maps are often
      // BC1/BC3/BC7, so format heuristics alone picked them as albedo). A slot
      // named like a diffuse map is strongly preferred. Unknown or generic
      // names leave the format-based scoring unchanged.
      int textureNameRole = 0; // -1 data/normal map, +1 albedo, 0 unknown
      if (commonPs != nullptr && commonPs->GetReflection() != nullptr) {
        const DxbcResourceBinding* binding =
          commonPs->GetReflection()->findBinding(DxbcResourceKind::Texture, slot);
        if (binding != nullptr && !binding->name.empty()) {
          std::string lower = binding->name;
          for (auto& c : lower)
            c = char(::tolower(static_cast<unsigned char>(c)));
          static const char* const kDataNames[] = {
            "normal", "nrm", "bump", "spec", "gloss", "rough", "metal", "mask",
            "env", "cube", "height", "parallax", "occlusion", "lightmap",
            "shadow", "depth", "noise", "flow", "dither", "lut", "ramp" };
          static const char* const kAlbedoNames[] = {
            "diffuse", "albedo", "basecolor", "base_color", "colormap", "color_map" };
          for (const char* n : kDataNames)
            if (lower.find(n) != std::string::npos) { textureNameRole = -1; break; }
          if (textureNameRole == 0) {
            for (const char* n : kAlbedoNames)
              if (lower.find(n) != std::string::npos) { textureNameRole = 1; break; }
          }
        }
      }
      // A slot the pixel shader unpacks with "*2-1" is a normal map, never
      // the albedo, whatever its format (stripped engines: Unity, UE, FO4).
      if (commonPs != nullptr && commonPs->GetTextureDecode(slot).normalEncoding != 0)
        textureNameRole = -1;
      const bool rejectMaterialCandidate = rejectUnprovenGeometryHash
        || textureNameRole < 0
        || (!clearAlbedo && (rejectTextureBrowserCandidate
        || likelyIntermediate
        || !albedoFormat
        || dataOrSceneFormat));

      int score = 0;
      if (textureNameRole > 0)
        score += 30;
      else if (textureNameRole < 0)
        score -= 60;
      if (colorBc)                  score += 14;  // Color BC = strong material signal.
      else if (bc)                  score -= 10;  // BC4/BC5/BC6 are masks/normals/HDR, not albedo.
      if (strongAlbedoFormat)       score += 8;
      else if (albedoFormat)        score += 2;
      if (hasMips)                  score += 5;   // Mipmapped = likely content
      if (!matchesRT)               score += 3;   // Different size from RT = likely content
      if (!isCurrentRT)             score += 2;   // Not actively rendering to it
      // Albedo is the largest colour texture sampled with UV0, not slot 0
      // (METHODS.md, Materials): slot order only breaks ties.
      score += std::max(0, 8 - (int)slot);
      if (psUvTraced && sampledSemanticIndex == 0)
        score += 6;
      {
        const uint32_t edge = std::max(imgInfo.extent.width, imgInfo.extent.height);
        uint32_t log2Edge = 0;
        while ((1u << (log2Edge + 1u)) <= edge && log2Edge < 15u)
          ++log2Edge;
        score += std::clamp(int(log2Edge) - 6, 0, 6);   // 128 px: 1 ... 4096 px: 6
      }

      if (dataOrSceneFormat)
        score -= 24;
      if (!materialViewDimension)
        score -= 16;
      if (multisampledView)
        score -= 32;

      // Global demotion for likely intermediate surfaces.
      // Hazard-capable resources are often postprocess, scene color, video, or other transient targets.
      // Many real material textures are BC-compressed and mipmapped, so only apply the strong penalty
      // when the texture also looks like a large single-mip intermediate.
      if (hasHazardBindFlags)
        score -= isSingleMipLargeTexture ? 16 : 6;

      // Large uncompressed single-mip textures are disproportionately likely to be transient scene/video data.
      if (isSingleMipLargeTexture)
        score -= 8;

      // Resources created primarily for RT/UAV work should lose to ordinary sampled textures whenever possible.
      if ((resourceDesc.BindFlags & (D3D11_BIND_RENDER_TARGET | D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_DEPTH_STENCIL)) != 0)
        score -= 4;

      // Currently bound as active RT â†’ negative score (only use as absolute last resort)
      if (isCurrentRT) score = -10;

      // Sampler address mode: WRAP/MIRROR indicates a tiling world texture (strong positive signal).
      // CLAMP/BORDER indicates an atlas, render target, or postprocess input (negative signal).
      if (slot < D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT) {
        D3D11SamplerState* samp = ps.samplers[slot];
        if (samp != nullptr) {
          D3D11_SAMPLER_DESC sampDesc = {};
          samp->GetDesc(&sampDesc);
          const bool uWrap = (sampDesc.AddressU == D3D11_TEXTURE_ADDRESS_WRAP
                           || sampDesc.AddressU == D3D11_TEXTURE_ADDRESS_MIRROR);
          const bool vWrap = (sampDesc.AddressV == D3D11_TEXTURE_ADDRESS_WRAP
                           || sampDesc.AddressV == D3D11_TEXTURE_ADDRESS_MIRROR);
          if (uWrap && vWrap)       score += 4;   // Tiling = world geometry texture
          else if (!uWrap && !vWrap) score -= 2;  // Clamped = likely atlas/postprocess
          
          // Engine-specific sampler fixes for texture corruption
          // Some engines use non-standard sampler settings that cause texture corruption
          if (RtxOptions::enableUnrealTextureFixes()) {
            if (strongAlbedoFormat && hasMips && !matchesRT && !hasUavBind)
              score += 3;
            if (dataOrSceneFormat || likelyIntermediate)
              score -= 16;
          }
          
          if (RtxOptions::enableSource2Fixes()) {
            // Source 2 engine has specific sampler requirements
            // Apply fixes for Source 2 texture handling
            if (sampDesc.AddressU == D3D11_TEXTURE_ADDRESS_CLAMP ||
                sampDesc.AddressV == D3D11_TEXTURE_ADDRESS_CLAMP) {
              // Source 2 often uses clamp addressing
              score += 1;  // Slight boost for Source 2 textures
            }
          }
        }
      }

      const bool srgbView = isSrgbFormat(fmt);

      const bool likelyNormalLikeTexture =
        !colorBc &&
        !strongAlbedoFormat &&
        !dataOrSceneFormat &&
        materialViewDimension &&
        !multisampledView &&
        !matchesRT &&
        !hasUavBind &&
        // fmt is a DXGI format; this compared it against Vulkan enums, which
        // never matched the intended formats.
        (fmt == DXGI_FORMAT_R8G8B8A8_UNORM
          || fmt == DXGI_FORMAT_R8G8_UNORM
          || fmt == DXGI_FORMAT_R8G8_SNORM
          || fmt == DXGI_FORMAT_R16G16_UNORM
          || fmt == DXGI_FORMAT_R16G16_SNORM
          || fmt == DXGI_FORMAT_R16G16_FLOAT
          || fmt == DXGI_FORMAT_R10G10B10A2_UNORM);

      const bool likelyAtlasOrHelperTexture =
        !bc &&
        !colorBc &&
        !hasMips &&
        materialViewDimension &&
        (imgInfo.extent.width <= 256 || imgInfo.extent.height <= 256);

      if (likelyNormalLikeTexture)
        score -= 18;
      if (likelyAtlasOrHelperTexture)
        score -= 10;
      // DX11_V298_SRGB_ALBEDO_DISCRIMINATOR: authored color content always
      // outranks any linear-format normal/mask candidate in the same draw.
      if (srgbView)
        score += 10;

      if (doLog) {
        Logger::info(str::format("[D3D11Rtx] FillMaterialData tex candidate: slot=", slot,
          " fmt=", (uint32_t)fmt,
          " w=", imgInfo.extent.width, " h=", imgInfo.extent.height,
          " mips=", imgInfo.mipLevels,
          " viewMips=", viewInfo.numLevels,
          " score=", score,
          bc ? " [BC]" : "",
          colorBc ? " [COLOR-BC]" : "",
          dataOrSceneFormat ? " [DATA/SCENE-FMT]" : "",
          albedoFormat ? " [ALBEDO-FMT]" : "",
          hasMips ? " [MIPS]" : "",
          hasHazardBindFlags ? " [HAZARD]" : "",
          hasRtBind ? " [RT-BIND]" : "",
          hasUavBind ? " [UAV-BIND]" : "",
          hasDepthBind ? " [DEPTH-BIND]" : "",
          !materialViewDimension ? " [NON-2D-MATERIAL-VIEW]" : "",
          multisampledView ? " [MSAA]" : "",
          isSingleMipLargeTexture ? " [SINGLE-MIP-LARGE]" : "",
          likelyIntermediate ? " [LIKELY-INTERMEDIATE]" : "",
          isCurrentRT ? " [BOUND-RT]" : "",
          matchesRT ? " [RT-SIZED]" : "",
          hasProvenGeometryUvContract ? " [PROVEN-UV]"
            : (psUvTraced ? str::format(" [NO-PROVEN-UV:VS-UNRESOLVED ", sampledSemanticName,
                                        sampledSemanticIndex, ".", sampledSemanticComponent, "]")
                          : std::string(" [NO-PROVEN-UV:PS-UNTRACED]")),
          rejectTextureBrowserCandidate ? " [REJECT-BROWSER]" : "",
          rejectMaterialCandidate ? " [REJECT-MATERIAL]" : ""));
      }

      // The legacy material can only bind a small number of color textures,
      // but Remix tooling still needs to see every safe game material texture
      // encountered by the draw stream.
      const bool safeForTextureBrowser =
        !rejectTextureBrowserCandidate &&
        (hasMips || bc || albedoFormat) &&
        !likelyNormalLikeTexture &&
        !likelyAtlasOrHelperTexture;

      if (safeForTextureBrowser)
        registerRemixTextureCandidate(view);

      if (rejectMaterialCandidate)
        continue;

      // Keep low-confidence non-albedo candidates out of the legacy material path.
      // This prevents small helper textures and likely normal/packed textures from
      // being merged into the two legacy color slots.
      if (score < 8 && !strongAlbedoFormat && !colorBc)
        continue;

      // Insert into top-N picks (sorted descending by score, no heap alloc).
      if (pickCount < kMaxPicks) {
        picks[pickCount] = { slot, std::move(view), score, isCurrentRT, likelyIntermediate };
        ++pickCount;
        if (pickCount == kMaxPicks) {
          // Find worst to know which slot to evict next.
          worstPickScore = picks[0].score;
          worstPickIdx = 0;
          for (uint32_t p = 1; p < kMaxPicks; ++p) {
            if (picks[p].score < worstPickScore) {
              worstPickScore = picks[p].score;
              worstPickIdx = p;
            }
          }
        }
      } else if (score > worstPickScore) {
        picks[worstPickIdx] = { slot, std::move(view), score, isCurrentRT, likelyIntermediate };
        // Re-find worst.
        worstPickScore = picks[0].score;
        worstPickIdx = 0;
        for (uint32_t p = 1; p < kMaxPicks; ++p) {
          if (picks[p].score < worstPickScore) {
            worstPickScore = picks[p].score;
            worstPickIdx = p;
          }
        }
      }
    }

    // Sort the picks descending by score (at most kMaxPicks = 2 elements).
    if (pickCount == 2 && picks[0].score < picks[1].score)
      std::swap(picks[0], picks[1]);

    // Assign up to maxTextures picks, skipping active RTs if better options exist.
    const uint32_t maxTextures = RtxOptions::ignoreSecondaryTextures()
                                ? 1u : kMaxPicks;
    bool pickedAny = false;
    bool anyPositive = (pickCount > 0 && picks[0].score > 0);
    for (uint32_t p = 0; p < pickCount && textureID < maxTextures; ++p) {
      auto& c = picks[p];
      if (c.isCurrentRT && anyPositive)
        continue;

      mat.colorTextures[textureID] = TextureRef(std::move(c.view));
      mat.colorTextureSlot[textureID] = c.slot;

      if (c.slot < D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT) {
        D3D11SamplerState* samp = ps.samplers[c.slot];
        mat.samplers[textureID] = samp ? samp->GetDXVKSampler() : getDefaultSampler();
      } else {
        mat.samplers[textureID] = getDefaultSampler();
      }

      pickedAny = true;
      ++textureID;
    }

    // Game normal map (documentation/engine_knowledge/METHODS.md, Materials):
    // a two-channel BC5/R8G8 texture the PS samples is a tangent-space XY
    // normal map (z rebuilt); an RGB texture is one only when the shader's
    // reflection names it so (Skyrim-style RGB normals). Mipped, not a
    // render target, not the albedo. Remix otherwise decodes normals as
    // octahedral assets, so the encoding travels with the texture.
    if (!m_abDisableEngineKnowledge && RtxOptions::dx11InferNormalMaps()) {
      const DxbcRdef* psReflection = commonPs != nullptr ? commonPs->GetReflection() : nullptr;
      auto resourceName = [&](uint32_t slot) -> std::string {
        if (psReflection == nullptr || !psReflection->isValid())
          return std::string();
        for (const auto& b : psReflection->resourceBindings())
          if (b.kind == DxbcResourceKind::Texture && slot >= b.bindPoint && slot < b.bindPoint + std::max(1u, b.bindCount)) {
            std::string n = b.name;
            std::transform(n.begin(), n.end(), n.begin(), [](unsigned char c) { return char(std::tolower(c)); });
            return n;
          }
        return std::string();
      };
      for (uint32_t slot = 0; slot < D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT; ++slot) {
        if (slot == mat.colorTextureSlot[0] || slot == mat.colorTextureSlot[1])
          continue;
        D3D11ShaderResourceView* srv = ps.shaderResources.views[slot].ptr();
        if (srv == nullptr || srv->GetResourceType() != D3D11_RESOURCE_DIMENSION_TEXTURE2D)
          continue;
        if (hasCompleteSampledResourceProfile && !commonPs->SamplesResourceSlot(slot))
          continue;
        D3D11_SHADER_RESOURCE_VIEW_DESC1 nDesc = {};
        srv->GetDesc1(&nDesc);
        if (nDesc.ViewDimension != D3D11_SRV_DIMENSION_TEXTURE2D)
          continue;
        const D3D11_COMMON_RESOURCE_DESC nRes = srv->GetResourceDesc();
        if (nRes.BindFlags & (D3D11_BIND_RENDER_TARGET | D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_DEPTH_STENCIL))
          continue;
        Rc<DxvkImageView> nView = srv->GetImageView();
        if (nView == nullptr || nView->image()->info().mipLevels <= 1)
          continue;
        // 1. The pixel shader's own dataflow (engine-independent, works on
        //    stripped shaders): a "*2-1" unpack is a normal map with that
        //    channel order; "1 - x" of a channel is smoothness.
        const D3D11CommonShader::TextureDecode decode = commonPs != nullptr
          ? commonPs->GetTextureDecode(slot) : D3D11CommonShader::TextureDecode();
        // 2. CRYENGINE's fixed material slots (cryengine_frostbite.md): t1
        //    normals (BC5 stored .yx), t5 smoothness, t13 emittance.
        const bool cryFixedSlots = GetD3D11EngineProfile().family() == D3D11EngineFamily::CryEngine;
        const bool bc5 = nDesc.Format == DXGI_FORMAT_BC5_UNORM || nDesc.Format == DXGI_FORMAT_BC5_SNORM;
        const std::string n = resourceName(slot);
        auto has = [&](const char* word) { return n.find(word) != std::string::npos; };

        uint8_t encoding = decode.normalEncoding;
        if (encoding == 0 && cryFixedSlots && slot == 1u && bc5)
          encoding = 5;
        if (encoding == 0) {
          // 3. Format, then reflection names.
          switch (nDesc.Format) {
            case DXGI_FORMAT_BC5_UNORM: case DXGI_FORMAT_R8G8_UNORM:
              encoding = 3; break;
            // Signed: sampled in [-1, 1] already (no "* 2 - 1").
            case DXGI_FORMAT_BC5_SNORM: case DXGI_FORMAT_R8G8_SNORM:
              encoding = 6; break;
            case DXGI_FORMAT_BC1_UNORM: case DXGI_FORMAT_BC3_UNORM: case DXGI_FORMAT_BC7_UNORM:
            case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM:
              if (has("normal") || has("nrm") || has("bump") || has("ddn"))
                encoding = 2;
              break;
            default: break;
          }
        }
        if (encoding != 0 && !mat.normalTexture.isValid()) {
          mat.normalTexture = TextureRef(nView);
          mat.normalEncoding = encoding;
          // CRYENGINE "_ddna" and similar keep smoothness in the normal map's
          // alpha; the dataflow proves it when the shader inverts it.
          if (decode.smoothnessChannel >= 0 && !mat.roughnessTexture.isValid()) {
            mat.roughnessTexture = TextureRef(nView);
            mat.roughnessChannel = uint8_t(decode.smoothnessChannel);
            mat.roughnessIsSmoothness = true;
          }
          continue;
        }

        if (decode.smoothnessChannel >= 0 && !mat.roughnessTexture.isValid()) {
          mat.roughnessTexture = TextureRef(nView);
          mat.roughnessChannel = uint8_t(decode.smoothnessChannel);
          mat.roughnessIsSmoothness = true;
          continue;
        }
        if (cryFixedSlots && n.empty()) {
          if (slot == 5u && !mat.roughnessTexture.isValid()) {
            mat.roughnessTexture = TextureRef(nView);
            mat.roughnessIsSmoothness = true;
            continue;
          }
          if (slot == 13u && !mat.emissiveTexture.isValid()) {
            mat.emissiveTexture = TextureRef(nView);
            continue;
          }
        }

        // 4. Maps the reflection names, with their documented channel
        //    layouts: Unity HDRP mask map (R metallic, A smoothness), Unity
        //    Standard metallic-gloss map (R metallic, A smoothness), Unreal
        //    ORM (G roughness, B metallic).
        if (n.empty())
          continue;
        const bool unityMask = has("maskmap") || has("metallicgloss");
        const bool orm = (has("_orm") || has("occlusionroughness")
          || (n.size() >= 3 && n.compare(n.size() - 3, 3, "orm") == 0)) && !has("normal");
        if (unityMask && !mat.metallicTexture.isValid()) {
          mat.metallicTexture = TextureRef(nView);
          mat.metallicChannel = 0;
          if (!mat.roughnessTexture.isValid()) {
            mat.roughnessTexture = TextureRef(nView);
            mat.roughnessChannel = 3;
            mat.roughnessIsSmoothness = true;
          }
        } else if (orm && !mat.metallicTexture.isValid()) {
          mat.metallicTexture = TextureRef(nView);
          mat.metallicChannel = 2;
          if (!mat.roughnessTexture.isValid()) {
            mat.roughnessTexture = TextureRef(nView);
            mat.roughnessChannel = 1;
          }
        } else if (!mat.roughnessTexture.isValid() && has("rough")) {
          mat.roughnessTexture = TextureRef(nView);
        } else if (!mat.roughnessTexture.isValid() && (has("smoothness") || has("gloss"))) {
          mat.roughnessTexture = TextureRef(nView);
          mat.roughnessIsSmoothness = true;
        } else if (!mat.metallicTexture.isValid() && (has("metallic") || has("metalness"))) {
          mat.metallicTexture = TextureRef(nView);
        } else if (!mat.emissiveTexture.isValid()
                && (has("emissive") || has("emission") || has("glow") || has("illum") || has("emittance"))) {
          mat.emissiveTexture = TextureRef(nView);
        }
      }
    }

    // Last resort: pick the best candidate even if it's an active RT.
    if (!pickedAny && pickCount > 0) {
      auto& c = picks[0];
      // If the only remaining candidate still looks like a transient intermediate,
      // prefer leaving the material untextured over flooding the browser with garbage/video surfaces.
      if (!c.likelyIntermediate) {
        mat.colorTextures[0] = TextureRef(std::move(c.view));
        mat.colorTextureSlot[0] = c.slot;
        if (c.slot < D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT) {
          D3D11SamplerState* samp = ps.samplers[c.slot];
          mat.samplers[0] = samp ? samp->GetDXVKSampler() : getDefaultSampler();
        } else {
          mat.samplers[0] = getDefaultSampler();
        }
        textureID = 1;
      } else {
        mat.colorTextureSlot[0] = kInvalidResourceSlot;
      }
    }

    // Legacy cubemap reflections (Bethesda envmaps: Skyrim, Fallout 4 and the
    // other Creation titles; any engine that adds "cube * strength * mask"):
    // the game adds an untinted reflection over a dark diffuse, which path
    // traces near-black unless the surface becomes a reflector. Strength is
    // the constant the PS scales the cube sample by; the mask is the 2D
    // channel it multiplies in (fo4-decomp MODLOG.md, "Envmap materials").
    if (commonPs != nullptr && !m_abDisableEngineKnowledge) {
      const D3D11CommonShader::EnvmapReflection& env = commonPs->GetEnvmapReflection();
      auto readCb = [&](int8_t slot, uint16_t reg, Vector4& v) -> bool {
        if (slot < 0 || slot >= int8_t(D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT))
          return false;
        const auto& cb = ps.constantBuffers[uint32_t(slot)];
        if (cb.buffer == nullptr)
          return false;
        const auto* bytes = reinterpret_cast<const uint8_t*>(cb.buffer->GetMappedSlice().mapPtr);
        if (bytes == nullptr)
          return false;
        const size_t size = cb.buffer->Desc()->ByteWidth;
        const size_t base = size_t(cb.constantOffset) * 16u;
        const size_t end = cb.constantCount > 0 ? std::min(base + size_t(cb.constantCount) * 16u, size) : size;
        const size_t offset = base + size_t(reg) * 16u;
        if (offset + 16u > end)
          return false;
        std::memcpy(v.data, bytes + offset, 16u);
        return true;
      };
      auto boundView = [&](int32_t slot) -> Rc<DxvkImageView> {
        if (slot < 0 || uint32_t(slot) >= ps.shaderResources.views.size())
          return nullptr;
        D3D11ShaderResourceView* srv = ps.shaderResources.views[uint32_t(slot)].ptr();
        if (srv == nullptr || srv->GetResourceType() == D3D11_RESOURCE_DIMENSION_BUFFER)
          return nullptr;
        return srv->GetImageView();
      };

      float strength = 0.0f;
      Rc<DxvkImageView> maskView;
      uint8_t maskChannel = 0;
      Rc<DxvkImageView> smoothnessView;
      uint8_t smoothnessChannel = 0;

      Vector4 v;
      if (env.cubeSlot >= 0 && boundView(env.cubeSlot) != nullptr
       && readCb(env.scaleCb, env.scaleReg, v)) {
        // Forward envmaps (BSLightingShader / BSEffectShader): the cube is
        // sampled in this draw.
        strength = v[env.scaleComponent];
        // The mask: a non-normal-map channel first (FO4 envmap mask t7.r,
        // spec map t2.r), else the normal map's alpha (Skyrim's specular mask).
        for (uint32_t pass = 0; pass < 2 && maskView == nullptr; ++pass) {
          for (uint32_t i = 0; i < 2 && maskView == nullptr; ++i) {
            const int32_t slot = env.maskSlot[i];
            if (slot < 0 || env.maskChannel[i] < 0)
              continue;
            const bool normalMap = commonPs->GetTextureDecode(uint32_t(slot)).normalEncoding != 0;
            if (normalMap != (pass == 1))
              continue;
            maskView = boundView(slot);
            maskChannel = uint8_t(env.maskChannel[i]);
          }
        }
      } else if (env.gbufferCb >= 0 && GetD3D11EngineProfile().family() == D3D11EngineFamily::Creation
              && readCb(env.gbufferCb, env.gbufferReg, v)) {
        // Creation deferred pre-pass: E = (cubemap index + 1, wet blend,
        // envmap scale, wet scale); dry strength is E.z when E.y != 0. The
        // composite scales the reflection by 3 x spec (t2.r x cb[0].y).
        if (v.x >= 1.0f && v.y != 0.0f) {
          Vector4 scales;
          const float specScale = readCb(env.gbufferCb, 0, scales) && scales.y > 0.0f && scales.y <= 10.0f
            ? scales.y : 1.0f;
          strength = 3.0f * specScale * v.z;
          maskView = boundView(2);
          maskChannel = 0;
          smoothnessView = maskView;
          smoothnessChannel = 1;
        }
      }

      if (std::isfinite(strength) && strength > 1.0e-3f) {
        mat.untintedReflection = true;
        mat.reflectionStrength = std::min(strength, 4.0f);
        if (maskView != nullptr) {
          mat.metallicTexture = TextureRef(maskView);
          mat.metallicChannel = maskChannel;
        } else {
          mat.metallicTexture = TextureRef();
        }
        if (smoothnessView != nullptr && !mat.roughnessTexture.isValid()) {
          mat.roughnessTexture = TextureRef(smoothnessView);
          mat.roughnessChannel = smoothnessChannel;
          mat.roughnessIsSmoothness = true;
        }
      }
    }

    // A draw with no real game texture stays genuinely untextured. It remains
    // full path-traced geometry and uses the legacy material's constant/vertex
    // albedo path, but receives no synthetic image and therefore no invented
    // texture hash. Texture tagging and replacement remain exclusively tied to
    // actual game textures or explicit user-authored material data.

    if (doLog) {
      Logger::info(str::format("[D3D11Rtx] FillMaterialData draw #", s_logCount,
        " picked ", textureID, " of ", pickCount, " candidate(s)",
        " ps=", commonPs != nullptr ? commonPs->GetName() : std::string("none"),
        " completeSampledProfile=", hasCompleteSampledResourceProfile ? 1 : 0));
      // Count every logged draw, not just draws that picked a texture.
      // Previously the counter only advanced when pickCount > 0, so in
      // deferred engines where most draws reject all candidates the 10-draw
      // cap never engaged and the per-draw candidate logging ran forever --
      // tens of thousands of str::format + log writes on the draw hot path
      // (a measurable CPU bottleneck and 30k+ line logs).
      ++s_logCount;
    }

    // Grey-scale (R8, BC4) and grey + alpha (R8G8) albedo would read as red /
    // red-green; the game's shader uses .r as the colour. Same image, so the
    // texture hash (tagging, replacements) is unchanged.
    if (textureID > 0) {
      Rc<DxvkImageView> grey = ToGreyAlbedoView(mat.colorTextures[0].getImageViewRc());
      if (grey != nullptr)
        mat.colorTextures[0] = TextureRef(grey);
    }

    if (textureID > 0 && primaryTextureHashOverride != 0)
      mat.colorTextures[0].setImageHashOverride(primaryTextureHashOverride);

    for (uint32_t textureIndex = 0; textureIndex < textureID; ++textureIndex) {
      const Rc<DxvkImageView> imageView = mat.colorTextures[textureIndex].getImageViewRc();
      const XXH64_hash_t textureHash = mat.colorTextures[textureIndex].getImageHash();
      if (imageView != nullptr && textureHash != 0) {
        ImGUI::AddTexture(textureHash, imageView, getTextureUiFeatureFlagsForView(imageView));
      }
    }

    // Material-instance identity. The primary albedo texture alone cannot separate
    // material instances that share an albedo but override other texture parameters,
    // so optionally key the material on the pixel shader plus the textures bound to
    // the slots that shader's DXBC reflection actually declares. Every input is a
    // pure function of the current draw, so a given instance always hashes the same.
    if (materialInstanceIdentity() && m_context->m_state.ps.shader != nullptr) {
      const D3D11CommonShader* commonPs = m_context->m_state.ps.shader->GetCommonShader();
      const XXH64_hash_t psHash = commonPs != nullptr ? commonPs->GetBytecodeHash() : 0;

      // Shaders compiled with reflection stripped cannot tell material samplers from
      // engine-wide ones; those draws keep plain texture-hash identity.
      if (psHash != 0
       && commonPs->GetReflection() != nullptr
       && !lookupHash(materialInstanceIdentityExcludedShaders(), psHash)) {
        mat.setPixelShaderHashForMaterialInstance(psHash);

        // Ordered (slot, image hash) fold over every declared, bound texture slot.
        // Ordering by ascending slot keeps the result independent of bind order.
        XXH64_hash_t textureSetHash = kEmptyHash;
        const auto& psViews = ps.shaderResources.views;

        for (uint32_t slot = 0; slot < psViews.size(); ++slot) {
          if (psViews[slot] == nullptr || !commonPs->DeclaresTextureBinding(slot))
            continue;

          const Rc<DxvkImageView>& view = psViews[slot]->GetImageView();
          if (view == nullptr)
            continue;

          const struct {
            uint32_t     slot;
            XXH64_hash_t imageHash;
          } entry = { slot, view->image()->getHash() };

          textureSetHash = XXH3_64bits_withSeed(&entry, sizeof(entry), textureSetHash);
        }

        mat.setMaterialTextureSetHashForMaterialInstance(textureSetHash);
      }
    }

    // Material defaults for the Remix legacy material pipeline.
    // D3D11 bakes blending/alpha into immutable state objects â€” we extract
    // what we can from BlendState and DepthStencilState below.
    // A textured D3D11 draw: colour and alpha both come from the selected
    // texture, with no vertex-colour modulation unless a later pass finds one.
    mat.colorSource             = D3D11ColorSource::Texture;
    mat.alphaSource             = D3D11ColorSource::Texture;
    mat.modulateVertexColor     = false;
    mat.blendConstant           = Vector4(1.0f, 1.0f, 1.0f, 1.0f);  // Opaque white

    // --- Blend state ---
    D3D11BlendState* blendState = m_context->m_state.om.cbState;
    if (blendState) {
      D3D11_BLEND_DESC1 blendDesc;
      blendState->GetDesc1(&blendDesc);
      const auto& rt0 = blendDesc.RenderTarget[0];

      mat.blendMode.enableBlending = rt0.BlendEnable;
      mat.blendMode.colorSrcFactor = mapD3D11Blend(rt0.SrcBlend, false);
      mat.blendMode.colorDstFactor = mapD3D11Blend(rt0.DestBlend, false);
      mat.blendMode.colorBlendOp   = mapD3D11BlendOp(rt0.BlendOp);
      mat.blendMode.alphaSrcFactor = mapD3D11Blend(rt0.SrcBlendAlpha, true);
      mat.blendMode.alphaDstFactor = mapD3D11Blend(rt0.DestBlendAlpha, true);
      mat.blendMode.alphaBlendOp   = mapD3D11BlendOp(rt0.BlendOpAlpha);
      mat.blendMode.writeMask      = rt0.RenderTargetWriteMask;

      // AlphaToCoverage = D3D11's cutout transparency (foliage, fences, hair).
      if (blendDesc.AlphaToCoverageEnable) {
        mat.alphaTestEnabled       = true;
        mat.alphaTestCompareOp     = VK_COMPARE_OP_GREATER;
        mat.alphaTestReferenceValue = 128;
      }

      // DX11_V281_FIXED_FUNCTION: the OM blend CONSTANT is real DX11/12
      // fixed-function state (OMSetBlendState's BlendFactor argument; the
      // identical D3D12 dynamic is OMSetBlendFactor). When this draw's blend
      // equation actually references it, forward the constant as the
      // material's constant color so constant-faded surfaces (UI fades,
      // scripted transparency ramps) keep their real opacity in the RT scene
      // instead of the opaque-white default.
      auto referencesBlendFactor = [](D3D11_BLEND b) {
        return b == D3D11_BLEND_BLEND_FACTOR || b == D3D11_BLEND_INV_BLEND_FACTOR;
      };
      if (rt0.BlendEnable
       && (referencesBlendFactor(rt0.SrcBlend)      || referencesBlendFactor(rt0.DestBlend)
        || referencesBlendFactor(rt0.SrcBlendAlpha) || referencesBlendFactor(rt0.DestBlendAlpha))) {
        // The D3D11 blend factor is plain floats, so carry it as floats rather
        // than round-tripping through a packed 8-bit colour.
        const FLOAT* bf = m_context->m_state.om.blendFactor;
        mat.blendConstant = Vector4(bf[0], bf[1], bf[2], bf[3]);
      }
    }

    // --- Material tint constant ---
    // Many engines multiply the albedo texture by a colour constant (Unity
    // _Color/_BaseColor, CRYENGINE MatDifColor, Source 2 g_vColorTint). When
    // the PS reflection names one, its value multiplies albedo through the
    // texture-factor path; without the name nothing is guessed.
    if (!m_abDisableEngineKnowledge && !mat.isTextureFactorBlend
     && m_context->m_state.ps.shader != nullptr) {
      const D3D11CommonShader* tintPs = m_context->m_state.ps.shader->GetCommonShader();
      const DxbcRdef* rdef = tintPs != nullptr ? tintPs->GetReflection() : nullptr;
      static constexpr const char* kTintNames[] = {
        "_Color", "_BaseColor", "_TintColor", "_MainColor", "MatDifColor",
        "g_vColorTint", "TintColor", "DiffuseColor", "BaseColor",
      };
      bool tinted = false;
      for (uint32_t c = 0; rdef != nullptr && rdef->isValid() && !tinted && c < rdef->constantBuffers().size(); ++c) {
        const DxbcConstantBufferInfo& cbInfo = rdef->constantBuffers()[c];
        uint32_t slot = UINT32_MAX;
        for (const auto& b : rdef->resourceBindings())
          if (b.kind == DxbcResourceKind::CBuffer && b.name == cbInfo.name)
            slot = b.bindPoint;
        if (slot >= D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT)
          continue;
        for (const auto& var : cbInfo.variables) {
          if (!var.used || var.size < 12u
           || std::find_if(std::begin(kTintNames), std::end(kTintNames),
                [&](const char* n) { return var.name == n; }) == std::end(kTintNames))
            continue;
          const auto& cb = m_context->m_state.ps.constantBuffers[slot];
          const uint8_t* ptr = cb.buffer != nullptr
            ? reinterpret_cast<const uint8_t*>(cb.buffer->GetMappedSlice().mapPtr) : nullptr;
          const size_t at = size_t(cb.constantOffset) * 16u + var.offset;
          if (ptr == nullptr || at + 16u > cb.buffer->Desc()->ByteWidth)
            break;
          float v[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
          std::memcpy(v, ptr + at, var.size >= 16u ? 16u : 12u);
          bool plausible = true;
          for (uint32_t i = 0; i < 3; ++i)
            plausible &= std::isfinite(v[i]) && v[i] >= 0.0f && v[i] <= 8.0f;
          const bool white = std::abs(v[0] - 1.0f) + std::abs(v[1] - 1.0f) + std::abs(v[2] - 1.0f) < 1.0e-3f;
          if (plausible && !white) {
            // Alpha only fades blended surfaces; on opaque ones it is often
            // unused or carries other data.
            const float alpha = mat.blendMode.enableBlending && std::isfinite(v[3])
              ? std::clamp(v[3], 0.0f, 1.0f) : 1.0f;
            mat.blendConstant = Vector4(v[0], v[1], v[2], alpha);
            mat.isTextureFactorBlend = true;
          }
          tinted = true;
          break;
        }
      }
    }

    // --- Alpha test, the DX10/11/12 way ---
    // D3D10 removed the fixed-function alpha test entirely; nothing in the
    // depth-stencil object expresses it (the previous stencil-func heuristic
    // here misfired on deferred renderers' real stencil usage and fed a
    // BITMASK - StencilReadMask - in as an alpha reference). The API
    // generation's actual mechanisms are AlphaToCoverage (handled above) and
    // shader discard: HLSL clip()/discard compiled into the pixel shader IS
    // the alpha test of DX10/11/12. A PS that can discard marks this draw as
    // cutout geometry, with the universal clip(alpha - 0.5) convention as the
    // reference. Opaque textures (alpha = 255) pass unconditionally, so this
    // is inert on draws whose discard serves another purpose.
    if (!mat.alphaTestEnabled && m_context->m_state.ps.shader != nullptr) {
      const D3D11CommonShader* commonPs = m_context->m_state.ps.shader->GetCommonShader();
      if (commonPs != nullptr && commonPs->UsesDiscard()) {
        mat.alphaTestEnabled        = true;
        mat.alphaTestCompareOp      = VK_COMPARE_OP_GREATER;
        mat.alphaTestReferenceValue = 128;
      }
    }

    // --- Refractive surfaces (water, glass) ---
    // D3D11 engines draw water as world geometry whose pixel shader samples a
    // copy of the already-rendered scene (the refraction buffer) next to a
    // tiling content texture (the wave normal map). Taken as a legacy opaque
    // material, that surface shows the game's rasterized scene copy stretched
    // over it. The path tracer refracts and reflects for real, so hand these
    // draws to Remix as a translucent water material instead. Light volumes
    // and other additive passes sample render targets too, but blend
    // additively; depth-only and fullscreen passes never reach here.
    mat.isRefractiveSurface = false;
    if (RtxOptions::dx11RefractiveSurfacesAsWater()
     && hasCompleteSampledResourceProfile
     && !(commonVs != nullptr && commonVs->WritesScreenSpacePosition())
     && mat.blendMode.colorDstFactor != VK_BLEND_FACTOR_ONE) {
      bool depthTested = true;
      if (D3D11DepthStencilState* dsState = m_context->m_state.om.dsState) {
        D3D11_DEPTH_STENCIL_DESC dsDesc;
        dsState->GetDesc(&dsDesc);
        depthTested = dsDesc.DepthEnable != FALSE;
      }

      uint32_t rt0Width = 0, rt0Height = 0;
      std::array<DxvkImage*, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT> boundTargets = {};
      for (uint32_t rt = 0; rt < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++rt) {
        auto* rtv = m_context->m_state.om.renderTargetViews[rt].ptr();
        if (rtv == nullptr || rtv->GetImageView() == nullptr)
          continue;
        boundTargets[rt] = rtv->GetImageView()->image().ptr();
        if (rt == 0) {
          rt0Width  = boundTargets[rt]->info().extent.width;
          rt0Height = boundTargets[rt]->info().extent.height;
        }
      }

      bool samplesSceneCopy = false;
      bool samplesTilingContent = false;
      Rc<DxvkImageView> contentView;
      uint32_t contentSlot = kInvalidResourceSlot;
      for (uint32_t slot = 0; depthTested && rt0Width > 0
           && slot < D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT; ++slot) {
        D3D11ShaderResourceView* srv = ps.shaderResources.views[slot].ptr();
        if (srv == nullptr || !commonPs->SamplesResourceSlot(slot)
         || srv->GetResourceType() != D3D11_RESOURCE_DIMENSION_TEXTURE2D)
          continue;
        Rc<DxvkImageView> view = srv->GetImageView();
        if (view == nullptr)
          continue;

        const auto& info = view->image()->info();
        bool isBoundTarget = false;
        for (DxvkImage* target : boundTargets)
          isBoundTarget |= target == view->image().ptr();
        if (isBoundTarget)
          continue;

        const D3D11_COMMON_RESOURCE_DESC resourceDesc = srv->GetResourceDesc();
        const bool rtBind    = (resourceDesc.BindFlags & D3D11_BIND_RENDER_TARGET) != 0;
        const bool depthBind = (resourceDesc.BindFlags & D3D11_BIND_DEPTH_STENCIL) != 0;

        // The scene copy: a single-mip colour render target at least half the
        // size of the target being drawn into (refraction is often half-res).
        if (rtBind && !depthBind && info.mipLevels == 1
         && info.extent.width * 2 >= rt0Width && info.extent.height * 2 >= rt0Height)
          samplesSceneCopy = true;

        // Authored content: mip-mapped, never a render target. The first one
        // (the wave/normal map) identifies the water surface.
        if (!rtBind && !depthBind && info.mipLevels > 1 && !samplesTilingContent) {
          samplesTilingContent = true;
          contentView = view;
          contentSlot = slot;
        }
      }

      mat.isRefractiveSurface = samplesSceneCopy && samplesTilingContent;

      // Key the water material on its authored texture. The generic picker
      // may have chosen the refraction copy (a render target): hidden from the
      // Remix texture list and unstable as a hash, so water could not be
      // found or tagged. The authored texture is stable and always listed.
      if (mat.isRefractiveSurface && contentView != nullptr) {
        mat.colorTextures[0] = TextureRef(contentView);
        mat.colorTextureSlot[0] = contentSlot;
        if (primaryTextureHashOverride != 0)
          mat.colorTextures[0].setImageHashOverride(primaryTextureHashOverride);
        const XXH64_hash_t contentHash = mat.colorTextures[0].getImageHash();
        if (contentHash != 0)
          ImGUI::AddTexture(contentHash, contentView, ImGUI::kTextureFlagsDefault);

        // The wave map is the water's normal map: the translucent material
        // decodes it by the encoding the shader's own unpack proves, else by
        // its format (two-channel XY, or RGB).
        uint8_t waterEncoding = commonPs->GetTextureDecode(contentSlot).normalEncoding;
        if (waterEncoding == 0) {
          D3D11_SHADER_RESOURCE_VIEW_DESC1 waterDesc = {};
          ps.shaderResources.views[contentSlot]->GetDesc1(&waterDesc);
          switch (waterDesc.Format) {
            case DXGI_FORMAT_BC5_UNORM: case DXGI_FORMAT_R8G8_UNORM:
              waterEncoding = 3; break;
            case DXGI_FORMAT_BC5_SNORM: case DXGI_FORMAT_R8G8_SNORM:
              waterEncoding = 6; break;
            default:
              waterEncoding = 2; break;
          }
        }
        mat.normalTexture = TextureRef(contentView);
        mat.normalEncoding = waterEncoding;
      }

      if (mat.isRefractiveSurface) {
        static fast_unordered_set s_loggedRefractiveShaders;
        const XXH64_hash_t psHash = commonPs->GetBytecodeHash();
        if (s_loggedRefractiveShaders.insert(psHash).second) {
          Logger::info(str::format(
            "[D3D11Rtx] Refractive surface -> Remix translucent water: ps=0x", std::hex, psHash, std::dec,
            " blend=", mat.blendMode.enableBlending ? 1 : 0));
        }
      }
    }

    // Preserve the game's real sampled albedo and alpha contract. Texture
    // categorization and replacements key on this same live TextureRef/hash;
    // replacing the combiner with opaque-white TFactor made every path-traced
    // surface washed out and hid the visible result of dev-menu tagging.
    mat.updateCachedHash();
  }

  void D3D11Rtx::SubmitDraw(bool indexed,
                             UINT count,
                             UINT start,
                             INT  base,
                             const Matrix4* instanceTransform,
                             UINT replayFirstInstance,
                             UINT replayInstanceCount,
                             bool requireExactPositionCapture) {
    ScopedCpuProfileZoneN("D3D11Rtx::SubmitDraw");
    if (TryInjectAtUiComposite())
      return;  // the composite itself stays a raster draw over the RT frame
    // Time the whole submission path for this draw. Scoped so every early-out
    // below is still measured - a draw that is expensive to *reject* costs the
    // frame just as much as one that is expensive to accept, and the rejection
    // paths are where the surprises tend to be.
    const bool timeDrawSubmission = RtxOptions::logDrawSubmissionPerf();
    const auto drawCpuStart = timeDrawSubmission ? std::chrono::high_resolution_clock::now()
      : std::chrono::high_resolution_clock::time_point();
    const uint32_t timedDrawId = m_drawCallID;
    const auto drawCpuScopeExit = [&]() {
      if (!timeDrawSubmission)
        return;
      const uint64_t elapsedNs = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::high_resolution_clock::now() - drawCpuStart).count());
      m_frameDrawCpuNs += elapsedNs;
      ++m_frameTimedDraws;
      if (elapsedNs > m_frameSlowestDrawNs) {
        m_frameSlowestDrawNs = elapsedNs;
        m_frameSlowestDrawId = timedDrawId;
        m_frameSlowestDrawIndices = count;
        m_frameSlowestDrawHash = m_context->m_state.ps.shader != nullptr
          ? m_context->m_state.ps.shader->GetCommonShader()->GetBytecodeHash()
          : kEmptyHash;
      }
    };
    struct ScopeGuard {
      const decltype(drawCpuScopeExit)& fn;
      ~ScopeGuard() { fn(); }
    } drawCpuGuard { drawCpuScopeExit };

    // Chromium renderer / utility processes never draw the game: no work.
    if (GetD3D11EngineProfile().chromiumHelperProcess)
      return;

    // Katana's clustered lights are bound to its full-screen deferred
    // lighting pass, which is rejected as a composite further down.
    if (!m_abDisableEngineKnowledge && RtxOptions::dx11ImportTiledLights()
     && GetD3D11EngineProfile().family() == D3D11EngineFamily::Katana
     && m_tiledLightImportFrame != m_context->m_device->getCurrentFrameId()
     && m_context->m_state.ps.shader != nullptr)
      ImportKatanaClusterLights(m_context->m_state.ps.shaderResources, m_context->m_state.ps.shader->GetCommonShader());

    if (m_pGeometryWorkers == nullptr) {
      const bool isDeferredContext = m_context->GetType() == D3D11_DEVICE_CONTEXT_DEFERRED;
      const uint32_t cores = std::max(2u, std::thread::hardware_concurrency());
      const uint32_t workers = isDeferredContext
        ? 1u
        : std::min(std::max(cores / 2, 2u), 6u);
      m_pGeometryWorkers = std::make_unique<GeometryProcessor>(workers,
        isDeferredContext ? "d3d11-deferred-geometry" : "d3d11-geometry");

      if (isDeferredContext) {
        static uint32_t s_deferredGeometryInitLogCount = 0;
        if (s_deferredGeometryInitLogCount < 8) {
          ++s_deferredGeometryInitLogCount;
          Logger::info(str::format("[D3D11Rtx] Enabled deferred-context RTX submission with ", workers, " worker(s)"));
        }
      }
    }

    ++m_submitRejectStats.total;
    s_processWideSubmittedDraws.fetch_add(1u, std::memory_order_relaxed);

    // Emulator integration is authenticated per draw through a versioned
    // ID3D11DeviceContext private-data ABI. No metadata means the normal PC
    // game path below is byte-for-byte unchanged. rtx.emulator.enableIntegration
    // is the hard separation switch: disabled, no emulator code (camera,
    // profile, transforms) can run at all and every draw takes the PC path.
    std::optional<remix::emulator::DrawMetadataV1> emulatorMetadata;
    if (RtxOptions::Emulator::enableIntegration())
      emulatorMetadata = readEmulatorDrawMetadata(m_context);
    const bool authenticatedEmulatorDraw = emulatorMetadata.has_value();
    const bool pcsx2PostTransformDraw = authenticatedEmulatorDraw
      && emulatorMetadata->provider == remix::emulator::Provider::Pcsx2
      && emulatorMetadata->coordinateSpace ==
         remix::emulator::CoordinateSpace::Pcsx2GsPostTransform;
    if (authenticatedEmulatorDraw) {
      m_authenticatedEmulatorHost = true;
      m_postTransformEmulatorHost = false;
      m_forceRasterPassThroughThisFrame = false;
      activateEmulatorProfile(*emulatorMetadata);

      // Guest-frame boundary: pick up a published ABI camera (if the emulator
      // provides one) and advance the camera-motion tracker. Every draw of a
      // guest frame shares the publisher's frameId.
      if (emulatorMetadata->frameId != s_emulatorCameraFrameId) {
        s_emulatorCameraFrameId = emulatorMetadata->frameId;
        s_emulatorPublishedCamera = readEmulatorCameraMetadata(m_context);
        if (RtxOptions::Emulator::estimateCameraMotion()) {
          s_emulatorCamera.beginFrame(
            uint32_t(std::max(RtxOptions::Emulator::cameraMotionMinSamplePoints(), 9)),
            RtxOptions::Emulator::cameraMotionMaxTranslation());
        }
      }
    }

    // PCSX2 renders PS2 GS commands after the guest CPU/VU has already applied
    // its model/view/projection transform. The D3D11 input is packed screen
    // XY/depth, and VS_EXPAND variants fetch the same record through
    // SV_VertexID from a StructuredBuffer with no input layout. A synthetic
    // world camera here would make geometry follow the host camera, destabilize
    // hashes, and build an enclosing slab/black rectangle. Once this capability
    // has been identified, keep the guest image as raster while still walking
    // bound textures so Remix's texture browser and hash tagging remain usable.
    if (m_postTransformEmulatorHost && !m_authenticatedEmulatorHost) {
      m_forceRasterPassThroughThisFrame = true;
      LegacyMaterialData rasterMaterial;
      FillMaterialData(rasterMaterial);
      ++m_submitRejectStats.postTransformEmulator;
      return;
    }

    // Once this frame has crossed onto its raster-overlay path, do not spend
    // GPU/CPU work capturing more screen-space triangles into the RT scene.
    // Still enumerate every bound texture so the Remix texture grid, manual
    // hash categories and capture/export tooling continue to see UI atlases.
    if (m_forceRasterPassThroughThisFrame && !m_authenticatedEmulatorHost) {
      LegacyMaterialData rasterMaterial;
      FillMaterialData(rasterMaterial);
      ++m_submitRejectStats.screenSpaceUiSkip;
      return;
    }

    // forceInjection overflow guard: when injection is forced but the
    // previous frame produced zero scene instances, only the first
    // kForceInjectionProbeDraws draws are considered (enough for camera and
    // scene discovery - SR4 finds its camera at drawCallID 41). Everything
    // past the window is rejected before geometry processing, so the
    // Remix UI and composite stay alive while the acceleration structure
    // stays empty instead of rebuilding 6k junk instances per frame.
    if (RtxOptions::forceInjection()
     && m_prevFrameSceneAccepted == 0
     && m_prevFrameRealSceneAccepted == 0
     && m_submitRejectStats.total > kForceInjectionProbeDraws) {
      // DX11_V295_ROTATING_PROBE: a fixed first-N window never discovered
      // cameras that only appear late in heavy frames (Hello Neighbor 2
      // issues ~2000 draws per frame; its scene/camera draws sit past the
      // window, so forceInjIdle rejected them every frame and the title
      // stayed rasterized forever). In addition to the first N draws, probe
      // a window that rotates through the frame's draw range: every draw
      // position is examined within a few frames while per-frame work stays
      // bounded.
      const uint32_t totalDraws =
        std::max(m_prevFrameTotalDraws, kForceInjectionProbeDraws + 1u);
      const uint32_t windowCount =
        (totalDraws + kForceInjectionProbeDraws - 1u) / kForceInjectionProbeDraws;
      const uint32_t windowBase =
        (m_forceInjectionProbePhase % windowCount) * kForceInjectionProbeDraws;
      const uint32_t drawIndex = m_submitRejectStats.total - 1u;
      const bool inRotatingWindow = drawIndex >= windowBase
        && drawIndex < windowBase + kForceInjectionProbeDraws;
      if (!inRotatingWindow) {
        ++m_submitRejectStats.forceInjectionIdle;
        return;
      }
    }

    // Throttle: don't exceed the worker ring buffer capacity.
    // Beyond this point new futures would overwrite in-flight ones â†’ corrupt hashes.
    if (m_drawCallID >= kMaxConcurrentDraws) {
      ++m_submitRejectStats.queueOverflow;
      return;
    }

    // --- Cheap pre-filters: discard draws that cannot contribute to raytracing ---

    // Only triangle topologies are raytraceable. Skip points, lines, patch lists, etc.
    // This check is first: it costs a single comparison before any other state is read.
    const D3D11_PRIMITIVE_TOPOLOGY d3dTopology = m_context->m_state.ia.primitiveTopology;
    // Patch lists with HS + DS bound produce triangles; they are captured from
    // the domain shader's output (TryCapturePositionsViaStreamOut). Their
    // control-point cage must never reach the BLAS, so capture is mandatory.
    const bool tessellatedDraw = RtxOptions::dx11CaptureTessellation() && !m_abDisableEngineKnowledge
      && d3dTopology >= D3D11_PRIMITIVE_TOPOLOGY_1_CONTROL_POINT_PATCHLIST
      && d3dTopology <= D3D11_PRIMITIVE_TOPOLOGY_32_CONTROL_POINT_PATCHLIST
      && m_context->m_state.hs.shader != nullptr && m_context->m_state.ds.shader != nullptr
      && m_context->m_state.gs.shader == nullptr;
    // A geometry shader that emits triangles turns points/lines/triangles into
    // the triangles the rasterizer sees (particles, rain, fur shells); the
    // recompiled game GS is captured, so its input topology does not matter.
    const bool geometryShaderDraw = RtxOptions::dx11CaptureGeometryShaders() && !m_abDisableEngineKnowledge
      && m_context->m_state.gs.shader != nullptr
      && m_context->m_state.hs.shader == nullptr && m_context->m_state.ds.shader == nullptr
      && m_context->m_state.gs.shader->GetCommonShader()->HasPositionCaptureCandidate()
      && (d3dTopology == D3D11_PRIMITIVE_TOPOLOGY_POINTLIST || d3dTopology == D3D11_PRIMITIVE_TOPOLOGY_LINELIST
       || d3dTopology == D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP || d3dTopology == D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST
       || d3dTopology == D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    if (d3dTopology != D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST &&
        d3dTopology != D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP && !tessellatedDraw && !geometryShaderDraw) {
      ++m_submitRejectStats.nonTriangleTopology;
      return;
    }
    if (tessellatedDraw) {
      requireExactPositionCapture = true;
      ++m_submitRejectStats.tessellatedAdmitted;
    }
    if (geometryShaderDraw) {
      requireExactPositionCapture = true;
      ++m_submitRejectStats.geometryShaderAdmitted;
    }

    // Skip depth-only passes: no pixel shader means depth prepass or shadow map.
    // Most engines draw opaque geometry twice â€” once for depth prepass (PS == null)
    // and once for the color pass (PS != null) with the same vertices.
    if (m_context->m_state.ps.shader == nullptr) {
      LearnSunFromShadowDraw();
      ++m_submitRejectStats.noPixelShader;
      return;
    }

    const auto& omState = m_context->m_state.om;
    const bool hasColorRenderTarget = std::any_of(
      omState.renderTargetViews.begin(),
      omState.renderTargetViews.end(),
      [](const auto& rtv) { return rtv.ptr() != nullptr; });
    const bool hasDepthStencilTarget = omState.depthStencilView.ptr() != nullptr;

    // Skip draws with no output target at all.
    if (!hasColorRenderTarget && !hasDepthStencilTarget) {
      ++m_submitRejectStats.noRenderTarget;
      return;
    }

    // DX11_V277_NO_DEPTH_ONLY_GEOMETRY: reject depth-only draws (depth/stencil
    // bound, NO color target). These are depth-prepass and shadow-map
    // re-renders of geometry the color pass ALSO draws - and they usually DO
    // bind a pixel shader (alpha-tested foliage/fences clip in the PS), so the
    // PS==null check above never caught them. Submitting them put every such
    // mesh into the RT scene two or three times per frame: the prepass copy
    // (no color texture) coincident with the textured main-pass copy - the
    // "grey/black flicker like a placeholder texture in the way" - and the
    // shadow-pass copy placed with the LIGHT's matrices (its cbuffers hold the
    // light view/proj during that pass) - the "geometry stacking on each
    // other" corruption. A depth-only draw can never contribute visible color;
    // the color pass provides the one true copy, so nothing visible is lost.
    if (!hasColorRenderTarget) {
      LearnSunFromShadowDraw();
      ++m_submitRejectStats.depthOnlySkipped;
      return;
    }

    // Colour targets bound but every write mask zero: occlusion-query boxes,
    // stencil-marking volumes (explosion/effect volumes, portals) and depth
    // prepasses drawn with colour writes disabled. They are invisible in the
    // game and must not become solid RT geometry.
    if (D3D11BlendState* blendState = m_context->m_state.om.cbState) {
      D3D11_BLEND_DESC1 blendDesc = {};
      blendState->GetDesc1(&blendDesc);
      bool writesAnyColor = false;
      for (uint32_t rt = 0; rt < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++rt) {
        if (omState.renderTargetViews[rt].ptr() == nullptr)
          continue;
        const auto& target = blendDesc.RenderTarget[blendDesc.IndependentBlendEnable ? rt : 0];
        if (target.RenderTargetWriteMask != 0) {
          writesAnyColor = true;
          break;
        }
      }
      if (!writesAnyColor) {
        LearnSunFromShadowDraw();
        ++m_submitRejectStats.depthOnlySkipped;
        return;
      }
    }

    // Far-plane geometry (sky domes, skyboxes, sun/moon/cloud layers): the
    // vertex shader writes SV_Position.z from .w, pinning depth to 1. Deferred
    // renderers draw it after the world from the same camera, so Remix's
    // sky heuristics miss it and it became a solid shell around the player.
    // The sky comes from Remix's atmosphere instead; never submit these.
    if (m_context->m_state.vs.shader != nullptr) {
      const D3D11CommonShader* skyVs = m_context->m_state.vs.shader->GetCommonShader();
      if (skyVs != nullptr && skyVs->WritesPositionAtFarPlane()) {
        ++m_submitRejectStats.farPlaneSkySkipped;
        return;
      }

    }

    // Screen-space passes: the vertex shader writes SV_Position.w as a
    // constant, so its vertices are already screen positions (full-screen
    // post-process triangles, UI quads). Taken as scene geometry such a
    // triangle became a camera-filling plane; one that samples the scene copy
    // was even turned into translucent water, greying out the whole
    // path-traced view. They are rejected after UI routing below (UI quads
    // must still reach it). Emulator post-transform draws are excluded: their
    // positions are screen space by design and carry their own camera.
    const bool screenSpaceVsDraw = !authenticatedEmulatorDraw
      && m_context->m_state.vs.shader != nullptr
      && m_context->m_state.vs.shader->GetCommonShader() != nullptr
      && m_context->m_state.vs.shader->GetCommonShader()->WritesScreenSpacePosition();

    // 2D lift (see Lift2DProjection): an orthographic or screen-space draw of
    // a 2D game becomes a layer of the path-traced scene. Decided here, before
    // the screen-space rejections below, which a lifted draw bypasses.
    bool clipOrthographic = false, clipPerspective = false;
    if (!authenticatedEmulatorDraw)
      ClassifyClipProjection(clipOrthographic, clipPerspective);
    const bool ortho2DDraw = screenSpaceVsDraw || clipOrthographic;
    bool lift2D = false;
    bool lift2DComposite = false;
    m_lift2DDraw = false;
    if (ortho2DDraw && !m_seenPerspectiveScene) {
      ++m_submitRejectStats.lift2DCandidates;
      bool samplesOnlyRenderTargets = false;
      const DxvkImage* sampledTarget = Lift2DSampledRenderTarget(samplesOnlyRenderTargets);
      auto* rtv0 = m_context->m_state.om.renderTargetViews[0].ptr();
      Rc<DxvkImageView> rtv0View = rtv0 != nullptr ? rtv0->GetImageView() : nullptr;
      const bool intoBackbuffer = rtv0View != nullptr && m_lastBackbufferImage != nullptr
        && rtv0View->image().ptr() == m_lastBackbufferImage;
      if (samplesOnlyRenderTargets) {
        // The composite of an offscreen playfield (or a post effect): it shows
        // layers that are lifted where they were drawn. Learn the playfield.
        lift2DComposite = true;
        if (intoBackbuffer && sampledTarget != nullptr)
          m_lift2DSceneTargetNext = sampledTarget;
      } else {
        lift2D = m_lift2DFrame && IsLift2DTarget();
      }
    }

    // Skip trivially small draws (< 3 elements = 0 triangles).
    if (count < 3) {
      ++m_submitRejectStats.trivialDraw;
      return;
    }

    // Read actual depth/stencil state from the OM â€” don't hardcode.
    bool zEnable = true;
    bool zWriteEnable = true;
    D3D11_COMPARISON_FUNC depthComparison = D3D11_COMPARISON_LESS;
    bool stencilEnabled = false;
    D3D11DepthStencilState* dsState = m_context->m_state.om.dsState;
    if (dsState) {
      D3D11_DEPTH_STENCIL_DESC dsDesc;
      dsState->GetDesc(&dsDesc);
      zEnable         = dsDesc.DepthEnable != FALSE;
      zWriteEnable    = dsDesc.DepthWriteMask != D3D11_DEPTH_WRITE_MASK_ZERO;
      depthComparison = dsDesc.DepthFunc;
      stencilEnabled  = dsDesc.StencilEnable != FALSE;
    }

    // Far-plane sky pinned through the viewport depth range (MinDepth ==
    // MaxDepth == far) instead of through the vertex shader.
    if (m_context->m_state.rs.numViewports > 0 && zEnable
     && isFarClampedViewport(m_context->m_state.rs.viewports[0], depthComparison,
          GetD3D11EngineProfile().facts->depth == D3D11DepthConvention::Reversed)) {
      ++m_submitRejectStats.farPlaneSkySkipped;
      return;
    }

    // Skip fullscreen quad / postprocess draws: depth disabled + 6 or fewer
    // elements (a fullscreen triangle or quad) + no depth write.
    // Only skip if BOTH depth test and write are off â€” some engines do
    // "depth off, write on" for sky or "depth on, write off" for decals.
    // Temporary sky diagnostic: one line per distinct vertex shader with how it
    // writes SV_Position.zw and the depth/viewport state it draws with.
    if (m_context->m_state.vs.shader != nullptr) {
      static fast_unordered_set s_loggedPositionWriters;
      static uint32_t s_positionWriterLogs = 0;
      const D3D11CommonShader* diagVs = m_context->m_state.vs.shader->GetCommonShader();
      if (diagVs != nullptr && s_positionWriterLogs < 400
       && s_loggedPositionWriters.insert(diagVs->GetBytecodeHash()).second) {
        ++s_positionWriterLogs;
        const auto& vp = m_context->m_state.rs.viewports[0];
        Logger::info(str::format("[D3D11Rtx][pos-writer] vs=0x", std::hex, diagVs->GetBytecodeHash(),
          " ps=0x", m_context->m_state.ps.shader != nullptr ? m_context->m_state.ps.shader->GetCommonShader()->GetBytecodeHash() : 0ull,
          std::dec, " count=", count, " zEnable=", zEnable ? 1 : 0, " zWrite=", zWriteEnable ? 1 : 0,
          " zFunc=", uint32_t(depthComparison), " vpDepth=", vp.MinDepth, "-", vp.MaxDepth,
          " farPlane=", diagVs->WritesPositionAtFarPlane() ? 1 : 0, " writes:", diagVs->GetPositionWriteSummary()));
      }
    }

    // Far-plane geometry, runtime form: engines that transform position with
    // a constant-buffer matrix (dp4 per clip component) pin the sky to the far
    // plane by uploading a projection whose z row equals its w row (z/w = 1),
    // or whose z row is zero under reversed-Z. The vertex shader alone cannot
    // show that, so read the clip-producing matrix's z and w rows from the
    // bound constants. Compared per component, so an infinite-far projection
    // (z and w rows differ only by the near-plane term) is never mistaken
    // for sky.
    if (m_context->m_state.vs.shader != nullptr) {
      const D3D11CommonShader* clipVs = m_context->m_state.vs.shader->GetCommonShader();
      const D3D11PositionTransformBinding* clipBinding =
        clipVs != nullptr ? clipVs->GetPositionTransformBinding() : nullptr;
      if (clipBinding != nullptr && clipBinding->valid && clipBinding->matrixCount >= 1u) {
        const D3D11PositionTransformMatrixBinding& clipMatrix =
          clipBinding->matrices[clipBinding->matrixCount - 1u];
        const uint32_t zReg = clipMatrix.constantRegisters[2];
        const uint32_t wReg = clipMatrix.constantRegisters[3];
        // Row form needs two distinct real rows; column form always yields both.
        const bool rowsUsable = clipMatrix.columns
          || (zReg != UINT32_MAX && wReg != UINT32_MAX && zReg != wReg);
        if (clipMatrix.constantBufferSlot < D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT && rowsUsable) {
          const auto& cb = m_context->m_state.vs.constantBuffers[clipMatrix.constantBufferSlot];
          Vector4 clipRows[4];
          if (readBindingRows(clipMatrix, cb, clipRows)) {
            {
              const float* zRow = clipRows[2].data;
              const float* wRow = clipRows[3].data;
              bool zEqualsW = true, zIsZero = true, wNonZero = false;
              for (uint32_t c = 0; c < 4; ++c) {
                const float tolerance = 2.0e-5f * std::max(std::abs(wRow[c]), 1.0f);
                zEqualsW &= std::isfinite(zRow[c]) && std::abs(zRow[c] - wRow[c]) <= tolerance;
                zIsZero  &= std::abs(zRow[c]) <= 1.0e-7f;
                wNonZero |= std::abs(wRow[c]) > 1.0e-6f;
              }
              const bool reversedDepth = depthComparison == D3D11_COMPARISON_GREATER
                                      || depthComparison == D3D11_COMPARISON_GREATER_EQUAL;
              if (wNonZero && (zEqualsW || (zIsZero && reversedDepth))) {
                ++m_submitRejectStats.farPlaneSkySkipped;
                return;
              }
              static uint32_t s_clipRowLogs = 0;
              if (!zWriteEnable && count > 64 && s_clipRowLogs < 40) {
                ++s_clipRowLogs;
                Logger::info(str::format("[D3D11Rtx][clip-rows] vs=0x", std::hex, clipVs->GetBytecodeHash(), std::dec,
                  " count=", count, " z=(", zRow[0], ",", zRow[1], ",", zRow[2], ",", zRow[3],
                  ") w=(", wRow[0], ",", wRow[1], ",", wRow[2], ",", wRow[3], ")"));
              }
            }
          }
        }
      }
    }

    // Camera-centred models (sky domes, cloud/star shells, weather cones):
    // engines draw the sky as an ordinary mesh whose origin is placed at the
    // eye every frame, so it surrounds the player at any distance. In the
    // ray-traced scene that mesh is a closed shell around the camera that
    // blocks every ray (and shows up stretched in every reflection), while
    // Remix supplies the real sky itself. The object origin's clip position
    // is the translation column of the shader's own object-to-clip
    // transform; an origin at the eye has clip x, y and w all ~0.
    // First-person passes (reserved depth range) are also eye-anchored and
    // are excluded.
    if (RtxOptions::dx11SkipCameraCenteredModels()
     && m_context->m_state.vs.shader != nullptr
     && m_context->m_state.rs.numViewports > 0
     && !isReservedDepthViewport(m_context->m_state.rs.viewports[0])) {
      const D3D11CommonShader* domeVs = m_context->m_state.vs.shader->GetCommonShader();
      const D3D11PositionTransformBinding* domeBinding =
        domeVs != nullptr ? domeVs->GetPositionTransformBinding() : nullptr;
      if (domeBinding != nullptr && domeBinding->matrixCount >= 1u && domeBinding->matrixCount <= 2u) {
        auto readRows = [&](const D3D11PositionTransformMatrixBinding& binding, Vector4 (&rows)[4]) {
          if (binding.constantBufferSlot >= D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT)
            return false;
          const auto& cb = m_context->m_state.vs.constantBuffers[binding.constantBufferSlot];
          return readBindingRows(binding, cb, rows);
        };

        Vector4 m0[4], m1[4];
        bool readable = readRows(domeBinding->matrices[0], m0);
        Vector4 originClip(m0[0].w, m0[1].w, m0[2].w, m0[3].w);
        if (readable && domeBinding->matrixCount == 2u) {
          readable = readRows(domeBinding->matrices[1], m1);
          if (readable)
            originClip = Vector4(dot(m1[0], originClip), dot(m1[1], originClip),
                                 dot(m1[2], originClip), dot(m1[3], originClip));
        }

        // Only perspective transforms qualify (w row carries view depth);
        // screen-space and orthographic passes have a constant w.
        Vector4 wRow = domeBinding->matrixCount == 2u ? m1[3] : m0[3];
        const bool perspective = readable
          && (std::abs(wRow.x) + std::abs(wRow.y) + std::abs(wRow.z)) > 1.0e-6f;
        if (perspective) {
          const float radius = RtxOptions::dx11CameraCenteredRadius();
          const bool centredOnEye = std::abs(originClip.x) <= radius
                                 && std::abs(originClip.y) <= radius
                                 && std::abs(originClip.w) <= radius;
          static fast_unordered_set s_loggedDomeShaders;
          if ((centredOnEye || (!zWriteEnable && count >= 300u))
           && s_loggedDomeShaders.size() < 64u
           && s_loggedDomeShaders.insert(domeVs->GetBytecodeHash() ^ (centredOnEye ? 1ull : 0ull)).second) {
            Logger::info(str::format("[D3D11Rtx][sky-model] vs=0x", std::hex, domeVs->GetBytecodeHash(), std::dec,
              " count=", count, " zWrite=", zWriteEnable ? 1 : 0,
              " originClip=(", originClip.x, ",", originClip.y, ",", originClip.z, ",", originClip.w, ")",
              centredOnEye ? " -> camera-centred, skipped" : ""));
          }
          if (centredOnEye) {
            ++m_submitRejectStats.cameraCenteredSkipped;
            return;
          }
        }
      }
    }

    if (!zEnable && !zWriteEnable && count <= 6 && !lift2D) {
      ++m_submitRejectStats.fullscreenPostFx;
      return;
    }

    // Draws whose positions never come from the input assembler as floats:
    // vertex pulling through SRVs (no layout) or quantized integer POSITION
    // decompressed in the VS. Positions come from post-VS capture only.
    bool vertexPulled = false;
    auto vertexPulledDrawEligible = [&]() {
      if (m_abDisableEngineKnowledge || !zEnable || count <= 6u)
        return false;
      if (m_context->m_state.vs.shader == nullptr
       || !m_context->m_state.vs.shader->GetCommonShader()->HasPositionCaptureCandidate())
        return false;
      if (m_context->m_state.gs.shader != nullptr || m_context->m_state.hs.shader != nullptr
       || m_context->m_state.ds.shader != nullptr)
        return false;
      const auto topology = m_context->m_state.ia.primitiveTopology;
      return topology == D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST
         || (!indexed && topology == D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    };

    D3D11InputLayout* layout = m_context->m_state.ia.inputLayout.ptr();
    if (!layout) {
      if (authenticatedEmulatorDraw) {
        // VS-expanded PCSX2 draws fetch GS records through SV_VertexID from a
        // StructuredBuffer. The explicit handshake is valid, but there is no
        // IA stream to decode into a BLAS. Keep only this draw on raster; the
        // PCSX2 integration disables VS expansion for capture-capable draws.
        LegacyMaterialData rasterMaterial;
        FillMaterialData(rasterMaterial);
        ++m_submitRejectStats.postTransformEmulator;
        static uint32_t s_expandedEmulatorDrawLogCount = 0;
        if (s_expandedEmulatorDrawLogCount++ < 8u) {
          Logger::warn(
            "[D3D11Rtx][emulator-profile] Authenticated PCSX2 draw used VS-expanded/no-layout input; preserving raster draw. DisableVertexShaderExpand is required for Remix scene capture.");
        }
        return;
      } else if (RtxOptions::Emulator::enableIntegration()
              && isPcsx2HostProcess() && !m_authenticatedEmulatorHost) {
        m_postTransformEmulatorHost = true;
        m_forceRasterPassThroughThisFrame = true;
        ++m_submitRejectStats.postTransformEmulator;
        Logger::info(
          "[D3D11Rtx][emulator-profile] PCSX2 post-transform GS path detected "
          "(SV_VertexID/no input layout). Preserving the guest raster surface "
          "and texture-hash discovery; RTX scene injection is disabled because "
          "no guest world vertices or camera reach host D3D11.");
        return;
      }
      ++m_submitRejectStats.noInputLayout;
      // Vertex-pulled world geometry (FO4 precombines read t5..t8 by
      // SV_VertexID; ACU, Apex, Chrome do the same) vs fullscreen passes,
      // which also have no layout. Counted and logged so the capture path for
      // the former can be sized from data.
      if (zEnable && zWriteEnable && count > 6u) {
        ++m_submitRejectStats.noLayoutWorldCandidate;
        static std::unordered_set<std::string> s_loggedPulledVs;
        const std::string vsName = m_context->m_state.vs.shader != nullptr
          ? m_context->m_state.vs.shader->GetCommonShader()->GetName() : std::string("none");
        if (s_loggedPulledVs.size() < 32u && s_loggedPulledVs.insert(vsName).second) {
          uint32_t vsSrvs = 0;
          for (const auto& view : m_context->m_state.vs.shaderResources.views)
            vsSrvs += view != nullptr ? 1u : 0u;
          Logger::info(str::format("[D3D11Rtx][vertex-pulled] vs=", vsName, " count=", count,
            " indexed=", indexed ? 1 : 0, " instances=", replayInstanceCount,
            " vsSRVs=", vsSrvs, " topology=", uint32_t(m_context->m_state.ia.primitiveTopology)));
        }
      }
      // Vertex-pulled world geometry has no IA positions, but its VS still
      // writes the exact SV_Position. Admit it with post-VS capture as the
      // only position source (a synthetic placeholder stream below carries
      // its identity until capture replaces it).
      if (!vertexPulledDrawEligible())
        return;
      vertexPulled = true;
      requireExactPositionCapture = true;
      --m_submitRejectStats.noInputLayout;
      ++m_submitRejectStats.vertexPulledAdmitted;
    }

    // Indirect draws: the CPU knows neither the vertex range nor the counts,
    // so positions come from capture only, whatever the input layout holds.
    if (m_indirectReplay.active && !vertexPulled) {
      if (!vertexPulledDrawEligible()) {
        ++m_submitRejectStats.indirectRejected;
        return;
      }
      vertexPulled = true;
      requireExactPositionCapture = true;
    }
    if (m_indirectReplay.active)
      ++m_submitRejectStats.indirectAdmitted;

    static const std::vector<D3D11RtxSemantic> kNoSemantics;
    const auto& semantics = layout != nullptr ? layout->GetRtxSemantics() : kNoSemantics;

    if (!m_authenticatedEmulatorHost
     && RtxOptions::Emulator::enableIntegration()
     && isPcsx2HostProcess() && isPcsx2GsVertexLayout(semantics)) {
      m_postTransformEmulatorHost = true;
      m_forceRasterPassThroughThisFrame = true;
      ++m_submitRejectStats.postTransformEmulator;
      Logger::info(
        "[D3D11Rtx][emulator-profile] PCSX2 packed POSITION0/POSITION1 GS "
        "layout detected (post-transform screen XY/depth). Preserving the "
        "guest raster surface and texture-hash discovery; RTX scene injection "
        "is disabled because no guest world vertices or camera reach host D3D11.");
      return;
    }

    if (semantics.empty() && !vertexPulled) {
      ++m_submitRejectStats.noSemantics;
      return;
    }

    // Quantized POSITION (Dunia/Disrupt int4 + _MeshDecompression, Anvil
    // fixed-point): the position scorer drops non-float formats and would
    // otherwise take a float TEXCOORD for the position. The VS decodes it, so
    // post-VS capture is the only exact position source.
    const D3D11RtxSemantic* quantizedPosition = nullptr;
    if (!vertexPulled && !pcsx2PostTransformDraw) {
      for (const D3D11RtxSemantic& s : semantics) {
        if (s.index == 0 && semanticNameStartsWith(s, "POSITION") && !isPositionFormat(s.format)
         && s.format != VK_FORMAT_R16G16B16A16_SFLOAT && s.format != VK_FORMAT_R16G16_SFLOAT)
          quantizedPosition = &s;
      }
      if (quantizedPosition != nullptr && vertexPulledDrawEligible()) {
        vertexPulled = true;
        requireExactPositionCapture = true;
        ++m_submitRejectStats.vertexPulledAdmitted;
      }
    }

    const D3D11RtxSemantic* posSem = vertexPulled ? nullptr
      : selectBestSemantic(semantics, scorePositionSemantic);
    const D3D11RtxSemantic* tcSem  = selectBestSemantic(semantics, scoreTexcoordSemantic, { posSem, quantizedPosition });
    if (!tcSem)
      tcSem = selectBestSemantic(semantics, scoreTexcoordFallbackSemantic, { posSem, quantizedPosition });

    auto findSemantic = [&](const char* name, uint32_t index) -> const D3D11RtxSemantic* {
      for (const D3D11RtxSemantic& semantic : semantics) {
        if (semantic.index == index && semanticNameStartsWith(semantic, name))
          return &semantic;
      }
      return nullptr;
    };
    const D3D11RtxSemantic* emulatorDepthSem = nullptr;
    const D3D11RtxSemantic* emulatorQSem = nullptr;
    if (pcsx2PostTransformDraw) {
      // PCSX2's fixed-function GS contract is explicit in the ABI. Do not let
      // generic semantic scoring accidentally select POSITION1 as XY or the
      // Q channel as UV.
      posSem = findSemantic("POSITION", 0);
      emulatorDepthSem = findSemantic("POSITION", 1);
      emulatorQSem = findSemantic("TEXCOORD", 1);
      tcSem = findSemantic("TEXCOORD",
        (emulatorMetadata->flags & remix::emulator::DrawFlagFixedUv) ? 2u : 0u);
      if (posSem == nullptr || emulatorDepthSem == nullptr
       || posSem->format != VK_FORMAT_R16G16_UINT
       || emulatorDepthSem->format != VK_FORMAT_R32_UINT) {
        ++m_submitRejectStats.positionFormatRejected;
        static uint32_t s_badPcsx2LayoutLogCount = 0;
        if (s_badPcsx2LayoutLogCount++ < 8u) {
          Logger::warn(
            "[D3D11Rtx][emulator-profile] Authenticated PCSX2 draw did not expose the declared R16G16_UINT XY + R32_UINT Z contract; preserving native draw.");
        }
        return;
      }
    }

    if (tcSem
     && !semanticNameStartsWith(*tcSem, "TEXCOORD")
     && !semanticNameStartsWith(*tcSem, "TEX")
     && !semanticNameStartsWith(*tcSem, "UV")
     && !semanticNameStartsWith(*tcSem, "TCOORD")
     && !semanticNameStartsWith(*tcSem, "MAP")) {
      static uint32_t sTexcoordDiscoverLogCount = 0;
      if (sTexcoordDiscoverLogCount < 32) {
        ++sTexcoordDiscoverLogCount;
        Logger::info(str::format(
          "[D3D11Rtx] Selected fallback TEXCOORD semantic: ",
          tcSem->name,
          tcSem->index,
          " fmt=",
          static_cast<uint32_t>(tcSem->format),
          " comps=",
          tcSem->componentCount,
          " reg=",
          tcSem->registerId));
      }
    }

    const D3D11RtxSemantic* nrmSem = selectBestSemantic(semantics, scoreNormalSemantic, { posSem, tcSem });
    const D3D11RtxSemantic* colSem = selectBestSemantic(semantics, scoreColorSemantic, { posSem, tcSem, nrmSem });
    const D3D11RtxSemantic* bwSem  = selectBestSemantic(semantics, scoreBlendWeightSemantic, { posSem, tcSem, nrmSem, colSem });
    const D3D11RtxSemantic* biSem  = selectBestSemantic(semantics, scoreBlendIndexSemantic, { posSem, tcSem, nrmSem, colSem, bwSem });

    if (!posSem && !vertexPulled) {
      ++m_submitRejectStats.noPositionSemantic;
      return;
    }

    // Skip 2D UI/HUD draws: if position is R32G32_SFLOAT it is in screen/clip space,
    // not world space, and cannot be raytraced.
    //
    // Caveat: some engines emit billboard/sprite geometry as R32G32_SFLOAT quads
    // and expand them into 3D inside the vertex shader using the camera basis.
    // Those draws are valid 3D content and participate in world-space lighting,
    // so they depth-test against the scene. Only reject 2D-position draws that
    // ALSO have depth testing off â€” which is the unambiguous HUD / overlay case.
    // In a 2D game these are its sprites: lift them (the composite of an
    // offscreen playfield stays out, see above).
    if (posSem != nullptr && posSem->format == VK_FORMAT_R32G32_SFLOAT && !zEnable
     && !lift2D && m_lift2DFrame && !lift2DComposite && IsLift2DTarget()) {
      lift2D = true;
      if (!ortho2DDraw)
        ++m_submitRejectStats.lift2DCandidates;
    }
    if (posSem != nullptr && posSem->format == VK_FORMAT_R32G32_SFLOAT && !zEnable && !lift2D) {
      ++m_submitRejectStats.position2D;
      return;
    }

    // D3D11 draws address the vertex buffer through BaseVertexLocation (indexed
    // draws) or StartVertexLocation (non-indexed draws), and indexed draws read
    // the index buffer starting at StartIndexLocation. Remix does none of this:
    // it reads indices from the start of the bound slice and fetches vertices by
    // the raw index value (no base added) - see RtxGeometryUtils::cacheIndexData
    // OnGPU and RasterGeometry::printDebugInfo. So the base/start offsets must be
    // folded into the buffer slices here. Without it, engines that pack many
    // sub-meshes into one shared vertex/index buffer (the DX11 norm) resolve
    // every non-zero-base draw to the wrong vertices, which makes the geometry
    // collapse toward one point and stretch into spikes when ray traced.
    const int64_t vertexStartIndex = indexed ? int64_t(base) : int64_t(start);

    auto makeVertexBuffer = [&](const D3D11RtxSemantic* sem) -> RasterBuffer {
      if (!sem)
        return RasterBuffer();
      const auto& vb = m_context->m_state.ia.vertexBuffers[sem->inputSlot];
      if (vb.buffer == nullptr)
        return RasterBuffer();
      // Advance the slice to the first vertex this draw touches. Per-slot stride
      // is used so multi-stream layouts stay correct. A negative BaseVertexLocation
      // that would move the slice before the buffer start cannot be represented,
      // so skip the attribute (the draw is dropped when this is the position).
      const int64_t sliceOffset = int64_t(vb.offset) + vertexStartIndex * int64_t(vb.stride);
      if (sliceOffset < 0)
        return RasterBuffer();
      DxvkBufferSlice slice = vb.buffer->GetBufferSlice(static_cast<VkDeviceSize>(sliceOffset));
      return RasterBuffer(slice, sem->byteOffset, vb.stride, sem->format);
    };

    RasterBuffer posBuffer;
    uint64_t vertexPulledIdentity = 0;
    if (vertexPulled) {
      // Placeholder position stream: capture replaces it before anything
      // reaches the BLAS (requireExactPositionCapture). Its first vertex
      // carries this draw's identity so geometry hashes stay distinct: the
      // VS, the draw range, the instance, the index buffer and the SRVs the VS
      // pulls vertices from.
      uint64_t id = m_context->m_state.vs.shader->GetCommonShader()->GetBytecodeHash();
      auto mixId = [&id](uint64_t v) { id ^= v + 0x9e3779b97f4a7c15ull + (id << 6) + (id >> 2); };
      mixId(count); mixId(start); mixId(uint64_t(int64_t(base))); mixId(replayFirstInstance);
      mixId(indexed ? 1u : 0u);
      if (m_indirectReplay.active) {
        mixId(m_indirectReplay.identity);
        if (m_indirectReplay.indexed) {
          const auto& ib = m_context->m_state.ia.indexBuffer;
          mixId(uint64_t(reinterpret_cast<uintptr_t>(ib.buffer.ptr())));
          mixId(ib.offset);
        }
        for (uint32_t slot = 0; slot < D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT; ++slot) {
          const auto& vb = m_context->m_state.ia.vertexBuffers[slot];
          if (vb.buffer != nullptr) { mixId(uint64_t(reinterpret_cast<uintptr_t>(vb.buffer.ptr()))); mixId(vb.offset); }
        }
      }
      if (indexed) {
        const auto& ib = m_context->m_state.ia.indexBuffer;
        mixId(uint64_t(reinterpret_cast<uintptr_t>(ib.buffer.ptr())));
        mixId(ib.offset);
      }
      const auto& vsViews = m_context->m_state.vs.shaderResources.views;
      for (uint32_t slot = 0; slot < vsViews.size(); ++slot)
        if (vsViews[slot] != nullptr) { mixId(slot); mixId(uint64_t(reinterpret_cast<uintptr_t>(vsViews[slot].ptr()))); }
      if (quantizedPosition != nullptr) {
        const auto& vb = m_context->m_state.ia.vertexBuffers[quantizedPosition->inputSlot];
        mixId(uint64_t(reinterpret_cast<uintptr_t>(vb.buffer.ptr())));
        mixId(vb.offset);
        mixId(vb.stride);
      }
      vertexPulledIdentity = id;

      const uint32_t placeholderVertices = std::max(1u, std::min<uint32_t>(count, kMaxHashedVertices));
      const VkDeviceSize placeholderBytes = VkDeviceSize(placeholderVertices) * 12u;
      Rc<DxvkBuffer> placeholder = AcquireHostVisibleHelperBuffer(placeholderBytes, "d3d11 rtx vertex-pulled identity");
      float* p = placeholder != nullptr ? reinterpret_cast<float*>(placeholder->mapPtr(0)) : nullptr;
      if (p == nullptr) {
        ++m_submitRejectStats.noPositionBuffer;
        return;
      }
      std::memset(p, 0, size_t(placeholderBytes));
      // Finite floats (21-bit integers), never NaN.
      p[0] = float(uint32_t(id) & 0x1FFFFFu);
      p[1] = float(uint32_t(id >> 21) & 0x1FFFFFu);
      p[2] = float(uint32_t(id >> 42) & 0x1FFFFFu);
      posBuffer = RasterBuffer(DxvkBufferSlice(placeholder, 0, placeholderBytes), 0, 12u, VK_FORMAT_R32G32B32_SFLOAT);
    } else {
      posBuffer = makeVertexBuffer(posSem);
      if (!posBuffer.defined()) {
        ++m_submitRejectStats.noPositionBuffer;
        return;
      }
    }
    RasterBuffer emulatorDepthBuffer = makeVertexBuffer(emulatorDepthSem);
    RasterBuffer emulatorQBuffer = makeVertexBuffer(emulatorQSem);

    // Normal buffer: only submit if enabled and the interleaver can convert.
    // Supported: R16G16_SFLOAT(83), R32G32_SFLOAT(103), R32G32B32_SFLOAT(106),
    // R32G32B32A32_SFLOAT(109), R8G8B8A8_UNORM(37), A2B10G10R10_SNORM(65).
    // D3D11 normals are often R16G16B16A16_SFLOAT(97) or R16G16B16A16_SNORM(98)
    // which the interleaver rejects.  Remix regenerates normals when absent.
    RasterBuffer nrmBuffer;
    if (nrmSem && RtxOptions::useInputAssemblerNormals()) {
      VkFormat nf = nrmSem->format;
      if (nf == VK_FORMAT_R8G8B8A8_UNORM
       || nf == VK_FORMAT_R32G32B32_SFLOAT
       || nf == VK_FORMAT_R32G32B32A32_SFLOAT
       || nf == VK_FORMAT_R32G32_SFLOAT
       || nf == VK_FORMAT_R16G16_SFLOAT
       || nf == static_cast<VkFormat>(65)) {  // A2B10G10R10_SNORM_PACK32
        nrmBuffer = makeVertexBuffer(nrmSem);
      }
    }
    RasterBuffer tcBuffer  = makeVertexBuffer(tcSem);

    RasterBuffer skinWeightBuffer;
    RasterBuffer skinIndexBuffer;
    uint32_t skinBonesPerVertex = 0;

    // Color0: the interleaver's uint path accepts ONLY B8G8R8A8_UNORM, but the
    // capture admits the formats games actually declare for COLOR0 and the
    // DX11_V268 normalization below converts them all into that layout:
    // packed bytes (BGRA/RGBA), float4 (very common in modern engines - was
    // silently dropped, washing baked lighting/tinting out to white), half4
    // and unorm16 (COLOR-named only; those formats double as normal/tangent
    // storage under other names).
    RasterBuffer colBuffer;
    if (colSem) {
      const VkFormat cf = colSem->format;
      const bool packedByteColor = cf == VK_FORMAT_B8G8R8A8_UNORM
                                || cf == VK_FORMAT_R8G8B8A8_UNORM;
      // DX11_V277: ALL wide color formats (float4 included) now require an
      // explicit COLOR semantic name. A generic-named float4 stream is more
      // often per-vertex data (weights, params) than diffuse color; feeding
      // it into the albedo modulate tinted surfaces with garbage - part of
      // the "wrong colors" corruption. Real float4 vertex colors are named
      // COLOR in practice, so nothing legitimate is lost.
      // Float RGB, 10:10:10:2 and 11:11:10 colours are COLOR-named only too:
      // those formats also store normals and tangents.
      const bool wideColor = (cf == VK_FORMAT_R32G32B32A32_SFLOAT
                           || cf == VK_FORMAT_R32G32B32_SFLOAT
                           || cf == VK_FORMAT_R16G16B16A16_UNORM
                           || cf == VK_FORMAT_R16G16B16A16_SFLOAT
                           || cf == VK_FORMAT_A2B10G10R10_UNORM_PACK32
                           || cf == VK_FORMAT_B10G11R11_UFLOAT_PACK32)
                          && semanticNameStartsWith(*colSem, "COLOR");
      if (packedByteColor || wideColor) {
        colBuffer = makeVertexBuffer(colSem);
      }
    }

    // Indexed triangle-list draws whose vertex shader has a provable position
    // output are captured post-VS and flattened: the capture replaces the IA
    // vertex domain, discarding COLOR0 and IA object-space bounds. CPU work
    // that only feeds those (format conversion, bounds sampling) is skipped.
    const D3D11CommonShader* flattenCaptureVs = m_context->m_state.vs.shader != nullptr
      ? m_context->m_state.vs.shader->GetCommonShader() : nullptr;
    const bool drawWillBeFlattenCaptured = indexed
      && m_context->m_state.ia.primitiveTopology == D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST
      && useVertexCapture()
      && m_context->m_device->features().extTransformFeedback.transformFeedback
      && m_context->m_state.gs.shader == nullptr
      && m_context->m_state.hs.shader == nullptr
      && m_context->m_state.ds.shader == nullptr
      && flattenCaptureVs != nullptr && flattenCaptureVs->HasPositionCaptureCandidate();

    RasterBuffer idxBuffer;
    // DX11_V319_INDEX_SHADOW: CPU-side copy of the index data for this draw,
    // used only when the real buffer is device-local and therefore unmappable.
    const D3D11Buffer* idxShadowSource = nullptr;
    VkDeviceSize       idxShadowOffset = 0;
    if (indexed) {
      const auto& ib = m_context->m_state.ia.indexBuffer;
      if (ib.buffer == nullptr) {
        ++m_submitRejectStats.noIndexBuffer;
        return;
      }
      VkIndexType idxType = (ib.format == DXGI_FORMAT_R32_UINT)
                          ? VK_INDEX_TYPE_UINT32
                          : VK_INDEX_TYPE_UINT16;
      uint32_t idxStride = (idxType == VK_INDEX_TYPE_UINT32) ? 4 : 2;
      // Skip StartIndexLocation indices so element 0 of the slice is the first
      // index this draw consumes (Remix always reads from the slice start).
      const VkDeviceSize idxSliceOffset = VkDeviceSize(ib.offset) + VkDeviceSize(start) * idxStride;
      idxBuffer = RasterBuffer(ib.buffer->GetBufferSlice(idxSliceOffset), 0, idxStride, idxType);
      if (!idxBuffer.defined()) {
        ++m_submitRejectStats.noIndexBuffer;
        return;
      }
      // DX11_V319_INDEX_SHADOW: remember where this draw's indices live so the
      // range scan below can fall back to the CPU-side copy when the device-local
      // buffer cannot be mapped.
      idxShadowSource = ib.buffer.ptr();
      idxShadowOffset = idxSliceOffset;
    }

    VkPrimitiveTopology vkTopology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    switch (m_context->m_state.ia.primitiveTopology) {
      case D3D11_PRIMITIVE_TOPOLOGY_POINTLIST:     vkTopology = VK_PRIMITIVE_TOPOLOGY_POINT_LIST;     break;
      case D3D11_PRIMITIVE_TOPOLOGY_LINELIST:      vkTopology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST;      break;
      case D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP:     vkTopology = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;     break;
      case D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST:  vkTopology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;  break;
      case D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP: vkTopology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP; break;
      default: break;
    }

    RasterGeometry geo;
    geo.topology       = vkTopology;
    geo.frontFace      = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    geo.positionBuffer = posBuffer;
    geo.normalBuffer   = nrmBuffer;
    geo.texcoordBuffer = tcBuffer;
    geo.color0Buffer   = colBuffer;
    geo.blendWeightBuffer = skinWeightBuffer;
    geo.blendIndicesBuffer = skinIndexBuffer;
    geo.numBonesPerVertex = skinBonesPerVertex;
    geo.indexBuffer    = idxBuffer;
    geo.indexCount     = indexed ? count : 0;

    // Read cull mode from the immutable ID3D11RasterizerState object.
    // Default: no culling (safe fallback when no state is bound).
    geo.cullMode = VK_CULL_MODE_NONE;
    D3D11RasterizerState* rsState = m_context->m_state.rs.state;
    if (rsState) {
      const auto* rsDesc = rsState->Desc();

      // DX11_V281_FIXED_FUNCTION: fill mode is real DX11/12 fixed-function
      // rasterizer state (D3D12 packs the same field into the PSO's
      // D3D12_RASTERIZER_DESC). Wireframe draws are debug visualizations and
      // editor overlays - their triangles are only ever LINES on screen, so
      // submitting them as solid RT geometry inserts opaque phantom surfaces
      // into the path-traced scene. Skip them; the game's own wireframe
      // rasterization still renders through the passthrough pipeline.
      if (rsDesc->FillMode == D3D11_FILL_WIREFRAME) {
        ++m_submitRejectStats.wireframeSkipped;
        return;
      }

      // DX11_V289_RAY_SAFE_TWO_SIDED: the application's raster cull mode is
      // valid only for rays originating at the raster camera. Remix launches
      // primary, shadow, reflection, and indirect rays from arbitrary points;
      // carrying D3D11_CULL_BACK/FRONT into the TLAS therefore removes valid
      // intersections from the opposite side and produces black faces, light
      // leaks, and missing foliage. Keep the original front-winding convention
      // below (it is still needed for hit orientation and normal handling), but
      // make captured DX11 geometry two-sided for ray traversal. Authored USD
      // replacements retain their own forceCullBit/cull policy downstream.
      geo.cullMode = VK_CULL_MODE_NONE;
      geo.frontFace = rsDesc->FrontCounterClockwise
        ? VK_FRONT_FACE_COUNTER_CLOCKWISE
        : VK_FRONT_FACE_CLOCKWISE;
    }

    // Compute vertex count â€” must cover the highest vertex index accessed by
    // this draw so Remix doesn't read out of bounds when building the BLAS.
    // The position slice now starts at the draw's first vertex (base/start folded
    // in above), so all counts below are relative to that origin.
    // Count only complete position elements. The old length/stride division
    // ignored the semantic byte offset and could advertise one extra vertex;
    // an index to that element then made the BLAS read past the buffer slice.
    // The authenticated GS path below decodes packed uint16 XY separately.
    const uint32_t positionBytes = pcsx2PostTransformDraw
      ? 4u : positionElementBytes(posBuffer.vertexFormat());
    const VkDeviceSize positionOffset = posBuffer.offsetFromSlice();
    const VkDeviceSize positionLength = posBuffer.length();
    const VkDeviceSize positionReadable = positionLength > positionOffset
      ? positionLength - positionOffset
      : 0;
    const VkDeviceSize maxVBVerticesWide = posBuffer.stride() > 0
      && positionBytes > 0
      && positionReadable >= positionBytes
      ? 1u + (positionReadable - positionBytes) / posBuffer.stride()
      : 0u;
    const uint32_t maxVBVertices = static_cast<uint32_t>(
      std::min<VkDeviceSize>(maxVBVerticesWide, UINT32_MAX));
    if (maxVBVertices == 0) {
      ++m_submitRejectStats.vertexRangeRejected;
      return;
    }
    uint32_t drawVertexCount;
    uint32_t hashStart = 0;
    uint32_t hashCount;
    bool indexRangeCpuVisible = false;
    bool indexRangeExact = !indexed;
    bool usedWholeVertexBufferFallback = false;
    if (vertexPulled) {
      // Capture replays the game's own draw: non-indexed emits `count`
      // vertices, indexed is flattened to one vertex per index. Nothing reads
      // the placeholder beyond its identity vertices.
      drawVertexCount = count;
      hashCount = std::min(count, maxVBVertices);
      indexRangeExact = !indexed;
      usedWholeVertexBufferFallback = indexed;
    } else if (!indexed) {
      // Non-indexed: relative vertices [0, count) after the start offset.
      if (count > maxVBVertices) {
        ++m_submitRejectStats.vertexRangeRejected;
        return;
      }
      drawVertexCount = count;
      hashCount = drawVertexCount;
    } else {
      // Indexed: index values are relative to the base vertex, so the highest
      // one referenced determines how many vertices Remix must copy. Scan the
      // (CPU-visible) index range for the exact maximum; fall back to the whole
      // remaining vertex buffer when the indices can't be read here or the draw
      // is too large to scan cheaply on the submit thread.
      uint32_t maxIndexPlusOne = 0;
      const uint32_t idxStrideBytes = std::max(idxBuffer.stride(), 1u);
      const void* idxScan = idxBuffer.defined() ? idxBuffer.mapPtr(0) : nullptr;

      // DX11_V319_INDEX_SHADOW: a static index buffer is device-local, so
      // mapPtr is null and the exact range cannot be scanned - which used to
      // force the whole-vertex-buffer fallback and then DROP the draw
      // ("gpu-index-flatten-required"), making ordinary level geometry
      // invisible. Fall back to the copy kept at buffer creation. Only the
      // bytes this draw actually consumes are requested, so a partial or
      // absent shadow declines safely and the old path still applies.
      if (idxScan == nullptr && idxShadowSource != nullptr) {
        idxScan = idxShadowSource->GetIndexShadow(
          idxShadowOffset, VkDeviceSize(count) * VkDeviceSize(idxStrideBytes));
      }

      indexRangeCpuVisible = idxScan != nullptr;
      const uint32_t idxAvail = idxBuffer.defined()
        ? static_cast<uint32_t>(idxBuffer.length() / idxStrideBytes)
        : 0u;
      // BLAS indexCount remains the original draw count. A short slice cannot
      // be repaired by scanning fewer elements; the GPU would read beyond it.
      if (idxAvail < count) {
        ++m_submitRejectStats.indexRangeRejected;
        return;
      }
      const uint32_t scanCount = count;
      static constexpr uint32_t kMaxIndexScan = 4u << 20; // cap submit-thread work
      if (idxScan && scanCount > 0 && scanCount <= kMaxIndexScan) {
        const bool primitiveRestart = vkTopology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
        const auto range = idxBuffer.indexType() == VK_INDEX_TYPE_UINT32
          ? rtx::scanIndexRange<uint32_t>(idxScan, scanCount, maxVBVertices, primitiveRestart)
          : rtx::scanIndexRange<uint16_t>(idxScan, scanCount, maxVBVertices, primitiveRestart);
        if (!range.valid) {
          ++m_submitRejectStats.indexRangeRejected;
          return;
        }
        maxIndexPlusOne = range.vertexCount;
      }
      if (maxIndexPlusOne > 0) {
        // Exact maximum known - size the vertex range to it.
        indexRangeExact = true;
        drawVertexCount = std::min(maxIndexPlusOne, maxVBVertices);
      } else {
        // Cover the index type's entire addressable range, bounded by the
        // remaining vertex slice. Shared 128 MiB buffers are common; rejecting
        // them by allocation size discarded valid 16-bit draws before compact
        // GPU capture could run, and copying them wasted millions of vertices.
        const bool primitiveRestart = vkTopology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
        const uint32_t addressableVertices = idxBuffer.indexType() == VK_INDEX_TYPE_UINT32
          ? rtx::addressableVertexCount<uint32_t>(maxVBVertices, primitiveRestart)
          : rtx::addressableVertexCount<uint16_t>(maxVBVertices, primitiveRestart);
        static constexpr uint32_t kMaxUnknownRangeVertices = 4u << 20;
        if (addressableVertices > kMaxUnknownRangeVertices) {
          ++m_submitRejectStats.vertexRangeRejected;
          return;
        }
        usedWholeVertexBufferFallback = true;
        drawVertexCount = addressableVertices;
      }
      hashCount = std::min(drawVertexCount, count);
    }
    if (drawVertexCount == 0)
      drawVertexCount = std::min(count, maxVBVertices > 0 ? maxVBVertices : count);
    if (hashCount == 0)
      hashCount = std::min(count, maxVBVertices);
    hashCount = std::min(hashCount, kMaxHashedVertices);
    geo.vertexCount = drawVertexCount;

    // PCSX2 delivers guest vertices after the game has already projected them
    // into the PS2 GS screen/depth domain. Reconstruct a canonical view-space
    // surface from that exact clip-space result. This preserves the on-screen
    // position/depth relationship for Remix capture and export without
    // pretending that unavailable guest world matrices were recovered.
    if (pcsx2PostTransformDraw) {
      const uint8_t* xyBase = reinterpret_cast<const uint8_t*>(
        posBuffer.mapPtr(posBuffer.offsetFromSlice()));
      const uint8_t* zBase = emulatorDepthBuffer.defined()
        ? reinterpret_cast<const uint8_t*>(emulatorDepthBuffer.mapPtr(
            emulatorDepthBuffer.offsetFromSlice()))
        : nullptr;
      const uint32_t xyStride = posBuffer.stride();
      const uint32_t zStride = emulatorDepthBuffer.stride();
      const size_t xyReadable = posBuffer.length() > posBuffer.offsetFromSlice()
        ? posBuffer.length() - posBuffer.offsetFromSlice() : 0;
      const size_t zReadable = emulatorDepthBuffer.length() > emulatorDepthBuffer.offsetFromSlice()
        ? emulatorDepthBuffer.length() - emulatorDepthBuffer.offsetFromSlice() : 0;

      if (xyBase == nullptr || zBase == nullptr || xyStride == 0 || zStride == 0
       || drawVertexCount > (1u << 20)) {
        ++m_submitRejectStats.positionFormatRejected;
        return;
      }

      const Matrix4 projection = effectiveEmulatorProjection(*emulatorMetadata);
      const float xScale = projection[0][0];
      const float yScale = projection[1][1];
      const float q = projection[2][2];
      const float nearQ = -projection[3][2];
      const float depthScale = std::isfinite(emulatorMetadata->depthScale)
                            && emulatorMetadata->depthScale > 0.0f
        ? emulatorMetadata->depthScale : std::ldexp(1.0f, -32);
      const VkDeviceSize dstSize = VkDeviceSize(drawVertexCount) * 12u;
      Rc<DxvkBuffer> dst = AcquireHostVisibleHelperBuffer(
        dstSize, "d3d11 rtx PCSX2 GS positions");
      float* out = dst != nullptr ? reinterpret_cast<float*>(dst->mapPtr(0)) : nullptr;
      if (out == nullptr) {
        ++m_submitRejectStats.positionFormatRejected;
        return;
      }

      for (uint32_t vertex = 0; vertex < drawVertexCount; ++vertex) {
        const size_t xyOffset = size_t(vertex) * xyStride;
        const size_t zOffset = size_t(vertex) * zStride;
        float viewX = 0.0f, viewY = 0.0f, viewZ = 0.1f;
        if (xyOffset + 4u <= xyReadable && zOffset + 4u <= zReadable) {
          const uint16_t* xy = reinterpret_cast<const uint16_t*>(xyBase + xyOffset);
          const uint32_t gsDepth = *reinterpret_cast<const uint32_t*>(zBase + zOffset);
          const float ndcX = (float(xy[0]) - 0.05f) * emulatorMetadata->vertexScale[0]
                           - emulatorMetadata->vertexOffset[0];
          const float ndcY = (float(xy[1]) - 0.05f) * -emulatorMetadata->vertexScale[1]
                           + emulatorMetadata->vertexOffset[1];
          const float ndcZ = std::clamp(float(gsDepth) * depthScale, 0.0f, 0.999999f);
          viewZ = std::clamp(nearQ / std::max(q - ndcZ, 1.0e-6f), 0.1f, 10000.0f);
          viewX = ndcX * viewZ / xScale;
          viewY = ndcY * viewZ / yScale;
        }
        out[vertex * 3u + 0u] = viewX;
        out[vertex * 3u + 1u] = viewY;
        out[vertex * 3u + 2u] = viewZ;
      }
      posBuffer = RasterBuffer(DxvkBufferSlice(dst, 0, dstSize),
                               0, 12u, VK_FORMAT_R32G32B32_SFLOAT);
      geo.positionBuffer = posBuffer;

      // Feed the camera-motion tracker: a stable mesh identity (guest texture
      // hash + counts + topology) plus a subsample of the reconstructed
      // view-space positions. Matching identities across frames give the
      // solver exact vertex correspondences.
      if (RtxOptions::Emulator::estimateCameraMotion()) {
        struct MeshKeySource {
          uint64_t textureHash;
          uint32_t vertexCount;
          uint32_t indexCount;
          uint32_t topology;
          uint32_t flags;
        };
        const MeshKeySource keySource = {
          emulatorMetadata->guestTextureHash,
          drawVertexCount,
          emulatorMetadata->indexCount,
          emulatorMetadata->topology,
          emulatorMetadata->flags,
        };
        s_emulatorCamera.addMeshSample(
          XXH3_64bits(&keySource, sizeof(keySource)), out, drawVertexCount);
      }

      // Decode the same texture coordinates consumed by PCSX2's tfx shader.
      // Fixed UV uses (UV-TextureOffset)*TextureScale; ST uses (ST-
      // TextureOffset)/Q. The output is ordinary float2 UV data understood by
      // Remix's interleaver and USD exporter.
      if ((emulatorMetadata->flags & remix::emulator::DrawFlagTextured)
       && tcBuffer.defined()) {
        const uint8_t* uvBase = reinterpret_cast<const uint8_t*>(
          tcBuffer.mapPtr(tcBuffer.offsetFromSlice()));
        const uint32_t uvStride = tcBuffer.stride();
        const size_t uvReadable = tcBuffer.length() > tcBuffer.offsetFromSlice()
          ? tcBuffer.length() - tcBuffer.offsetFromSlice() : 0;
        const uint8_t* qBase = emulatorQBuffer.defined()
          ? reinterpret_cast<const uint8_t*>(emulatorQBuffer.mapPtr(
              emulatorQBuffer.offsetFromSlice())) : nullptr;
        const uint32_t qStride = emulatorQBuffer.stride();
        const size_t qReadable = emulatorQBuffer.defined()
          && emulatorQBuffer.length() > emulatorQBuffer.offsetFromSlice()
          ? emulatorQBuffer.length() - emulatorQBuffer.offsetFromSlice() : 0;
        const bool fixedUv =
          (emulatorMetadata->flags & remix::emulator::DrawFlagFixedUv) != 0;
        const uint32_t uvElementSize = fixedUv ? 4u : 8u;

        if (uvBase != nullptr && uvStride > 0) {
          const VkDeviceSize uvSize = VkDeviceSize(drawVertexCount) * 8u;
          Rc<DxvkBuffer> uvDst = AcquireHostVisibleHelperBuffer(
            uvSize, "d3d11 rtx PCSX2 GS texcoords");
          float* uvOut = uvDst != nullptr
            ? reinterpret_cast<float*>(uvDst->mapPtr(0)) : nullptr;
          if (uvOut != nullptr) {
            for (uint32_t vertex = 0; vertex < drawVertexCount; ++vertex) {
              const size_t uvOffset = size_t(vertex) * uvStride;
              float u = 0.0f, v = 0.0f;
              if (uvOffset + uvElementSize <= uvReadable) {
                if (fixedUv) {
                  const uint16_t* uv = reinterpret_cast<const uint16_t*>(uvBase + uvOffset);
                  u = (float(uv[0]) - emulatorMetadata->textureOffset[0])
                    * emulatorMetadata->textureScale[0];
                  v = (float(uv[1]) - emulatorMetadata->textureOffset[1])
                    * emulatorMetadata->textureScale[1];
                } else {
                  const float* st = reinterpret_cast<const float*>(uvBase + uvOffset);
                  float perspectiveQ = 1.0f;
                  const size_t qOffset = size_t(vertex) * qStride;
                  if (qBase != nullptr && qStride > 0 && qOffset + 4u <= qReadable)
                    perspectiveQ = *reinterpret_cast<const float*>(qBase + qOffset);
                  if (!std::isfinite(perspectiveQ) || std::abs(perspectiveQ) < 1.0e-8f)
                    perspectiveQ = 1.0f;
                  u = (st[0] - emulatorMetadata->textureOffset[0]) / perspectiveQ;
                  v = (st[1] - emulatorMetadata->textureOffset[1]) / perspectiveQ;
                }
              }
              uvOut[vertex * 2u + 0u] = std::isfinite(u) ? u : 0.0f;
              uvOut[vertex * 2u + 1u] = std::isfinite(v) ? v : 0.0f;
            }
            tcBuffer = RasterBuffer(DxvkBufferSlice(uvDst, 0, uvSize),
                                    0, 8u, VK_FORMAT_R32G32_SFLOAT);
            geo.texcoordBuffer = tcBuffer;
          }
        }
      }
    }

    // Persistent geometry-contract telemetry. GPU-only index buffers are
    // common, but when their exact maximum is unavailable this draw must cover
    // the entire remaining shared VB. Those ranges are the primary capture
    // cost driver and were previously invisible except as unexplained 200k
    // vertex replays. Log the exact addressing contract, not texture/material
    // hashes, so one line is sufficient to diagnose offset/base mistakes.
    if (indexed && (usedWholeVertexBufferFallback || drawVertexCount >= 65536u)) {
      static uint32_t sLargeIndexedRangeLogCount = 0;
      if (sLargeIndexedRangeLogCount < 48) {
        ++sLargeIndexedRangeLogCount;
        Logger::info(str::format(
          "[D3D11Rtx][geometry-range] indexed draw: indices=", count,
          " startIndex=", start,
          " baseVertex=", base,
          " indexStride=", idxBuffer.stride(),
          " indexCpuVisible=", indexRangeCpuVisible ? 1 : 0,
          " exactMax=", indexRangeExact ? 1 : 0,
          " wholeVbFallback=", usedWholeVertexBufferFallback ? 1 : 0,
          " vertices=", drawVertexCount,
          " maxVbVertices=", maxVBVertices,
          " positionStride=", posBuffer.stride(),
          " positionSemanticOffset=", posBuffer.offsetFromSlice(),
          " topology=", static_cast<uint32_t>(vkTopology)));
      }
    }

    // DX11_V250_DYNAMIC_BUFFER_SNAPSHOT: DrawCallStates are queued and the RT
    // scene is recorded at EndFrame, but D3D11 dynamic buffers are renamed
    // (Map WRITE_DISCARD) many times per frame. A directly-bound slice resolves
    // to the physical backing CURRENT AT RECORD TIME - i.e. the bytes of a
    // LATER draw or a recycled slice - so every dynamic-buffer mesh reads
    // someone else's vertices or zeros. Zeros collapse the mesh to a single
    // point; foreign data renders as garbage. The rtx.useBuffersDirectly=false
    // copy path was never ported from D3D11 (the option has no consumer in this
    // fork), so implement the snapshot here: host-visible (renameable) sources
    // are copied at submit time, while their contents are the ones this draw
    // actually used. Device-local buffers cannot be CPU-renamed and stay
    // zero-copy. Interleaved layouts sharing one buffer are copied once.
    {
      struct SnapEntry {
        DxvkBuffer*  src = nullptr;
        VkDeviceSize srcOffset = 0;
        uint32_t     stride = 0;
        Rc<DxvkBuffer> copy;
        VkDeviceSize bytes = 0;
      };
      SnapEntry snapEntries[4];
      uint32_t snapCount = 0;
      static constexpr VkDeviceSize kMaxSnapshotBytes = 32ull << 20;

      auto snapshotVertexBuffer = [&](RasterBuffer& buf) {
        if (!buf.defined() || buf.stride() == 0 || drawVertexCount == 0)
          return;
        const void* srcMap = buf.mapPtr(0);
        if (srcMap == nullptr)
          return; // device-local: not renameable, safe to bind directly

        DxvkBuffer* srcBuf = buf.buffer().ptr();
        const VkDeviceSize srcOffset = buf.offset();
        const uint32_t stride = buf.stride();

        // Reuse a snapshot already taken for this (buffer, slice, stride) -
        // interleaved layouts share one buffer across several attributes.
        for (uint32_t i = 0; i < snapCount; ++i) {
          if (snapEntries[i].src == srcBuf && snapEntries[i].srcOffset == srcOffset && snapEntries[i].stride == stride) {
            buf = RasterBuffer(DxvkBufferSlice(snapEntries[i].copy, 0, snapEntries[i].bytes),
                               buf.offsetFromSlice(), stride, buf.vertexFormat());
            return;
          }
        }

        // Cover every byte the draw can address: full stride per vertex plus a
        // small margin for exotic layouts whose attribute offset exceeds the
        // stride, clamped to the actual slice extent.
        const VkDeviceSize wanted = VkDeviceSize(drawVertexCount) * stride + 256u;
        const VkDeviceSize bytes = std::min<VkDeviceSize>(wanted, buf.length());
        if (bytes == 0 || bytes > kMaxSnapshotBytes) {
          static uint32_t sSnapshotSkipLog = 0;
          if (bytes > kMaxSnapshotBytes && sSnapshotSkipLog < 4) {
            ++sSnapshotSkipLog;
            Logger::info(str::format("[D3D11Rtx] Dynamic vertex snapshot skipped (", bytes >> 20, " MiB exceeds cap); binding directly."));
          }
          return;
        }

        Rc<DxvkBuffer> copy = AcquireHostVisibleHelperBuffer(bytes, "d3d11 rtx dynamic vb snapshot");
        void* dst = copy != nullptr ? copy->mapPtr(0) : nullptr;
        if (dst == nullptr)
          return;
        std::memcpy(dst, srcMap, size_t(bytes));

        if (snapCount < 4)
          snapEntries[snapCount++] = { srcBuf, srcOffset, stride, copy, bytes };
        buf = RasterBuffer(DxvkBufferSlice(copy, 0, bytes), buf.offsetFromSlice(), stride, buf.vertexFormat());
      };

      if (!vertexPulled)
        snapshotVertexBuffer(posBuffer);
      snapshotVertexBuffer(nrmBuffer);
      snapshotVertexBuffer(tcBuffer);
      snapshotVertexBuffer(colBuffer);
      geo.positionBuffer = posBuffer;
      geo.normalBuffer   = nrmBuffer;
      geo.texcoordBuffer = tcBuffer;
      geo.color0Buffer   = colBuffer;

      if (indexed && idxBuffer.defined()) {
        const void* srcMap = idxBuffer.mapPtr(0);
        if (srcMap != nullptr) {
          const uint32_t idxStrideBytesSnap = std::max(idxBuffer.stride(), 1u);
          const VkDeviceSize wanted = VkDeviceSize(count) * idxStrideBytesSnap;
          const VkDeviceSize bytes = std::min<VkDeviceSize>(wanted, idxBuffer.length());
          if (bytes > 0 && bytes <= kMaxSnapshotBytes) {
            Rc<DxvkBuffer> copy = AcquireHostVisibleHelperBuffer(bytes, "d3d11 rtx dynamic ib snapshot");
            void* dst = copy != nullptr ? copy->mapPtr(0) : nullptr;
            if (dst != nullptr) {
              std::memcpy(dst, srcMap, size_t(bytes));
              idxBuffer = RasterBuffer(DxvkBufferSlice(copy, 0, bytes), 0, idxBuffer.stride(), idxBuffer.indexType());
              geo.indexBuffer = idxBuffer;
            }
          }
        }
      }
    }

    // DX11_V249_INTERLEAVER_FORMAT_NORMALIZATION: the interleaver (interleave_
    // geometry.h) decodes exactly six formats: R16G16_SFLOAT, R32G32_SFLOAT,
    // R32G32B32_SFLOAT, R32G32B32A32_SFLOAT, R8G8B8A8_UNORM and
    // A2B10G10R10_SNORM_PACK32. The capture accept-lists were wider, which
    // produced two whole bug classes:
    //  - POSITION in R16G16B16A16_SFLOAT (fmt 97, common in optimized engines):
    //    the interleaver refuses the whole geometry and leaves the interleaved
    //    vertex output garbage -> exploded triangle spikes.
    //  - TEXCOORD in half4 / 8-bit / 16-bit (u)norm: the interleaver skips the
    //    texcoord channel -> meshes bind textures with no UVs -> corrupt/flat
    //    texturing.
    // Normalize both CPU-side into interleaver-native helper buffers here, so
    // every submitted mesh is decodable by construction.
    {
      auto interleaverSupportsFloatFormat = [](VkFormat f) -> bool {
        switch (f) {
          case VK_FORMAT_R16G16_SFLOAT:
          case VK_FORMAT_R32G32_SFLOAT:
          case VK_FORMAT_R32G32B32_SFLOAT:
          case VK_FORMAT_R32G32B32A32_SFLOAT:
          case VK_FORMAT_R8G8B8A8_UNORM:
          case static_cast<VkFormat>(65): // A2B10G10R10_SNORM_PACK32
          // DX11_V286_HALF4_POSITIONS: the interleaver now decodes half4
          // directly (interleave_geometry.h), so device-local half4 position
          // buffers - Skyrim SE's entire static world, previously REJECTED
          // because CPU conversion needs a mappable buffer - bind directly.
          case static_cast<VkFormat>(97): // R16G16B16A16_SFLOAT
            return true;
          default:
            return false;
        }
      };

      static constexpr uint32_t kMaxFormatConvertVertices = 1u << 20;

      // --- POSITION ---
      if (!interleaverSupportsFloatFormat(geo.positionBuffer.vertexFormat())) {
        bool positionConverted = false;
        const VkFormat srcFmt = geo.positionBuffer.vertexFormat();
        const uint8_t* srcBase = reinterpret_cast<const uint8_t*>(
          geo.positionBuffer.mapPtr(geo.positionBuffer.offsetFromSlice()));
        const uint32_t srcStride = geo.positionBuffer.stride();
        const uint32_t srcSliceOff = geo.positionBuffer.offsetFromSlice();
        const size_t srcLen = geo.positionBuffer.length() > srcSliceOff
          ? geo.positionBuffer.length() - srcSliceOff
          : 0;

        if (srcFmt == static_cast<VkFormat>(97) // R16G16B16A16_SFLOAT
         && srcBase != nullptr && srcStride > 0
         && drawVertexCount > 0 && drawVertexCount <= kMaxFormatConvertVertices) {
          const VkDeviceSize dstSize = VkDeviceSize(drawVertexCount) * 12u;
          Rc<DxvkBuffer> dst = AcquireHostVisibleHelperBuffer(dstSize, "d3d11 rtx positions f16->f32");
          float* out = dst != nullptr ? reinterpret_cast<float*>(dst->mapPtr(0)) : nullptr;
          if (out != nullptr) {
            for (uint32_t v = 0; v < drawVertexCount; ++v) {
              const size_t off = size_t(v) * srcStride;
              float x = 0.f, y = 0.f, z = 0.f;
              if (off + 8 <= srcLen) {
                const uint16_t* h = reinterpret_cast<const uint16_t*>(srcBase + off);
                x = decodeFloat16(h[0]); y = decodeFloat16(h[1]); z = decodeFloat16(h[2]);
                if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) {
                  x = y = z = 0.f; // neutralize poisoned elements instead of exploding
                }
              }
              out[v * 3 + 0] = x; out[v * 3 + 1] = y; out[v * 3 + 2] = z;
            }
            geo.positionBuffer = RasterBuffer(DxvkBufferSlice(dst, 0, dstSize), 0, 12u, VK_FORMAT_R32G32B32_SFLOAT);
            posBuffer = geo.positionBuffer;
            positionConverted = true;
          }
        }

        if (!positionConverted) {
          // The interleaver would refuse this geometry and output garbage -
          // dropping the draw is strictly better than an exploded mesh.
          ++m_submitRejectStats.positionFormatRejected;
          return;
        }
      }

      // --- TEXCOORD ---
      if (geo.texcoordBuffer.defined()
       && !interleaverSupportsFloatFormat(geo.texcoordBuffer.vertexFormat())) {
        bool texcoordConverted = false;
        const VkFormat srcFmt = geo.texcoordBuffer.vertexFormat();
        const uint8_t* srcBase = reinterpret_cast<const uint8_t*>(
          geo.texcoordBuffer.mapPtr(geo.texcoordBuffer.offsetFromSlice()));
        const uint32_t srcStride = geo.texcoordBuffer.stride();
        const uint32_t srcSliceOff = geo.texcoordBuffer.offsetFromSlice();
        const size_t srcLen = geo.texcoordBuffer.length() > srcSliceOff
          ? geo.texcoordBuffer.length() - srcSliceOff
          : 0;

        // Decode a UV pair for the formats with well-defined semantics.
        // Integer formats use rtx.integerTexcoordScale - fixed-point UVs with an
        // engine-specific divisor (Saints Row IV: TEXCOORD0 = R16G16_SINT).
        const float intUvScale = integerTexcoordScale();
        auto decodeUv = [srcFmt, intUvScale](const uint8_t* src, float& u, float& v) -> bool {
          switch (srcFmt) {
            case VK_FORMAT_R16G16_SINT: {
              const int16_t* s = reinterpret_cast<const int16_t*>(src);
              u = s[0] * intUvScale; v = s[1] * intUvScale;
            } return true;
            case VK_FORMAT_R16G16_UINT: {
              const uint16_t* s = reinterpret_cast<const uint16_t*>(src);
              u = s[0] * intUvScale; v = s[1] * intUvScale;
            } return true;
            case static_cast<VkFormat>(97): { // R16G16B16A16_SFLOAT -> xy
              const uint16_t* h = reinterpret_cast<const uint16_t*>(src);
              u = decodeFloat16(h[0]); v = decodeFloat16(h[1]);
            } return true;
            case VK_FORMAT_R8G8_UNORM:
              u = src[0] / 255.0f; v = src[1] / 255.0f;
              return true;
            case VK_FORMAT_R8G8_SNORM: {
              const int8_t* s = reinterpret_cast<const int8_t*>(src);
              u = std::max(s[0] / 127.0f, -1.0f); v = std::max(s[1] / 127.0f, -1.0f);
            } return true;
            case VK_FORMAT_R16G16_UNORM:
            case VK_FORMAT_R16G16B16A16_UNORM: { // xy
              const uint16_t* s = reinterpret_cast<const uint16_t*>(src);
              u = s[0] / 65535.0f; v = s[1] / 65535.0f;
            } return true;
            case VK_FORMAT_R16G16_SNORM:
            case VK_FORMAT_R16G16B16A16_SNORM: { // xy
              const int16_t* s = reinterpret_cast<const int16_t*>(src);
              u = std::max(s[0] / 32767.0f, -1.0f); v = std::max(s[1] / 32767.0f, -1.0f);
            } return true;
            // DX11_V269: previously accepted by the capture but never decoded
            // here NOR supported by the interleaver - the channel was dropped
            // and textures rendered with no UVs (flat albedo).
            case VK_FORMAT_R8G8B8A8_SNORM: { // xy
              const int8_t* s = reinterpret_cast<const int8_t*>(src);
              u = std::max(s[0] / 127.0f, -1.0f); v = std::max(s[1] / 127.0f, -1.0f);
            } return true;
            case VK_FORMAT_R32G32_SINT: {
              const int32_t* s = reinterpret_cast<const int32_t*>(src);
              u = s[0] * intUvScale; v = s[1] * intUvScale;
            } return true;
            case VK_FORMAT_R32G32_UINT: {
              const uint32_t* s = reinterpret_cast<const uint32_t*>(src);
              u = s[0] * intUvScale; v = s[1] * intUvScale;
            } return true;
            default:
              return false;
          }
        };

        const uint32_t uvBytes =
          (srcFmt == VK_FORMAT_R8G8_UNORM || srcFmt == VK_FORMAT_R8G8_SNORM) ? 2u :
          (srcFmt == VK_FORMAT_R16G16_UNORM || srcFmt == VK_FORMAT_R16G16_SNORM
           || srcFmt == VK_FORMAT_R16G16_SINT || srcFmt == VK_FORMAT_R16G16_UINT
           || srcFmt == VK_FORMAT_R8G8B8A8_SNORM) ? 4u : 8u;

        float probeU = 0.f, probeV = 0.f;
        if (srcBase != nullptr && srcStride > 0 && srcLen >= uvBytes
         && drawVertexCount > 0 && drawVertexCount <= kMaxFormatConvertVertices
         && decodeUv(srcBase, probeU, probeV)) {
          const VkDeviceSize dstSize = VkDeviceSize(drawVertexCount) * 8u;
          Rc<DxvkBuffer> dst = AcquireHostVisibleHelperBuffer(dstSize, "d3d11 rtx texcoords ->f32");
          float* out = dst != nullptr ? reinterpret_cast<float*>(dst->mapPtr(0)) : nullptr;
          if (out != nullptr) {
            for (uint32_t v = 0; v < drawVertexCount; ++v) {
              const size_t off = size_t(v) * srcStride;
              float tu = 0.f, tv = 0.f;
              if (off + uvBytes <= srcLen) {
                decodeUv(srcBase + off, tu, tv);
                if (!std::isfinite(tu) || !std::isfinite(tv)) {
                  tu = tv = 0.f;
                }
              }
              out[v * 2 + 0] = tu; out[v * 2 + 1] = tv;
            }
            geo.texcoordBuffer = RasterBuffer(DxvkBufferSlice(dst, 0, dstSize), 0, 8u, VK_FORMAT_R32G32_SFLOAT);
            tcBuffer = geo.texcoordBuffer;
            texcoordConverted = true;
          }
        }

        if (!texcoordConverted) {
          // Formats without defined decode semantics here (e.g. integer UVs with
          // an engine-specific fixed-point scale): drop the channel explicitly.
          // Remix then uses its no-UV fallback, which renders flat but never
          // corrupts; the interleaver skipping an undecodable channel is the
          // same result reached less predictably.
          geo.texcoordBuffer = RasterBuffer();
          tcBuffer = RasterBuffer();
        }
      }

      // --- COLOR0 ---
      // DX11_V268_VERTEX_COLOR_FORMATS (supersedes the RGBA8-only V252 swap):
      // the interleaver's uint path accepts ONLY B8G8R8A8_UNORM. Convert every
      // other admitted COLOR0 format into that layout - RGBA8 (byte swizzle),
      // float4 (was silently dropped: games that bake lighting/tinting into
      // vertex colors washed out to white), half4 and unorm16. B8G8R8A8 is
      // the D3DCOLOR byte order the Remix shaders decode, matching what the
      // native d3d11 path always fed them.
      if (geo.color0Buffer.defined()
       && geo.color0Buffer.vertexFormat() != VK_FORMAT_B8G8R8A8_UNORM) {
        const VkFormat colFmt = geo.color0Buffer.vertexFormat();
        const uint32_t colElemBytes =
            colFmt == VK_FORMAT_R8G8B8A8_UNORM           ? 4u
          : colFmt == VK_FORMAT_A2B10G10R10_UNORM_PACK32 ? 4u
          : colFmt == VK_FORMAT_B10G11R11_UFLOAT_PACK32  ? 4u
          : colFmt == VK_FORMAT_R16G16B16A16_UNORM       ? 8u
          : colFmt == VK_FORMAT_R16G16B16A16_SFLOAT      ? 8u
          : colFmt == VK_FORMAT_R32G32B32_SFLOAT         ? 12u
          : colFmt == VK_FORMAT_R32G32B32A32_SFLOAT      ? 16u
          : 0u;

        bool colorConverted = false;
        const uint8_t* srcBase = reinterpret_cast<const uint8_t*>(
          geo.color0Buffer.mapPtr(geo.color0Buffer.offsetFromSlice()));
        const uint32_t srcStride = geo.color0Buffer.stride();
        const uint32_t srcSliceOff = geo.color0Buffer.offsetFromSlice();
        const size_t srcLen = geo.color0Buffer.length() > srcSliceOff
          ? geo.color0Buffer.length() - srcSliceOff
          : 0;

        // Non-DYNAMIC streams keep their contents between frames, so convert
        // once and reuse. DYNAMIC buffers are renamed or appended every frame
        // and are always converted. Entries are refreshed every
        // kColorConvertRefreshFrames to bound staleness from rare in-place
        // UpdateSubresource/CopyResource writes.
        // Indexed triangle-list draws whose vertex shader can be captured are
        // flattened by the post-VS capture, which discards COLOR0 (the IA
        // vertex domain no longer matches). Converting it first only burned
        // CPU - over 100 ms per frame in Fallout 4, where whole shared vertex
        // buffers were read back over PCIe for every draw.
        if (drawWillBeFlattenCaptured)
          srcBase = nullptr;

        static constexpr uint32_t kColorConvertRefreshFrames = 300u;
        static constexpr VkDeviceSize kMaxColorConvertCacheBytes = 128ull << 20;
        const uint32_t colorFrame = m_context->m_device->getCurrentFrameId();
        const bool colorCacheable = colSem != nullptr
          && m_context->m_state.ia.vertexBuffers[colSem->inputSlot].buffer != nullptr
          && m_context->m_state.ia.vertexBuffers[colSem->inputSlot].buffer->Desc()->Usage
               != D3D11_USAGE_DYNAMIC;
        uint64_t colorKey = 0;
        if (colorCacheable && srcBase != nullptr) {
          const uint64_t keyParts[5] = {
            uint64_t(reinterpret_cast<uintptr_t>(geo.color0Buffer.buffer().ptr())),
            uint64_t(reinterpret_cast<uintptr_t>(srcBase)),
            uint64_t(srcStride), uint64_t(drawVertexCount), uint64_t(colFmt) };
          colorKey = XXH3_64bits(keyParts, sizeof(keyParts));
        }
        if (colorKey != 0) {
          auto cached = m_colorConvertCache.find(colorKey);
          if (cached != m_colorConvertCache.end()
           && colorFrame - cached->second.convertedFrame < kColorConvertRefreshFrames) {
            cached->second.lastUsedFrame = colorFrame;
            geo.color0Buffer = RasterBuffer(DxvkBufferSlice(cached->second.buffer, 0, cached->second.size),
                                            0, 4u, VK_FORMAT_B8G8R8A8_UNORM);
            colBuffer = geo.color0Buffer;
            colorConverted = true;
          }
        }

        if (!colorConverted && colElemBytes != 0 && srcBase != nullptr && srcStride > 0
         && drawVertexCount > 0 && drawVertexCount <= kMaxFormatConvertVertices) {
          const VkDeviceSize dstSize = VkDeviceSize(drawVertexCount) * 4u;
          Rc<DxvkBuffer> dst;
          if (colorKey != 0) {
            // Bound the cache: drop entries unused for a while, then oldest.
            if (m_colorConvertCacheBytes + dstSize > kMaxColorConvertCacheBytes) {
              for (auto it = m_colorConvertCache.begin(); it != m_colorConvertCache.end();) {
                if (colorFrame - it->second.lastUsedFrame > 2u) {
                  m_colorConvertCacheBytes -= it->second.size;
                  it = m_colorConvertCache.erase(it);
                } else {
                  ++it;
                }
              }
            }
            if (m_colorConvertCacheBytes + dstSize <= kMaxColorConvertCacheBytes) {
              // Always convert into a fresh buffer: the previous one may still
              // be read by in-flight GPU work; DXVK keeps it alive until then.
              auto existing = m_colorConvertCache.find(colorKey);
              if (existing != m_colorConvertCache.end()) {
                m_colorConvertCacheBytes -= existing->second.size;
                m_colorConvertCache.erase(existing);
              }
              {
                DxvkBufferCreateInfo info;
                info.size   = dstSize;
                info.usage  = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT
                            | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
                info.stages = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
                info.access = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT;
                dst = m_context->m_device->createBuffer(info,
                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                  DxvkMemoryStats::Category::RTXBuffer, "d3d11 rtx cached color0");
              }
            }
          }
          const bool storeInCache = dst != nullptr;
          if (dst == nullptr)
            dst = AcquireHostVisibleHelperBuffer(dstSize, "d3d11 rtx color0 to bgra");
          uint8_t* out = dst != nullptr ? reinterpret_cast<uint8_t*>(dst->mapPtr(0)) : nullptr;
          if (out != nullptr && storeInCache) {
            auto& entry = m_colorConvertCache[colorKey];
            if (entry.buffer == nullptr)
              m_colorConvertCacheBytes += dstSize;
            entry.buffer = dst;
            entry.size = dstSize;
            entry.convertedFrame = colorFrame;
            entry.lastUsedFrame = colorFrame;
          }
          if (out != nullptr) {
            auto toByte = [](float c) -> uint8_t {
              if (!std::isfinite(c)) c = 1.0f;
              c = c < 0.0f ? 0.0f : (c > 1.0f ? 1.0f : c);
              return static_cast<uint8_t>(c * 255.0f + 0.5f);
            };
            for (uint32_t v = 0; v < drawVertexCount; ++v) {
              const size_t off = size_t(v) * srcStride;
              uint8_t r = 255, g = 255, b = 255, a = 255;
              if (off + colElemBytes <= srcLen) {
                const uint8_t* src = srcBase + off;
                switch (colFmt) {
                  case VK_FORMAT_R8G8B8A8_UNORM:
                    r = src[0]; g = src[1]; b = src[2]; a = src[3];
                    break;
                  case VK_FORMAT_R16G16B16A16_UNORM: {
                    const uint16_t* u = reinterpret_cast<const uint16_t*>(src);
                    r = uint8_t(u[0] >> 8); g = uint8_t(u[1] >> 8);
                    b = uint8_t(u[2] >> 8); a = uint8_t(u[3] >> 8);
                    break;
                  }
                  case VK_FORMAT_R16G16B16A16_SFLOAT: {
                    const uint16_t* h = reinterpret_cast<const uint16_t*>(src);
                    r = toByte(decodeFloat16(h[0])); g = toByte(decodeFloat16(h[1]));
                    b = toByte(decodeFloat16(h[2])); a = toByte(decodeFloat16(h[3]));
                    break;
                  }
                  case VK_FORMAT_R32G32B32A32_SFLOAT: {
                    const float* f = reinterpret_cast<const float*>(src);
                    r = toByte(f[0]); g = toByte(f[1]); b = toByte(f[2]); a = toByte(f[3]);
                    break;
                  }
                  case VK_FORMAT_R32G32B32_SFLOAT: {
                    const float* f = reinterpret_cast<const float*>(src);
                    r = toByte(f[0]); g = toByte(f[1]); b = toByte(f[2]);
                    break;
                  }
                  case VK_FORMAT_A2B10G10R10_UNORM_PACK32: {
                    // DXGI R10G10B10A2: R bits 0-9, G 10-19, B 20-29, A 30-31.
                    uint32_t w;
                    std::memcpy(&w, src, sizeof(w));
                    r = uint8_t((w & 1023u) >> 2);
                    g = uint8_t(((w >> 10) & 1023u) >> 2);
                    b = uint8_t(((w >> 20) & 1023u) >> 2);
                    a = uint8_t(((w >> 30) & 3u) * 85u);
                    break;
                  }
                  case VK_FORMAT_B10G11R11_UFLOAT_PACK32: {
                    // DXGI R11G11B10_FLOAT: R 11 bits (6e5m) at 0, G 11 at 11,
                    // B 10 bits (5e5m) at 22; no sign.
                    uint32_t w;
                    std::memcpy(&w, src, sizeof(w));
                    auto smallFloat = [](uint32_t v, uint32_t mantissaBits) {
                      const uint32_t e = v >> mantissaBits;
                      const uint32_t m = v & ((1u << mantissaBits) - 1u);
                      if (e == 0u)
                        return std::ldexp(float(m), -14 - int(mantissaBits));
                      if (e == 31u)
                        return m ? 0.0f : 65504.0f;
                      return std::ldexp(1.0f + float(m) / float(1u << mantissaBits), int(e) - 15);
                    };
                    r = toByte(smallFloat(w & 2047u, 6u));
                    g = toByte(smallFloat((w >> 11) & 2047u, 6u));
                    b = toByte(smallFloat((w >> 22) & 1023u, 5u));
                    break;
                  }
                  default:
                    break;
                }
              }
              out[v * 4 + 0] = b; out[v * 4 + 1] = g; out[v * 4 + 2] = r; out[v * 4 + 3] = a;
            }
            geo.color0Buffer = RasterBuffer(DxvkBufferSlice(dst, 0, dstSize), 0, 4u, VK_FORMAT_B8G8R8A8_UNORM);
            colBuffer = geo.color0Buffer;
            colorConverted = true;
          }
        }

        if (!colorConverted) {
          // Cannot convert (unmapped/huge/unknown): drop the channel so the
          // interleaver never sees a format it cannot decode. White fallback,
          // never corrupt.
          geo.color0Buffer = RasterBuffer();
          colBuffer = RasterBuffer();
        }
      }
    }

    // Object-space mesh bounding box. The D3D11 capture path never produced one,
    // so every feature that depends on it silently no-oped on all GPUs:
    // GPU Scene (significance culling projects the world bounds vs the sub-pixel
    // threshold) and Anti-Culling (keeps off-screen bounds in the frustum). Both
    // are enabled/available here, so compute the AABB - vendor-agnostic CPU
    // min/max over the drawn vertex range - only when a feature needs it. The
    // instance manager reads geo.boundingBox directly, and finalizeGeometry
    // BoundingBox() leaves it untouched unless a futureBoundingBox was scheduled,
    // so setting it here is sufficient. Fail-safe: an unmapped/unsupported/empty
    // position buffer leaves the bbox invalid, which keeps the instance.
    // Flattened captures replace the IA vertex domain with post-VS positions in
    // a different space; IA object-space bounds do not describe them. Skip the
    // sampling (missing bounds are treated as "keep") - it read ~1000 mapped
    // vertices per draw over PCIe on the game's render thread.
    if (RtxOptions::needsMeshBoundingBox() && posBuffer.stride() > 0 && drawVertexCount > 0
     && !drawWillBeFlattenCaptured && !vertexPulled) {
      const VkFormat posFmt = posBuffer.vertexFormat();
      const uint32_t elemBytes = positionElementBytes(posFmt);
      const uint8_t* posBase = elemBytes > 0
        ? reinterpret_cast<const uint8_t*>(posBuffer.mapPtr(posBuffer.offsetFromSlice()))
        : nullptr;
      if (posBase != nullptr) {
        const uint32_t stride = posBuffer.stride();
        // posBase points at offsetFromSlice() within the slice, so the readable
        // span from it is length() minus that attribute offset. Bounds-check reads
        // against this (not the full slice length) to avoid running off the buffer.
        const uint32_t posSliceOff = posBuffer.offsetFromSlice();
        const size_t posLen = posBuffer.length() > posSliceOff
          ? posBuffer.length() - posSliceOff
          : 0;
        // Sample evenly across the whole vertex range so the extent is captured
        // without iterating millions of vertices on the submit thread. Kept modest
        // because this runs per accepted draw.
        static constexpr uint32_t kMaxBBoxSampleVertices = 1024u;
        const uint32_t sampleCount = std::min(drawVertexCount, kMaxBBoxSampleVertices);
        float mn[3] = { std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), std::numeric_limits<float>::max() };
        float mx[3] = { -std::numeric_limits<float>::max(), -std::numeric_limits<float>::max(), -std::numeric_limits<float>::max() };
        bool anyValid = false;
        uint32_t sampledVerts = 0;

        // Bounds of non-DYNAMIC vertex data do not change between frames.
        // Sampling them every frame read ~1000 vertices per draw from mapped
        // (PCIe) memory - tens of ms per frame at Fallout 4's ~9600 draws.
        const bool boundsCacheable = posSem != nullptr
          && m_context->m_state.ia.vertexBuffers[posSem->inputSlot].buffer != nullptr
          && m_context->m_state.ia.vertexBuffers[posSem->inputSlot].buffer->Desc()->Usage
               != D3D11_USAGE_DYNAMIC;
        uint64_t boundsKey = 0;
        if (boundsCacheable) {
          const uint64_t keyParts[5] = {
            uint64_t(reinterpret_cast<uintptr_t>(posBuffer.buffer().ptr())),
            uint64_t(reinterpret_cast<uintptr_t>(posBase)),
            uint64_t(stride), uint64_t(drawVertexCount), uint64_t(posFmt) };
          boundsKey = XXH3_64bits(keyParts, sizeof(keyParts));
        }
        bool boundsFromCache = false;
        if (boundsKey != 0) {
          auto cached = m_boundsCache.find(boundsKey);
          if (cached != m_boundsCache.end()) {
            const BoundsCacheEntry& e = cached->second;
            for (int c = 0; c < 3; ++c) { mn[c] = e.mn[c]; mx[c] = e.mx[c]; }
            anyValid = e.anyValid;
            sampledVerts = e.sampledVerts;
            boundsFromCache = true;
          }
        }

        for (uint32_t i = 0; !boundsFromCache && i < sampleCount; ++i) {
          const uint32_t v = (sampleCount >= drawVertexCount || sampleCount <= 1)
            ? i
            : static_cast<uint32_t>(uint64_t(i) * uint64_t(drawVertexCount - 1) / uint64_t(sampleCount - 1));
          const size_t byteOff = size_t(v) * stride;
          if (byteOff + elemBytes > posLen)
            continue;
          float p[3];
          ++sampledVerts;
          // Post-normalization the format is always decodable here, so a false
          // return means the values are non-finite. Skip them for the bounds
          // (a mesh may carry unreferenced NaN padding), but count them.
          if (!decodePositionForBounds(posBase + byteOff, posFmt, p))
            continue;
          for (int c = 0; c < 3; ++c) {
            mn[c] = std::min(mn[c], p[c]);
            mx[c] = std::max(mx[c], p[c]);
          }
          anyValid = true;
        }
        // DX11_V249: every sampled position non-finite means the "positions"
        // are not positions at all (wrong stride/slot/offset - reading garbage
        // memory). Feeding them to the BLAS renders exploded spikes and risks a
        // GPU hang, so drop the draw. Requiring ALL samples to be garbage keeps
        // this fail-safe for meshes with sparse NaN padding.
        if (boundsKey != 0 && !boundsFromCache) {
          if (m_boundsCache.size() >= 65536u)
            m_boundsCache.clear();
          BoundsCacheEntry& e = m_boundsCache[boundsKey];
          for (int c = 0; c < 3; ++c) { e.mn[c] = mn[c]; e.mx[c] = mx[c]; }
          e.anyValid = anyValid;
          e.sampledVerts = sampledVerts;
        }
        if (sampledVerts >= 16 && !anyValid) {
          ++m_submitRejectStats.poisonedPositions;
          return;
        }
        if (anyValid) {
          // Bias the sampled extent outward a hair so under-sampling a large mesh
          // can never shrink an object below the sub-pixel cull threshold and drop
          // visible geometry. Symmetric about the centroid.
          for (int c = 0; c < 3; ++c) {
            const float center = 0.5f * (mn[c] + mx[c]);
            const float half = std::max(0.0f, 0.5f * (mx[c] - mn[c])) * 1.05f;
            mn[c] = center - half;
            mx[c] = center + half;
          }
          geo.boundingBox.minPos = Vector3(mn[0], mn[1], mn[2]);
          geo.boundingBox.maxPos = Vector3(mx[0], mx[1], mx[2]);
        }
      }
    }

    if (nrmBuffer.defined() && bwSem && biSem) {
      RasterBuffer nativeWeightBuffer = makeVertexBuffer(bwSem);
      RasterBuffer nativeIndexBuffer = makeVertexBuffer(biSem);

      if (nativeWeightBuffer.defined() && nativeIndexBuffer.defined()) {
        const uint8_t* weightBase = reinterpret_cast<const uint8_t*>(nativeWeightBuffer.mapPtr(nativeWeightBuffer.offsetFromSlice()));
        const uint8_t* indexBase = reinterpret_cast<const uint8_t*>(nativeIndexBuffer.mapPtr(nativeIndexBuffer.offsetFromSlice()));

        if (weightBase != nullptr && indexBase != nullptr && nativeWeightBuffer.stride() > 0 && nativeIndexBuffer.stride() > 0) {
          float sourceWeights[4] = {};
          uint32_t sourceWeightCount = 0;
          uint32_t sourceIndices[4] = {};
          uint32_t sourceIndexCount = 0;

          if (decodeBlendWeights(weightBase, nativeWeightBuffer.vertexFormat(), sourceWeights, sourceWeightCount)
           && decodeBlendIndices(indexBase, nativeIndexBuffer.vertexFormat(), sourceIndices, sourceIndexCount)) {
            const uint32_t configuredMaxBones = std::min<uint32_t>(4u, RtxOptions::limitedBonesPerVertex());
            skinBonesPerVertex = std::min({ sourceIndexCount, sourceWeightCount + 1u, configuredMaxBones });

            if (skinBonesPerVertex >= 2) {
              const uint32_t explicitWeightCount = skinBonesPerVertex - 1;
              const VkFormat normalizedWeightFormat = normalizedBlendWeightFormat(explicitWeightCount);

              if (normalizedWeightFormat != VK_FORMAT_UNDEFINED) {
                const VkDeviceSize weightBufferSize = VkDeviceSize(explicitWeightCount) * VkDeviceSize(drawVertexCount) * sizeof(float);
                const VkDeviceSize indexBufferSize = VkDeviceSize(drawVertexCount) * sizeof(uint32_t);

                Rc<DxvkBuffer> normalizedWeightBuffer = AcquireHostVisibleHelperBuffer(weightBufferSize, "d3d11 skinning weights");
                Rc<DxvkBuffer> normalizedIndexBuffer = AcquireHostVisibleHelperBuffer(indexBufferSize, "d3d11 skinning indices");

                if (normalizedWeightBuffer != nullptr && normalizedIndexBuffer != nullptr) {
                  float* dstWeights = reinterpret_cast<float*>(normalizedWeightBuffer->mapPtr(0));
                  uint8_t* dstIndices = reinterpret_cast<uint8_t*>(normalizedIndexBuffer->mapPtr(0));
                  bool normalizedOk = dstWeights != nullptr && dstIndices != nullptr;

                  for (uint32_t vertex = 0; normalizedOk && vertex < drawVertexCount; ++vertex) {
                    const uint8_t* srcWeights = reinterpret_cast<const uint8_t*>(nativeWeightBuffer.mapPtr(nativeWeightBuffer.offsetFromSlice() + size_t(vertex) * nativeWeightBuffer.stride()));
                    const uint8_t* srcIndices = reinterpret_cast<const uint8_t*>(nativeIndexBuffer.mapPtr(nativeIndexBuffer.offsetFromSlice() + size_t(vertex) * nativeIndexBuffer.stride()));
                    if (srcWeights == nullptr || srcIndices == nullptr) {
                      normalizedOk = false;
                      break;
                    }

                    float decodedWeights[4] = {};
                    uint32_t decodedWeightCount = 0;
                    uint32_t decodedIndices[4] = {};
                    uint32_t decodedIndexCount = 0;
                    if (!decodeBlendWeights(srcWeights, nativeWeightBuffer.vertexFormat(), decodedWeights, decodedWeightCount)
                     || !decodeBlendIndices(srcIndices, nativeIndexBuffer.vertexFormat(), decodedIndices, decodedIndexCount)) {
                      normalizedOk = false;
                      break;
                    }

                    if (decodedWeightCount + 1 < skinBonesPerVertex || decodedIndexCount < skinBonesPerVertex) {
                      normalizedOk = false;
                      break;
                    }

                    float explicitSum = 0.0f;
                    for (uint32_t bone = 0; bone < explicitWeightCount; ++bone) {
                      explicitSum += decodedWeights[bone];
                    }
                    if (explicitSum > 1.0f && explicitSum > 0.0f) {
                      const float invSum = 1.0f / explicitSum;
                      for (uint32_t bone = 0; bone < explicitWeightCount; ++bone) {
                        decodedWeights[bone] *= invSum;
                      }
                    }

                    for (uint32_t bone = 0; bone < explicitWeightCount; ++bone) {
                      dstWeights[vertex * explicitWeightCount + bone] = decodedWeights[bone];
                    }

                    std::array<uint8_t, 4> packedIndices = { 0, 0, 0, 0 };
                    for (uint32_t bone = 0; bone < skinBonesPerVertex; ++bone) {
                      if (decodedIndices[bone] > 255u) {
                        normalizedOk = false;
                        break;
                      }
                      packedIndices[bone] = uint8_t(decodedIndices[bone]);
                    }

                    if (!normalizedOk)
                      break;

                    std::memcpy(dstIndices + size_t(vertex) * sizeof(uint32_t), packedIndices.data(), sizeof(uint32_t));
                  }

                  if (normalizedOk) {
                    skinWeightBuffer = RasterBuffer(
                      DxvkBufferSlice { normalizedWeightBuffer, 0, weightBufferSize },
                      0,
                      explicitWeightCount * sizeof(float),
                      normalizedWeightFormat);
                    skinIndexBuffer = RasterBuffer(
                      DxvkBufferSlice { normalizedIndexBuffer, 0, indexBufferSize },
                      0,
                      sizeof(uint32_t),
                      VK_FORMAT_R8G8B8A8_USCALED);
                  } else {
                    skinBonesPerVertex = 0;
                  }
                }
              }
            }
          }
        }
      }
    }

    geo.blendWeightBuffer = skinWeightBuffer;
    geo.blendIndicesBuffer = skinIndexBuffer;
    geo.numBonesPerVertex = skinBonesPerVertex;

    geo.futureGeometryHashes = ComputeGeometryHashes(geo, drawVertexCount,
                                                     hashStart, hashCount);
    if (!geo.futureGeometryHashes.valid()) {
      ++m_submitRejectStats.geometryHashScheduleFailed;
      return;
    }

    Future<SkinningData> futureSkinningData;
    if (geo.blendWeightBuffer.defined() && geo.blendIndicesBuffer.defined() && geo.numBonesPerVertex >= 2) {
      static constexpr size_t kMaxSkinningScanBytes = 8192;
      auto cbRange = [](const D3D11ConstantBufferBinding& cb) -> std::pair<size_t, size_t> {
        const size_t bufSize = cb.buffer->Desc()->ByteWidth;
        const size_t base = size_t(cb.constantOffset) * 16;
        if (base >= bufSize)
          return { 0, 0 };
        size_t end = cb.constantCount > 0
          ? std::min(base + size_t(cb.constantCount) * 16, bufSize)
          : bufSize;
        if (end - base > kMaxSkinningScanBytes)
          end = base + kMaxSkinningScanBytes;
        return { base, end };
      };

      std::vector<SkinningConstantBufferSnapshot> skinningCbuffers;
      skinningCbuffers.reserve(D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT);
      for (uint32_t slot = 0; slot < D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT; ++slot) {
        const auto& cb = m_context->m_state.vs.constantBuffers[slot];
        if (cb.buffer == nullptr)
          continue;

        const auto mapped = cb.buffer->GetMappedSlice();
        const uint8_t* ptr = reinterpret_cast<const uint8_t*>(mapped.mapPtr);
        if (ptr == nullptr)
          continue;

        auto [baseOffset, endOffset] = cbRange(cb);
        if (endOffset <= baseOffset || endOffset - baseOffset < 128)
          continue;

        SkinningConstantBufferSnapshot snapshot;
        snapshot.slot = slot;
        snapshot.data.resize(endOffset - baseOffset);
        std::memcpy(snapshot.data.data(), ptr + baseOffset, snapshot.data.size());
        skinningCbuffers.push_back(std::move(snapshot));
      }

      if (!skinningCbuffers.empty()) {
        const RasterBuffer weightBuffer = geo.blendWeightBuffer;
        const RasterBuffer indexBufferForSkinning = geo.blendIndicesBuffer;
        const uint32_t bonesPerVertex = geo.numBonesPerVertex;
        const bool columnMajorSkinning = m_columnMajor;

        futureSkinningData = m_pGeometryWorkers->Schedule([
          weightBuffer,
          indexBufferForSkinning,
          drawVertexCount,
          bonesPerVertex,
          columnMajorSkinning,
          cbufferSnapshots = std::move(skinningCbuffers)
        ]() mutable -> SkinningData {
          SkinningData skinningData;

          const float* weightData = reinterpret_cast<const float*>(weightBuffer.mapPtr(weightBuffer.offsetFromSlice()));
          const uint8_t* indexData = reinterpret_cast<const uint8_t*>(indexBufferForSkinning.mapPtr(indexBufferForSkinning.offsetFromSlice()));
          if (weightData == nullptr || indexData == nullptr || bonesPerVertex < 2)
            return skinningData;

          const uint32_t explicitWeightCount = bonesPerVertex - 1;
          const uint32_t weightStride = weightBuffer.stride() / sizeof(float);
          const uint32_t indexStride = indexBufferForSkinning.stride();
          if (weightStride < explicitWeightCount || indexStride < bonesPerVertex)
            return skinningData;

          std::array<bool, 256> usedBoneMask = {};
          uint32_t minBoneIndex = 255u;
          uint32_t maxBoneIndex = 0u;
          std::vector<uint32_t> usedBoneIndices;
          usedBoneIndices.reserve(32);

          const uint32_t sampledVertexCount = std::min(drawVertexCount, kMaxSkinningVerticesToScan);
          auto sampleVertexIndex = [&](uint32_t sampleIndex) {
            if (sampledVertexCount <= 1 || drawVertexCount <= 1)
              return 0u;

            return uint32_t((uint64_t(sampleIndex) * uint64_t(drawVertexCount - 1))
              / uint64_t(sampledVertexCount - 1));
          };

          for (uint32_t sampleIndex = 0; sampleIndex < sampledVertexCount; ++sampleIndex) {
            const uint32_t vertex = sampleVertexIndex(sampleIndex);
            const float* vertexWeights = weightData + size_t(vertex) * weightStride;
            const uint8_t* vertexIndices = indexData + size_t(vertex) * indexStride;

            float explicitSum = 0.0f;
            for (uint32_t bone = 0; bone < explicitWeightCount; ++bone) {
              const float weight = vertexWeights[bone];
              if (!std::isfinite(weight))
                return SkinningData {};
              explicitSum += std::clamp(weight, 0.0f, 1.0f);
            }

            for (uint32_t bone = 0; bone < bonesPerVertex; ++bone) {
              const float weight = bone < explicitWeightCount
                ? std::clamp(vertexWeights[bone], 0.0f, 1.0f)
                : std::max(0.0f, 1.0f - explicitSum);
              if (weight <= 1.0e-5f)
                continue;

              const uint32_t index = vertexIndices[bone];
              if (!usedBoneMask[index]) {
                usedBoneMask[index] = true;
                usedBoneIndices.push_back(index);
                minBoneIndex = std::min(minBoneIndex, index);
                maxBoneIndex = std::max(maxBoneIndex, index);
              }
            }
          }

          if (usedBoneIndices.empty())
            return skinningData;

          auto scorePalette = [&](const SkinningConstantBufferSnapshot& snapshot, size_t startOffset, bool transposeMatrix) -> float {
            float score = 0.0f;
            uint32_t validCount = 0;
            uint32_t nonIdentityCount = 0;
            const size_t sampleCount = std::min<size_t>(usedBoneIndices.size(), 16);

            for (size_t i = 0; i < sampleCount; ++i) {
              const uint32_t boneIndex = usedBoneIndices[i];
              const size_t matrixOffset = startOffset + size_t(boneIndex) * 64;
              if (matrixOffset + 64 > snapshot.data.size())
                return -1.0e30f;

              Matrix4 matrix = readCbMatrix(snapshot.data.data(), matrixOffset, snapshot.data.size());
              if (transposeMatrix)
                matrix = transpose(matrix);
              if (!isSkinningMatrix(matrix))
                return -1.0e30f;

              ++validCount;
              if (!isIdentityExact(matrix))
                ++nonIdentityCount;
            }

            if (validCount == 0)
              return -1.0e30f;

            score += validCount * 4.0f;
            score += nonIdentityCount * 2.0f;
            score -= float(startOffset) / 256.0f;
            score -= float(snapshot.slot) * 0.5f;

            if (nonIdentityCount == 0)
              score -= 6.0f;

            return score;
          };

          const SkinningConstantBufferSnapshot* bestSnapshot = nullptr;
          size_t bestStartOffset = 0;
          bool bestTranspose = false;
          float bestScore = -1.0e30f;

          for (const auto& snapshot : cbufferSnapshots) {
            const size_t requiredBytes = (size_t(maxBoneIndex) + 1) * 64;
            if (snapshot.data.size() < requiredBytes)
              continue;

            for (size_t startOffset = 0; startOffset + requiredBytes <= snapshot.data.size(); startOffset += 16) {
              const float rowMajorScore = scorePalette(snapshot, startOffset, false);
              if (rowMajorScore > bestScore) {
                bestScore = rowMajorScore;
                bestSnapshot = &snapshot;
                bestStartOffset = startOffset;
                bestTranspose = false;
              }

              const float columnMajorScore = scorePalette(snapshot, startOffset, true);
              if (columnMajorScore > bestScore) {
                bestScore = columnMajorScore;
                bestSnapshot = &snapshot;
                bestStartOffset = startOffset;
                bestTranspose = true;
              }
            }
          }

          if (bestSnapshot == nullptr || bestScore < 4.0f)
            return skinningData;

          skinningData.numBonesPerVertex = bonesPerVertex;
          skinningData.minBoneIndex = minBoneIndex;
          skinningData.numBones = maxBoneIndex + 1;
          skinningData.pBoneMatrices.resize(skinningData.numBones, Matrix4());

          for (uint32_t boneIndex = 0; boneIndex < skinningData.numBones; ++boneIndex) {
            const size_t matrixOffset = bestStartOffset + size_t(boneIndex) * 64;
            if (matrixOffset + 64 > bestSnapshot->data.size())
              break;

            Matrix4 matrix = readCbMatrix(bestSnapshot->data.data(), matrixOffset, bestSnapshot->data.size());
            if (bestTranspose)
              matrix = transpose(matrix);
            if (!isSkinningMatrix(matrix))
              matrix = Matrix4();
            skinningData.pBoneMatrices[boneIndex] = matrix;
          }

          skinningData.computeHash();
          return skinningData;
        });
      }
    }

    DrawCallState dcs;
    dcs.geometryData     = geo;
    dcs.transformData    = ExtractTransforms();
    dcs.futureSkinningData = futureSkinningData;

    // A lifted 2D layer is placed by its captured clip position alone (see
    // TryCapturePositionsViaStreamOut): identity world and view, and the
    // synthetic 2D camera every lifted layer shares.
    if (lift2D) {
      DrawCallTransforms& t = dcs.transformData;
      t.objectToWorld = Matrix4();
      t.objectToView = Matrix4();
      t.worldToView = Matrix4();
      t.viewToProjection = Lift2DProjection();
      t.usedViewportFallbackProjection = true;
      t.cameraRelativeView = false;
      t.exactReplacementCamera = true;
      t.offscreenRenderTarget = false;
      t.instancesToObject.reset();
      dcs.allowMainCameraUpdate = true;
    }

    if (pcsx2PostTransformDraw) {
      // The reconstructed buffer above is canonical view space. worldToView
      // carries the published or estimated guest camera pose and objectToWorld
      // its rigid inverse, so objectToView stays exactly identity: on-screen
      // raster alignment is untouched while static geometry stays anchored in
      // a consistent world space (real motion vectors, stable temporal
      // accumulation/denoising, and a usable free camera - the behavior of a
      // native game). Without either camera source, fall back to the fixed
      // non-identity pose.
      dcs.transformData.viewToProjection =
        effectiveEmulatorProjection(*emulatorMetadata);
      if (s_emulatorPublishedCamera
       && (s_emulatorPublishedCamera->flags
           & remix::emulator::CameraFlagHasWorldToView)) {
        const Matrix4 worldToView =
          matrixFromAbiRows(s_emulatorPublishedCamera->worldToView);
        dcs.transformData.worldToView = worldToView;
        dcs.transformData.objectToWorld = inverse(worldToView);
      } else if (RtxOptions::Emulator::estimateCameraMotion()) {
        dcs.transformData.worldToView = s_emulatorCamera.worldToView();
        dcs.transformData.objectToWorld = s_emulatorCamera.viewToWorld();
      } else {
        constexpr float kCameraOffset = 0.001f;
        dcs.transformData.worldToView = Matrix4(Vector3(0.0f, 0.0f, kCameraOffset));
        dcs.transformData.objectToWorld = Matrix4(Vector3(0.0f, 0.0f, -kCameraOffset));
      }
      dcs.transformData.objectToView = Matrix4();
      dcs.transformData.usedViewportFallbackProjection = false;
      dcs.transformData.cameraRelativeView = false;
      dcs.transformData.offscreenRenderTarget = false;
    } else if (authenticatedEmulatorDraw
            && (emulatorMetadata->coordinateSpace == remix::emulator::CoordinateSpace::View
             || emulatorMetadata->coordinateSpace == remix::emulator::CoordinateSpace::World)
            && s_emulatorPublishedCamera
            && (s_emulatorPublishedCamera->flags & remix::emulator::CameraFlagHasWorldToView)
            && (s_emulatorPublishedCamera->flags & remix::emulator::CameraFlagHasViewToProjection)) {
      // Emulators that publish real guest matrices (e.g. GC/Wii XF state from
      // a Dolphin D3D11 publisher, PSP GE matrices) alongside view- or
      // world-space vertex data: adopt them directly, exactly like a native
      // game's captured camera. Requires the emulator to run its D3D11
      // backend - this ABI travels over ID3D11DeviceContext private data.
      const Matrix4 worldToView =
        matrixFromAbiRows(s_emulatorPublishedCamera->worldToView);
      dcs.transformData.viewToProjection =
        matrixFromAbiRows(s_emulatorPublishedCamera->viewToProjection);
      dcs.transformData.worldToView = worldToView;
      if (emulatorMetadata->coordinateSpace == remix::emulator::CoordinateSpace::View) {
        dcs.transformData.objectToWorld = inverse(worldToView);
        dcs.transformData.objectToView = Matrix4();
      } else {
        dcs.transformData.objectToWorld = Matrix4();
        dcs.transformData.objectToView = worldToView;
      }
      dcs.transformData.usedViewportFallbackProjection = false;
      dcs.transformData.cameraRelativeView = false;
      dcs.transformData.offscreenRenderTarget = false;
    }

    // Apply per-instance world transform when submitting instanced draws.
    if (instanceTransform) {
      dcs.transformData.objectToWorld = *instanceTransform;
      // Recompute objectToView with the per-instance world matrix.
      dcs.transformData.objectToView = dcs.transformData.objectToWorld;
      if (!isIdentityExact(dcs.transformData.worldToView))
        dcs.transformData.objectToView = dcs.transformData.worldToView * dcs.transformData.objectToWorld;
    }

    // Mirrored views (planar water/mirror reflection passes: Glacier mirrors,
    // FC4 water reflection, CRYENGINE $WaterVolumeRefl) can pass the extent
    // gate at half resolution. Their view has the opposite handedness of the
    // main camera, whose sign is learned from many scene draws first (some
    // engines' main view is itself det -1, e.g. Fallout 4).
    if (!m_abDisableEngineKnowledge && !dcs.transformData.offscreenRenderTarget
     && !isIdentityExact(dcs.transformData.worldToView)) {
      const Matrix4& v = dcs.transformData.worldToView;
      const float det = v[0][0] * (v[1][1] * v[2][2] - v[2][1] * v[1][2])
                      - v[1][0] * (v[0][1] * v[2][2] - v[2][1] * v[0][2])
                      + v[2][0] * (v[0][1] * v[1][2] - v[1][1] * v[0][2]);
      if (std::isfinite(det) && std::abs(det) > 0.5f) {
        const int sign = det > 0.0f ? 1 : -1;
        if (m_mainViewDetSign == 0) {
          uint32_t& same = sign > 0 ? m_viewDetPositiveVotes : m_viewDetNegativeVotes;
          const uint32_t other = sign > 0 ? m_viewDetNegativeVotes : m_viewDetPositiveVotes;
          if (++same >= 256u && other * 16u < same)
            m_mainViewDetSign = sign;
        } else if (sign != m_mainViewDetSign) {
          dcs.transformData.offscreenRenderTarget = true;
          ++m_submitRejectStats.mirroredViewSkipped;
        }
      }
    }

    // Reflection/probe/cubemap passes must remain native offscreen work. Their
    // geometry is drawn again by the main pass; admitting the auxiliary copy
    // adds a second, camera-incompatible instance to the primary RT scene and
    // lets its projection move world geometry with the probe camera.
    if (dcs.transformData.offscreenRenderTarget) {
      static uint32_t sOffscreenGeometrySkipLogs = 0;
      if (sOffscreenGeometrySkipLogs++ < 16u) {
        Logger::info(str::format(
          "[D3D11Rtx] Skipping auxiliary-camera geometry from the primary RT scene: drawId=",
          dcs.drawCallID, " count=", count));
      }
      return;
    }

    // DX11_V278_MIRRORED_TRANSFORM_WINDING (generalized from FO4-Remix's
    // "preserve mirror transforms in batched bases" inside-out-geometry fix):
    // a mirrored placement (negative-determinant world transform - games
    // mirror batched statics constantly: left/right prop variants, reflected
    // room chunks) flips triangle winding. Left uncorrected, the mesh renders
    // INSIDE-OUT in the RT scene: viewed from outside it back-face culls away
    // or shades black. Flip the declared front face for negative-determinant
    // placements so mirrored instances shade correctly. The 3x3 determinant
    // is transpose-invariant, so this is matrix-layout-proof.
    {
      const Matrix4& o2w = dcs.transformData.objectToWorld;
      const float det3 =
          o2w[0][0] * (o2w[1][1] * o2w[2][2] - o2w[1][2] * o2w[2][1])
        - o2w[0][1] * (o2w[1][0] * o2w[2][2] - o2w[1][2] * o2w[2][0])
        + o2w[0][2] * (o2w[1][0] * o2w[2][1] - o2w[1][1] * o2w[2][0]);
      if (det3 < 0.0f) {
        dcs.geometryData.frontFace =
          (dcs.geometryData.frontFace == VK_FRONT_FACE_COUNTER_CLOCKWISE)
            ? VK_FRONT_FACE_CLOCKWISE
            : VK_FRONT_FACE_COUNTER_CLOCKWISE;
      }
    }

    // Let processCameraData() classify the camera from the matrices.
    // Hardcoding Main would bypass Remix's sky/portal/shadow detection.
    dcs.cameraType       = CameraType::Unknown;
    dcs.usesVertexShader = (m_context->m_state.vs.shader != nullptr);
    dcs.usesPixelShader  = (m_context->m_state.ps.shader != nullptr);

    // DX11_V277_REAL_SHADER_MODEL: report the ACTUAL shader model parsed from
    // each shader's DXBC version token (4.0 - 5.1). Modern D3D11 games ship
    // SM 5.x; the previous hardcoded {4, 0} misreported every draw.
    if (dcs.usesVertexShader) {
      const D3D11CommonShader* commonVs = m_context->m_state.vs.shader->GetCommonShader();
      dcs.vertexShaderInfo = ShaderProgramInfo{
        commonVs->GetShaderModelMajor(), commonVs->GetShaderModelMinor() };
      // Lets camera-manager diagnostics correlate a decision back to the
      // originating shader (cached at shader creation, not hashed per draw).
      dcs.programmableVertexShaderBytecodeHash = commonVs->GetBytecodeHash();
    }
    if (dcs.usesPixelShader) {
      const D3D11CommonShader* commonPs = m_context->m_state.ps.shader->GetCommonShader();
      dcs.pixelShaderInfo = ShaderProgramInfo{
        commonPs->GetShaderModelMajor(), commonPs->GetShaderModelMinor() };
    }
    dcs.zWriteEnable     = zWriteEnable;
    dcs.zEnable          = zEnable;
    dcs.stencilEnabled   = stencilEnabled;
    dcs.drawCallID       = m_drawCallID++;

    // Viewport depth range from D3D11_VIEWPORT.MinDepth / MaxDepth.
    if (m_context->m_state.rs.numViewports > 0) {
      const auto& vp = m_context->m_state.rs.viewports[0];
      dcs.minZ = std::clamp(vp.MinDepth, 0.0f, 1.0f);
      dcs.maxZ = std::clamp(vp.MaxDepth, 0.0f, 1.0f);
    } else {
      dcs.minZ = 0.0f;
      dcs.maxZ = 1.0f;
    }

    // D3D11 has no legacy fog â€” engines bake fog into shaders.
    // FogState defaults to mode=0 (none), which is correct.

    const auto isLikelyScreenSpaceCompositePass = [&]() {
      const bool fallbackSpace = dcs.transformData.usedViewportFallbackProjection
        && isIdentityExact(dcs.transformData.objectToWorld)
        && isIdentityExact(dcs.transformData.worldToView);
      const bool unoccludedFullscreenDraw = count <= 6u && !zWriteEnable
        && (!zEnable || depthComparison == D3D11_COMPARISON_ALWAYS);
      if (!fallbackSpace && !unoccludedFullscreenDraw)
        return false;

      const D3D11CommonShader* pixelShader = m_context->m_state.ps.shader != nullptr
        ? m_context->m_state.ps.shader->GetCommonShader() : nullptr;
      const bool knownResources = pixelShader && pixelShader->HasCompleteSampledResourceProfile();
      // A recovered camera can remain bound during post processing. In that
      // case require actual sampled render targets and a depth-independent
      // fullscreen draw; a camera-extraction failure is not required.
      if (!fallbackSpace && !knownResources)
        return false;

      const bool likelyFullscreenPrimitive = count <= 12;
      const bool likelyScreenSpaceDepthState = !zEnable || !zWriteEnable;
      if (!likelyFullscreenPrimitive && !likelyScreenSpaceDepthState)
        return false;

      // Fullscreen triangles/quads with only a synthesized camera and no
      // object/view transform are almost always post-process or UI composite
      // passes rather than stable scene geometry.
      // Reject these even if the bound textures are not exact RT aliases.
      const auto& omState = m_context->m_state.om;
      std::array<DxvkImage*, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT> boundRTImages = {};
      uint32_t rtWidth = 0;
      uint32_t rtHeight = 0;
      for (uint32_t rt = 0; rt < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++rt) {
        auto* rtv = omState.renderTargetViews[rt].ptr();
        if (!rtv)
          continue;

        Rc<DxvkImageView> rtvView = rtv->GetImageView();
        if (rtvView == nullptr)
          continue;

        boundRTImages[rt] = rtvView->image().ptr();
        if (rt == 0) {
          rtWidth = rtvView->image()->info().extent.width;
          rtHeight = rtvView->image()->info().extent.height;
        }
      }

      auto isBlockCompressed = [](DXGI_FORMAT fmt) {
        return (fmt >= DXGI_FORMAT_BC1_TYPELESS && fmt <= DXGI_FORMAT_BC1_UNORM_SRGB)
            || (fmt >= DXGI_FORMAT_BC2_TYPELESS && fmt <= DXGI_FORMAT_BC2_UNORM_SRGB)
            || (fmt >= DXGI_FORMAT_BC3_TYPELESS && fmt <= DXGI_FORMAT_BC3_UNORM_SRGB)
            || (fmt >= DXGI_FORMAT_BC4_TYPELESS && fmt <= DXGI_FORMAT_BC4_SNORM)
            || (fmt >= DXGI_FORMAT_BC5_TYPELESS && fmt <= DXGI_FORMAT_BC5_SNORM)
            || (fmt >= DXGI_FORMAT_BC6H_TYPELESS && fmt <= DXGI_FORMAT_BC7_UNORM_SRGB);
      };

      uint32_t candidateCount = 0;
      uint32_t rtSizedCount = 0;
      uint32_t contentLikeCount = 0;
      bool onlyRenderedInputs = true;

      for (uint32_t slot = 0; slot < D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT; ++slot) {
        if (knownResources && !pixelShader->SamplesResourceSlot(slot))
          continue;
        D3D11ShaderResourceView* srv = m_context->m_state.ps.shaderResources.views[slot].ptr();
        if (!srv || srv->GetResourceType() != D3D11_RESOURCE_DIMENSION_TEXTURE2D)
          continue;

        Rc<DxvkImageView> view = srv->GetImageView();
        if (view == nullptr)
          continue;

        const auto& imgInfo = view->image()->info();
        if (imgInfo.extent.width <= 2 && imgInfo.extent.height <= 2)
          continue;

        ++candidateCount;
        onlyRenderedInputs &= (imgInfo.usage &
          (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)) != 0;

        D3D11_SHADER_RESOURCE_VIEW_DESC1 srvDesc = {};
        srv->GetDesc1(&srvDesc);

        const bool matchesRT = rtWidth > 0 && rtHeight > 0
          && imgInfo.extent.width == rtWidth
          && imgInfo.extent.height == rtHeight;
        const bool hasMips = imgInfo.mipLevels > 1;
        const bool bc = isBlockCompressed(srvDesc.Format);

        bool isCurrentRT = false;
        for (DxvkImage* boundRT : boundRTImages) {
          if (boundRT == view->image().ptr()) {
            isCurrentRT = true;
            break;
          }
        }

        if (matchesRT || isCurrentRT)
          ++rtSizedCount;
        if (bc || hasMips || (!matchesRT && !isCurrentRT))
          ++contentLikeCount;
      }

      if (candidateCount == 0)
        return fallbackSpace && likelyFullscreenPrimitive && likelyScreenSpaceDepthState;

      return rtSizedCount == candidateCount && contentLikeCount == 0
        && (fallbackSpace || onlyRenderedInputs);
    };

    const auto isLikelyScreenSpaceUiPass = [&]() {
      // DX11_V306_UI_NOT_GATED_ON_CAMERA_FAILURE: this classifier used to bail
      // unless usedViewportFallbackProjection was set, i.e. unless Remix had
      // FAILED to recover a camera for the draw. That made UI detection
      // impossible for any engine that supplies real matrices for its HUD/menu
      // (Skyrim does): the fallback never engages, so the classifier returned
      // false on the first line and no draw was ever routed to the raster UI
      // layer. Field logs show the consequence directly - ui=0, rasterUi=0,
      // uiMidInject=0, uiPassThrough=0 and not one [ui-layer] line in a whole
      // session, while menu UI was instead swept into the RT scene and picked
      // up whatever texture category matched (it got tagged as sky).
      //
      // Camera-recovery failure is evidence, not a requirement. The remaining
      // conditions below are a projection-independent screen-space signature
      // and are what actually identify UI:
      //   o objectToWorld AND worldToView both EXACTLY identity - the geometry
      //     is already in view/clip space, which is what a screen pass emits
      //   o no depth write (a composited overlay does not author scene depth)
      //   o blending explicitly enabled
      //   o no bound SRV that is the current RT or matches its size
      //   o every candidate texture non-mipped and clamp-sampled (atlas signal)
      //
      // Post-transform-captured WORLD geometry can also present identity
      // matrices in this fork, so it is worth being explicit about why it does
      // not collide: it writes depth and is typically opaque (excluded by the
      // depth/blend tests), and world textures are mipped (excluded by the
      // atlas test). Keeping the fallback as a *sufficient* signal preserves
      // the original behaviour for engines that do trip it.
      // DX11_V319_UI_CLASSIFIER_PROBE: name the condition that rejected a
      // plausible UI draw.
      //
      // Every field log so far reports ui=0, rasterUi=0, uiMidInject=0 and
      // uiPassThrough=0 in EVERY game - Skyrim, SpongeBob, Little Nightmares II,
      // Mine Souls III, Granny - so this classifier has never once matched and
      // the game's UI is going into the ray-traced scene instead of onto its own
      // layer. The conditions below are ANDed and none of them says which one
      // failed, so the log cannot distinguish "no UI in this frame" from "UI
      // present but rejected at step 4".
      //
      // That distinction is the whole problem: the transform test can never pass
      // in a game whose camera Remix recovered (worldToView holds the real view
      // for every draw, UI included), while a game that DOES present identity
      // matrices must be failing something further down. Those need opposite
      // fixes, and this classifier has already been revised once on a guess
      // (DX11_V306). Report the reason, then fix what the reports actually say.
      //
      // Only draws that already look like plausible UI candidates are reported,
      // and the count is bounded, so this cannot flood a frame.
      const bool uiProbeCandidate = !zWriteEnable && count >= 3 && count <= 262144;
      static uint32_t sUiRejectLogCount = 0;
      auto reportUiReject = [&](const char* reason) {
        if (!uiProbeCandidate || sUiRejectLogCount >= 48u)
          return;
        ++sUiRejectLogCount;
        Logger::info(str::format(
          "[D3D11Rtx][ui-probe] rejected UI candidate: reason=", reason,
          " drawId=", dcs.drawCallID,
          " count=", count,
          " identityWorld=", isIdentityExact(dcs.transformData.objectToWorld) ? 1 : 0,
          " identityView=", isIdentityExact(dcs.transformData.worldToView) ? 1 : 0,
          " zEnable=", dcs.zEnable ? 1 : 0,
          " zWrite=", zWriteEnable ? 1 : 0,
          " fallbackCamera=", dcs.transformData.usedViewportFallbackProjection ? 1 : 0,
          " textured=", dcs.materialData.usesTexture() ? 1 : 0));
      };

      const bool screenSpaceTransforms =
        isIdentityExact(dcs.transformData.objectToWorld)
        && isIdentityExact(dcs.transformData.worldToView);

      if (!screenSpaceTransforms) {
        reportUiReject("non-identity-transforms");
        return false;
      }

      // Unity batches a complete Canvas into one indexed draw. Granny's menu,
      // for example, is 5,040 indices; a fullscreen-quad-only limit silently
      // admitted that Canvas as world geometry. Keep a generous corruption
      // guard without assuming that UI is always one quad.
      if (count < 3 || count > 262144) {
        reportUiReject("index-count-out-of-range");
        return false;
      }

      // A screen Canvas is composited and does not write scene depth. Requiring
      // the actual blend state keeps opaque fallback-camera world geometry out
      // of this classifier even when matrix recovery is incomplete.
      if (zWriteEnable) {
        reportUiReject("writes-depth");
        return false;
      }

      D3D11BlendState* blendState = m_context->m_state.om.cbState;
      if (blendState == nullptr) {
        reportUiReject("no-blend-state");
        return false;
      }

      D3D11_BLEND_DESC1 blendDesc = {};
      blendState->GetDesc1(&blendDesc);
      if (!blendDesc.RenderTarget[0].BlendEnable) {
        reportUiReject("blending-disabled");
        return false;
      }

      const auto& omState = m_context->m_state.om;
      std::array<DxvkImage*, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT> boundRTImages = {};
      uint32_t rtWidth = 0;
      uint32_t rtHeight = 0;
      for (uint32_t rt = 0; rt < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++rt) {
        auto* rtv = omState.renderTargetViews[rt].ptr();
        if (!rtv)
          continue;

        Rc<DxvkImageView> rtvView = rtv->GetImageView();
        if (rtvView == nullptr)
          continue;

        boundRTImages[rt] = rtvView->image().ptr();
        if (rt == 0) {
          rtWidth = rtvView->image()->info().extent.width;
          rtHeight = rtvView->image()->info().extent.height;
        }
      }

      uint32_t candidateCount = 0;
      uint32_t uiLikeCount = 0;

      for (uint32_t slot = 0; slot < D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT; ++slot) {
        D3D11ShaderResourceView* srv = m_context->m_state.ps.shaderResources.views[slot].ptr();
        if (!srv || srv->GetResourceType() != D3D11_RESOURCE_DIMENSION_TEXTURE2D)
          continue;

        Rc<DxvkImageView> view = srv->GetImageView();
        if (view == nullptr)
          continue;

        const auto& imgInfo = view->image()->info();
        if (imgInfo.extent.width <= 2 && imgInfo.extent.height <= 2)
          continue;

        ++candidateCount;

        D3D11_SHADER_RESOURCE_VIEW_DESC1 srvDesc = {};
        srv->GetDesc1(&srvDesc);

        const bool matchesRT = rtWidth > 0 && rtHeight > 0
          && imgInfo.extent.width == rtWidth
          && imgInfo.extent.height == rtHeight;
        const bool hasMips = imgInfo.mipLevels > 1;
        bool isCurrentRT = false;
        for (DxvkImage* boundRT : boundRTImages) {
          if (boundRT == view->image().ptr()) {
            isCurrentRT = true;
            break;
          }
        }

        if (matchesRT || isCurrentRT) {
          reportUiReject(matchesRT ? "srv-matches-rt-size" : "srv-is-current-rt");
          return false;
        }

        D3D11SamplerState* samp = m_context->m_state.ps.samplers[slot];
        bool clampSampler = true;
        if (samp != nullptr) {
          D3D11_SAMPLER_DESC sampDesc = {};
          samp->GetDesc(&sampDesc);
          auto isWrapMode = [](D3D11_TEXTURE_ADDRESS_MODE mode) {
            return mode == D3D11_TEXTURE_ADDRESS_WRAP || mode == D3D11_TEXTURE_ADDRESS_MIRROR;
          };
          clampSampler = !isWrapMode(sampDesc.AddressU) && !isWrapMode(sampDesc.AddressV);
        }

        // UI atlases may legitimately be BC-compressed (the observed Unity
        // menu atlas is BC1). Compression says nothing about coordinate space;
        // non-mipped clamp sampling is the reliable atlas signal here.
        if (!hasMips && clampSampler)
          ++uiLikeCount;
      }

      // The atlas test is the last gate, and the one most likely to reject a
      // real UI draw silently: a single mipped or wrap-sampled texture anywhere
      // in the batch disqualifies the whole draw. Report the counts so the log
      // says how close it came instead of just "no".
      const bool atlasSignature = candidateCount > 0 && uiLikeCount == candidateCount;
      if (!atlasSignature) {
        reportUiReject(candidateCount == 0 ? "no-texture-candidates"
                                           : "textures-not-atlas-like");
      }

      return atlasSignature;
    };

    const bool renderDocAttached = isRenderDocAttached();
    auto& sceneManager = m_context->m_device->getCommon()->getSceneManager();
    const auto& cameraManager = sceneManager.getCameraManager();
    const bool hasStableSceneCamera = cameraManager.isCameraValid(CameraType::Main)
      || cameraManager.getLastSetCameraType() != CameraType::Unknown
      || cameraManager.hasSeenRealMainCamera();
    const bool viewportFallbackAfterRealCamera =
      dcs.transformData.usedViewportFallbackProjection
      && cameraManager.hasSeenRealMainCamera();
    const bool allowViewportFallbackScreenSpaceReject = !renderDocAttached
      && dcs.transformData.usedViewportFallbackProjection
      && (hasStableSceneCamera
       || viewportFallbackAfterRealCamera
       || m_submitRejectStats.accepted > 0
       || sceneManager.isPreviousFrameSceneAvailable());

    const bool deferViewportFallbackScreenSpaceReject =
      dcs.transformData.usedViewportFallbackProjection
      && !allowViewportFallbackScreenSpaceReject;

    if (deferViewportFallbackScreenSpaceReject) {
      static uint32_t sDeferredFallbackScreenRejectLogCount = 0;
      if (sDeferredFallbackScreenRejectLogCount < 8) {
        ++sDeferredFallbackScreenRejectLogCount;
        Logger::info(str::format(
          "[D3D11Rtx] Deferring viewport-fallback screen-space rejection until a stable scene camera exists (count=",
          count,
          ", zEnable=",
          zEnable ? 1 : 0,
          ", zWrite=",
          zWriteEnable ? 1 : 0,
          ")"));
      }
    }

    if (!lift2D && !renderDocAttached
      && (allowViewportFallbackScreenSpaceReject || !dcs.transformData.usedViewportFallbackProjection)
      && isLikelyScreenSpaceCompositePass()) {
      ++m_submitRejectStats.compositeSkip;
      static uint32_t sScreenSpaceCompositeSkipLogCount = 0;
      if (sScreenSpaceCompositeSkipLogCount < 8) {
          ++sScreenSpaceCompositeSkipLogCount;
        Logger::info(str::format(
          "[D3D11Rtx] Preserving screen-space composite as raster: camera fallback or depth-independent sampled render targets (count=",
          count,
          ", zEnable=",
          zEnable ? 1 : 0,
          ", zWrite=",
          zWriteEnable ? 1 : 0,
          ")"));
      }
      return;
    }

    // Strong per-draw Canvas evidence is safe before a scene camera exists and
    // is precisely what startup/menu frames need. Composite heuristics remain
    // camera-gated above, but UI must not be admitted as the first fake scene.
    const bool likelyScreenSpaceUiPass =
      !lift2D && !renderDocAttached && isLikelyScreenSpaceUiPass();
    if (likelyScreenSpaceUiPass) {
      static uint32_t sScreenSpaceUiSkipLogCount = 0;
      if (sScreenSpaceUiSkipLogCount < 8) {
        ++sScreenSpaceUiSkipLogCount;
        Logger::info(str::format(
          "[D3D11Rtx] Detected screen-space UI pass for WorldUI routing: identity object/view transforms + no depth write + blending + atlas-style textures (count=",
          count,
          ", zEnable=",
          zEnable ? 1 : 0,
          ", zWrite=",
          zWriteEnable ? 1 : 0,
          ", viewportFallback=",
          dcs.transformData.usedViewportFallbackProjection ? 1 : 0,
          ")"));
      }
    }

    if (renderDocAttached && dcs.transformData.usedViewportFallbackProjection) {
      static uint32_t sRenderDocFallbackBypassLogCount = 0;
      if (sRenderDocFallbackBypassLogCount < 8) {
        ++sRenderDocFallbackBypassLogCount;
        Logger::info(str::format(
          "[D3D11Rtx] RenderDoc detected - bypassing viewport-fallback screen-space rejection (count=",
          count,
          ", zEnable=",
          zEnable ? 1 : 0,
          ", zWrite=",
          zWriteEnable ? 1 : 0,
          ")"));
      }
    }

    // Launcher / helper-window guard:
    // Many D3D11 launchers present tiny swap chains (211x36, 161x36, 480x420, 10x10, etc.)
    // before the real game scene exists. Running full Remix material categorization and RT
    // submission heuristics on those windows can destabilize startup while providing no useful
    // scene data. Until a stable scene camera or previous scene exists, hard-skip these tiny
    // outputs and wait for the real game viewport.
    const uint32_t activeOutputWidth =
      m_lastRemixViewportExtent.width > 0u ? m_lastRemixViewportExtent.width : m_lastOutputExtent.width;
    const uint32_t activeOutputHeight =
      m_lastRemixViewportExtent.height > 0u ? m_lastRemixViewportExtent.height : m_lastOutputExtent.height;
    const bool launcherSizedOutput =
      activeOutputWidth > 0u && activeOutputHeight > 0u &&
      (activeOutputWidth < 640u || activeOutputHeight < 480u);
    const bool noStableSceneYet =
      !hasStableSceneCamera &&
      !viewportFallbackAfterRealCamera &&
      m_submitRejectStats.accepted == 0 &&
      !sceneManager.isPreviousFrameSceneAvailable();

    if (launcherSizedOutput && noStableSceneYet) {
      static uint32_t sLauncherSizedOutputSkipLogCount = 0;
      if (sLauncherSizedOutputSkipLogCount < 12) {
        ++sLauncherSizedOutputSkipLogCount;
        Logger::info(str::format(
          "[D3D11Rtx] Skipping launcher/helper-window draw before RT submission: output=",
          activeOutputWidth, "x", activeOutputHeight,
          " count=", count,
          " zEnable=", zEnable ? 1 : 0,
          " zWrite=", zWriteEnable ? 1 : 0));
      }
      return;
    }

    // Launcher / helper-window guard:
    // Ignore tiny startup swap chains until a stable scene exists.

    // Register this context as the active rendering context so the primary
    // swap chain routes EndFrame/OnPresent through us, not a video-playback
    // device that happened to present first.
    // Do this only after rejecting obvious composite passes so skipped draws
    // do not pay the material/texture selection cost.
    // Trust the guest texture identity only when the emulator marked the draw
    // as textured; an untextured draw's hash field may carry stale state from
    // the publisher's reused draw config and would tag the wrong surface.
    const bool texturedEmulatorDraw = authenticatedEmulatorDraw
      && (emulatorMetadata->flags & remix::emulator::DrawFlagTextured) != 0;
    FillMaterialData(dcs.materialData,
      texturedEmulatorDraw ? emulatorMetadata->guestTextureHash : 0);
    dcs.materialData.isLiftedSprite = lift2D;

    // Refracting water: Remix's animated-water layering (two scrolling
    // samples of the wave normal map) replaces the motion the game's pixel
    // shader gave it, which a captured surface no longer has.
    if (dcs.materialData.isRefractiveSurface && dcs.materialData.normalTexture.isValid())
      dcs.setCategory(InstanceCategories::AnimatedWater, true);

    // Splat-blended terrain (METHODS.md, Terrain layer blending): the PS
    // blends several colour layers, so any single texture Remix picks is one
    // layer. Tagged Terrain, the baker replays the game's own blend into the
    // terrain cascades. Opaque, depth-writing world geometry only.
    if (RtxOptions::dx11AutoTerrainBlend() && !m_abDisableEngineKnowledge && !lift2D
     && zEnable && zWriteEnable && !dcs.testCategoryFlags(InstanceCategories::Terrain)
     && m_context->m_state.ps.shader != nullptr) {
      bool blending = false;
      if (D3D11BlendState* bs = m_context->m_state.om.cbState) {
        D3D11_BLEND_DESC1 bd;
        bs->GetDesc1(&bd);
        blending = bd.RenderTarget[0].BlendEnable != FALSE;
      }
      const D3D11CommonShader* terrainPs = m_context->m_state.ps.shader->GetCommonShader();
      uint32_t colourLayers = 0;
      for (uint32_t slot = 0; !blending && terrainPs != nullptr
           && slot < D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT; ++slot) {
        D3D11ShaderResourceView* srv = m_context->m_state.ps.shaderResources.views[slot].ptr();
        if (srv == nullptr || !terrainPs->SamplesResourceSlot(slot)
         || terrainPs->GetTextureDecode(slot).normalEncoding != 0)
          continue;
        D3D11_SHADER_RESOURCE_VIEW_DESC1 sd = {};
        srv->GetDesc1(&sd);
        if (sd.ViewDimension != D3D11_SRV_DIMENSION_TEXTURE2D)
          continue;
        switch (sd.Format) {
          case DXGI_FORMAT_BC1_UNORM: case DXGI_FORMAT_BC1_UNORM_SRGB:
          case DXGI_FORMAT_BC3_UNORM: case DXGI_FORMAT_BC3_UNORM_SRGB:
          case DXGI_FORMAT_BC7_UNORM: case DXGI_FORMAT_BC7_UNORM_SRGB:
          case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
            break;
          default:
            continue;
        }
        if ((srv->GetResourceDesc().BindFlags & (D3D11_BIND_RENDER_TARGET | D3D11_BIND_UNORDERED_ACCESS)) != 0)
          continue;
        Rc<DxvkImageView> view = srv->GetImageView();
        if (view == nullptr || view->image()->info().mipLevels <= 1 || view->image()->info().extent.width < 64u)
          continue;
        ++colourLayers;
      }
      if (colourLayers >= 4u) {
        dcs.setCategory(InstanceCategories::Terrain, true);
        ++m_submitRejectStats.autoTerrain;
      }
    }

    // Engines draw a water surface in more than one pass over the same planes
    // (depth/fog prepass, then the refracting surface). Only the refracting
    // pass becomes Remix water; a non-refracting draw of the same surface
    // texture is that companion pass and would put an opaque copy of the
    // water over the translucent one.
    {
      const XXH64_hash_t surfaceHash = dcs.materialData.getColorTexture().getImageHash();
      if (surfaceHash != kEmptyHash) {
        if (dcs.materialData.isRefractiveSurface) {
          m_refractiveSurfaceTextures.insert(surfaceHash);
        } else if (m_refractiveSurfaceTextures.count(surfaceHash) != 0) {
          ++m_submitRejectStats.waterCompanionSkipped;
          return;
        }
      }
    }

    // The DX11 bridge builds its LegacyMaterialData directly from the bound
    // SRVs, unlike the original D3D11 path.  Category evaluation therefore has
    // to happen after FillMaterialData has selected the final color texture.
    // Without this call the Remix UI could persist a selected texture hash,
    // but every submitted DrawCallState kept an empty category bitset: terrain,
    // sky, ignore, player-model, decal, particle, and UI tagging were all
    // observable no-ops.
    dcs.setupCategoriesForTexture();

    // --- Automatic decal / particle classification ---
    // Remix only knows decals and particles through manual texture tags, so
    // untagged ones arrived as ordinary opaque geometry: decals z-fought the
    // surface under them, deferred decal boxes became solid boxes, and
    // particles were hard-edged quads in the BVH. Classify from the draw's own
    // state; any manual tag on the texture wins (categories already set).
    bool decalVolumeCandidate = false;
    if (RtxOptions::dx11AutoClassifyDecalsAndParticles() && dcs.getCategoryFlags().raw() == 0u && !lift2D) {
      bool blending = false;
      bool additive = false;
      if (D3D11BlendState* blendState = m_context->m_state.om.cbState) {
        D3D11_BLEND_DESC1 blendDesc;
        blendState->GetDesc1(&blendDesc);
        blending = blendDesc.RenderTarget[0].BlendEnable != FALSE;
        additive = blending && blendDesc.RenderTarget[0].DestBlend == D3D11_BLEND_ONE;
      }

      float depthBias = 0.0f;
      D3D11_CULL_MODE cullMode = D3D11_CULL_BACK;
      if (D3D11RasterizerState* classifyRs = m_context->m_state.rs.state) {
        const auto* rsDesc = classifyRs->Desc();
        depthBias = std::abs(float(rsDesc->DepthBias)) + std::abs(rsDesc->SlopeScaledDepthBias);
        cullMode = rsDesc->CullMode;
      }

      // Does the pixel shader read the depth buffer (soft particles, deferred
      // decals and volumes)?
      bool samplesDepthBuffer = false;
      if (m_context->m_state.ps.shader != nullptr) {
        const D3D11CommonShader* classifyPs = m_context->m_state.ps.shader->GetCommonShader();
        for (uint32_t slot = 0; slot < D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT && !samplesDepthBuffer; ++slot) {
          D3D11ShaderResourceView* srv = m_context->m_state.ps.shaderResources.views[slot].ptr();
          if (srv == nullptr || (classifyPs != nullptr && classifyPs->HasCompleteSampledResourceProfile()
                                 && !classifyPs->SamplesResourceSlot(slot)))
            continue;
          samplesDepthBuffer = (srv->GetResourceDesc().BindFlags & D3D11_BIND_DEPTH_STENCIL) != 0;
        }
      }

      const bool overlay = zEnable && !zWriteEnable;
      // A small blended box that reads scene depth is a deferred/DBuffer
      // decal (UE DBuffer, Unity URP/HDRP, CRYENGINE DeferredDecalVolume,
      // Frostbite mainGBufferDecal). It cannot be ray traced as a box; it is
      // projected onto its surface below once its placement is exact.
      const bool boxSized = count <= 36u;
      if (overlay && samplesDepthBuffer && boxSized && blending && !additive) {
        decalVolumeCandidate = true;
        ++m_submitRejectStats.volumeBoxSkipped;
      }
      // Engines whose decals re-draw the G-buffer MRTs (Stingray, Katana,
      // CRYENGINE OVERLAYS, Frostbite, Fox, UE): a blended, no-depth-write,
      // non-additive draw into two or more render targets is a decal layer.
      uint32_t boundTargets = 0;
      for (const auto& rtv : m_context->m_state.om.renderTargetViews)
        boundTargets += rtv != nullptr ? 1u : 0u;
      const bool gbufferDecal = GetD3D11EngineProfile().facts->decalsInGBuffer
        && overlay && blending && !additive && boundTargets >= 2u && !samplesDepthBuffer;
      if (decalVolumeCandidate) {
        dcs.setCategory(InstanceCategories::DecalDynamic, true);
        ++m_submitRejectStats.autoDecals;
      } else if (overlay && blending && (depthBias > 0.0f || gbufferDecal) && !additive) {
        dcs.setCategory(InstanceCategories::DecalDynamic, true);
        ++m_submitRejectStats.autoDecals;
      } else if (overlay && blending && (additive || samplesDepthBuffer || colSem != nullptr)) {
        dcs.setCategory(InstanceCategories::Particle, true);
        ++m_submitRejectStats.autoParticles;
      }
    }

    const XXH64_hash_t categorizedTextureHash =
      dcs.materialData.getColorTexture().getImageHash();
    const uint64_t categoryBits = dcs.getCategoryFlags().raw();
    if (categoryBits != 0u) {
      static uint32_t sTextureCategoryApplyLogCount = 0;
      if (sTextureCategoryApplyLogCount < 64u) {
        ++sTextureCategoryApplyLogCount;
        Logger::info(str::format(
          "[D3D11Rtx][texture-category] applied hash=0x",
          std::hex, categorizedTextureHash,
          " categories=0x", categoryBits,
          std::dec,
          " drawId=", dcs.drawCallID,
          " count=", count,
          " indexed=", indexed ? 1 : 0));
      }
    }

    // Match the established Remix/D3D11 contract: an Ignore-tagged texture is
    // intentionally absent from the RT scene.  Merely carrying the category
    // into InstanceManager is insufficient because Ignore is an admission
    // category, not a shading flag.
    if (dcs.testCategoryFlags(InstanceCategories::Ignore)) {
      static uint32_t sIgnoredTextureDrawLogCount = 0;
      if (sIgnoredTextureDrawLogCount < 32u) {
        ++sIgnoredTextureDrawLogCount;
        Logger::info(str::format(
          "[D3D11Rtx][texture-category] skipped Ignore-tagged draw hash=0x",
          std::hex, categorizedTextureHash, std::dec,
          " drawId=", dcs.drawCallID));
      }
      return;
    }

    const auto routeRasterUiLayer = [&](const char* classifier) {
      m_rasterUiSeenThisFrame = true;
      m_allowNativeRasterForCurrentDraw = true;
      ++m_submitRejectStats.screenSpaceUiSkip;

      // A previous proven UI draw already placed the RTX composite. Keep this
      // independently proven UI draw on the native overlay, but never inject a
      // second time in the same frame.
      if (m_midFrameRtxInjected)
        return;

      Rc<DxvkImage> uiTarget;
      auto* rtv0 = m_context->m_state.om.renderTargetViews[0].ptr();
      if (rtv0 != nullptr) {
        Rc<DxvkImageView> uiTargetView = rtv0->GetImageView();
        if (uiTargetView != nullptr)
          uiTarget = uiTargetView->image();
      }

      const uint32_t expectedWidth = m_lastOutputExtent.width > 0u
        ? m_lastOutputExtent.width
        : m_lastRemixViewportExtent.width;
      const uint32_t expectedHeight = m_lastOutputExtent.height > 0u
        ? m_lastOutputExtent.height
        : m_lastRemixViewportExtent.height;
      const VkExtent3D uiTargetExtent = uiTarget != nullptr
        ? uiTarget->info().extent
        : VkExtent3D { 0u, 0u, 0u };
      const bool fullOutputTarget = uiTarget != nullptr
        && (expectedWidth == 0u || uiTargetExtent.width == expectedWidth)
        && (expectedHeight == 0u || uiTargetExtent.height == expectedHeight);
      const bool realSceneBeforeUi = m_submitRejectStats.realSceneAccepted > 0u;
      const bool stablePreviousScene = sceneManager.isPreviousFrameSceneAvailable();

      // Offscreen UI target (Scaleform/HUD render targets, FO4 kUI): the UI
      // is drawn into its own texture and composited onto the back buffer
      // later. Injecting here wrote the RT frame into the UI texture; forcing
      // pass-through dropped RT for the frame. Keep the UI raster, remember
      // the target, and inject at the composite draw that samples it
      // (METHODS.md, UI composition).
      if (!m_abDisableEngineKnowledge && uiTarget != nullptr && m_lastBackbufferImage != nullptr
       && uiTarget.ptr() != m_lastBackbufferImage) {
        if (m_offscreenUiTargets.size() < 8u
         && std::find(m_offscreenUiTargets.begin(), m_offscreenUiTargets.end(), uiTarget.ptr()) == m_offscreenUiTargets.end()) {
          m_offscreenUiTargets.push_back(uiTarget.ptr());
          Logger::info(str::format("[D3D11Rtx][ui-layer] offscreen UI target ",
            uiTargetExtent.width, "x", uiTargetExtent.height, " (", classifier,
            "): RT is injected at its composite onto the back buffer"));
        }
        return;
      }

      // Current-frame scene submissions and this injection are emitted to the
      // same command stream in order. Requiring isPreviousFrameSceneAvailable
      // here is both unnecessary and racy: the CPU draw thread can reach the
      // UI before the CS thread publishes that flag, which forced Unreal games
      // into raster pass-through even after dozens of real scene draws. A real
      // current-frame scene plus the full output target is the complete safety
      // condition needed to insert RTX immediately before the raster overlay.
      if (realSceneBeforeUi && fullOutputTarget) {
        // SubmitDraw runs immediately before D3D11 queues the application's
        // draw. Queue RTX now, after all scene draws and before this Canvas;
        // the application's UI draw then lands on top of the final image.
        m_context->EmitCs([uiTarget](DxvkContext* ctx) {
          static_cast<RtxContext*>(ctx)->injectRTX(0, uiTarget);
        });
        m_midFrameRtxInjected = true;

        static uint32_t sUiLayerMidFrameLogCount = 0;
        if (sUiLayerMidFrameLogCount < 32u) {
          ++sUiLayerMidFrameLogCount;
          Logger::info(str::format(
            "[D3D11Rtx][ui-layer] queued RTX before raster UI: classifier=",
            classifier,
            " count=", count,
            " textureHash=0x", std::hex, categorizedTextureHash, std::dec,
            " target=", uiTargetExtent.width, "x", uiTargetExtent.height,
            " realScene=", m_submitRejectStats.realSceneAccepted,
            " previousScene=", stablePreviousScene ? 1 : 0));
        }
      } else {
        // UI appeared before a trustworthy 3D scene (menus, startup logos,
        // loading screens), or on a helper target. Preserve the complete
        // raster frame instead of overwriting it with stale/partial RTX.
        // A 2D game being lifted has no other scene: its frame stays path
        // traced, and this draw (into a helper target) is only an input.
        if (m_lift2DFrame)
          return;
        m_forceRasterPassThroughThisFrame = true;

        static uint32_t sUiLayerPassThroughLogCount = 0;
        if (sUiLayerPassThroughLogCount < 32u) {
          ++sUiLayerPassThroughLogCount;
          Logger::info(str::format(
            "[D3D11Rtx][ui-layer] raster pass-through: classifier=",
            classifier,
            " count=", count,
            " textureHash=0x", std::hex, categorizedTextureHash, std::dec,
            " target=", uiTargetExtent.width, "x", uiTargetExtent.height,
            " expected=", expectedWidth, "x", expectedHeight,
            " realScene=", m_submitRejectStats.realSceneAccepted,
            " previousScene=", stablePreviousScene ? 1 : 0));
        }
      }
    };

    if (likelyScreenSpaceUiPass) {
      const bool explicitlyWorldRouted =
           dcs.testCategoryFlags(InstanceCategories::WorldUI)
        || dcs.testCategoryFlags(InstanceCategories::WorldMatte)
        || dcs.testCategoryFlags(InstanceCategories::Particle)
        || dcs.testCategoryFlags(InstanceCategories::Beam);

      // Manual texture-hash categories are authoritative. An explicitly tagged
      // world UI/material continues through the RT/export path. Untagged
      // screen UI keeps its real texture hash registered by FillMaterialData,
      // but its triangles stay on the raster layer where they belong.
      if (!explicitlyWorldRouted) {
        routeRasterUiLayer("screen-space atlas");
        return;
      }
    }

    // rtx.orthographicIsUI (frameworks_2d_web.md, injection recommendation
    // 3): once a game has drawn a perspective scene, an orthographic draw
    // (constant clip w) that writes no depth is HUD or menu content and stays
    // on the UI layer. A 2D playfield drawn orthographically is lifted
    // instead (lift2D, decided earlier), orthographic depth passes (shadow
    // cascades) write depth, and texture-hash world categories win.
    if (orthographicIsUI() && clipOrthographic && !lift2D && m_seenPerspectiveScene && !zWriteEnable
     && !dcs.testCategoryFlags(InstanceCategories::WorldUI)
     && !dcs.testCategoryFlags(InstanceCategories::WorldMatte)
     && !dcs.testCategoryFlags(InstanceCategories::Particle)
     && !dcs.testCategoryFlags(InstanceCategories::Beam)) {
      ++m_submitRejectStats.orthographicUi;
      routeRasterUiLayer("orthographic");
      return;
    }

    // Screen-space vertex output (see screenSpaceVsDraw) never becomes scene
    // geometry; anything that was UI has been routed above.
    if (screenSpaceVsDraw && !lift2D
     && !dcs.testCategoryFlags(InstanceCategories::WorldUI)
     && !dcs.testCategoryFlags(InstanceCategories::WorldMatte)) {
      ++m_submitRejectStats.screenSpaceVsSkipped;
      return;
    }

    const uint32_t tinyFallbackPrimitiveCount = dcs.geometryData.calculatePrimitiveCount();
    const bool tinyFallbackHasSceneDepthSignal =
      dcs.zEnable && (dcs.zWriteEnable || dcs.maxZ >= 0.99f);
    const bool tinyFallbackMicroRaster =
      !renderDocAttached &&
      dcs.transformData.usedViewportFallbackProjection &&
      count <= 6u &&
      tinyFallbackPrimitiveCount <= 2u &&
      !tinyFallbackHasSceneDepthSignal;

    if (tinyFallbackMicroRaster && !likelyScreenSpaceUiPass && !lift2D) {
      static uint32_t sTinyFallbackMicroRasterLogCount = 0;
      if (sTinyFallbackMicroRasterLogCount < 16u) {
        ++sTinyFallbackMicroRasterLogCount;
        Logger::info(str::format(
          "[D3D11Rtx] Keeping tiny fallback-camera micro-raster out of RTX without starting the UI layer: count=",
          count, ", indexed=", indexed ? 1 : 0,
          ", primitives=", tinyFallbackPrimitiveCount,
          ", zEnable=", dcs.zEnable ? 1 : 0,
          ", zWrite=", dcs.zWriteEnable ? 1 : 0));
      }
      return;
    }

    // RTX has already replaced the native color target. A draw that reached
    // this point did not satisfy either screen-space UI classifier, so letting
    // its native raster command execute would stack scene/helper geometry on
    // top of the path-traced image. Do not submit it to the now-finalized RT
    // scene either; it will be considered in normal order on the next frame.
    if (m_midFrameRtxInjected) {
      static uint32_t sPostCompositeRasterSuppressLogCount = 0;
      if (sPostCompositeRasterSuppressLogCount < 32u) {
        ++sPostCompositeRasterSuppressLogCount;
        Logger::info(str::format(
          "[D3D11Rtx][raster-layer] suppressed post-composite non-UI draw",
          " count=", count,
          " indexed=", indexed ? 1 : 0,
          " zEnable=", dcs.zEnable ? 1 : 0,
          " zWrite=", dcs.zWriteEnable ? 1 : 0,
          " fallbackCamera=", dcs.transformData.usedViewportFallbackProjection ? 1 : 0,
          " textured=", dcs.materialData.usesTexture() ? 1 : 0));
      }
      return;
    }

    uint32_t transientInputCount = 0;
    uint32_t significantInputCount = 0;
    const auto isLikelyTransientScreenSpacePass = [&]() {
      const uint32_t primitiveCount = dcs.geometryData.calculatePrimitiveCount();
      const bool hasSceneDepthSignal = dcs.zEnable
        && (dcs.zWriteEnable || dcs.maxZ >= 0.99f);
      const bool identitySceneSpace =
        isIdentityExact(dcs.transformData.objectToWorld)
        && isIdentityExact(dcs.transformData.worldToView)
        && (isIdentityExact(dcs.transformData.objectToView)
         || dcs.transformData.objectToView == dcs.transformData.objectToWorld);
      const bool viewportFallbackScreenSpace =
        dcs.transformData.usedViewportFallbackProjection
        && identitySceneSpace;

      // A synthesized viewport camera plus identity transforms is a raster
      // composition/UI pass, not a stable camera-space scene for Remix.  This
      // catches startup splash frames that repeatedly fed a rejected fake camera
      // into the RT scene before the actual game camera appeared.
      if (viewportFallbackScreenSpace && !hasSceneDepthSignal && count <= 4096)
        return true;

      const bool smallOrScreenPrimitive = count <= 12 || primitiveCount <= 4;
      const bool weakSceneSignal = !hasSceneDepthSignal || dcs.transformData.usedViewportFallbackProjection;
      if (!smallOrScreenPrimitive || !weakSceneSignal)
        return false;

      std::array<DxvkImage*, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT> boundRTImages = {};
      uint32_t rtWidth = 0;
      uint32_t rtHeight = 0;
      for (uint32_t rt = 0; rt < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++rt) {
        auto* rtv = omState.renderTargetViews[rt].ptr();
        if (!rtv)
          continue;

        Rc<DxvkImageView> rtvView = rtv->GetImageView();
        if (rtvView == nullptr)
          continue;

        boundRTImages[rt] = rtvView->image().ptr();
        if (rt == 0) {
          rtWidth = rtvView->image()->info().extent.width;
          rtHeight = rtvView->image()->info().extent.height;
        }
      }

      const uint32_t outputWidth = m_lastOutputExtent.width != 0u
        ? m_lastOutputExtent.width
        : m_lastRemixViewportExtent.width;
      const uint32_t outputHeight = m_lastOutputExtent.height != 0u
        ? m_lastOutputExtent.height
        : m_lastRemixViewportExtent.height;

      auto isBlockCompressed = [](DXGI_FORMAT fmt) {
        return (fmt >= DXGI_FORMAT_BC1_TYPELESS && fmt <= DXGI_FORMAT_BC1_UNORM_SRGB)
            || (fmt >= DXGI_FORMAT_BC2_TYPELESS && fmt <= DXGI_FORMAT_BC2_UNORM_SRGB)
            || (fmt >= DXGI_FORMAT_BC3_TYPELESS && fmt <= DXGI_FORMAT_BC3_UNORM_SRGB)
            || (fmt >= DXGI_FORMAT_BC4_TYPELESS && fmt <= DXGI_FORMAT_BC4_SNORM)
            || (fmt >= DXGI_FORMAT_BC5_TYPELESS && fmt <= DXGI_FORMAT_BC5_SNORM)
            || (fmt >= DXGI_FORMAT_BC6H_TYPELESS && fmt <= DXGI_FORMAT_BC7_UNORM_SRGB);
      };

      auto isDataOrSceneFormat = [](DXGI_FORMAT fmt) {
        switch (fmt) {
          case DXGI_FORMAT_R10G10B10A2_UNORM:
          case DXGI_FORMAT_R10G10B10A2_UINT:
          case DXGI_FORMAT_R11G11B10_FLOAT:
          case DXGI_FORMAT_R16_FLOAT:
          case DXGI_FORMAT_R16G16_FLOAT:
          case DXGI_FORMAT_R16G16B16A16_FLOAT:
          case DXGI_FORMAT_R32_FLOAT:
          case DXGI_FORMAT_R32G32_FLOAT:
          case DXGI_FORMAT_R32G32B32_FLOAT:
          case DXGI_FORMAT_R32G32B32A32_FLOAT:
          case DXGI_FORMAT_R8_UINT:
          case DXGI_FORMAT_R8_SINT:
          case DXGI_FORMAT_R8G8_UINT:
          case DXGI_FORMAT_R8G8_SINT:
          case DXGI_FORMAT_R8G8B8A8_UINT:
          case DXGI_FORMAT_R8G8B8A8_SINT:
          case DXGI_FORMAT_R16_UINT:
          case DXGI_FORMAT_R16_SINT:
          case DXGI_FORMAT_R16G16_UINT:
          case DXGI_FORMAT_R16G16_SINT:
          case DXGI_FORMAT_R16G16B16A16_UINT:
          case DXGI_FORMAT_R16G16B16A16_SINT:
          case DXGI_FORMAT_R32_UINT:
          case DXGI_FORMAT_R32_SINT:
          case DXGI_FORMAT_R32G32_UINT:
          case DXGI_FORMAT_R32G32_SINT:
          case DXGI_FORMAT_R32G32B32_UINT:
          case DXGI_FORMAT_R32G32B32_SINT:
          case DXGI_FORMAT_R32G32B32A32_UINT:
          case DXGI_FORMAT_R32G32B32A32_SINT:
          case DXGI_FORMAT_R16_TYPELESS:
          case DXGI_FORMAT_R24G8_TYPELESS:
          case DXGI_FORMAT_R32_TYPELESS:
          case DXGI_FORMAT_R32G8X24_TYPELESS:
          case DXGI_FORMAT_D16_UNORM:
          case DXGI_FORMAT_D24_UNORM_S8_UINT:
          case DXGI_FORMAT_D32_FLOAT:
          case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
          case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
          case DXGI_FORMAT_X24_TYPELESS_G8_UINT:
          case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:
          case DXGI_FORMAT_X32_TYPELESS_G8X24_UINT:
          case DXGI_FORMAT_R8G8_B8G8_UNORM:
          case DXGI_FORMAT_G8R8_G8B8_UNORM:
            return true;
          default:
            return false;
        }
      };

      auto matchesExtent = [](const VkExtent3D& extent, uint32_t width, uint32_t height) {
        return width != 0u && height != 0u
          && extent.width == width
          && extent.height == height;
      };

      auto matchesHalfExtent = [](const VkExtent3D& extent, uint32_t width, uint32_t height) {
        return width != 0u && height != 0u
          && extent.width * 2u == width
          && extent.height * 2u == height;
      };

      transientInputCount = 0;
      significantInputCount = 0;

      for (uint32_t slot = 0; slot < D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT; ++slot) {
        D3D11ShaderResourceView* srv = m_context->m_state.ps.shaderResources.views[slot].ptr();
        if (!srv || srv->GetResourceType() != D3D11_RESOURCE_DIMENSION_TEXTURE2D)
          continue;

        Rc<DxvkImageView> view = srv->GetImageView();
        if (view == nullptr)
          continue;

        const auto& imgInfo = view->image()->info();
        if (imgInfo.extent.width <= 2 && imgInfo.extent.height <= 2)
          continue;

        ++significantInputCount;

        D3D11_SHADER_RESOURCE_VIEW_DESC1 srvDesc = {};
        srv->GetDesc1(&srvDesc);
        const D3D11_COMMON_RESOURCE_DESC resourceDesc = srv->GetResourceDesc();
        const bool bc = isBlockCompressed(srvDesc.Format);
        const bool dataOrSceneFormat = isDataOrSceneFormat(srvDesc.Format);
        const bool hasHazardBindFlags = srv->TestHazards() != FALSE;
        const bool hasRtBind = (resourceDesc.BindFlags & D3D11_BIND_RENDER_TARGET) != 0;
        const bool hasUavBind = (resourceDesc.BindFlags & D3D11_BIND_UNORDERED_ACCESS) != 0;
        const bool hasDepthBind = (resourceDesc.BindFlags & D3D11_BIND_DEPTH_STENCIL) != 0;
        const bool singleMipLarge = imgInfo.mipLevels <= 1
          && (imgInfo.extent.width >= 512 || imgInfo.extent.height >= 512);
        const bool rtSized = matchesExtent(imgInfo.extent, rtWidth, rtHeight)
          || matchesExtent(imgInfo.extent, outputWidth, outputHeight)
          || matchesHalfExtent(imgInfo.extent, outputWidth, outputHeight);
        const bool multisampledView = srvDesc.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2DMS
          || srvDesc.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2DMSARRAY
          || imgInfo.sampleCount != VK_SAMPLE_COUNT_1_BIT;

        bool isCurrentRT = false;
        for (DxvkImage* boundRT : boundRTImages) {
          if (boundRT == view->image().ptr()) {
            isCurrentRT = true;
            break;
          }
        }

        const bool transientInput =
          isCurrentRT
          || multisampledView
          || (rtSized && (hasHazardBindFlags || hasDepthBind || hasRtBind || hasUavBind || dataOrSceneFormat))
          || (!bc && singleMipLarge && (hasDepthBind || hasRtBind || hasUavBind) && (hasHazardBindFlags || dataOrSceneFormat));

        if (transientInput)
          ++transientInputCount;
      }

      if (significantInputCount == 0)
        return viewportFallbackScreenSpace || (!dcs.materialData.usesTexture() && smallOrScreenPrimitive && !hasSceneDepthSignal);

      return transientInputCount == significantInputCount;
    };

    if (!lift2D && !renderDocAttached && isLikelyTransientScreenSpacePass()) {
      ++m_submitRejectStats.screenSpaceGarbageSkip;
      static uint32_t sScreenSpaceGarbageSkipLogCount = 0;
      if (sScreenSpaceGarbageSkipLogCount < 12) {
        ++sScreenSpaceGarbageSkipLogCount;
        Logger::info(str::format(
          "[D3D11Rtx] Skipping transient screen-space pass before RTX scene submission: count=",
          count,
          " primitives=",
          dcs.geometryData.calculatePrimitiveCount(),
          " zEnable=",
          zEnable ? 1 : 0,
          " zWrite=",
          zWriteEnable ? 1 : 0,
          " fallbackCamera=",
          dcs.transformData.usedViewportFallbackProjection ? 1 : 0,
          " transientInputs=",
          transientInputCount,
          "/",
          significantInputCount));
      }
      return;
    }

    // DX11_V271_NO_BLACK_FROM_VERTEX_COLOR: an all-black COLOR0 stream must
    // never become albedo. Untextured draws SelectArg1(vertexColor) -> black
    // geometry; textured draws Modulate(texture, vertexColor) -> black
    // textures. And with vertexColorIsBakedLighting (default true) the shader
    // normalizes color by its max channel, which for (0,0,0) is a divide-by-
    // zero that cannot recover. Widening COLOR0 acceptance (V268: float4/half4/
    // unorm16) let non-diffuse streams that decode to ~0 reach here. By this
    // point geo.color0Buffer is always B8G8R8A8_UNORM (native passes through;
    // every other format is converted to it above), so one sampled scan for a
    // uniformly-black stream covers all formats. Drop it if so - the surface
    // then uses its texture / white default instead of rendering black. A
    // stream with ANY non-trivial color anywhere is kept (real vertex color /
    // baked lighting with shadows is legitimate).
    if (geo.color0Buffer.defined()) {
      const uint8_t* colScan = reinterpret_cast<const uint8_t*>(
        geo.color0Buffer.mapPtr(geo.color0Buffer.offsetFromSlice()));
      const uint32_t colScanStride = geo.color0Buffer.stride();
      const uint32_t colScanSliceOff = geo.color0Buffer.offsetFromSlice();
      const size_t colScanLen = geo.color0Buffer.length() > colScanSliceOff
        ? geo.color0Buffer.length() - colScanSliceOff
        : 0;
      if (colScan != nullptr && colScanStride >= 4u && geo.vertexCount > 0) {
        constexpr uint32_t kMaxColorScan = 256u;
        const uint32_t sampleCount = std::min(geo.vertexCount, kMaxColorScan);
        const uint32_t step = std::max(1u, geo.vertexCount / sampleCount);
        uint8_t maxChannel = 0;
        for (uint32_t v = 0; v < geo.vertexCount && maxChannel < 4u; v += step) {
          const size_t off = size_t(v) * colScanStride;
          if (off + 3u > colScanLen)
            break;
          // B8G8R8A8: bytes 0,1,2 are B,G,R (alpha ignored - alpha 0 is common
          // and legitimate, only RGB drives albedo brightness).
          maxChannel = std::max({ maxChannel, colScan[off + 0], colScan[off + 1], colScan[off + 2] });
        }
        if (maxChannel < 4u) {
          // Uniformly black (< ~1.5% on every sampled vertex) - not real
          // diffuse color. Drop so it cannot blacken the surface.
          // DX11_V280 fix: dcs.geometryData was COPIED from geo before this
          // point, so the local clear alone never reached the submitted
          // draw - the all-black stream still shipped to the RT scene.
          geo.color0Buffer = RasterBuffer();
          colBuffer = RasterBuffer();
          dcs.geometryData.color0Buffer = RasterBuffer();
          static uint32_t sBlackColorDropLogCount = 0;
          if (sBlackColorDropLogCount < 8) {
            ++sBlackColorDropLogCount;
            Logger::info("[D3D11Rtx] Dropped all-black COLOR0 stream (would render surface black); using texture/white albedo.");
          }
        }
      }
    }

    // Vertex-color wiring. N64-era ports and fixed-function-style renderers
    // bake shading - or the entire surface color - into COLOR0: SM64-style
    // characters have untextured, vertex-colored body parts. With arg1
    // hardwired to Texture, untextured draws sampled a nonexistent texture
    // and rendered black. When the draw carries vertex colors: untextured
    // draws select the vertex color directly; textured draws use the classic
    // fixed-function default, Modulate(texture, vertex color).
    if (!dcs.materialData.usesTexture()) {
      const XXH64_hash_t sourceTagHash = dcs.materialData.getHash();
      // No image is created or bound here. Untextured geometry remains a real
      // path-traced surface and reads its albedo from actual vertex color when
      // present, otherwise from an opaque-white material constant. Selecting
      // Texture with no image is undefined in the legacy combiner and was the
      // direct reason genuinely untextured models could render black.
      const D3D11ColorSource untexturedSource = geo.color0Buffer.defined()
        ? D3D11ColorSource::VertexColor
        : D3D11ColorSource::BlendConstant;
      dcs.materialData.colorSource = untexturedSource;
      // Vertex alpha in modern DX11 layouts is frequently padding or baked
      // data. With no real texture/PS alpha available, keep the path-traced
      // surface opaque instead of allowing an incidental zero to erase it.
      dcs.materialData.alphaSource = D3D11ColorSource::BlendConstant;
      dcs.materialData.modulateVertexColor = false;
      dcs.materialData.blendConstant = Vector4(1.0f, 1.0f, 1.0f, 1.0f);
      // Preserve the source texture's authoring/tag hash even though the
      // image itself is intentionally absent from the base RT material.
      dcs.materialData.setHashOverride(sourceTagHash);
    } else if (geo.color0Buffer.defined()) {
      // Textured draw that also carries vertex colours: modulate one by the other.
      dcs.materialData.modulateVertexColor = true;
      dcs.materialData.updateCachedHash();
    }

    bool deferTexcoordRecoveryToPositionCapture = false;
    auto applyMissingTexcoordFallback = [&]() {
      if (dcs.transformData.texgenMode != TexGenMode::None)
        return;

      // Prefer real vertex colors to a constant when the VS exposes no usable
      // UV output at all. This path is only reached after both the combined
      // position/UV replay and the dedicated UV replay are unavailable.
      const D3D11ColorSource albedoSource =
        geo.color0Buffer.defined() ? D3D11ColorSource::VertexColor
                                   : D3D11ColorSource::BlendConstant;
      dcs.materialData.colorSource = albedoSource;
      dcs.materialData.alphaSource = D3D11ColorSource::BlendConstant;
      dcs.materialData.modulateVertexColor = false;
      dcs.materialData.blendConstant = Vector4(1.0f, 1.0f, 1.0f, 1.0f);
      dcs.materialData.updateCachedHash();
      ++m_submitRejectStats.texcoordGenerated;

      static uint32_t sTexcoordFallbackLogCount = 0;
      if (sTexcoordFallbackLogCount < 12) {
        ++sTexcoordFallbackLogCount;
        const XXH64_hash_t texHash =
          dcs.materialData.getColorTexture().getImageHash();
        Logger::info(str::format(
          "[D3D11Rtx] Textured draw has no recoverable TEXCOORD; using final flat albedo fallback (",
          geo.color0Buffer.defined() ? "vertex color" : "TFactor white",
          ", count=", count,
          ", indexed=", indexed ? 1 : 0,
          ", fallbackCamera=",
          dcs.transformData.usedViewportFallbackProjection ? 1 : 0,
          ", textureHash=0x", std::hex, texHash, std::dec, ")"));
      }
    };

    if (!geo.texcoordBuffer.defined() && dcs.materialData.usesTexture()) {
      ++m_submitRejectStats.noTexcoordLayout;

      const D3D11CommonShader* activeCommonVs =
        m_context->m_state.vs.shader != nullptr
          ? m_context->m_state.vs.shader->GetCommonShader()
          : nullptr;
      const bool combinedPositionUvCandidate = activeCommonVs != nullptr
        && activeCommonVs->PositionCaptureIncludesTexcoord()
        && m_context->m_state.gs.shader == nullptr
        && m_context->m_state.hs.shader == nullptr
        && m_context->m_state.ds.shader == nullptr;

      const uint32_t primitiveCountNoUv = dcs.geometryData.calculatePrimitiveCount();
      const bool noUvHasSceneDepthSignal = dcs.zEnable
        && (dcs.zWriteEnable || dcs.maxZ >= 0.99f);
      const bool noUvLikelyScreenGarbage =
        dcs.transformData.usedViewportFallbackProjection &&
        !indexed &&
        primitiveCountNoUv <= 8u &&
        !noUvHasSceneDepthSignal;

      if (noUvLikelyScreenGarbage) {
        ++m_submitRejectStats.screenSpaceGarbageSkip;
        return;
      }

      if (dcs.transformData.usedViewportFallbackProjection
       && !combinedPositionUvCandidate) {
      ++m_submitRejectStats.screenSpaceGarbageSkip;
      static uint32_t sDx11V124NoUvFallbackSkipLogCount = 0;
      if (sDx11V124NoUvFallbackSkipLogCount < 16) {
        ++sDx11V124NoUvFallbackSkipLogCount;
        Logger::info(str::format(
          "[D3D11Rtx] DX11_V124: skipping textured no-TEXCOORD draw under viewport-fallback camera to prevent white/color flash artifacts (count=",
          count,
          ", indexed=",
          indexed ? 1 : 0,
          ", primitives=",
          primitiveCountNoUv,
          ")"));
      }
      return;
    }

      // DX11_V126 NO-TEXCOORD ALBEDO FIX:
      // These textured draws have no TEXCOORD semantic in the input layout, so
      // the previous code forced TexGenMode::ViewPositions, which synthesizes
      // UVs from each vertex's camera-relative view-space position
      // (surface_interaction.slangh: mul(worldToView, worldPos)). The resulting
      // UVs span enormous ranges across a triangle and swim with every camera
      // move, so the texture is sampled at essentially random texels -> the
      // garbled-smear / black / blown-white surfaces observed in Granny's main
      // geometry (the count=273 draws).
      //
      // There is no correct UV to recover here (the layout genuinely has none),
      // and synthesized UVs can only be wrong. Instead, leave texgen OFF and
      // force a flat neutral albedo through the existing TFactor + SelectArg1
      // path: the shader picks the material's albedo from tFactor (a constant
      // register) and never depends on valid texture coordinates, so the
      // surface renders as solid geometry lit by the path tracer rather than
      // as a garbled texture smear. This is a strict improvement over the
      // swimming-texgen artifact and over simply dropping the draw.
      //
      // DX11_V280_TEXCOORD_CAPTURE: before falling back to flat albedo, try
      // to recover the REAL UVs. When the input layout has no TEXCOORD, most
      // engines still compute one in the vertex shader; the stream-out replay
      // reads that output back as a per-vertex stream, so the draw keeps its
      // actual texture with correct coordinates. Only when capture is not
      // possible (no texcoord VS output, GS/tessellation active, budget
      // exhausted, xfb unsupported) does the flat-albedo fallback apply.
      if (combinedPositionUvCandidate) {
        // The later exact-position replay emits SV_Position and TEXCOORD into
        // one interleaved record. Deferring avoids a duplicate VS/XFB pass and,
        // for indexed flattening, guarantees both attributes use the same
        // expanded vertex order.
        deferTexcoordRecoveryToPositionCapture = true;
      } else if (!(vertexPulled && indexed)  // flattened replay: different vertex domain
              && TryCaptureTexcoordsViaStreamOut(dcs, geo, indexed, count, start, base)) {
        ++m_submitRejectStats.texcoordCaptured;
      } else {
        applyMissingTexcoordFallback();
      }
    }

    const uint32_t tinyRasterPrimitiveCount = dcs.geometryData.calculatePrimitiveCount();
    const bool tinyRasterHasSceneDepthSignal = dcs.zEnable
      && (dcs.zWriteEnable || dcs.maxZ >= 0.99f);
    const bool tinyPostCameraRasterJunk =
      cameraManager.hasSeenRealMainCamera()
      && !dcs.transformData.usedViewportFallbackProjection
      && indexed
      && count <= 6u
      && tinyRasterPrimitiveCount <= 2u
      && !tinyRasterHasSceneDepthSignal;

    if (!renderDocAttached && tinyPostCameraRasterJunk) {
      ++m_submitRejectStats.screenSpaceGarbageSkip;
      static uint32_t sTinyRasterJunkLogCount = 0;
      if (sTinyRasterJunkLogCount < 16) {
        ++sTinyRasterJunkLogCount;
        Logger::info(str::format(
          "[D3D11Rtx] Skipping tiny post-main-camera raster junk draw: count=",
          count,
          ", indexed=",
          indexed ? 1 : 0,
          ", primitives=",
          tinyRasterPrimitiveCount,
          ", zEnable=",
          dcs.zEnable ? 1 : 0,
          ", zWrite=",
          dcs.zWriteEnable ? 1 : 0));
      }
      return;
    }

    // Engine knowledge (documentation/engine_knowledge): many engines draw a
    // mesh more than once per frame - Unity's additive ForwardAdd pass per
    // light, UE/Dawn velocity re-draws, light-prepass engines (CRYENGINE 3,
    // Northlight, MT Framework, Foundation, Deus Ex HR) that lay down a thin
    // G-buffer and then re-draw with depth EQUAL for the material. Every extra
    // copy becomes coincident RT geometry (z-fighting, doubled BLAS and
    // capture cost). Keep exactly one: the first, unless a depth-EQUAL opaque
    // material pass is known to follow, in which case that one carries the
    // real material and the geometry pass is left out.
    // UE binds its whole local-light list to forward/translucent pixel
    // shaders; one successful read per frame imports all of them.
    if (!m_abDisableEngineKnowledge && RtxOptions::dx11ImportTiledLights()
     && GetD3D11EngineProfile().family() == D3D11EngineFamily::Unreal
     && m_tiledLightImportFrame != m_context->m_device->getCurrentFrameId()
     && (m_drawCallID & 7u) == 0u)
      ImportTypedLightBuffer(m_context->m_state.ps.shaderResources);
    if (!m_abDisableEngineKnowledge && RtxOptions::dx11ImportTiledLights()
     && GetD3D11EngineProfile().family() == D3D11EngineFamily::Creation)
      CollectSkyrimDrawLights();

    uint64_t passKey = 0;
    uint64_t meshKey = 0;
    bool havePassKey = false;
    bool isOpaqueEqualMaterialPass = false;
    if (!m_abDisableEngineKnowledge && instanceTransform == nullptr && replayInstanceCount <= 1u && !lift2D) {
      havePassKey = ComputeDrawPassKey(indexed, count, start, base, replayFirstInstance,
                                       posBuffer, idxBuffer, vertexPulledIdentity, passKey, meshKey);
      if (!havePassKey) {
        ++m_submitRejectStats.passKeyUnavailable;
      } else {
        bool blendEnabled = false;
        bool additiveBlend = false;
        if (D3D11BlendState* blendState = m_context->m_state.om.cbState) {
          D3D11_BLEND_DESC1 blendDesc = {};
          blendState->GetDesc1(&blendDesc);
          const auto& rt0 = blendDesc.RenderTarget[0];
          blendEnabled = rt0.BlendEnable;
          // ForwardAdd / light accumulation: dst += src.
          additiveBlend = blendEnabled && rt0.DestBlend == D3D11_BLEND_ONE
            && (rt0.SrcBlend == D3D11_BLEND_ONE || rt0.SrcBlend == D3D11_BLEND_SRC_ALPHA);
        }
        // Material pass after a geometry/depth pass: depth test against the
        // laid-down depth (EQUAL, or LESS/GREATER_EQUAL with reversed Z), no
        // depth write, opaque. Recorded BEFORE the duplicate check - it shares
        // the geometry pass's key, so recording only accepted draws meant it
        // was never seen and the albedo-less geometry copy won permanently.
        isOpaqueEqualMaterialPass = zEnable && !zWriteEnable && !blendEnabled
          && (depthComparison == D3D11_COMPARISON_EQUAL
           || depthComparison == D3D11_COMPARISON_LESS_EQUAL
           || depthComparison == D3D11_COMPARISON_GREATER_EQUAL);
        const D3D11EngineProfile& engine = GetD3D11EngineProfile();
        const bool lightPrepassPossible = engine.family() == D3D11EngineFamily::Unknown
          || (engine.facts->multiPass & (D3D11MultiPassLightPrepass | D3D11MultiPassDepthPrepass)) != 0u;
        if (isOpaqueEqualMaterialPass && lightPrepassPossible)
          m_equalPassKeysThisFrame.insert(meshKey);

        // Velocity pass: every bound colour target is two-channel (motion
        // vectors). It re-draws meshes with previous-frame matrices or bones,
        // so its pass key differs; the mesh itself is already in the scene.
        bool velocityOnlyTargets = false;
        {
          const auto& rtvs = m_context->m_state.om.renderTargetViews;
          uint32_t bound = 0, twoChannel = 0;
          for (const auto& rtv : rtvs) {
            if (rtv == nullptr)
              continue;
            ++bound;
            D3D11_RENDER_TARGET_VIEW_DESC rtvDesc;
            rtv->GetDesc(&rtvDesc);
            switch (rtvDesc.Format) {
              case DXGI_FORMAT_R16G16_FLOAT: case DXGI_FORMAT_R16G16_UNORM:
              case DXGI_FORMAT_R16G16_SNORM: case DXGI_FORMAT_R32G32_FLOAT:
                ++twoChannel; break;
              default: break;
            }
          }
          velocityOnlyTargets = bound > 0u && twoChannel == bound;
        }

        // Alpha-blended re-draws of the same mesh are material layers
        // (CRYENGINE OVERLAYS terrain layers, blend shells), not copies of
        // it; they keep their own path (decal/layer classification).
        const bool redrawIsCopy = !blendEnabled || additiveBlend;
        if ((redrawIsCopy && m_passKeysThisFrame.count(passKey) != 0u)
         || (velocityOnlyTargets && m_meshKeysThisFrame.count(meshKey) != 0u)) {
          ++m_submitRejectStats.duplicatePassSkipped;
          return;
        }
        if (lightPrepassPossible && zWriteEnable && !isOpaqueEqualMaterialPass
         && m_equalPassKeysPrevFrame.count(meshKey) != 0u) {
          ++m_submitRejectStats.lightPrepassGeometrySkipped;
          return;
        }
      }
    }

    ++m_submitRejectStats.accepted;

    // Resolve only this draw's uncaptured IA transforms. Exact position capture
    // runs after admission and establishes its own paired camera/geometry space.
    // Carrying capture flags in context members here used the PREVIOUS draw's
    // result, allowing an unrelated overlay to reclassify the next world mesh.
    {
      CameraEvidence evidence;
      evidence.objectToWorld = dcs.transformData.objectToWorld;
      evidence.worldToView = dcs.transformData.worldToView;
      evidence.viewToProjection = dcs.transformData.viewToProjection;
      evidence.viewConfirmed = m_viewConfirmed;
      evidence.viewIsCameraRelative = m_viewCameraRelative;
      evidence.usedViewportFallbackProjection =
        dcs.transformData.usedViewportFallbackProjection;
      evidence.depthWriteDisabled = !dcs.zWriteEnable;

      // DrawCallState carries no blend flag; read it from the OM state the same
      // way isLikelyScreenSpaceUiPass does. Only used to separate Clip from View.
      bool shadowBlendEnabled = false;
      if (D3D11BlendState* blendState = m_context->m_state.om.cbState) {
        D3D11_BLEND_DESC1 blendDesc = {};
        blendState->GetDesc1(&blendDesc);
        shadowBlendEnabled = blendDesc.RenderTarget[0].BlendEnable;
      }
      evidence.blendEnabled = shadowBlendEnabled;

      const ResolvedTransform resolved = resolveTransformSpace(evidence);
      const TransformSpace legacySpace = legacySpaceFromCameraRelativeFlag(
        dcs.transformData.cameraRelativeView, dcs.transformData.objectToWorld);

      if (resolved.space != legacySpace) {
        // DX11_V313_CAMERA_RESOLVER_STEP2: act on the resolver, not just log it.
        //
        // Step 1 ran in shadow mode for two sessions and returned a unanimous
        // verdict - 64 of 64 accepted draws disagreed with the SAME signature
        // (resolved=view legacy=world, 'confirmed camera-relative identity
        // view'). The runtime confirms the view is camera-relative, meaning the
        // vertices are already in camera space, and then submits them flagged as
        // world space. With camPos=[0,0,0] that places the geometry on the eye.
        //
        // Only View vs World is representable in the legacy bool, and only a
        // high-confidence verdict is allowed to override, so a low-evidence
        // guess can never move geometry. rtx.dx11UseResolvedTransformSpace
        // turns this off without a rebuild if it regresses.
        constexpr uint32_t kMinConfidenceToOverride = 75u;

        const bool applyResolvedSpace = RtxOptions::dx11UseResolvedTransformSpace()
         && resolved.confidence >= kMinConfidenceToOverride
         && (resolved.space == TransformSpace::View
          || resolved.space == TransformSpace::World);
        if (applyResolvedSpace) {
          dcs.transformData.cameraRelativeView =
            (resolved.space == TransformSpace::View);
        }

        static uint32_t sCameraResolverDisagreeLogCount = 0;
        if (sCameraResolverDisagreeLogCount < 64u) {
          ++sCameraResolverDisagreeLogCount;
          Logger::debug(str::format(
            "[D3D11Rtx][camera-resolver] ",
            applyResolvedSpace ? "APPLIED" : "DISAGREE(log-only)",
            " resolved=", transformSpaceName(resolved.space),
            " legacy=", transformSpaceName(legacySpace),
            " confidence=", resolved.confidence,
            " reason='", resolved.reason, "'",
            " drawId=", dcs.drawCallID,
            " indices=", dcs.geometryData.indexCount,
            " cameraRelative=", dcs.transformData.cameraRelativeView ? 1 : 0,
            " viewConfirmed=", m_viewConfirmed ? 1 : 0,
            " viewIsCamRel=", m_viewCameraRelative ? 1 : 0,
            " identityObjToWorld=", isIdentityExact(dcs.transformData.objectToWorld) ? 1 : 0,
            " identityWorldToView=", isIdentityExact(dcs.transformData.worldToView) ? 1 : 0,
            " zWrite=", dcs.zWriteEnable ? 1 : 0,
            " blend=", shadowBlendEnabled ? 1 : 0));
        }
      }
    }

    // Raw IA bounds precede the vertex shader and cannot prove final placement.
    // A shader may place an origin-centered mesh far from the eye. Require exact
    // capture for suspicious bounds instead of deleting that draw before replay.
    bool eyeBoundsRequireExactCapture = false;
    if (RtxOptions::logCameraObstruction() || RtxOptions::dropCollapsedEyeGeometry()) {
      const AxisAlignedBoundingBox& objectBox = dcs.geometryData.boundingBox;
      const bool boundsResolved =
        objectBox.minPos.x <= objectBox.maxPos.x &&
        objectBox.minPos.y <= objectBox.maxPos.y &&
        objectBox.minPos.z <= objectBox.maxPos.z &&
        std::isfinite(objectBox.minPos.x) && std::isfinite(objectBox.minPos.y) &&
        std::isfinite(objectBox.minPos.z) && std::isfinite(objectBox.maxPos.x) &&
        std::isfinite(objectBox.maxPos.y) && std::isfinite(objectBox.maxPos.z);
      const Vector3 extent = objectBox.maxPos - objectBox.minPos;
      if (boundsResolved && extent.x > 0.01f && extent.y > 0.01f && extent.z > 0.01f) {
        const Matrix4 objectToView = dcs.transformData.worldToView * dcs.transformData.objectToWorld;
        Vector3 viewMin(FLT_MAX, FLT_MAX, FLT_MAX);
        Vector3 viewMax(-FLT_MAX, -FLT_MAX, -FLT_MAX);
        bool finiteBounds = true;
        for (uint32_t corner = 0; corner < 8; ++corner) {
          const Vector3 objectCorner(
            (corner & 1) ? objectBox.maxPos.x : objectBox.minPos.x,
            (corner & 2) ? objectBox.maxPos.y : objectBox.minPos.y,
            (corner & 4) ? objectBox.maxPos.z : objectBox.minPos.z);
          const Vector4 viewCorner = objectToView * Vector4(objectCorner, 1.0f);
          finiteBounds &= std::isfinite(viewCorner.x) && std::isfinite(viewCorner.y)
                       && std::isfinite(viewCorner.z);
          viewMin = min(viewMin, viewCorner.xyz());
          viewMax = max(viewMax, viewCorner.xyz());
        }
        const bool enclosesEye = finiteBounds
          && viewMin.x <= 0.0f && viewMax.x >= 0.0f
          && viewMin.y <= 0.0f && viewMax.y >= 0.0f
          && viewMin.z <= 0.0f && viewMax.z >= 0.0f;
        if (enclosesEye) {
          auto translationIsOrigin = [](const Matrix4& matrix) {
            return std::abs(matrix[3][0]) < 1.0e-4f
                && std::abs(matrix[3][1]) < 1.0e-4f
                && std::abs(matrix[3][2]) < 1.0e-4f;
          };
          const bool ambiguousOrigin = translationIsOrigin(dcs.transformData.objectToWorld)
                                   && translationIsOrigin(dcs.transformData.worldToView);
          const bool ambiguousQuad = dcs.geometryData.calculatePrimitiveCount() <= 2u
                                  && !dcs.materialData.usesTexture();
          eyeBoundsRequireExactCapture = RtxOptions::dropCollapsedEyeGeometry()
            && dcs.usesVertexShader && !pcsx2PostTransformDraw
            && (ambiguousOrigin || ambiguousQuad);
          static uint32_t sObstructionLogCount = 0;
          if (RtxOptions::logCameraObstruction()
           && sObstructionLogCount < RtxOptions::logCameraObstructionMaxEntries()) {
            ++sObstructionLogCount;
            Logger::debug(str::format(
              "[D3D11Rtx][cam-obstruction] pre-shader bounds enclose eye: drawId=", m_drawCallID,
              " indices=", count, " requireExactCapture=", eyeBoundsRequireExactCapture,
              " viewBox=[", viewMin.x, ",", viewMin.y, ",", viewMin.z,
              "]..[", viewMax.x, ",", viewMax.y, ",", viewMax.z, "]"));
          }
        }
      }
    }
    // DX11_V319_DEFERRED_LIGHT_VOLUMES: turn a deferred renderer's light-volume
    // draws into real Remix lights.
    //
    // D3D11 exposes no lights at all, so this runtime captures none: every scene
    // is lit by the fallback light alone. Light data does exist, but only as
    // untyped bytes in a constant or structured buffer whose layout differs per
    // engine, so it cannot be read generically.
    //
    // A deferred renderer, however, DRAWS each light: a unit sphere for a point
    // light, a unit cone for a spot, placed and scaled by an ordinary world
    // matrix. That matrix is already recovered for every draw here, so the
    // light's position is its translation and its range is its scale - no
    // constant-buffer parsing and nothing engine-specific. The accumulation pass
    // is recognisable by its state: additive blending into the light buffer with
    // depth writes off, over a small stand-in mesh.
    //
    // Colour and intensity are not in the geometry and are settings, not
    // guesses.
    //
    // DX11_V319_LIGHT_VOLUMES_SELF_ARM: whether this runs is decided by what the
    // game DRAWS, not by which engine it is. The signature below is evaluated on
    // every draw and the matches are counted; once a frame contains enough of
    // them the capture arms itself (see EndFrame). A deferred renderer of any
    // engine therefore switches it on by behaving like one, and a forward
    // renderer never does - no executable names, no per-title profiles, and
    // nothing to maintain as games are added.
    const bool lightVolumeArmed =
      RtxOptions::deferredLightVolumeCapture() || m_deferredLightVolumeAutoArmed;

    if ((lightVolumeArmed || RtxOptions::deferredLightVolumeAutoDetect())
     && m_deferredLightVolumesThisFrame < RtxOptions::deferredLightVolumeMaxPerFrame()) {
      const uint32_t volumePrimitives = dcs.geometryData.calculatePrimitiveCount();
      const Matrix4& lightToWorld = dcs.transformData.objectToWorld;

      // Range comes from the volume's scale. Use the largest basis axis: a point
      // light's sphere is scaled uniformly, and a spot light's cone is scaled by
      // its range along its axis.
      auto axisLength = [&lightToWorld](uint32_t axis) {
        const Vector3 basis(lightToWorld[axis][0], lightToWorld[axis][1], lightToWorld[axis][2]);
        return length(basis);
      };
      const float volumeRange = std::max(axisLength(0), std::max(axisLength(1), axisLength(2)));

      // Additive blend with no depth write is what a light accumulation pass
      // looks like; world geometry writes depth, and UI is not additive over a
      // scaled volume mesh. Requiring all three together is what keeps ordinary
      // transparent geometry out.
      const bool additiveAccumulation =
           dcs.materialData.blendMode.enableBlending
        && !dcs.zWriteEnable;

      if (additiveAccumulation
       && volumePrimitives > 0u
       && volumePrimitives <= RtxOptions::deferredLightVolumeMaxPrimitives()
       && std::isfinite(volumeRange)
       && volumeRange >= RtxOptions::deferredLightVolumeMinRange()
       && !isIdentityExact(lightToWorld)) {
        const Vector3 lightPosition(lightToWorld[3][0], lightToWorld[3][1], lightToWorld[3][2]);

        if (std::isfinite(lightPosition.x) && std::isfinite(lightPosition.y)
         && std::isfinite(lightPosition.z)) {
          // Count the evidence whether or not the capture is armed - this is
          // what lets it arm itself from a genuinely deferred frame.
          //
          // While unarmed the draw is deliberately left completely alone: it is
          // neither consumed nor altered, so a forward-rendered game that trips
          // the signature a few times is unaffected by the detector observing
          // it. Only an armed capture takes the draw over.
          ++m_deferredLightVolumeCandidatesThisFrame;

          // Real light volumes are distinct lights. Repeated draws at the same
          // spot with the same reach are one emitter (or not a light at all);
          // stacking them multiplied the brightness into flashes.
          bool duplicateLight = false;
          for (const Vector4& existing : m_deferredLightVolumePositionsThisFrame) {
            const Vector3 delta(existing.x - lightPosition.x,
                                existing.y - lightPosition.y,
                                existing.z - lightPosition.z);
            if (length(delta) < 0.05f * std::max(existing.w, volumeRange)
             && std::abs(existing.w - volumeRange) < 0.25f * std::max(existing.w, volumeRange)) {
              duplicateLight = true;
              break;
            }
          }

          if (lightVolumeArmed && duplicateLight) {
            // Consume the duplicate volume draw: it is a lighting operator, not
            // scene geometry, and its light already exists this frame.
            return;
          }

          if (lightVolumeArmed) {
          m_deferredLightVolumePositionsThisFrame.push_back(
            Vector4(lightPosition.x, lightPosition.y, lightPosition.z, volumeRange));
          Vector3 colour = RtxOptions::deferredLightVolumeColor()
                         * RtxOptions::deferredLightVolumeIntensity();

          // The light's own colour (METHODS.md, Lights): the volume's PS
          // constants hold the light position next to its colour (UE
          // LightPositionAndInvRadius/LightColorAndFalloffExponent, Unity
          // _LightPos/_LightColor, Fox b3, CE3 light volumes). Find the
          // register whose xyz is this light's position - absolute or
          // camera-relative - and take the adjacent non-negative rgb.
          if (!m_abDisableEngineKnowledge) {
            const auto& camera = m_context->m_device->getCommon()->getSceneManager()
              .getCameraManager().getCamera(CameraType::Main);
            const Vector3 eye = camera.getPosition(false);
            const Vector3 relative = lightPosition - eye;
            const float tolerance = std::max(0.01f * volumeRange, 0.05f);
            bool found = false;
            for (uint32_t slot = 0; slot < D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT && !found; ++slot) {
              const auto& cb = m_context->m_state.ps.constantBuffers[slot];
              if (cb.buffer == nullptr)
                continue;
              const auto* ptr = reinterpret_cast<const uint8_t*>(cb.buffer->GetMappedSlice().mapPtr);
              if (ptr == nullptr)
                continue;
              const size_t base = size_t(cb.constantOffset) * 16u;
              const size_t size = cb.buffer->Desc()->ByteWidth;
              const size_t end = std::min(size, base + 64u * 16u);
              for (size_t off = base; off + 32u <= end && !found; off += 16u) {
                float v[4];
                std::memcpy(v, ptr + off, sizeof(v));
                const Vector3 p(v[0], v[1], v[2]);
                if (length(p - lightPosition) > tolerance && length(p - relative) > tolerance)
                  continue;
                for (const int64_t step : { 16, -16 }) {
                  const int64_t colourOff = int64_t(off) + step;
                  if (colourOff < int64_t(base) || size_t(colourOff) + 16u > size)
                    continue;
                  float c[4];
                  std::memcpy(c, ptr + colourOff, sizeof(c));
                  const float maxC = std::max(c[0], std::max(c[1], c[2]));
                  if (std::isfinite(maxC) && maxC > 1.0e-4f && maxC < 1.0e6f
                   && c[0] >= 0.0f && c[1] >= 0.0f && c[2] >= 0.0f) {
                    colour = Vector3(c[0], c[1], c[2]) * (RtxOptions::deferredLightVolumeIntensity() / maxC);
                    found = true;
                    ++m_submitRejectStats.lightVolumeColours;
                    break;
                  }
                }
              }
            }
          }

          Dx11LightDesc light = Dx11LightStateApi::makePoint(
            lightPosition.x, lightPosition.y, lightPosition.z,
            colour.x, colour.y, colour.z,
            volumeRange);
          // The volume bounds how far the light reaches; it is not the size of
          // the emitter. Derive a plausible bulb radius from it so shadows are
          // not perfectly hard.
          light.Falloff = std::max(RtxOptions::deferredLightVolumeRadiusScale(), 0.0f);

          ++m_deferredLightVolumesThisFrame;
          m_context->EmitCs([cLight = light](DxvkContext* ctx) {
            static_cast<RtxContext*>(ctx)->addLights(&cLight, 1u);
          });

          static uint32_t sLightVolumeLogCount = 0;
          if (sLightVolumeLogCount < 24u) {
            ++sLightVolumeLogCount;
            Logger::info(str::format(
              "[D3D11Rtx][light-volume] created light from a deferred light volume: drawId=",
              dcs.drawCallID, " prims=", volumePrimitives,
              " pos=[", lightPosition.x, ",", lightPosition.y, ",", lightPosition.z,
              "] range=", volumeRange));
          }

          // The volume is a lighting operator, not scene geometry. Submitting it
          // as well would put a translucent sphere in the world around the light.
          return;
          } // lightVolumeArmed
        }
      }
    }

    {
      const uint32_t primitiveCount = dcs.geometryData.calculatePrimitiveCount();
      const bool hasSceneDepthSignal = dcs.zEnable
        && (dcs.zWriteEnable || dcs.maxZ >= 0.99f);
      const bool hasRealProjection = !dcs.transformData.usedViewportFallbackProjection;
      const bool hasViewOrStrongProjection = !isIdentityExact(dcs.transformData.worldToView)
        || (hasRealProjection && primitiveCount >= 32u);
      // DX11_V319_VIEWPORT_FALLBACK_SCENE: this gate used to also require
      // !cameraManager.hasSeenRealMainCamera(), which made it self-defeating.
      //
      // A main camera is established FROM these very draws, so the first
      // viewport-fallback frame that produced a camera permanently disqualified
      // every viewport-fallback draw that followed. For a title whose shaders
      // only ever expose a combined object-to-clip matrix - optimized Unity
      // being the common case - no draw ever has a real projection, so the
      // whole game fell out of the scene path and rendered rasterized with
      // scene=0. Measured in Mine Souls III: accepted=126 scene=0 sceneCand=0
      // every frame with EVERY rejection counter at zero, so nothing was being
      // rejected for cause - the draws simply could not qualify.
      //
      // It also contradicted the capture layer, which deliberately supports a
      // viewport-derived projection (see capturedClipUsesWDepth: "a
      // viewport-derived replacement projection is still sufficient because
      // visible perspective vertices carry exact linear camera depth in clip.w").
      //
      // m_hasSeenRealSceneProjection is the evidence the policy actually wants:
      // it is set only when a draw presented a genuine projection, so a game
      // that produces real projections anywhere still refuses fallback draws,
      // while a game that never produces one is no longer locked out by a
      // camera derived from the fallback itself. The overlay/UI protections
      // below are structural and are unaffected.
      const bool strongViewportFallbackScene = dcs.transformData.usedViewportFallbackProjection
        && !m_hasSeenRealSceneProjection
        && hasSceneDepthSignal
        && primitiveCount >= 32u;
      const bool isSceneCandidate = (lift2D && primitiveCount >= 1u) || (hasSceneDepthSignal
        && primitiveCount >= 1u
        && ((hasRealProjection && hasViewOrStrongProjection) || strongViewportFallbackScene));

      if (cameraManager.hasSeenRealMainCamera() && !isSceneCandidate) {
        // Bounded admission telemetry for the remaining camera-enclosing slab
        // failure.  This records structural signals only (no per-vertex dump),
        // so a live game run can identify the non-scene draw family without
        // turning the hot path logger into a performance problem.
        static uint32_t sNonSceneAdmissionLogCount = 0;
        if (sNonSceneAdmissionLogCount < 96u) {
          ++sNonSceneAdmissionLogCount;
          Logger::info(str::format(
            "[D3D11Rtx][admission] accepted non-scene draw",
            " drawId=", dcs.drawCallID,
            " count=", count,
            " indexed=", indexed ? 1 : 0,
            " primitives=", primitiveCount,
            " zEnable=", dcs.zEnable ? 1 : 0,
            " zWrite=", dcs.zWriteEnable ? 1 : 0,
            " minZ=", dcs.minZ,
            " maxZ=", dcs.maxZ,
            " fallbackCamera=", dcs.transformData.usedViewportFallbackProjection ? 1 : 0,
            " identityWorld=", isIdentityExact(dcs.transformData.objectToWorld) ? 1 : 0,
            " identityView=", isIdentityExact(dcs.transformData.worldToView) ? 1 : 0,
            " cameraRelative=", dcs.transformData.cameraRelativeView ? 1 : 0,
            " textured=", dcs.materialData.usesTexture() ? 1 : 0,
            " textureHash=0x", std::hex,
            dcs.materialData.getColorTexture().getImageHash(),
            " materialHash=0x", dcs.materialData.getHash(),
            " categories=0x", dcs.getCategoryFlags().raw(),
              std::dec));
        }

        // A real scene camera is already established, so a draw that has no
        // scene-depth/projection evidence belongs to a raster overlay/helper
        // pass rather than the ray-traced world.  Replaying these draws through
        // transform feedback is not merely wasteful: Unreal's solid-colour UI
        // batches commonly reuse a scene VS with an unbound material texture.
        // Feeding their tiny clip-space quads to BLAS construction caused an
        // NVIDIA device reset at the first gameplay transition.  Preserve the
        // native draw for the UI compositor and keep it out of RT admission.
        // The test is structural and engine-independent; textured or depth-
        // participating world geometry continues through the scene path.
        const bool rasterOverlayOrHelper =
             !dcs.zEnable
          || dcs.transformData.usedViewportFallbackProjection
          || (primitiveCount <= 4u && !dcs.materialData.usesTexture());
        if (rasterOverlayOrHelper && !lift2D) {
          static uint32_t sRasterOverlaySkipLogCount = 0;
          if (sRasterOverlaySkipLogCount < 96u) {
            ++sRasterOverlaySkipLogCount;
            Logger::info(str::format(
              "[D3D11Rtx][raster-layer] preserved non-scene overlay/helper draw",
              " drawId=", dcs.drawCallID,
              " count=", count,
              " primitives=", primitiveCount,
              " zEnable=", dcs.zEnable ? 1 : 0,
              " zWrite=", dcs.zWriteEnable ? 1 : 0,
              " fallbackCamera=", dcs.transformData.usedViewportFallbackProjection ? 1 : 0,
              " textured=", dcs.materialData.usesTexture() ? 1 : 0));
          }
          return;
        }
      }

      if (isSceneCandidate) {

        // UE-style significance culling: this draw is a scene candidate. Count
        // it, and if the budgeting loop has armed a distance threshold, drop
        // candidates farther than it so the per-frame budget is spent on the
        // nearest (most important) geometry rather than arrival order. The
        // camera-space depth is column 3, row 2 of objectToView (the object
        // origin's view-space Z); abs() since view Z is negative looking down -Z.
        ++m_submitRejectStats.sceneCandidates;
        if (RtxOptions::significanceCulling() && m_significanceMaxDistanceSq > 0.0f) {
          const float viewZ = dcs.transformData.objectToView[3][2];
          const float distSq = viewZ * viewZ;
          if (distSq > m_significanceMaxDistanceSq) {
            ++m_submitRejectStats.significanceCulled;
            return;
          }
        }

        ++m_submitRejectStats.sceneAccepted;
        if (lift2D) {
          // Lifted 2D layers are a scene of their own, not evidence of a real
          // game camera (see EndFrame).
          ++m_submitRejectStats.lift2DAccepted;
        } else {
          if (hasRealProjection && hasViewOrStrongProjection) {
            ++m_submitRejectStats.realSceneAccepted;
            m_hasSeenRealSceneProjection = true;
            m_lastRealCameraFrameId = m_context->m_device->getCurrentFrameId();
          }
          // A perspective scene ends 2D lifting for good: a game projection
          // with a perspective w row, or a perspective clip matrix in the VS.
          const Matrix4& proj = dcs.transformData.viewToProjection;
          const bool perspectiveProjection = hasRealProjection
            && std::abs(proj[2][3]) > 1.0e-6f && std::abs(proj[3][3]) < 1.0e-6f;
          if (perspectiveProjection || clipPerspective)
            ++m_perspectiveSceneThisFrame;
          if (auto* sceneRtv = m_context->m_state.om.renderTargetViews[0].ptr()) {
            Rc<DxvkImageView> sceneView = sceneRtv->GetImageView();
            if (sceneView != nullptr)
              m_frameSceneTargetWidth = std::max(m_frameSceneTargetWidth, sceneView->image()->info().extent.width);
          }
        }
      }

      // Temporary diagnostic: describe the draws behind the screen-centre pick.
      {
        const XXH64_hash_t probeHash = s_centerPickHash.load();
        static uint32_t s_probeLogs = 0;
        static XXH64_hash_t s_lastProbeHash = kEmptyHash;
        if (probeHash != s_lastProbeHash) {
          s_lastProbeHash = probeHash;
          s_probeLogs = 0;
        }
        if (probeHash != kEmptyHash && s_probeLogs < 4
         && dcs.materialData.getColorTexture().getImageHash() == probeHash) {
          ++s_probeLogs;
          float probeDepthBias = 0.0f;
          uint32_t probeCull = 0;
          if (D3D11RasterizerState* probeRs = m_context->m_state.rs.state) {
            probeDepthBias = float(probeRs->Desc()->DepthBias) + probeRs->Desc()->SlopeScaledDepthBias;
            probeCull = uint32_t(probeRs->Desc()->CullMode);
          }
          Logger::info(str::format("[D3D11Rtx][center-probe] state: categories=0x", std::hex,
            dcs.getCategoryFlags().raw(), std::dec,
            " blend=", dcs.materialData.blendMode.enableBlending ? 1 : 0,
            " src=", uint32_t(dcs.materialData.blendMode.colorSrcFactor),
            " dst=", uint32_t(dcs.materialData.blendMode.colorDstFactor),
            " depthBias=", probeDepthBias, " cull=", probeCull,
            " alphaTest=", dcs.materialData.alphaTestEnabled ? 1 : 0,
            " vpDepth=", dcs.minZ, "-", dcs.maxZ));
          const Matrix4& o2w = dcs.transformData.objectToWorld;
          const Matrix4& w2v = dcs.transformData.worldToView;
          const AxisAlignedBoundingBox& box = dcs.geometryData.boundingBox;
          Logger::info(str::format("[D3D11Rtx][center-probe] tex=0x", std::hex, probeHash,
            " vs=0x", dcs.programmableVertexShaderBytecodeHash,
            " ps=0x", m_context->m_state.ps.shader != nullptr ? m_context->m_state.ps.shader->GetCommonShader()->GetBytecodeHash() : 0ull,
            std::dec, " count=", count, " indexed=", indexed ? 1 : 0,
            " zWrite=", dcs.zWriteEnable ? 1 : 0, " zEnable=", dcs.zEnable ? 1 : 0,
            " posStride=", dcs.geometryData.positionBuffer.stride(),
            " posFmt=", uint32_t(dcs.geometryData.positionBuffer.vertexFormat()),
            " o2wT=(", o2w[3][0], ",", o2w[3][1], ",", o2w[3][2], ")",
            " o2wScale=(", length(o2w[0].xyz()), ",", length(o2w[1].xyz()), ",", length(o2w[2].xyz()), ")",
            " w2vT=(", w2v[3][0], ",", w2v[3][1], ",", w2v[3][2], ")",
            " box=[", box.minPos.x, ",", box.minPos.y, ",", box.minPos.z, "]-[", box.maxPos.x, ",", box.maxPos.y, ",", box.maxPos.z, "]",
            " camRel=", dcs.transformData.cameraRelativeView ? 1 : 0,
            " fallbackProj=", dcs.transformData.usedViewportFallbackProjection ? 1 : 0,
            " refractive=", dcs.materialData.isRefractiveSurface ? 1 : 0));
        }
      }
    }

    // Exact world transform (reverse-engineered Creation Engine pattern, see
    // parseCameraRelativeWorldBinding). When the vertex shader is proven to
    // compute ViewProj * (absolute world with the eye subtracted) * POSITION,
    // the mesh is placed with the game's own world matrix instead of being
    // re-captured every frame and anchored with an estimate: placement is
    // exact, the vertex data is the game's static buffer (so the BLAS is
    // reused rather than rebuilt), and per-instance motion is the real change
    // of the world matrix. Requires the camera's exact eye (same buffer and
    // register the shader subtracts), so world and camera share one origin.
    bool exactWorldTransform = false;
    // Diagnostic: report once why a proven shader did not take the exact path.
    if (m_context->m_state.vs.shader != nullptr
     && m_context->m_state.vs.shader->GetCommonShader()->GetCameraRelativeWorldBinding().valid) {
      static uint32_t s_exactGateLogs = 0;
      const D3D11CameraRelativeWorldBinding& gb = m_context->m_state.vs.shader->GetCommonShader()->GetCameraRelativeWorldBinding();
      if (s_exactGateLogs < 6u && (instanceTransform != nullptr || requireExactPositionCapture
          || replayInstanceCount > 1u || m_eyeOffset == SIZE_MAX || m_projStage != 0
          || gb.cameraSlot != m_projSlot || size_t(gb.eyeRegister) * 16u != m_eyeOffset)) {
        ++s_exactGateLogs;
        Logger::info(str::format("[D3D11Rtx][exact-world] gate: instanceT=", instanceTransform != nullptr ? 1 : 0,
          " requireExact=", requireExactPositionCapture ? 1 : 0, " replayInstances=", replayInstanceCount,
          " eyeOffset=", m_eyeOffset == SIZE_MAX ? -1 : int64_t(m_eyeOffset), " projStage=", m_projStage,
          " projSlot=", m_projSlot, " camSlot=", gb.cameraSlot, " eyeReg=", gb.eyeRegister));
      }
    }
    if (RtxOptions::dx11ExactWorldTransforms() && !m_abDisableExactWorld && !lift2D
     && m_context->m_state.vs.shader != nullptr
     && instanceTransform == nullptr && !requireExactPositionCapture
     && replayInstanceCount <= 1u
     && m_eyeOffset != SIZE_MAX
     && m_projStage == 0 && m_projSlot != UINT32_MAX) {
      const D3D11CommonShader* exactVs = m_context->m_state.vs.shader->GetCommonShader();
      const D3D11CameraRelativeWorldBinding& wb = exactVs->GetCameraRelativeWorldBinding();
      if (wb.valid && wb.cameraSlot == m_projSlot && size_t(wb.eyeRegister) * 16u == m_eyeOffset
       && wb.worldSlot < D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT) {
        const auto& worldCb = m_context->m_state.vs.constantBuffers[wb.worldSlot];
        const uint8_t* worldPtr = worldCb.buffer != nullptr
          ? reinterpret_cast<const uint8_t*>(worldCb.buffer->GetMappedSlice().mapPtr) : nullptr;
        const size_t worldBase = size_t(worldCb.constantOffset) * 16u + size_t(wb.worldRegister) * 16u;
        if (worldPtr != nullptr && worldBase + 48u <= worldCb.buffer->Desc()->ByteWidth) {
          float rows[3][4];
          std::memcpy(rows, worldPtr + worldBase, sizeof(rows));
          bool finite = true;
          for (auto& r : rows) for (float v : r) finite &= std::isfinite(v);
          if (finite) {
            // Matrix4 is column storage: m[column][row].
            Matrix4 objectToWorld;
            for (uint32_t row = 0; row < 3; ++row)
              for (uint32_t col = 0; col < 4; ++col)
                objectToWorld[col][row] = rows[row][col];
            objectToWorld[0][3] = 0.0f; objectToWorld[1][3] = 0.0f;
            objectToWorld[2][3] = 0.0f; objectToWorld[3][3] = 1.0f;
            objectToWorld[3][0] += m_eyeOriginShift.x;
            objectToWorld[3][1] += m_eyeOriginShift.y;
            objectToWorld[3][2] += m_eyeOriginShift.z;
            dcs.transformData.objectToWorld = objectToWorld;
            dcs.transformData.objectToView = dcs.transformData.worldToView * objectToWorld;
            dcs.transformData.cameraRelativeView = false;
            dcs.transformData.usedViewportFallbackProjection = false;

            if (wb.hasUvTransform && wb.uvSlot < D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT) {
              const auto& uvCb = m_context->m_state.vs.constantBuffers[wb.uvSlot];
              const uint8_t* uvPtr = uvCb.buffer != nullptr
                ? reinterpret_cast<const uint8_t*>(uvCb.buffer->GetMappedSlice().mapPtr) : nullptr;
              const size_t uvOff = size_t(uvCb.constantOffset) * 16u + size_t(wb.uvRegister) * 16u;
              if (uvPtr != nullptr && uvOff + 16u <= uvCb.buffer->Desc()->ByteWidth) {
                float uv[4];  // xy = offset, zw = scale
                std::memcpy(uv, uvPtr + uvOff, sizeof(uv));
                if (std::isfinite(uv[0]) && std::isfinite(uv[1]) && std::isfinite(uv[2]) && std::isfinite(uv[3])) {
                  Matrix4 uvTransform;
                  uvTransform[0][0] = uv[2];
                  uvTransform[1][1] = uv[3];
                  uvTransform[3][0] = uv[0];
                  uvTransform[3][1] = uv[1];
                  dcs.transformData.textureTransform = uvTransform;
                }
              }
            }
            exactWorldTransform = true;
            ++m_submitRejectStats.exactWorldTransform;
          }
        }
      }
    }

    // Recover the exact position the rasterizer saw only after every
    // screen-space, missing-shader, and significance rejection has completed.
    // Replaying rejected draws consumed the old capture budget before real
    // scene geometry and created needless capture-buffer/BLAS pressure.
    // Projected (deferred / DBuffer / volume) decal: the box only exists to
    // rasterize the projection; in a ray tracer it would be a solid box. Put
    // the decal where it lands instead: a quad on the box's projection plane
    // (thinnest axis, through its centre) with the decal texture across the
    // box's other two axes, tagged as a decal so Remix layers it onto the
    // surface. Exact for flat receivers (most bullet holes, blood, posters).
    // Needs exact placement; without it the box is left out of the RT scene.
    bool projectedDecal = false;
    if (decalVolumeCandidate && !vertexPulled) {
      const bool placementExact = exactWorldTransform
        || (!dcs.transformData.cameraRelativeView && m_context->m_state.vs.shader != nullptr
            && m_context->m_state.vs.shader->GetCommonShader()->GetPositionTransformBinding() != nullptr);
      const RasterBuffer& box = dcs.geometryData.positionBuffer;
      const VkFormat boxFormat = box.vertexFormat();
      const uint32_t boxVertices = std::min(dcs.geometryData.vertexCount, 4096u);
      const uint8_t* boxData = nullptr;
      if (placementExact && box.defined() && box.stride() >= 12u && boxVertices >= 4u
       && (boxFormat == VK_FORMAT_R32G32B32_SFLOAT || boxFormat == VK_FORMAT_R32G32B32A32_SFLOAT)) {
        boxData = reinterpret_cast<const uint8_t*>(box.mapPtr(0));
        if (boxData == nullptr && posSem != nullptr) {
          const auto& vb = m_context->m_state.ia.vertexBuffers[posSem->inputSlot];
          const int64_t first = int64_t(vb.offset) + vertexStartIndex * int64_t(vb.stride);
          if (vb.buffer != nullptr && first >= 0)
            boxData = reinterpret_cast<const uint8_t*>(vb.buffer->GetIndexShadow(
              VkDeviceSize(first), VkDeviceSize(boxVertices) * box.stride()));
        }
      }
      if (boxData != nullptr) {
        Vector3 lo(FLT_MAX), hi(-FLT_MAX);
        bool finite = true;
        for (uint32_t v = 0; v < boxVertices && finite; ++v) {
          float p[3];
          std::memcpy(p, boxData + size_t(v) * box.stride() + box.offsetFromSlice(), sizeof(p));
          finite = std::isfinite(p[0]) && std::isfinite(p[1]) && std::isfinite(p[2]);
          for (uint32_t c = 0; c < 3; ++c) { lo[c] = std::min(lo[c], p[c]); hi[c] = std::max(hi[c], p[c]); }
        }
        const Vector3 extent = hi - lo;
        if (finite && extent.x > 0.0f && extent.y > 0.0f && extent.z > 0.0f) {
          // Projection axis = thinnest extent (decal depth); a cube projects along z.
          uint32_t axis = 2;
          if (extent.x < extent.y * 0.9f && extent.x < extent.z * 0.9f) axis = 0;
          else if (extent.y < extent.x * 0.9f && extent.y < extent.z * 0.9f) axis = 1;
          const uint32_t ua = (axis + 1u) % 3u, va = (axis + 2u) % 3u;
          // Full projection (METHODS.md, Decals): the box goes to the shading
          // stage as a decal record and lands on whatever surface lies inside
          // it, curved or not. worldToDecal = unit box from object box, after
          // the inverse of this draw's placement in the RT world.
          if (RtxOptions::dx11ProjectedDecalsAtShading() && dcs.materialData.getColorTexture().isValid()) {
            Matrix4 boxFromObject;  // identity
            for (uint32_t c = 0; c < 3; ++c) {
              boxFromObject[c][c] = 1.0f / extent[c];
              boxFromObject[3][c] = -0.5f * (lo[c] + hi[c]) / extent[c];
            }
            const Matrix4 worldToDecal = boxFromObject * inverse(dcs.transformData.objectToWorld);
            bool finiteDecal = true;
            for (uint32_t c = 0; c < 4; ++c)
              for (uint32_t r = 0; r < 4; ++r)
                finiteDecal &= std::isfinite(worldToDecal[c][r]);
            if (finiteDecal) {
              TextureRef decalTexture = dcs.materialData.getColorTexture();
              Rc<DxvkSampler> decalSampler = dcs.materialData.getSampler();
              m_context->EmitCs([worldToDecal, axis, decalTexture, decalSampler](DxvkContext* ctx) {
                static_cast<RtxContext*>(ctx)->addProjectedDecal(worldToDecal, axis, decalTexture, decalSampler);
              });
              ++m_submitRejectStats.projectedDecals;
              return;
            }
          }
          const float plane = 0.5f * (lo[axis] + hi[axis]);
          auto corner = [&](float su, float sv, float* out) {
            Vector3 p;
            p[axis] = plane;
            p[ua] = su > 0.5f ? hi[ua] : lo[ua];
            p[va] = sv > 0.5f ? hi[va] : lo[va];
            out[0] = p.x; out[1] = p.y; out[2] = p.z;
            out[3] = su; out[4] = 1.0f - sv;  // D3D UV: v grows downward
          };
          const float quad[6][2] = { {0,0}, {1,0}, {1,1}, {0,0}, {1,1}, {0,1} };
          constexpr VkDeviceSize kQuadBytes = 6u * 20u;
          Rc<DxvkBuffer> quadBuffer = AcquireHostVisibleHelperBuffer(kQuadBytes, "d3d11 rtx projected decal");
          float* q = quadBuffer != nullptr ? reinterpret_cast<float*>(quadBuffer->mapPtr(0)) : nullptr;
          if (q != nullptr) {
            for (uint32_t i = 0; i < 6; ++i)
              corner(quad[i][0], quad[i][1], q + i * 5u);
            RasterGeometry& g = dcs.geometryData;
            const DxvkBufferSlice slice(quadBuffer, 0, kQuadBytes);
            g.positionBuffer = RasterBuffer(slice, 0, 20u, VK_FORMAT_R32G32B32_SFLOAT);
            g.texcoordBuffer = RasterBuffer(slice, 12u, 20u, VK_FORMAT_R32G32_SFLOAT);
            g.normalBuffer = RasterBuffer();
            g.color0Buffer = RasterBuffer();
            g.blendWeightBuffer = RasterBuffer();
            g.blendIndicesBuffer = RasterBuffer();
            g.indexBuffer = RasterBuffer();
            g.indexCount = 0;
            g.vertexCount = 6;
            g.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
            g.cullMode = VK_CULL_MODE_NONE;
            projectedDecal = true;
            ++m_submitRejectStats.projectedDecals;
          }
        }
      }
      if (!projectedDecal) {
        ++m_submitRejectStats.decalVolumeUnresolved;
        return;
      }
    }

    const uint32_t captureBudgetRejectsBefore =
      m_submitRejectStats.positionCaptureBudgetRejected;
    // A lifted 2D layer: its depth in painter's order (later = nearer).
    m_lift2DDraw = lift2D;
    if (lift2D) {
      const uint32_t layer = std::min(m_lift2DLayer++, kLift2DLayers - 1u);
      m_lift2DDepth = kLift2DFar - (kLift2DFar - kLift2DNear) * (float(layer) / float(kLift2DLayers));
    }
    const bool capturedExactPositions = !pcsx2PostTransformDraw && !exactWorldTransform
      && !projectedDecal
      && TryCapturePositionsViaStreamOut(
        dcs, geo, indexed, count, start, base,
        instanceTransform != nullptr, replayFirstInstance, replayInstanceCount,
        usedWholeVertexBufferFallback && !tessellatedDraw);
    const bool exactCaptureBudgetRejected =
      m_submitRejectStats.positionCaptureBudgetRejected != captureBudgetRejectsBefore;
    m_lift2DDraw = false;
    if (lift2D && !capturedExactPositions) {
      // Without its captured clip position a sprite has no placement.
      ++m_submitRejectStats.lift2DCaptureFailed;
      return;
    }
    if (eyeBoundsRequireExactCapture && !capturedExactPositions) {
      // Preserve a complete native frame instead of submitting uncertain mesh
      // placement or presenting a partially captured world with missing walls.
      // Drop only this draw: its placement is unverified. A whole native
      // frame here hid the entire RT scene behind one ambiguous quad.
      ++m_submitRejectStats.collapsedEyeGeometry;
      return;
    }
    // First-person passes (arms, weapon) are drawn into a reserved depth range
    // with the game's separate first-person camera. They must never steer the
    // main RT camera: when one wins the frame's first-touch, the whole scene is
    // viewed through the first-person camera instead of the world camera.
    if (m_context->m_state.rs.numViewports > 0
     && isReservedDepthViewport(m_context->m_state.rs.viewports[0])) {
      dcs.allowMainCameraUpdate = false;

      // Temporary diagnostic: first-person camera vs main camera direction.
      static uint32_t s_lastFirstPersonLogFrame = 0;
      const uint32_t fpFrame = m_context->m_device->getCurrentFrameId();
      if (fpFrame >= s_lastFirstPersonLogFrame + 120u) {
        s_lastFirstPersonLogFrame = fpFrame;
        const Matrix4& v = dcs.transformData.worldToView;
        const auto& mainCamera = m_context->m_device->getCommon()->getSceneManager()
          .getCameraManager().getCamera(CameraType::Main);
        const Vector3 mainForward = mainCamera.getDirection(false);
        Logger::info(str::format("[D3D11Rtx][first-person-camera] frame=", fpFrame,
          " fpViewRow2=(", v[0][2], ",", v[1][2], ",", v[2][2], ")",
          " fpViewRow0=(", v[0][0], ",", v[1][0], ",", v[2][0], ")",
          " fpT=(", v[3][0], ",", v[3][1], ",", v[3][2], ")",
          " mainFwd=(", mainForward.x, ",", mainForward.y, ",", mainForward.z, ")",
          " camRel=", dcs.transformData.cameraRelativeView ? 1 : 0,
          " captured=", capturedExactPositions ? 1 : 0));
      }
    }

    if (capturedExactPositions) {
      ++m_submitRejectStats.positionCaptured;
      // Exact indexed capture must own one compact vertex per source index and
      // therefore must have consumed the application's index buffer.  Keep a
      // final submission firewall here so a future topology/capture change
      // cannot silently reintroduce an index domain that addresses beyond the
      // XFB allocation and hangs the GPU.
      if (indexed && dcs.geometryData.indexBuffer.defined()) {
        Logger::err(str::format(
          "[D3D11Rtx][position-capture] rejected mismatched captured index domain: drawId=",
          dcs.drawCallID,
          " count=", count,
          " capturedVertices=", dcs.geometryData.vertexCount));
        return;
      }
    } else if (!pcsx2PostTransformDraw
            && (requireExactPositionCapture
             || exactCaptureBudgetRejected
             || (dcs.transformData.cameraRelativeView
              && dcs.usesVertexShader))) {
      // exactCaptureBudgetRejected: capture was the chosen path for this draw
      // and only the per-frame budget deferred it. Falling back to a guessed
      // cbuffer transform placed Fallout 4's streamed-in world as huge slabs
      // stretched around the camera (black RT frame). Leave it out this frame;
      // a later frame captures it exactly.
      // A camera-relative camera defines the RT world as current view space.
      // IA object-space positions combined with a guessed generic cbuffer
      // matrix do not belong to that world. Submitting them anyway is worse
      // than a missing mesh: their triangles become the enclosing slabs and
      // camera-following black rectangle that occlude every valid hit.
      // DX11_V299_INSTANCED_CAMERA_RELATIVE_GUARD: a fitted per-instance world
      // matrix is no more valid here than a guessed cbuffer matrix - it is
      // expressed in the game's world space, which does not exist while the RT
      // world IS view space. The old instanceTransform==nullptr exemption let
      // exactly those batches through and they are the largest meshes in a
      // Unity/Unreal frame, so they produced the black enclosing box.
      // Skip only this draw; the rest of the frame remains path traced.
      ++m_submitRejectStats.unsafeCameraRelativeSkipped;
      static uint32_t sUnsafeCameraRelativeSkipLogCount = 0;
      if (sUnsafeCameraRelativeSkipLogCount < 32) {
        ++sUnsafeCameraRelativeSkipLogCount;
        Logger::warn(str::format(
          "[D3D11Rtx][position-capture] skipped unsafe uncaptured draw: reason=",
          exactCaptureBudgetRejected ? "budget"
            : (usedWholeVertexBufferFallback ? "gpu-index-flatten-required"
              : (requireExactPositionCapture ? "instanced-exact-required" : "camera-relative")),
          " count=",
          count,
          " indexed=", indexed ? 1 : 0,
          " start=", start,
          " base=", base,
          " firstInstance=", replayFirstInstance,
          " instanceCount=", replayInstanceCount,
          " drawId=", dcs.drawCallID));
      }
      return;
    }

    if (deferTexcoordRecoveryToPositionCapture && !projectedDecal
     && !geo.texcoordBuffer.defined()) {
      // Position capture may be unavailable for a safe non-camera-relative
      // draw (budget/capability/profile). Retain coverage by trying the legacy
      // dedicated UV replay before using the explicit flat fallback.
      if (!(vertexPulled && indexed)
       && TryCaptureTexcoordsViaStreamOut(dcs, geo, indexed, count, start, base))
        ++m_submitRejectStats.texcoordCaptured;
      else
        applyMissingTexcoordFallback();
    }

    // Exact-world draws hand Remix the game's own vertex buffer, sized from
    // vertex 0 up to the highest index. Precombined meshes share one large
    // buffer, so a draw that uses vertices 50000..52000 copied 52000 vertices
    // into its Remix geometry buffer - gigabytes across a scene. Rebase the
    // draw onto the range its indices actually touch.
    if (exactWorldTransform && indexed && dcs.geometryData.indexBuffer.defined()) {
      RebaseIndexedVertexRange(dcs, count, idxShadowSource, idxShadowOffset);
    }

    DrawParameters params;
    params.instanceCount = 1;
    const bool submitIndexed = indexed && dcs.geometryData.indexBuffer.defined();
    params.vertexCount   = submitIndexed ? 0
      : ((capturedExactPositions || projectedDecal) ? dcs.geometryData.vertexCount : count);
    params.indexCount    = submitIndexed ? count : 0;
    // SubmitDraw already folds StartIndexLocation and BaseVertexLocation (or
    // StartVertexLocation) into RasterBuffer slice offsets above. Reapplying
    // them here double-offsets sky/terrain helper draws and can read beyond the
    // compact capture. The RT-facing buffers always begin at element zero.
    params.firstIndex    = 0;
    params.vertexOffset  = 0;

    if (havePassKey) {
      m_passKeysThisFrame.insert(passKey);
      m_meshKeysThisFrame.insert(meshKey);
    }

    dcs.gameDraw.valid = !m_indirectReplay.active;
    dcs.gameDraw.indexed = indexed;
    dcs.gameDraw.count = count;
    dcs.gameDraw.start = start;
    dcs.gameDraw.base = base;
    dcs.gameDraw.firstInstance = replayFirstInstance;
    dcs.gameDraw.instanceCount = std::max(replayInstanceCount, 1u);

    m_context->EmitCs([params, dcs](DxvkContext* ctx) mutable {
      static_cast<RtxContext*>(ctx)->commitGeometryToRT(params, dcs);
    });

    // CPU-GPU pacing: flush the CS chunk periodically so the GPU can start
    // processing draw batches while the CPU is still recording.  Without
    // this, the CPU can race thousands of draws ahead, bloating memory with
    // buffered DrawCallState objects and causing the GPU to stall at end-of-
    // frame when it has to process the entire backlog at once.
    if (++m_drawsSinceFlush >= m_drawsPerFlush) {
      m_drawsSinceFlush = 0;
      m_context->FlushCsChunk();
    }
  }

  void D3D11Rtx::UpdateTrackedExtents(const Rc<DxvkImage>& outputImage, VkExtent2D remixViewportExtent) {
    // Capture the stable previous values BEFORE any mutation. Every
    // comparison below must run against last frame's state — comparing the
    // incoming extent against a tracker this function already overwrote
    // (the bug in the previous EndFrame implementation) makes the
    // "much smaller than stable output" test compare a value against itself.
    const VkExtent2D previousStableOutput   = m_lastOutputExtent;
    const VkExtent2D previousStableViewport = m_lastRemixViewportExtent;

    VkExtent2D outputExtent = { 0u, 0u };
    if (outputImage != nullptr) {
      const VkExtent3D e = outputImage->info().extent;
      if (e.width > 0u && e.height > 0u)
        outputExtent = { e.width, e.height };
    }

    // The Remix-owned output extent is the only fallback for a missing
    // viewport extent. Note this is NOT the inverse promotion the old code
    // did: a valid sub-output viewport (letterboxed scene) is preserved
    // as-is and never silently replaced with the larger output extent,
    // otherwise remixViewportAspect and the viewport-fallback projection
    // would be computed from the wrong rectangle.
    if (remixViewportExtent.width == 0u || remixViewportExtent.height == 0u)
      remixViewportExtent = outputExtent;

    // Heuristic: does this extent look like a small helper/launcher window
    // occluding the real game output (overlay swapchains, splash windows,
    // emulator tool panes) rather than a legitimate resize?
    const auto isOccludingHelperExtent = [&](VkExtent2D candidate) -> bool {
      if (candidate.width == 0u || candidate.height == 0u)
        return false; // empty extents are skipped by applyExtent, not "occluding"

      const bool hadStableViewport =
        previousStableViewport.width >= 640u && previousStableViewport.height >= 480u;
      const bool hadStableOutput =
        previousStableOutput.width >= 640u && previousStableOutput.height >= 480u;

      if (!hadStableViewport && !hadStableOutput)
        return false; // nothing stable to defend yet — accept whatever arrives

      const bool tiny = candidate.width < 640u || candidate.height < 480u;

      const auto muchSmallerThan = [&](VkExtent2D stable) {
        return (uint64_t(candidate.width)  * 10ull < uint64_t(stable.width)  * 7ull)
            || (uint64_t(candidate.height) * 10ull < uint64_t(stable.height) * 7ull);
      };

      const bool muchSmallerThanViewport = hadStableViewport && muchSmallerThan(previousStableViewport);
      const bool muchSmallerThanOutput   = hadStableOutput   && muchSmallerThan(previousStableOutput);

      return tiny || muchSmallerThanViewport || muchSmallerThanOutput;
    };

    // Persistence escape hatch: a rejected extent that keeps arriving is the
    // new reality (the user really did shrink the window below the heuristic
    // floor). Returns true once the same extent has been rejected enough
    // consecutive times that it should be accepted after all.
    const auto rejectedExtentBecamePersistent = [&](VkExtent2D rejected) -> bool {
      if (rejected.width == m_pendingRejectedExtent.width
       && rejected.height == m_pendingRejectedExtent.height) {
        if (++m_pendingRejectedExtentCount >= kRejectedExtentAcceptEvents) {
          m_pendingRejectedExtentCount = 0;
          return true;
        }
      } else {
        m_pendingRejectedExtent = rejected;
        m_pendingRejectedExtentCount = 1;
      }
      return false;
    };

    // driveResizeTransition: per the header contract, only m_lastOutputExtent
    // changes may trigger resize-grace handling.
    const auto applyExtent = [&](VkExtent2D newExtent, VkExtent2D& trackedExtent, bool driveResizeTransition) {
      if (newExtent.width == 0u || newExtent.height == 0u)
        return;

      if (driveResizeTransition
       && trackedExtent.width != 0u && trackedExtent.height != 0u
       && (trackedExtent.width != newExtent.width || trackedExtent.height != newExtent.height)) {
        m_resizeTransitionFramesRemaining = std::max(m_resizeTransitionFramesRemaining, kResizeCameraGraceFrames);
      }

      trackedExtent = newExtent;
    };

    const auto considerExtent = [&](VkExtent2D candidate, VkExtent2D& trackedExtent, bool driveResizeTransition, const char* trackerName) {
      if (candidate.width == 0u || candidate.height == 0u)
        return;

      // During a genuine resize transition new extents flow through freely;
      // outside one, an occluding-helper-looking extent is rejected so a
      // launcher/overlay swapchain cannot clobber the trackers, trigger
      // bogus resize grace every flip-flopped present, or shrink the
      // viewport-fallback projection. Crucially this now protects
      // m_lastOutputExtent too — previously only the viewport tracker was
      // guarded, so a 320x240 helper present poisoned the output extent and
      // kept the resize-carryover camera hack permanently engaged.
      const bool occluding = m_resizeTransitionFramesRemaining == 0
                          && isOccludingHelperExtent(candidate);

      if (occluding && !rejectedExtentBecamePersistent(candidate)) {
        static uint32_t sIgnoredSmallExtentLogCount = 0;
        if (sIgnoredSmallExtentLogCount < 16) {
          ++sIgnoredSmallExtentLogCount;
          Logger::info(str::format(
            "[D3D11Rtx] Ignoring small/occluding ", trackerName, " extent update: new=",
            candidate.width, "x", candidate.height,
            " prevViewport=", previousStableViewport.width, "x", previousStableViewport.height,
            " prevOutput=", previousStableOutput.width, "x", previousStableOutput.height,
            " rejectStreak=", m_pendingRejectedExtentCount));
        }
        return;
      }

      applyExtent(candidate, trackedExtent, driveResizeTransition);
    };

    // Debounce output-extent changes: deferred pipelines bind several RT
    // sizes per frame at scene transitions; committing each one re-armed
    // resize grace every frame (resize storm). A changed extent must repeat
    // kResizeDebounceFrames times consecutively before it commits.
    if (outputExtent.width != 0u && outputExtent.height != 0u
     && m_lastOutputExtent.width != 0u && m_lastOutputExtent.height != 0u
     && (outputExtent.width != m_lastOutputExtent.width || outputExtent.height != m_lastOutputExtent.height)) {
      if (outputExtent.width == m_pendingResizeExtent.width && outputExtent.height == m_pendingResizeExtent.height) {
        ++m_pendingResizeCount;
      } else {
        m_pendingResizeExtent = outputExtent;
        m_pendingResizeCount = 1;
      }
      if (m_pendingResizeCount < kResizeDebounceFrames) {
        outputExtent = { 0u, 0u }; // not yet: skip the output-tracker update this round
      } else {
        m_pendingResizeCount = 0;
      }
    } else {
      m_pendingResizeCount = 0;
    }

    considerExtent(outputExtent,        m_lastOutputExtent,        true,  "output");
    considerExtent(remixViewportExtent, m_lastRemixViewportExtent, false, "Remix viewport");

    // Any accepted frame with non-occluding extents resets the persistence
    // streak so unrelated later rejections start counting from scratch.
    if (outputExtent.width != 0u
     && !(m_resizeTransitionFramesRemaining == 0 && isOccludingHelperExtent(outputExtent))) {
      m_pendingRejectedExtent = { 0u, 0u };
      m_pendingRejectedExtentCount = 0;
    }
  }

  void D3D11Rtx::RequestScreenshot() {
    m_context->EmitCs([](DxvkContext*) {
      RtxContext::triggerScreenshot(false);
    });
  }

  void D3D11Rtx::EndFrame(const Rc<DxvkImage>& backbuffer, VkExtent2D remixViewportExtent) {
    ScopedCpuProfileZoneN("D3D11Rtx::EndFrame");
    // Remember the presented image: UI drawn anywhere else is an offscreen
    // UI target that is composited onto it later (see routeRasterUiLayer).
    m_lastBackbufferImage = backbuffer.ptr();
    if (!m_frameDrawLights.empty()) {
      m_submitRejectStats.tiledLightsImported += uint32_t(m_frameDrawLights.size());
      m_context->EmitCs([cLights = std::move(m_frameDrawLights)](DxvkContext* ctx) {
        static_cast<RtxContext*>(ctx)->addLights(cLights.data(), uint32_t(cLights.size()));
      });
      m_frameDrawLights = {};
      m_frameDrawLightKeys.clear();
    }
    // DX11_V301_PERF_LOG: report where the frame's CPU time went in the capture
    // layer. The raytracing passes time themselves and land in the low
    // milliseconds, so when the frame rate is far below what the scene warrants
    // the cost is here. Reporting the single slowest draw alongside the total
    // separates "many small draws" from "one pathological draw" - a distinction
    // a frame-level number alone cannot make.
    if (RtxOptions::logDrawSubmissionPerf() && m_frameTimedDraws > 0) {
      const double totalMs = double(m_frameDrawCpuNs) / 1.0e6;
      const double slowestMs = double(m_frameSlowestDrawNs) / 1.0e6;

      // DX11_V319_PERF_LOG_THROTTLE: a game that never drops below the
      // threshold used to emit one warning EVERY frame - Saints Row IV wrote
      // 21,966 of them into a single 5.8 MB log, drowning every other
      // diagnostic in the file. Report the opening burst so the problem is
      // visible at once, then fall back to a periodic sample. A frame
      // materially worse than anything reported so far always gets through:
      // the throttle must not hide an escalation, only the steady state.
      const uint32_t intervalFrames = RtxOptions::logDrawSubmissionPerfIntervalFrames();
      constexpr uint32_t kPerfLogOpeningBurst = 8u;
      constexpr double kPerfLogEscalationFactor = 2.0;

      const uint32_t perfFrame = m_context->m_device->getCurrentFrameId();
      const bool withinOpeningBurst = m_drawPerfLogCount < kPerfLogOpeningBurst;
      const bool intervalElapsed = intervalFrames == 0u
        || m_drawPerfLogLastFrame == ~0u
        || perfFrame - m_drawPerfLogLastFrame >= intervalFrames;
      const bool escalated = totalMs > m_drawPerfLogWorstMs * kPerfLogEscalationFactor;

      if (totalMs >= double(RtxOptions::logDrawSubmissionPerfThresholdMs())
       && (withinOpeningBurst || intervalElapsed || escalated)) {
        ++m_drawPerfLogCount;
        m_drawPerfLogLastFrame = perfFrame;
        m_drawPerfLogWorstMs = std::max(m_drawPerfLogWorstMs, totalMs);

        Logger::warn(str::format(
          "[D3D11Rtx][perf] draw submission cost ", totalMs, " ms across ",
          m_frameTimedDraws, " draws (avg ", totalMs / double(m_frameTimedDraws),
          " ms); slowest single draw ", slowestMs, " ms"
          " drawId=", m_frameSlowestDrawId,
          " indices=", m_frameSlowestDrawIndices,
          " psHash=0x", std::hex, m_frameSlowestDrawHash, std::dec,
          // DX11_V312_PHASE_TIMERS: where the frame's CPU time actually went.
          // If these three sum to roughly the total, the blocker is named. If
          // they are all near zero while the total is ~96ms, the stall is
          // somewhere else in SubmitDraw and the next probe goes deeper.
          " | extract=", double(m_framePhaseExtractNs) / 1.0e6,
          " ms material=", double(m_framePhaseMaterialNs) / 1.0e6,
          " ms helperAcquire=", double(m_framePhaseHelperNs) / 1.0e6, " ms"));
      }
    }

    m_framePhaseExtractNs = 0;
    m_framePhaseMaterialNs = 0;
    m_framePhaseHelperNs = 0;
    m_frameDrawCpuNs = 0;
    m_frameSlowestDrawNs = 0;
    m_frameSlowestDrawId = 0;
    m_frameSlowestDrawIndices = 0;
    m_frameSlowestDrawHash = kEmptyHash;
    m_frameTimedDraws = 0;

    // DX11_V318_CATEGORY_TABLE_REFRESH: rebuild the hash -> category-bits lookup
    // table if any texture-category option set changed since the last frame.
    // Without this call the table was built lazily on the very first draw and
    // then frozen for the life of the process, so every texture tagged in the
    // Remix UI after that point - sky, terrain, ignore, decal, particle, UI -
    // silently did nothing. refreshCategoryLookupTable is a cheap size
    // fingerprint compare when nothing changed, and this runs on the same
    // draw-submission thread that reads the table.
    DrawCallState::refreshCategoryLookupTable();

    // DX11_V263_CRASH_FILTER_SAFE: games install their own unhandled-exception
    // filter during startup, replacing ours; periodically re-assert so the
    // crash signature is always logged (theirs still runs via the chain).
    {
      static uint32_t s_filterReassertCounter = 0;
      if ((s_filterReassertCounter++ & 255u) == 0u)
        ::RemixReassertCrashSignatureFilter();
    }

    // Seal this frame's camera samples and consume only GPU-completed batches.
    // Each estimator advances once per source batch, preserving gaps without
    // mixing capture frames or blocking on the CS thread/GPU.
    ConsumeCameraAnchorSamples();

    // An in-process GPU capture is the only reliable way to diagnose a
    // fullscreen game launched through Steam (desktop capture APIs run in a
    // different session). Drop dx11-remix-screenshot.flag beside the game
    // executable; it is consumed once and captures the final image plus the
    // albedo, normals, motion, depth, noisy and denoised lighting buffers.
    // Poll at a low cadence so the dormant diagnostic has negligible cost.
    const uint32_t screenshotFrame = m_context->m_device->getCurrentFrameId();
    if ((screenshotFrame & 31u) == 0u
     && ::GetFileAttributesW(L"dx11-remix-screenshot.flag") != INVALID_FILE_ATTRIBUTES) {
      ::DeleteFileW(L"dx11-remix-screenshot.flag");
      m_context->EmitCs([](DxvkContext*) {
        RtxContext::triggerScreenshot(true);
      });
      Logger::info("[D3D11Rtx] Consumed dx11-remix-screenshot.flag; capturing GPU debug images");
    }

    // DX11_V280: per-frame stream-out capture budget.
    // DX11_V319_LIGHT_VOLUMES_SELF_ARM: arm the capture once the game has shown,
    // over several consecutive frames, that it draws light volumes.
    //
    // One frame is not enough evidence - a handful of small additive draws can
    // occur anywhere - so a run of frames is required, and any frame that falls
    // short resets the run. That makes the decision a property of how the game
    // renders rather than of which game it is, which is the whole point: an
    // engine that lights deferred arms this by behaving like one, and nothing
    // has to recognise it by name.
    if (RtxOptions::deferredLightVolumeAutoDetect()
     && !m_deferredLightVolumeAutoArmed
     && !RtxOptions::deferredLightVolumeCapture()) {
      if (m_deferredLightVolumeCandidatesThisFrame
            >= RtxOptions::deferredLightVolumeAutoDetectMinPerFrame()) {
        ++m_deferredLightVolumeArmingFrames;
        if (m_deferredLightVolumeArmingFrames
              >= RtxOptions::deferredLightVolumeAutoDetectFrames()) {
          m_deferredLightVolumeAutoArmed = true;
          Logger::info(str::format(
            "[D3D11Rtx][light-volume] deferred light volumes detected (",
            m_deferredLightVolumeCandidatesThisFrame, " per frame for ",
            m_deferredLightVolumeArmingFrames, " frames); capturing them as lights. "
            "This runtime reads no game lights otherwise. "
            "Set rtx.dx11.deferredLightVolumeAutoDetect=False to stop this."));
        }
      } else {
        m_deferredLightVolumeArmingFrames = 0;
      }
    }
    m_deferredLightVolumeCandidatesThisFrame = 0;

    m_deferredLightVolumesThisFrame = 0;
    m_deferredLightVolumePositionsThisFrame.clear();
    // Water textures stay known across frames; bound the set for streaming worlds.
    if (m_refractiveSurfaceTextures.size() > 4096u)
      m_refractiveSurfaceTextures.clear();
    m_texcoordCapturesThisFrame = 0;
    m_texcoordCaptureBytesThisFrame = 0;
    m_positionCapturesThisFrame = 0;
    m_positionNewCaptureBuffersThisFrame = 0;
    m_positionReplayCapturesThisFrame = 0;
    m_positionCaptureBytesThisFrame = 0;
    m_positionCaptureVerticesSinceSubmission = 0;
    m_positionCaptureOccurrencesThisFrame.clear();

    // DX11_V285: age out capture buffers for meshes no longer drawn, and
    // return provably-released helper buffers to the reuse pool.
    SweepTexcoordCaptureCache(m_context->m_device->getCurrentFrameId());
    SweepPositionCaptureCache(m_context->m_device->getCurrentFrameId());
    RecycleHelperBuffers();

    UpdateTrackedExtents(backbuffer, remixViewportExtent);

    // DX11_V255_FULLRES_TARGET_GUARD: the ray tracer's output resolution is the
    // extent of the image injected into (injectRTX sizes everything from
    // targetImage->info().extent). Games create helper/dummy swapchains (2x2
    // observed in Saints Row IV) and small intermediate targets; if one of those
    // ever reaches this point as the injection target, the whole path-traced
    // frame renders at that tiny size instead of the monitor resolution. Never
    // inject into a target dramatically smaller than the established output
    // extent - skip the frame and let the full-resolution primary drive RT.
    if (backbuffer != nullptr
     && m_lastOutputExtent.width > 0u && m_lastOutputExtent.height > 0u) {
      const VkExtent3D targetExtent = backbuffer->info().extent;
      if (targetExtent.width * 2u < m_lastOutputExtent.width
       || targetExtent.height * 2u < m_lastOutputExtent.height) {
        static uint32_t sSmallTargetSkipLog = 0;
        if (sSmallTargetSkipLog < 8) {
          ++sSmallTargetSkipLog;
          Logger::info(str::format("[D3D11Rtx] Skipping RTX injection into undersized target ",
            targetExtent.width, "x", targetExtent.height, " (output is ",
            m_lastOutputExtent.width, "x", m_lastOutputExtent.height, ")"));
        }
        return;
      }
    }

    // Let the real-camera latch decay after extended absence so menu and
    // loading-screen draws (viewport-fallback reliant) are not permanently
    // blocked once a session has run. ~4x the scene grace window.
    if (m_hasSeenRealSceneProjection) {
      const uint32_t currentFrame = m_context->m_device->getCurrentFrameId();
      if (currentFrame > m_lastRealCameraFrameId
       && (currentFrame - m_lastRealCameraFrameId) > kSceneCameraGraceFrames * 4u) {
        m_hasSeenRealSceneProjection = false;
      }
    }

    const uint32_t gameViewportCount = m_context->m_state.rs.numViewports;
    const VkExtent2D singleRemixViewportExtent = m_lastRemixViewportExtent;
    const uint32_t draws = m_drawCallID;
    const uint32_t acceptedDraws = m_submitRejectStats.accepted;
    m_prevFrameSceneAccepted = m_submitRejectStats.sceneAccepted;
    m_prevFrameRealSceneAccepted = m_submitRejectStats.realSceneAccepted;

    // UE-style significance control loop. Adjust the squared-distance threshold
    // toward the instance budget for next frame: if this frame had more scene
    // candidates than the budget, tighten (admit only nearer geometry); if it
    // comfortably fit, relax/disarm so sparse views regain full detail. The
    // step is multiplicative and clamped to +/-40%/frame, so the threshold
    // glides rather than popping. Disarmed (==0) means "no limit".
    if (RtxOptions::significanceCulling()) {
      const uint32_t budget = std::max(RtxOptions::maxInstanceSubmissions(), 1u);
      const uint32_t candidates = m_submitRejectStats.sceneCandidates;
      const float farthestKeptSq = m_significanceMaxDistanceSq;
      if (candidates > budget) {
        // Over budget: tighten. Seed from the current accepted set's implied
        // reach if disarmed, else shrink by the overshoot ratio (capped).
        const float ratio = static_cast<float>(budget) / static_cast<float>(candidates);
        const float shrink = std::max(ratio, 0.6f); // never below 60%/frame
        if (m_significanceMaxDistanceSq <= 0.0f) {
          // First arm: start generous (a large reach) so only the farthest are cut.
          m_significanceMaxDistanceSq = 1.0e12f * shrink;
        } else {
          m_significanceMaxDistanceSq *= shrink;
        }
      } else if (m_significanceMaxDistanceSq > 0.0f) {
        // Within budget: relax by up to 40%/frame; disarm once very large.
        m_significanceMaxDistanceSq *= 1.4f;
        if (m_significanceMaxDistanceSq > 1.0e13f) {
          m_significanceMaxDistanceSq = 0.0f; // disarm: no limit needed
        }
      }
      (void) farthestKeptSq;
    } else {
      m_significanceMaxDistanceSq = 0.0f;
    }
    m_prevFrameSceneCandidates = m_submitRejectStats.sceneCandidates;


    const uint32_t sceneAcceptedDraws = m_submitRejectStats.sceneAccepted;
    const uint32_t realSceneAcceptedDraws = m_submitRejectStats.realSceneAccepted;
    const uint32_t sceneCandidateDraws = m_submitRejectStats.sceneCandidates;
    const bool rasterUiSeen = m_rasterUiSeenThisFrame;
    const bool midFrameRtxInjected = m_midFrameRtxInjected;
    const bool forceRasterPassThrough = m_forceRasterPassThroughThisFrame;
    // Lifted 2D layers are the whole scene of a 2D game (see Lift2DProjection).
    const uint32_t lift2DAcceptedDraws = m_submitRejectStats.lift2DAccepted;
    const uint32_t trustedSceneAcceptedDraws = std::max(lift2DAcceptedDraws, realSceneAcceptedDraws > 0
      ? realSceneAcceptedDraws
      : (m_hasSeenRealSceneProjection ? 0u : sceneAcceptedDraws));
    static uint32_t s_endFrameLogCount = 0;
    static uint32_t s_submitSummaryLogCount = 0;
    if (s_endFrameLogCount < 8) {
      ++s_endFrameLogCount;
      Logger::info(str::format("[D3D11Rtx] EndFrame: draws=", draws,
        " processWideDraws=", s_processWideSubmittedDraws.load(std::memory_order_relaxed),
        " backbuffer=", backbuffer != nullptr ? 1 : 0,
        " remixViewport=", singleRemixViewportExtent.width, "x", singleRemixViewportExtent.height,
        " gameRasterViewports=", gameViewportCount,
        " singleRemixViewport=1"));
    }
    if (gameViewportCount > 1) {
      static uint32_t s_multiViewportLogCount = 0;
      if (s_multiViewportLogCount < 8) {
        ++s_multiViewportLogCount;
        Logger::info(str::format(
          "[D3D11Rtx] Game submitted multiple raster viewports; Remix output remains one viewport and viewport-fallback camera selection stays disabled for this frame. gameRasterViewports=",
          gameViewportCount,
          " remixViewport=", singleRemixViewportExtent.width, "x", singleRemixViewportExtent.height));
      }
    }
    // DX11_V286: the 24-line session cap was fully consumed at the menu, so
    // in-world accept/reject statistics were never visible in field logs.
    //
    // DX11_V304_SUBMIT_SUMMARY_REACHES_WORLD: the fix above still never
    // reported in-world geometry. Two reasons, both observed in a field log
    // that crashed 12s after the player loaded in:
    //
    //  o The 24-line burst was again spent entirely on menu/loading frames
    //    (accepted=2..4 of 25..49 draws), because those frames arrive first.
    //  o "every 900 frames" is a frame COUNT, but the frame id advances twice
    //    per present here, and an in-world frame costs ~100ms. 900 ids is
    //    therefore ~45 seconds of gameplay - longer than the session lasted.
    //
    // So the one statistic that diagnoses a black/empty scene was structurally
    // unobservable. Make the cadence wall-clock (a slow frame no longer delays
    // the report) and spend the burst on frames that actually carry scene
    // candidates, so menu frames cannot consume it.
    const bool frameHasSceneGeometry = m_submitRejectStats.sceneCandidates > 0;

    static std::chrono::steady_clock::time_point s_lastSubmitSummaryTime {};
    const auto submitSummaryNow = std::chrono::steady_clock::now();
    static const bool logCaptureDiagnostics = env::getEnvVar("DXVK_REMIX_CAPTURE_LOG") == "1";
    // Steam launches never carry per-launch environment variables, so the
    // in-world summary is periodic by default (one line every few seconds);
    // DXVK_REMIX_CAPTURE_LOG=1 only shortens the interval.
    const bool submitSummaryPeriodicDue =
      s_lastSubmitSummaryTime.time_since_epoch().count() == 0
      || (submitSummaryNow - s_lastSubmitSummaryTime)
           >= std::chrono::seconds(logCaptureDiagnostics ? 3 : 5);

    // Budget the burst separately for menu and world so neither starves the
    // other: whichever kind of frame is running, the first few are reported.
    static uint32_t s_submitSummaryWorldLogCount = 0;
    const uint32_t burstBudget = frameHasSceneGeometry
      ? s_submitSummaryWorldLogCount : s_submitSummaryLogCount;

    if ((burstBudget < 4 || submitSummaryPeriodicDue)
     && m_submitRejectStats.total > draws) {
      s_lastSubmitSummaryTime = submitSummaryNow;

      if (frameHasSceneGeometry) {
        ++s_submitSummaryWorldLogCount;
      }
      ++s_submitSummaryLogCount;
      Logger::info(str::format(
        "[D3D11Rtx] Submit summary: total=", m_submitRejectStats.total,
        " forceInjIdle=", m_submitRejectStats.forceInjectionIdle,
        " accepted=", m_submitRejectStats.accepted,
        " scene=", m_submitRejectStats.sceneAccepted,
        " realScene=", m_submitRejectStats.realSceneAccepted,
        " sceneCand=", m_submitRejectStats.sceneCandidates,
        " sigCulled=", m_submitRejectStats.significanceCulled,
        " overflow=", m_submitRejectStats.queueOverflow,
        " nonTriangle=", m_submitRejectStats.nonTriangleTopology,
        " noPS=", m_submitRejectStats.noPixelShader,
        " noRT=", m_submitRejectStats.noRenderTarget,
        " farPlaneSky=", m_submitRejectStats.farPlaneSkySkipped,
        " waterCompanion=", m_submitRejectStats.waterCompanionSkipped,
        " cameraCentred=", m_submitRejectStats.cameraCenteredSkipped,
        " screenSpaceVs=", m_submitRejectStats.screenSpaceVsSkipped,
        " orthoUi=", m_submitRejectStats.orthographicUi,
        " volumeBox=", m_submitRejectStats.volumeBoxSkipped,
        " otherCamNoSteer=", m_submitRejectStats.otherCameraNoSteer,
        " exactWorld=", m_submitRejectStats.exactWorldTransform,
        " exactRebased=", m_submitRejectStats.exactWorldRebased,
        " exactVertsSaved=", m_submitRejectStats.exactWorldVerticesSaved,
        " engine=", GetD3D11EngineProfile().name(),
        " dupPassSkipped=", m_submitRejectStats.duplicatePassSkipped,
        " lppGeomSkipped=", m_submitRejectStats.lightPrepassGeometrySkipped,
        " passKeyNone=", m_submitRejectStats.passKeyUnavailable,
        " noLayoutWorld=", m_submitRejectStats.noLayoutWorldCandidate,
        " vertexPulled=", m_submitRejectStats.vertexPulledAdmitted,
        " projectedDecals=", m_submitRejectStats.projectedDecals,
        " tessellated=", m_submitRejectStats.tessellatedAdmitted,
        " indirect=", m_submitRejectStats.indirectAdmitted,
        " geometryShader=", m_submitRejectStats.geometryShaderAdmitted,
        " lightVolumeColours=", m_submitRejectStats.lightVolumeColours,
        " indirectRejected=", m_submitRejectStats.indirectRejected,
        " decalUnresolved=", m_submitRejectStats.decalVolumeUnresolved,
        " mirroredView=", m_submitRejectStats.mirroredViewSkipped,
        " tiledLights=", m_submitRejectStats.tiledLightsImported,
        " lift2D=", m_submitRejectStats.lift2DAccepted, "/", m_submitRejectStats.lift2DCandidates,
        " lift2DCaptureFailed=", m_submitRejectStats.lift2DCaptureFailed,
        " secondaryView=", m_submitRejectStats.secondaryViewSkipped,
        " autoTerrain=", m_submitRejectStats.autoTerrain,
        " autoDecal=", m_submitRejectStats.autoDecals,
        " autoParticle=", m_submitRejectStats.autoParticles,
        " trivial=", m_submitRejectStats.trivialDraw,
        " fullscreen=", m_submitRejectStats.fullscreenPostFx,
        " noLayout=", m_submitRejectStats.noInputLayout,
        " noSemantics=", m_submitRejectStats.noSemantics,
        " noTexcoord=", m_submitRejectStats.noTexcoordLayout,
        " texgen=", m_submitRejectStats.texcoordGenerated,
        " texCapture=", m_submitRejectStats.texcoordCaptured,
        " posCapture=", m_submitRejectStats.positionCaptured,
        " posCaptureBudget=", m_submitRejectStats.positionCaptureBudgetRejected,
        " cameraRelativeUnsafe=", m_submitRejectStats.unsafeCameraRelativeSkipped,
        " noPosSem=", m_submitRejectStats.noPositionSemantic,
        " pos2D=", m_submitRejectStats.position2D,
        " noPosBuffer=", m_submitRejectStats.noPositionBuffer,
        " noIB=", m_submitRejectStats.noIndexBuffer,
        " composite=", m_submitRejectStats.compositeSkip,
        " ui=", m_submitRejectStats.screenSpaceUiSkip,
        " screenGarbage=", m_submitRejectStats.screenSpaceGarbageSkip,
        " hashFail=", m_submitRejectStats.geometryHashScheduleFailed,
        " posFmtRej=", m_submitRejectStats.positionFormatRejected,
        " posPoison=", m_submitRejectStats.poisonedPositions,
        " vtxRangeRej=", m_submitRejectStats.vertexRangeRejected,
        " idxRangeRej=", m_submitRejectStats.indexRangeRejected,
        " emulatorRaster=", m_submitRejectStats.postTransformEmulator,
        " helperMiB=", m_helperPoolBytes >> 20,
        " helperRetired=", m_helperRetired.size(),
        " helperFree=", m_helperFree.size(),
        " uvCacheMiB=", m_texcoordCaptureCacheBytes >> 20,
        " uvCacheEntries=", m_texcoordCaptureCache.size(),
        " posCacheMiB=", m_positionCaptureCacheBytes >> 20,
        " posCacheEntries=", m_positionCaptureCache.size(),
        " posCacheNew=", m_submitRejectStats.posCacheNew,
        " posCacheEvicted=", m_submitRejectStats.posCacheEvicted,
        " posCacheReset=", m_submitRejectStats.posCacheContractReset,
        " posCacheStaleReuse=", m_submitRejectStats.posCacheStaleReuse,
        " collapsedEye=", m_submitRejectStats.collapsedEyeGeometry,
        " rasterUi=", rasterUiSeen ? 1 : 0,
        " uiMidInject=", midFrameRtxInjected ? 1 : 0,
        " uiPassThrough=", forceRasterPassThrough ? 1 : 0));
    }

    ResetCommandListState();
    // Projection cache (m_projSlot, m_projOffset, m_projStage, m_columnMajor)
    // is NOT reset â€” the validation path at the start of ExtractTransforms
    // re-reads and re-scans only when the cached location becomes stale.
    // Keep the world-matrix cache for the same reason: modern games can have
    // thousands of draws per frame, and rescanning all cbuffers on every frame
    // creates unnecessary CPU pressure. The world-cache fast path still
    // validates the cached location every draw and falls back to a full rescan
    // automatically when the shader layout changes.
    ++m_axisDetectFrame;

    const bool allowResizeCameraCarryover = m_resizeTransitionFramesRemaining > 0;
    m_context->EmitCs([backbuffer, draws, acceptedDraws, sceneAcceptedDraws, realSceneAcceptedDraws, sceneCandidateDraws, trustedSceneAcceptedDraws, lift2DAcceptedDraws, allowResizeCameraCarryover, rasterUiSeen, midFrameRtxInjected, forceRasterPassThrough](DxvkContext* ctx) {
      RtxContext* rtx = static_cast<RtxContext*>(ctx);
      const uint32_t fid = rtx->getDevice()->getCurrentFrameId();
      bool camValid = rtx->getSceneManager().getCamera().isValid(fid);
      const bool allowSceneCameraCarryover = trustedSceneAcceptedDraws > 0 || acceptedDraws > 0;
      if (!camValid && (allowResizeCameraCarryover || allowSceneCameraCarryover)) {
        auto& cameraManager = rtx->getSceneManager().getCameraManager();
        auto& mainCamera = cameraManager.getCamera(CameraType::Main);
        const uint32_t lastUpdateFrame = mainCamera.getLastUpdateFrame();
        const bool lastCameraWasViewportFallback = cameraManager.mainCameraLastUpdateUsedViewportFallback();
        const uint32_t cameraGraceFrames = allowResizeCameraCarryover
          ? D3D11Rtx::kResizeCameraGraceFrames
          : D3D11Rtx::kSceneCameraGraceFrames;

        if (lastUpdateFrame != uint32_t(-1)
         && fid > lastUpdateFrame
         && fid - lastUpdateFrame <= cameraGraceFrames
         && (allowResizeCameraCarryover || allowSceneCameraCarryover || !lastCameraWasViewportFallback)) {
          cameraManager.processExternalCamera(
            CameraType::Main,
            Matrix4 { mainCamera.getWorldToView(false) },
            Matrix4 { mainCamera.getViewToProjection() });
          camValid = true;

          static uint32_t sResizeCameraCarryoverLogCount = 0;
          if (sResizeCameraCarryoverLogCount < 8) {
            ++sResizeCameraCarryoverLogCount;
            Logger::info(str::format(
              "[D3D11Rtx] Carrying forward last valid main camera across resize transition: frameId=",
              fid,
              " lastUpdate=",
              lastUpdateFrame));
          }
          if (!allowResizeCameraCarryover) {
            static uint32_t sSceneCameraCarryoverLogCount = 0;
            if (sSceneCameraCarryoverLogCount < 12) {
              ++sSceneCameraCarryoverLogCount;
              Logger::info(str::format(
                "[D3D11Rtx] Carrying forward last valid main camera across a short scene camera gap: frameId=",
                fid,
                " lastUpdate=",
                lastUpdateFrame,
                " realSceneDraws=",
                realSceneAcceptedDraws,
                " sceneDraws=",
                sceneAcceptedDraws,
                " trustedSceneDraws=",
                trustedSceneAcceptedDraws));
            }
          }
        }
      }
      if (fid < 32 || (fid < 512 && (fid % 64) == 0)) {
        Logger::info(str::format("[D3D11Rtx] CS endFrame: frameId=", fid,
          " draws=", draws, " camValid=", camValid ? 1 : 0));
      }

      // Only a draw that passed the scene classifier may start RTX.
      // Generic accepted draws include Bink/video quads and startup branding;
      // treating those as a scene replaced Bethesda/publisher presentation
      // frames with an empty composite before the first real camera existed.
      const bool previousSceneAvailable = rtx->getSceneManager().isPreviousFrameSceneAvailable();
      // Once a path-traced scene exists, scene candidates must stay on the RT
      // path even when a bounded capture/BLAS budget temporarily rejects all
      // of them. Falling back to the current raster backbuffer on those frames
      // produced the reported raster/path-trace overlap and flicker. Pure
      // screen-space startup/video frames have no scene candidates and still
      // pass through normally.
      const bool hasGameSceneDraws = trustedSceneAcceptedDraws > 0
        || (previousSceneAvailable && sceneCandidateDraws > 0);

      // DX11_V274_REQUIRE_REAL_VIEW_TO_INJECT: RtCamera::isValid() only checks
      // the camera was touched this frame - it is TRUE even when the view
      // matrix is identity (the "view=NO" state: a projection was found but
      // the view matrix was not). A correct projection with an identity view
      // puts the RT camera at the world origin looking at nothing, so the
      // whole path-traced frame renders BLACK - the exact "raytracing is
      // black" report, independent of albedo / lighting / denoiser. Require a
      // REAL (non-identity) view to inject; when the view cannot be resolved,
      // pass the frame through to the game's own raster so the screen is
      // never black. (View-matrix detection can fail per engine - notably
      // Unity; passthrough is the safe result until the layout is located.
      // Set DXVK_REMIX_MTXDUMP=1 to dump the cbuffer matrices and fix it.)
      const Matrix4d& camWorldToView = rtx->getSceneManager().getCamera().getWorldToView(false);
      double viewIdentityDeviation = 0.0;
      for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
          viewIdentityDeviation += std::abs(camWorldToView[r][c] - (r == c ? 1.0 : 0.0));
      const bool hasRealView = viewIdentityDeviation > 1.0e-4;
      const bool hasConfirmedCameraRelativeView =
        rtx->getSceneManager().getCameraManager().mainCameraLastUpdateUsedCameraRelativeView();
      // The 2D lift camera is identity by construction (layers are placed in
      // its view space), not an unresolved game camera.
      const bool hasRealCamera = camValid && (hasRealView || hasConfirmedCameraRelativeView
                                              || lift2DAcceptedDraws > 0);

      static uint32_t sNoRealViewLogCount = 0;
      if (camValid && !hasRealView && !hasConfirmedCameraRelativeView && lift2DAcceptedDraws == 0
       && sNoRealViewLogCount < 12) {
        ++sNoRealViewLogCount;
        Logger::info(str::format(
          "[D3D11Rtx] Camera has no real view matrix (identity view=origin camera) - passing frame "
          "through instead of injecting a black RT frame. frameId=", fid,
          " draws=", draws, " (set DXVK_REMIX_MTXDUMP=1 to capture matrices for view-detection fix)"));
      }

      const bool shouldInjectRtx = !forceRasterPassThrough
        && !midFrameRtxInjected
        && shouldInjectD3D11RtxFrame(
            backbuffer != nullptr,
            hasGameSceneDraws,
            hasRealCamera,
            previousSceneAvailable && hasGameSceneDraws);

      if (!shouldInjectRtx) {
        static uint32_t sStartupPassThroughLogCount = 0;
        if (sStartupPassThroughLogCount < 16) {
          ++sStartupPassThroughLogCount;
          Logger::info(str::format(
            "[D3D11Rtx] Passing through startup/loading frame without RTX injection: frameId=",
            fid,
            " draws=",
            draws,
            " accepted=",
            acceptedDraws,
            " scene=",
            sceneAcceptedDraws,
            " realScene=",
            realSceneAcceptedDraws,
            " trustedScene=",
            trustedSceneAcceptedDraws,
            " camValid=",
            camValid ? 1 : 0,
            " previousScene=",
            previousSceneAvailable ? 1 : 0,
            " rasterUi=",
            rasterUiSeen ? 1 : 0,
            " uiMidInject=",
            midFrameRtxInjected ? 1 : 0,
            " uiPassThrough=",
            forceRasterPassThrough ? 1 : 0,
            " backbuffer=",
            backbuffer != nullptr ? 1 : 0));
        }
      }

      rtx->endFrame(0, backbuffer, shouldInjectRtx);
    });

    if (m_resizeTransitionFramesRemaining > 0)
      --m_resizeTransitionFramesRemaining;
  }

  void D3D11Rtx::OnPresent(const Rc<DxvkImage>& swapchainImage, VkExtent2D remixViewportExtent) {
    ScopedCpuProfileZoneN("D3D11Rtx::OnPresent");
    // Same coherent policy as EndFrame — see UpdateTrackedExtents. The HWND
    // client rect is only an occlusion signal and must not drive the
    // renderer; only the present-image extent may trigger resize handling.
    UpdateTrackedExtents(swapchainImage, remixViewportExtent);

    // Temporary diagnostic: periodically pick the object at the screen centre.
    {
      static uint32_t s_presents = 0;
      ++s_presents;
      if ((s_presents % 120u) == 0u && m_lastOutputExtent.width > 0u) {
        // Cycle through the centre and four points around it, so an object
        // covering part of the view is found even when the centre is clear.
        static const float kPickPoints[6][2] = { { 0.5f, 0.5f }, { 0.75f, 0.3f }, { 0.25f, 0.3f }, { 0.75f, 0.7f }, { 0.25f, 0.7f }, { 0.39f, 0.45f } };
        const uint32_t pickIndex = (s_presents / 120u) % 6u;
        const Vector2i center { int32_t(float(m_lastOutputExtent.width) * kPickPoints[pickIndex][0]),
                                int32_t(float(m_lastOutputExtent.height) * kPickPoints[pickIndex][1]) };
        m_context->m_device->getCommon()->metaDebugView().ObjectPicking.request(
          center, center + Vector2i { 1, 1 },
          [pickIndex](std::vector<ObjectPickingValue>&& values, std::optional<XXH64_hash_t> textureHash) {
            Logger::info(str::format("[D3D11Rtx][center-probe] point=", pickIndex, " pick values=", values.size(),
              " value0=", values.empty() ? 0u : uint32_t(values[0]),
              " textureHash=0x", std::hex, textureHash.value_or(kEmptyHash)));
            s_centerPickHash.store(textureHash.value_or(kEmptyHash));
          });
      }
    }

    m_context->EmitCs([swapchainImage](DxvkContext* ctx) {
      RtxContext* rtx = static_cast<RtxContext*>(ctx);
      rtx->onPresent(swapchainImage);
    });
  }

  // Camera math shared with the DX12 / Vulkan front end
  // (d3d11_vk_capture.cpp), so both paths classify and factor matrices the
  // same way. Declared in d3d11_camera_math.h.
  int D3D11ClassifyPerspective(const Matrix4& m) {
    return classifyPerspective(m);
  }

  bool D3D11FactorViewProjection(const Matrix4& viewProjection, Matrix4& projection, Matrix4& view) {
    return factorViewProjection(viewProjection, projection, view);
  }

  Matrix4 D3D11CanonicalizeProjection(const Matrix4& projection, bool* flippedX, bool* flippedY) {
    return canonicalizeProjectionOrientation(projection, flippedX, flippedY);
  }

  Matrix4 D3D11ReadMatrix(const uint8_t* ptr, size_t offset, size_t size) {
    return readCbMatrix(ptr, offset, size);
  }

}
