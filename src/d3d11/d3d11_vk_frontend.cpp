#include <algorithm>
#include <atomic>
#include <cstring>
#include <cwctype>

#include "d3d11_vk_frontend.h"
#include "d3d11_vk_spirv.h"
#include "d3d11_input_guard.h"

#include "../dxvk/imgui/dxvk_imgui.h"
#include "../dxvk/rtx_render/rtx_context.h"
#include "../dxvk/rtx_render/rtx_option_manager.h"
#include "../dxvk/rtx_render/rtx_options.h"
#include "../dxvk/dxvk_scoped_annotation.h"

#include <d3d11_video_blit_vert.h>
#include <d3d11_vk_ui_composite_frag.h>
#include <d3d11_vk_ui_layer_frag.h>

// Remix's C objects. Global namespace: they are declared there by the C
// header.
struct remix_vkfe_pending_t {
  virtual ~remix_vkfe_pending_t() = default;
};

struct remix_vkfe_instance_t {
  dxvk::Rc<dxvk::DxvkInstance> instance;
  remix_vkfe_frontend          frontend;
};

namespace dxvk {

  // One Remix renderer per process: the first game VkDevice Remix starts on
  // holds the claim until it is destroyed; devices the game creates beside
  // it run without Remix. Taken in planDevice, before Remix changes the
  // device's create info.
  class RendererClaim {
  public:
    RendererClaim() = default;
    RendererClaim(const RendererClaim&) = delete;
    RendererClaim& operator=(const RendererClaim&) = delete;
    ~RendererClaim() {
      if (m_held)
        s_claimed.store(false, std::memory_order_release);
    }

    bool tryTake() {
      bool expected = false;
      m_held = s_claimed.compare_exchange_strong(expected, true, std::memory_order_acq_rel);
      return m_held;
    }

    // Takes over another holder's claim (this one holds none).
    void adopt(RendererClaim& other) {
      m_held = std::exchange(other.m_held, false);
    }

  private:
    bool m_held = false;
    inline static std::atomic<bool> s_claimed { false };
  };

}

struct remix_vkfe_device_t {
  // Declared first, so it is released after the device has shut down.
  dxvk::RendererClaim                          claim;
  std::unique_ptr<dxvk::D3D11VkFrontendDevice> device;
};

namespace dxvk {

  HMODULE D3D11LoadSiblingDxgi();
  void D3D11InitRemixFileSystem();

  namespace {

    struct InstancePending : remix_vkfe_pending_t {
      remix_vkfe_frontend       frontend;
      VkInstanceCreateInfo      info = {};
      VkApplicationInfo         app = {};
      std::vector<std::string>  extensionStorage;
      std::vector<const char*>  extensions;
    };

    struct DevicePending : remix_vkfe_pending_t {
      RendererClaim                       claim;
      remix_vkfe_frontend                 frontend;
      Rc<DxvkInstance>                    instance;
      Rc<DxvkAdapter>                     adapter;
      std::unique_ptr<DxvkDeviceImporter> importer;
    };

    // Remix needs core Vulkan 1.3 on the game's device: timeline semaphores,
    // synchronization2, buffer device address and the Vulkan1x feature
    // structs. The effective device version is min(instance apiVersion,
    // physical device version), so the instance's apiVersion is raised.
    constexpr uint32_t kRequiredApiVersion = VK_API_VERSION_1_3;

    // The game's IDXGIAdapter equivalent from Remix's own dxgi.dll. The D3D11
    // device needs one for IDXGIDevice::GetAdapter.
    Com<IDXGIAdapter> findDxgiAdapter(const Rc<DxvkAdapter>& adapter) {
      HMODULE dxgi = D3D11LoadSiblingDxgi();

      if (!dxgi)
        return nullptr;

      using PFN_CreateDXGIFactory1 = HRESULT (WINAPI*)(REFIID, void**);
      auto createFactory = reinterpret_cast<PFN_CreateDXGIFactory1>(::GetProcAddress(dxgi, "CreateDXGIFactory1"));

      Com<IDXGIFactory1> factory;

      if (!createFactory || FAILED(createFactory(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory))))
        return nullptr;

      const auto& id = adapter->devicePropertiesExt().coreDeviceId;
      Com<IDXGIAdapter> first;

      for (UINT i = 0; ; i++) {
        Com<IDXGIAdapter1> candidate;

        if (factory->EnumAdapters1(i, &candidate) != S_OK)
          break;

        if (first == nullptr)
          first = candidate.ptr();

        DXGI_ADAPTER_DESC1 desc;

        if (id.deviceLUIDValid && SUCCEEDED(candidate->GetDesc1(&desc))
         && !std::memcmp(&desc.AdapterLuid, id.deviceLUID, sizeof(LUID)))
          return candidate.ptr();
      }

      return first;
    }

    // ---------------------------------------------------------------------
    // Remix menu input on the game's window (DX12 / Vulkan): the counterpart
    // of D3D11SwapChain's window procedure for DX11 games.

    struct MenuWindowHook {
      WNDPROC         previous = nullptr;
      Rc<DxvkDevice>  device;
    };

    std::mutex                                  g_menuHookMutex;
    std::unordered_map<HWND, MenuWindowHook>    g_menuHooks;

    LRESULT CALLBACK menuWindowProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
      WNDPROC previous = nullptr;
      Rc<DxvkDevice> device;

      {
        std::lock_guard lock(g_menuHookMutex);
        auto it = g_menuHooks.find(hWnd);

        if (it != g_menuHooks.end()) {
          previous = it->second.previous;
          device   = it->second.device;
        }
      }

      if (device != nullptr) {
        ImGUI& gui = device->getCommon()->getImgui();

        // ImGui only sees the window's messages while the menu is open, so
        // the game's input is untouched otherwise (the Alt+X toggle is
        // polled at present).
        if (gui.isInit() && gui.isMenuOpen()) {
          if (gui.wndProcHandler(hWnd, msg, wParam, lParam))
            return 0;

          const bool key   = (msg >= WM_KEYFIRST && msg <= WM_KEYLAST) || (msg >= WM_SYSKEYDOWN && msg <= WM_SYSDEADCHAR);
          const bool mouse = msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST;

          // The game must not turn the camera or fire while the menu is used.
          if (RtxOptions::blockInputToGameInUI() && (key || mouse || msg == WM_INPUT))
            return 0;

          // Alt+F4, Alt+Enter and a bare Alt still reach the game.
          if ((msg == WM_SYSKEYDOWN || msg == WM_SYSKEYUP) && wParam != VK_MENU && wParam != VK_F4 && wParam != VK_RETURN)
            return 0;

          if (msg == WM_SYSCHAR)
            return 0;
        }
      }

      return previous ? CallWindowProcW(previous, hWnd, msg, wParam, lParam)
                      : DefWindowProcW(hWnd, msg, wParam, lParam);
    }

    void hookMenuWindow(HWND window, const Rc<DxvkDevice>& device) {
      if (!IsWindow(window))
        return;

      std::lock_guard lock(g_menuHookMutex);
      auto& hook = g_menuHooks[window];
      hook.device = device;

      WNDPROC current = reinterpret_cast<WNDPROC>(GetWindowLongPtrW(window, GWLP_WNDPROC));

      if (current != menuWindowProc) {
        hook.previous = current;
        SetWindowLongPtrW(window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(menuWindowProc));
        Logger::info(str::format("[Remix-VkFrontend] Remix menu input hooked on HWND ", uintptr_t(window)));
      }
    }

    void unhookMenuWindows(const Rc<DxvkDevice>& device) {
      std::lock_guard lock(g_menuHookMutex);

      for (auto it = g_menuHooks.begin(); it != g_menuHooks.end(); ) {
        if (it->second.device != device) {
          ++it;
          continue;
        }

        // Only restore when no one hooked the window after Remix.
        if (IsWindow(it->first)
         && reinterpret_cast<WNDPROC>(GetWindowLongPtrW(it->first, GWLP_WNDPROC)) == menuWindowProc)
          SetWindowLongPtrW(it->first, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(it->second.previous));

        it = g_menuHooks.erase(it);
      }
    }

  }


  D3D11VkFrontendDevice::D3D11VkFrontendDevice(
          remix_vkfe_frontend frontend,
    const Rc<DxvkInstance>&   instance,
    const Rc<DxvkAdapter>&    adapter,
    const Rc<DxvkDevice>&     device,
          bool                sharesGameQueue)
  : m_frontend(frontend), m_sharesGameQueue(sharesGameQueue),
    m_instance(instance), m_adapter(adapter), m_device(device) {
    Com<IDXGIAdapter> dxgiAdapter = findDxgiAdapter(adapter);

    if (dxgiAdapter == nullptr)
      throw DxvkError("Remix front end: Remix's dxgi.dll is not next to d3d11.dll");

    m_dxgiDevice = new D3D11DXGIDevice(dxgiAdapter.ptr(), instance, adapter, device,
      D3D_FEATURE_LEVEL_11_1, 0);

    if (FAILED(m_dxgiDevice->QueryInterface(__uuidof(ID3D11Device), reinterpret_cast<void**>(&m_d3d11Device))))
      throw DxvkError("Remix front end: no ID3D11Device");

    m_d3d11Device->GetImmediateContext(&m_d3d11Context);
    m_context = static_cast<D3D11ImmediateContext*>(m_d3d11Context.ptr());

    DxvkBufferCreateInfo counterInfo = {};
    counterInfo.size   = kCounterSlots * sizeof(uint32_t);
    counterInfo.usage  = VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_COUNTER_BUFFER_BIT_EXT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    counterInfo.stages = VK_PIPELINE_STAGE_TRANSFORM_FEEDBACK_BIT_EXT | VK_PIPELINE_STAGE_HOST_BIT;
    counterInfo.access = VK_ACCESS_TRANSFORM_FEEDBACK_COUNTER_WRITE_BIT_EXT | VK_ACCESS_HOST_READ_BIT;
    m_counterBuffer = device->createBuffer(counterInfo,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
      DxvkMemoryStats::Category::RTXBuffer, "Remix front-end XFB counters");

    DxvkBufferCreateInfo argsInfo = {};
    argsInfo.size   = kArgsReadbackSize;
    argsInfo.usage  = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    argsInfo.stages = VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_HOST_BIT;
    argsInfo.access = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_HOST_READ_BIT | VK_ACCESS_HOST_WRITE_BIT;
    m_argsReadback = device->createBuffer(argsInfo,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT,
      DxvkMemoryStats::Category::RTXBuffer, "Remix front-end command stream copies");

    // Starfield: layouts verified against shaders of game version 1.16.244
    // (starfield-decomp MODLOG.md); the walk checks every value it reads.
    {
      wchar_t exe[MAX_PATH] = {};
      GetModuleFileNameW(nullptr, exe, MAX_PATH);
      std::wstring name(exe);
      const size_t slash = name.find_last_of(L"\\/");
      name = slash == std::wstring::npos ? name : name.substr(slash + 1);
      for (auto& c : name)
        c = wchar_t(towlower(c));
      m_starfieldMaterials = frontend == REMIX_VKFE_FRONTEND_DX12_VKD3D && name == L"starfield.exe";

      if (m_starfieldMaterials)
        Logger::info("[Remix-VkFrontend] Starfield: bindless material tables are walked per draw");
    }

    DxvkBufferCreateInfo bakeInfo = {};
    bakeInfo.size   = VkDeviceSize(kBakeSlots) * kBakesPerFrame * REMIX_VKFE_MAX_BAKE_CASCADES * 64u;
    bakeInfo.usage  = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    bakeInfo.stages = VK_PIPELINE_STAGE_VERTEX_INPUT_BIT | VK_PIPELINE_STAGE_HOST_BIT;
    bakeInfo.access = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_HOST_WRITE_BIT;
    m_bakeMatrices = device->createBuffer(bakeInfo,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
      DxvkMemoryStats::Category::RTXBuffer, "Remix front-end terrain bake matrices");

    for (uint32_t i = 0; i < kFrameFences; i++) {
      VkFenceCreateInfo fenceInfo = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };

      if (device->vkd()->vkCreateFence(device->handle(), &fenceInfo, nullptr, &m_frameFences[i]) != VK_SUCCESS)
        throw DxvkError("Remix front end: vkCreateFence failed");
    }

    // UI composite: full-screen triangle (the video blit's vertex shader)
    // and the composite fragment shader.
    {
      const SpirvCodeBuffer vsCode(d3d11_video_blit_vert);
      const SpirvCodeBuffer fsCode(d3d11_vk_ui_composite_frag);

      const std::array<DxvkResourceSlot, 3> fsSlots = {{
        { 0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_IMAGE_VIEW_TYPE_2D },
        { 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_IMAGE_VIEW_TYPE_2D },
        { 2, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_IMAGE_VIEW_TYPE_2D },
      }};

      m_uiCompositeVs = device->createShader(VK_SHADER_STAGE_VERTEX_BIT, 0, nullptr, { 0u, 1u }, vsCode);
      m_uiCompositeFs = device->createShader(VK_SHADER_STAGE_FRAGMENT_BIT,
        uint32_t(fsSlots.size()), fsSlots.data(), { 1u, 1u, 0u, 0u }, fsCode);

      const SpirvCodeBuffer layerCode(d3d11_vk_ui_layer_frag);
      m_uiLayerFs = device->createShader(VK_SHADER_STAGE_FRAGMENT_BIT,
        2u, fsSlots.data(), { 1u, 1u, 0u, 0u }, layerCode);
    }

    Logger::info(str::format("[Remix-VkFrontend] Remix attached to the game's VkDevice (",
      frontend == REMIX_VKFE_FRONTEND_DX12_VKD3D ? "DX12 via vkd3d-proton" : "Vulkan layer", ")"));
  }


  D3D11VkFrontendDevice::~D3D11VkFrontendDevice() {
    // The window keeps receiving messages after Remix is gone.
    unhookMenuWindows(m_device);

    // The game destroys its VkDevice right after this returns: drain Remix's
    // CS thread and GPU work first.
    if (m_context) {
      m_context->Flush();
      m_context->SynchronizeCsThread(DxvkCsThread::SynchronizeAll);
    }

    m_device->waitForIdle();

    for (auto& entry : m_swapchains)
      destroySwapchain(entry.second);

    for (VkFence& fence : m_frameFences) {
      if (fence != VK_NULL_HANDLE)
        m_device->vkd()->vkDestroyFence(m_device->handle(), fence, nullptr);
      fence = VK_NULL_HANDLE;
    }

    m_pendingIndirect.clear();
    m_countedReserve.clear();
    m_counterBuffer = nullptr;
    m_argsReadback = nullptr;

    for (auto& s : m_uiSnapshots)
      s = UiSnapshot();

    for (auto& s : m_bakeSlots)
      s = BakeSlot();

    m_bakeScratch.clear();
    m_bakeMatrices = nullptr;
    m_retiredBakeImages.clear();

    for (auto& l : m_uiLayers)
      l = UiLayer();

    m_uiLayerDepth.clear();
    m_uiLayerCommands.clear();
    m_uiLayerIncomplete.clear();
    m_uiLayerFs = nullptr;

    m_uiSnapshotCommands.clear();
    m_uiCompositeVs = nullptr;
    m_uiCompositeFs = nullptr;
    m_frameBytes.clear();
    m_submitBytes.clear();
    m_swapchains.clear();
    m_recorded.clear();
    m_frameDraws.clear();
    m_captureUsers.clear();
    m_captureCurrent = nullptr;
    m_captureChunks.clear();
    m_capture = D3D11VkCaptureState();
    m_pipelines.clear();
    m_context = nullptr;
    m_d3d11Context = nullptr;
    m_d3d11Device = nullptr;
    m_dxgiDevice = nullptr;
  }


  void D3D11VkFrontendDevice::forwardFrame(
          VkQueue                     queue,
          VkSemaphore                 gameDone,
          VkSemaphore                 remixDone,
          bool                        waitEmitted,
          bool                        signalEmitted) {
    // Remix's CS thread already queued the wait on gameDone: let it signal
    // remixDone too, after whatever it recorded.
    if (waitEmitted) {
      if (!signalEmitted) {
        auto ctxLock = m_context->LockContext();
        m_context->EmitCs([remixDone] (DxvkContext* ctx) {
          static_cast<RtxContext*>(ctx)->flushCommandListWithSync(VK_NULL_HANDLE, remixDone);
        });
        m_context->FlushCsChunk();
      }

      m_context->SynchronizeCsThread(DxvkCsThread::SynchronizeAll);
      m_device->lockSubmission();
      m_device->unlockSubmission();
      return;
    }

    // Nothing of Remix's waits on gameDone: hand it straight to remixDone on
    // the game's queue, so the present shows the game's unmodified frame.
    const VkPipelineStageFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;

    VkSubmitInfo submit = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    submit.waitSemaphoreCount   = 1;
    submit.pWaitSemaphores      = &gameDone;
    submit.pWaitDstStageMask    = &stage;
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores    = &remixDone;

    lockQueue(queue);
    m_device->vkd()->vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE);
    unlockQueue(queue);
  }


  void D3D11VkFrontendDevice::lockQueue(VkQueue queue) {
    if (m_sharesGameQueue && queue == m_device->queues().graphics.queueHandle)
      m_device->lockSubmissionUnsynchronized();
  }


  void D3D11VkFrontendDevice::unlockQueue(VkQueue queue) {
    if (m_sharesGameQueue && queue == m_device->queues().graphics.queueHandle)
      m_device->unlockSubmission();
  }


  VkSemaphore D3D11VkFrontendDevice::createSemaphore() {
    VkSemaphoreCreateInfo info = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    VkSemaphore semaphore = VK_NULL_HANDLE;

    if (m_device->vkd()->vkCreateSemaphore(m_device->handle(), &info, nullptr, &semaphore) != VK_SUCCESS)
      throw DxvkError("Remix front end: vkCreateSemaphore failed");

    return semaphore;
  }


  void D3D11VkFrontendDevice::destroySwapchain(D3D11VkSwapchain& swapchain) {
    auto vkd = m_device->vkd();

    for (VkSemaphore s : swapchain.gameDone)
      vkd->vkDestroySemaphore(m_device->handle(), s, nullptr);

    for (VkSemaphore s : swapchain.remixDone)
      vkd->vkDestroySemaphore(m_device->handle(), s, nullptr);

    swapchain.gameDone.clear();
    swapchain.remixDone.clear();
    swapchain.images.clear();
    swapchain.backbuffer = nullptr;
    swapchain.finalCopy = nullptr;
    swapchain.uiOut = nullptr;
  }


  void D3D11VkFrontendDevice::onSwapchain(
          VkSwapchainKHR              swapchain,
    const VkSwapchainCreateInfoKHR*   info,
          uint32_t                    imageCount,
    const VkImage*                    images) {
    ScopedCpuProfileZone();
    std::lock_guard lock(m_mutex);

    // Swap chain images are referenced by in-flight Remix work.
    m_context->Flush();
    m_context->SynchronizeCsThread(DxvkCsThread::SynchronizeAll);
    m_device->waitForIdle();

    auto& sc = m_swapchains[swapchain];
    destroySwapchain(sc);

    constexpr VkImageUsageFlags kNeeded = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;

    if ((info->imageUsage & kNeeded) != kNeeded) {
      // The front ends add these bits when the swap chain is created; a
      // swap chain without them cannot receive Remix's frame.
      Logger::err(str::format("[Remix-VkFrontend] swap chain usage 0x", std::hex, info->imageUsage,
        " lacks TRANSFER_SRC|TRANSFER_DST; Remix cannot write its frame"));
      m_swapchains.erase(swapchain);
      return;
    }

    sc.format = info->imageFormat;
    sc.extent = info->imageExtent;
    sc.usage  = info->imageUsage;

    DxvkImageCreateInfo imageInfo;
    imageInfo.type        = VK_IMAGE_TYPE_2D;
    imageInfo.format      = info->imageFormat;
    imageInfo.flags       = 0;
    imageInfo.sampleCount = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.extent      = { info->imageExtent.width, info->imageExtent.height, 1 };
    imageInfo.numLayers   = 1;
    imageInfo.mipLevels   = 1;
    imageInfo.usage       = info->imageUsage;
    imageInfo.stages      = 0;
    imageInfo.access      = 0;
    imageInfo.tiling      = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.layout      = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    imageInfo.shared      = VK_TRUE;

    for (uint32_t i = 0; i < imageCount; i++) {
      sc.images.push_back(m_device->createImageFromVkImage(imageInfo, images[i]));
      sc.gameDone.push_back(createSemaphore());
      sc.remixDone.push_back(createSemaphore());
    }

    // Remix's render target: same format and size, with the usages
    // injectRTX needs. Storage only where the format allows it (sRGB
    // formats do not).
    DxvkImageCreateInfo bbInfo = imageInfo;
    bbInfo.usage  = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT
                  | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    bbInfo.stages = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT
                  | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    bbInfo.access = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT
                  | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
    bbInfo.layout = VK_IMAGE_LAYOUT_GENERAL;
    bbInfo.shared = VK_FALSE;

    if (m_adapter->formatProperties(info->imageFormat).optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) {
      bbInfo.usage  |= VK_IMAGE_USAGE_STORAGE_BIT;
      bbInfo.access |= VK_ACCESS_SHADER_WRITE_BIT;
    }

    sc.backbuffer = m_device->createImage(bbInfo, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
      DxvkMemoryStats::Category::RTXRenderTarget, "Remix front-end backbuffer");

    // UI composite images: the game's presented frame (sampled) and the
    // composite output (rendered, then copied to the swap chain image).
    DxvkImageCreateInfo uiInfo = imageInfo;
    uiInfo.usage  = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT
                  | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    uiInfo.stages = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT
                  | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    uiInfo.access = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT
                  | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
    uiInfo.layout = VK_IMAGE_LAYOUT_GENERAL;
    uiInfo.shared = VK_FALSE;

    sc.finalCopy = m_device->createImage(uiInfo, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
      DxvkMemoryStats::Category::RTXRenderTarget, "Remix front-end UI final");
    sc.uiOut = m_device->createImage(uiInfo, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
      DxvkMemoryStats::Category::RTXRenderTarget, "Remix front-end UI composite");

    Logger::info(str::format("[Remix-VkFrontend] swap chain ", imageCount, " x ",
      info->imageExtent.width, "x", info->imageExtent.height, " format ", info->imageFormat));
  }


  void D3D11VkFrontendDevice::onSwapchainWindow(VkSwapchainKHR swapchain, HWND window) {
    {
      std::lock_guard lock(m_mutex);
      auto it = m_swapchains.find(swapchain);

      if (it == m_swapchains.end())
        return;

      it->second.window = window;
    }

    hookMenuWindow(window, m_device);
    // DirectInput / cursor hooks that hold the game back while the menu is open.
    D3D11InputGuard::install();
  }


  void D3D11VkFrontendDevice::onSwapchainDestroy(VkSwapchainKHR swapchain) {
    std::lock_guard lock(m_mutex);

    auto it = m_swapchains.find(swapchain);

    if (it == m_swapchains.end())
      return;

    m_context->Flush();
    m_context->SynchronizeCsThread(DxvkCsThread::SynchronizeAll);
    m_device->waitForIdle();

    destroySwapchain(it->second);
    m_swapchains.erase(it);
  }


  void D3D11VkFrontendDevice::onPipeline(
    const remix_vkfe_pipeline_desc*   desc,
          remix_vkfe_capture_plan*    plan) {
    ScopedCpuProfileZone();

    *plan = remix_vkfe_capture_plan{};

    auto pipeline = std::make_shared<D3D11VkPipeline>();
    pipeline->key   = desc->key;
    pipeline->state = *desc;

    for (uint32_t s = 0; s < REMIX_VKFE_STAGE_COUNT; s++) {
      const remix_vkfe_shader& src = desc->stages[s];

      if (!src.code || !src.size)
        continue;

      auto& dst = pipeline->stages[s];
      dst.format = src.format;
      dst.code.assign(reinterpret_cast<const uint8_t*>(src.code), reinterpret_cast<const uint8_t*>(src.code) + src.size);

      if (src.entry_point)
        dst.entryPoint = src.entry_point;
    }

    pipeline->attributes.assign(desc->attributes, desc->attributes + desc->attribute_count);
    pipeline->semantics.reserve(desc->attribute_count);

    for (auto& a : pipeline->attributes) {
      pipeline->semantics.emplace_back(a.semantic ? a.semantic : "");
      a.semantic = nullptr;
    }

    for (size_t i = 0; i < pipeline->attributes.size(); i++)
      pipeline->attributes[i].semantic = pipeline->semantics[i].c_str();

    pipeline->bindings.assign(desc->bindings, desc->bindings + desc->binding_count);

    // The copy owns the arrays now; the caller's pointers are stale.
    for (auto& stage : pipeline->state.stages)
      stage = remix_vkfe_shader{};

    pipeline->state.attributes = pipeline->attributes.data();
    pipeline->state.bindings   = pipeline->bindings.data();

    // Identity that survives across runs: the shaders and the state that
    // shapes what Remix sees. vkd3d-proton compiles DXIL deterministically,
    // so DX12 pipelines hash the same every launch too.
    {
      XXH64_hash_t h = 0x52454d4958564b31ull;  // "REMIXVK1"

      for (const auto& stage : pipeline->stages) {
        if (!stage.code.empty())
          h = XXH3_64bits_withSeed(stage.code.data(), stage.code.size(), h);
      }

      const uint32_t state[] = {
        uint32_t(desc->topology), desc->blend_enable, uint32_t(desc->src_color_blend), uint32_t(desc->dst_color_blend),
        desc->depth_test, desc->depth_write, uint32_t(desc->depth_compare), uint32_t(desc->cull_mode),
      };

      pipeline->stableHash = XXH3_64bits_withSeed(state, sizeof(state), h);
    }

    // The game already streams out of this pipeline: a second set of
    // transform feedback decorations would collide with its own. The pixel
    // analysis still runs (materials).
    if (!desc->has_stream_output) {
      D3D11VkBuildCapturePlan(*pipeline);
    } else if (pipeline->stages[REMIX_VKFE_STAGE_PIXEL].format == REMIX_VKFE_SHADER_SPIRV) {
      const auto& ps = pipeline->stages[REMIX_VKFE_STAGE_PIXEL].code;
      pipeline->pixelAnalysis = D3D11VkAnalyzePixelShader(reinterpret_cast<const uint32_t*>(ps.data()), ps.size() / 4);
    }

    // Depth-only pipelines (prepasses, shadow maps): their draws are never
    // committed (captureFrame drops depth-only draws), so no capture variant
    // is compiled for them. A write mask set per draw keeps its variant.
    if (desc->render_target_count == 0 || (desc->color_write_mask == 0 && !desc->bake_blocking_dynamic_state))
      pipeline->captureSupported = false;

    if (pipeline->captureSupported) {
      plan->supported  = 1;
      plan->stage      = pipeline->captureStage;
      plan->spirv      = pipeline->captureSpirv.data();
      plan->spirv_size = pipeline->captureSpirv.size() * sizeof(uint32_t);
      plan->stride     = pipeline->captureStride;

      // GPU-driven draws of this layout (vkd3d ExecuteIndirect with root
      // constants): the capture stage reading its constants from the stream.
      // Vertex stage only: DrawIndex exists there.
      if (desc->indirect_constant_count && pipeline->captureStage == REMIX_VKFE_STAGE_VERTEX
       && D3D11VkPatchSpirvForIndirect(pipeline->captureSpirv.data(), pipeline->captureSpirv.size(),
                                       desc->indirect_constants, desc->indirect_constant_count,
                                       desc->indirect_stride, pipeline->indirectSpirv)) {
        plan->indirect_spirv      = pipeline->indirectSpirv.data();
        plan->indirect_spirv_size = pipeline->indirectSpirv.size() * sizeof(uint32_t);
      }
    }

    // Terrain bake variant: opaque, depth-writing, single-sampled pipelines
    // with a vertex shader only, an 8-bit RGBA target for the albedo, and
    // either the auto terrain rule's several colour layers or tagged
    // terrain textures in use. Each is a second pipeline compile, so only
    // pipelines that can draw terrain get one.
    {
      const auto& s = pipeline->state;
      uint32_t colourRoles = 0;

      for (const auto& t : pipeline->pixelAnalysis.textures)
        colourRoles += (!t.normal && t.metallicChannel < 0 && t.roughnessChannel < 0 && !t.emissive) ? 1u : 0u;

      const bool wanted = TerrainBaker::enableBaking()
        && ((RtxOptions::dx11AutoTerrainBlend() && colourRoles >= 4) || !RtxOptions::terrainTextures().empty());

      const bool vertexOnly = pipeline->stages[REMIX_VKFE_STAGE_VERTEX].format == REMIX_VKFE_SHADER_SPIRV
        && !pipeline->stages[REMIX_VKFE_STAGE_VERTEX].code.empty()
        && pipeline->stages[REMIX_VKFE_STAGE_HULL].code.empty() && pipeline->stages[REMIX_VKFE_STAGE_DOMAIN].code.empty()
        && pipeline->stages[REMIX_VKFE_STAGE_GEOMETRY].code.empty() && pipeline->stages[REMIX_VKFE_STAGE_MESH].code.empty();

      uint32_t albedoTarget = ~0u;

      for (uint32_t r = 0; r < std::min(s.render_target_count, 8u) && albedoTarget == ~0u; r++) {
        switch (s.render_target_formats[r]) {
          case VK_FORMAT_R8G8B8A8_UNORM: case VK_FORMAT_R8G8B8A8_SRGB:
          case VK_FORMAT_B8G8R8A8_UNORM: case VK_FORMAT_B8G8R8A8_SRGB:
            albedoTarget = r;
            break;
          default:
            break;
        }
      }

      const auto& vs = pipeline->stages[REMIX_VKFE_STAGE_VERTEX].code;

      if (wanted && vertexOnly && albedoTarget != ~0u && s.depth_write && !s.blend_enable
       && s.samples <= VK_SAMPLE_COUNT_1_BIT && !s.bake_blocking_dynamic_state && vs.size() % 4 == 0
       && D3D11VkPatchSpirvForBake(reinterpret_cast<const uint32_t*>(vs.data()), vs.size() / 4,
                                   pipeline->bakeSpirv, pipeline->bakeMatrixLocation)) {
        pipeline->bakeSupported    = true;
        pipeline->bakeAlbedoTarget = albedoTarget;

        plan->bake_spirv           = pipeline->bakeSpirv.data();
        plan->bake_spirv_size      = pipeline->bakeSpirv.size() * sizeof(uint32_t);
        plan->bake_matrix_location = pipeline->bakeMatrixLocation;
      }
    }

    std::lock_guard lock(m_mutex);
    m_pipelines[desc->key] = std::move(pipeline);
  }


  void D3D11VkFrontendDevice::onPipelineDestroy(uint64_t key) {
    std::lock_guard lock(m_mutex);
    m_pipelines.erase(key);
  }


  namespace {

    // Vertices transform feedback writes for one instance of a draw: the
    // stream is always a list of whole primitives. 0 = not a triangle draw.
    uint32_t capturedVertexCount(VkPrimitiveTopology topology, uint32_t count) {
      switch (topology) {
        case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST:
          return count - count % 3;
        case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP:
        case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN:
          return count >= 3 ? 3 * (count - 2) : 0;
        default:
          // Points and lines have no surface to trace; patches and
          // adjacency come out of a tessellation or geometry stage whose
          // output count is unknown on the CPU.
          return 0;
      }
    }

  }


  bool D3D11VkFrontendDevice::allocateCapture(
          VkCommandBuffer             commandBuffer,
          VkDeviceSize                size,
          Rc<DxvkBuffer>&             buffer,
          VkDeviceSize&               offset) {
    constexpr VkDeviceSize kChunkSize   = 64ull << 20;
    constexpr VkDeviceSize kAlignment   = 256;
    // Frames a chunk stays untouched after its last command buffer is reset
    // or submitted, covering the GPU frames in flight.
    constexpr uint64_t     kRetireFrames = 3;
    // Per-frame cap on newly captured bytes; the rest of a huge frame is
    // captured on the following frames (the DX11 path's budget does the
    // same).
    constexpr VkDeviceSize kFrameBudget = 512ull << 20;

    size = align(size, kAlignment);

    if (m_captureBytesThisFrame + size > kFrameBudget)
      return false;

    if (!m_captureCurrent || m_captureCurrent->used + size > m_captureCurrent->buffer->info().size) {
      m_captureCurrent = nullptr;

      for (auto& chunk : m_captureChunks) {
        if (chunk->users == 0 && chunk->lastFrame + kRetireFrames <= m_presentCount
         && chunk->buffer->info().size >= size) {
          chunk->used = 0;
          m_captureCurrent = chunk.get();
          break;
        }
      }

      if (!m_captureCurrent) {
        DxvkBufferCreateInfo info = {};
        info.size   = std::max(kChunkSize, size);
        info.usage  = VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_BUFFER_BIT_EXT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT
                    | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        info.stages = VK_PIPELINE_STAGE_TRANSFORM_FEEDBACK_BIT_EXT | VK_PIPELINE_STAGE_VERTEX_INPUT_BIT
                    | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
        info.access = VK_ACCESS_TRANSFORM_FEEDBACK_WRITE_BIT_EXT | VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT
                    | VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT;

        auto chunk = std::make_unique<D3D11VkCaptureChunk>();
        chunk->buffer = m_device->createBuffer(info, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
          DxvkMemoryStats::Category::RTXBuffer, "Remix front-end capture");
        m_captureCurrent = chunk.get();
        m_captureChunks.push_back(std::move(chunk));
      }
    }

    D3D11VkCaptureChunk* chunk = m_captureCurrent;

    auto& users = m_captureUsers[commandBuffer];

    if (std::find(users.begin(), users.end(), chunk) == users.end()) {
      users.push_back(chunk);
      chunk->users++;
    }

    buffer = chunk->buffer;
    offset = chunk->used;
    chunk->used += size;
    m_captureBytesThisFrame += size;
    return true;
  }


  VkDeviceSize D3D11VkFrontendDevice::storageCopyLimit(const remix_vkfe_binding& b, VkDeviceSize defaultLimit) const {
    // Starfield's material tables (CommonMaterialEntry t9, MaterialLayerEntry
    // t25, space 2) are indexed by material, 288 / 40 bytes per entry.
    if (m_starfieldMaterials && b.space == 2 && (b.slot == 9 || b.slot == 25))
      return std::max<VkDeviceSize>(defaultLimit, 4ull << 20);

    return defaultLimit;
  }


  bool D3D11VkFrontendDevice::allocateArgsCopy(VkDeviceSize size, VkDeviceSize& offset) {
    constexpr VkDeviceSize kAlignment = 256;
    size = align(size, kAlignment);

    if (m_argsReadback == nullptr || size > kArgsReadbackSize / 4)
      return false;

    if (m_argsNext + size > kArgsReadbackSize)
      m_argsNext = 0;

    offset = m_argsNext;
    m_argsNext += size;
    return true;
  }


  void D3D11VkFrontendDevice::releaseCaptureUsers(VkCommandBuffer commandBuffer) {
    auto it = m_captureUsers.find(commandBuffer);

    if (it == m_captureUsers.end())
      return;

    for (D3D11VkCaptureChunk* chunk : it->second) {
      chunk->users--;
      chunk->lastFrame = m_presentCount;
    }

    m_captureUsers.erase(it);
  }


  void D3D11VkFrontendDevice::onDraw(
    const remix_vkfe_draw_desc*       desc,
          remix_vkfe_draw_capture*    capture) {
    // Upper bound on bytes copied per buffer binding at record time. D3D12
    // root CBVs and Vulkan UBOs are at most 64 KiB; storage buffers are read
    // for engine light lists (thousands of 96-224 B records).
    constexpr VkDeviceSize kMaxConstantBytes = 64ull << 10;
    constexpr VkDeviceSize kMaxStorageBytes  = 512ull << 10;

    ScopedCpuProfileZone();
    *capture = remix_vkfe_draw_capture{};

    D3D11VkDraw draw;
    draw.desc = *desc;
    draw.vertexBuffers.assign(desc->vertex_buffers, desc->vertex_buffers + desc->vertex_buffer_count);
    draw.bindings.assign(desc->bindings, desc->bindings + desc->binding_count);
    draw.bindingBytes.resize(draw.bindings.size());

    std::lock_guard lock(m_mutex);
    draw.imageSeq = m_imageDestroySeq;

    for (size_t i = 0; i < draw.bindings.size(); i++) {
      auto& b = draw.bindings[i];

      const bool constants = b.kind == REMIX_VKFE_BINDING_CONSTANT_BUFFER;
      const bool storage   = b.kind == REMIX_VKFE_BINDING_STORAGE_BUFFER;
      const bool push      = b.kind == REMIX_VKFE_BINDING_PUSH_CONSTANTS;

      if ((constants || storage || push) && b.host_data && b.size) {
        const size_t n = size_t(std::min(b.size, storage ? storageCopyLimit(b, kMaxStorageBytes) : kMaxConstantBytes));
        const uint8_t* src = reinterpret_cast<const uint8_t*>(b.host_data);

        // Push constants live in the caller's command state and change from
        // draw to draw: never shared. Shadowed default-heap constants
        // (host_data_per_draw) are system memory refilled between draws:
        // shared only while the bytes are unchanged.
        auto cached = push ? m_frameBytes.end() : m_frameBytes.find(b.host_data);

        if (cached != m_frameBytes.end() && b.host_data_per_draw
         && (cached->second->size() < n || std::memcmp(cached->second->data(), src, n) != 0))
          cached = m_frameBytes.end();

        if (cached != m_frameBytes.end() && cached->second->size() >= n) {
          draw.bindingBytes[i] = cached->second;
        } else {
          auto copy = std::make_shared<std::vector<uint8_t>>(src, src + n);
          draw.bindingBytes[i] = copy;

          if (!push)
            m_frameBytes[b.host_data] = copy;
        }
      }

      // The descriptor heap handle is no pointer to bytes: it stays, and is
      // only used through remix_vkd3d_heap_view (which checks it is live).
      if (b.kind != REMIX_VKFE_BINDING_DESCRIPTOR_HEAP)
        b.host_data = nullptr;
    }

    // Vertex and index host pointers stay valid only during the call.
    for (auto& vb : draw.vertexBuffers)
      vb.host_data = nullptr;

    draw.desc.index_host_data = nullptr;
    draw.desc.vertex_buffers  = nullptr;
    draw.desc.bindings        = nullptr;

    auto pipeline = m_pipelines.find(desc->pipeline_key);

    if (pipeline != m_pipelines.end())
      draw.pipeline = pipeline->second;

    // Capture: triangle draws of pipelines with a capture variant. Depth-only
    // draws are never committed (captureFrame); their bindings still feed the
    // sun and the camera, so they are recorded, just not replayed.
    const bool gpuCounted = desc->indirect_buffer
      || (draw.pipeline && (draw.pipeline->state.topology == VK_PRIMITIVE_TOPOLOGY_MAX_ENUM
                         || draw.pipeline->captureCountedOnGpu));
    const bool depthOnlyDraw = desc->render_target_count == 0
      || (draw.pipeline && draw.pipeline->state.color_write_mask == 0);

    if (depthOnlyDraw) {
      // No capture.
    } else if (draw.pipeline && draw.pipeline->captureSupported && gpuCounted) {
      // GPU-counted (indirect, dynamic topology, tessellation / GS): the
      // vertex count is decided on the GPU (Unreal's culling writes the
      // arguments every frame), so reserve a range and let the XFB byte
      // counter say how much was written. Transform feedback stops at the
      // end of the range, never past it. The range per pipeline adapts to
      // what earlier captures wrote (processPendingIndirect).
      constexpr VkDeviceSize kInitialReserve = 1ull << 20;
      auto hint = m_countedReserve.find(draw.pipeline->key);
      const VkDeviceSize reserve = hint != m_countedReserve.end() ? hint->second : kInitialReserve;

      // Generated commands are split per command from a copy of the stream
      // (and the count): without one the capture could not be split.
      bool argsReady = !desc->generated_commands;

      if (desc->generated_commands && desc->indirect_stride && desc->indirect_draw_count) {
        constexpr VkDeviceSize kMaxCommands = 131072;
        const VkDeviceSize bytes = std::min<VkDeviceSize>(desc->indirect_draw_count, kMaxCommands) * desc->indirect_stride;
        VkDeviceSize offset = 0;

        if (allocateArgsCopy(align(bytes, VkDeviceSize(16)) + 16, offset)) {
          draw.argsCopyOffset  = offset;
          draw.argsCopySize    = bytes;
          draw.countCopyOffset = offset + align(bytes, VkDeviceSize(16));
          *reinterpret_cast<uint32_t*>(m_argsReadback->mapPtr(draw.countCopyOffset)) = kArgsCountSentinel;

          const DxvkBufferSliceHandle args = m_argsReadback->getSliceHandle();
          capture->args_copy_buffer  = args.handle;
          capture->args_copy_offset  = args.offset + draw.argsCopyOffset;
          capture->args_copy_size    = bytes;
          capture->count_copy_buffer = args.handle;
          capture->count_copy_offset = args.offset + draw.countCopyOffset;
          argsReady = true;
        }
      }

      if (argsReady && allocateCapture(desc->command_buffer, reserve, draw.captureBuffer, draw.captureOffset)) {
        const DxvkBufferSliceHandle slice   = draw.captureBuffer->getSliceHandle();
        const DxvkBufferSliceHandle counter = m_counterBuffer->getSliceHandle();

        draw.captureSize  = reserve;
        draw.captureChunk = m_captureCurrent;
        draw.counterSlot  = m_counterNext++ % kCounterSlots;

        capture->capture        = 1;
        capture->buffer         = slice.handle;
        capture->offset         = slice.offset + draw.captureOffset;
        capture->size           = reserve;
        capture->counter_buffer = counter.handle;
        capture->counter_offset = counter.offset + VkDeviceSize(draw.counterSlot) * sizeof(uint32_t);
      }
    } else if (draw.pipeline && draw.pipeline->captureSupported) {
      const uint32_t perInstance = capturedVertexCount(draw.pipeline->state.topology, desc->vertex_or_index_count);
      const uint32_t vertices = perInstance * std::max(desc->instance_count, 1u);
      const VkDeviceSize bytes = VkDeviceSize(vertices) * draw.pipeline->captureStride;

      if (vertices && allocateCapture(desc->command_buffer, bytes, draw.captureBuffer, draw.captureOffset)) {
        const DxvkBufferSliceHandle slice = draw.captureBuffer->getSliceHandle();

        draw.captureSize      = bytes;
        draw.capturedVertices = vertices;

        capture->capture = 1;
        capture->buffer  = slice.handle;
        capture->offset  = slice.offset + draw.captureOffset;
        capture->size    = bytes;
      }
    }

    // Splat-blended terrain: baked by the caller into Remix's cascades.
    if (draw.pipeline && draw.pipeline->bakeSupported)
      prepareBake(draw, capture);

    // Starfield's material tables in GPU-only memory: read back once per
    // frame per table, for the material walk (applyMaterialChain).
    if (m_starfieldMaterials && desc->generated_commands && capture->capture) {
      if (m_tableCopiesFrame != m_presentCount) {
        m_tableCopies.clear();
        m_tableCopiesFrame = m_presentCount;
      }

      for (size_t i = 0; i < draw.bindings.size(); i++) {
        const auto& b = draw.bindings[i];
        const int table = b.space == 2 && b.slot == 9 ? 0 : b.space == 2 && b.slot == 25 ? 1 : -1;

        if (table < 0 || b.kind != REMIX_VKFE_BINDING_STORAGE_BUFFER || draw.bindingBytes[i]
         || b.host_data_at_submit || !b.buffer || !b.size)
          continue;

        const auto key = std::make_pair(b.buffer, b.offset);
        auto known = m_tableCopies.find(key);

        if (known == m_tableCopies.end()) {
          const VkDeviceSize size = std::min<VkDeviceSize>(b.size, 4ull << 20);
          VkDeviceSize offset = 0;

          if (capture->buffer_copy_count >= 2 || !allocateArgsCopy(size, offset))
            continue;

          const DxvkBufferSliceHandle ring = m_argsReadback->getSliceHandle();
          auto& copy = capture->buffer_copies[capture->buffer_copy_count++];
          copy.src        = b.buffer;
          copy.src_offset = b.offset;
          copy.size       = size;
          copy.dst        = ring.handle;
          copy.dst_offset = ring.offset + offset;
          known = m_tableCopies.emplace(key, std::make_pair(offset, size)).first;
        }

        draw.tableCopyOffset[table] = known->second.first;
        draw.tableCopySize[table]   = known->second.second;
      }
    }

    auto recording = m_recorded.find(desc->command_buffer);

    if (recording == m_recorded.end()) {
      std::vector<D3D11VkDraw> draws;

      if (!m_spareRecordings.empty()) {
        draws = std::move(m_spareRecordings.back());
        m_spareRecordings.pop_back();
      }

      recording = m_recorded.emplace(desc->command_buffer, std::move(draws)).first;
    }

    recording->second.push_back(std::move(draw));
  }


  void D3D11VkFrontendDevice::onBake(VkCommandBuffer commandBuffer) {
    std::lock_guard lock(m_mutex);
    auto it = m_recorded.find(commandBuffer);

    if (it == m_recorded.end() || it->second.empty())
      return;

    D3D11VkDraw& draw = it->second.back();

    if (draw.bakeSlot < 0 || draw.bake == nullptr)
      return;

    draw.baked = true;

    // Later bakes of the frame into this image load what this one wrote.
    BakeSlot& slot = m_bakeSlots[draw.bakeSlot];
    const VkFormat format = draw.pipeline->state.render_target_formats[draw.pipeline->bakeAlbedoTarget];
    slot.cleared.insert(uint32_t(format));
  }


  void D3D11VkFrontendDevice::onCommandBufferReset(VkCommandBuffer commandBuffer) {
    ScopedCpuProfileZone();
    std::lock_guard lock(m_mutex);

    // A bounded pool: enough for a frame's command buffers in flight.
    constexpr size_t kMaxSpareRecordings = 256;
    auto recording = m_recorded.find(commandBuffer);

    if (recording != m_recorded.end()) {
      if (m_spareRecordings.size() < kMaxSpareRecordings) {
        recording->second.clear();
        m_spareRecordings.push_back(std::move(recording->second));
      }

      m_recorded.erase(recording);
    }

    m_uiSnapshotCommands.erase(commandBuffer);
    m_uiLayerCommands.erase(commandBuffer);
    m_uiLayerIncomplete.erase(commandBuffer);
    releaseCaptureUsers(commandBuffer);
  }


  remix_vkfe_result D3D11VkFrontendDevice::getUiSnapshot(
          VkCommandBuffer             commandBuffer,
          VkFormat                    format,
          VkExtent2D                  extent,
          remix_vkfe_ui_snapshot*     snapshot) {
    std::lock_guard lock(m_mutex);

    // Rotating slots: the game may record the next frame's copy while Remix
    // still composites this one. Advance once per frame and never hand out
    // the slot a submitted copy is waiting in.
    if (m_uiSnapshotSlotFrame != m_presentCount) {
      m_uiSnapshotSlotFrame = m_presentCount;
      m_uiSnapshotSlot = (m_uiSnapshotSlot + 1) % kUiSnapshotSlots;

      if (int32_t(m_uiSnapshotSlot) == m_frameUiSnapshot)
        m_uiSnapshotSlot = (m_uiSnapshotSlot + 1) % kUiSnapshotSlots;
    }

    const uint32_t slot = m_uiSnapshotSlot;
    UiSnapshot& s = m_uiSnapshots[slot];

    if (s.image == nullptr || s.format != format || s.extent.width != extent.width || s.extent.height != extent.height) {
      DxvkImageCreateInfo info;
      info.type        = VK_IMAGE_TYPE_2D;
      info.format      = format;
      info.flags       = 0;
      info.sampleCount = VK_SAMPLE_COUNT_1_BIT;
      info.extent      = { extent.width, extent.height, 1 };
      info.numLayers   = 1;
      info.mipLevels   = 1;
      info.usage       = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
      info.stages      = VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
      info.access      = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
      info.tiling      = VK_IMAGE_TILING_OPTIMAL;
      // The game's queue writes it, Remix's reads it: one layout for both.
      info.layout      = VK_IMAGE_LAYOUT_GENERAL;
      info.shared      = VK_TRUE;

      UiSnapshot next;
      next.image = m_device->createImage(info, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
        DxvkMemoryStats::Category::RTXRenderTarget, "Remix front-end UI snapshot");

      DxvkImageViewCreateInfo view;
      view.type      = VK_IMAGE_VIEW_TYPE_2D;
      view.format    = format;
      view.usage     = VK_IMAGE_USAGE_SAMPLED_BIT;
      view.aspect    = VK_IMAGE_ASPECT_COLOR_BIT;
      view.minLevel  = 0;
      view.numLevels = 1;
      view.minLayer  = 0;
      view.numLayers = 1;

      next.view   = m_device->createImageView(next.image, view);
      next.format = format;
      next.extent = extent;
      s = std::move(next);

      // Its contents are undefined until the game's first copy; Remix only
      // samples a slot a submitted copy wrote (m_frameUiSnapshot). The layout
      // still has to be GENERAL before the game's queue copies into it.
      {
        auto ctxLock = m_context->LockContext();
        m_context->EmitCs([image = s.image] (DxvkContext* ctx) {
          ctx->initImage(image, image->getAvailableSubresources(), VK_IMAGE_LAYOUT_UNDEFINED);
          ctx->flushCommandList();
        });
        m_context->FlushCsChunk();
      }

      m_context->SynchronizeCsThread(DxvkCsThread::SynchronizeAll);
      m_device->lockSubmission();
      m_device->unlockSubmission();
    }

    snapshot->image  = s.image->handle();
    snapshot->layout = VK_IMAGE_LAYOUT_GENERAL;
    m_uiSnapshotCommands[commandBuffer] = slot;
    return REMIX_VKFE_OK;
  }


  remix_vkfe_result D3D11VkFrontendDevice::getUiLayer(
          VkCommandBuffer             commandBuffer,
          VkFormat                    colorFormat,
          VkFormat                    depthFormat,
          VkExtent2D                  extent,
          remix_vkfe_ui_layer*        layer) {
    std::lock_guard lock(m_mutex);

    *layer = remix_vkfe_ui_layer{};

    // Rotating like the snapshots: never the slot a submitted frame's
    // replays wait in.
    if (m_uiLayerSlotFrame != m_presentCount) {
      m_uiLayerSlotFrame = m_presentCount;
      m_uiLayerSlot = (m_uiLayerSlot + 1) % kUiSnapshotSlots;

      if (int32_t(m_uiLayerSlot) == m_frameUiLayer)
        m_uiLayerSlot = (m_uiLayerSlot + 1) % kUiSnapshotSlots;

      m_uiLayers[m_uiLayerSlot].cleared = false;
    }

    UiLayer& slot = m_uiLayers[m_uiLayerSlot];
    BakeImage& color = slot.color;

    if (color.image == nullptr || color.format != colorFormat
     || color.extent.width != extent.width || color.extent.height != extent.height) {
      if (!createBakeImage(color, colorFormat, extent)) {
        color = BakeImage();
        return REMIX_VKFE_UNSUPPORTED;
      }

      slot.cleared = false;
    }

    const BakeImage* depth = nullptr;

    if (depthFormat != VK_FORMAT_UNDEFINED) {
      depth = bakeImage(m_uiLayerDepth, uint64_t(depthFormat), depthFormat, extent);

      if (!depth)
        return REMIX_VKFE_UNSUPPORTED;
    }

    layer->color_view = color.attachmentView->handle();
    layer->depth_view = depth ? depth->attachmentView->handle() : VK_NULL_HANDLE;
    layer->extent     = extent;
    layer->clear      = slot.cleared ? 0u : 1u;
    layer->generation = m_bakeGeneration;

    m_uiLayerCommands[commandBuffer] = m_uiLayerSlot;
    return REMIX_VKFE_OK;
  }


  void D3D11VkFrontendDevice::onUiLayer(VkCommandBuffer commandBuffer, bool replayed) {
    std::lock_guard lock(m_mutex);

    if (!replayed) {
      // A UI draw is missing from this command buffer's layer.
      m_uiLayerIncomplete.insert(commandBuffer);
      return;
    }

    auto it = m_uiLayerCommands.find(commandBuffer);

    if (it != m_uiLayerCommands.end())
      m_uiLayers[it->second].cleared = true;
  }


  void D3D11VkFrontendDevice::onUiSnapshot(VkCommandBuffer commandBuffer) {
    // getUiSnapshot already recorded the slot; the copy counts once the
    // command buffer is submitted (onSubmit).
    std::lock_guard lock(m_mutex);
    (void)m_uiSnapshotCommands.count(commandBuffer);
  }


  void D3D11VkFrontendDevice::onImageDestroy(VkImage image) {
    // The wrapper never owns the VkImage; dropping it releases Remix's view
    // once Remix's GPU work that used it has completed.
    std::lock_guard lock(m_mutex);
    m_capture.textures.erase(image);

    // Draws recorded before now that are committed later must not wrap it
    // again: the handle is about to be freed (or reused for another image).
    m_destroyedImages[image] = DestroyedImage { ++m_imageDestroySeq, m_presentCount };
  }


  namespace {

    Rc<DxvkImageView> compositeView(const Rc<DxvkDevice>& device, const Rc<DxvkImage>& image,
                                    VkFormat format, VkImageUsageFlags usage) {
      DxvkImageViewCreateInfo info;
      info.type      = VK_IMAGE_VIEW_TYPE_2D;
      info.format    = format;
      info.usage     = usage;
      info.aspect    = VK_IMAGE_ASPECT_COLOR_BIT;
      info.minLevel  = 0;
      info.numLevels = 1;
      info.minLayer  = 0;
      info.numLayers = 1;
      return device->createImageView(image, info);
    }

  }


  void D3D11VkFrontendDevice::compositeUi(D3D11VkSwapchain& sc, const UiSnapshot& snapshot) {
    Rc<DxvkImageView> finalView = compositeView(m_device, sc.finalCopy, sc.format, VK_IMAGE_USAGE_SAMPLED_BIT);
    Rc<DxvkImageView> remixView = compositeView(m_device, sc.backbuffer, sc.format, VK_IMAGE_USAGE_SAMPLED_BIT);

    drawComposite(sc, m_uiCompositeFs, finalView, snapshot.view, remixView);
  }


  void D3D11VkFrontendDevice::compositeUiLayer(D3D11VkSwapchain& sc, const UiLayer& layer) {
    // In the swap chain's format, so both inputs decode alike.
    Rc<DxvkImageView> layerView = compositeView(m_device, layer.color.image, sc.format, VK_IMAGE_USAGE_SAMPLED_BIT);
    Rc<DxvkImageView> remixView = compositeView(m_device, sc.backbuffer, sc.format, VK_IMAGE_USAGE_SAMPLED_BIT);

    drawComposite(sc, m_uiLayerFs, layerView, remixView, nullptr);
  }


  void D3D11VkFrontendDevice::drawComposite(D3D11VkSwapchain& sc, const Rc<DxvkShader>& shader,
                                            const Rc<DxvkImageView>& a, const Rc<DxvkImageView>& b,
                                            const Rc<DxvkImageView>& c) {
    Rc<DxvkImageView> outView = compositeView(m_device, sc.uiOut, sc.format, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);

    m_context->EmitCs([vs = m_uiCompositeVs, fs = shader, finalView = a, preView = b,
                       remixView = c, outView, extent = sc.extent] (DxvkContext* ctx) {
      DxvkRenderTargets rt;
      rt.color[0].view   = outView;
      rt.color[0].layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
      ctx->bindRenderTargets(rt);

      ctx->bindShader(VK_SHADER_STAGE_VERTEX_BIT, vs);
      ctx->bindShader(VK_SHADER_STAGE_FRAGMENT_BIT, fs);

      DxvkInputAssemblyState ia = {};
      ia.primitiveTopology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
      ctx->setInputAssemblyState(ia);
      ctx->setInputLayout(0, nullptr, 0, nullptr);

      DxvkRasterizerState rs = {};
      rs.polygonMode     = VK_POLYGON_MODE_FILL;
      rs.cullMode        = VK_CULL_MODE_NONE;
      rs.frontFace       = VK_FRONT_FACE_COUNTER_CLOCKWISE;
      rs.depthClipEnable = VK_FALSE;
      rs.depthBiasEnable = VK_FALSE;
      rs.conservativeMode = VK_CONSERVATIVE_RASTERIZATION_MODE_DISABLED_EXT;
      rs.sampleCount     = VK_SAMPLE_COUNT_1_BIT;
      ctx->setRasterizerState(rs);

      DxvkMultisampleState ms = {};
      ms.sampleMask = 0xffffffffu;
      ctx->setMultisampleState(ms);

      DxvkDepthStencilState ds = {};
      ds.depthCompareOp = VK_COMPARE_OP_ALWAYS;
      ctx->setDepthStencilState(ds);

      DxvkLogicOpState lo = {};
      ctx->setLogicOpState(lo);

      DxvkBlendMode bm = {};
      bm.enableBlending = VK_FALSE;
      bm.colorSrcFactor = VK_BLEND_FACTOR_ONE;
      bm.colorDstFactor = VK_BLEND_FACTOR_ZERO;
      bm.colorBlendOp   = VK_BLEND_OP_ADD;
      bm.alphaSrcFactor = VK_BLEND_FACTOR_ONE;
      bm.alphaDstFactor = VK_BLEND_FACTOR_ZERO;
      bm.alphaBlendOp   = VK_BLEND_OP_ADD;
      bm.writeMask      = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT
                        | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
      ctx->setBlendMode(0, bm);

      VkViewport viewport = { 0.0f, 0.0f, float(extent.width), float(extent.height), 0.0f, 1.0f };
      VkRect2D scissor = { { 0, 0 }, extent };
      ctx->setViewports(1, &viewport, &scissor);

      // Slots 0..2 in the order the fragment shader declares them.
      ctx->bindResourceView(0, finalView, nullptr);
      ctx->bindResourceView(1, preView, nullptr);
      ctx->bindResourceView(2, remixView, nullptr);

      ctx->draw(3, 1, 0, 0);

      // Leave no stale target bound for Remix's next frame.
      ctx->bindRenderTargets(DxvkRenderTargets());
    });
  }


  void D3D11VkFrontendDevice::onSubmitBytes(
          VkCommandBuffer             commandBuffer,
          uint32_t                    drawIndex,
          uint32_t                    bindingIndex,
    const void*                       data,
          VkDeviceSize                size) {
    ScopedCpuProfileZone();
    constexpr VkDeviceSize kMaxConstantBytes = 64ull << 10;
    constexpr VkDeviceSize kMaxStorageBytes  = 512ull << 10;

    std::lock_guard lock(m_mutex);
    auto it = m_recorded.find(commandBuffer);

    if (it == m_recorded.end() || drawIndex >= it->second.size())
      return;

    D3D11VkDraw& draw = it->second[drawIndex];

    if (bindingIndex >= draw.bindings.size() || !draw.bindings[bindingIndex].host_data_at_submit)
      return;

    const remix_vkfe_binding& binding = draw.bindings[bindingIndex];
    const bool storage = binding.kind == REMIX_VKFE_BINDING_STORAGE_BUFFER;
    const size_t n = size_t(std::min(size, storage ? storageCopyLimit(binding, kMaxStorageBytes) : kMaxConstantBytes));

    // The per-view constants thousands of draws share are copied once per
    // submission.
    auto cached = m_submitBytes.find(data);

    if (cached != m_submitBytes.end() && cached->second->size() >= n) {
      draw.bindingBytes[bindingIndex] = cached->second;
      return;
    }

    const uint8_t* src = reinterpret_cast<const uint8_t*>(data);
    auto copy = std::make_shared<std::vector<uint8_t>>(src, src + n);
    draw.bindingBytes[bindingIndex] = copy;
    m_submitBytes[data] = std::move(copy);
  }


  void D3D11VkFrontendDevice::onSubmit(const remix_vkfe_submit_desc* desc) {
    ScopedCpuProfileZone();
    std::lock_guard lock(m_mutex);

    // Submit-time bytes (onSubmitBytes, called just before) are shared
    // within this submission only.
    struct ClearSubmitBytes {
      std::unordered_map<const void*, std::shared_ptr<const std::vector<uint8_t>>>& bytes;
      ~ClearSubmitBytes() { bytes.clear(); }
    } clearSubmitBytes { m_submitBytes };

    // Command buffers may be submitted more than once (D3D12 bundles,
    // Vulkan simultaneous-use buffers), so the recording is copied, not
    // moved.
    for (uint32_t i = 0; i < desc->command_buffer_count; i++) {
      // The last pre-UI copy submitted this frame is the one to composite with.
      auto snapshot = m_uiSnapshotCommands.find(desc->command_buffers[i]);

      if (snapshot != m_uiSnapshotCommands.end())
        m_frameUiSnapshot = int32_t(snapshot->second);

      auto uiLayer = m_uiLayerCommands.find(desc->command_buffers[i]);

      if (uiLayer != m_uiLayerCommands.end() && m_uiLayers[uiLayer->second].cleared)
        m_frameUiLayer = int32_t(uiLayer->second);

      if (m_uiLayerIncomplete.count(desc->command_buffers[i]))
        m_frameUiLayerIncomplete = true;

      auto users = m_captureUsers.find(desc->command_buffers[i]);

      if (users != m_captureUsers.end()) {
        for (D3D11VkCaptureChunk* chunk : users->second)
          chunk->lastFrame = m_presentCount;
      }

      auto it = m_recorded.find(desc->command_buffers[i]);

      if (it == m_recorded.end())
        continue;

      for (const auto& draw : it->second)
        m_frameDraws.push_back(draw);
    }
  }


  remix_vkfe_result D3D11VkFrontendDevice::onPresent(
    const remix_vkfe_present_desc*    desc,
          remix_vkfe_present_result*  result) {
    ScopedCpuProfileZone();

    result->wait_semaphore = VK_NULL_HANDLE;

    std::lock_guard lock(m_mutex);

    auto it = m_swapchains.find(desc->swapchain);

    if (it == m_swapchains.end() || desc->image_index >= it->second.images.size()) {
      m_frameDraws.clear();
      return REMIX_VKFE_OK;
    }

    D3D11VkSwapchain& sc = it->second;

    // Remix menu (as D3D11SwapChain::PresentImage): Alt+X toggles it - polled,
    // since the window procedure hands ImGui messages only while it is open -
    // and the game's input is held back while it is open.
    if (sc.window) {
      ImGUI& gui = m_device->getCommon()->getImgui();
      D3D11InputGuard::setBlocking(gui.isInit() && gui.isMenuOpen() && RtxOptions::blockInputToGameInUI());

      const bool down = (::GetAsyncKeyState(VK_MENU) & 0x8000) && (::GetAsyncKeyState('X') & 0x8000);

      if (down && !m_menuHotkeyDown && GetForegroundWindow() == sc.window) {
        gui.toggleMenuFromHotkey();
        gui.markRemixMenuHotkeyHandled();
      }

      m_menuHotkeyDown = down;
    }

    const uint32_t index = desc->image_index;
    const VkSemaphore gameDone  = sc.gameDone[index];
    const VkSemaphore remixDone = sc.remixDone[index];
    const Rc<DxvkImage> image   = sc.images[index];
    const Rc<DxvkImage> target  = sc.backbuffer;
    const VkExtent2D extent     = sc.extent;

    // 1. The game's frame -> gameDone, on the game's present queue. The
    // caller holds that queue's external synchronization during this call.
    // A semaphore signal covers every command submitted earlier on the
    // queue, so this also orders the game's rendering when the present has
    // no wait semaphores.
    std::vector<VkPipelineStageFlags> waitStages(desc->wait_semaphore_count, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);

    VkSubmitInfo submit = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    submit.waitSemaphoreCount   = desc->wait_semaphore_count;
    submit.pWaitSemaphores      = desc->wait_semaphores;
    submit.pWaitDstStageMask    = waitStages.data();
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores    = &gameDone;

    // The frame fence tells the capture code when this frame's indirect
    // captures and XFB counters are complete. A fence is reused 8 frames
    // later; it has long signalled by then, the wait is a guard.
    const uint32_t fenceIndex = uint32_t(m_presentCount % kFrameFences);
    VkFence fence = m_frameFences[fenceIndex];

    if (m_frameFenceFrame[fenceIndex]) {
      m_device->vkd()->vkWaitForFences(m_device->handle(), 1, &fence, VK_TRUE, ~0ull);
      m_device->vkd()->vkResetFences(m_device->handle(), 1, &fence);
    }

    m_frameFenceFrame[fenceIndex] = m_presentCount + 1;

    lockQueue(desc->queue);
    const VkResult submitted = m_device->vkd()->vkQueueSubmit(desc->queue, 1, &submit, fence);
    unlockQueue(desc->queue);

    if (submitted != VK_SUCCESS) {
      m_frameFenceFrame[fenceIndex] = 0;
      Logger::err("[Remix-VkFrontend] vkQueueSubmit on the game's present queue failed; frame presented without Remix");
      m_frameDraws.clear();
      return REMIX_VKFE_UNSUPPORTED;
    }

    // From here on the game's present semaphores are consumed: whatever
    // fails, the present must get a remixDone that will be signalled
    // (forwardFrame below), never the game's semaphores again.
    bool waitEmitted   = false;   // Remix's CS will wait on gameDone
    bool signalEmitted = false;   // Remix's CS will signal remixDone

    try {

    {
      auto ctxLock = m_context->LockContext();

      const VkImageSubresourceLayers layers = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
      const VkExtent3D copyExtent = { extent.width, extent.height, 1 };

      // 2. Wait for the game's frame first. Everything Remix recorded so far
      // is submitted with the wait, and a semaphore wait holds back every
      // later submission on Remix's queue. So nothing the capture below
      // records - geometry reading the capture buffers, texture read-backs,
      // the swap chain copy - can run before the game's GPU work, even if
      // the command list is split in between.
      m_context->EmitCs([gameDone] (DxvkContext* ctx) {
        static_cast<RtxContext*>(ctx)->flushCommandListWithSync(gameDone, VK_NULL_HANDLE);
        // XFB byte counters written by the game's queue become visible to
        // the host once this submission completes (a fence alone covers
        // device access only).
        ctx->emitMemoryBarrier(0,
          VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_WRITE_BIT,
          VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_READ_BIT);
      });
      waitEmitted = true;

      // 3. Scene input for this frame.
      captureFrame(m_frameDraws);
      m_frameDraws.clear();

      // Destroyed-image entries no draw can still be committed against.
      {
        constexpr uint64_t kKeepFrames = 16;

        for (auto i = m_destroyedImages.begin(); i != m_destroyedImages.end(); ) {
          if (i->second.frame + kKeepFrames < m_presentCount)
            i = m_destroyedImages.erase(i);
          else
            ++i;
        }
      }

      // Bake / UI layer images replaced long enough ago that no game
      // command buffer in flight renders into them.
      {
        constexpr uint64_t kRetireFrames = 8;
        auto expired = std::remove_if(m_retiredBakeImages.begin(), m_retiredBakeImages.end(),
          [&] (const std::pair<uint64_t, BakeImage>& r) { return r.first + kRetireFrames <= m_presentCount; });
        m_retiredBakeImages.erase(expired, m_retiredBakeImages.end());
      }

      // UI composite this frame: a pre-UI snapshot of this swap chain's size
      // was copied by a command buffer submitted this frame.
      const UiSnapshot* uiSnapshot = nullptr;

      if (m_frameUiSnapshot >= 0 && sc.finalCopy != nullptr && sc.uiOut != nullptr) {
        const UiSnapshot& s = m_uiSnapshots[m_frameUiSnapshot];

        if (s.view != nullptr && s.extent.width == extent.width && s.extent.height == extent.height)
          uiSnapshot = &s;
      }

      m_frameUiSnapshot = -1;

      // A UI layer this frame takes the place of the snapshot.
      const UiLayer* uiLayer = nullptr;

      if (m_frameUiLayer >= 0 && !m_frameUiLayerIncomplete && sc.uiOut != nullptr) {
        const UiLayer& l = m_uiLayers[m_frameUiLayer];

        if (l.color.image != nullptr && l.color.format == sc.format
         && l.color.extent.width == extent.width && l.color.extent.height == extent.height)
          uiLayer = &l;
      }

      m_frameUiLayer = -1;
      m_frameUiLayerIncomplete = false;

      if (uiLayer)
        uiSnapshot = nullptr;

      const Rc<DxvkImage> finalCopy = uiSnapshot ? sc.finalCopy : nullptr;

      // 4. Remix's frame, ending in remixDone.
      m_context->EmitCs([image, target, finalCopy, layers, copyExtent] (DxvkContext* ctx) {
        ctx->copyImage(target, layers, VkOffset3D(), image, layers, VkOffset3D(), copyExtent);

        if (finalCopy != nullptr)
          ctx->copyImage(finalCopy, layers, VkOffset3D(), image, layers, VkOffset3D(), copyExtent);
      });

      m_context->m_rtx.EndFrame(target, extent);

      // The game's HUD over Remix's frame.
      if (uiLayer)
        compositeUiLayer(sc, *uiLayer);
      else if (uiSnapshot)
        compositeUi(sc, *uiSnapshot);

      const Rc<DxvkImage> presented = (uiSnapshot || uiLayer) ? sc.uiOut : target;

      // The Remix menu over everything, the game's HUD included.
      if (sc.window) {
        RtxOptionManager::applyPendingValues(m_device.ptr(), false);
        Rc<DxvkImageView> menuTarget = compositeView(m_device, presented, sc.format, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);

        m_context->EmitCs([device = m_device, window = sc.window, menuTarget, extent] (DxvkContext* ctx) {
          DxvkRenderTargets targets;
          targets.color[0].view   = menuTarget;
          targets.color[0].layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
          ctx->bindRenderTargets(targets);

          device->getCommon()->getImgui().render(window, Rc<DxvkContext>(ctx), extent, false);

          ctx->bindRenderTargets(DxvkRenderTargets());
        });
      }

      m_context->EmitCs([remixDone, image, presented, layers, copyExtent] (DxvkContext* ctx) {
        ctx->copyImage(image, layers, VkOffset3D(), presented, layers, VkOffset3D(), copyExtent);
        static_cast<RtxContext*>(ctx)->flushCommandListWithSync(VK_NULL_HANDLE, remixDone);
      });
      signalEmitted = true;

      m_context->m_rtx.OnPresent(image, extent);

      m_context->EmitCs([device = m_device] (DxvkContext*) {
        device->incrementPresentCount();
      });

      m_context->FlushCsChunk();
    }

    // 5. The present waits on remixDone, so its signal must already be
    // submitted to Vulkan: wait for the CS thread, then for DXVK's
    // submission thread.
    m_context->SynchronizeCsThread(DxvkCsThread::SynchronizeAll);
    m_device->lockSubmission();
    m_device->unlockSubmission();

    } catch (const DxvkError& e) {
      Logger::err(str::format("[Remix-VkFrontend] frame failed: ", e.message(), "; presenting the game's frame"));
      m_frameDraws.clear();
      forwardFrame(desc->queue, gameDone, remixDone, waitEmitted, signalEmitted);
    }

    result->wait_semaphore = remixDone;
    m_presentCount++;
    m_captureBytesThisFrame = 0;
    m_frameBytes.clear();

    if (m_presentCount <= 3 || (m_presentCount % 600) == 0)
      Logger::info(str::format("[Remix-VkFrontend] present ", m_presentCount, " image ", index));

    return REMIX_VKFE_OK;
  }

}


// ---------------------------------------------------------------------------
// C API
// ---------------------------------------------------------------------------

namespace {

  using namespace dxvk;

  remix_vkfe_result planInstance(
    const remix_vkfe_instance_request* request,
          remix_vkfe_instance_plan*    plan) {
    if (!request || !request->create_info || !plan)
      return REMIX_VKFE_BAD_ARGUMENT;

    auto pending = std::make_unique<InstancePending>();
    pending->frontend = request->frontend;
    pending->info = *request->create_info;

    if (request->create_info->pApplicationInfo)
      pending->app = *request->create_info->pApplicationInfo;
    else
      pending->app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;

    pending->app.apiVersion = std::max(pending->app.apiVersion, kRequiredApiVersion);
    pending->info.pApplicationInfo = &pending->app;

    // Remix does not present on an imported device, so it needs no
    // instance extensions beyond what the game enabled. The list is
    // copied so finish_instance can record it.
    for (uint32_t i = 0; i < request->create_info->enabledExtensionCount; i++)
      pending->extensionStorage.emplace_back(request->create_info->ppEnabledExtensionNames[i]);

    for (const auto& name : pending->extensionStorage)
      pending->extensions.push_back(name.c_str());

    pending->info.enabledExtensionCount   = uint32_t(pending->extensions.size());
    pending->info.ppEnabledExtensionNames = pending->extensions.data();

    plan->create_info = &pending->info;
    plan->pending = pending.release();
    return REMIX_VKFE_OK;
  }


  remix_vkfe_result finishInstance(
          remix_vkfe_pending        pendingHandle,
          VkResult                  createResult,
          VkInstance                instance,
          PFN_vkGetInstanceProcAddr getInstanceProcAddr,
          remix_vkfe_instance*      outInstance) {
    std::unique_ptr<InstancePending> pending(static_cast<InstancePending*>(pendingHandle));

    if (!pending || !outInstance)
      return REMIX_VKFE_BAD_ARGUMENT;

    *outInstance = nullptr;

    if (createResult != VK_SUCCESS || instance == VK_NULL_HANDLE)
      return REMIX_VKFE_UNSUPPORTED;

    D3D11InitRemixFileSystem();

    try {
      DxvkInstanceImport import;
      import.instance            = instance;
      import.getInstanceProcAddr = getInstanceProcAddr;
      import.extensionCount      = uint32_t(pending->extensions.size());
      import.extensionNames      = pending->extensions.data();

      auto* result = new remix_vkfe_instance_t;
      result->instance = new DxvkInstance(import);
      result->frontend = pending->frontend;
      *outInstance = result;
      return REMIX_VKFE_OK;
    } catch (const DxvkError& e) {
      Logger::err(str::format("[Remix-VkFrontend] cannot adopt the game's VkInstance: ", e.message()));
      return REMIX_VKFE_UNSUPPORTED;
    }
  }


  void destroyInstance(remix_vkfe_instance instance) {
    delete instance;
  }


  remix_vkfe_result planDevice(
    const remix_vkfe_device_request* request,
          remix_vkfe_device_plan*    plan) {
    if (!request || !request->instance || !request->create_info || !plan)
      return REMIX_VKFE_BAD_ARGUMENT;

    const Rc<DxvkInstance>& instance = request->instance->instance;
    Rc<DxvkAdapter> adapter;

    for (uint32_t i = 0; i < instance->adapterCount(); i++) {
      Rc<DxvkAdapter> candidate = instance->enumAdapters(i);

      if (candidate->handle() == request->physical_device)
        adapter = candidate;
    }

    if (adapter == nullptr) {
      Logger::err("[Remix-VkFrontend] the game's VkPhysicalDevice is not among Remix's adapters");
      return REMIX_VKFE_UNSUPPORTED;
    }

    auto pending = std::make_unique<DevicePending>();

    // A D3D11 device the game created earlier already runs Remix here.
    if (D3D11DXGIDevice::RemixDeviceLive() && !D3D11DXGIDevice::RemixRunsOnGameDevice()) {
      Logger::warn("[Remix-VkFrontend] Remix already renders on a D3D11 device in this process; this VkDevice runs without Remix");
      return REMIX_VKFE_UNSUPPORTED;
    }

    if (!pending->claim.tryTake()) {
      Logger::warn("[Remix-VkFrontend] Remix already renders on another of the game's VkDevices; this one runs without Remix");
      return REMIX_VKFE_UNSUPPORTED;
    }

    pending->frontend = request->instance->frontend;
    pending->instance = instance;
    pending->adapter  = adapter;
    pending->importer = std::make_unique<DxvkDeviceImporter>(instance, adapter);

    // The D3D11 runtime runs on this device, so ask for what a D3D11
    // FL 11_1 device gets.
    const DxvkDeviceFeatures baseline = D3D11Device::GetDeviceFeatures(adapter, D3D_FEATURE_LEVEL_11_1);

    if (!pending->importer->prepare(*request->create_info, &baseline))
      return REMIX_VKFE_UNSUPPORTED;

    plan->create_info = &pending->importer->createInfo();
    plan->pending = pending.release();
    return REMIX_VKFE_OK;
  }


  remix_vkfe_result finishDevice(
          remix_vkfe_pending      pendingHandle,
          VkResult                createResult,
          VkDevice                device,
          PFN_vkGetDeviceProcAddr getDeviceProcAddr,
          remix_vkfe_device*      outDevice) {
    std::unique_ptr<DevicePending> pending(static_cast<DevicePending*>(pendingHandle));

    if (!pending || !outDevice)
      return REMIX_VKFE_BAD_ARGUMENT;

    *outDevice = nullptr;
    pending->importer->restore();

    if (createResult != VK_SUCCESS || device == VK_NULL_HANDLE) {
      Logger::err(str::format("[Remix-VkFrontend] the game's vkCreateDevice with Remix's additions failed: ", createResult));
      return REMIX_VKFE_UNSUPPORTED;
    }

    try {
      Rc<DxvkDevice> dxvkDevice = pending->importer->import(device, getDeviceProcAddr);

      auto result = std::make_unique<remix_vkfe_device_t>();
      result->device = std::make_unique<D3D11VkFrontendDevice>(
        pending->frontend, pending->instance, pending->adapter, dxvkDevice,
        pending->importer->sharesGameQueue());
      result->claim.adopt(pending->claim);
      *outDevice = result.release();
      return REMIX_VKFE_OK;
    } catch (const DxvkError& e) {
      Logger::err(str::format("[Remix-VkFrontend] Remix could not start on the game's device: ", e.message()));
      return REMIX_VKFE_UNSUPPORTED;
    }
  }


  void destroyDevice(remix_vkfe_device device) {
    delete device;
  }


  void onSwapchain(remix_vkfe_device device, VkSwapchainKHR swapchain,
                   const VkSwapchainCreateInfoKHR* info, uint32_t count, const VkImage* images) {
    if (device && info)
      device->device->onSwapchain(swapchain, info, count, images);
  }

  void onSwapchainDestroy(remix_vkfe_device device, VkSwapchainKHR swapchain) {
    if (device)
      device->device->onSwapchainDestroy(swapchain);
  }

  void onPipeline(remix_vkfe_device device, const remix_vkfe_pipeline_desc* desc,
                  remix_vkfe_capture_plan* plan) {
    if (!plan)
      return;

    *plan = remix_vkfe_capture_plan{};

    if (device && desc)
      device->device->onPipeline(desc, plan);
  }

  void onPipelineDestroy(remix_vkfe_device device, uint64_t key) {
    if (device)
      device->device->onPipelineDestroy(key);
  }

  void onDraw(remix_vkfe_device device, const remix_vkfe_draw_desc* desc,
              remix_vkfe_draw_capture* capture) {
    if (!capture)
      return;

    *capture = remix_vkfe_draw_capture{};

    if (device && desc)
      device->device->onDraw(desc, capture);
  }

  void onCommandBufferReset(remix_vkfe_device device, VkCommandBuffer cmd) {
    if (device)
      device->device->onCommandBufferReset(cmd);
  }

  void onSubmit(remix_vkfe_device device, const remix_vkfe_submit_desc* desc) {
    if (device && desc)
      device->device->onSubmit(desc);
  }

  remix_vkfe_result onPresent(remix_vkfe_device device, const remix_vkfe_present_desc* desc,
                              remix_vkfe_present_result* result) {
    if (!device || !desc || !result)
      return REMIX_VKFE_BAD_ARGUMENT;

    try {
      return device->device->onPresent(desc, result);
    } catch (const DxvkError& e) {
      Logger::err(str::format("[Remix-VkFrontend] present: ", e.message()));
      result->wait_semaphore = VK_NULL_HANDLE;
      return REMIX_VKFE_UNSUPPORTED;
    }
  }

  remix_vkfe_result getUiSnapshot(remix_vkfe_device device, VkCommandBuffer cmd, VkFormat format,
                                  VkExtent2D extent, remix_vkfe_ui_snapshot* snapshot) {
    if (!device || !snapshot)
      return REMIX_VKFE_BAD_ARGUMENT;

    try {
      return device->device->getUiSnapshot(cmd, format, extent, snapshot);
    } catch (const DxvkError& e) {
      Logger::err(str::format("[Remix-VkFrontend] UI snapshot: ", e.message()));
      return REMIX_VKFE_UNSUPPORTED;
    }
  }

  void onUiSnapshot(remix_vkfe_device device, VkCommandBuffer cmd) {
    if (device)
      device->device->onUiSnapshot(cmd);
  }

  void onImageDestroy(remix_vkfe_device device, VkImage image) {
    if (device)
      device->device->onImageDestroy(image);
  }

  void onBake(remix_vkfe_device device, VkCommandBuffer cmd) {
    if (device)
      device->device->onBake(cmd);
  }

  remix_vkfe_result getUiLayer(remix_vkfe_device device, VkCommandBuffer cmd, VkFormat colorFormat,
                               VkFormat depthFormat, VkExtent2D extent, remix_vkfe_ui_layer* layer) {
    if (!device || !layer)
      return REMIX_VKFE_BAD_ARGUMENT;

    try {
      return device->device->getUiLayer(cmd, colorFormat, depthFormat, extent, layer);
    } catch (const DxvkError& e) {
      Logger::err(str::format("[Remix-VkFrontend] UI layer: ", e.message()));
      return REMIX_VKFE_UNSUPPORTED;
    }
  }

  void onUiLayer(remix_vkfe_device device, VkCommandBuffer cmd, uint32_t replayed) {
    if (device)
      device->device->onUiLayer(cmd, replayed != 0);
  }

  void onSubmitBytes(remix_vkfe_device device, VkCommandBuffer cmd, uint32_t drawIndex,
                     uint32_t bindingIndex, const void* data, VkDeviceSize size) {
    if (device && data)
      device->device->onSubmitBytes(cmd, drawIndex, bindingIndex, data, size);
  }

  void onSwapchainWindow(remix_vkfe_device device, VkSwapchainKHR swapchain, void* window) {
    if (device && window)
      device->device->onSwapchainWindow(swapchain, static_cast<HWND>(window));
  }

  uint32_t getReflexMode(remix_vkfe_device) {
    if (!RtxOptions::isReflexEnabled())
      return REMIX_VKFE_REFLEX_OFF;

    switch (RtxOptions::reflexMode()) {
      case ReflexMode::LowLatency:      return REMIX_VKFE_REFLEX_LOW_LATENCY;
      case ReflexMode::LowLatencyBoost: return REMIX_VKFE_REFLEX_BOOST;
      default:                          return REMIX_VKFE_REFLEX_OFF;
    }
  }

  void lockQueue(remix_vkfe_device device, VkQueue queue) {
    if (device)
      device->device->lockQueue(queue);
  }

  void unlockQueue(remix_vkfe_device device, VkQueue queue) {
    if (device)
      device->device->unlockQueue(queue);
  }

  const remix_vkfe_api g_api = {
    REMIX_VKFE_VERSION,
    planInstance,
    finishInstance,
    destroyInstance,
    planDevice,
    finishDevice,
    destroyDevice,
    onSwapchain,
    onSwapchainDestroy,
    onPipeline,
    onPipelineDestroy,
    onDraw,
    onCommandBufferReset,
    onSubmit,
    onPresent,
    lockQueue,
    unlockQueue,
    getUiSnapshot,
    onUiSnapshot,
    onImageDestroy,
    onBake,
    getUiLayer,
    onUiLayer,
    onSubmitBytes,
    getReflexMode,
    onSwapchainWindow,
  };

}


// Exported through d3d11.def.
extern "C" remix_vkfe_result remix_vkfe_get_api(
        uint32_t          version,
  const remix_vkfe_api**  api) {
  if (!api)
    return REMIX_VKFE_BAD_ARGUMENT;

  *api = nullptr;

  if (version != REMIX_VKFE_VERSION)
    return REMIX_VKFE_VERSION_MISMATCH;

  *api = &g_api;
  return REMIX_VKFE_OK;
}
