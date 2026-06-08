/*
* Copyright (c) 2022-2024, NVIDIA CORPORATION. All rights reserved.
*
* Permission is hereby granted, free of charge, to any person obtaining a
* copy of this software and associated documentation files (the "Software"),
* to deal in the Software without restriction, including without limitation
* the rights to use, copy, modify, merge, publish, distribute, sublicense,
* and/or sell copies of the Software, and to permit persons to whom the
* Software is furnished to do so, subject to the following conditions:
*
* The above copyright notice and this permission notice shall be included in
* all copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
* THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
* LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
* FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
* DEALINGS IN THE SOFTWARE.
*/
#include "rtx_camera_manager.h"

#include "dxvk_device.h"

#include <cmath>

namespace {
  constexpr float kFovToleranceRadians = 0.001f;
  constexpr float kMaxProjectionShear = 1.5f;
  constexpr float kMinViewportFallbackMainCameraScore = 0.0f;

  bool isFiniteMatrix(const dxvk::Matrix4& matrix) {
    for (uint32_t row = 0; row < 4; ++row) {
      for (uint32_t col = 0; col < 4; ++col) {
        if (!std::isfinite(matrix[row][col]))
          return false;
      }
    }

    return true;
  }

  bool isSafelyInvertibleMatrix(const dxvk::Matrix4& matrix) {
    if (!isFiniteMatrix(matrix))
      return false;

    const double det = dxvk::determinant(matrix);
    return std::isfinite(det) && std::abs(det) >= 1.0e-12;
  }

  float scoreMainCameraCandidate(const dxvk::DrawCallState& input, const ::DecomposeProjectionParams& projection) {
    float score = 0.0f;

    if (!input.getTransformData().usedViewportFallbackProjection) {
      score += 6.0f;
    } else {
      score -= 4.0f;
    }

    const float projectionShear = std::max(std::abs(projection.shearX), std::abs(projection.shearY));
    if (projectionShear > 0.02f)
      score -= std::min(projectionShear, 1.0f);

    if (!dxvk::isIdentityExact(input.getTransformData().worldToView))
      score += 3.0f;
    else if (input.getTransformData().usedViewportFallbackProjection
          && dxvk::isIdentityExact(input.getTransformData().objectToWorld))
      score -= 2.0f;

    if (input.zEnable)
      score += 1.5f;
    else
      score -= 1.0f;

    if (input.zWriteEnable)
      score += 1.0f;

    if (input.maxZ >= 0.99f)
      score += 1.0f;
    if (input.minZ <= 0.01f)
      score += 0.5f;

    if (input.usesVertexShader)
      score += 0.5f;
    if (input.usesPixelShader)
      score += 0.5f;

    if (input.isUsingRaytracedRenderTarget)
      score -= 2.0f;

    const uint32_t primitiveCount = input.getGeometryData().calculatePrimitiveCount();
    if (primitiveCount >= 64u)
      score += 1.0f;
    else if (primitiveCount <= 4u)
      score -= 1.5f;

    if (std::isfinite(projection.aspectRatio) && projection.aspectRatio > 0.5f && projection.aspectRatio < 4.0f)
      score += 0.5f;

    return score;
  }
}

namespace dxvk {

  CameraManager::CameraManager(DxvkDevice* device) : CommonDeviceObject(device) {
    for (int i = 0; i < CameraType::Count; i++) {
      m_cameras[i].setCameraType(CameraType::Enum(i));
    }
  }

  bool CameraManager::isCameraValid(CameraType::Enum cameraType) const {
    assert(cameraType < CameraType::Enum::Count);
    return accessCamera(*this, cameraType).isValid(m_device->getCurrentFrameId());
  }

  void CameraManager::onFrameEnd() {
    m_lastSetCameraType = CameraType::Unknown;
    m_mainCameraCandidateScore = -1.0e30f;
    m_mainCameraCandidateUsedFallback = false;
    m_decompositionCache.clear();
  }

  CameraType::Enum CameraManager::processCameraData(const DrawCallState& input) {
    // If theres no real camera data here - bail
    if (isIdentityExact(input.getTransformData().viewToProjection)) {
      return input.testCategoryFlags(InstanceCategories::Sky) ? CameraType::Sky : CameraType::Unknown;
    }

    if (!isSafelyInvertibleMatrix(input.getTransformData().viewToProjection)
     || (!isIdentityExact(input.getTransformData().worldToView)
      && !isSafelyInvertibleMatrix(input.getTransformData().worldToView))) {
      ONCE(Logger::warn("[RTX] CameraManager: rejected a camera with non-invertible matrices"));
      return input.getCategoryFlags().test(InstanceCategories::Sky) ? CameraType::Sky : CameraType::Unknown;
    }

    switch (RtxOptions::fusedWorldViewMode()) {
    case FusedWorldViewMode::None:
      if (input.getTransformData().objectToView == input.getTransformData().objectToWorld && !isIdentityExact(input.getTransformData().objectToView)) {
        return input.testCategoryFlags(InstanceCategories::Sky) ? CameraType::Sky : CameraType::Unknown;
      }
      break;
    case FusedWorldViewMode::View:
      if (Logger::logLevel() >= LogLevel::Warn) {
        // Check if World is identity
        ONCE_IF_FALSE(isIdentityExact(input.getTransformData().objectToWorld),
                      Logger::warn("[RTX-Compatibility] Fused world-view tranform set to View but World transform is not identity!"));
      }
      break;
    case FusedWorldViewMode::World:
      if (Logger::logLevel() >= LogLevel::Warn) {
        // Check if View is identity
        ONCE_IF_FALSE(isIdentityExact(input.getTransformData().objectToView),
                      Logger::warn("[RTX-Compatibility] Fused world-view tranform set to World but View transform is not identity!"));
      }
      break;
    }

    // Get camera params
    DecomposeProjectionParams decomposeProjectionParams = getOrDecomposeProjection(input.getTransformData().viewToProjection);

    // Filter invalid cameras, extreme shearing
    static auto isFovValid = [](float fovA) {
      return fovA >= kFovToleranceRadians;
    };
    static auto areFovsClose = [](float fovA, const RtCamera& cameraB) {
      return std::abs(fovA - cameraB.getFov()) < kFovToleranceRadians;
    };

    const float projectionShear = std::max(std::abs(decomposeProjectionParams.shearX), std::abs(decomposeProjectionParams.shearY));
    if (!std::isfinite(projectionShear) || projectionShear > kMaxProjectionShear || !isFovValid(decomposeProjectionParams.fov)) {
      ONCE(Logger::warn("[RTX] CameraManager: rejected an invalid camera"));
      return input.getCategoryFlags().test(InstanceCategories::Sky) ? CameraType::Sky : CameraType::Unknown;
    }


    auto isViewModel = [this](float fov, float maxZ, uint32_t frameId) {
      if (RtxOptions::ViewModel::enable()) {
        // Note: max Z check is the top-priority
        if (maxZ <= RtxOptions::ViewModel::maxZThreshold()) {
          return true;
        }
        if (getCamera(CameraType::Main).isValid(frameId)) {
          // FOV is different from Main camera => assume that it's a ViewModel one
          if (!areFovsClose(fov, getCamera(CameraType::Main))) {
            return true;
          }
        }
      }
      return false;
    };

    const uint32_t frameId = m_device->getCurrentFrameId();

    auto cameraType = CameraType::Main;
    if (input.isDrawingToRaytracedRenderTarget) {
      cameraType = CameraType::RenderToTexture;
    } else if (input.testCategoryFlags(InstanceCategories::Sky)) {
      cameraType = CameraType::Sky;
    } else if (isViewModel(decomposeProjectionParams.fov, input.maxZ, frameId)) {
      cameraType = CameraType::ViewModel;
    }

    // Suppress viewport-fallback candidates for Main once a real camera has
    // ever been accepted. The fallback is only a bootstrap for games that
    // never expose a usable projection cbuffer; after a real camera exists,
    // letting a synthesized projection overwrite Main can path trace a
    // post-process/menu viewport and destabilize temporal resources.
    if (cameraType == CameraType::Main
        && input.getTransformData().usedViewportFallbackProjection
        && m_hasSeenRealMainCamera) {
      m_lastSetCameraType = CameraType::Unknown;

      static uint32_t sPostRealViewportFallbackLogCount = 0;
      if (sPostRealViewportFallbackLogCount < 12) {
        ++sPostRealViewportFallbackLogCount;
        Logger::info(str::format(
          "[RTX] CameraManager: rejected viewport-fallback main-camera candidate after a real main camera was seen (drawCallID=",
          input.drawCallID,
          ")"));
      }

      return CameraType::Unknown;
    }

    // Check fov consistency across frames
    if (frameId > 0) {
      if (getCamera(cameraType).isValid(frameId - 1) && !areFovsClose(decomposeProjectionParams.fov, getCamera(cameraType))) {
        ONCE(Logger::info("[RTX] CameraManager: FOV of a camera changed between frames"));
      }
    }

    auto& camera = getCamera(cameraType);
    auto cameraSequence = RtCameraSequence::getInstance();
    const float candidateScore = cameraType == CameraType::Main
      ? scoreMainCameraCandidate(input, decomposeProjectionParams)
      : 0.0f;
    bool shouldUpdateMainCamera = cameraType == CameraType::Main && camera.getLastUpdateFrame() != frameId;
    bool isPlaying = RtCameraSequence::mode() == RtCameraSequence::Mode::Playback;
    bool isBrowsing = RtCameraSequence::mode() == RtCameraSequence::Mode::Browse;
    bool isCameraCut = false;
    Matrix4 worldToView = input.getTransformData().worldToView;
    Matrix4 viewToProjection = input.getTransformData().viewToProjection;
    const bool candidateUsesFallbackProjection = input.getTransformData().usedViewportFallbackProjection;

    if (cameraType == CameraType::Main
        && candidateUsesFallbackProjection
        && candidateScore < kMinViewportFallbackMainCameraScore) {
      m_lastSetCameraType = CameraType::Unknown;

      static uint32_t sWeakViewportFallbackCameraLogCount = 0;
      if (sWeakViewportFallbackCameraLogCount < 12) {
        ++sWeakViewportFallbackCameraLogCount;
        Logger::info(str::format(
          "[RTX] CameraManager: rejected weak viewport-fallback main-camera candidate (score=",
          candidateScore,
          " drawCallID=",
          input.drawCallID,
          ")"));
      }

      return input.getCategoryFlags().test(InstanceCategories::Sky) ? CameraType::Sky : CameraType::Unknown;
    }

    if (cameraType == CameraType::Main && camera.getLastUpdateFrame() == frameId && !isPlaying && !isBrowsing) {
      const bool shouldReplaceCurrentMain = (m_mainCameraCandidateUsedFallback != candidateUsesFallbackProjection)
        ? (m_mainCameraCandidateUsedFallback && !candidateUsesFallbackProjection)
        : (candidateScore > m_mainCameraCandidateScore + 0.5f);
      if (!shouldReplaceCurrentMain) {
        m_lastSetCameraType = cameraType;
        return cameraType;
      }

      shouldUpdateMainCamera = true;

      static uint32_t sMainCameraReplacementLogCount = 0;
      if (sMainCameraReplacementLogCount < 12) {
        ++sMainCameraReplacementLogCount;
        Logger::info(str::format(
          "[RTX] CameraManager: replacing weaker main-camera candidate in-frame (oldScore=",
          m_mainCameraCandidateScore,
          " newScore=",
          candidateScore,
          " drawCallID=",
          input.drawCallID,
          input.getTransformData().usedViewportFallbackProjection ? " viewportFallback" : " cbufferProjection",
          ")"));
      }
    }
    if (isPlaying || isBrowsing) {
      if (shouldUpdateMainCamera) {
        RtCamera::RtCameraSetting setting;
        cameraSequence->getRecord(cameraSequence->currentFrame(), setting);
        isCameraCut = camera.updateFromSetting(frameId, setting, 0);

        if (isPlaying) {
          cameraSequence->goToNextFrame();
        }
      }
    } else {
      isCameraCut = camera.update(
        frameId,
        worldToView,
        viewToProjection,
        decomposeProjectionParams.fov,
        decomposeProjectionParams.aspectRatio,
        decomposeProjectionParams.nearPlane,
        decomposeProjectionParams.farPlane,
        decomposeProjectionParams.isLHS
      );
    }


    if (shouldUpdateMainCamera && RtCameraSequence::mode() == RtCameraSequence::Mode::Record) {
      auto& setting = camera.getSetting();
      cameraSequence->addRecord(setting);
    }

    if (cameraType == CameraType::Main && camera.getLastUpdateFrame() == frameId) {
      m_mainCameraCandidateScore = candidateScore;
      const bool usedFallback = input.getTransformData().usedViewportFallbackProjection;
      m_mainCameraCandidateUsedFallback = usedFallback;
      if (!usedFallback) {
        m_lastMainCbufferProjFrameId = frameId;
        m_hasSeenRealMainCamera = true;
      }
      // Force a camera cut when the projection source flips between
      // fallback and real cbuffer. Without this the denoiser keeps
      // temporal history from the wrong projection for ~10 frames,
      // which is exactly the "flicker after Remix loads" the user sees
      // when the splash/fallback phase ends and gameplay begins.
      if (m_hasLastMainUpdate && usedFallback != m_lastMainUsedFallbackProj) {
        isCameraCut = true;
      }
      m_lastMainUsedFallbackProj = usedFallback;
      m_hasLastMainUpdate = true;
    }

    // Register camera cut when there are significant interruptions to the view (like changing level, or opening a menu)
    if (isCameraCut && cameraType == CameraType::Main) {
      m_lastCameraCutFrameId = m_device->getCurrentFrameId();
    }
    m_lastSetCameraType = cameraType;

    return cameraType;
  }

  bool CameraManager::isCameraCutThisFrame() const {
    return m_lastCameraCutFrameId == m_device->getCurrentFrameId();
  }

  void CameraManager::processExternalCamera(CameraType::Enum type,
                                            const Matrix4& worldToView,
                                            const Matrix4& viewToProjection) {
    if (type >= CameraType::Count || type == CameraType::Unknown) {
      ONCE(Logger::warn("[RTX] CameraManager: ignored an external camera with an invalid type"));
      return;
    }

    if (!isSafelyInvertibleMatrix(viewToProjection)
     || (!isIdentityExact(worldToView) && !isSafelyInvertibleMatrix(worldToView))) {
      ONCE(Logger::warn("[RTX] CameraManager: rejected an external camera with non-invertible matrices"));
      return;
    }

    DecomposeProjectionParams decomposeProjectionParams = getOrDecomposeProjection(viewToProjection);

    const float projectionShear = std::max(std::abs(decomposeProjectionParams.shearX), std::abs(decomposeProjectionParams.shearY));
    if (!std::isfinite(projectionShear) || projectionShear > kMaxProjectionShear || decomposeProjectionParams.fov < kFovToleranceRadians) {
      ONCE(Logger::warn("[RTX] CameraManager: rejected an invalid external camera"));
      return;
    }

    const bool isCameraCut = getCamera(type).update(
      m_device->getCurrentFrameId(),
      worldToView,
      viewToProjection,
      decomposeProjectionParams.fov,
      decomposeProjectionParams.aspectRatio,
      decomposeProjectionParams.nearPlane,
      decomposeProjectionParams.farPlane,
      decomposeProjectionParams.isLHS);

    if (type == CameraType::Main) {
      if (isCameraCut) {
        m_lastCameraCutFrameId = m_device->getCurrentFrameId();
      }
      m_mainCameraCandidateScore = std::max(m_mainCameraCandidateScore, 0.0f);
      m_mainCameraCandidateUsedFallback = false;
      m_lastMainCbufferProjFrameId = m_device->getCurrentFrameId();
      m_lastMainUsedFallbackProj = false;
      m_hasLastMainUpdate = true;
      m_hasSeenRealMainCamera = true;
    }

    m_lastSetCameraType = type;
  }

    DecomposeProjectionParams CameraManager::getOrDecomposeProjection(const Matrix4& viewToProjection) {
      XXH64_hash_t projectionHash = XXH64(&viewToProjection, sizeof(viewToProjection), 0);
      auto iter = m_decompositionCache.find(projectionHash);
      if (iter != m_decompositionCache.end()) {
        return iter->second;
      }

      DecomposeProjectionParams decomposeProjectionParams;
      decomposeProjection(viewToProjection, decomposeProjectionParams);
      m_decompositionCache.emplace(projectionHash, decomposeProjectionParams);
      return decomposeProjectionParams;
    }
}  // namespace dxvk
