#include "d3d11_context_imm.h"
#include "d3d11_device.h"
#include "d3d11_swapchain.h"

#include "../dxvk/imgui/dxvk_imgui.h"
#include "../dxvk/rtx_render/rtx_option_manager.h"
#include "../util/log/log.h"

#include <mutex>
#include <unordered_map>

namespace dxvk {

  static UINT getRemixToggleUiMessage() {
    static const UINT s_msg = RegisterWindowMessageA("DXVK_REMIX_TOGGLE_UI");
    return s_msg;
  }

  // Per-HWND map so the static WndProc callback can find its swapchain.
  static std::mutex                                    g_d3d11WndProcMutex;
  static std::unordered_map<HWND, D3D11SwapChain*>    g_d3d11WndProcMap;

  // Primary swap chain: only this one renders the Remix UI overlay and drives
  // RT frame boundaries (EndFrame / OnPresent).  Multi-device games (Unity,
  // UE4) and emulators create multiple swap chains; without this guard the
  // overlay would render twice and RT state would be confused.  The first
  // swap chain to present claims primary.  If it's destroyed, the next one
  // takes over.
  static std::mutex          g_primaryMutex;
  static D3D11SwapChain*     g_primarySwapChain = nullptr;

  static bool isVirtualKeyPressedNow(UINT vk) {
    return (::GetAsyncKeyState(static_cast<int>(vk)) & 0x8000) != 0;
  }

  static bool matchesMenuHotkeyMessage(UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg != WM_KEYDOWN && msg != WM_SYSKEYDOWN)
      return false;

    if ((lParam & 0x40000000u) != 0)
      return false;

    const VirtualKeys& hotkey = RtxOptions::remixMenuKeyBinds();
    if (hotkey.empty())
      return false;

    bool hasNonModifier = false;
    bool matchedTriggerKey = false;

    for (const auto& vk : hotkey) {
      switch (vk.val) {
      case VK_SHIFT:
        if (!(wParam == VK_SHIFT || wParam == VK_LSHIFT || wParam == VK_RSHIFT || isVirtualKeyPressedNow(VK_SHIFT)))
          return false;
        break;
      case VK_CONTROL:
        if (!(wParam == VK_CONTROL || wParam == VK_LCONTROL || wParam == VK_RCONTROL || isVirtualKeyPressedNow(VK_CONTROL)))
          return false;
        break;
      case VK_MENU:
        if (!(wParam == VK_MENU || wParam == VK_LMENU || wParam == VK_RMENU || isVirtualKeyPressedNow(VK_MENU)))
          return false;
        break;
      default:
        hasNonModifier = true;
        matchedTriggerKey = matchedTriggerKey || (static_cast<UINT>(vk.val) == static_cast<UINT>(wParam));
        break;
      }
    }

    return hasNonModifier && matchedTriggerKey;
  }

  static LRESULT CALLBACK D3D11SwapChainWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    // Reentrancy guard: driver-injected WndProcs (e.g. Intel OPENGL32.dll)
    // can re-dispatch messages to the same HWND, causing infinite recursion.
    // On re-entry, skip our processing but still forward through the chain
    // so the game's WndProc receives messages. Hard-cap depth to prevent
    // stack overflow from pathological chains.
    thread_local int recursionDepth = 0;
    ++recursionDepth;

    if (recursionDepth > 8) {
      --recursionDepth;
      return DefWindowProcW(hWnd, msg, wParam, lParam);
    }

    if (recursionDepth > 1) {
      WNDPROC prev = nullptr;
      {
        std::lock_guard<std::mutex> lk(g_d3d11WndProcMutex);
        auto it = g_d3d11WndProcMap.find(hWnd);
        if (it != g_d3d11WndProcMap.end())
          prev = it->second->m_prevWndProc;
      }
      LRESULT result = prev ? CallWindowProcW(prev, hWnd, msg, wParam, lParam)
                            : DefWindowProcW(hWnd, msg, wParam, lParam);
      --recursionDepth;
      return result;
    }

    D3D11SwapChain* sc  = nullptr;
    WNDPROC         prev = nullptr;
    {
      std::lock_guard<std::mutex> lk(g_d3d11WndProcMutex);
      auto it = g_d3d11WndProcMap.find(hWnd);
      if (it != g_d3d11WndProcMap.end()) {
        sc   = it->second;
        prev = sc->m_prevWndProc;
      }
    }

    if (sc != nullptr && msg == getRemixToggleUiMessage()) {
      auto& gui = sc->m_device->getCommon()->getImgui();
      gui.switchMenu(static_cast<UIType>(wParam), lParam != 0, false);
      --recursionDepth;
      return 0;
    }

    if (sc != nullptr && matchesMenuHotkeyMessage(msg, wParam, lParam)) {
      auto& gui = sc->m_device->getCommon()->getImgui();
      // Skip if the hotkey was already processed by processHotkeys() this frame
      if (gui.isRemixMenuHotkeyLatched()) {
        --recursionDepth;
        return 0;
      }
      const bool uiOpen = gui.isMenuOpen();
      const UIType nextType = uiOpen
        ? UIType::None
        : (RtxOptions::defaultToAdvancedUI() ? UIType::Advanced : UIType::Basic);
      gui.switchMenu(nextType, true, false);
      gui.markRemixMenuHotkeyHandled();
      --recursionDepth;
      return 0;
    }

    // --- In-process input handling ---
    // The D3D11 bridge loads directly into the game process. The game's
    // WndProc and DefWindowProcW have direct side effects on the message queue:
    //
    //  - DefWindowProcW(WM_SYSKEYDOWN) activates the system menu, which
    //    generates spurious WM_KEYUP events that reset ImGui's key state.
    //    This makes hotkeys rapid-toggle (open/close every frame) instead
    //    of toggling once per press.
    //
    //  - WM_SYSCHAR triggers SC_KEYMENU in DefWindowProcW, adding more
    //    key state corruption.
    //
    // Fix: let ImGui process ALL messages first, then block system key
    // messages from reaching the game's WndProc / DefWindowProcW.
    // Allow Alt+F4 (close), Alt+Enter (fullscreen), and bare Alt
    // (VK_MENU) through so standard OS combos still work.

    if (sc != nullptr) {
      auto& gui = sc->m_device->getCommon()->getImgui();
      if (gui.isInit()) {
        // ImGui sees the message first — updates key state and UI input
        const bool consumedByGui = gui.wndProcHandler(hWnd, msg, wParam, lParam);

        const bool isKeyMsg   = (msg >= WM_KEYFIRST && msg <= WM_KEYLAST)
                             || (msg >= WM_SYSKEYDOWN && msg <= WM_SYSDEADCHAR);
        const bool isMouseMsg = (msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST);
        const bool isRawInputMsg = msg == WM_INPUT;

        if (consumedByGui) {
          --recursionDepth;
          return 0;
        }

        // When the Remix UI is open, block game input unconditionally.
        // Games like Skyrim SE read mouse/keyboard via WM_INPUT (raw input)
        // and direct WM_MOUSE* / WM_KEY* messages in their own WndProc —
        // if we let those through, the player's camera keeps spinning, the
        // weapon swings, and the game reacts to every click on a Remix
        // widget, which feels like "the game is stealing control".
        //
        // blockInputToGameInUI was previously opt-in, but the correct default
        // for any modded game is to freeze input while the overlay is up.
        // The setting is kept as a debug escape hatch: set it to False only
        // if you explicitly want the game to react while you're in the menu.
        const bool uiOpen = gui.isMenuOpen();
        if (uiOpen && RtxOptions::blockInputToGameInUI()) {
          if (isKeyMsg || isMouseMsg || isRawInputMsg) {
            --recursionDepth;
            return 0;
          }
        }

        // Block system key messages from the game ONLY when UI is open to prevent key state corruption.
        // Bare Alt (VK_MENU) passes through so games can detect Alt held/released.
        // Alt+F4 and Alt+Enter pass through for close and fullscreen toggle.
        if (uiOpen) {
          switch (msg) {
          case WM_SYSKEYDOWN:
          case WM_SYSKEYUP:
            if (wParam == VK_MENU || wParam == VK_F4 || wParam == VK_RETURN)
              break;  // allow these through to the game
            --recursionDepth;
            return 0;
          case WM_SYSCHAR:
            --recursionDepth;
            return 0;
          }
        }
      }
    }

    // Decrement AFTER CallWindowProcW returns — the previous WndProc
    // (e.g. opengl32) can pump messages that re-enter this callback.
    LRESULT result = prev ? CallWindowProcW(prev, hWnd, msg, wParam, lParam)
                          : DefWindowProcW(hWnd, msg, wParam, lParam);
    --recursionDepth;
    return result;
  }

  static uint16_t MapGammaControlPoint(float x) {
    if (x < 0.0f) x = 0.0f;
    if (x > 1.0f) x = 1.0f;
    return uint16_t(65535.0f * x);
  }


  D3D11SwapChain::D3D11SwapChain(
          D3D11DXGIDevice*        pContainer,
          D3D11Device*            pDevice,
          HWND                    hWnd,
    const DXGI_SWAP_CHAIN_DESC1*  pDesc)
  : m_dxgiDevice(pContainer),
    m_parent    (pDevice),
    m_window    (hWnd),
    m_desc      (*pDesc),
    m_device    (pDevice->GetDXVKDevice()),
    m_context   (m_device->createContext()),
    m_frameLatencyCap(pDevice->GetOptions()->maxFrameLatency) {
    CreateFrameLatencyEvent();

    // Hook the game's WndProc so ImGui receives WM_KEYDOWN/WM_KEYUP for hotkeys.
    // Must use SetWindowLongPtrW (GWLP_WNDPROC) — the correct x64 API.
    {
      std::lock_guard<std::mutex> lk(g_d3d11WndProcMutex);
      if (g_d3d11WndProcMap.find(hWnd) == g_d3d11WndProcMap.end()) {
        m_prevWndProc = reinterpret_cast<WNDPROC>(
          SetWindowLongPtrW(hWnd, GWLP_WNDPROC,
            reinterpret_cast<LONG_PTR>(D3D11SwapChainWndProc)));
        g_d3d11WndProcMap[hWnd] = this;
      }
    }

    Logger::info(str::format("[D3D11SwapChain] Created: HWND=", (uintptr_t)hWnd,
      " ", pDesc->Width, "x", pDesc->Height,
      " fmt=", pDesc->Format,
      " buffers=", pDesc->BufferCount,
      " this=", (uintptr_t)this));

    if (!pDevice->GetOptions()->deferSurfaceCreation)
      CreatePresenter();
    
    CreateBackBuffer();
    CreateBlitter();
    CreateHud();
  }


  D3D11SwapChain::~D3D11SwapChain() {
    // Restore the original WndProc and remove from map FIRST — prevents the
    // static WndProc callback from accessing this swap chain while waitForIdle
    // runs.  Messages during cleanup reach the game's own handler.
    if (m_prevWndProc != nullptr) {
      std::lock_guard<std::mutex> lk(g_d3d11WndProcMutex);
      SetWindowLongPtrW(m_window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(m_prevWndProc));
      g_d3d11WndProcMap.erase(m_window);
      m_prevWndProc = nullptr;
    }

    m_device->waitForSubmission(&m_presentStatus);
    m_device->waitForIdle();

    // Release primary swap chain ownership so the next one can take over.
    {
      std::lock_guard<std::mutex> lk(g_primaryMutex);
      if (g_primarySwapChain == this)
        g_primarySwapChain = nullptr;
    }
    
    if (m_backBuffer)
      m_backBuffer->ReleasePrivate();

    DestroyFrameLatencyEvent();
  }


  HRESULT STDMETHODCALLTYPE D3D11SwapChain::QueryInterface(
          REFIID                  riid,
          void**                  ppvObject) {
    if (ppvObject == nullptr)
      return E_POINTER;

    InitReturnPtr(ppvObject);

    if (riid == __uuidof(IUnknown)
     || riid == __uuidof(IDXGIVkSwapChain)) {
      *ppvObject = ref(this);
      return S_OK;
    }

    Logger::warn("D3D11SwapChain::QueryInterface: Unknown interface query");
    return E_NOINTERFACE;
  }


  HRESULT STDMETHODCALLTYPE D3D11SwapChain::GetDesc(
          DXGI_SWAP_CHAIN_DESC1*    pDesc) {
    *pDesc = m_desc;
    return S_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D11SwapChain::GetAdapter(
          REFIID                    riid,
          void**                    ppvObject) {
    return m_dxgiDevice->GetParent(riid, ppvObject);
  }


  HRESULT STDMETHODCALLTYPE D3D11SwapChain::GetDevice(
          REFIID                    riid,
          void**                    ppDevice) {
    return m_dxgiDevice->QueryInterface(riid, ppDevice);
  }


  HRESULT STDMETHODCALLTYPE D3D11SwapChain::GetImage(
          UINT                      BufferId,
          REFIID                    riid,
          void**                    ppBuffer) {
    InitReturnPtr(ppBuffer);

    if (BufferId > 0) {
      Logger::err("D3D11: GetImage: BufferId > 0 not supported");
      return DXGI_ERROR_UNSUPPORTED;
    }

    return m_backBuffer->QueryInterface(riid, ppBuffer);
  }


  UINT STDMETHODCALLTYPE D3D11SwapChain::GetImageIndex() {
    return 0;
  }


  UINT STDMETHODCALLTYPE D3D11SwapChain::GetFrameLatency() {
    return m_frameLatency;
  }


  HANDLE STDMETHODCALLTYPE D3D11SwapChain::GetFrameLatencyEvent() {
    return m_frameLatencyEvent;
  }


  HRESULT STDMETHODCALLTYPE D3D11SwapChain::ChangeProperties(
    const DXGI_SWAP_CHAIN_DESC1*  pDesc) {

    const bool swapchainGeometryChanged = m_desc.Format      != pDesc->Format
                                       || m_desc.Width       != pDesc->Width
                                       || m_desc.Height      != pDesc->Height
                                       || m_desc.BufferCount != pDesc->BufferCount
                                       || m_desc.Flags       != pDesc->Flags;

    m_dirty |= swapchainGeometryChanged;

    if (swapchainGeometryChanged) {
      // Live resize/fullscreen transitions can replace the backbuffer while the
      // previous frame is still in flight. Drain the old present work first so
      // the new backbuffer does not race the old swapchain images and trip a
      // device loss when emulators resize while rendering.
      m_device->waitForSubmission(&m_presentStatus);
      m_device->waitForIdle();
    }

    m_desc = *pDesc;
    CreateBackBuffer();
    return S_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D11SwapChain::SetPresentRegion(
    const RECT*                     pRegion) {
    // TODO implement
    return E_NOTIMPL;
  }


  HRESULT STDMETHODCALLTYPE D3D11SwapChain::SetGammaControl(
          UINT                      NumControlPoints,
    const DXGI_RGB*                 pControlPoints) {
    bool isIdentity = true;

    if (NumControlPoints > 1) {
      std::array<DxvkGammaCp, 1025> cp;

      if (NumControlPoints > cp.size())
        return E_INVALIDARG;
      
      for (uint32_t i = 0; i < NumControlPoints; i++) {
        uint16_t identity = MapGammaControlPoint(float(i) / float(NumControlPoints - 1));

        cp[i].r = MapGammaControlPoint(pControlPoints[i].Red);
        cp[i].g = MapGammaControlPoint(pControlPoints[i].Green);
        cp[i].b = MapGammaControlPoint(pControlPoints[i].Blue);
        cp[i].a = 0;

        isIdentity &= cp[i].r == identity
                   && cp[i].g == identity
                   && cp[i].b == identity;
      }

      if (!isIdentity)
        m_blitter->setGammaRamp(NumControlPoints, cp.data());
    }

    if (isIdentity)
      m_blitter->setGammaRamp(0, nullptr);

    return S_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D11SwapChain::SetFrameLatency(
          UINT                      MaxLatency) {
    if (MaxLatency == 0 || MaxLatency > DXGI_MAX_SWAP_CHAIN_BUFFERS)
      return DXGI_ERROR_INVALID_CALL;

    if (m_frameLatencyEvent) {
      // Windows DXGI does not seem to handle the case where the new maximum
      // latency is less than the current value, and some games relying on
      // this behaviour will hang if we attempt to decrement the semaphore.
      // Thus, only increment the semaphore as necessary.
      if (MaxLatency > m_frameLatency)
        ReleaseSemaphore(m_frameLatencyEvent, MaxLatency - m_frameLatency, nullptr);
    }

    m_frameLatency = MaxLatency;
    return S_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D11SwapChain::Present(
          UINT                      SyncInterval,
          UINT                      PresentFlags,
    const DXGI_PRESENT_PARAMETERS*  pPresentParameters) {
    auto options = m_parent->GetOptions();

    if (options->syncInterval >= 0)
      SyncInterval = options->syncInterval;

    if (!(PresentFlags & DXGI_PRESENT_TEST)) {
      bool vsync = SyncInterval != 0;

      m_dirty |= vsync != m_vsync;
      m_vsync  = vsync;
    }

    if (m_presenter == nullptr)
      CreatePresenter();

    HRESULT hr = S_OK;

    if (!m_presenter->hasSwapChain()) {
      RecreateSwapChain(m_vsync);
      m_dirty = false;
    }

    if (!m_presenter->hasSwapChain())
      hr = DXGI_STATUS_OCCLUDED;

    if (m_device->getDeviceStatus() != VK_SUCCESS)
      hr = DXGI_ERROR_DEVICE_RESET;

    if ((PresentFlags & DXGI_PRESENT_TEST) || hr != S_OK)
      return hr;

    if (std::exchange(m_dirty, false))
      RecreateSwapChain(m_vsync);
    
    try {
      PresentImage(SyncInterval);
    } catch (const DxvkError& e) {
      Logger::err(e.message());
      hr = E_FAIL;
    }

    return hr;
  }


  void STDMETHODCALLTYPE D3D11SwapChain::NotifyModeChange(
          BOOL                      Windowed,
    const DXGI_MODE_DESC*           pDisplayMode) {
    if (Windowed || !pDisplayMode) {
      // Display modes aren't meaningful in windowed mode
      m_displayRefreshRate = 0.0;
    } else {
      DXGI_RATIONAL rate = pDisplayMode->RefreshRate;
      m_displayRefreshRate = double(rate.Numerator) / double(rate.Denominator);
    }

    if (m_presenter != nullptr)
      m_presenter->setFrameRateLimiterRefreshRate(m_displayRefreshRate);
  }


  HRESULT D3D11SwapChain::PresentImage(UINT SyncInterval) {
    Com<ID3D11DeviceContext> deviceContext = nullptr;
    m_parent->GetImmediateContext(&deviceContext);

    auto immediateContext = static_cast<D3D11ImmediateContext*>(deviceContext.ptr());

    // Determine primary swap chain BEFORE flush.
    // Only the primary drives RT frame boundaries and the Remix overlay.
    // Prefer the visible, window-sized presenter with real scene draws so
    // transient helper or dummy chains do not steal authority.
    bool isPrimary = false;
    bool stole = false;
    const uint32_t thisDraws = immediateContext->m_rtx.getDrawCallID();
    m_lastPresentDrawCount = thisDraws;

    struct PrimaryCandidateInfo {
      bool visible = false;
      bool exactClientMatch = false;
      bool nearClientMatch = false;
      bool hasDraws = false;
      uint64_t area = 0;
      uint32_t draws = 0;
      uint32_t clientWidth = 0;
      uint32_t clientHeight = 0;
    };

    auto describeCandidate = [](D3D11SwapChain* sc) {
      PrimaryCandidateInfo info;

      if (sc == nullptr)
        return info;

      info.draws = sc->m_lastPresentDrawCount;
      info.hasDraws = info.draws > 0;

      const uint32_t width = sc->m_desc.Width;
      const uint32_t height = sc->m_desc.Height;
      info.area = uint64_t(width) * uint64_t(height);

      if (width < 64 || height < 64)
        return info;

      info.visible = IsWindow(sc->m_window) && IsWindowVisible(sc->m_window) && !IsIconic(sc->m_window);

      RECT clientRect = {};
      if (IsWindow(sc->m_window) && GetClientRect(sc->m_window, &clientRect)) {
        auto nonNegative = [](LONG value) -> uint32_t {
          return value > 0 ? static_cast<uint32_t>(value) : 0u;
        };
        auto absDiff = [](uint32_t a, uint32_t b) -> uint32_t {
          return a > b ? a - b : b - a;
        };

        info.clientWidth = nonNegative(clientRect.right - clientRect.left);
        info.clientHeight = nonNegative(clientRect.bottom - clientRect.top);

        if (info.clientWidth > 0 && info.clientHeight > 0) {
          const uint32_t exactWidthTolerance = info.clientWidth / 16u + 8u;
          const uint32_t exactHeightTolerance = info.clientHeight / 16u + 8u;
          const uint32_t nearWidthTolerance = info.clientWidth / 4u + 64u;
          const uint32_t nearHeightTolerance = info.clientHeight / 4u + 64u;

          info.exactClientMatch = absDiff(width, info.clientWidth) <= exactWidthTolerance
                               && absDiff(height, info.clientHeight) <= exactHeightTolerance;
          info.nearClientMatch = absDiff(width, info.clientWidth) <= nearWidthTolerance
                              && absDiff(height, info.clientHeight) <= nearHeightTolerance;
        }
      }

      return info;
    };

    auto isClearlyBetterCandidate = [](const PrimaryCandidateInfo& candidate, const PrimaryCandidateInfo& current) {
      if (candidate.visible != current.visible)
        return candidate.visible;

      if (candidate.exactClientMatch != current.exactClientMatch)
        return candidate.exactClientMatch;

      if (candidate.nearClientMatch != current.nearClientMatch)
        return candidate.nearClientMatch;

      if (candidate.hasDraws != current.hasDraws)
        return candidate.hasDraws;

      if (current.area == 0)
        return candidate.area > 0;

      if (candidate.area > current.area + current.area / 8)
        return true;

      if (candidate.draws > current.draws + 32)
        return true;

      return false;
    };

    D3D11SwapChain* previousPrimary = nullptr;

    {
      std::lock_guard<std::mutex> lk(g_primaryMutex);
      previousPrimary = g_primarySwapChain;
      if (g_primarySwapChain == nullptr || !IsWindow(g_primarySwapChain->m_window)) {
        g_primarySwapChain = this;
        stole = true;
      } else if (g_primarySwapChain != this && thisDraws > 0) {
        const PrimaryCandidateInfo candidate = describeCandidate(this);
        const PrimaryCandidateInfo current = describeCandidate(g_primarySwapChain);
        if (isClearlyBetterCandidate(candidate, current)) {
          g_primarySwapChain = this;
          stole = true;
        }
      }
      isPrimary = (g_primarySwapChain == this);
    }

    if (stole && previousPrimary != nullptr && previousPrimary != this && previousPrimary->m_device != m_device) {
      auto& previousGui = previousPrimary->m_device->getCommon()->getImgui();
      auto& currentGui = m_device->getCommon()->getImgui();
      const UIType previousMenuType = previousGui.getMenuType();
      const UIType currentMenuType = currentGui.getMenuType();

      if (previousMenuType != currentMenuType) {
        Logger::info(str::format("[D3D11SwapChain] Mirroring UI state onto new primary device: old=",
          static_cast<int>(previousMenuType),
          " new=", static_cast<int>(currentMenuType),
          " oldSwapChain=", (uintptr_t)previousPrimary,
          " newSwapChain=", (uintptr_t)this));
        currentGui.switchMenu(previousMenuType, true, false);
      }
    }

    auto getClientExtent = [this]() {
      VkExtent2D extent = { 0u, 0u };
      RECT clientRect = {};

      if (IsWindow(m_window) && GetClientRect(m_window, &clientRect)) {
        const LONG width = clientRect.right - clientRect.left;
        const LONG height = clientRect.bottom - clientRect.top;

        if (width > 0 && height > 0) {
          extent.width = static_cast<uint32_t>(width);
          extent.height = static_cast<uint32_t>(height);
        }
      }

      return extent;
    };

    if (stole) {
      const PrimaryCandidateInfo candidate = describeCandidate(this);
      Logger::info(str::format("[D3D11SwapChain] Primary claimed: HWND=", (uintptr_t)m_window,
        " draws=", thisDraws,
        " ", m_desc.Width, "x", m_desc.Height,
        " client=", candidate.clientWidth, "x", candidate.clientHeight,
        " exact=", candidate.exactClientMatch ? 1 : 0,
        " near=", candidate.nearClientMatch ? 1 : 0,
        " this=", (uintptr_t)this));
    }

    // EndFrame BEFORE Flush — EndFrame emits injectRTX to the CS queue;
    // the Flush below submits those RT commands to the GPU so the backbuffer
    // contains the ray-traced composite when the blitter copies it to the
    // Vulkan swap chain image.
    if (isPrimary) {
      immediateContext->m_rtx.EndFrame(m_swapImage, getClientExtent());
    }

    // Flush all pending CS work: game draw commands AND (if primary) the
    // injectRTX RT blit.  After this, m_swapImage holds the composited image.
    immediateContext->Flush();

    // Bump our frame id.
    ++m_frameId;
    
    for (uint32_t i = 0; i < SyncInterval || i < 1; i++) {
      SynchronizePresent();

      if (!m_presenter->hasSwapChain())
        return DXGI_STATUS_OCCLUDED;

      // Presentation semaphores and WSI swap chain image
      vk::PresenterInfo info = m_presenter->info();
      vk::PresenterSync sync;

      uint32_t imageIndex = 0;

      VkResult status = m_presenter->acquireNextImage(sync, imageIndex);

      uint32_t acquireRetries = 0;
      while (status != VK_SUCCESS) {
        RecreateSwapChain(m_vsync);

        if (!m_presenter->hasSwapChain())
          return DXGI_STATUS_OCCLUDED;
        
        info = m_presenter->info();
        status = m_presenter->acquireNextImage(sync, imageIndex);

        if (status == VK_SUBOPTIMAL_KHR)
          break;

        // Bail out after a few retries — the surface keeps invalidating
        // (window backgrounded, minimized, or being resized).  The caller
        // will retry on the next Present call.
        if (++acquireRetries > 3)
          return DXGI_STATUS_OCCLUDED;
      }

      // Resolve back buffer if it is multisampled. We
      // only have to do it only for the first frame.
      m_context->beginRecording(
        m_device->createCommandList());

      if (m_swapImageRtView != nullptr) {
        DxvkRenderTargets renderTargets;
        renderTargets.color[0].view = m_swapImageRtView;
        renderTargets.color[0].layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        m_context->bindRenderTargets(renderTargets);

        if (m_hud != nullptr)
          m_hud->render(m_context, info.format, info.imageExtent);

        if (isPrimary) {
          // Ensure our WndProc hook is installed and points to THIS swap chain.
          // Needed when: (a) game replaced our hook, or (b) a previous primary's
          // destructor restored the original WndProc before this chain took over.
          {
            std::lock_guard<std::mutex> lk(g_d3d11WndProcMutex);
            WNDPROC current = reinterpret_cast<WNDPROC>(GetWindowLongPtrW(m_window, GWLP_WNDPROC));
            if (current != D3D11SwapChainWndProc) {
              m_prevWndProc = current;
              SetWindowLongPtrW(m_window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(D3D11SwapChainWndProc));
            }
            g_d3d11WndProcMap[m_window] = this;
          }

          // Render Remix ImGui into the Remix backbuffer so the presented image
          // remains a single Remix-owned composition instead of a DXVK-side overlay.
          RtxOptionManager::applyPendingValues(m_device.ptr(), false);
          auto& gui = m_device->getCommon()->getImgui();
          gui.render(m_window, m_context, info.imageExtent, m_vsync);
        }
      }
      
      m_blitter->presentImage(m_context.ptr(),
        m_imageViews.at(imageIndex), VkRect2D(),
        m_swapImageView, VkRect2D());

      if (i + 1 >= SyncInterval)
        m_context->signal(m_frameLatencySignal, m_frameId);

      if (isPrimary) {
        // Register the acquired swapchain image as the present target.
        immediateContext->m_rtx.OnPresent(m_imageViews.at(imageIndex)->image(), getClientExtent());
      }

      SubmitPresent(immediateContext, sync, i, isPrimary);
    }

    SyncFrameLatency();
    return S_OK;
  }


  void D3D11SwapChain::SubmitPresent(
          D3D11ImmediateContext*  pContext,
    const vk::PresenterSync&      Sync,
          uint32_t                FrameId,
          bool                    IsPrimary) {
    auto lock = pContext->LockContext();

    // Present from CS thread so that we don't
    // have to synchronize with it first.
    m_presentStatus.result = VK_NOT_READY;

    pContext->EmitCs([this,
      cFrameId     = FrameId,
      cSync        = Sync,
      cHud         = m_hud,
      cIsPrimary   = IsPrimary,
      cCommandList = m_context->endRecording()
    ] (DxvkContext* ctx) {
      m_device->submitCommandList(cCommandList,
        cSync.acquire, cSync.present);

      if (cHud != nullptr && !cFrameId)
        cHud->update(0);

      m_device->presentImage(0, false, 0, m_presenter, &m_presentStatus);

      // Only the primary chain advances the Remix frame counter.  Secondary
      // chains (video/loading) present their own images but must not bump
      // getCurrentFrameId() — that would desynchronize the camera validity
      // check in injectRTX and cause "not detecting a valid camera" errors.
      if (cIsPrimary) {
        m_device->incrementPresentCount();
        const uint32_t newFrameId = m_device->getCurrentFrameId();
        if (newFrameId < 32 || (newFrameId < 512 && (newFrameId % 64) == 0)) {
          Logger::info(str::format("[D3D11SwapChain] CS incrementPresentCount: newFrameId=",
            newFrameId));
        }
      }
    });

    pContext->FlushCsChunk();
  }


  void D3D11SwapChain::SynchronizePresent() {
    // Recreate swap chain if the previous present call failed
    VkResult status = m_device->waitForSubmission(&m_presentStatus);
    
    if (status != VK_SUCCESS)
      RecreateSwapChain(m_vsync);
  }


  void D3D11SwapChain::RecreateSwapChain(BOOL Vsync) {
    // Logged so we can correlate "game refreshes / black flash" events in
    // the log with actual swapchain recreations. Very useful when the UI
    // triggers a settings change that forces a recreate.
    Logger::info(str::format("[D3D11SwapChain] RecreateSwapChain: ",
      m_desc.Width, "x", m_desc.Height,
      " bufCount=", m_desc.BufferCount,
      " vsync=", Vsync ? 1 : 0,
      " fmt=", (uint32_t) m_desc.Format));

    // Ensure that we can safely destroy the swap chain
    m_device->waitForSubmission(&m_presentStatus);
    m_device->waitForIdle();

    m_presentStatus.result = VK_SUCCESS;

    vk::PresenterDesc presenterDesc;
    presenterDesc.imageExtent     = { m_desc.Width, m_desc.Height };
    presenterDesc.imageCount      = PickImageCount(m_desc.BufferCount + 1);
    presenterDesc.numFormats      = PickFormats(m_desc.Format, presenterDesc.formats);
    presenterDesc.numPresentModes = PickPresentModes(Vsync, presenterDesc.presentModes);
    presenterDesc.fullScreenExclusive = PickFullscreenMode();

    if (m_presenter->recreateSwapChain(presenterDesc) != VK_SUCCESS)
      throw DxvkError("D3D11SwapChain: Failed to recreate swap chain");
    
    CreateRenderTargetViews();
  }


  void D3D11SwapChain::CreateFrameLatencyEvent() {
    m_frameLatencySignal = new sync::CallbackFence(m_frameId);

    if (m_desc.Flags & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT)
      m_frameLatencyEvent = CreateSemaphore(nullptr, m_frameLatency, DXGI_MAX_SWAP_CHAIN_BUFFERS, nullptr);
  }


  void D3D11SwapChain::CreatePresenter() {
    DxvkDeviceQueue graphicsQueue = m_device->queues().graphics;

    vk::PresenterDevice presenterDevice;
    presenterDevice.queueFamily   = graphicsQueue.queueFamily;
    presenterDevice.queue         = graphicsQueue.queueHandle;
    presenterDevice.adapter       = m_device->adapter()->handle();
    presenterDevice.features.fullScreenExclusive = m_device->extensions().extFullScreenExclusive;

    vk::PresenterDesc presenterDesc;
    presenterDesc.imageExtent     = { m_desc.Width, m_desc.Height };
    presenterDesc.imageCount      = PickImageCount(m_desc.BufferCount + 1);
    presenterDesc.numFormats      = PickFormats(m_desc.Format, presenterDesc.formats);
    presenterDesc.numPresentModes = PickPresentModes(false, presenterDesc.presentModes);
    presenterDesc.fullScreenExclusive = PickFullscreenMode();

    m_presenter = new vk::Presenter(m_window,
      m_device->adapter()->vki(),
      m_device->vkd(),
      presenterDevice,
      presenterDesc);
    
    m_presenter->setFrameRateLimit(m_parent->GetOptions()->maxFrameRate);
    m_presenter->setFrameRateLimiterRefreshRate(m_displayRefreshRate);

    CreateRenderTargetViews();
  }


  void D3D11SwapChain::CreateRenderTargetViews() {
    vk::PresenterInfo info = m_presenter->info();

    m_imageViews.clear();
    m_imageViews.resize(info.imageCount);

    DxvkImageCreateInfo imageInfo;
    imageInfo.type        = VK_IMAGE_TYPE_2D;
    imageInfo.format      = info.format.format;
    imageInfo.flags       = 0;
    imageInfo.sampleCount = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.extent      = { info.imageExtent.width, info.imageExtent.height, 1 };
    imageInfo.numLayers   = 1;
    imageInfo.mipLevels   = 1;
    imageInfo.usage       = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    imageInfo.stages      = 0;
    imageInfo.access      = 0;
    imageInfo.tiling      = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.layout      = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    imageInfo.shared      = VK_TRUE;

    DxvkImageViewCreateInfo viewInfo;
    viewInfo.type         = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format       = info.format.format;
    viewInfo.usage        = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    viewInfo.aspect       = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.minLevel     = 0;
    viewInfo.numLevels    = 1;
    viewInfo.minLayer     = 0;
    viewInfo.numLayers    = 1;

    for (uint32_t i = 0; i < info.imageCount; i++) {
      VkImage imageHandle = m_presenter->getImage(i).image;
      
      Rc<DxvkImage> image = new DxvkImage(
        m_device.ptr(), imageInfo, imageHandle);

      m_imageViews[i] = new DxvkImageView(
        m_device->vkd(), image, viewInfo);
    }
  }


  void D3D11SwapChain::CreateBackBuffer() {
    // Explicitly destroy current swap image before
    // creating a new one to free up resources
    if (m_backBuffer)
      m_backBuffer->ReleasePrivate();
    
    m_swapImage         = nullptr;
    m_swapImageView     = nullptr;
    m_swapImageRtView   = nullptr;
    m_backBuffer        = nullptr;

    // Create new back buffer
    D3D11_COMMON_TEXTURE_DESC desc;
    desc.Width              = std::max(m_desc.Width,  1u);
    desc.Height             = std::max(m_desc.Height, 1u);
    desc.Depth              = 1;
    desc.MipLevels          = 1;
    desc.ArraySize          = 1;
    desc.Format             = m_desc.Format;
    desc.SampleDesc         = m_desc.SampleDesc;
    desc.Usage              = D3D11_USAGE_DEFAULT;
    desc.BindFlags          = 0;
    desc.CPUAccessFlags     = 0;
    desc.MiscFlags          = 0;
    desc.TextureLayout      = D3D11_TEXTURE_LAYOUT_UNDEFINED;

    if (m_desc.BufferUsage & DXGI_USAGE_RENDER_TARGET_OUTPUT)
      desc.BindFlags |= D3D11_BIND_RENDER_TARGET;

    if (m_desc.BufferUsage & DXGI_USAGE_SHADER_INPUT)
      desc.BindFlags |= D3D11_BIND_SHADER_RESOURCE;

    if (m_desc.BufferUsage & DXGI_USAGE_UNORDERED_ACCESS)
      desc.BindFlags |= D3D11_BIND_UNORDERED_ACCESS;
    
    if (m_desc.Flags & DXGI_SWAP_CHAIN_FLAG_GDI_COMPATIBLE)
      desc.MiscFlags |= D3D11_RESOURCE_MISC_GDI_COMPATIBLE;
    
    DXGI_USAGE dxgiUsage = DXGI_USAGE_BACK_BUFFER;

    if (m_desc.SwapEffect == DXGI_SWAP_EFFECT_DISCARD
     || m_desc.SwapEffect == DXGI_SWAP_EFFECT_FLIP_DISCARD)
      dxgiUsage |= DXGI_USAGE_DISCARD_ON_PRESENT;

    m_backBuffer = new D3D11Texture2D(m_parent, &desc, dxgiUsage, VK_NULL_HANDLE);
    m_backBuffer->AddRefPrivate();

    m_swapImage = GetCommonTexture(m_backBuffer)->GetImage();

    // Create an image view that allows the
    // image to be bound as a shader resource.
    DxvkImageViewCreateInfo viewInfo;
    viewInfo.type       = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format     = m_swapImage->info().format;
    viewInfo.usage      = VK_IMAGE_USAGE_SAMPLED_BIT;
    viewInfo.aspect     = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.minLevel   = 0;
    viewInfo.numLevels  = 1;
    viewInfo.minLayer   = 0;
    viewInfo.numLayers  = 1;
    m_swapImageView = m_device->createImageView(m_swapImage, viewInfo);

    viewInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    m_swapImageRtView = m_device->createImageView(m_swapImage, viewInfo);
    
    // Initialize the image so that we can use it. Clearing
    // to black prevents garbled output for the first frame.
    VkImageSubresourceRange subresources;
    subresources.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
    subresources.baseMipLevel   = 0;
    subresources.levelCount     = 1;
    subresources.baseArrayLayer = 0;
    subresources.layerCount     = 1;

    VkClearColorValue clearColor;
    clearColor.float32[0] = 0.0f;
    clearColor.float32[1] = 0.0f;
    clearColor.float32[2] = 0.0f;
    clearColor.float32[3] = 0.0f;

    m_context->beginRecording(
      m_device->createCommandList());
    
    m_context->clearColorImage(
      m_swapImage, clearColor, subresources);

    m_device->submitCommandList(
      m_context->endRecording(),
      VK_NULL_HANDLE,
      VK_NULL_HANDLE);
  }


  void D3D11SwapChain::CreateBlitter() {
    m_blitter = new DxvkSwapchainBlitter(m_device);    
  }


  void D3D11SwapChain::CreateHud() {
    m_hud = hud::Hud::createHud(m_device);

    if (m_hud != nullptr)
      m_hud->addItem<hud::HudClientApiItem>("api", 1, GetApiName());
  }


  void D3D11SwapChain::DestroyFrameLatencyEvent() {
    CloseHandle(m_frameLatencyEvent);
  }


  void D3D11SwapChain::SyncFrameLatency() {
    // Wait for the sync event so that we respect the maximum frame latency
    m_frameLatencySignal->wait(m_frameId - GetActualFrameLatency());

    if (m_frameLatencyEvent) {
      m_frameLatencySignal->setCallback(m_frameId, [cFrameLatencyEvent = m_frameLatencyEvent] () {
        ReleaseSemaphore(cFrameLatencyEvent, 1, nullptr);
      });
    }
  }


  uint32_t D3D11SwapChain::GetActualFrameLatency() {
    uint32_t maxFrameLatency = m_frameLatency;

    if (!(m_desc.Flags & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT))
      m_dxgiDevice->GetMaximumFrameLatency(&maxFrameLatency);

    if (m_frameLatencyCap)
      maxFrameLatency = std::min(maxFrameLatency, m_frameLatencyCap);

    maxFrameLatency = std::min(maxFrameLatency, m_desc.BufferCount + 1);
    return maxFrameLatency;
  }


  uint32_t D3D11SwapChain::PickFormats(
          DXGI_FORMAT               Format,
          VkSurfaceFormatKHR*       pDstFormats) {
    uint32_t n = 0;

    switch (Format) {
      default:
        Logger::warn(str::format("D3D11SwapChain: Unexpected format: ", m_desc.Format));
        
      case DXGI_FORMAT_R8G8B8A8_UNORM:
      case DXGI_FORMAT_B8G8R8A8_UNORM: {
        pDstFormats[n++] = { VK_FORMAT_R8G8B8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR };
        pDstFormats[n++] = { VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR };
      } break;
      
      case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
      case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: {
        pDstFormats[n++] = { VK_FORMAT_R8G8B8A8_SRGB, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR };
        pDstFormats[n++] = { VK_FORMAT_B8G8R8A8_SRGB, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR };
      } break;
      
      case DXGI_FORMAT_R10G10B10A2_UNORM: {
        pDstFormats[n++] = { VK_FORMAT_A2B10G10R10_UNORM_PACK32, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR };
        pDstFormats[n++] = { VK_FORMAT_A2R10G10B10_UNORM_PACK32, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR };
      } break;
      
      case DXGI_FORMAT_R16G16B16A16_FLOAT: {
        pDstFormats[n++] = { VK_FORMAT_R16G16B16A16_SFLOAT, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR };
      } break;
    }

    return n;
  }


  uint32_t D3D11SwapChain::PickPresentModes(
          BOOL                      Vsync,
          VkPresentModeKHR*         pDstModes) {
    uint32_t n = 0;

    if (Vsync) {
      if (m_parent->GetOptions()->tearFree == Tristate::False)
        pDstModes[n++] = VK_PRESENT_MODE_FIFO_RELAXED_KHR;
      pDstModes[n++] = VK_PRESENT_MODE_FIFO_KHR;
    } else {
      if (m_parent->GetOptions()->tearFree != Tristate::True)
        pDstModes[n++] = VK_PRESENT_MODE_IMMEDIATE_KHR;
      pDstModes[n++] = VK_PRESENT_MODE_MAILBOX_KHR;
    }

    return n;
  }


  uint32_t D3D11SwapChain::PickImageCount(
          UINT                      Preferred) {
    int32_t option = m_parent->GetOptions()->numBackBuffers;
    return option > 0 ? uint32_t(option) : uint32_t(Preferred);
  }


  VkFullScreenExclusiveEXT D3D11SwapChain::PickFullscreenMode() {
    return m_desc.Flags & DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH
      ? VK_FULL_SCREEN_EXCLUSIVE_ALLOWED_EXT
      : VK_FULL_SCREEN_EXCLUSIVE_DISALLOWED_EXT;
  }


  std::string D3D11SwapChain::GetApiName() const {
    Com<IDXGIDXVKDevice> device;
    m_parent->QueryInterface(__uuidof(IDXGIDXVKDevice), reinterpret_cast<void**>(&device));

    uint32_t apiVersion = device->GetAPIVersion();
    uint32_t featureLevel = m_parent->GetFeatureLevel();

    uint32_t flHi = (featureLevel >> 12);
    uint32_t flLo = (featureLevel >> 8) & 0x7;

    return str::format("D3D", apiVersion, " FL", flHi, "_", flLo);
  }

}
