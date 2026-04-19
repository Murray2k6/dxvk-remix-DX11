/*
* Copyright (c) 2021-2026, NVIDIA CORPORATION. All rights reserved.
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
#include "rtx_options.h"
#include "../imgui/dxvk_imgui.h"

#include <filesystem>
#include <nvapi.h>
#include "../imgui/imgui.h"
#include "rtx_bridge_message_channel.h"
#include "rtx_terrain_baker.h"
#include "rtx_nee_cache.h"
#include "rtx_xess.h"
#include "rtx_rtxdi_rayquery.h"
#include "rtx_restir_gi_rayquery.h"
#include "rtx_composite.h"
#include "rtx_demodulate.h"
#include "rtx_neural_radiance_cache.h"
#include "rtx_ray_reconstruction.h"

#include "dxvk_device.h"
#include "rtx_global_volumetrics.h"

namespace dxvk {
  RtxOptions* RtxOptions::s_instance = nullptr;
  HashRule RtxOptions::s_geometryHashGenerationRule = 0;
  HashRule RtxOptions::s_geometryAssetHashRule = 0;

  
  void RtxOptions::graphicsPresetOnChange(DxvkDevice* device) {
    // device will be nullptr during initial config loading.
    if (device == nullptr) {
      return;
    }

    // When switching to Custom preset, migrate ALL Quality layer settings to User layer.
    // This allows users to customize settings that were previously controlled by the preset.
    // NOTE: this does not run during the initial load due to the device nullptr check above.
    if (RtxOptions::graphicsPreset() == GraphicsPreset::Custom) {
      const RtxOptionLayer* qualityLayer = RtxOptionLayer::getQualityLayer();
      const RtxOptionLayer* userLayer = RtxOptionLayer::getUserLayer();
      
      if (qualityLayer && userLayer) {
        // Migrate ALL options from Quality layer to User layer (not just those with the flag)
        for (auto& [hash, optionPtr] : RtxOptionImpl::getGlobalOptionMap()) {
          optionPtr->moveLayerValue(qualityLayer, userLayer);
        }
        Logger::info("[Graphics Preset] Switched to Custom - Quality settings migrated to User layer");
      }
      return;  // Don't apply preset settings when Custom - Quality layer is now empty
    }

    // TODO[REMIX-1482]: Currently tests expect to skip applying the graphics preset, so this needs to be skipped in test runs.
    // When we fix tests to actually use the preset, the if statement should be removed.
    if (env::getEnvVar("DXVK_TERMINATE_APP_FRAME") == "" ||
        env::getEnvVar("DXVK_GRAPHICS_PRESET_TYPE") != "0") {
      RtxOptions::updateGraphicsPresets(device);
    }
  }

  void RtxOptions::showUICursorOnChange(DxvkDevice* device) {
    if (ImGui::GetCurrentContext() != nullptr) {
      auto& io = ImGui::GetIO();
      const bool uiOpen = dxvk::ImGUI::GetLastKnownMenuType() != UIType::None;
      io.MouseDrawCursor = RtxOptions::showUICursor() && uiOpen;
    }
  }

  void RtxOptions::blockInputToGameInUIOnChange(DxvkDevice* device) {
    const bool uiOpen = dxvk::ImGUI::GetLastKnownMenuType() != UIType::None;
    const bool doBlock = RtxOptions::blockInputToGameInUI() && uiOpen;

    BridgeMessageChannel::get().send("UWM_REMIX_UIACTIVE_MSG", doBlock ? 1 : 0, 0);
  }

  namespace {
    bool migrateHashSet(const GenericValue& src, GenericValue& dst, bool destHasExistingValue) {
      HashSetLayer* sourceHashSet = src.hashSet;
      HashSetLayer* destHashSet = dst.hashSet;
      if (!sourceHashSet || sourceHashSet->empty() || !destHashSet) {
        return false;
      }
      // Union merge for hash sets
      destHashSet->mergeFrom(*sourceHashSet);
      return true;
    }
  }
  void RtxOptions::dynamicDecalTexturesOnChange(DxvkDevice* device) {
    if (dynamicDecalTextures.migrateValuesTo(&decalTextures, migrateHashSet)) {
      dynamicDecalTextures.clearFromStrongerLayers(RtxOptionLayer::getDefaultLayer());
      Logger::info("[Deprecated Config] rtx.dynamicDecalTextures has been deprecated, "
                   "we have moved all your textures from this list to rtx.decalTextures, "
                   "no further action is required from you. "
                   "Please re-save your rtx config to get rid of this message.");
    }
  }

  void RtxOptions::singleOffsetDecalTexturesOnChange(DxvkDevice* device) {
    if (singleOffsetDecalTextures.migrateValuesTo(&decalTextures, migrateHashSet)) {
      singleOffsetDecalTextures.clearFromStrongerLayers(RtxOptionLayer::getDefaultLayer());
      Logger::info("[Deprecated Config] rtx.singleOffsetDecalTextures has been deprecated, "
                   "we have moved all your textures from this list to rtx.decalTextures, "
                   "no further action is required from you. "
                   "Please re-save your rtx config to get rid of this message.");
    }
  }

  void RtxOptions::nonOffsetDecalTexturesOnChange(DxvkDevice* device) {
    if (nonOffsetDecalTextures.migrateValuesTo(&decalTextures, migrateHashSet)) {
      nonOffsetDecalTextures.clearFromStrongerLayers(RtxOptionLayer::getDefaultLayer());
      Logger::info("[Deprecated Config] rtx.nonOffsetDecalTextures has been deprecated, "
                   "we have moved all your textures from this list to rtx.decalTextures, "
                   "no further action is required from you. "
                   "Please re-save your rtx config to get rid of this message.");
    }
  }

  void RtxOptions::updateUpscalerFromDlssPreset(DxvkDevice* device) {
    // Code-driven changes for DLSS preset (automatically routes to User layer when preset is Custom)
    RtxOptionLayerTarget layerTarget(RtxOptionEditTarget::Derived);

    if (RtxOptions::Automation::disableUpdateUpscaleFromDlssPreset()) {
      return;
    }

    switch (dlssPreset()) {
      // TODO[REMIX-4105] all of these are used right after being set, so this needs to be setImmediately.
      // This should be addressed by REMIX-4109 if that is done before REMIX-4105 is fully cleaned up.
      case DlssPreset::Off:
        upscalerType.setImmediately(UpscalerType::None);
        reflexMode.setImmediately(ReflexMode::None);
        break;
      case DlssPreset::On:
      {
        const UpscalerType requestedUpscaler = UpscalerType::DLSS;
        const UpscalerType resolvedUpscaler = device != nullptr
          ? getSupportedUpscalerForDevice(device, requestedUpscaler)
          : requestedUpscaler;

        if (resolvedUpscaler != requestedUpscaler) {
          Logger::info(str::format("RTX: DLSS preset requested an unsupported upscaler on this GPU, falling back from ", static_cast<int>(requestedUpscaler), " to ", static_cast<int>(resolvedUpscaler)));
        }

        upscalerType.setImmediately(resolvedUpscaler);
        qualityDLSS.setImmediately(DLSSProfile::Auto);
        reflexMode.setImmediately(ReflexMode::LowLatency); // Reflex uses ON under G (not Boost)
        break;
      }
      case DlssPreset::Custom:
        break;
    }
  }

  void RtxOptions::updateUpscalerFromNisPreset() {
    // Code-driven changes for NIS preset (automatically routes to User layer when preset is Custom)
    RtxOptionLayerTarget layerTarget(RtxOptionEditTarget::Derived);

    switch (nisPreset()) {
    case NisPreset::Performance:
      resolutionScale.setDeferred(0.5f);
      break;
    case NisPreset::Balanced:
      resolutionScale.setDeferred(0.66f);
      break;
    case NisPreset::Quality:
      resolutionScale.setDeferred(0.75f);
      break;
    case NisPreset::Fullscreen:
      resolutionScale.setDeferred(1.0f);
      break;
    }
  }

  void RtxOptions::updateUpscalerFromTaauPreset() {
    // Code-driven changes for TAAU preset (automatically routes to User layer when preset is Custom)
    RtxOptionLayerTarget layerTarget(RtxOptionEditTarget::Derived);

    switch (taauPreset()) {
    case TaauPreset::UltraPerformance:
      resolutionScale.setDeferred(0.33f);
      break;
    case TaauPreset::Performance:
      resolutionScale.setDeferred(0.5f);
      break;
    case TaauPreset::Balanced:
      resolutionScale.setDeferred(0.66f);
      break;
    case TaauPreset::Quality:
      resolutionScale.setDeferred(0.75f);
      break;
    case TaauPreset::Fullscreen:
      resolutionScale.setDeferred(1.0f);
      break;
    }
  }

  void RtxOptions::updatePresetFromUpscaler() {
    // Code-driven changes for upscaler preset (automatically routes to User layer when preset is Custom)
    RtxOptionLayerTarget layerTarget(RtxOptionEditTarget::Derived);

    if (RtxOptions::upscalerType() == UpscalerType::None &&
        reflexMode() == ReflexMode::None) {
      RtxOptions::dlssPreset.setDeferred(DlssPreset::Off);
    } else if (RtxOptions::upscalerType() == UpscalerType::DLSS &&
               reflexMode() == ReflexMode::LowLatency) {
      if ((graphicsPreset() == GraphicsPreset::Ultra || graphicsPreset() == GraphicsPreset::High) &&
          qualityDLSS() == DLSSProfile::Auto) {
        RtxOptions::dlssPreset.setDeferred(DlssPreset::On);
      } else {
        RtxOptions::dlssPreset.setDeferred(DlssPreset::Custom);
      }
    } else {
      RtxOptions::dlssPreset.setDeferred(DlssPreset::Custom);
    }

    switch (RtxOptions::upscalerType()) {
      case UpscalerType::NIS: {
        const float nisResolutionScale = resolutionScale();
        if (nisResolutionScale <= 0.5f) {
          RtxOptions::nisPreset.setDeferred(NisPreset::Performance);
        } else if (nisResolutionScale <= 0.66f) {
          RtxOptions::nisPreset.setDeferred(NisPreset::Balanced);
        } else if (nisResolutionScale <= 0.75f) {
          RtxOptions::nisPreset.setDeferred(NisPreset::Quality);
        } else {
          RtxOptions::nisPreset.setDeferred(NisPreset::Fullscreen);
        }
        break;
      }
      case UpscalerType::TAAU: {
        const float taauResolutionScale = resolutionScale();
        if (taauResolutionScale <= 0.33f) {
          RtxOptions::taauPreset.setDeferred(TaauPreset::UltraPerformance);
        } else if (taauResolutionScale <= 0.5f) {
          RtxOptions::taauPreset.setDeferred(TaauPreset::Performance);
        } else if (taauResolutionScale <= 0.66f) {
          RtxOptions::taauPreset.setDeferred(TaauPreset::Balanced);
        } else if (taauResolutionScale <= 0.75f) {
          RtxOptions::taauPreset.setDeferred(TaauPreset::Quality);
        } else {
          RtxOptions::taauPreset.setDeferred(TaauPreset::Fullscreen);
        }
        break;
      }
      default:
        break;
    }
  }

  UpscalerType RtxOptions::getSupportedUpscalerForDevice(DxvkDevice* device, UpscalerType requestedUpscaler) {
    auto supportsUpscaler = [&](UpscalerType upscaler) {
      switch (upscaler) {
        case UpscalerType::None:
        case UpscalerType::NIS:
        case UpscalerType::TAAU:
          return true;
        case UpscalerType::DLSS:
          return device != nullptr && device->getCommon()->metaDLSS().supportsDLSS();
        case UpscalerType::XeSS:
          return device != nullptr && device->getCommon()->metaXeSS().supportsXeSS();
      }

      return false;
    };

    if (requestedUpscaler == UpscalerType::None || supportsUpscaler(requestedUpscaler)) {
      return requestedUpscaler;
    }

    const uint32_t vendorID = device != nullptr ? device->adapter()->deviceProperties().vendorID : 0;

    auto chooseFirstSupported = [&](std::initializer_list<UpscalerType> candidates) {
      for (UpscalerType candidate : candidates) {
        if (supportsUpscaler(candidate)) {
          return candidate;
        }
      }

      return UpscalerType::None;
    };

    if (vendorID == static_cast<uint32_t>(DxvkGpuVendor::Nvidia)) {
      return chooseFirstSupported({ UpscalerType::DLSS, UpscalerType::XeSS, UpscalerType::NIS, UpscalerType::TAAU });
    }

    if (vendorID == static_cast<uint32_t>(DxvkGpuVendor::Intel)) {
      return chooseFirstSupported({ UpscalerType::XeSS, UpscalerType::NIS, UpscalerType::TAAU, UpscalerType::DLSS });
    }

    return chooseFirstSupported({ UpscalerType::XeSS, UpscalerType::NIS, UpscalerType::TAAU, UpscalerType::DLSS });
  }

  IntegrateIndirectMode RtxOptions::getSupportedIntegrateIndirectMode(DxvkDevice* device, IntegrateIndirectMode requestedMode) {
    if (requestedMode != IntegrateIndirectMode::NeuralRadianceCache || device == nullptr) {
      return requestedMode;
    }

    return NeuralRadianceCache::checkIsSupported(device)
      ? requestedMode
      : IntegrateIndirectMode::ReSTIRGI;
  }

  static bool queryNvidiaArchInfo(NV_GPU_ARCH_INFO& archInfo) {
    NvAPI_Status status;
    status = NvAPI_Initialize();
    if (status != NVAPI_OK) {
      return false;
    }
    
    NvPhysicalGpuHandle nvGPUHandle[NVAPI_MAX_PHYSICAL_GPUS];
    NvU32 GpuCount;
    status = NvAPI_EnumPhysicalGPUs(nvGPUHandle, &GpuCount);
    if (status != NVAPI_OK) {
      return false;
    }
    
    assert(GpuCount > 0);

    archInfo.version = NV_GPU_ARCH_INFO_VER;
    // Note: Currently only using the first returned GPU Handle. Ideally this should use the GPU Handle Vulkan is using
    // though in the case of a mixed architecture multi-GPU system.
    status = NvAPI_GPU_GetArchInfo(nvGPUHandle[0], &archInfo);
    return status == NVAPI_OK;
  }

  NV_GPU_ARCHITECTURE_ID RtxOptions::getNvidiaArch() {
    NV_GPU_ARCH_INFO archInfo;
    if (queryNvidiaArchInfo(archInfo) == false) {
      return NV_GPU_ARCHITECTURE_TU100;
    }
    
    return archInfo.architecture_id;
  }

  NV_GPU_ARCH_IMPLEMENTATION_ID RtxOptions::getNvidiaChipId() {
    NV_GPU_ARCH_INFO archInfo;
    if (queryNvidiaArchInfo(archInfo) == false) {
      return NV_GPU_ARCH_IMPLEMENTATION_TU100;
    }
    
    return archInfo.implementation_id;
  }

  void RtxOptions::updatePathTracerPreset(PathTracerPreset preset) {
    // Code-driven changes for path tracer preset (automatically routes to User layer when preset is Custom)
    RtxOptionLayerTarget layerTarget(RtxOptionEditTarget::Derived);

    if (preset == PathTracerPreset::RayReconstruction) {
      // RTXDI
      DxvkRtxdiRayQuery::stealBoundaryPixelSamplesWhenOutsideOfScreen.setDeferred(false);
      DxvkRtxdiRayQuery::permutationSamplingNthFrame.setDeferred(1);
      DxvkRtxdiRayQuery::enableDenoiserConfidence.setDeferred(false);
      DxvkRtxdiRayQuery::enableBestLightSampling.setDeferred(false);
      DxvkRtxdiRayQuery::initialSampleCount.setDeferred(3);
      DxvkRtxdiRayQuery::spatialSamples.setDeferred(2);
      DxvkRtxdiRayQuery::disocclusionSamples.setDeferred(2);
      DxvkRtxdiRayQuery::enableSampleStealing.setDeferred(false);

      // ReSTIR GI
      if (RtxOptions::useReSTIRGI()) {
        DxvkReSTIRGIRayQuery::setToRayReconstructionPreset();
      }

      // Integrator
      minOpaqueDiffuseLobeSamplingProbability.setDeferred(0.05f);
      minOpaqueSpecularLobeSamplingProbability.setDeferred(0.05f);
      enableFirstBounceLobeProbabilityDithering.setDeferred(false);
      russianRouletteMode.setDeferred(RussianRouletteMode::SpecularBased);

      // NEE Cache
      NeeCachePass::enableModeAfterFirstBounce.setDeferred(NeeEnableMode::All);

      // Demodulate
      DemodulatePass::enableDirectLightBoilingFilter.setDeferred(false);

      // Composite
      CompositePass::postFilterThreshold.setDeferred(10.0f);
      CompositePass::usePostFilter.setDeferred(false);

    } else if (preset == PathTracerPreset::Default) {
      // This is the default setting used by NRD
      // RTXDI
      DxvkRtxdiRayQuery::stealBoundaryPixelSamplesWhenOutsideOfScreenObject().resetToDefault();
      DxvkRtxdiRayQuery::permutationSamplingNthFrameObject().resetToDefault();
      DxvkRtxdiRayQuery::enableDenoiserConfidenceObject().resetToDefault();
      DxvkRtxdiRayQuery::enableBestLightSamplingObject().resetToDefault();
      DxvkRtxdiRayQuery::initialSampleCountObject().resetToDefault();
      DxvkRtxdiRayQuery::spatialSamplesObject().resetToDefault();
      DxvkRtxdiRayQuery::disocclusionSamplesObject().resetToDefault();
      DxvkRtxdiRayQuery::enableSampleStealingObject().resetToDefault();

      // ReSTIR GI
      if (RtxOptions::useReSTIRGI()) {
        DxvkReSTIRGIRayQuery::setToNRDPreset();
      }

      // Integrator
      minOpaqueDiffuseLobeSamplingProbabilityObject().resetToDefault();
      minOpaqueSpecularLobeSamplingProbabilityObject().resetToDefault();
      enableFirstBounceLobeProbabilityDitheringObject().resetToDefault();
      russianRouletteModeObject().resetToDefault();

      // NEE Cache
      NeeCachePass::enableModeAfterFirstBounceObject().resetToDefault();

      // Demodulate
      DemodulatePass::enableDirectLightBoilingFilterObject().resetToDefault();

      // Composite
      CompositePass::postFilterThresholdObject().resetToDefault();
      CompositePass::usePostFilterObject().resetToDefault();
    }
  }

  void RtxOptions::updateLightingSetting() {
    // Code-driven changes for lighting setting (automatically routes to User layer when preset is Custom)
    RtxOptionLayerTarget layerTarget(RtxOptionEditTarget::Derived);

    bool isRayReconstruction = RtxOptions::isRayReconstructionEnabled();
    bool isDLSS = RtxOptions::isDLSSEnabled();
    bool isNative = RtxOptions::upscalerType() == UpscalerType::None;
    if (isRayReconstruction) {
      updatePathTracerPreset(DxvkRayReconstruction::pathTracerPreset());
    } else if (isDLSS) {
      updatePathTracerPreset(PathTracerPreset::Default);
    } else if (isNative) {
      if (!DxvkRayReconstruction::preserveSettingsInNativeMode()) {
        updatePathTracerPreset(PathTracerPreset::Default);
      }
    }
  }
    
  void RtxOptions::updateGraphicsPresets(DxvkDevice* device) {
    // Guard against calls that arrive before RtxOptions has been constructed.
    // rtx_initializer.cpp calls this once before applyPendingValues (no-op) and
    // once after (the real run with a valid device for all GPU vendors).
    if (!RtxOptionImpl::isInitialized()) {
      return;
    }

    // Code-driven changes for graphics preset (automatically routes to User layer when preset is Custom)
    RtxOptionLayerTarget layerTarget(RtxOptionEditTarget::Derived);

    // graphicsPreset drives the Quality layer; it should never retain its own value there.
    // A stale Quality-layer Auto entry will override the User layer and make the UI appear stuck.
    RtxOptions::graphicsPreset.disableLayerValue(RtxOptionLayer::getQualityLayer());

    GraphicsPreset effectiveGraphicsPreset = RtxOptions::graphicsPreset();

    // Handle Automatic Graphics Preset (From configuration/default)

    if (effectiveGraphicsPreset == GraphicsPreset::Auto) {
      const DxvkDeviceInfo& deviceInfo = device->adapter()->devicePropertiesExt();
      const uint32_t vendorID = deviceInfo.core.properties.vendorID;
      UpscalerType preferredUpscaler = UpscalerType::TAAU;
      
      // Default updateGraphicsPresets value, don't want to hit this path intentionally or Low settings will be used
      assert(vendorID != 0);

      Logger::info("Automatic Graphics Preset in use (Set rtx.graphicsPreset to something other than Auto use a non-automatic preset)");

      GraphicsPreset preferredDefault = GraphicsPreset::Low;

      if (vendorID == static_cast<uint32_t>(DxvkGpuVendor::Nvidia)) {
        const NV_GPU_ARCHITECTURE_ID archId = getNvidiaArch();

        if (archId < NV_GPU_ARCHITECTURE_TU100) {
          // Pre-Turing
          Logger::info("NVIDIA architecture without HW RTX support detected, setting default graphics settings to Low, but your experience may not be optimal");
          preferredDefault = GraphicsPreset::Low;
          RtxOptions::qualityDLSS.setDeferred(DLSSProfile::MaxPerf);
          DxvkXeSS::XessOptions::preset.setDeferred(XeSSPreset::Performance);
          RtxOptions::nisPreset.setDeferred(NisPreset::Performance);
          RtxOptions::taauPreset.setDeferred(TaauPreset::Performance);
        } else if (archId < NV_GPU_ARCHITECTURE_GA100) {
          // Turing
          Logger::info("NVIDIA Turing architecture detected, setting default graphics settings to Low");
          preferredDefault = GraphicsPreset::Low;
          RtxOptions::qualityDLSS.setDeferred(DLSSProfile::MaxPerf);
          DxvkXeSS::XessOptions::preset.setDeferred(XeSSPreset::Performance);
          RtxOptions::nisPreset.setDeferred(NisPreset::Performance);
          RtxOptions::taauPreset.setDeferred(TaauPreset::Performance);
        } else if (archId < NV_GPU_ARCHITECTURE_AD100) {
          // Ampere
          Logger::info("NVIDIA Ampere architecture detected, setting default graphics settings to Medium");
          preferredDefault = GraphicsPreset::Medium;
          RtxOptions::qualityDLSS.setDeferred(DLSSProfile::Balanced);
          DxvkXeSS::XessOptions::preset.setDeferred(XeSSPreset::Balanced);
          RtxOptions::nisPreset.setDeferred(NisPreset::Balanced);
          RtxOptions::taauPreset.setDeferred(TaauPreset::Balanced);
        } else if (archId < NV_GPU_ARCHITECTURE_GB200) {
          // Ada
          Logger::info("NVIDIA Ada architecture detected, setting default graphics settings to High");
          preferredDefault = GraphicsPreset::High;
          RtxOptions::qualityDLSS.setDeferred(DLSSProfile::Auto);
          DxvkXeSS::XessOptions::preset.setDeferred(XeSSPreset::Quality);
          RtxOptions::nisPreset.setDeferred(NisPreset::Quality);
          RtxOptions::taauPreset.setDeferred(TaauPreset::Quality);
        } else {
          // Blackwell and beyond
          Logger::info("NVIDIA Blackwell architecture detected, setting default graphics settings to Ultra");
          preferredDefault = GraphicsPreset::Ultra;
          RtxOptions::qualityDLSS.setDeferred(DLSSProfile::Auto);
          DxvkXeSS::XessOptions::preset.setDeferred(XeSSPreset::Quality);
          RtxOptions::nisPreset.setDeferred(NisPreset::Quality);
          RtxOptions::taauPreset.setDeferred(TaauPreset::Quality);
        }

        preferredUpscaler = UpscalerType::DLSS;
      } else if (vendorID == static_cast<uint32_t>(DxvkGpuVendor::Amd)) {
        // AMD preset: VRAM-based tier selection.
        // RDNA 3 (RX 7900 XT/XTX) = 20-24 GB, RDNA 2 (6800+) = 16 GB,
        // mid-range (6700/7600) = 8-12 GB, budget = 4-8 GB.
        VkPhysicalDeviceMemoryProperties amdMemProps = device->adapter()->memoryProperties();
        VkDeviceSize amdVidMem = 0;
        for (uint32_t i = 0; i < amdMemProps.memoryTypeCount; i++) {
          if (amdMemProps.memoryTypes[i].propertyFlags == VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) {
            amdVidMem = amdMemProps.memoryHeaps[amdMemProps.memoryTypes[i].heapIndex].size;
            break;
          }
        }

        if (amdVidMem >= 16ull * 1024 * 1024 * 1024) {
          Logger::info(str::format("AMD GPU detected with ", amdVidMem / (1024*1024), " MB VRAM, setting preset to Medium"));
          preferredDefault = GraphicsPreset::Medium;
          DxvkXeSS::XessOptions::preset.setDeferred(XeSSPreset::Quality);
          RtxOptions::nisPreset.setDeferred(NisPreset::Quality);
          RtxOptions::taauPreset.setDeferred(TaauPreset::Balanced);
        } else if (amdVidMem >= 8ull * 1024 * 1024 * 1024) {
          Logger::info(str::format("AMD GPU detected with ", amdVidMem / (1024*1024), " MB VRAM, setting preset to Low"));
          preferredDefault = GraphicsPreset::Low;
          DxvkXeSS::XessOptions::preset.setDeferred(XeSSPreset::Balanced);
          RtxOptions::nisPreset.setDeferred(NisPreset::Balanced);
          RtxOptions::taauPreset.setDeferred(TaauPreset::Performance);
        } else {
          Logger::info(str::format("AMD GPU detected with ", amdVidMem / (1024*1024), " MB VRAM (budget), setting preset to Low"));
          preferredDefault = GraphicsPreset::Low;
          DxvkXeSS::XessOptions::preset.setDeferred(XeSSPreset::Performance);
          RtxOptions::nisPreset.setDeferred(NisPreset::Performance);
          RtxOptions::taauPreset.setDeferred(TaauPreset::Performance);
          // Budget AMD: halve ray-trace resolution to avoid TDR on low-end cards.
          RtxOptions::resolutionScale.setDeferred(0.5f);
        }

        preferredUpscaler = UpscalerType::XeSS;
      } else if (vendorID == static_cast<uint32_t>(DxvkGpuVendor::Intel)) {
        // Intel Arc preset: VRAM-based tier selection.
        // Arc A770/B580 = 16 GB, A750 = 8 GB, A380 = 6 GB.
        VkPhysicalDeviceMemoryProperties intelMemProps = device->adapter()->memoryProperties();
        VkDeviceSize intelVidMem = 0;
        for (uint32_t i = 0; i < intelMemProps.memoryTypeCount; i++) {
          if (intelMemProps.memoryTypes[i].propertyFlags == VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) {
            intelVidMem = intelMemProps.memoryHeaps[intelMemProps.memoryTypes[i].heapIndex].size;
            break;
          }
        }

        if (intelVidMem >= 12ull * 1024 * 1024 * 1024) {
          Logger::info(str::format("Intel Arc GPU detected with ", intelVidMem / (1024*1024), " MB VRAM, setting preset to Medium"));
          preferredDefault = GraphicsPreset::Medium;
          DxvkXeSS::XessOptions::preset.setDeferred(XeSSPreset::Quality);
          RtxOptions::nisPreset.setDeferred(NisPreset::Quality);
          RtxOptions::taauPreset.setDeferred(TaauPreset::Balanced);
        } else if (intelVidMem >= 8ull * 1024 * 1024 * 1024) {
          Logger::info(str::format("Intel Arc GPU detected with ", intelVidMem / (1024*1024), " MB VRAM, setting preset to Low"));
          preferredDefault = GraphicsPreset::Low;
          DxvkXeSS::XessOptions::preset.setDeferred(XeSSPreset::Balanced);
          RtxOptions::nisPreset.setDeferred(NisPreset::Balanced);
          RtxOptions::taauPreset.setDeferred(TaauPreset::Performance);
        } else {
          Logger::info(str::format("Intel GPU detected with ", intelVidMem / (1024*1024), " MB VRAM (budget), setting preset to Low"));
          preferredDefault = GraphicsPreset::Low;
          DxvkXeSS::XessOptions::preset.setDeferred(XeSSPreset::Performance);
          RtxOptions::nisPreset.setDeferred(NisPreset::Performance);
          RtxOptions::taauPreset.setDeferred(TaauPreset::Performance);
        }

        preferredUpscaler = UpscalerType::XeSS;
        // Intel Arc lacks dedicated RT hardware bandwidth — halve the ray-trace resolution
        // so the GPU renders at 50% pixels and XeSS upscales back to display res.
        // This is a permanent code default, not a config file setting.
        RtxOptions::resolutionScale.setDeferred(0.5f);
      } else {
        // Unknown vendor — safe defaults
        Logger::info("Unknown GPU vendor detected, setting default graphics settings to Low with automatic upscaler fallback");
        preferredDefault = GraphicsPreset::Low;

        DxvkXeSS::XessOptions::preset.setDeferred(XeSSPreset::Balanced);
        RtxOptions::nisPreset.setDeferred(NisPreset::Performance);
        RtxOptions::taauPreset.setDeferred(TaauPreset::Balanced);
        preferredUpscaler = UpscalerType::XeSS;
      }

      const UpscalerType resolvedUpscaler = getSupportedUpscalerForDevice(device, preferredUpscaler);
      if (resolvedUpscaler != preferredUpscaler) {
        Logger::info(str::format("RTX: preferred auto upscaler unsupported, falling back from ", static_cast<int>(preferredUpscaler), " to ", static_cast<int>(resolvedUpscaler)));
      }
      RtxOptions::upscalerType.setImmediately(resolvedUpscaler, RtxOptionLayer::getDerivedLayer());

      // figure out how much vidmem we have
      VkPhysicalDeviceMemoryProperties memProps = device->adapter()->memoryProperties();
      VkDeviceSize vidMemSize = 0;
      for (uint32_t i = 0; i < memProps.memoryTypeCount; i++) {
        if (memProps.memoryTypes[i].propertyFlags == VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) {
          vidMemSize = memProps.memoryHeaps[memProps.memoryTypes[i].heapIndex].size;
          break;
        }
      }

      // for 8GB GPUs we lower the quality even further.
      if (vidMemSize <= 8ull * 1024 * 1024 * 1024) {
        Logger::info("8GB GPU detected, lowering quality setting.");
        preferredDefault = (GraphicsPreset)std::clamp((int)preferredDefault + 1, (int) GraphicsPreset::Medium, (int) GraphicsPreset::Low);
        RtxOptions::lowMemoryGpu.setDeferred(true);
      } else {
        RtxOptions::lowMemoryGpu.setDeferred(false);
      }

      // Apply stability mode settings for improved compatibility across all engines
      if (RtxOptions::enableStabilityMode()) {
        Logger::info("Stability mode enabled, applying compatibility settings");
        
        // Reduce ray tracing resolution for better stability
        if (RtxOptions::resolutionScale() > 0.75f) {
          RtxOptions::resolutionScale.setDeferred(0.75f);
        }
        
        // Reduce path bounces for better performance on complex scenes
        if (RtxOptions::pathMaxBounces() > 4) {
          RtxOptions::pathMaxBounces.setDeferred(4);
        }
        
        // Enable conservative RTXDI settings for stability
        DxvkRtxdiRayQuery::enableDenoiserConfidence.setDeferred(true);
        DxvkRtxdiRayQuery::enableBestLightSampling.setDeferred(false);
        DxvkRtxdiRayQuery::initialSampleCount.setDeferred(2);
        DxvkRtxdiRayQuery::spatialSamples.setDeferred(1);
        DxvkRtxdiRayQuery::disocclusionSamples.setDeferred(1);
        
        // For CPU-bound scenarios, reduce geometry processing overhead
        // This helps with games that have complex geometry or many draw calls
        // NOTE: enableAsyncAssetLoading option does not exist, so this line is removed.
        
        // Enable shader compilation optimization for better CPU performance
        RtxOptions::Shader::enableAsyncCompilation.setDeferred(true);
        RtxOptions::Shader::maxConcurrentShaderCompilations.setDeferred(4);
      }

      {
        RtxOptionLayerTarget userTarget(RtxOptionEditTarget::User);
        RtxOptions::graphicsPreset.setImmediately(preferredDefault, RtxOptionLayer::getUserLayer());
      }
      effectiveGraphicsPreset = RtxOptions::graphicsPreset();
    }

    auto common = device->getCommon();
    DxvkPostFx& postFx = common->metaPostFx();
    DxvkRtxdiRayQuery& rtxdiRayQuery = common->metaRtxdiRayQuery();
    DxvkReSTIRGIRayQuery& restirGiRayQuery = common->metaReSTIRGIRayQuery();

    // Handle Graphics Presets
    bool isRayReconstruction = RtxOptions::isRayReconstructionEnabled();

    auto lowGraphicsPresetCommonSettings = [&]() {
      pathMinBounces.setDeferred(0);
      pathMaxBounces.setDeferred(2);
      enableTransmissionApproximationInIndirectRays.setDeferred(true);
      enableUnorderedEmissiveParticlesInIndirectRays.setDeferred(false);
      denoiseDirectAndIndirectLightingSeparately.setDeferred(false);
      enableUnorderedResolveInIndirectRays.setDeferred(false);
      NeeCachePass::enable.setDeferred(isRayReconstruction);
      rtxdiRayQuery.enableRayTracedBiasCorrection.setDeferred(false);
      restirGiRayQuery.biasCorrectionMode.setDeferred(ReSTIRGIBiasCorrection::BRDF);
      restirGiRayQuery.useReflectionReprojection.setDeferred(false);
      common->metaComposite().enableStochasticAlphaBlend.setDeferred(false);
      postFx.enable.setDeferred(false);
    };

    auto enableNrcPreset = [&](NeuralRadianceCache::QualityPreset nrcPreset) {
      const IntegrateIndirectMode resolvedMode = getSupportedIntegrateIndirectMode(device, IntegrateIndirectMode::NeuralRadianceCache);
      // TODO[REMIX-4105] trying to use NRC for a frame when it isn't supported will cause a crash, so this needs to be setImmediately.
      // Should refactor this to use a separate global for the final state, and indicate user preference with the option.
      RtxOptions::integrateIndirectMode.setImmediately(resolvedMode, RtxOptionLayer::getQualityLayer());
      if (RtxOptions::integrateIndirectMode() != resolvedMode) {
        RtxOptions::integrateIndirectMode.clearFromStrongerLayers(RtxOptionLayer::getQualityLayer());
        RtxOptions::integrateIndirectMode.setImmediately(resolvedMode, RtxOptionLayer::getQualityLayer());
      }

      if (resolvedMode == IntegrateIndirectMode::NeuralRadianceCache) {
        NeuralRadianceCache& nrc = device->getCommon()->metaNeuralRadianceCache();
        nrc.setQualityPreset(nrcPreset);
      }
    };

    auto enableLowCostIndirectPreset = [&]() {
      RtxOptions::integrateIndirectMode.setImmediately(IntegrateIndirectMode::ImportanceSampled, RtxOptionLayer::getQualityLayer());
      if (RtxOptions::integrateIndirectMode() != IntegrateIndirectMode::ImportanceSampled) {
        Logger::info("[RtxOptions] integrateIndirectMode low-cost preset is blocked by a stronger layer. Clearing stronger layers and retrying.");
        RtxOptions::integrateIndirectMode.clearFromStrongerLayers(RtxOptionLayer::getQualityLayer());
        RtxOptions::integrateIndirectMode.setImmediately(IntegrateIndirectMode::ImportanceSampled, RtxOptionLayer::getQualityLayer());
      }
    };

    // If Auto was not resolved by updateGraphicsPresets(), fall back to a
    // safe default rather than crashing.  This can happen if the config file
    // contains an invalid value or resolution runs before the GPU is known.
    Logger::info(str::format("[RtxOptions] graphicsPreset after auto block = ", (int)effectiveGraphicsPreset));
    if (effectiveGraphicsPreset == GraphicsPreset::Auto) {
      Logger::info("RtxOptions: graphicsPreset is still Auto after resolution; defaulting to Low.");
      // Write to User layer (matching the auto-resolution block above) so
      // the value isn't shadowed by a lower-priority layer's Auto default.
      {
        RtxOptionLayerTarget userTarget(RtxOptionEditTarget::User);
        graphicsPreset.clearFromStrongerLayers(RtxOptionLayer::getUserLayer());
        graphicsPreset.setImmediately(GraphicsPreset::Low, RtxOptionLayer::getUserLayer());
      }

      effectiveGraphicsPreset = graphicsPreset();
      if (effectiveGraphicsPreset == GraphicsPreset::Auto) {
        Logger::info("[RtxOptions] graphicsPreset write is still blocked after fallback. Applying Low preset settings using a local effective preset for this run.");
        effectiveGraphicsPreset = GraphicsPreset::Low;
      } else {
        Logger::info(str::format("[RtxOptions] graphicsPreset after fallback write = ", (int)effectiveGraphicsPreset));
      }
    }

    RtxGlobalVolumetrics& volumetrics = device->getCommon()->metaGlobalVolumetrics();

    if (effectiveGraphicsPreset == GraphicsPreset::Ultra) {
      pathMinBounces.setDeferred(1);
      pathMaxBounces.setDeferred(4);
      enableTransmissionApproximationInIndirectRays.setDeferred(false);
      enableUnorderedEmissiveParticlesInIndirectRays.setDeferred(true);
      denoiseDirectAndIndirectLightingSeparately.setDeferred(true);
      enableUnorderedResolveInIndirectRays.setDeferred(true);
      NeeCachePass::enable.setDeferred(true);

      russianRouletteMaxContinueProbability.setDeferred(0.9f);
      russianRoulette1stBounceMinContinueProbability.setDeferred(0.6f);

      rtxdiRayQuery.enableRayTracedBiasCorrection.setDeferred(true);
      restirGiRayQuery.biasCorrectionMode.setDeferred(ReSTIRGIBiasCorrection::PairwiseRaytrace);
      restirGiRayQuery.useReflectionReprojection.setDeferred(true);
      common->metaComposite().enableStochasticAlphaBlend.setDeferred(true);
      postFx.enable.setDeferred(true);

      volumetrics.setQualityLevel(RtxGlobalVolumetrics::Ultra);
      enableNrcPreset(NeuralRadianceCache::QualityPreset::Ultra);

      DxvkRayReconstruction::model.setDeferred(DxvkRayReconstruction::RayReconstructionModel::Transformer);
    } else if (effectiveGraphicsPreset == GraphicsPreset::High) {
      pathMinBounces.setDeferred(0);
      pathMaxBounces.setDeferred(2);
      enableTransmissionApproximationInIndirectRays.setDeferred(true);
      enableUnorderedEmissiveParticlesInIndirectRays.setDeferred(false);
      denoiseDirectAndIndirectLightingSeparately.setDeferred(false);
      enableUnorderedResolveInIndirectRays.setDeferred(true);
      NeeCachePass::enable.setDeferred(isRayReconstruction);

      rtxdiRayQuery.enableRayTracedBiasCorrection.setDeferred(true);
      restirGiRayQuery.biasCorrectionMode.setDeferred(ReSTIRGIBiasCorrection::PairwiseRaytrace);
      restirGiRayQuery.useReflectionReprojection.setDeferred(true);
      common->metaComposite().enableStochasticAlphaBlend.setDeferred(true);
      postFx.enable.setDeferred(true);

      russianRouletteMaxContinueProbability.setDeferred(0.9f);
      russianRoulette1stBounceMinContinueProbability.setDeferred(0.6f);

      volumetrics.setQualityLevel(RtxGlobalVolumetrics::High);
      enableNrcPreset(NeuralRadianceCache::QualityPreset::High);

      DxvkRayReconstruction::model.setDeferred(DxvkRayReconstruction::RayReconstructionModel::Transformer);
    } else if (effectiveGraphicsPreset == GraphicsPreset::Medium) {
      lowGraphicsPresetCommonSettings();

      russianRouletteMaxContinueProbability.setDeferred(0.7f);
      russianRoulette1stBounceMinContinueProbability.setDeferred(0.4f);

      volumetrics.setQualityLevel(RtxGlobalVolumetrics::Medium);
      enableNrcPreset(NeuralRadianceCache::QualityPreset::Medium);

      DxvkRayReconstruction::model.setDeferred(DxvkRayReconstruction::RayReconstructionModel::CNN);
    } else if (effectiveGraphicsPreset == GraphicsPreset::Low) {
      lowGraphicsPresetCommonSettings();

      pathMaxBounces.setDeferred(1);
      resolutionScale.setDeferred(0.5f);

      russianRouletteMaxContinueProbability.setDeferred(0.7f);
      russianRoulette1stBounceMinContinueProbability.setDeferred(0.4f);

      volumetrics.setQualityLevel(RtxGlobalVolumetrics::Low);
      enableLowCostIndirectPreset();

      DxvkRayReconstruction::model.setDeferred(DxvkRayReconstruction::RayReconstructionModel::CNN);
    }

    // DLSS quality was set per-arch in the auto-detection block above.
    // Only set Auto as fallback if the user explicitly selected Custom DLSS preset
    // and hasn't provided their own qualityDLSS value.
    if (dlssPreset() == DlssPreset::Custom && upscalerType() == UpscalerType::DLSS) {
      qualityDLSS.setDeferred(DLSSProfile::Auto);
    }

    // else Graphics Preset == Custom
    updateLightingSetting();
  }

  void RtxOptions::updateRaytraceModePresets(const uint32_t vendorID, const VkDriverId driverID) {
    // Handle Automatic Raytrace Mode Preset (From configuration/default)

    if (RtxOptions::raytraceModePreset() == RaytraceModePreset::Auto) {
      Logger::info("Automatic Raytrace Mode Preset in use (Set rtx.raytraceModePreset to something other than Auto use a non-automatic preset)");

      auto applyAutoQualityRaytraceMode = [](auto& option, auto preferredValue, const char* optionName) {
        option.setImmediately(preferredValue, RtxOptionLayer::getQualityLayer());

        if (option() == preferredValue)
          return;

        Logger::warn(str::format(
          "[RtxOptions] ", optionName,
          " auto-resolution is blocked by a stronger layer. Clearing stronger layers and retrying."));
        option.clearFromStrongerLayers(RtxOptionLayer::getQualityLayer());
        option.setImmediately(preferredValue, RtxOptionLayer::getQualityLayer());

        if (option() != preferredValue) {
          // Fallback: Quality layer write failed even after retry.
          // Write to the Default layer instead, which is always present and has the lowest
          // priority — but since no stronger layer holds a conflicting value (we just cleared
          // them), the Default layer value will become the resolved value.
          Logger::warn(str::format(
            "[RtxOptions] ", optionName,
            " Quality layer write failed after retry (current value=",
            static_cast<int>(option()),
            "). Falling back to Default layer write."));

          option.setImmediately(preferredValue, RtxOptionLayer::getDefaultLayer());

          if (option() == preferredValue) {
            Logger::info(str::format(
              "[RtxOptions] ", optionName,
              " successfully set via Default layer fallback. Resolved value=",
              static_cast<int>(option())));
          } else {
            // Last resort: try the Derived layer
            Logger::warn(str::format(
              "[RtxOptions] ", optionName,
              " Default layer fallback also failed. Trying Derived layer."));

            option.setImmediately(preferredValue, RtxOptionLayer::getDerivedLayer());

            if (option() == preferredValue) {
              Logger::info(str::format(
                "[RtxOptions] ", optionName,
                " successfully set via Derived layer fallback. Resolved value=",
                static_cast<int>(option())));
            } else {
              Logger::err(str::format(
                "[RtxOptions] ", optionName,
                " all fallback attempts failed. Auto-detected raytrace mode could not be applied."
                " Current resolved value=", static_cast<int>(option()),
                ", expected=", static_cast<int>(preferredValue)));
            }
          }
        }
      };

      // Note: Left undefined as these values are initialized in all paths.
      DxvkPathtracerGbuffer::RaytraceMode preferredGBufferRaytraceMode;
      DxvkPathtracerIntegrateDirect::RaytraceMode preferredIntegrateDirectRaytraceMode;
      DxvkPathtracerIntegrateIndirect::RaytraceMode preferredIntegrateIndirectRaytraceMode;

      preferredGBufferRaytraceMode = DxvkPathtracerGbuffer::RaytraceMode::RayQuery;
      preferredIntegrateDirectRaytraceMode = DxvkPathtracerIntegrateDirect::RaytraceMode::RayQuery;

      if (vendorID == static_cast<uint32_t>(DxvkGpuVendor::Nvidia) || driverID == VK_DRIVER_ID_MESA_RADV) {
        // Default to a mixture of Trace Ray and Ray Query on NVIDIA and RADV
        if (driverID == VK_DRIVER_ID_MESA_RADV) {
          Logger::info("RADV driver detected, setting default raytrace modes to Trace Ray (Indirect Integrate) and Ray Query (GBuffer, Direct Integrate)");
        } else {
          Logger::info("NVIDIA architecture detected, setting default raytrace modes to Trace Ray (Indirect Integrate) and Ray Query (GBuffer, Direct Integrate)");
        }

        preferredIntegrateIndirectRaytraceMode = DxvkPathtracerIntegrateIndirect::RaytraceMode::TraceRay;
      } else if (vendorID == static_cast<uint32_t>(DxvkGpuVendor::Amd)) {
        // AMD proprietary driver: Ray Query is more stable than TraceRay
        Logger::info("AMD GPU detected (proprietary driver), setting default raytrace modes to Ray Query");

        preferredIntegrateIndirectRaytraceMode = DxvkPathtracerIntegrateIndirect::RaytraceMode::RayQuery;
      } else {
        // Intel and other vendors: default to Ray Query
        Logger::info("Non-NVIDIA/non-AMD architecture detected, setting default raytrace modes to Ray Query");

        preferredIntegrateIndirectRaytraceMode = DxvkPathtracerIntegrateIndirect::RaytraceMode::RayQuery;
      }

      applyAutoQualityRaytraceMode(RtxOptions::renderPassGBufferRaytraceMode, preferredGBufferRaytraceMode, "renderPassGBufferRaytraceMode");
      applyAutoQualityRaytraceMode(RtxOptions::renderPassIntegrateDirectRaytraceMode, preferredIntegrateDirectRaytraceMode, "renderPassIntegrateDirectRaytraceMode");
      applyAutoQualityRaytraceMode(RtxOptions::renderPassIntegrateIndirectRaytraceMode, preferredIntegrateIndirectRaytraceMode, "renderPassIntegrateIndirectRaytraceMode");
    }
  }

  void RtxOptions::detectEngineAndApplySettings(DxvkDevice* device) {
    // Auto-detect game engine and apply appropriate settings
    // This helps with compatibility across different engines and emulators
    
    // Get the current executable name for engine detection
    std::string exeName = std::filesystem::path(getExePath()).filename().string();
    std::transform(exeName.begin(), exeName.end(), exeName.begin(), ::tolower);
    
    // Detect common engines and apply engine-specific settings
    bool isUnity = false;
    bool isUnreal = false;
    bool isSource2 = false;
    bool isL4D2 = false;
    
    // Check for Unity engine
    if (exeName.find("unity") != std::string::npos) {
      isUnity = true;
      Logger::info("[RtxOptions] Unity engine detected, applying Unity-specific settings");
    }
    
    // Check for Unreal Engine
    if (exeName.find("unreal") != std::string::npos || 
        exeName.find("ue4") != std::string::npos ||
        exeName.find("ue5") != std::string::npos) {
      isUnreal = true;
      Logger::info("[RtxOptions] Unreal Engine detected, applying Unreal-specific settings");
    }
    
    // Check for Source 2 engine (e.g., Left 4 Dead 2)
    if (exeName.find("left4dead2") != std::string::npos ||
        exeName.find("l4d2") != std::string::npos) {
      isL4D2 = true;
      isSource2 = true;
      Logger::info("[RtxOptions] Source 2 engine (Left 4 Dead 2) detected, applying Source 2-specific settings");
    }
    
    // Apply engine-specific settings
    if (isUnity) {
      // Unity-specific settings
      // Unity often uses different UV coordinate systems
      // Enable UV correction for Unity
      RtxOptions::enableUvCorrection.setDeferred(true, RtxOptionLayer::getDerivedLayer());
    }
    
    if (isUnreal) {
      // Unreal-specific settings
      // Unreal Engine has specific texture formats and hashing
      RtxOptions::enableUnrealTextureFixes.setDeferred(true, RtxOptionLayer::getDerivedLayer());
    }
    
    if (isSource2) {
      // Source 2-specific settings
      // Source 2 has specific geometry and texture handling
      RtxOptions::enableSource2Fixes.setDeferred(true, RtxOptionLayer::getDerivedLayer());
      
      // Source 2 often uses specific texture formats - apply fixes
      Logger::info("[RtxOptions] Source 2 engine detected, applying texture format fixes");
    }
    
    // Apply general engine-agnostic settings for stability
    // These settings work across all engines and improve stability
    RtxOptions::enableStabilityMode.setDeferred(true, RtxOptionLayer::getDerivedLayer());
    
    Logger::info("[RtxOptions] Engine detection complete. Engine-specific settings applied.");
  }

  void RtxOptions::resetUpscaler() {
    // Code-driven changes for upscaler reset (automatically routes to User layer when preset is Custom)
    RtxOptionLayerTarget layerTarget(RtxOptionEditTarget::Derived);
    
    RtxOptions::upscalerType.setDeferred(UpscalerType::DLSS);
    reflexMode.setDeferred(ReflexMode::LowLatency);
  }

  std::string RtxOptions::getCurrentDirectory() {
    return std::filesystem::current_path().string();
  }

  std::string RtxOptions::getExePath() {
    char path[MAX_PATH];
    HMODULE hModule = nullptr;
    
    // Get the module handle for the current process
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          reinterpret_cast<LPCSTR>(&getExePath), &hModule) != 0) {
      if (GetModuleFileNameA(hModule, path, MAX_PATH) > 0) {
        return std::string(path);
      }
    }
    
    // Fallback: return current directory
    return getCurrentDirectory();
  }

  bool RtxOptions::needsMeshBoundingBox() {
    return AntiCulling::isObjectAntiCullingEnabled() ||
           AntiCulling::isLightAntiCullingEnabled() ||
           TerrainBaker::needsTerrainBaking() ||
           enableAlwaysCalculateAABB() ||
           NeeCachePass::enable();
  }

  void RtxOptions::resolveTransparencyThresholdOnChange(DxvkDevice* device) {
    // Adjust valid range on resolveOpaquenessThreshold to prevent value below resolveTransparencyThreshold
    resolveOpaquenessThresholdObject().setMinValue(resolveTransparencyThreshold());
  }

  void RtxOptions::pathMinBouncesOnChange(DxvkDevice* device) {
    // Adjust valid range on pathMaxBounces to prevent value below pathMinBounces
    pathMaxBouncesObject().setMinValue(pathMinBounces());
  }

  void RtxOptions::pathMaxBouncesOnChange(DxvkDevice* device) {
    // Adjust valid range on pathMinBounces to prevent value above pathMaxBounces
    pathMinBouncesObject().setMaxValue(pathMaxBounces());
  }

  void RtxOptions::rayPortalModelTextureHashesOnChange(DxvkDevice* device) {
    // Ensure the Ray Portal texture hashes are always in pairs of 2 and don't exceed maxRayPortalCount
    std::vector<XXH64_hash_t> trimmedHashes = rayPortalModelTextureHashes();
    
    // Must be a multiple of 2
    if (trimmedHashes.size() % 2 == 1) {
      trimmedHashes.pop_back();
    }
    
    // Must not exceed maxRayPortalCount
    if (trimmedHashes.size() > maxRayPortalCount) {
      trimmedHashes.erase(trimmedHashes.begin() + maxRayPortalCount, trimmedHashes.end());
    }
    
    // Only update if the value changed
    if (trimmedHashes != rayPortalModelTextureHashes()) {
      rayPortalModelTextureHashesObject().setDeferred(trimmedHashes);
    }
  }

  void RtxOptions::geometryGenerationHashRuleStringOnChange(DxvkDevice* device) {
    s_geometryHashGenerationRule = createRule("Geometry generation", geometryGenerationHashRuleString());
  }

  void RtxOptions::geometryAssetHashRuleStringOnChange(DxvkDevice* device) {
    s_geometryAssetHashRule = createRule("Geometry asset", geometryAssetHashRuleString());
  }

  void RtxOptions::rayPortalSamplingWeightMinDistanceOnChange(DxvkDevice* device) {
    // Adjust valid range on rayPortalSamplingWeightMaxDistance to prevent value below min distance
    rayPortalSamplingWeightMaxDistanceObject().setMinValue(rayPortalSamplingWeightMinDistance());
  }

  void RtxOptions::rayPortalSamplingWeightMaxDistanceOnChange(DxvkDevice* device) {
    // Adjust valid range on rayPortalSamplingWeightMinDistance to prevent value above max distance
    rayPortalSamplingWeightMinDistanceObject().setMaxValue(rayPortalSamplingWeightMaxDistance());
  }

  void ViewDistanceOptions::distanceFadeMinOnChange(DxvkDevice* device) {
    // Adjust valid range on distanceFadeMax to prevent value below distanceFadeMin
    distanceFadeMaxObject().setMinValue(distanceFadeMin());
  }

  void ViewDistanceOptions::distanceFadeMaxOnChange(DxvkDevice* device) {
    // Adjust valid range on distanceFadeMin to prevent value above distanceFadeMax
    distanceFadeMinObject().setMaxValue(distanceFadeMax());
  }
}
