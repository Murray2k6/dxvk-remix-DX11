#include "d3d11_input_guard.h"

#include "../util/log/log.h"
#include "../util/util_string.h"

#include <windows.h>

#include <array>
#include <atomic>
#include <cstring>
#include <mutex>

namespace dxvk::D3D11InputGuard {

  namespace {

    std::atomic<bool> g_blocking = { false };
    std::once_flag    g_installOnce;

    // --- Cursor ------------------------------------------------------------

    using ClipCursorFn   = BOOL (WINAPI*)(const RECT*);
    using SetCursorPosFn = BOOL (WINAPI*)(int, int);

    ClipCursorFn   g_realClipCursor   = nullptr;
    SetCursorPosFn g_realSetCursorPos = nullptr;

    BOOL WINAPI hookClipCursor(const RECT* rect) {
      if (g_blocking.load(std::memory_order_relaxed))
        return g_realClipCursor(nullptr);
      return g_realClipCursor(rect);
    }

    BOOL WINAPI hookSetCursorPos(int x, int y) {
      // The game re-centres the cursor every frame for mouse look; while the
      // UI is open the cursor belongs to the UI.
      if (g_blocking.load(std::memory_order_relaxed))
        return TRUE;
      return g_realSetCursorPos(x, y);
    }

    // Points the main executable's user32 imports at the hooks. Only the game
    // module is patched, so the runtime's own UI keeps the real functions.
    void patchExecutableImports() {
      HMODULE user32 = GetModuleHandleW(L"user32.dll");
      if (user32 == nullptr)
        return;
      g_realClipCursor   = reinterpret_cast<ClipCursorFn>(GetProcAddress(user32, "ClipCursor"));
      g_realSetCursorPos = reinterpret_cast<SetCursorPosFn>(GetProcAddress(user32, "SetCursorPos"));
      if (g_realClipCursor == nullptr || g_realSetCursorPos == nullptr)
        return;

      auto* base = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
      auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
      if (dos->e_magic != IMAGE_DOS_SIGNATURE)
        return;
      auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
      const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
      if (dir.VirtualAddress == 0)
        return;

      uint32_t patched = 0;
      for (auto* desc = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress);
           desc->Name != 0; ++desc) {
        if (_stricmp(reinterpret_cast<const char*>(base + desc->Name), "user32.dll") != 0)
          continue;
        for (auto* thunk = reinterpret_cast<IMAGE_THUNK_DATA*>(base + desc->FirstThunk);
             thunk->u1.Function != 0; ++thunk) {
          void* target = nullptr;
          if (reinterpret_cast<void*>(thunk->u1.Function) == reinterpret_cast<void*>(g_realClipCursor))
            target = reinterpret_cast<void*>(&hookClipCursor);
          else if (reinterpret_cast<void*>(thunk->u1.Function) == reinterpret_cast<void*>(g_realSetCursorPos))
            target = reinterpret_cast<void*>(&hookSetCursorPos);
          if (target == nullptr)
            continue;
          DWORD oldProtect = 0;
          if (VirtualProtect(&thunk->u1.Function, sizeof(thunk->u1.Function), PAGE_READWRITE, &oldProtect)) {
            thunk->u1.Function = reinterpret_cast<ULONG_PTR>(target);
            VirtualProtect(&thunk->u1.Function, sizeof(thunk->u1.Function), oldProtect, &oldProtect);
            ++patched;
          }
        }
      }
      Logger::info(str::format("[D3D11InputGuard] cursor imports hooked: ", patched));
    }

    // --- DirectInput -------------------------------------------------------
    // IDirectInputDevice8 vtable slots (identical for the A and W interfaces).

    constexpr size_t kAcquire        = 7;
    constexpr size_t kUnacquire      = 8;
    constexpr size_t kGetDeviceState = 9;
    constexpr size_t kGetDeviceData  = 10;

    using DeviceFn         = HRESULT (WINAPI*)(void*);
    using GetDeviceStateFn = HRESULT (WINAPI*)(void*, DWORD, void*);
    using GetDeviceDataFn  = HRESULT (WINAPI*)(void*, DWORD, void*, DWORD*, DWORD);

    // dinput8 has one device vtable per character set; each is patched once and
    // the hooks find their originals by the calling object's vtable.
    struct PatchedVtable {
      void**           vtable = nullptr;
      DeviceFn         acquire = nullptr;
      DeviceFn         unacquire = nullptr;
      GetDeviceStateFn getDeviceState = nullptr;
      GetDeviceDataFn  getDeviceData = nullptr;
    };
    std::array<PatchedVtable, 4> g_vtables;
    uint32_t g_vtableCount = 0;

    const PatchedVtable* findVtable(void* device) {
      void** vtable = *reinterpret_cast<void***>(device);
      for (uint32_t i = 0; i < g_vtableCount; ++i) {
        if (g_vtables[i].vtable == vtable)
          return &g_vtables[i];
      }
      return nullptr;
    }

    HRESULT WINAPI hookAcquire(void* device) {
      const PatchedVtable* v = findVtable(device);
      // Report success without taking the device: the game keeps its normal
      // state machine, but the mouse stays free for the UI.
      if (g_blocking.load(std::memory_order_relaxed))
        return S_OK;
      return v != nullptr ? v->acquire(device) : E_FAIL;
    }

    // DIERR_NOTACQUIRED / DIERR_INPUTLOST.
    bool isLostAcquisition(HRESULT hr) {
      return hr == HRESULT(0x8007000Cu) || hr == HRESULT(0x8007001Eu);
    }

    HRESULT WINAPI hookGetDeviceState(void* device, DWORD size, void* data) {
      const PatchedVtable* v = findVtable(device);
      if (v == nullptr)
        return E_FAIL;
      if (g_blocking.load(std::memory_order_relaxed)) {
        // Drop an exclusive acquisition (hidden, confined cursor) and report
        // no input while the UI is open.
        v->unacquire(device);
        if (data != nullptr)
          std::memset(data, 0, size);
        return S_OK;
      }
      HRESULT hr = v->getDeviceState(device, size, data);
      // The guard released this device; take it back itself rather than rely
      // on the game. Fallout 4 only reacquires on a focus change, so mouse
      // look stayed dead after the Remix menu was closed.
      if (isLostAcquisition(hr) && SUCCEEDED(v->acquire(device)))
        hr = v->getDeviceState(device, size, data);
      return hr;
    }

    HRESULT WINAPI hookGetDeviceData(void* device, DWORD objectSize, void* objects, DWORD* inOut, DWORD flags) {
      const PatchedVtable* v = findVtable(device);
      if (v == nullptr)
        return E_FAIL;
      if (g_blocking.load(std::memory_order_relaxed)) {
        v->unacquire(device);
        if (inOut != nullptr)
          *inOut = 0;
        return S_OK;
      }
      const DWORD requested = inOut != nullptr ? *inOut : 0;
      HRESULT hr = v->getDeviceData(device, objectSize, objects, inOut, flags);
      if (isLostAcquisition(hr) && SUCCEEDED(v->acquire(device))) {
        if (inOut != nullptr)
          *inOut = requested;
        hr = v->getDeviceData(device, objectSize, objects, inOut, flags);
      }
      return hr;
    }

    bool patchSlot(void** vtable, size_t slot, void* hook) {
      DWORD oldProtect = 0;
      if (!VirtualProtect(&vtable[slot], sizeof(void*), PAGE_READWRITE, &oldProtect))
        return false;
      vtable[slot] = hook;
      VirtualProtect(&vtable[slot], sizeof(void*), oldProtect, &oldProtect);
      return true;
    }

    void patchDeviceVtable(void* device) {
      void** vtable = *reinterpret_cast<void***>(device);
      if (findVtable(device) != nullptr || g_vtableCount >= g_vtables.size())
        return;
      PatchedVtable& entry = g_vtables[g_vtableCount];
      entry.vtable         = vtable;
      entry.acquire        = reinterpret_cast<DeviceFn>(vtable[kAcquire]);
      entry.unacquire      = reinterpret_cast<DeviceFn>(vtable[kUnacquire]);
      entry.getDeviceState = reinterpret_cast<GetDeviceStateFn>(vtable[kGetDeviceState]);
      entry.getDeviceData  = reinterpret_cast<GetDeviceDataFn>(vtable[kGetDeviceData]);
      // Publish the originals before any hook can run.
      ++g_vtableCount;
      patchSlot(vtable, kAcquire, reinterpret_cast<void*>(&hookAcquire));
      patchSlot(vtable, kGetDeviceState, reinterpret_cast<void*>(&hookGetDeviceState));
      patchSlot(vtable, kGetDeviceData, reinterpret_cast<void*>(&hookGetDeviceData));
    }

    void patchDirectInput() {
      // The device vtables live in dinput8.dll, so patching them is inert for
      // games that never create a DirectInput device.
      HMODULE dinput = GetModuleHandleW(L"dinput8.dll");
      if (dinput == nullptr)
        dinput = LoadLibraryExW(L"dinput8.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
      if (dinput == nullptr)
        return;
      using DirectInput8CreateFn = HRESULT (WINAPI*)(HINSTANCE, DWORD, REFIID, void**, IUnknown*);
      auto create = reinterpret_cast<DirectInput8CreateFn>(GetProcAddress(dinput, "DirectInput8Create"));
      if (create == nullptr)
        return;

      static const GUID kIidDirectInput8A = { 0xBF798030, 0x483A, 0x4DA2, { 0xAA, 0x99, 0x5D, 0x64, 0xED, 0x36, 0x97, 0x00 } };
      static const GUID kIidDirectInput8W = { 0xBF798031, 0x483A, 0x4DA2, { 0xAA, 0x99, 0x5D, 0x64, 0xED, 0x36, 0x97, 0x00 } };
      static const GUID kSysMouse         = { 0x6F1D2B60, 0xD5A0, 0x11CF, { 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00 } };
      static const GUID kSysKeyboard      = { 0x6F1D2B61, 0xD5A0, 0x11CF, { 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00 } };

      using CreateDeviceFn = HRESULT (WINAPI*)(void*, REFGUID, void**, IUnknown*);
      for (const GUID* iid : { &kIidDirectInput8A, &kIidDirectInput8W }) {
        void* directInput = nullptr;
        if (FAILED(create(GetModuleHandleW(nullptr), 0x0800, *iid, &directInput, nullptr)) || directInput == nullptr)
          continue;
        auto createDevice = reinterpret_cast<CreateDeviceFn>((*reinterpret_cast<void***>(directInput))[3]);
        for (const GUID* deviceGuid : { &kSysMouse, &kSysKeyboard }) {
          void* device = nullptr;
          if (SUCCEEDED(createDevice(directInput, *deviceGuid, &device, nullptr)) && device != nullptr) {
            patchDeviceVtable(device);
            static_cast<IUnknown*>(device)->Release();
          }
        }
        static_cast<IUnknown*>(directInput)->Release();
      }
      Logger::info(str::format("[D3D11InputGuard] DirectInput device vtables hooked: ", g_vtableCount));
    }

  }

  void install() {
    std::call_once(g_installOnce, []() {
      patchExecutableImports();
      patchDirectInput();
    });
  }

  void setBlocking(bool blocking) {
    const bool wasBlocking = g_blocking.exchange(blocking);
    // Release a confinement the game set before the UI opened, and keep it
    // released while the UI is up.
    if (blocking && g_realClipCursor != nullptr)
      g_realClipCursor(nullptr);
    (void)wasBlocking;
  }

  bool isBlocking() {
    return g_blocking.load(std::memory_order_relaxed);
  }

}
