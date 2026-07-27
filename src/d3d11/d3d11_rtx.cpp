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

#include "../dxvk/imgui/dxvk_imgui.h"
#include "../dxvk/rtx_render/rtx_context.h"
#include "../dxvk/rtx_render/rtx_options.h"
#include "../dxvk/rtx_render/rtx_camera.h"
#include "../dxvk/rtx_render/rtx_camera_manager.h"
#include "../dxvk/rtx_render/rtx_scene_manager.h"
#include "../dxvk/rtx_render/rtx_light_manager.h"
#include "../dxvk/rtx_render/rtx_matrix_helpers.h"
#include "../dxvk/rtx_render/rtx_option_manager.h"
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

    ViewSpaceCameraTracker s_pcViewSpaceCamera;
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

    // Same threading contract as the trackers above: one immediate context
    // driven from a single app thread.
    CameraRelativeWorldAnchor s_cameraRelativeWorldAnchor;

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
        return true;
      }

      const Vector4 p = clipToPosition * Vector4(clip[0], clip[1], clip[2], clip[3]);
      if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z)
       || !std::isfinite(p.w) || std::abs(p.w) < 1.0e-20f)
        return false;
      const float invW = 1.0f / p.w;
      position = Vector3(p.x * invW, p.y * invW, p.z * invW);
      return true;
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
      if (RtxOptions::forceInjection()) {
        const bool remixUiOpen = RtxOptions::showUI() != UIType::None;
        return remixUiOpen || hasValidCamera || previousSceneAvailable;
      }

      // First-time RTX injection needs a real scene camera. Otherwise loading
      // screens, menus, and weak viewport-fallback candidates can replace the
      // game frame with a black Remix composite. Previous scenes may only carry
      // when the current game frame still has a valid camera.
      return hasValidCamera && hasGameSceneDraws; // DX11_V124_CAMERA_ARTIFACT_STABILITY: do not inject fallback-only bootstrap/menu composite frames
    }

  }

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

  D3D11Rtx::D3D11Rtx(D3D11DeviceContext* pContext)
    : m_context(pContext) {}

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

  bool D3D11Rtx::OnDrawAuto() {
    BeginNativeRasterDrawRouting();
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
    const auto mapped = buffer->GetMappedSlice();
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(mapped.mapPtr);
    const size_t byteWidth = buffer->Desc()->ByteWidth;
    if (bytes == nullptr || argumentOffset > byteWidth
     || sizeof(D3D11_DRAW_INSTANCED_INDIRECT_ARGS) > byteWidth - argumentOffset) {
      static uint32_t sGpuIndirectLogCount = 0;
      if (sGpuIndirectLogCount++ < 8u) {
        Logger::info(
          "[D3D11Rtx] GPU-only DrawInstancedIndirect arguments are not CPU-visible at record time; "
          "leaving the raster draw untouched instead of submitting guessed RTX geometry");
      }
      return m_allowNativeRasterForCurrentDraw;
    }

    D3D11_DRAW_INSTANCED_INDIRECT_ARGS args = {};
    std::memcpy(&args, bytes + argumentOffset, sizeof(args));
    if (args.VertexCountPerInstance == 0u || args.InstanceCount == 0u)
      return m_allowNativeRasterForCurrentDraw;
    SubmitInstancedDraw(false, args.VertexCountPerInstance,
      args.StartVertexLocation, 0, args.InstanceCount, args.StartInstanceLocation);
    return m_allowNativeRasterForCurrentDraw;
  }

  bool D3D11Rtx::OnDrawIndexedInstancedIndirect(ID3D11Buffer* argumentBuffer, UINT argumentOffset) {
    BeginNativeRasterDrawRouting();
    if (argumentBuffer == nullptr)
      return m_allowNativeRasterForCurrentDraw;

    const auto* buffer = static_cast<D3D11Buffer*>(argumentBuffer);
    const auto mapped = buffer->GetMappedSlice();
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(mapped.mapPtr);
    const size_t byteWidth = buffer->Desc()->ByteWidth;
    if (bytes == nullptr || argumentOffset > byteWidth
     || sizeof(D3D11_DRAW_INDEXED_INSTANCED_INDIRECT_ARGS) > byteWidth - argumentOffset) {
      static uint32_t sGpuIndexedIndirectLogCount = 0;
      if (sGpuIndexedIndirectLogCount++ < 8u) {
        Logger::info(
          "[D3D11Rtx] GPU-only DrawIndexedInstancedIndirect arguments are not CPU-visible at record time; "
          "leaving the raster draw untouched instead of submitting guessed RTX geometry");
      }
      return m_allowNativeRasterForCurrentDraw;
    }

    D3D11_DRAW_INDEXED_INSTANCED_INDIRECT_ARGS args = {};
    std::memcpy(&args, bytes + argumentOffset, sizeof(args));
    if (args.IndexCountPerInstance == 0u || args.InstanceCount == 0u)
      return m_allowNativeRasterForCurrentDraw;
    SubmitInstancedDraw(true, args.IndexCountPerInstance,
      args.StartIndexLocation, args.BaseVertexLocation,
      args.InstanceCount, args.StartInstanceLocation);
    return m_allowNativeRasterForCurrentDraw;
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
    m_submitRejectStats = {};
    m_rasterUiSeenThisFrame = false;
    m_midFrameRtxInjected = false;
    m_forceRasterPassThroughThisFrame = false;
    m_allowNativeRasterForCurrentDraw = true;
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
    static constexpr uint32_t     kMaxCaptureVerticesPerDraw = 512u << 10;
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

    const uint32_t vertexCount = geo.vertexCount;
    if (vertexCount == 0 || vertexCount > kMaxCaptureVerticesPerDraw)
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

    if (m_context->m_state.gs.shader != nullptr
     || m_context->m_state.hs.shader != nullptr
     || m_context->m_state.ds.shader != nullptr)
      return false;

    for (const auto& soTarget : m_context->m_state.so.targets) {
      if (soTarget.buffer != nullptr)
        return false;
    }

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

    static constexpr uint32_t     kMaxCaptureVerticesPerDraw = 512u << 10;
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
    if (indexed && !triangleList)
      return false;

    const bool flattenIndexed = indexed && triangleList;
    if (requireIndexedFlatten && !flattenIndexed)
      return false;

    const uint32_t verticesPerInstance = flattenIndexed ? count : geo.vertexCount;
    const uint64_t totalVertexCount =
      uint64_t(verticesPerInstance) * uint64_t(replayInstanceCount);
    if (verticesPerInstance == 0 || totalVertexCount == 0
     || totalVertexCount > kMaxCaptureVerticesPerDraw)
      return false;
    const uint32_t vertexCount = uint32_t(totalVertexCount);

    if (m_context->m_state.vs.shader == nullptr)
      return false;
    const D3D11CommonShader* commonVs = m_context->m_state.vs.shader->GetCommonShader();
    if (commonVs == nullptr || !commonVs->HasPositionCaptureCandidate())
      return false;

    const bool capturesHomogeneousClip =
      commonVs->IsPositionCaptureHomogeneousClipSpace();
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
    const bool captureIncludesTexcoord = dcs.materialData.usesTexture()
      && hasPsSampledTexcoord
      && commonVs->ResolvePositionCaptureTexcoord(
           requestedTexcoordName, requestedTexcoordIndex,
           requestedTexcoordComponent,
           captureTexcoordName, captureTexcoordIndex,
           captureTexcoordComponent);
    const uint32_t captureStride = positionBytes
      + (captureIncludesTexcoord ? 8u : 0u);
    const VkDeviceSize captureBytes =
      VkDeviceSize(vertexCount) * captureStride;
    Matrix4 capturedClipToPosition;
    bool capturedClipUsesWDepth = false;
    if (capturesHomogeneousClip) {
      // Exact SV_Position is already the complete result of the game's vertex
      // transform.  Reconstruct the position directly in the replacement
      // camera's view-space world.  Do NOT also apply objectToView here: that
      // matrix is inferred independently and, when it is merely plausible
      // rather than the exact shader transform, inverse(O2V) turns ordinary
      // meshes into the giant camera-enclosing slabs seen in the RT G-buffer.
      // inverse(P) * clip followed by the homogeneous divide is sufficient and
      // is valid for every game whose raster projection was recovered.
      const Matrix4 inverseProjection = inverse(dcs.transformData.viewToProjection);
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
      for (uint32_t column = 0; column < 4; ++column) {
        for (uint32_t row = 0; row < 4; ++row) {
          if (!std::isfinite(capturedClipToPosition[column][row]))
            return false;
        }
      }
    }

    Rc<DxvkShader> captureGs = commonVs->GetPositionCaptureShader(
      captureTexcoordName, captureTexcoordIndex, captureTexcoordComponent);
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
    if (m_context->m_state.ia.inputLayout != nullptr) {
      for (const auto& semantic : m_context->m_state.ia.inputLayout->GetRtxSemantics()) {
        if (semantic.inputSlot < captureInputSlots.size()) {
          captureInputSlots[semantic.inputSlot] = true;
          capturePerInstanceSlots[semantic.inputSlot] |= semantic.perInstance;
        }
      }
    } else {
      captureInputSlots.fill(true);
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
      const bool hasNarrowTransformBinding = captureBinding != nullptr
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
        if (m_projStage != 0 || m_projSlot != slot || m_projOffset == SIZE_MAX)
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
              m_projStage == 0 && m_projSlot == dependency.slot
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

      // Dynamic IA buffers are host visible in DXVK. Hash precisely the vertex
      // interval replayed by transform feedback, so WRITE_NO_OVERWRITE changes
      // invalidate the capture while an unchanged renamed buffer does not create
      // a new BLAS every frame. Device-local inputs retain their logical/physical
      // allocation identity below and are immutable in the common static path.
      for (uint32_t slot = 0; slot < captureInputSlots.size() && readable; ++slot) {
        if (!captureInputSlots[slot])
          continue;
        const auto& vb = m_context->m_state.ia.vertexBuffers[slot];
        if (vb.buffer == nullptr || vb.stride == 0)
          continue;
        stateHash = XXH3_64bits_withSeed(&slot, sizeof(slot), stateHash);
        stateHash = XXH3_64bits_withSeed(&vb.stride, sizeof(vb.stride), stateHash);
        if (vb.buffer->GetMapMode() == D3D11_COMMON_BUFFER_MAP_MODE_NONE)
          continue;
        const DxvkBufferSliceHandle mapped = vb.buffer->GetMappedSlice();
        const uint8_t* ptr = reinterpret_cast<const uint8_t*>(mapped.mapPtr);
        const size_t bufferSize = vb.buffer->Desc()->ByteWidth;
        const bool perInstance = capturePerInstanceSlots[slot];
        const size_t elementIndex = perInstance
          ? size_t(replayFirstInstance)
          : size_t(firstVertex);
        const size_t begin = size_t(vb.offset) + elementIndex * vb.stride;
        const size_t byteLength = perInstance
          ? size_t(vb.stride) * size_t(replayInstanceCount)
          : size_t(verticesPerInstance) * vb.stride;
        if (ptr == nullptr || begin >= bufferSize || byteLength > bufferSize - begin) {
          readable = false;
          break;
        }
        stateHash = XXH3_64bits_withSeed(ptr + begin, byteLength, stateHash);
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
    bool captureMustReplayEveryFrame = capturedSkinnedPositions
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
        if (cb.buffer == nullptr)
          return false;

        const auto mapped = cb.buffer->GetMappedSlice();
        const uint8_t* ptr = reinterpret_cast<const uint8_t*>(mapped.mapPtr);
        const size_t bufferSize = cb.buffer->Desc()->ByteWidth;
        const size_t bindingBase = size_t(cb.constantOffset) * 16u;
        const size_t bindingEnd = cb.constantCount > 0
          ? std::min(bindingBase + size_t(cb.constantCount) * 16u, bufferSize)
          : bufferSize;
        if (ptr == nullptr || bindingBase >= bindingEnd)
          return false;

        for (uint32_t row = 0; row < 4; ++row) {
          const size_t offset = bindingBase
            + size_t(matrixBinding.constantRegisters[row]) * 16u;
          if (offset + 16u > bindingEnd || offset + 16u > bufferSize)
            return false;
          std::memcpy(matrix[row].data, ptr + offset, 16u);
        }
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
    mixOutputIdentity(usedShaderProvenCaptureTransform ? 0x50524f56454eull : 0x46414c4c4241434bull);
    mixOutputIdentity(capturedClipUsesWDepth ? 0x574445505448ull : 0x4d4154524958ull);
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
    if (capturesHomogeneousClip) {
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
      existing = m_positionCaptureCache.find(cacheKey);
    }

    PositionCaptureEntry& entry = existing != m_positionCaptureCache.end()
      ? existing->second
      : m_positionCaptureCache.emplace(cacheKey, PositionCaptureEntry()).first->second;
    if (entry.contractIdentity != captureContractIdentity) {
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
      && (!capturesHomogeneousClip || entry.hasCapturedClipToPosition);
    const bool transformStateMatches = capturesHomogeneousClip
      && hasHomogeneousTransformStateIdentity
      && entry.hasTransformStateIdentity
      && entry.transformStateIdentity == homogeneousTransformStateIdentity;
    const bool captureIsCurrent = haveUsableBuffer
      && entry.lastCapturedFrame != ~0u
      && (captureMustReplayEveryFrame
        ? entry.lastCapturedFrame == curFrame
        : (!capturesHomogeneousClip || transformStateMatches));
    entry.lastUsedFrame = curFrame;

    if (!captureIsCurrent) {
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

      const bool reuseStaleCapture = totalBudgetExhausted || classBudgetExhausted
        || m_positionCaptureBytesThisFrame + captureBytes > kMaxCaptureBytesPerFrame;
      if (reuseStaleCapture) {
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
        if (!haveReusableCapture) {
          // DX11_V298_PHASE1_STASH (adapted from FO4-Remix e00baae): a
          // budget-refused draw is PENDING, not absent. Keep the cache entry -
          // it already carries the contract identity, occurrence bookkeeping
          // and canonical transforms - so next frame's retry resumes against
          // warm state instead of re-emplacing from scratch every frame while
          // the cold-capture lane is saturated (level loads). The entry holds
          // no buffer, so it costs a map slot and nothing else; LRU eviction
          // reclaims it if the draw never returns.
          return false;
        }

        // A previous exact capture is safer than either dropping a mesh every
        // other frame or recapturing an unbounded dynamic scene.  Its matching
        // clip-to-position matrix remains stored in this entry, and the hash
        // below is tied to lastCapturedFrame so the scene manager reuses the
        // corresponding BLAS instead of interpreting stale bytes as new data.
        static uint32_t sStalePositionCaptureLogCount = 0;
        if (sStalePositionCaptureLogCount < 32u) {
          ++sStalePositionCaptureLogCount;
          Logger::info(str::format(
            "[D3D11Rtx][position-capture] reusing last exact capture under bounded replay lane",
            " frame=", curFrame,
            " capturedFrame=", entry.lastCapturedFrame,
            " vertices=", vertexCount,
            " drawId=", dcs.drawCallID));
        }
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

      // DX11_V303_CAPTURE_READBACK_ONLY_WAIT: the boundary does two separable
      // things - it flushes (bounding submission size, which is cheap and always
      // worth doing) and it BLOCKS the render thread until the GPU retires the
      // write (a full round trip, ~100ms under FIFO). The block is only required
      // because the camera-motion estimator maps this buffer on the CPU further
      // down; if nothing reads it back, GPU-side ordering already guarantees the
      // RT stream sees the finished write and stalling buys nothing.
      //
      // The estimator only runs on the camera-relative fallback, and only when a
      // real view has not been confirmed - a game whose view Remix can read has
      // no need to infer camera motion from captured geometry. So a confirmed
      // view now costs no readback and no stall.
      const bool captureWillBeReadBackOnCpu =
        capturesHomogeneousClip
        && !useWorldAnchoredHomogeneousCapture
        && RtxOptions::estimateViewSpaceCameraMotion()
        && !isKnownEmulatorHostProcess()
        && !m_viewConfirmed;

      const bool waitForCaptureWrite =
        forceCaptureSubmissionBoundary && captureWillBeReadBackOnCpu;
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
                         cForceSubmissionBoundary = forceCaptureSubmissionBoundary,
                         cWaitForCaptureWrite = waitForCaptureWrite](DxvkContext* ctx) {
        const DxvkInputAssemblyState pointIa = { VK_PRIMITIVE_TOPOLOGY_POINT_LIST, VK_FALSE, 0 };
        ctx->bindShader(VK_SHADER_STAGE_GEOMETRY_BIT, cGs);
        ctx->bindXfbBuffer(0, cBuf, DxvkBufferSlice());
        ctx->setInputAssemblyState(pointIa);
        if (cFlattenIndexed) {
          ctx->drawIndexed(cCount, cInstanceCount, cStartIndex, cBaseVertex, cFirstInstance);
        } else {
          ctx->draw(cCount, cInstanceCount, cFirst, cFirstInstance);
        }
        ctx->bindShader(VK_SHADER_STAGE_GEOMETRY_BIT, nullptr);
        ctx->bindXfbBuffer(0, DxvkBufferSlice(), DxvkBufferSlice());
        ctx->setInputAssemblyState(cRestoreIa);
        if (cForceSubmissionBoundary) {
          // This replay runs before RTX injection, so use the base DXVK flush:
          // it submits the captured buffers and starts a fully dirty command
          // list without invoking RtxContext's end-of-frame sky handling.
          ctx->DxvkContext::flushCommandList();

          // Only block when the CPU is about to map this allocation. The RT
          // stream consumes it on the GPU, where queue ordering already
          // guarantees the write has landed - stalling the render thread for
          // that case just serialises CPU and GPU for no benefit. The readback
          // path (camera-motion estimation) genuinely cannot proceed without
          // the data, so it still waits.
          if (cWaitForCaptureWrite) {
            ctx->getDevice()->waitForResource(cBuf.buffer(), DxvkAccess::Write);
          }
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
        entry.capturedVertexCount = vertexCount;
        entry.capturedStride = captureStride;
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

    if (capturesHomogeneousClip) {
      // Never combine clip coordinates from an earlier capture with the
      // current frame's inverse projection. That mismatch is a moving box/
      // overlap around the camera. Projection-only changes deliberately reuse
      // the paired matrix stored with the captured buffer.
      if (!entry.hasCapturedClipToPosition)
        return false;
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
    if (flattenIndexed || multiInstanceCapture) {
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
    if (capturesHomogeneousClip && hasHomogeneousTransformStateIdentity) {
      outputHash = XXH3_64bits_withSeed(
        &homogeneousTransformStateIdentity,
        sizeof(homogeneousTransformStateIdentity), outputHash);
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

        // DX11_V287_PC_VIEWSPACE_CAMERA: this is the camera-relative fallback
        // (no proven world matrix, unconfirmed view - the "camera position is
        // wrong / pinned at origin" case for PC games). Estimate the real
        // camera motion from the captured geometry itself, exactly like the
        // emulator path but with a SEPARATE tracker instance so PC and
        // emulator camera state never mix. objectToView stays untouched, so
        // raster alignment is identical; only the world anchoring changes.
        // Must stay in lockstep with captureWillBeReadBackOnCpu above: that flag
        // decides whether the render thread waited for the GPU write. Mapping
        // here without that wait would read a buffer the GPU may still be
        // filling, so the two conditions have to agree exactly - including the
        // m_viewConfirmed term, which skips the estimator entirely when a real
        // view is available and there is nothing to infer.
        if (RtxOptions::estimateViewSpaceCameraMotion()
         && !isKnownEmulatorHostProcess()
         && !m_viewConfirmed) {
          const float projectionScaleX = dcs.transformData.viewToProjection[0][0];
          const float projectionScaleY = dcs.transformData.viewToProjection[1][1];
          const bool perspectiveWDepth =
            dcs.transformData.viewToProjection[2][3] == 1.0f
            && std::isfinite(projectionScaleX) && projectionScaleX > 1.0e-5f
            && std::isfinite(projectionScaleY) && projectionScaleY > 1.0e-5f;
          const uint8_t* clipBase = reinterpret_cast<const uint8_t*>(
            capturedPositions.mapPtr(capturedPositions.offsetFromSlice()));
          const uint32_t clipStride = capturedPositions.stride();
          if (perspectiveWDepth && clipBase != nullptr && clipStride >= 16u
           && vertexCount >= 3u) {
            // Unproject a small sample back to view space: for a standard
            // perspective projection clip = (view.x*P00, view.y*P11, ...,
            // view.z), so view is recovered exactly from x/P00, y/P11, w.
            constexpr uint32_t kSampleCount = ViewSpaceCameraTracker::kPointsPerMesh;
            float viewSamples[kSampleCount * 3u] = {};
            const uint32_t step = std::max(1u, vertexCount / kSampleCount);
            uint32_t sampled = 0;
            bool samplesValid = true;
            for (uint32_t vertex = 0; vertex < vertexCount
                 && sampled < kSampleCount; vertex += step) {
              const float* clip = reinterpret_cast<const float*>(
                clipBase + size_t(vertex) * clipStride);
              const float w = clip[3];
              if (!std::isfinite(w) || std::abs(w) < 1.0e-6f
               || !std::isfinite(clip[0]) || !std::isfinite(clip[1])) {
                samplesValid = false;
                break;
              }
              viewSamples[sampled * 3u + 0u] = clip[0] / projectionScaleX;
              viewSamples[sampled * 3u + 1u] = clip[1] / projectionScaleY;
              viewSamples[sampled * 3u + 2u] = w;
              ++sampled;
            }
            if (samplesValid && sampled >= 3u) {
              // postVsCaptureIdentity is the camera-independent mesh identity
              // built above - the exact stable key the tracker needs.
              s_pcViewSpaceCamera.addMeshSample(
                geo.postVsCaptureIdentity, viewSamples, sampled);
            }
          }

          // DX11_V293_CONFIDENCE_GATE: before the first successful solve the
          // estimated pose is only the seed; applying it changed menu/intro
          // frames (no trackable meshes - e.g. Call of Duty front-ends) away
          // from the proven camera-relative fallback. Keep the original
          // fallback bit-for-bit until real camera motion has been solved.
          if (s_pcViewSpaceCamera.hasConfidentPose()) {
            dcs.transformData.worldToView = s_pcViewSpaceCamera.worldToView();
            dcs.transformData.objectToWorld = s_pcViewSpaceCamera.viewToWorld();
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
            || !s_pcViewSpaceCamera.hasConfidentPose()) {
        dcs.transformData.cameraRelativeView = true;
      }
      dcs.transformData.exactReplacementCamera = true;
      // The geometry and camera now form one exact replacement coordinate
      // system. Treat it as a real camera even when the projection was derived
      // from the viewport; the old fallback marker would make EndFrame reject
      // this valid path and leave optimized Unity scenes permanently raster-only.
      dcs.transformData.usedViewportFallbackProjection = false;

      // DX11_V309_CAMERA_RESOLVER (shadow): record which space this capture
      // actually landed in, for the comparison at the accept site. Recording
      // only - nothing above or below reads these.
      m_shadowCapturedPostTransform = true;
      m_shadowCaptureWorldAnchored = useWorldAnchoredHomogeneousCapture;
    } else {
      dcs.transformData.cameraRelativeView = false;
      dcs.transformData.exactReplacementCamera = false;

      m_shadowCapturedPostTransform = false;
      m_shadowCaptureWorldAnchored = false;
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
                                         uint64_t meshKey) {
    auto& requests = m_cameraAnchorRequests[m_cameraAnchorWriteIndex];
    if (requests.size() >= kCameraAnchorMaxSampleMeshes)
      return;

    if (entry.buffer == nullptr
     || !entry.hasCapturedViewRotationToWorld
     || !entry.hasCapturedClipToPosition
     || entry.capturedStride == 0u
     || entry.capturedVertexCount < kCameraAnchorSampleVertices)
      return;

    // The clip-W reconstruction is a synthetic replacement-camera space built
    // from a viewport-derived projection, not the game's view space, so the
    // game's view rotation does not belong to it and R^T*v would be meaningless.
    // Those captures are simply not sampled; a title that never recovers a real
    // projection keeps its existing camera-relative behavior.
    if (entry.capturedClipUsesWDepth)
      return;

    const VkDeviceSize sampleBytes =
      VkDeviceSize(kCameraAnchorSampleVertices) * entry.capturedStride;
    if (sampleBytes > VkDeviceSize(kCameraAnchorSampleSlotBytes)
     || sampleBytes > entry.capacity)
      return;

    Rc<DxvkBuffer>& staging = m_cameraAnchorStaging[m_cameraAnchorWriteIndex];
    if (staging == nullptr) {
      DxvkBufferCreateInfo info;
      info.size   = VkDeviceSize(kCameraAnchorMaxSampleMeshes)
                  * VkDeviceSize(kCameraAnchorSampleSlotBytes);
      info.usage  = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
      info.stages = VK_PIPELINE_STAGE_TRANSFER_BIT;
      info.access = VK_ACCESS_TRANSFER_WRITE_BIT;

      staging = m_context->m_device->createBuffer(
        info,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        DxvkMemoryStats::Category::RTXBuffer,
        "dx11 world-anchor camera samples");
      if (staging == nullptr)
        return;
    }

    // One fixed slot per request keeps the copies independent of each other,
    // so a request that is skipped never shifts the bytes of the ones already
    // queued this frame.
    const VkDeviceSize dstOffset =
      VkDeviceSize(requests.size()) * VkDeviceSize(kCameraAnchorSampleSlotBytes);
    m_context->EmitCs([cDst      = staging,
                       cDstOffset = dstOffset,
                       cSrc      = entry.buffer,
                       cBytes    = sampleBytes](DxvkContext* ctx) {
      ctx->copyBuffer(cDst, cDstOffset, cSrc, 0, cBytes);
    });

    CameraAnchorSampleRequest request;
    request.meshKey             = meshKey;
    request.viewRotationToWorld = entry.capturedViewRotationToWorld;
    request.clipToPosition      = entry.capturedClipToPosition;
    request.clipUsesWDepth      = entry.capturedClipUsesWDepth;
    request.vertexCount         = kCameraAnchorSampleVertices;
    request.stride              = entry.capturedStride;
    requests.push_back(request);
  }

  void D3D11Rtx::ConsumeCameraAnchorSamples() {
    // EndFrame is not guaranteed to run exactly once per presented frame.
    // Running this twice would flip the ping-pong back onto the batch queued a
    // moment ago and wait on copies that have not retired - reintroducing
    // precisely the CPU/GPU round trip the two-buffer scheme exists to avoid.
    const uint32_t currentFrame = m_context->m_device->getCurrentFrameId();
    if (m_cameraAnchorLastConsumedFrame == currentFrame)
      return;
    m_cameraAnchorLastConsumedFrame = currentFrame;

    // The batch read here is the one queued during the PREVIOUS frame: its
    // copies were submitted a whole frame and a present ago, so waiting on them
    // is free while still being a real guarantee rather than an assumption
    // about how far the CS thread has run. Reading this frame's batch would
    // mean blocking the render thread on the GPU instead.
    const uint32_t readIndex = m_cameraAnchorWriteIndex ^ 1u;
    auto& requests = m_cameraAnchorRequests[readIndex];
    const Rc<DxvkBuffer>& staging = m_cameraAnchorStaging[readIndex];

    // DX11_V319_ANCHOR_NEVER_BLOCKS: poll, never wait.
    //
    // This used to call waitForResource() on the previous frame's staging
    // buffer, on the assumption that a copy submitted a frame ago had certainly
    // retired. Under a path-traced frame the GPU is routinely more than a frame
    // behind, so that wait became a full CPU/GPU serialisation every frame.
    // Measured in Skyrim SE: frames pinned at 98-104ms (10 FPS) with the entire
    // cost inside a single 12-index draw and every phase timer at zero - the
    // signature of a sync, not of work. No other title showed it, and Skyrim is
    // the only one where this anchor path runs at all.
    //
    // Nothing here is worth a stall: the samples only refine a camera position
    // that is already correct to within a frame of motion. If the copy has not
    // retired, drop the batch and take the next one. Skipping frames is safe
    // because the estimator accumulates total displacement - a delta measured
    // across a two-frame gap is still the right total - and endFrame() keeps the
    // previous sample window when a frame yields nothing.
    const bool stagingReady = staging != nullptr
                           && !staging->isInUse(DxvkAccess::Write);

    if (!requests.empty() && stagingReady) {
      const uint8_t* const base =
        reinterpret_cast<const uint8_t*>(staging->mapPtr(0));

      for (size_t index = 0; base != nullptr && index < requests.size(); ++index) {
        const CameraAnchorSampleRequest& request = requests[index];
        const uint8_t* const slot =
          base + index * size_t(kCameraAnchorSampleSlotBytes);

        // Any consistent point on a rigid mesh works here - only the
        // frame-to-frame difference carries the camera translation - but
        // averaging a handful of vertices damps the float noise a single
        // unprojected position would contribute to the median.
        Vector3 offsetSum(0.0f, 0.0f, 0.0f);
        uint32_t sampled = 0;
        for (uint32_t vertex = 0; vertex < request.vertexCount; ++vertex) {
          const float* const clip = reinterpret_cast<const float*>(
            slot + size_t(vertex) * size_t(request.stride));

          Vector3 viewPosition;
          if (!unprojectCapturedClip(request.clipToPosition,
                                     request.clipUsesWDepth, clip, viewPosition))
            continue;

          // q = R^T * v is where this vertex sits relative to the camera in
          // WORLD orientation, which is the quantity whose frame-to-frame
          // difference is exactly the camera translation.
          const Vector4 offset =
            request.viewRotationToWorld * Vector4(viewPosition, 1.0f);
          offsetSum += Vector3(offset.x, offset.y, offset.z);
          ++sampled;
        }

        if (sampled == request.vertexCount) {
          s_cameraRelativeWorldAnchor.addSample(
            request.meshKey, offsetSum / float(sampled));
        }
      }
    }

    requests.clear();
    // Next frame writes into the batch that was just drained; the batch queued
    // during this frame becomes the one read at the end of the next.
    m_cameraAnchorWriteIndex = readIndex;

    s_cameraRelativeWorldAnchor.endFrame(kCameraAnchorMaxTranslationPerFrame);
  }

  void D3D11Rtx::SubmitInstancedDraw(bool indexed, UINT count, UINT start, INT base,
                                       UINT instanceCount, UINT startInstance) {
    if (instanceCount <= 1) {
      SubmitDraw(indexed, count, start, base);
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
      // Bound pathological vegetation draws while retaining every instance in
      // ordinary Unity/Unreal batches. Each replay is also capped at two million
      // emitted vertices, matching the capture allocator's hard safety limit.
      static constexpr UINT kMaxExactInstancesPerDraw = 4096u;
      static constexpr UINT kMaxCaptureVerticesPerBatch = 2u << 20;
      const UINT requestedLimit = std::max(1u, RtxOptions::maxInstanceSubmissions());
      const UINT selectedCount = std::min(
        instanceCount, std::min(requestedLimit, kMaxExactInstancesPerDraw));
      const UINT instancesPerBatch = std::max(1u, std::min(
        selectedCount, kMaxCaptureVerticesPerBatch / std::max(1u, count)));
      const UINT batchCount =
        (selectedCount + instancesPerBatch - 1u) / instancesPerBatch;

      static uint32_t sExactInstanceLogCount = 0;
      if (sExactInstanceLogCount++ < 12u) {
        Logger::info(str::format(
          "[D3D11Rtx] Exact shader-profile instancing: sourceInstances=",
          instanceCount, " selected=", selectedCount,
          " batches=", batchCount,
          " startInstance=", startInstance,
          " indexed=", indexed ? 1 : 0));
      }

      for (UINT offset = 0; offset < selectedCount; offset += instancesPerBatch) {
        const UINT batchInstances = std::min(instancesPerBatch, selectedCount - offset);
        SubmitDraw(indexed, count, start, base, nullptr,
          startInstance + offset, batchInstances, true);
      }
      return;
    }

    // Find per-instance float4 rows in the input layout that form a world matrix.
    // Engines encode this as 3 or 4 consecutive float4 elements with per-instance step rate,
    // using semantics like INSTANCETRANSFORM, WORLD, I, INST, or TEXCOORD at high indices.
    auto* layout = m_context->m_state.ia.inputLayout.ptr();
    if (!layout) {
      SubmitDraw(indexed, count, start, base);
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
    ScopedPhaseTimer phaseTimer(m_framePhaseExtractNs);

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
    if (projSlot == UINT32_MAX && singleSceneViewport) {
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

          bool locked = false;
          for (uint32_t vi = 0; vi < vpCount && !locked; ++vi) {
            const Matrix4& stored = vps[vi].m;
            const size_t off = vps[vi].offset;
              for (uint32_t ci = 0; ci < candCount && !locked; ++ci) {
                if (cands[ci].offset == off) continue;
                if (matricesNearlyEqual(cands[ci].view * rawProjNormalized, stored)
                 || matricesNearlyEqual(rawProjNormalized * cands[ci].view, stored)) {
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
                      cands[ci].columnMajor ? " [column-major]" : " [row-major]"));
                  }
                }
              }
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
    if (haveRawProjNormalized
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

        if (!yFlipOverrideEnabled) {
          m_yFlipVotes += projectionWasFlippedY ? 1 : -1;
          if (!m_yFlipSettled && std::abs(m_yFlipVotes) >= kVoteThreshold) {
            m_yFlipSettled = true;
            const bool yFlip = m_yFlipVotes > 0;
            RtCamera::correctProjectionYFlipObject().setDeferred(yFlip, RtxOptionLayer::getDerivedLayer());
          }
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
          m_yFlipVotes > 0 ? " Y-flipped" : "",
          m_zUpVotes > 0 ? " Z-up" : " Y-up",
          m_columnMajor ? " col-major" : " row-major",
          " (proj stage=", kStageNames[std::max(0, m_projStage)],
          " slot=", m_projSlot, " off=", m_projOffset, ")"));
      }
    }

    // --- CAMERA POSITION SMOOTHING ---
    // The view matrix encodes camera position in its translation row (row 3).
    // Floating-point rounding in cbuffer reads causes sub-pixel jitter between
    // draws/frames. Apply exponential moving average on the position to dampen
    // this without introducing visible lag. The rotation (upper 3x3) is left
    // untouched â€” rotation jitter is rare and smoothing it causes ghosting.
    //
    // D3D row-major view matrix layout:
    //   [R00 R01 R02  0]    pos = -R^T * t
    //   [R10 R11 R12  0]    where t = (V[3][0], V[3][1], V[3][2])
    //   [R20 R21 R22  0]
    //   [tx  ty  tz   1]
    if (!isIdentityExact(transforms.worldToView)) {
      const auto& V = transforms.worldToView;
      // Camera world position: pos = -R^T * t for view matrix V = [R | 0; t | 1]
      Vector3 t(V[3][0], V[3][1], V[3][2]);
      Vector3 camPos(
        -(V[0][0] * t.x + V[1][0] * t.y + V[2][0] * t.z),
        -(V[0][1] * t.x + V[1][1] * t.y + V[2][1] * t.z),
        -(V[0][2] * t.x + V[1][2] * t.y + V[2][2] * t.z));

      constexpr float kSmoothAlpha = 0.8f; // 0 = full smooth (laggy), 1 = no smooth (jittery)
      constexpr float kTeleportThreshold = 5.0f; // snap on large jumps (cutscene, teleport)

      if (m_hasPrevCamPos) {
        Vector3 delta = camPos - m_smoothedCamPos;
        float distSq = delta.x * delta.x + delta.y * delta.y + delta.z * delta.z;
        if (distSq < kTeleportThreshold * kTeleportThreshold) {
          m_smoothedCamPos = Vector3(
            m_smoothedCamPos.x + kSmoothAlpha * (camPos.x - m_smoothedCamPos.x),
            m_smoothedCamPos.y + kSmoothAlpha * (camPos.y - m_smoothedCamPos.y),
            m_smoothedCamPos.z + kSmoothAlpha * (camPos.z - m_smoothedCamPos.z));
        } else {
          m_smoothedCamPos = camPos;
        }
      } else {
        m_smoothedCamPos = camPos;
        m_hasPrevCamPos = true;
      }

      // Reconstruct translation row from smoothed position: t = -R * smoothPos
      transforms.worldToView[3][0] = -(V[0][0] * m_smoothedCamPos.x + V[0][1] * m_smoothedCamPos.y + V[0][2] * m_smoothedCamPos.z);
      transforms.worldToView[3][1] = -(V[1][0] * m_smoothedCamPos.x + V[1][1] * m_smoothedCamPos.y + V[1][2] * m_smoothedCamPos.z);
      transforms.worldToView[3][2] = -(V[2][0] * m_smoothedCamPos.x + V[2][1] * m_smoothedCamPos.y + V[2][2] * m_smoothedCamPos.z);
    }

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

                  const float score = scoreWorldCandidate(si, slot, off, candidateObjectToWorld) + 2.5f;
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
        if (bestDerivedWorldSlot != UINT32_MAX && bestDerivedWorldScore >= bestRawWorldScore) {
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
          if (cb.buffer == nullptr)
            return false;

          const auto mapped = cb.buffer->GetMappedSlice();
          const uint8_t* ptr = reinterpret_cast<const uint8_t*>(mapped.mapPtr);
          if (ptr == nullptr)
            return false;

          const size_t bufferSize = cb.buffer->Desc()->ByteWidth;
          const auto [bindingBase, bindingEnd] = cbRange(cb);
          for (uint32_t row = 0; row < 4; ++row) {
            const uint32_t shaderRegister = matrixBinding.constantRegisters[row];
            if (shaderRegister == UINT32_MAX) {
              // A three-row affine transform commonly ends with `mov w, 1`.
              // Preserve that exact homogeneous row without reading a
              // nonexistent fourth constant register.
              shaderMatrix[row] = Vector4(0.0f, 0.0f, 0.0f, 1.0f);
              continue;
            }

            const size_t offset = bindingBase + size_t(shaderRegister) * 16u;
            if (offset + 16u > bindingEnd || offset + 16u > bufferSize)
              return false;
            std::memcpy(shaderMatrix[row].data, ptr + offset, 16u);
          }
          return isFiniteMatrix(shaderMatrix);
        };

        bool matricesReadable = true;
        for (uint32_t i = 0; i < binding->matrixCount; ++i)
          matricesReadable &= readShaderMatrix(binding->matrices[i], shaderMatrices[i]);

        if (matricesReadable) {
          // DXBC dp4 constants are normally stored as row vectors while
          // Matrix4 stores columns. Test both representations. For a proven
          // two-stage shader chain, preserve application order A then B, but
          // also test the reversed multiplication needed by row-vector
          // conventions. The affine factor test below rejects bad variants.
          std::vector<Matrix4> shaderObjectToClipCandidates;
          if (binding->matrixCount == 1u) {
            shaderObjectToClipCandidates.push_back(shaderMatrices[0]);
            shaderObjectToClipCandidates.push_back(transpose(shaderMatrices[0]));
          } else {
            const std::array<Matrix4, 2> first = {
              shaderMatrices[0], transpose(shaderMatrices[0]) };
            const std::array<Matrix4, 2> second = {
              shaderMatrices[1], transpose(shaderMatrices[1]) };
            for (const Matrix4& a : first) {
              for (const Matrix4& b : second) {
                shaderObjectToClipCandidates.push_back(b * a);
                shaderObjectToClipCandidates.push_back(a * b);
              }
            }
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
              candidates.reserve(shaderObjectToClipCandidates.size() * 2u);
              for (const Matrix4& shaderObjectToClip : shaderObjectToClipCandidates) {
                candidates.push_back(inverseProjection * shaderObjectToClip);
                candidates.push_back(transpose(shaderObjectToClip * inverseProjection));
              }

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
                // Full DX11 replacement camera: the RT world is view space,
                // its camera is identity, and every draw carries the complete
                // shader-proven model-view transform. Do not mix game-specific
                // world/view layouts between shaders.
                transforms.objectToWorld = bestObjectToView;
                transforms.worldToView = Matrix4();
                transforms.cameraRelativeView = true;
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

      if (m_cameraAnchorViewTranslationFree
       && s_cameraRelativeWorldAnchor.hasPosition()) {
        const Vector3& cameraPosition = s_cameraRelativeWorldAnchor.position();

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

        static bool sCameraAnchorLogged = false;
        if (!sCameraAnchorLogged) {
          sCameraAnchorLogged = true;
          Logger::info(str::format(
            "[D3D11Rtx][world-anchor] camera-relative engine detected (view rotation "
            "with zero translation); anchoring the world with a camera position "
            "solved from captured geometry: pos=[",
            cameraPosition.x, ",", cameraPosition.y, ",", cameraPosition.z,
            "] meshes=", s_cameraRelativeWorldAnchor.lastMatchedMeshes()));
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

  void D3D11Rtx::FillMaterialData(
      LegacyMaterialData& mat,
      XXH64_hash_t primaryTextureHashOverride) const {
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
    const bool doLog = (s_logCount < 10);

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
      const bool hasProvenGeometryUvContract =
        commonPs != nullptr
        && commonVs != nullptr
        && commonPs->GetSampledTexcoordSemantic(
             slot, sampledSemanticName, sampledSemanticIndex,
             sampledSemanticComponent)
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
      const bool rejectMaterialCandidate = rejectUnprovenGeometryHash
        || (!clearAlbedo && (rejectTextureBrowserCandidate
        || likelyIntermediate
        || !albedoFormat
        || dataOrSceneFormat));

      int score = 0;
      if (colorBc)                  score += 14;  // Color BC = strong material signal.
      else if (bc)                  score -= 10;  // BC4/BC5/BC6 are masks/normals/HDR, not albedo.
      if (strongAlbedoFormat)       score += 8;
      else if (albedoFormat)        score += 2;
      if (hasMips)                  score += 5;   // Mipmapped = likely content
      if (!matchesRT)               score += 3;   // Different size from RT = likely content
      if (!isCurrentRT)             score += 2;   // Not actively rendering to it
      score += std::max(0, 16 - (int)slot);       // Prefer lower slots (albedo first)

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
        (fmt == VK_FORMAT_R8G8B8A8_UNORM
          || fmt == VK_FORMAT_R8G8_UNORM
          || fmt == VK_FORMAT_R8G8_SNORM
          || fmt == VK_FORMAT_R16G16_UNORM
          || fmt == VK_FORMAT_R16G16_SNORM
          || fmt == VK_FORMAT_R16G16_SFLOAT
          || fmt == static_cast<VkFormat>(65));

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
          hasProvenGeometryUvContract ? " [PROVEN-UV]" : " [NO-PROVEN-UV]",
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

    // A draw with no real game texture stays genuinely untextured. It remains
    // full path-traced geometry and uses the legacy material's constant/vertex
    // albedo path, but receives no synthetic image and therefore no invented
    // texture hash. Texture tagging and replacement remain exclusively tied to
    // actual game textures or explicit user-authored material data.

    if (doLog) {
      Logger::info(str::format("[D3D11Rtx] FillMaterialData draw #", s_logCount,
        " picked ", textureID, " of ", pickCount, " candidate(s)"));
      // Count every logged draw, not just draws that picked a texture.
      // Previously the counter only advanced when pickCount > 0, so in
      // deferred engines where most draws reject all candidates the 10-draw
      // cap never engaged and the per-draw candidate logging ran forever --
      // tens of thousands of str::format + log writes on the draw hot path
      // (a measurable CPU bottleneck and 30k+ line logs).
      ++s_logCount;
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
    // Time the whole submission path for this draw. Scoped so every early-out
    // below is still measured - a draw that is expensive to *reject* costs the
    // frame just as much as one that is expensive to accept, and the rejection
    // paths are where the surprises tend to be.
    const auto drawCpuStart = std::chrono::high_resolution_clock::now();
    const uint32_t timedDrawId = m_drawCallID;
    const auto drawCpuScopeExit = [&]() {
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
      const std::function<void()>& fn;
      ~ScopeGuard() { fn(); }
    } drawCpuGuard { drawCpuScopeExit };

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
    if (d3dTopology != D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST &&
        d3dTopology != D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP) {
      ++m_submitRejectStats.nonTriangleTopology;
      return;
    }

    // Skip depth-only passes: no pixel shader means depth prepass or shadow map.
    // Most engines draw opaque geometry twice â€” once for depth prepass (PS == null)
    // and once for the color pass (PS != null) with the same vertices.
    if (m_context->m_state.ps.shader == nullptr) {
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
      ++m_submitRejectStats.depthOnlySkipped;
      return;
    }

    // Skip trivially small draws (< 3 elements = 0 triangles).
    if (count < 3) {
      ++m_submitRejectStats.trivialDraw;
      return;
    }

    // Read actual depth/stencil state from the OM â€” don't hardcode.
    bool zEnable = true;
    bool zWriteEnable = true;
    bool stencilEnabled = false;
    D3D11DepthStencilState* dsState = m_context->m_state.om.dsState;
    if (dsState) {
      D3D11_DEPTH_STENCIL_DESC dsDesc;
      dsState->GetDesc(&dsDesc);
      zEnable         = dsDesc.DepthEnable != FALSE;
      zWriteEnable    = dsDesc.DepthWriteMask != D3D11_DEPTH_WRITE_MASK_ZERO;
      stencilEnabled  = dsDesc.StencilEnable != FALSE;
    }

    // Skip fullscreen quad / postprocess draws: depth disabled + 6 or fewer
    // elements (a fullscreen triangle or quad) + no depth write.
    // Only skip if BOTH depth test and write are off â€” some engines do
    // "depth off, write on" for sky or "depth on, write off" for decals.
    if (!zEnable && !zWriteEnable && count <= 6) {
      ++m_submitRejectStats.fullscreenPostFx;
      return;
    }

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
      return;
    }

    const auto& semantics = layout->GetRtxSemantics();

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

    if (semantics.empty()) {
      ++m_submitRejectStats.noSemantics;
      return;
    }

    const D3D11RtxSemantic* posSem = selectBestSemantic(semantics, scorePositionSemantic);
    const D3D11RtxSemantic* tcSem  = selectBestSemantic(semantics, scoreTexcoordSemantic, { posSem });
    if (!tcSem)
      tcSem = selectBestSemantic(semantics, scoreTexcoordFallbackSemantic, { posSem });

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

    if (!posSem) {
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
    if (posSem->format == VK_FORMAT_R32G32_SFLOAT && !zEnable) {
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

    RasterBuffer posBuffer = makeVertexBuffer(posSem);
    if (!posBuffer.defined()) {
      ++m_submitRejectStats.noPositionBuffer;
      return;
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
      const bool wideColor = (cf == VK_FORMAT_R32G32B32A32_SFLOAT
                           || cf == VK_FORMAT_R16G16B16A16_UNORM
                           || cf == VK_FORMAT_R16G16B16A16_SFLOAT)
                          && semanticNameStartsWith(*colSem, "COLOR");
      if (packedByteColor || wideColor) {
        colBuffer = makeVertexBuffer(colSem);
      }
    }

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
    const uint32_t positionBytes = positionElementBytes(posBuffer.vertexFormat());
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
    if (!indexed) {
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
        bool invalidIndex = false;
        if (idxBuffer.indexType() == VK_INDEX_TYPE_UINT32) {
          const uint32_t* ip = static_cast<const uint32_t*>(idxScan);
          for (uint32_t i = 0; i < scanCount; ++i) {
            const uint32_t index = ip[i];
            if (primitiveRestart && index == UINT32_MAX)
              continue;
            if (index >= maxVBVertices) {
              invalidIndex = true;
              break;
            }
            maxIndexPlusOne = std::max(maxIndexPlusOne, index + 1u);
          }
        } else {
          const uint16_t* ip = static_cast<const uint16_t*>(idxScan);
          for (uint32_t i = 0; i < scanCount; ++i) {
            const uint32_t index = ip[i];
            if (primitiveRestart && index == UINT16_MAX)
              continue;
            if (index >= maxVBVertices) {
              invalidIndex = true;
              break;
            }
            maxIndexPlusOne = std::max(maxIndexPlusOne, index + 1u);
          }
        }
        if (invalidIndex || maxIndexPlusOne == 0) {
          ++m_submitRejectStats.indexRangeRejected;
          return;
        }
      }
      if (maxIndexPlusOne > 0) {
        // Exact maximum known - size the vertex range to it.
        indexRangeExact = true;
        drawVertexCount = std::min(maxIndexPlusOne, maxVBVertices);
      } else {
        // Index data not CPU-readable (or draw too large to scan). Index values
        // may reference ANY vertex in the remaining buffer: many engines bake
        // absolute offsets into the indices instead of using BaseVertexLocation,
        // so clamping to the index count would leave indices pointing past the
        // vertex range Remix copies - out-of-bounds fetches that render as
        // exploded triangle spikes. Cover the whole remaining buffer, bounded to
        // keep the interleave allocation sane; beyond that the draw cannot be
        // made safe, so drop it rather than corrupt the scene.
        static constexpr uint32_t kMaxUnknownRangeVertices = 4u << 20;
        if (maxVBVertices > kMaxUnknownRangeVertices) {
          ++m_submitRejectStats.vertexRangeRejected;
          return;
        }
        usedWholeVertexBufferFallback = true;
        drawVertexCount = maxVBVertices;
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
            colFmt == VK_FORMAT_R8G8B8A8_UNORM      ? 4u
          : colFmt == VK_FORMAT_R16G16B16A16_UNORM  ? 8u
          : colFmt == VK_FORMAT_R16G16B16A16_SFLOAT ? 8u
          : colFmt == VK_FORMAT_R32G32B32A32_SFLOAT ? 16u
          : 0u;

        bool colorConverted = false;
        const uint8_t* srcBase = reinterpret_cast<const uint8_t*>(
          geo.color0Buffer.mapPtr(geo.color0Buffer.offsetFromSlice()));
        const uint32_t srcStride = geo.color0Buffer.stride();
        const uint32_t srcSliceOff = geo.color0Buffer.offsetFromSlice();
        const size_t srcLen = geo.color0Buffer.length() > srcSliceOff
          ? geo.color0Buffer.length() - srcSliceOff
          : 0;

        if (colElemBytes != 0 && srcBase != nullptr && srcStride > 0
         && drawVertexCount > 0 && drawVertexCount <= kMaxFormatConvertVertices) {
          const VkDeviceSize dstSize = VkDeviceSize(drawVertexCount) * 4u;
          Rc<DxvkBuffer> dst = AcquireHostVisibleHelperBuffer(dstSize, "d3d11 rtx color0 to bgra");
          uint8_t* out = dst != nullptr ? reinterpret_cast<uint8_t*>(dst->mapPtr(0)) : nullptr;
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
    if (RtxOptions::needsMeshBoundingBox() && posBuffer.stride() > 0 && drawVertexCount > 0) {
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
        for (uint32_t i = 0; i < sampleCount; ++i) {
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
      if (!dcs.transformData.usedViewportFallbackProjection)
        return false;

      if (!isIdentityExact(dcs.transformData.objectToWorld)
       || !isIdentityExact(dcs.transformData.worldToView))
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
        return likelyFullscreenPrimitive && likelyScreenSpaceDepthState;

      return rtSizedCount == candidateCount && contentLikeCount == 0;
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

    if (allowViewportFallbackScreenSpaceReject && isLikelyScreenSpaceCompositePass()) {
      ++m_submitRejectStats.compositeSkip;
      static uint32_t sScreenSpaceCompositeSkipLogCount = 0;
      if (sScreenSpaceCompositeSkipLogCount < 8) {
          ++sScreenSpaceCompositeSkipLogCount;
        Logger::info(str::format(
          "[D3D11Rtx] Skipping screen-space composite pass: viewport fallback camera + identity transforms + RT-sized/empty inputs (count=",
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
      !renderDocAttached && isLikelyScreenSpaceUiPass();
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

    // The DX11 bridge builds its LegacyMaterialData directly from the bound
    // SRVs, unlike the original D3D11 path.  Category evaluation therefore has
    // to happen after FillMaterialData has selected the final color texture.
    // Without this call the Remix UI could persist a selected texture hash,
    // but every submitted DrawCallState kept an empty category bitset: terrain,
    // sky, ignore, player-model, decal, particle, and UI tagging were all
    // observable no-ops.
    dcs.setupCategoriesForTexture();

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

    const uint32_t tinyFallbackPrimitiveCount = dcs.geometryData.calculatePrimitiveCount();
    const bool tinyFallbackHasSceneDepthSignal =
      dcs.zEnable && (dcs.zWriteEnable || dcs.maxZ >= 0.99f);
    const bool tinyFallbackMicroRaster =
      !renderDocAttached &&
      dcs.transformData.usedViewportFallbackProjection &&
      count <= 6u &&
      tinyFallbackPrimitiveCount <= 2u &&
      !tinyFallbackHasSceneDepthSignal;

    if (tinyFallbackMicroRaster && !likelyScreenSpaceUiPass) {
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

    if (!renderDocAttached && isLikelyTransientScreenSpacePass()) {
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
      } else if (TryCaptureTexcoordsViaStreamOut(dcs, geo, indexed, count, start, base)) {
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

    ++m_submitRejectStats.accepted;

    // DX11_V309_CAMERA_RESOLVER - STEP 1, SHADOW MODE. Changes no behaviour.
    //
    // Ask the new single-authority resolver which space this draw's vertices are
    // in, and compare that with what the legacy cameraRelativeView bool amounts
    // to. Only DISAGREEMENTS are logged: those are the draws where the 13
    // scattered writes to that bool produced a different answer than the ranked
    // evidence does, and one of them is the geometry that encloses the eye.
    //
    // Step 2 switches the submit path onto this resolver once the disagreement
    // list has been reviewed. Nothing below reads resolvedSpace today.
    {
      CameraEvidence evidence;
      evidence.objectToWorld = dcs.transformData.objectToWorld;
      evidence.worldToView = dcs.transformData.worldToView;
      evidence.viewToProjection = dcs.transformData.viewToProjection;
      evidence.capturedPostTransform = m_shadowCapturedPostTransform;
      evidence.worldAnchoredCapture = m_shadowCaptureWorldAnchored;
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

        if (RtxOptions::dx11UseResolvedTransformSpace()
         && resolved.confidence >= kMinConfidenceToOverride
         && (resolved.space == TransformSpace::View
          || resolved.space == TransformSpace::World)) {
          dcs.transformData.cameraRelativeView =
            (resolved.space == TransformSpace::View);
        }

        static uint32_t sCameraResolverDisagreeLogCount = 0;
        if (sCameraResolverDisagreeLogCount < 64u) {
          ++sCameraResolverDisagreeLogCount;
          Logger::warn(str::format(
            "[D3D11Rtx][camera-resolver] ",
            RtxOptions::dx11UseResolvedTransformSpace() ? "APPLIED" : "DISAGREE(log-only)",
            " resolved=", transformSpaceName(resolved.space),
            " legacy=", transformSpaceName(legacySpace),
            " confidence=", resolved.confidence,
            " reason='", resolved.reason, "'",
            " drawId=", dcs.drawCallID,
            " indices=", dcs.geometryData.indexCount,
            " cameraRelative=", dcs.transformData.cameraRelativeView ? 1 : 0,
            " viewConfirmed=", m_viewConfirmed ? 1 : 0,
            " viewIsCamRel=", m_viewCameraRelative ? 1 : 0,
            " postTransform=", m_shadowCapturedPostTransform ? 1 : 0,
            " worldAnchored=", m_shadowCaptureWorldAnchored ? 1 : 0,
            " identityObjToWorld=", isIdentityExact(dcs.transformData.objectToWorld) ? 1 : 0,
            " identityWorldToView=", isIdentityExact(dcs.transformData.worldToView) ? 1 : 0,
            " zWrite=", dcs.zWriteEnable ? 1 : 0,
            " blend=", shadowBlendEnabled ? 1 : 0));
        }
      }
    }

    // DX11_V300_CAMERA_OBSTRUCTION_LOG: identify geometry that ends up sitting on
    // the camera. Transform the object-space bounding box into VIEW space - a mesh
    // that is genuinely elsewhere in the world lands away from the view origin,
    // while anything pinned to the viewpoint (bad anchoring, a mis-scoped
    // transform, camera-relative capture) brackets the origin and fills the
    // screen no matter where the player looks. Report the draws that enclose the
    // eye, plus what they are, so the obstruction can be named instead of guessed.
    if (RtxOptions::logCameraObstruction()) {
      // Below this per-axis size a mesh cannot meaningfully block the view; it is
      // a flat quad or a degenerate sliver. Deliberately generous - real hits are
      // orders of magnitude larger.
      constexpr float kMinObstructionExtent = 0.01f;

      const AxisAlignedBoundingBox& objectBox = dcs.geometryData.boundingBox;

      // The bounds are produced asynchronously (see futureBoundingBox); until that
      // resolves the box is still its empty sentinel, min=+FLT_MAX / max=-FLT_MAX.
      // Transforming that yields infinities which trivially bracket the origin, so
      // every draw would look like an obstruction. Only judge a resolved box.
      const bool boundsResolved =
        objectBox.minPos.x <= objectBox.maxPos.x &&
        objectBox.minPos.y <= objectBox.maxPos.y &&
        objectBox.minPos.z <= objectBox.maxPos.z;

      // Screen-space and HUD geometry is FLAT - zero extent on one axis - and is
      // authored around the origin with no translation, so it trivially brackets
      // the view origin and floods this report without ever being an obstruction.
      // A mesh that actually blocks the camera has volume. Requiring extent on
      // all three axes separates the two cleanly: a real hit measured hundreds of
      // units per side, while the false positives are sub-unit flat quads.
      const bool geometryIsVolumetric =
        (objectBox.maxPos.x - objectBox.minPos.x) > kMinObstructionExtent &&
        (objectBox.maxPos.y - objectBox.minPos.y) > kMinObstructionExtent &&
        (objectBox.maxPos.z - objectBox.minPos.z) > kMinObstructionExtent;

      const Matrix4 objectToView = dcs.transformData.worldToView * dcs.transformData.objectToWorld;

      if (boundsResolved && geometryIsVolumetric) {

      // Project all eight corners; an arbitrary transform can rotate the box, so
      // transforming only min/max would understate the extents.
      Vector3 viewMin( FLT_MAX,  FLT_MAX,  FLT_MAX);
      Vector3 viewMax(-FLT_MAX, -FLT_MAX, -FLT_MAX);
      for (uint32_t corner = 0; corner < 8; ++corner) {
        const Vector3 objectCorner(
          (corner & 1) ? objectBox.maxPos.x : objectBox.minPos.x,
          (corner & 2) ? objectBox.maxPos.y : objectBox.minPos.y,
          (corner & 4) ? objectBox.maxPos.z : objectBox.minPos.z);
        const Vector4 viewCorner = objectToView * Vector4(objectCorner, 1.0f);
        viewMin = min(viewMin, viewCorner.xyz());
        viewMax = max(viewMax, viewCorner.xyz());
      }

      // "On the camera" = the box brackets the view origin on every axis.
      const bool enclosesEye =
        viewMin.x <= 0.0f && viewMax.x >= 0.0f &&
        viewMin.y <= 0.0f && viewMax.y >= 0.0f &&
        viewMin.z <= 0.0f && viewMax.z >= 0.0f;

      if (enclosesEye) {
        static uint32_t sObstructionLogCount = 0;
        if (sObstructionLogCount < RtxOptions::logCameraObstructionMaxEntries()) {
          ++sObstructionLogCount;
          Logger::warn(str::format(
            "[D3D11Rtx][cam-obstruction] geometry encloses the eye: drawId=", m_drawCallID,
            " indices=", count,
            " prims=", dcs.geometryData.calculatePrimitiveCount(),
            " textureHash=0x", std::hex, dcs.materialData.getHash(), std::dec,
            " cameraRelative=", dcs.transformData.cameraRelativeView ? 1 : 0,
            // If worldToView is identity then "world" space IS camera space, so
            // an object placed with no translation lands exactly on the eye.
            // Reporting it alongside cameraRelative exposes the mismatch where
            // the view is camera-relative but the draw was not flagged as such.
            " identityView=", isIdentityExact(dcs.transformData.worldToView) ? 1 : 0,
            " objToWorldT=[", dcs.transformData.objectToWorld[3][0], ",",
                              dcs.transformData.objectToWorld[3][1], ",",
                              dcs.transformData.objectToWorld[3][2], "]",
            " worldToViewT=[", dcs.transformData.worldToView[3][0], ",",
                               dcs.transformData.worldToView[3][1], ",",
                               dcs.transformData.worldToView[3][2], "]",
            " texgen=", static_cast<uint32_t>(dcs.transformData.texgenMode),
            " viewBox=[", viewMin.x, ",", viewMin.y, ",", viewMin.z,
            "]..[", viewMax.x, ",", viewMax.y, ",", viewMax.z, "]",
            " objectBox=[", objectBox.minPos.x, ",", objectBox.minPos.y, ",", objectBox.minPos.z,
            "]..[", objectBox.maxPos.x, ",", objectBox.maxPos.y, ",", objectBox.maxPos.z, "]"));
        }

        // DX11_V314_DROP_COLLAPSED_EYE_GEOMETRY: actually remove the mesh that
        // sits on the eye, rather than only reporting it.
        //
        // Enclosing the eye is NOT on its own a defect - stand inside a room,
        // a cave or a water volume and that mesh legitimately brackets the
        // camera on all three axes. Culling on "enclosesEye" alone would delete
        // interiors. So this requires the DEGENERATE COLLAPSE signature seen in
        // every field log of the black box:
        //
        //   objToWorldT=[0,0,0]   worldToViewT=[-0,-0,-0]
        //
        // both the object AND the camera sitting exactly at the world origin.
        // A real interior has a non-zero object placement, or a camera that is
        // somewhere other than the origin. When both translations are zero the
        // geometry has not been placed at all - it has collapsed onto the
        // viewpoint - and it occludes the scene that renders correctly behind it.
        //
        // This is a SAFETY NET, not the cure. The cure is submitting the draw in
        // the right coordinate space (rtx.dx11UseResolvedTransformSpace); if that
        // works, the collapse stops happening and this never fires.
        // rtx.dropCollapsedEyeGeometry turns it off without a rebuild.
        if (RtxOptions::dropCollapsedEyeGeometry()) {
          auto translationIsOrigin = [](const Matrix4& m) {
            constexpr float kOriginEpsilon = 1.0e-4f;
            return std::abs(m[3][0]) < kOriginEpsilon
                && std::abs(m[3][1]) < kOriginEpsilon
                && std::abs(m[3][2]) < kOriginEpsilon;
          };

          // DX11_V319_DROP_EYE_ENCLOSING_QUAD: the collapse test above requires
          // BOTH translations to be exactly zero, which only catches geometry
          // that was never placed at all. A screen-space quad that carries a
          // real transform slips straight through it.
          //
          // Field evidence (SpongeBob: Battle for Bikini Bottom - Rehydrated):
          // 31 draws of "indices=6 prims=2 textureHash=0x0" bracketing the view
          // origin on all three axes - a single untextured two-triangle quad
          // wrapped around the player. That is the "weird box around SpongeBob".
          // Its translations are non-zero, so nothing dropped it.
          //
          // Two primitives cannot bound a volume. Any mesh that genuinely
          // encloses the camera - a room, a cave, a water volume - is made of
          // many more triangles than that, so requiring a degenerate primitive
          // count keeps real interiors safe while removing the flat quad that
          // is really a post-process/UI blit misrouted into the world. The
          // untextured test is the same signal the raster-overlay rule already
          // trusts for solid-colour batches.
          constexpr uint32_t kMaxEyeEnclosingQuadPrimitives = 2u;
          const bool degenerateEyeEnclosingQuad =
               dcs.geometryData.calculatePrimitiveCount() <= kMaxEyeEnclosingQuadPrimitives
            && !dcs.materialData.usesTexture();

          if (degenerateEyeEnclosingQuad) {
            ++m_submitRejectStats.collapsedEyeGeometry;

            static uint32_t sEyeQuadCullLogCount = 0;
            if (sEyeQuadCullLogCount < 16u) {
              ++sEyeQuadCullLogCount;
              Logger::warn(str::format(
                "[D3D11Rtx][cam-obstruction] DROPPED eye-enclosing untextured quad: drawId=", m_drawCallID,
                " indices=", count,
                " prims=", dcs.geometryData.calculatePrimitiveCount(),
                " (two triangles cannot bound a volume, so this is a screen-space blit"
                " wrapped around the camera rather than world geometry)"));
            }
            return;
          }

          if (translationIsOrigin(dcs.transformData.objectToWorld)
           && translationIsOrigin(dcs.transformData.worldToView)) {
            ++m_submitRejectStats.collapsedEyeGeometry;

            static uint32_t sCollapsedEyeCullLogCount = 0;
            if (sCollapsedEyeCullLogCount < 16u) {
              ++sCollapsedEyeCullLogCount;
              Logger::warn(str::format(
                "[D3D11Rtx][cam-obstruction] DROPPED collapsed-on-eye geometry: drawId=", m_drawCallID,
                " indices=", count,
                " textureHash=0x", std::hex, dcs.materialData.getHash(), std::dec,
                " (object and camera translations are both zero, so this mesh was never placed;"
                " it occludes the scene rendering correctly behind it)"));
            }
            return;
          }
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

          if (lightVolumeArmed) {
          const Vector3 colour = RtxOptions::deferredLightVolumeColor()
                               * RtxOptions::deferredLightVolumeIntensity();

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
      const bool isSceneCandidate = hasSceneDepthSignal
        && primitiveCount >= 1u
        && ((hasRealProjection && hasViewOrStrongProjection) || strongViewportFallbackScene);

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
        if (rasterOverlayOrHelper) {
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
        if (hasRealProjection && hasViewOrStrongProjection) {
          ++m_submitRejectStats.realSceneAccepted;
          m_hasSeenRealSceneProjection = true;
          m_lastRealCameraFrameId = m_context->m_device->getCurrentFrameId();
        }
      }
    }

    // Recover the exact position the rasterizer saw only after every
    // screen-space, missing-shader, and significance rejection has completed.
    // Replaying rejected draws consumed the old capture budget before real
    // scene geometry and created needless capture-buffer/BLAS pressure.
    const uint32_t captureBudgetRejectsBefore =
      m_submitRejectStats.positionCaptureBudgetRejected;
    const bool capturedExactPositions = !pcsx2PostTransformDraw
      && TryCapturePositionsViaStreamOut(
        dcs, geo, indexed, count, start, base,
        instanceTransform != nullptr, replayFirstInstance, replayInstanceCount,
        usedWholeVertexBufferFallback);
    const bool exactCaptureBudgetRejected =
      m_submitRejectStats.positionCaptureBudgetRejected != captureBudgetRejectsBefore;
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
             || (dcs.transformData.cameraRelativeView
              && dcs.usesVertexShader))) {
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
      ++m_submitRejectStats.unsafeCameraRelativeSkipped;
      static uint32_t sUnsafeCameraRelativeSkipLogCount = 0;
      if (sUnsafeCameraRelativeSkipLogCount < 32) {
        ++sUnsafeCameraRelativeSkipLogCount;
        Logger::warn(str::format(
          "[D3D11Rtx][position-capture] skipped unsafe uncaptured draw: reason=",
          requireExactPositionCapture && exactCaptureBudgetRejected ? "budget"
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

    if (deferTexcoordRecoveryToPositionCapture
     && !geo.texcoordBuffer.defined()) {
      // Position capture may be unavailable for a safe non-camera-relative
      // draw (budget/capability/profile). Retain coverage by trying the legacy
      // dedicated UV replay before using the explicit flat fallback.
      if (TryCaptureTexcoordsViaStreamOut(dcs, geo, indexed, count, start, base))
        ++m_submitRejectStats.texcoordCaptured;
      else
        applyMissingTexcoordFallback();
    }

    DrawParameters params;
    params.instanceCount = 1;
    const bool submitIndexed = indexed && dcs.geometryData.indexBuffer.defined();
    params.vertexCount   = submitIndexed ? 0
      : (capturedExactPositions ? dcs.geometryData.vertexCount : count);
    params.indexCount    = submitIndexed ? count : 0;
    // SubmitDraw already folds StartIndexLocation and BaseVertexLocation (or
    // StartVertexLocation) into RasterBuffer slice offsets above. Reapplying
    // them here double-offsets sky/terrain helper draws and can read beyond the
    // compact capture. The RT-facing buffers always begin at element zero.
    params.firstIndex    = 0;
    params.vertexOffset  = 0;

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

    // DX11_V287_PC_VIEWSPACE_CAMERA: advance the PC view-space camera tracker
    // on the app-thread frame boundary (the same thread that samples in the
    // capture path, so no synchronization is needed). The emulator tracker
    // rotates separately on the publisher's guest frameId - the two never mix.
    if (RtxOptions::estimateViewSpaceCameraMotion() && !isKnownEmulatorHostProcess()) {
      s_pcViewSpaceCamera.beginFrame(
        kPcCameraMinSamplePoints, kPcCameraMaxTranslationPerFrame);
    }

    // DX11_V319_WORLD_ANCHOR_CAMERA: read back the previous frame's geometry
    // samples and advance the camera-position solve for camera-relative
    // engines. Runs on the same app thread as the capture path that queued
    // them, so the sample maps need no synchronization of their own.
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
    const uint32_t trustedSceneAcceptedDraws = realSceneAcceptedDraws > 0
      ? realSceneAcceptedDraws
      : (m_hasSeenRealSceneProjection ? 0u : sceneAcceptedDraws);
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
    const bool submitSummaryPeriodicDue =
      s_lastSubmitSummaryTime.time_since_epoch().count() == 0
      || (submitSummaryNow - s_lastSubmitSummaryTime) >= std::chrono::seconds(3);

    // Budget the burst separately for menu and world so neither starves the
    // other: whichever kind of frame is running, the first few are reported.
    static uint32_t s_submitSummaryWorldLogCount = 0;
    const uint32_t burstBudget = frameHasSceneGeometry
      ? s_submitSummaryWorldLogCount : s_submitSummaryLogCount;

    if ((burstBudget < 24 || submitSummaryPeriodicDue)
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
    m_context->EmitCs([backbuffer, draws, acceptedDraws, sceneAcceptedDraws, realSceneAcceptedDraws, sceneCandidateDraws, trustedSceneAcceptedDraws, allowResizeCameraCarryover, rasterUiSeen, midFrameRtxInjected, forceRasterPassThrough](DxvkContext* ctx) {
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
      const bool hasRealCamera = camValid && (hasRealView || hasConfirmedCameraRelativeView);

      static uint32_t sNoRealViewLogCount = 0;
      if (camValid && !hasRealView && !hasConfirmedCameraRelativeView && sNoRealViewLogCount < 12) {
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
    // Same coherent policy as EndFrame — see UpdateTrackedExtents. The HWND
    // client rect is only an occlusion signal and must not drive the
    // renderer; only the present-image extent may trigger resize handling.
    UpdateTrackedExtents(swapchainImage, remixViewportExtent);

    m_context->EmitCs([swapchainImage](DxvkContext* ctx) {
      RtxContext* rtx = static_cast<RtxContext*>(ctx);
      rtx->onPresent(swapchainImage);
    });
  }

}
