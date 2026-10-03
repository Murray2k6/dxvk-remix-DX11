#pragma once

#include <windows.h>
#include <atomic>
#include <mutex>
#include <vector>

namespace dx11_capture {

  // The native DXGI swapchain keeps its game HWND. Vulkan owns a separate child
  // which is visible only after a complete Remix frame is acknowledged. Its UI
  // thread keeps pumping while the game's Present thread waits on bridge IPC.
  class PresentationWindow {
  public:
    static PresentationWindow& instance() {
      // Hooks and their UI window have process lifetime. Never join a UI thread
      // from a DLL/static destructor running under the Windows loader lock.
      static auto* state = new PresentationWindow();
      return *state;
    }

    HWND ensure(HWND parent) {
      if (!parent || !IsWindow(parent) || !start()) {
        setVisible(false);
        return nullptr;
      }
      RECT rectangle = {};
      const HWND child = m_publishedChild.load();
      if (child && parent == m_publishedParent.load() && !IsIconic(parent)
       && GetClientRect(parent, &rectangle)
       && rectangle.right > 0 && rectangle.bottom > 0
       && m_publishedExtent.load() == extent(rectangle.right, rectangle.bottom))
        return child;
      DWORD_PTR result = 0;
      if (!SendMessageTimeoutW(m_control.load(), Ensure, reinterpret_cast<WPARAM>(parent), 0,
                               SMTO_ABORTIFHUNG, 2000, &result)) {
        setVisible(false);
        return nullptr;
      }
      return reinterpret_cast<HWND>(result);
    }

    bool setVisible(bool visible) {
      m_wantedVisible.store(visible);
      const HWND control = m_control.load();
      if (!control)
        return !visible;
      // No Windows message on the common path where visibility stays the same.
      if (m_visible.load() == visible)
        return true;
      DWORD_PTR result = 0;
      if (SendMessageTimeoutW(control, Visibility, 0, 0, SMTO_ABORTIFHUNG, 2000, &result))
        return result != 0;
      // A timed-out show must not later expose a stale frame during fallback.
      m_wantedVisible.store(false);
      PostMessageW(control, Visibility, 0, 0);
      return false;
    }

    void retirePrevious() {
      if (m_hasRetired.load()) {
        if (const HWND control = m_control.load())
          PostMessageW(control, Retire, 0, 0);
      }
    }

    // Explicit bridge teardown only; never invoked from DllMain.
    void shutdown() {
      m_wantedVisible.store(false);
      if (const HWND control = m_control.load()) {
        DWORD_PTR result = 0;
        SendMessageTimeoutW(control, Stop, 0, 0, SMTO_ABORTIFHUNG, 2000, &result);
      }
      std::lock_guard<std::mutex> lock(m_startMutex);
      if (m_thread && GetThreadId(m_thread) != GetCurrentThreadId())
        WaitForSingleObject(m_thread, 2000);
      closeFinishedThread();
    }

  private:
    static constexpr UINT Ensure = WM_APP + 61;
    static constexpr UINT Visibility = WM_APP + 62;
    static constexpr UINT Retire = WM_APP + 63;
    static constexpr UINT StopIfUnused = WM_APP + 64;
    static constexpr UINT Stop = WM_APP + 65;
    static constexpr const wchar_t* ClassName = L"RemixDx11PresentationWindow";

    std::mutex m_startMutex;
    std::atomic<HWND> m_control { nullptr };
    std::atomic<bool> m_visible { false };
    std::atomic<bool> m_wantedVisible { false };
    std::atomic<HWND> m_publishedChild { nullptr };
    std::atomic<HWND> m_publishedParent { nullptr };
    std::atomic<unsigned long long> m_publishedExtent { 0 };
    std::atomic<bool> m_hasRetired { false };
    HANDLE m_started = nullptr;
    HANDLE m_thread = nullptr;
    HINSTANCE m_module = nullptr;
    HWND m_child = nullptr; // UI thread only below this point.
    HWND m_parent = nullptr;
    LONG m_width = 0;
    LONG m_height = 0;
    std::vector<HWND> m_retired;

    static unsigned long long extent(LONG width, LONG height) {
      return (static_cast<unsigned long long>(static_cast<ULONG>(width)) << 32)
        | static_cast<ULONG>(height);
    }

    bool start() {
      std::lock_guard<std::mutex> lock(m_startMutex);
      if (m_control.load())
        return true;
      closeFinishedThread();
      if (!m_thread) {
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
              reinterpret_cast<LPCWSTR>(&threadMain), &m_module))
          return false;
        m_started = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!m_started)
          return false;
        m_thread = CreateThread(nullptr, 0, &threadMain, this, 0, nullptr);
        if (!m_thread) {
          CloseHandle(m_started);
          m_started = nullptr;
          return false;
        }
      }
      return WaitForSingleObject(m_started, 2000) == WAIT_OBJECT_0 && m_control.load();
    }

    void closeFinishedThread() {
      if (m_thread && WaitForSingleObject(m_thread, 0) == WAIT_OBJECT_0) {
        CloseHandle(m_thread);
        CloseHandle(m_started);
        m_thread = nullptr;
        m_started = nullptr;
      }
    }

    static DWORD WINAPI threadMain(void* argument) {
      auto& state = *static_cast<PresentationWindow*>(argument);
      WNDCLASSW windowClass = {};
      windowClass.lpfnWndProc = &windowProc;
      windowClass.hInstance = state.m_module;
      windowClass.lpszClassName = ClassName;
      const ATOM registered = RegisterClassW(&windowClass);
      if (registered || GetLastError() == ERROR_CLASS_ALREADY_EXISTS) {
        state.m_control.store(CreateWindowExW(0, ClassName, L"", 0, 0, 0, 0, 0,
          HWND_MESSAGE, nullptr, state.m_module, &state));
      }
      SetEvent(state.m_started);
      if (!state.m_control.load())
        return 1;
      MSG message;
      while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
      }
      return 0;
    }

    static LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
      auto* state = reinterpret_cast<PresentationWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA));
      if (message == WM_NCCREATE) {
        state = static_cast<PresentationWindow*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
      }
      if (state && message == Ensure)
        return reinterpret_cast<LRESULT>(state->ensureOnThread(reinterpret_cast<HWND>(wparam)));
      if (state && message == Visibility) {
        const bool show = state->m_wantedVisible.load() && IsWindow(state->m_child);
        if (IsWindow(state->m_child)) {
          if (show) {
            // Raise above any sibling the game created after us so its native
            // output cannot be composited over the ray-traced image.
            SetWindowPos(state->m_child, HWND_TOP, 0, 0, 0, 0,
              SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_SHOWWINDOW);
          } else {
            ShowWindow(state->m_child, SW_HIDE);
          }
        }
        state->m_visible.store(show);
        return show == state->m_wantedVisible.load();
      }
      if (state && message == Retire) {
        for (const HWND oldWindow : state->m_retired)
          if (IsWindow(oldWindow)) DestroyWindow(oldWindow);
        state->m_retired.clear();
        state->m_hasRetired.store(false);
        return 0;
      }
      if (state && (message == Stop || (message == StopIfUnused && !IsWindow(state->m_child)))) {
        if (IsWindow(state->m_child)) DestroyWindow(state->m_child);
        for (const HWND oldWindow : state->m_retired)
          if (IsWindow(oldWindow)) DestroyWindow(oldWindow);
        state->m_retired.clear();
        state->m_hasRetired.store(false);
        state->m_control.store(nullptr);
        DestroyWindow(window);
        PostQuitMessage(0);
        return 0;
      }
      if (state && message == WM_NCDESTROY && window == state->m_child) {
        state->m_child = nullptr;
        state->m_publishedChild.store(nullptr);
        state->m_visible.store(false);
        if (const HWND control = state->m_control.load())
          PostMessageW(control, StopIfUnused, 0, 0);
      }
      if (message == WM_ERASEBKGND)
        return 1;
      if (message == WM_PAINT) {
        ValidateRect(window, nullptr);
        return 0;
      }
      return DefWindowProcW(window, message, wparam, lparam);
    }

    HWND ensureOnThread(HWND parent) {
      RECT rectangle = {};
      if (!IsWindow(parent) || !GetClientRect(parent, &rectangle))
        return nullptr;
      const LONG width = rectangle.right - rectangle.left;
      const LONG height = rectangle.bottom - rectangle.top;
      if (width <= 0 || height <= 0 || IsIconic(parent)) {
        if (IsWindow(m_child)) ShowWindow(m_child, SW_HIDE);
        m_visible.store(false);
        return nullptr;
      }
      if (!IsWindow(m_child) || m_parent != parent) {
        if (IsWindow(m_child)) {
          ShowWindow(m_child, SW_HIDE);
          // The server retires its old surface during Present's retarget. Keep
          // this hidden HWND alive until that command has been acknowledged.
          m_retired.push_back(m_child);
          m_hasRetired.store(true);
        }
        m_visible.store(false);
        m_wantedVisible.store(false);
        m_child = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_NOPARENTNOTIFY,
          ClassName, L"Remix presentation", WS_CHILD | WS_DISABLED | WS_CLIPSIBLINGS,
          0, 0, width, height, parent, nullptr, m_module, this);
        m_parent = parent;
        m_width = width;
        m_height = height;
      } else if (m_width != width || m_height != height) {
        if (!SetWindowPos(m_child, nullptr, 0, 0, width, height,
                         SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOOWNERZORDER))
          return nullptr;
        m_width = width;
        m_height = height;
      }
      m_publishedParent.store(m_parent);
      m_publishedExtent.store(extent(m_width, m_height));
      m_publishedChild.store(m_child);
      return m_child;
    }
  };
}
