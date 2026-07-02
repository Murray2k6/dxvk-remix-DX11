/*
* Copyright (c) 2022-2025, NVIDIA CORPORATION. All rights reserved.
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
#pragma once

#include "rtx_initializer.h"
#include "rtx_options.h"
#include "../../util/log/log.h"
#include "../../util/util_env.h"
#include "../../util/util_string.h"
#include <cctype>
#include "../../util/thread.h"
#include "dxvk_context.h"
#include "dxvk_device.h"
#include "rtx_shader_manager.h"
#include "rtx_texture_manager.h"
#include "rtx_io.h"
#include "dxvk_raytracing.h"
#include "rtx_debug_view.h"

namespace dxvk {
  RtxInitializer::RtxInitializer(DxvkDevice* device)
  : CommonDeviceObject(device) { 
  }

  void RtxInitializer::initialize() {
    ShaderManager::getInstance()->setDevice(m_device);

#ifdef WITH_RTXIO
    if (RtxIo::enabled()) {
      RtxIo::get().initialize(m_device);
    }
    // start async before starting asset loading
    DxvkObjects* pCommon = m_device->getCommon();
    pCommon->getTextureManager().startAsync();
#endif

    // Initialize RTX settings presets
    // Todo: Improve this preset override functionality [REMIX-1482]
    // Currently this logic is very confusing and is intended to skip preset initialization from overriding options, but only results in weird behavior
    // when a termination frame is not set (due to running a test locally in a more open-ended way), or due to how the ultra preset is being used but
    // it is being treated more as a custom preset in practice (except it's not fully custom either due to other preset initialization happening in dxvk_imgui.cpp,
    // though to be fair this logic is not acutally invoked I think unless the Remix menu is opened, but it still shouldn't be split out like this especially if a user
    // is debugging tests and opens the menu only for all the graphics settings to change).
    // Additionally, skipping this logic skips the DLSS preset initialization which is also probably wrong (though the tests will have to explicitly ask for DLSS
    // to be disabled if this is changed).
    if (env::getEnvVar("DXVK_TERMINATE_APP_FRAME") == "" ||
        env::getEnvVar("DXVK_GRAPHICS_PRESET_TYPE") != "0") {
      const DxvkDeviceInfo& deviceInfo = m_device->adapter()->devicePropertiesExt();

      RtxOptions::updateUpscalerFromDlssPreset();
      RtxOptions::updateGraphicsPresets(m_device);
      RtxOptions::updateRaytraceModePresets(deviceInfo.core.properties.vendorID, deviceInfo.khrDeviceDriverProperties.driverID);
    } else {
      // Default, init to custom unless otherwise specified
      if (RtxOptions::graphicsPreset() == GraphicsPreset::Auto) {
        RtxOptions::graphicsPreset.setDeferred(GraphicsPreset::Custom);
      }

      // Need to initialize DLSS-RR settings in test cases.
      // Warning: this will override multiple global options, including any values set by the test workflow.
      if (env::getEnvVar("DXVK_RAY_RECONSTRUCTION") != "0") {
        RtxOptions::updateLightingSetting();
      }
    }

    // DX11_V228_ALBEDO_SELECTION: the generic albedo texture-selection reinforcement
    // (rtx.enableUnrealTextureFixes - boost strong-albedo mipmapped textures, demote scene/intermediate
    // surfaces) now defaults ON for ANY game/engine, so real textures get picked over the neutral
    // untextured placeholder without per-game config. Detect a few engines from the exe name and enable
    // their additional quirk fixes; log everything for diagnostics. Explicit user/config still wins.
    {
      std::string exeLower = env::getExeName();
      for (char& ch : exeLower) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
      const bool isUnreal = exeLower.find("-shipping") != std::string::npos
                         || exeLower.find("unrealengine") != std::string::npos;
      const bool isSource2 = exeLower.find("source2") != std::string::npos
                          || exeLower.find("dota2") != std::string::npos
                          || exeLower.find("hl_vr") != std::string::npos
                          || exeLower.find("csgo") != std::string::npos
                          || exeLower.find("cs2") != std::string::npos;
      if (isSource2 && !RtxOptions::enableSource2Fixes()) {
        RtxOptions::enableSource2Fixes.setDeferred(true);
      }
      const char* engine = isUnreal ? "Unreal" : (isSource2 ? "Source2" : "generic");
      Logger::info(str::format("[Remix-DX11][init] game='", env::getExeName(), "' engine=", engine,
        " albedoTexFixes=", RtxOptions::enableUnrealTextureFixes() ? "on" : "off",
        " source2Fixes=", (RtxOptions::enableSource2Fixes() || isSource2) ? "on" : "off"));
    }

    // Configure shader manager to understand bindless layouts
    ShaderManager::getInstance()->addGlobalExtraLayout(pCommon->getSceneManager().getBindlessResourceManager().getGlobalBindlessTableLayout(BindlessResourceManager::Buffers));
    ShaderManager::getInstance()->addGlobalExtraLayout(pCommon->getSceneManager().getBindlessResourceManager().getGlobalBindlessTableLayout(BindlessResourceManager::Textures));
    ShaderManager::getInstance()->addGlobalExtraLayout(pCommon->getSceneManager().getBindlessResourceManager().getGlobalBindlessTableLayout(BindlessResourceManager::Samplers));

    // Need to promote all of the hardware support Options before prewarming shaders.
    Logger::info("[Remix-DX11][init] applying pending RtxOptions...");
    RtxOptionManager::applyPendingValues(m_device, /* forceOnChange */ true);

    // Kick off shader prewarming
    Logger::info("[Remix-DX11][init] starting shader prewarm...");
    startPrewarmShaders();

    // Load assets (if any) as early as possible
    Logger::info("[Remix-DX11][init] loading assets...");
    if (RtxOptions::asyncAssetLoading()) {
      // Async asset loading (USD)
      m_asyncAssetLoadThread = dxvk::thread([this] {
        env::setThreadName("rtx-initialize-assets");
        loadAssets();
      });
    } else {
      loadAssets();
    }

    // DX11_V227: The DLSS / DLSS Frame Generation lazy allocators are load-bearing
    // for the present + frame-generation path, so they MUST be constructed here on
    // every GPU. (A previous attempt to skip them on non-NGX GPUs broke device/
    // swapchain initialization across all vendors with error 0x00000008 - reverted.)
    Logger::info("[Remix-DX11][init] initializing DLSS + DLFG lazy allocators...");
    pCommon->metaDLSS(); // Lazy allocator triggers init in ctor
    pCommon->metaDLFG();

    if (!asyncShaderFinalizing()) {
      // Wait for all prewarming to complete before calling "RTX initialized"
      Logger::info("[Remix-DX11][init] waiting for shader prewarm...");
      waitForShaderPrewarm();
    }
    Logger::info("[Remix-DX11][init] RtxInitializer::initialize() complete.");
  }

  void RtxInitializer::release() {
    if (asyncShaderFinalizing()) {
      // Wait for all prewarming to complete 
      waitForShaderPrewarm();
    }

    ShaderManager::destroyInstance();
#ifdef WITH_RTXIO
    RtxIo::get().release();
#endif
  }

  void RtxInitializer::loadAssets() {
    m_assetsLoaded = false;

    Rc<DxvkContext> ctx = m_device->createContext();

    ctx->beginRecording(m_device->createCommandList());

    DxvkObjects* pCommon = m_device->getCommon();
    pCommon->getSceneManager().initialize(ctx);

    ctx->flushCommandList();

    m_assetsLoaded = true;
  }

  void RtxInitializer::startPrewarmShaders() {
    // If we want to run without shader prewarming, then pipelines will be built inline with other GPU work on first use (typically means
    // long stutters whenever a yet to be compiled pipeline comes into use).
    // DX11_V228_CROSS_VENDOR: bulk RT-pipeline shader prewarming crashes/deadlocks at LAUNCH across
    // every GPU vendor in this DX11 fork - AMD: long-standing deadlock (original WAR below); Intel Arc
    // (Battlemage B580) AND NVIDIA: launch crash inside startPrewarmShaders(), confirmed via the
    // [Remix-DX11][init] markers (the init log stops right after "starting shader prewarm..." with no
    // further step). Since all three IHVs fail here, disable prewarming unconditionally; the RT
    // pipelines then compile inline on first use (minor first-use stutter, but the game boots and
    // path tracing runs on any GPU). Re-evaluate per-vendor once the prewarm path is fixed.
    // DX11_V245_NVIDIA_PREWARM: the earlier blanket disable lumped NVIDIA in with the
    // vendors whose prewarm genuinely fails - AMD (long-standing deadlock) and Intel Arc
    // (Battlemage launch crash). NVIDIA prewarms correctly, and it NEEDS prewarm: without
    // it the large RGS ray-tracing pipelines (NVIDIA's default indirect-integrate path)
    // compile INLINE on the first ray-traced frame, causing long stutters and a first-frame
    // crash risk (matches the RTX 4060 / Minecraft crash that lands right at the first RT
    // frame). So prewarm on NVIDIA and keep it disabled on AMD/Intel. Escape hatch:
    // DXVK_REMIX_PREWARM = "0" forces off, "1" forces on, on any vendor.
    const uint32_t vendorId = m_device->properties().core.properties.vendorID;
    const bool prewarmUnsafeVendor =
         vendorId == static_cast<uint32_t>(DxvkGpuVendor::Amd)
      || vendorId == static_cast<uint32_t>(DxvkGpuVendor::Intel);
    bool doPrewarm = !prewarmUnsafeVendor; // NVIDIA (and unknown vendors) prewarm; AMD/Intel do not
    const std::string prewarmOverride = env::getEnvVar("DXVK_REMIX_PREWARM");
    if (prewarmOverride == "0")
      doPrewarm = false;
    else if (prewarmOverride == "1")
      doPrewarm = true;

    if (!asyncShaderPrewarming() || !doPrewarm) {
      Logger::info("[Remix-DX11][init] shader prewarm disabled (AMD deadlock / Intel Arc launch-crash WAR); pipelines compile inline on first use.");
      return;
    }
    Logger::info("[Remix-DX11][init] shader prewarm ENABLED (prewarming RT pipelines up front to avoid first-frame inline-compile stutter/crash).");

    DxvkObjects* pCommon = m_device->getCommon();

    // Prewarm all the shaders we'll need for RT by registering them (per-pass) with the driver
    pCommon->metaPathtracerGbuffer().prewarmShaders(pCommon->pipelineManager());
    pCommon->metaPathtracerIntegrateDirect().prewarmShaders(pCommon->pipelineManager());
    pCommon->metaPathtracerIntegrateIndirect().prewarmShaders(pCommon->pipelineManager());

    pCommon->metaDebugView().prewarmShaders(pCommon->pipelineManager());

    // Prewarm the rest of the pipelines that can be done automatically
    AutoShaderPipelinePrewarmer::prewarmComputePipelines(pCommon->pipelineManager());
  }

  void RtxInitializer::waitForShaderPrewarm() {
    if (m_warmupComplete) {
      return;
    }

    // Wait for all shader prewarming to complete
    while (m_device->getCommon()->pipelineManager().isCompilingShaders()) {
      Sleep(1);
    }

    DxvkRaytracingPipeline::releaseFinalizer();

    m_warmupComplete = true;
  }
}