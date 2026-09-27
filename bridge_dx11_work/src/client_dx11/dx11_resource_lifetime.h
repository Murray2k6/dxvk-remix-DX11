#pragma once

#include <d3d11.h>
#include <atomic>
#include <new>

namespace dx11_capture {

  // The native resource owns this tag. The tag deliberately holds no COM
  // reference to the resource, so destruction cannot form an ownership cycle.
  class ResourceLifetime final : public IUnknown {
  public:
    template<typename T, void (*ReleaseMetadata)(T*)>
    static bool attach(T* resource) {
      if (!resource) return false;
      // Native resources can outlive the caller's LoadLibrary reference.
      static const bool modulePinned = [] {
        HMODULE module = nullptr;
        return GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
          reinterpret_cast<LPCWSTR>(&ResourceLifetime::pinAnchor), &module) != FALSE;
      }();
      if (!modulePinned) return false;
      auto* tag = new (std::nothrow) ResourceLifetime(resource, [](void* object) {
        ReleaseMetadata(static_cast<T*>(object));
      });
      if (!tag) return false;
      // Replacing a prior tag releases old metadata before the caller inserts
      // its replacement. A failed attachment never authorizes caching metadata.
      const HRESULT result = resource->SetPrivateDataInterface(Identifier, tag);
      tag->m_armed = SUCCEEDED(result);
      tag->Release();
      return SUCCEEDED(result);
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** output) override {
      if (!output) return E_POINTER;
      *output = nullptr;
      if (iid != __uuidof(IUnknown)) return E_NOINTERFACE;
      *output = static_cast<IUnknown*>(this);
      AddRef();
      return S_OK;
    }

    ULONG STDMETHODCALLTYPE AddRef() override { return ++m_references; }

    ULONG STDMETHODCALLTYPE Release() override {
      const ULONG remaining = --m_references;
      if (!remaining) delete this;
      return remaining;
    }

  private:
    static constexpr GUID Identifier = { 0x27e9ba57, 0x48ac, 0x4faa, { 0x93, 0x9c, 0x18, 0xcf, 0x4a, 0xec, 0x12, 0x72 } };
    static void pinAnchor() { }
    ResourceLifetime(void* resource, void (*release)(void*)) : m_resource(resource), m_release(release) { }
    ~ResourceLifetime() { if (m_armed) m_release(m_resource); }
    std::atomic<ULONG> m_references { 1 };
    void* m_resource;
    void (*m_release)(void*);
    bool m_armed = false;
  };
}
