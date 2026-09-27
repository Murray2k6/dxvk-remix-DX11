#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "../../../bridge_dx11_work/src/client_dx11/dx11_presentation_window.h"
#include <cstdio>
#include <cstdlib>

static void require(bool condition, const char* message) {
  if (!condition) { std::fprintf(stderr, "%s (Win32 %lu)\n", message, GetLastError()); std::exit(1); }
}

int main() {
  WNDCLASSW windowClass = {};
  windowClass.lpfnWndProc = DefWindowProcW;
  windowClass.hInstance = GetModuleHandleW(nullptr);
  windowClass.lpszClassName = L"RemixPresentationOwnershipTest";
  require(RegisterClassW(&windowClass) != 0, "register parent");
  const auto createParent = [&] {
    return CreateWindowW(windowClass.lpszClassName, L"", WS_OVERLAPPEDWINDOW,
      0, 0, 320, 240, nullptr, nullptr, windowClass.hInstance, nullptr);
  };
  HWND first = createParent();
  HWND second = createParent();
  require(first && second, "create parents");
  auto& presentation = dx11_capture::PresentationWindow::instance();
  HWND child = presentation.ensure(first);
  require(child && child != first && GetParent(child) == first, "distinct presentation HWND");
  require(GetWindowThreadProcessId(child, nullptr) != GetCurrentThreadId(), "independent UI pump");
  require((GetWindowLongPtrW(child, GWL_STYLE) & WS_DISABLED) != 0, "child must not take input");
  require((GetWindowLongPtrW(child, GWL_STYLE) & WS_VISIBLE) == 0, "no unready frame visible");
  for (unsigned i = 0; i < 1000; ++i)
    require(presentation.ensure(first) == child, "stable HWND across frame lookups");
  require(presentation.setVisible(true), "show acknowledged frame");
  require((GetWindowLongPtrW(child, GWL_STYLE) & WS_VISIBLE) != 0, "acknowledged frame hidden");
  require(presentation.setVisible(false), "native fallback");
  require((GetWindowLongPtrW(child, GWL_STYLE) & WS_VISIBLE) == 0, "stale RT frame covers native fallback");
  require(SetWindowPos(first, nullptr, 0, 0, 641, 481, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE) != 0, "resize parent");
  require(presentation.ensure(first) == child, "resize recreated HWND unnecessarily");
  RECT parentRect = {}, childRect = {};
  GetClientRect(first, &parentRect);
  GetClientRect(child, &childRect);
  require(parentRect.right == childRect.right && parentRect.bottom == childRect.bottom, "child extent mismatch");
  HWND replacement = presentation.ensure(second);
  require(replacement && replacement != child && GetParent(replacement) == second, "retarget child");
  require(IsWindow(child) != 0, "old surface HWND retired before server acknowledgement");
  presentation.retirePrevious();
  // The next synchronous visibility change follows the posted retirement.
  require(presentation.setVisible(true), "show replacement");
  for (unsigned i = 0; i < 200 && IsWindow(child); ++i) Sleep(1);
  require(!IsWindow(child), "old acknowledged surface HWND leaked");
  require(presentation.setVisible(false), "hide replacement");
  require(!presentation.ensure(nullptr), "invalid parent accepted");
  require(DestroyWindow(first) != 0, "destroy original native parent");
  require(DestroyWindow(second) != 0, "destroy replacement native parent");
  for (unsigned i = 0; i < 2000 && IsWindow(replacement); ++i) Sleep(1);
  require(!IsWindow(replacement), "parent teardown leaked child");
  // Explicit shutdown runs outside loader lock and permits later game devices.
  presentation.shutdown();
  for (unsigned cycle = 0; cycle != 20; ++cycle) {
    HWND parent = createParent();
    require(parent != nullptr, "create replacement device parent");
    HWND next = presentation.ensure(parent);
    require(next && GetParent(next) == parent, "UI thread did not restart after teardown");
    presentation.shutdown();
    require(!IsWindow(next) && IsWindow(parent), "explicit teardown damaged native parent or leaked child");
    DestroyWindow(parent);
  }
  std::puts("Independent presentation HWND, resize, retarget, native fallback and 20 UI thread restart cycles passed.");
}
