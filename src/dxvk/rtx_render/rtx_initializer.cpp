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
  namespace {
    class ShaderPrewarmDialog {
    public:
      ShaderPrewarmDialog() = default;
      ShaderPrewarmDialog(const ShaderPrewarmDialog&) = delete;
      ShaderPrewarmDialog& operator=(const ShaderPrewarmDialog&) = delete;

      ~ShaderPrewarmDialog() {
        close();
      }

      void open() {
        if (m_thread.joinable())
          return;

        m_thread = dxvk::thread([this] {
          env::setThreadName("rtx-shader-dialog");
          run();
        });

        const uint64_t waitStart = ::GetTickCount64();
        while (!m_ready.load(std::memory_order_acquire)
            && ::GetTickCount64() - waitStart < 2000u) {
          ::Sleep(1);
        }
      }

      void update(uint32_t pending, uint64_t elapsedMs) {
        m_pending.store(pending, std::memory_order_release);
        m_elapsedMs.store(elapsedMs, std::memory_order_release);

        uint32_t observedMaximum = m_maxPending.load(std::memory_order_acquire);
        while (pending > observedMaximum
            && !m_maxPending.compare_exchange_weak(
                 observedMaximum, pending, std::memory_order_release,
                 std::memory_order_acquire)) {
        }

        const HWND hwnd = m_hwnd.load(std::memory_order_acquire);
        if (hwnd != nullptr)
          ::InvalidateRect(hwnd, nullptr, FALSE);
      }

      void close() {
        const HWND hwnd = m_hwnd.load(std::memory_order_acquire);
        if (hwnd != nullptr)
          ::PostMessageW(hwnd, WM_CLOSE, 0, 0);
        if (m_thread.joinable())
          m_thread.join();
      }

    private:
      static LRESULT CALLBACK windowProc(
          HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
        ShaderPrewarmDialog* self = reinterpret_cast<ShaderPrewarmDialog*>(
          ::GetWindowLongPtrW(hwnd, GWLP_USERDATA));

        if (message == WM_NCCREATE) {
          const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
          self = static_cast<ShaderPrewarmDialog*>(create->lpCreateParams);
          ::SetWindowLongPtrW(
            hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }

        switch (message) {
          case WM_ERASEBKGND:
            return 1;

          case WM_PAINT:
            if (self != nullptr) {
              self->paint(hwnd);
              return 0;
            }
            break;

          case WM_CLOSE:
            ::DestroyWindow(hwnd);
            return 0;

          case WM_NCDESTROY:
            if (self != nullptr)
              self->m_hwnd.store(nullptr, std::memory_order_release);
            ::PostQuitMessage(0);
            return 0;

          default:
            break;
        }

        return ::DefWindowProcW(hwnd, message, wParam, lParam);
      }

      void paint(HWND hwnd) {
        PAINTSTRUCT paint = {};
        HDC dc = ::BeginPaint(hwnd, &paint);
        RECT client = {};
        ::GetClientRect(hwnd, &client);

        HBRUSH background = ::CreateSolidBrush(RGB(28, 30, 34));
        ::FillRect(dc, &client, background);
        ::DeleteObject(background);

        ::SetBkMode(dc, TRANSPARENT);
        ::SetTextColor(dc, RGB(245, 245, 245));

        HFONT titleFont = ::CreateFontW(
          -22, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
          DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
          CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        HFONT bodyFont = ::CreateFontW(
          -16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
          DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
          CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

        HFONT previousFont = static_cast<HFONT>(::SelectObject(dc, titleFont));
        RECT titleRect = { 24, 22, client.right - 24, 54 };
        ::DrawTextW(dc, L"Please wait", -1, &titleRect,
          DT_LEFT | DT_SINGLELINE | DT_VCENTER);

        ::SelectObject(dc, bodyFont);
        ::SetTextColor(dc, RGB(205, 210, 218));
        RECT bodyRect = { 24, 58, client.right - 24, 84 };
        ::DrawTextW(dc, L"RTX Remix is compiling cached game and path-tracing shaders before game initialization.",
          -1, &bodyRect, DT_LEFT | DT_SINGLELINE | DT_VCENTER);

        const uint32_t pending = m_pending.load(std::memory_order_acquire);
        const uint32_t maximum = m_maxPending.load(std::memory_order_acquire);
        const uint64_t elapsed = m_elapsedMs.load(std::memory_order_acquire);
        wchar_t status[128] = {};
        _snwprintf_s(status, _TRUNCATE,
          L"%u shader pipeline%s remaining  |  %llu.%01llu seconds",
          pending, pending == 1u ? L"" : L"s",
          static_cast<unsigned long long>(elapsed / 1000u),
          static_cast<unsigned long long>((elapsed % 1000u) / 100u));
        RECT statusRect = { 24, 91, client.right - 24, 118 };
        ::DrawTextW(dc, status, -1, &statusRect,
          DT_LEFT | DT_SINGLELINE | DT_VCENTER);

        RECT bar = { 24, 129, client.right - 24, 148 };
        HBRUSH barBackground = ::CreateSolidBrush(RGB(61, 65, 72));
        ::FillRect(dc, &bar, barBackground);
        ::DeleteObject(barBackground);

        if (maximum > 0u) {
          const uint32_t completed = maximum > pending ? maximum - pending : 0u;
          RECT fill = bar;
          fill.right = fill.left + static_cast<LONG>(
            (uint64_t(bar.right - bar.left) * completed) / maximum);
          HBRUSH progress = ::CreateSolidBrush(RGB(118, 185, 0));
          ::FillRect(dc, &fill, progress);
          ::DeleteObject(progress);
        }

        ::SelectObject(dc, previousFont);
        ::DeleteObject(titleFont);
        ::DeleteObject(bodyFont);
        ::EndPaint(hwnd, &paint);
      }

      void run() {
        static constexpr wchar_t kClassName[] =
          L"RtxRemixShaderPrewarmDialog";
        const HINSTANCE instance = ::GetModuleHandleW(nullptr);
        WNDCLASSEXW windowClass = {};
        windowClass.cbSize = sizeof(windowClass);
        windowClass.lpfnWndProc = &ShaderPrewarmDialog::windowProc;
        windowClass.hInstance = instance;
        windowClass.hCursor = ::LoadCursorW(nullptr, MAKEINTRESOURCEW(32514));
        windowClass.hbrBackground = nullptr;
        windowClass.lpszClassName = kClassName;
        ::RegisterClassExW(&windowClass);

        constexpr int width = 570;
        constexpr int height = 215;
        const int x = (::GetSystemMetrics(SM_CXSCREEN) - width) / 2;
        const int y = (::GetSystemMetrics(SM_CYSCREEN) - height) / 2;
        const HWND hwnd = ::CreateWindowExW(
          WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
          kClassName,
          L"RTX Remix Shader Compiler",
          WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
          x, y, width, height,
          nullptr, nullptr, instance, this);

        m_hwnd.store(hwnd, std::memory_order_release);
        m_ready.store(true, std::memory_order_release);
        if (hwnd == nullptr)
          return;

        ::ShowWindow(hwnd, SW_SHOWNORMAL);
        ::UpdateWindow(hwnd);

        MSG message = {};
        while (::GetMessageW(&message, nullptr, 0, 0) > 0) {
          ::TranslateMessage(&message);
          ::DispatchMessageW(&message);
        }
      }

      std::atomic<HWND> m_hwnd { nullptr };
      std::atomic<bool> m_ready { false };
      std::atomic<uint32_t> m_pending { 0u };
      std::atomic<uint32_t> m_maxPending { 0u };
      std::atomic<uint64_t> m_elapsedMs { 0u };
      dxvk::thread m_thread;
    };
  }

  RtxInitializer::RtxInitializer(DxvkDevice* device)
  : CommonDeviceObject(device) { 
  }

  void RtxInitializer::initialize() {
    // DX11_V267_BOOT_MARKER: the very first line of Remix initialization at
    // game boot. Everything before the first pre-existing [init] line (RTXIO
    // init, texture-manager async start, graphics-preset device queries) ran
    // silently - a boot that froze or crashed in that window showed nothing
    // from the initializer at all. With this marker plus the step lines
    // below, a hung-boot log pinpoints the exact last-completed step.
    Logger::info(str::format("[Remix-DX11][init] BEGIN RtxInitializer::initialize() game='",
      env::getExeName(), "' pid=", GetCurrentProcessId()));

    ShaderManager::getInstance()->setDevice(m_device);

#ifdef WITH_RTXIO
    if (RtxIo::enabled()) {
      Logger::info("[Remix-DX11][init] initializing RTXIO...");
      RtxIo::get().initialize(m_device);
    }
    // start async before starting asset loading
    Logger::info("[Remix-DX11][init] starting texture manager async worker...");
    DxvkObjects* pCommon = m_device->getCommon();
    pCommon->getTextureManager().startAsync();
#endif

    Logger::info("[Remix-DX11][init] applying graphics/upscaler/raytrace-mode presets...");

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
    // surfaces) now defaults ON for ANY game/engine, so real textures are selected when they are
    // actually bound while genuinely untextured geometry remains untextured. Detect a few engines
    // from the exe name and enable their additional quirk fixes; log everything for diagnostics.
    // Explicit user/config still wins.
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
    const bool prewarmStarted = startPrewarmShaders();

    if (prewarmStarted && RtxOptions::Shader::waitForPrewarmOnBoot()) {
      Logger::info("[Remix-DX11][init] waiting for boot shader prewarm before game initialization...");
      waitForShaderPrewarm(RtxOptions::Shader::showPrewarmDialog());
    }

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
      waitForShaderPrewarm(false);
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

  bool RtxInitializer::startPrewarmShaders() {
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
    // DX11_V275_NO_PREWARM_BY_DEFAULT: prewarm registration (below) can hang or
    // crash SYNCHRONOUSLY at launch - the init log stops right after "starting
    // shader prewarm" with no further step. This is the "game only stays in
    // Task Manager and never boots" / launch-freeze report, and it has now been
    // seen on all three vendors (AMD deadlock, Intel Arc crash, NVIDIA hang),
    // not just AMD/Intel. Since V248 made the RT pipelines compile ASYNC +
    // NON-BLOCKING on first use (they enqueue to the state-cache workers and
    // the frame simply skips RT until each pipeline is ready - no inline stall,
    // no first-frame crash, which V244 NRC-opt-in also addressed), prewarm is
    // no longer needed to avoid first-frame stutter. Default it OFF on EVERY
    // vendor so the game ALWAYS boots ("any game must work"); pipelines warm up
    // in the background and RT engages once they are ready. Re-enable for
    // benchmarking / prewarm testing with env DXVK_REMIX_PREWARM=1.
    const bool doPrewarm = RtxOptions::Shader::prewarmOnBoot();

    if (!asyncShaderPrewarming() || !doPrewarm) {
      Logger::info("[Remix-DX11][init] shader prewarm disabled by configuration; pipelines will compile asynchronously on first use.");
      return false;
    }
    Logger::info(str::format(
      "[Remix-DX11][init] shader prewarm ENABLED in game process after launcher handoff (allVariants=",
      RtxOptions::Shader::prewarmAllVariants() ? 1 : 0,
      ", waitingBeforeGameInit=",
      RtxOptions::Shader::waitForPrewarmOnBoot() ? 1 : 0, ")."));

    DxvkObjects* pCommon = m_device->getCommon();

    // Prewarm all the shaders we'll need for RT by registering them (per-pass) with the driver
    pCommon->metaPathtracerGbuffer().prewarmShaders(pCommon->pipelineManager());
    pCommon->metaPathtracerIntegrateDirect().prewarmShaders(pCommon->pipelineManager());
    pCommon->metaPathtracerIntegrateIndirect().prewarmShaders(pCommon->pipelineManager());

    pCommon->metaDebugView().prewarmShaders(pCommon->pipelineManager());

    // Prewarm the rest of the pipelines that can be done automatically
    AutoShaderPipelinePrewarmer::prewarmComputePipelines(pCommon->pipelineManager());

    Logger::info(str::format(
      "[Remix-DX11][init] shader prewarm registration complete; pendingPipelines=",
      pCommon->pipelineManager().remixShaderCompilationCount()));
    return true;
  }

  void RtxInitializer::waitForShaderPrewarm(bool showProgressDialog) {
    if (m_warmupComplete) {
      return;
    }

    // Full-variant prewarming can legitimately take several minutes on an empty
    // driver cache. A fixed total timeout released the game halfway through that
    // work, contradicting the all-variants setting and moving the remaining
    // stalls into gameplay. Wait for the Remix count to reach zero as long as it
    // is making progress. A separate no-progress timeout still protects launch
    // from a genuinely wedged driver/compiler without penalizing slow machines.
    constexpr uint64_t kRegularPrewarmTimeoutMs = 120000;
    constexpr uint64_t kNoProgressTimeoutMs = 180000;
    constexpr uint64_t kProgressLogIntervalMs = 5000;
    const uint64_t startMs = ::GetTickCount64();
    uint64_t lastProgressMs = startMs;
    uint64_t lastProgressLogMs = startMs;
    const bool fullVariantPrewarm = RtxOptions::Shader::prewarmAllVariants();
    auto& pipelineManager = m_device->getCommon()->pipelineManager();
    uint32_t pendingPipelines = pipelineManager.remixShaderCompilationCount();
    bool aborted = false;
    bool stalled = false;
    ShaderPrewarmDialog progressDialog;
    if (showProgressDialog) {
      progressDialog.update(pendingPipelines, 0u);
      progressDialog.open();
    }

    while (pendingPipelines > 0u) {
      const uint64_t nowMs = ::GetTickCount64();
      const uint64_t elapsedMs = nowMs - startMs;
      const uint32_t currentPending =
        pipelineManager.remixShaderCompilationCount();
      if (currentPending != pendingPipelines) {
        pendingPipelines = currentPending;
        lastProgressMs = nowMs;
      }

      if (showProgressDialog)
        progressDialog.update(pendingPipelines, elapsedMs);

      if (nowMs - lastProgressLogMs >= kProgressLogIntervalMs) {
        Logger::info(str::format(
          "[Remix-DX11][init] shader prewarm progress: pendingPipelines=",
          pendingPipelines, " elapsedMs=", elapsedMs));
        lastProgressLogMs = nowMs;
      }

      stalled = nowMs - lastProgressMs >= kNoProgressTimeoutMs;
      const bool regularTimeout = !fullVariantPrewarm
        && elapsedMs >= kRegularPrewarmTimeoutMs;
      if (stalled || regularTimeout) {
        aborted = true;
        break;
      }
      Sleep(10);
    }

    if (showProgressDialog) {
      progressDialog.update(0u, ::GetTickCount64() - startMs);
      progressDialog.close();
    }

    const uint64_t totalElapsedMs = ::GetTickCount64() - startMs;
    if (aborted) {
      Logger::err(str::format(
        "[Remix-DX11][init] shader prewarm aborted: reason=",
        stalled ? "no-progress-180s" : "regular-time-limit-120s",
        " remainingPipelines=", pendingPipelines,
        " elapsedMs=", totalElapsedMs,
        "; continuing launch so a driver compiler failure cannot hang the game."));
    } else {
      Logger::info(str::format(
        "[Remix-DX11][init] shader prewarm complete: pendingPipelines=0 elapsedMs=",
        totalElapsedMs));
    }

    DxvkRaytracingPipeline::releaseFinalizer();

    m_warmupComplete = true;
  }

  void RtxInitializer::prewarmCachedGameShaders(
      uint32_t cachedShaderCount,
      const GameShaderRegistrar& registerShaders) {
    if (cachedShaderCount == 0u || !registerShaders)
      return;

    constexpr uint64_t kNoProgressTimeoutMs = 180000;
    constexpr uint64_t kProgressLogIntervalMs = 5000;
    const uint64_t startMs = ::GetTickCount64();
    ShaderPrewarmDialog progressDialog;
    const bool showDialog = RtxOptions::Shader::showPrewarmDialog();

    if (showDialog) {
      progressDialog.update(cachedShaderCount, 0u);
      progressDialog.open();
    }

    Logger::info(str::format(
      "[Remix-DX11][game-shader-cache] pre-init preload started: cachedShaders=",
      cachedShaderCount));

    registerShaders([&](uint32_t remainingShaders) {
      if (showDialog)
        progressDialog.update(
          remainingShaders, ::GetTickCount64() - startMs);
    });

    auto& pipelineManager = m_device->getCommon()->pipelineManager();
    uint32_t pendingPipelines = pipelineManager.shaderCompilationCount();
    uint64_t lastProgressMs = ::GetTickCount64();
    uint64_t lastProgressLogMs = lastProgressMs;
    bool stalled = false;

    if (showDialog)
      progressDialog.update(pendingPipelines, ::GetTickCount64() - startMs);

    Logger::info(str::format(
      "[Remix-DX11][game-shader-cache] cached shader registration complete; pendingPipelines=",
      pendingPipelines));

    while (pendingPipelines > 0u) {
      const uint64_t nowMs = ::GetTickCount64();
      const uint64_t elapsedMs = nowMs - startMs;
      const uint32_t currentPending =
        pipelineManager.shaderCompilationCount();
      if (currentPending != pendingPipelines) {
        pendingPipelines = currentPending;
        lastProgressMs = nowMs;
      }

      if (showDialog)
        progressDialog.update(pendingPipelines, elapsedMs);

      if (nowMs - lastProgressLogMs >= kProgressLogIntervalMs) {
        Logger::info(str::format(
          "[Remix-DX11][game-shader-cache] pipeline prewarm progress: pendingPipelines=",
          pendingPipelines, " elapsedMs=", elapsedMs));
        lastProgressLogMs = nowMs;
      }

      if (nowMs - lastProgressMs >= kNoProgressTimeoutMs) {
        stalled = true;
        break;
      }
      ::Sleep(10);
    }

    if (showDialog) {
      progressDialog.update(0u, ::GetTickCount64() - startMs);
      progressDialog.close();
    }

    const uint64_t elapsedMs = ::GetTickCount64() - startMs;
    if (stalled) {
      Logger::err(str::format(
        "[Remix-DX11][game-shader-cache] pipeline prewarm stopped after no progress for 180 seconds; remainingPipelines=",
        pendingPipelines, " elapsedMs=", elapsedMs,
        "; continuing launch so a driver compiler failure cannot hang the game."));
    } else {
      Logger::info(str::format(
        "[Remix-DX11][game-shader-cache] pre-init preload complete: pendingPipelines=0 elapsedMs=",
        elapsedMs));
    }
  }
}
