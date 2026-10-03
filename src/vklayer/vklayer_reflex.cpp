#include <algorithm>
#include <cstring>

#include "vklayer.h"

// Reflex on the game's swap chain through VK_NV_low_latency2 (ReflexState).
// Per frame: the sleep right after the game's present holds the game back
// until the driver wants its next frame to start, which is what keeps
// latency low; markers around it tell the driver where simulation, render
// submission and presentation happen. The layer only sees submissions and
// presents, so simulation ends at the frame's first submission.

namespace remix_vklayer {

  namespace {

    bool hasExtension(const VkDeviceCreateInfo* info, const char* name) {
      for (uint32_t i = 0; info && i < info->enabledExtensionCount; i++) {
        if (!std::strcmp(info->ppEnabledExtensionNames[i], name))
          return true;
      }

      return false;
    }

    const VkPhysicalDevicePresentIdFeaturesKHR* findPresentIdFeatures(const VkDeviceCreateInfo* info) {
      for (auto* s = reinterpret_cast<const VkBaseInStructure*>(info->pNext); s; s = s->pNext) {
        if (s->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR)
          return reinterpret_cast<const VkPhysicalDevicePresentIdFeaturesKHR*>(s);
      }

      return nullptr;
    }

    void marker(DeviceData* dev, VkSwapchainKHR swapchain, uint64_t id, VkLatencyMarkerNV which) {
      VkSetLatencyMarkerInfoNV info = { VK_STRUCTURE_TYPE_SET_LATENCY_MARKER_INFO_NV };
      info.presentID = id;
      info.marker    = which;
      dev->reflex.setLatencyMarker(dev->device, swapchain, &info);
    }

    uint32_t currentMode(DeviceData* dev) {
      const remix_vkfe_api* api = remixApi();
      return api && api->get_reflex_mode ? api->get_reflex_mode(dev->remix) : REMIX_VKFE_REFLEX_LOW_LATENCY;
    }

  }


  void reflexPlanDevice(InstanceData* inst, VkPhysicalDevice physical,
                        const VkDeviceCreateInfo* gameInfo, const VkDeviceCreateInfo* info,
                        ReflexDeviceRequest* request) {
    // A native Vulkan game that enables the extension runs Reflex itself.
    // vkd3d-proton enables it whenever the driver has it, but only drives it
    // for NVAPI calls, which on Windows reach the system nvapi64.dll and never
    // vkd3d - so for DX12 games the layer drives it.
    if (!inst->vkd3d && hasExtension(gameInfo, VK_NV_LOW_LATENCY_2_EXTENSION_NAME)) {
      log("the game enables VK_NV_low_latency2 itself; Reflex is left to the game");
      return;
    }

    if (!inst->EnumerateDeviceExtensionProperties)
      return;

    uint32_t count = 0;
    inst->EnumerateDeviceExtensionProperties(physical, nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> supported(count);

    if (inst->EnumerateDeviceExtensionProperties(physical, nullptr, &count, supported.data()) < 0)
      return;

    supported.resize(count);
    bool lowLatency2 = false;
    bool presentId = false;

    for (const auto& e : supported) {
      lowLatency2 |= !std::strcmp(e.extensionName, VK_NV_LOW_LATENCY_2_EXTENSION_NAME);
      presentId   |= !std::strcmp(e.extensionName, VK_KHR_PRESENT_ID_EXTENSION_NAME);
    }

    if (!lowLatency2) {
      log("the GPU driver has no VK_NV_low_latency2; Reflex is off on this device");
      return;
    }

    // Present IDs need the extension and its feature.
    if (presentId) {
      VkPhysicalDevicePresentIdFeaturesKHR query = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR };
      VkPhysicalDeviceFeatures2 features = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
      features.pNext = &query;
      auto getFeatures = inst->GetPhysicalDeviceFeatures2 ? inst->GetPhysicalDeviceFeatures2 : inst->GetPhysicalDeviceFeatures2KHR;

      if (getFeatures)
        getFeatures(physical, &features);

      presentId = query.presentId == VK_TRUE;
    }

    request->info = *info;
    request->extensions.assign(info->ppEnabledExtensionNames, info->ppEnabledExtensionNames + info->enabledExtensionCount);

    if (!hasExtension(info, VK_NV_LOW_LATENCY_2_EXTENSION_NAME))
      request->extensions.push_back(VK_NV_LOW_LATENCY_2_EXTENSION_NAME);

    if (presentId) {
      if (!hasExtension(info, VK_KHR_PRESENT_ID_EXTENSION_NAME))
        request->extensions.push_back(VK_KHR_PRESENT_ID_EXTENSION_NAME);

      // vkd3d-proton may chain the feature itself; the struct is const there,
      // so a game struct asking for no present IDs leaves them off.
      if (const auto* existing = findPresentIdFeatures(info)) {
        presentId = existing->presentId == VK_TRUE;
      } else {
        request->presentId.presentId = VK_TRUE;
        request->presentId.pNext = const_cast<void*>(info->pNext);
        request->info.pNext = &request->presentId;
      }
    }

    request->info.enabledExtensionCount   = uint32_t(request->extensions.size());
    request->info.ppEnabledExtensionNames = request->extensions.data();
    request->enabled    = true;
    request->presentIds = presentId;
  }


  void reflexInitDevice(DeviceData* dev, const ReflexDeviceRequest& request) {
    if (!request.enabled)
      return;

    ReflexState& r = dev->reflex;
    r.setLatencySleepMode = reinterpret_cast<PFN_vkSetLatencySleepModeNV>(dev->gdpa(dev->device, "vkSetLatencySleepModeNV"));
    r.latencySleep        = reinterpret_cast<PFN_vkLatencySleepNV>(dev->gdpa(dev->device, "vkLatencySleepNV"));
    r.setLatencyMarker    = reinterpret_cast<PFN_vkSetLatencyMarkerNV>(dev->gdpa(dev->device, "vkSetLatencyMarkerNV"));
    r.createSemaphore     = reinterpret_cast<PFN_vkCreateSemaphore>(dev->gdpa(dev->device, "vkCreateSemaphore"));
    r.destroySemaphore    = reinterpret_cast<PFN_vkDestroySemaphore>(dev->gdpa(dev->device, "vkDestroySemaphore"));
    r.waitSemaphores      = reinterpret_cast<PFN_vkWaitSemaphores>(dev->gdpa(dev->device, "vkWaitSemaphores"));

    if (!r.setLatencySleepMode || !r.latencySleep || !r.setLatencyMarker
     || !r.createSemaphore || !r.destroySemaphore || !r.waitSemaphores) {
      log("VK_NV_low_latency2 functions missing; Reflex is off on this device");
      return;
    }

    VkSemaphoreTypeCreateInfo type = { VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO };
    type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    VkSemaphoreCreateInfo info = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    info.pNext = &type;

    if (r.createSemaphore(dev->device, &info, nullptr, &r.semaphore) != VK_SUCCESS) {
      log("Reflex sleep semaphore could not be created; Reflex is off on this device");
      return;
    }

    r.enabled    = true;
    // vkd3d-proton chains present IDs of its own on some swap chains (FIFO);
    // adding the layer's to the others could make a swap chain's IDs go
    // backwards, so DX12 games use vkd3d's IDs where there are any.
    r.presentIds = request.presentIds && !dev->instance->vkd3d;
    log("Reflex runs on the game's swap chains (VK_NV_low_latency2%s)", r.presentIds ? ", present IDs" : "");
  }


  void reflexDestroyDevice(DeviceData* dev) {
    ReflexState& r = dev->reflex;

    if (r.semaphore && r.destroySemaphore)
      r.destroySemaphore(dev->device, r.semaphore, nullptr);

    r.semaphore = VK_NULL_HANDLE;
    r.enabled = false;
  }


  void reflexOnSubmit(DeviceData* dev) {
    ReflexState& r = dev->reflex;

    if (!r.enabled)
      return;

    std::lock_guard lock(r.mutex);

    if (r.submitMarked || !r.swapchain)
      return;

    r.submitMarked = true;
    marker(dev, r.swapchain, r.frameId, VK_LATENCY_MARKER_SIMULATION_END_NV);
    marker(dev, r.swapchain, r.frameId, VK_LATENCY_MARKER_RENDERSUBMIT_START_NV);
  }


  uint64_t reflexBeforePresent(DeviceData* dev, VkSwapchainKHR swapchain, uint64_t gameId) {
    ReflexState& r = dev->reflex;
    uint64_t id = 0;
    uint64_t chained = 0;

    if (!r.enabled)
      return 0;

    {
      std::lock_guard lock(dev->mutex);
      auto it = dev->swapchains.find(swapchain);

      if (it == dev->swapchains.end() || !it->second.latency)
        return 0;

      SwapchainInfo& sc = it->second;

      if (gameId) {
        id = gameId;
      } else {
        id = sc.lastPresentId + 1;
        chained = r.presentIds ? id : 0;
      }

      sc.lastPresentId = std::max(sc.lastPresentId, id);
    }

    std::lock_guard lock(r.mutex);

    // A frame with no submission of its own (a present-only frame).
    if (!r.submitMarked || r.swapchain != swapchain) {
      marker(dev, swapchain, id, VK_LATENCY_MARKER_SIMULATION_END_NV);
      marker(dev, swapchain, id, VK_LATENCY_MARKER_RENDERSUBMIT_START_NV);
    }

    r.submitMarked = true;
    marker(dev, swapchain, id, VK_LATENCY_MARKER_RENDERSUBMIT_END_NV);
    marker(dev, swapchain, id, VK_LATENCY_MARKER_PRESENT_START_NV);
    r.swapchain = swapchain;
    r.frameId = id;
    return chained;
  }


  void reflexAfterPresent(DeviceData* dev, VkSwapchainKHR swapchain, uint64_t presentId) {
    ReflexState& r = dev->reflex;

    if (!r.enabled)
      return;

    const uint32_t mode = currentMode(dev);
    bool applyMode = false;
    uint64_t id = 0;

    {
      std::lock_guard lock(dev->mutex);
      auto it = dev->swapchains.find(swapchain);

      if (it == dev->swapchains.end() || !it->second.latency)
        return;

      applyMode = it->second.reflexMode != mode;
      it->second.reflexMode = mode;
      id = presentId ? presentId : it->second.lastPresentId;
    }

    marker(dev, swapchain, id, VK_LATENCY_MARKER_PRESENT_END_NV);

    if (applyMode) {
      VkLatencySleepModeInfoNV info = { VK_STRUCTURE_TYPE_LATENCY_SLEEP_MODE_INFO_NV };
      info.lowLatencyMode    = mode != REMIX_VKFE_REFLEX_OFF;
      info.lowLatencyBoost   = mode == REMIX_VKFE_REFLEX_BOOST;
      info.minimumIntervalUs = 0;

      VkResult result = r.setLatencySleepMode(dev->device, swapchain, &info);
      log("Reflex mode %u on swap chain 0x%llx (%d)", mode, (unsigned long long)swapchain, result);
    }

    // Off: the driver paces nothing, so there is nothing to wait for.
    if (mode != REMIX_VKFE_REFLEX_OFF) {
      VkLatencySleepInfoNV sleep = { VK_STRUCTURE_TYPE_LATENCY_SLEEP_INFO_NV };
      sleep.signalSemaphore = r.semaphore;
      sleep.value           = ++r.sleepValue;

      if (r.latencySleep(dev->device, swapchain, &sleep) == VK_SUCCESS) {
        VkSemaphoreWaitInfo wait = { VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO };
        wait.semaphoreCount = 1;
        wait.pSemaphores    = &r.semaphore;
        wait.pValues        = &sleep.value;

        // The driver signals within a frame interval; a timeout means it
        // never will, and waiting every frame would stall the game.
        if (r.waitSemaphores(dev->device, &wait, 200000000ull) != VK_SUCCESS) {
          log("Reflex sleep was never signaled; Reflex is off on this device");
          r.enabled = false;
          return;
        }
      }
    }

    // The game's next frame starts now.
    std::lock_guard lock(r.mutex);
    r.frameId = id + 1;
    r.swapchain = swapchain;
    r.submitMarked = false;
    marker(dev, swapchain, r.frameId, VK_LATENCY_MARKER_SIMULATION_START_NV);
  }

}
