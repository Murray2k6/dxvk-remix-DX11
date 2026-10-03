#include <cstring>
#include <cwctype>
#include <shared_mutex>

#include "vklayer.h"

// Instance and device creation, the object registries, and the extensions
// the layer hides from the game.

namespace remix_vklayer {

  namespace {

    std::shared_mutex                                         g_registryMutex;
    std::unordered_map<DispatchKey, std::unique_ptr<InstanceData>> g_instances;
    std::unordered_map<DispatchKey, std::unique_ptr<DeviceData>>   g_devices;

    // Engines that translate another API to Vulkan and that Remix does not
    // trace here. Remix's own DXVK instance ("DXVK_NvRemix") passes through
    // too. vkd3d-proton is not in the list: DX12 games are traced through
    // this layer, with D3D12 bindings from Remix's vkd3d patch
    // (remix_vklayer_annotate_draw).
    bool isTranslationLayer(const VkApplicationInfo* app) {
      if (!app || !app->pEngineName)
        return false;

      static const char* kNames[] = { "DXVK", "WineD3D", "Zink", "ANGLE" };

      for (const char* n : kNames) {
        if (!_strnicmp(app->pEngineName, n, strlen(n)))
          return true;
      }

      return false;
    }

    // Extensions that take draws out of reach of capture while Remix runs:
    //   - descriptor buffers: descriptors become plain memory writes;
    //   - graphics pipeline libraries / shader objects / module identifiers /
    //     pipeline binaries: no complete pipeline with its SPIR-V is created;
    //   - mesh shaders and device-generated commands: geometry and draws
    //     come from the GPU with no transform feedback;
    //   - 64-bit image atomics: Unreal Engine 5 enables Nanite only with
    //     them (Vulkan RHI: shaderImageInt64Atomics; D3D12 through
    //     vkd3d-proton: AtomicInt64OnTypedResourceSupported). Nanite draws
    //     its meshes by software rasterization into a visibility buffer;
    //     without it UE5 draws their fallback meshes with ordinary draws,
    //     which are captured with their materials.
    // Engines fall back to their classic paths when these are absent
    // (vulkan_layer_capture.md, section 1.6).
    const char* const kHiddenExtensions[] = {
      VK_EXT_DESCRIPTOR_BUFFER_EXTENSION_NAME,
      VK_EXT_GRAPHICS_PIPELINE_LIBRARY_EXTENSION_NAME,
      VK_EXT_SHADER_OBJECT_EXTENSION_NAME,
      VK_EXT_SHADER_MODULE_IDENTIFIER_EXTENSION_NAME,
      VK_KHR_PIPELINE_BINARY_EXTENSION_NAME,
      VK_EXT_MESH_SHADER_EXTENSION_NAME,
      VK_NV_MESH_SHADER_EXTENSION_NAME,
      VK_NV_DEVICE_GENERATED_COMMANDS_EXTENSION_NAME,
      VK_EXT_DEVICE_GENERATED_COMMANDS_EXTENSION_NAME,
      VK_EXT_SHADER_IMAGE_ATOMIC_INT64_EXTENSION_NAME,
    };

    // Remix takes over upscaling and ray tracing, so the game must not run
    // its own:
    //   - NVX binary import / image view handle: NGX (DLSS, DLSS frame
    //     generation, Streamline) refuses to create its features on a
    //     Vulkan device without them, so the game reports DLSS unavailable;
    //     vkd3d-proton's DLSS interop needs them too. Optical flow is DLSS
    //     frame generation's other requirement.
    //   - ray tracing: without acceleration structures, ray tracing
    //     pipelines and ray query a Vulkan game turns its RT off, and
    //     vkd3d-proton reports D3D12_RAYTRACING_TIER_NOT_SUPPORTED, so a DX12
    //     game does the same (gameRequiresHardwareRayTracing excepts games
    //     that refuse to start without it).
    // Remix's own device still gets every one of these: DxvkDeviceImporter
    // queries the GPU below this layer and adds what Remix needs to the
    // game's vkCreateDevice.
    const char* const kHiddenUpscalerExtensions[] = {
      VK_NVX_BINARY_IMPORT_EXTENSION_NAME,
      VK_NVX_IMAGE_VIEW_HANDLE_EXTENSION_NAME,
      VK_NV_OPTICAL_FLOW_EXTENSION_NAME,
    };

    const char* const kHiddenRayTracingExtensions[] = {
      VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME,
      VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME,
      VK_KHR_RAY_QUERY_EXTENSION_NAME,
      VK_KHR_RAY_TRACING_MAINTENANCE_1_EXTENSION_NAME,
      VK_KHR_RAY_TRACING_POSITION_FETCH_EXTENSION_NAME,
      VK_NV_RAY_TRACING_EXTENSION_NAME,
      VK_NV_RAY_TRACING_MOTION_BLUR_EXTENSION_NAME,
      VK_NV_RAY_TRACING_INVOCATION_REORDER_EXTENSION_NAME,
      VK_EXT_OPACITY_MICROMAP_EXTENSION_NAME,
    };

    // Games that exit at startup on a GPU without hardware ray tracing. They
    // keep it (and run their own RT beside Remix's).
    bool gameRequiresHardwareRayTracing() {
      static const bool required = [] {
        wchar_t path[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, path, MAX_PATH);
        std::wstring p(path);
        for (auto& c : p)
          c = wchar_t(towlower(c));
        auto has = [&](const wchar_t* s) { return p.find(s) != std::wstring::npos; };
        const bool result =
             has(L"\\thegreatcircle.exe")                    // Indiana Jones and the Great Circle
          || has(L"\\doomthedarkages.exe")                   // DOOM: The Dark Ages
          || (has(L"enhanced edition") && has(L"\\metroexodus.exe"));  // Metro Exodus Enhanced Edition
        if (result)
          log("this game requires hardware ray tracing: the game keeps its ray tracing extensions");
        return result;
      }();
      return required;
    }

    // vkd3d-proton keeps device-generated commands: it uses them only for
    // ExecuteIndirect command signatures that also set root constants or
    // views, and without them it skips those calls outright (Starfield draws
    // its opaque world that way). Plain draws and plain indirect draws do
    // not use them.
    bool isDeviceGeneratedCommands(const char* name) {
      return !strcmp(name, VK_NV_DEVICE_GENERATED_COMMANDS_EXTENSION_NAME)
          || !strcmp(name, VK_EXT_DEVICE_GENERATED_COMMANDS_EXTENSION_NAME);
    }

    bool isHidden(const char* name, const InstanceData* inst) {
      if (inst && inst->vkd3d && isDeviceGeneratedCommands(name))
        return false;

      for (const char* h : kHiddenExtensions) {
        if (!strcmp(name, h))
          return true;
      }

      for (const char* h : kHiddenUpscalerExtensions) {
        if (!strcmp(name, h))
          return true;
      }

      if (!gameRequiresHardwareRayTracing()) {
        for (const char* h : kHiddenRayTracingExtensions) {
          if (!strcmp(name, h))
            return true;
        }
      }

      return false;
    }

    // Matching feature structs, zeroed when queried.
    size_t hiddenFeatureStructSize(VkStructureType sType, const InstanceData* inst) {
      switch (sType) {
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_OPTICAL_FLOW_FEATURES_NV:                return sizeof(VkPhysicalDeviceOpticalFlowFeaturesNV);
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEVICE_GENERATED_COMMANDS_FEATURES_NV:
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEVICE_GENERATED_COMMANDS_FEATURES_EXT:
          if (inst && inst->vkd3d)
            return 0;
          break;
        default: break;
      }

      if (!gameRequiresHardwareRayTracing()) {
        switch (sType) {
          case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR:      return sizeof(VkPhysicalDeviceAccelerationStructureFeaturesKHR);
          case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR:        return sizeof(VkPhysicalDeviceRayTracingPipelineFeaturesKHR);
          case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR:                   return sizeof(VkPhysicalDeviceRayQueryFeaturesKHR);
          case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_MAINTENANCE_1_FEATURES_KHR:   return sizeof(VkPhysicalDeviceRayTracingMaintenance1FeaturesKHR);
          case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_POSITION_FETCH_FEATURES_KHR:  return sizeof(VkPhysicalDeviceRayTracingPositionFetchFeaturesKHR);
          case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_MOTION_BLUR_FEATURES_NV:      return sizeof(VkPhysicalDeviceRayTracingMotionBlurFeaturesNV);
          case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_INVOCATION_REORDER_FEATURES_NV: return sizeof(VkPhysicalDeviceRayTracingInvocationReorderFeaturesNV);
          case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_OPACITY_MICROMAP_FEATURES_EXT:            return sizeof(VkPhysicalDeviceOpacityMicromapFeaturesEXT);
          default: break;
        }
      }

      switch (sType) {
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_BUFFER_FEATURES_EXT:          return sizeof(VkPhysicalDeviceDescriptorBufferFeaturesEXT);
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_FEATURES_EXT:  return sizeof(VkPhysicalDeviceGraphicsPipelineLibraryFeaturesEXT);
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_OBJECT_FEATURES_EXT:              return sizeof(VkPhysicalDeviceShaderObjectFeaturesEXT);
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_MODULE_IDENTIFIER_FEATURES_EXT:   return sizeof(VkPhysicalDeviceShaderModuleIdentifierFeaturesEXT);
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_BINARY_FEATURES_KHR:            return sizeof(VkPhysicalDevicePipelineBinaryFeaturesKHR);
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT:                return sizeof(VkPhysicalDeviceMeshShaderFeaturesEXT);
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_NV:                 return sizeof(VkPhysicalDeviceMeshShaderFeaturesNV);
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEVICE_GENERATED_COMMANDS_FEATURES_NV:   return sizeof(VkPhysicalDeviceDeviceGeneratedCommandsFeaturesNV);
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEVICE_GENERATED_COMMANDS_FEATURES_EXT:  return sizeof(VkPhysicalDeviceDeviceGeneratedCommandsFeaturesEXT);
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_IMAGE_ATOMIC_INT64_FEATURES_EXT:  return sizeof(VkPhysicalDeviceShaderImageAtomicInt64FeaturesEXT);
        default: return 0;
      }
    }

    template<typename T>
    T* findChain(const void* pNext, VkStructureType sType) {
      for (auto* s = reinterpret_cast<const VkBaseInStructure*>(pNext); s; s = s->pNext) {
        if (s->sType == sType)
          return reinterpret_cast<T*>(const_cast<VkBaseInStructure*>(s));
      }

      return nullptr;
    }


    VKAPI_ATTR VkResult VKAPI_CALL CreateInstance(
      const VkInstanceCreateInfo*   pCreateInfo,
      const VkAllocationCallbacks*  pAllocator,
            VkInstance*             pInstance) {
      VkLayerInstanceCreateInfo* chain = nullptr;

      for (auto* s = reinterpret_cast<const VkBaseInStructure*>(pCreateInfo->pNext); s; s = s->pNext) {
        auto* c = reinterpret_cast<VkLayerInstanceCreateInfo*>(const_cast<VkBaseInStructure*>(s));

        if (c->sType == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO && c->function == VK_LAYER_LINK_INFO) {
          chain = c;
          break;
        }
      }

      if (!chain || !chain->u.pLayerInfo)
        return VK_ERROR_INITIALIZATION_FAILED;

      PFN_vkGetInstanceProcAddr gipa = chain->u.pLayerInfo->pfnNextGetInstanceProcAddr;
      auto createInstance = reinterpret_cast<PFN_vkCreateInstance>(gipa(VK_NULL_HANDLE, "vkCreateInstance"));

      if (!createInstance)
        return VK_ERROR_INITIALIZATION_FAILED;

      // Advance the link for the next layer. The create info Remix returns
      // shares this pNext chain, so the change applies to both. Layers below
      // advance it again in place; a retry restores this value first.
      chain->u.pLayerInfo = chain->u.pLayerInfo->pNext;
      VkLayerInstanceLink* const nextLink = chain->u.pLayerInfo;

      const remix_vkfe_api* api = isTranslationLayer(pCreateInfo->pApplicationInfo) ? nullptr : remixApi();
      const VkInstanceCreateInfo* info = pCreateInfo;
      remix_vkfe_pending pending = nullptr;

      if (api) {
        const VkApplicationInfo* app = pCreateInfo->pApplicationInfo;
        const bool vkd3d = app && app->pEngineName && !_strnicmp(app->pEngineName, "vkd3d", 5);
        remix_vkfe_instance_request request = {
          vkd3d ? REMIX_VKFE_FRONTEND_DX12_VKD3D : REMIX_VKFE_FRONTEND_VULKAN_LAYER, pCreateInfo };
        remix_vkfe_instance_plan plan = {};

        if (api->plan_instance(&request, &plan) == REMIX_VKFE_OK) {
          info = plan.create_info;
          pending = plan.pending;
        }
      }

      VkResult result = createInstance(info, pAllocator, pInstance);

      if (result != VK_SUCCESS && pending) {
        // The game's own request, without Remix's changes.
        remix_vkfe_instance unused = nullptr;
        api->finish_instance(pending, result, VK_NULL_HANDLE, nullptr, &unused);
        pending = nullptr;
        log("vkCreateInstance with Remix's API version failed (%d); retrying the game's request", result);
        chain->u.pLayerInfo = nextLink;
        result = createInstance(pCreateInfo, pAllocator, pInstance);
      }

      if (result != VK_SUCCESS)
        return result;

      auto data = std::make_unique<InstanceData>();
      data->instance = *pInstance;
      data->gipa = gipa;
      {
        const VkApplicationInfo* app = pCreateInfo->pApplicationInfo;
        data->vkd3d = app && app->pEngineName && !_strnicmp(app->pEngineName, "vkd3d", 5);
      }
      data->DestroyInstance = reinterpret_cast<PFN_vkDestroyInstance>(gipa(*pInstance, "vkDestroyInstance"));
      data->EnumerateDeviceExtensionProperties = reinterpret_cast<PFN_vkEnumerateDeviceExtensionProperties>(
        gipa(*pInstance, "vkEnumerateDeviceExtensionProperties"));
      data->GetPhysicalDeviceFeatures2 = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(
        gipa(*pInstance, "vkGetPhysicalDeviceFeatures2"));
      data->GetPhysicalDeviceFeatures2KHR = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2KHR>(
        gipa(*pInstance, "vkGetPhysicalDeviceFeatures2KHR"));
      data->GetPhysicalDeviceSurfaceCapabilitiesKHR = reinterpret_cast<PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR>(
        gipa(*pInstance, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR"));
      data->CreateWin32SurfaceKHR = reinterpret_cast<PFN_vkCreateWin32SurfaceKHR>(
        gipa(*pInstance, "vkCreateWin32SurfaceKHR"));
      data->DestroySurfaceKHR = reinterpret_cast<PFN_vkDestroySurfaceKHR>(
        gipa(*pInstance, "vkDestroySurfaceKHR"));

      if (pending) {
        if (api->finish_instance(pending, result, *pInstance, gipa, &data->remix) == REMIX_VKFE_OK)
          log("Remix adopted the game's VkInstance");
        else
          log("Remix could not adopt the game's VkInstance; see Remix's log");
      }

      std::unique_lock lock(g_registryMutex);
      g_instances[keyOf(*pInstance)] = std::move(data);
      return VK_SUCCESS;
    }


    VKAPI_ATTR void VKAPI_CALL DestroyInstance(VkInstance instance, const VkAllocationCallbacks* pAllocator) {
      if (!instance)
        return;

      std::unique_ptr<InstanceData> data;

      {
        std::unique_lock lock(g_registryMutex);
        auto it = g_instances.find(keyOf(instance));

        if (it == g_instances.end())
          return;

        data = std::move(it->second);
        g_instances.erase(it);
      }

      if (data->remix)
        remixApi()->destroy_instance(data->remix);

      data->DestroyInstance(instance, pAllocator);
    }


    VKAPI_ATTR VkResult VKAPI_CALL CreateWin32SurfaceKHR(
            VkInstance                      instance,
      const VkWin32SurfaceCreateInfoKHR*    pCreateInfo,
      const VkAllocationCallbacks*          pAllocator,
            VkSurfaceKHR*                   pSurface) {
      InstanceData* inst = findInstance(keyOf(instance));

      if (!inst || !inst->CreateWin32SurfaceKHR)
        return VK_ERROR_EXTENSION_NOT_PRESENT;

      VkResult result = inst->CreateWin32SurfaceKHR(instance, pCreateInfo, pAllocator, pSurface);

      if (result == VK_SUCCESS && inst->remix) {
        std::lock_guard lock(inst->surfaceMutex);
        inst->surfaceWindows[*pSurface] = pCreateInfo->hwnd;
      }

      return result;
    }


    VKAPI_ATTR void VKAPI_CALL DestroySurfaceKHR(VkInstance instance, VkSurfaceKHR surface,
                                                 const VkAllocationCallbacks* pAllocator) {
      InstanceData* inst = findInstance(keyOf(instance));

      if (!inst || !inst->DestroySurfaceKHR)
        return;

      {
        std::lock_guard lock(inst->surfaceMutex);
        inst->surfaceWindows.erase(surface);
      }

      inst->DestroySurfaceKHR(instance, surface, pAllocator);
    }


    VKAPI_ATTR VkResult VKAPI_CALL EnumerateDeviceExtensionProperties(
            VkPhysicalDevice        physicalDevice,
      const char*                   pLayerName,
            uint32_t*               pPropertyCount,
            VkExtensionProperties*  pProperties) {
      InstanceData* inst = findInstanceForPhysicalDevice(physicalDevice);

      if (!inst)
        return VK_ERROR_INITIALIZATION_FAILED;

      if (!inst->remix || (pLayerName && *pLayerName))
        return inst->EnumerateDeviceExtensionProperties(physicalDevice, pLayerName, pPropertyCount, pProperties);

      uint32_t count = 0;
      VkResult result = inst->EnumerateDeviceExtensionProperties(physicalDevice, nullptr, &count, nullptr);

      if (result != VK_SUCCESS)
        return result;

      std::vector<VkExtensionProperties> all(count);
      result = inst->EnumerateDeviceExtensionProperties(physicalDevice, nullptr, &count, all.data());

      if (result != VK_SUCCESS && result != VK_INCOMPLETE)
        return result;

      all.resize(count);

      std::vector<VkExtensionProperties> shown;

      for (const auto& e : all) {
        if (!isHidden(e.extensionName, inst))
          shown.push_back(e);
      }

      if (!pProperties) {
        *pPropertyCount = uint32_t(shown.size());
        return VK_SUCCESS;
      }

      const uint32_t n = std::min<uint32_t>(*pPropertyCount, uint32_t(shown.size()));
      std::memcpy(pProperties, shown.data(), n * sizeof(VkExtensionProperties));
      *pPropertyCount = n;
      return n < shown.size() ? VK_INCOMPLETE : VK_SUCCESS;
    }


    void maskHiddenFeatures(VkPhysicalDeviceFeatures2* features, const InstanceData* inst) {
      for (auto* s = reinterpret_cast<VkBaseOutStructure*>(features->pNext); s; s = s->pNext) {
        const size_t size = hiddenFeatureStructSize(s->sType, inst);

        if (size > sizeof(VkBaseOutStructure))
          std::memset(reinterpret_cast<uint8_t*>(s) + sizeof(VkBaseOutStructure), 0, size - sizeof(VkBaseOutStructure));
      }
    }


    VKAPI_ATTR void VKAPI_CALL GetPhysicalDeviceFeatures2(VkPhysicalDevice physicalDevice, VkPhysicalDeviceFeatures2* pFeatures) {
      InstanceData* inst = findInstanceForPhysicalDevice(physicalDevice);

      if (!inst || !inst->GetPhysicalDeviceFeatures2)
        return;

      inst->GetPhysicalDeviceFeatures2(physicalDevice, pFeatures);

      if (inst->remix)
        maskHiddenFeatures(pFeatures, inst);
    }


    VKAPI_ATTR void VKAPI_CALL GetPhysicalDeviceFeatures2KHR(VkPhysicalDevice physicalDevice, VkPhysicalDeviceFeatures2* pFeatures) {
      InstanceData* inst = findInstanceForPhysicalDevice(physicalDevice);

      if (!inst || !inst->GetPhysicalDeviceFeatures2KHR)
        return;

      inst->GetPhysicalDeviceFeatures2KHR(physicalDevice, pFeatures);

      if (inst->remix)
        maskHiddenFeatures(pFeatures, inst);
    }


    VKAPI_ATTR VkResult VKAPI_CALL CreateDevice(
            VkPhysicalDevice        physicalDevice,
      const VkDeviceCreateInfo*     pCreateInfo,
      const VkAllocationCallbacks*  pAllocator,
            VkDevice*               pDevice) {
      InstanceData* inst = findInstanceForPhysicalDevice(physicalDevice);

      VkLayerDeviceCreateInfo* chain = nullptr;

      for (auto* s = reinterpret_cast<const VkBaseInStructure*>(pCreateInfo->pNext); s; s = s->pNext) {
        auto* c = reinterpret_cast<VkLayerDeviceCreateInfo*>(const_cast<VkBaseInStructure*>(s));

        if (c->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO && c->function == VK_LAYER_LINK_INFO) {
          chain = c;
          break;
        }
      }

      if (!inst || !chain || !chain->u.pLayerInfo)
        return VK_ERROR_INITIALIZATION_FAILED;

      PFN_vkGetInstanceProcAddr gipa = chain->u.pLayerInfo->pfnNextGetInstanceProcAddr;
      PFN_vkGetDeviceProcAddr   gdpa = chain->u.pLayerInfo->pfnNextGetDeviceProcAddr;
      auto createDevice = reinterpret_cast<PFN_vkCreateDevice>(gipa(inst->instance, "vkCreateDevice"));

      if (!createDevice)
        return VK_ERROR_INITIALIZATION_FAILED;

      chain->u.pLayerInfo = chain->u.pLayerInfo->pNext;
      VkLayerDeviceLink* const nextLink = chain->u.pLayerInfo;

      const remix_vkfe_api* api = inst->remix ? remixApi() : nullptr;
      const VkDeviceCreateInfo* info = pCreateInfo;
      remix_vkfe_pending pending = nullptr;

      ReflexDeviceRequest reflex;
      // Remix's planned create info, before Reflex adds to it.
      const VkDeviceCreateInfo* remixInfo = pCreateInfo;

      if (api) {
        remix_vkfe_device_request request = { inst->remix, physicalDevice, pCreateInfo };
        remix_vkfe_device_plan plan = {};

        if (api->plan_device(&request, &plan) == REMIX_VKFE_OK) {
          info = plan.create_info;
          remixInfo = info;
          pending = plan.pending;

          reflexPlanDevice(inst, physicalDevice, pCreateInfo, info, &reflex);

          if (reflex.enabled)
            info = &reflex.info;
        } else {
          log("Remix cannot run on this device (no ray tracing support or no spare queue); passing through");
        }
      }

      VkResult result = createDevice(physicalDevice, info, pAllocator, pDevice);

      if (result != VK_SUCCESS && reflex.enabled) {
        log("vkCreateDevice with Reflex's extensions failed (%d); retrying without them", result);
        reflex.enabled = false;
        chain->u.pLayerInfo = nextLink;
        result = createDevice(physicalDevice, remixInfo, pAllocator, pDevice);
      }

      if (result != VK_SUCCESS && pending) {
        remix_vkfe_device unused = nullptr;
        api->finish_device(pending, result, VK_NULL_HANDLE, nullptr, &unused);
        pending = nullptr;
        log("vkCreateDevice with Remix's extensions failed (%d); retrying the game's request", result);
        chain->u.pLayerInfo = nextLink;
        result = createDevice(physicalDevice, pCreateInfo, pAllocator, pDevice);
      }

      if (result != VK_SUCCESS)
        return result;

      auto data = std::make_unique<DeviceData>();
      data->device   = *pDevice;
      data->physical = physicalDevice;
      data->instance = inst;
      data->gdpa     = gdpa;

#define REMIX_VKLAYER_LOAD(name) data->name = reinterpret_cast<PFN_vk##name>(gdpa(*pDevice, "vk" #name));
      REMIX_VKLAYER_DEVICE_FUNCTIONS(REMIX_VKLAYER_LOAD)
#undef REMIX_VKLAYER_LOAD

      if (pending) {
        if (api->finish_device(pending, result, *pDevice, gdpa, &data->remix) == REMIX_VKFE_OK)
          log("Remix is running on the game's VkDevice");
        else
          log("Remix could not start on the game's VkDevice; see Remix's log");
      }

      if (data->remix)
        reflexInitDevice(data.get(), reflex);

      std::unique_lock lock(g_registryMutex);
      g_devices[keyOf(*pDevice)] = std::move(data);
      return VK_SUCCESS;
    }


    VKAPI_ATTR void VKAPI_CALL DestroyDevice(VkDevice device, const VkAllocationCallbacks* pAllocator) {
      if (!device)
        return;

      // Remix drains its GPU work and releases everything on the device
      // before the device goes away. The device stays registered meanwhile,
      // with Remix detached: a call Remix's teardown makes through the
      // loader reaches the layer's hooks and must pass straight through.
      remix_vkfe_device remix = nullptr;

      {
        std::shared_lock lock(g_registryMutex);
        auto it = g_devices.find(keyOf(device));

        if (it == g_devices.end())
          return;

        remix = it->second->remix;
        it->second->remix = nullptr;
      }

      if (remix)
        remixApi()->destroy_device(remix);

      std::unique_ptr<DeviceData> data;

      {
        std::unique_lock lock(g_registryMutex);
        auto it = g_devices.find(keyOf(device));

        if (it == g_devices.end())
          return;

        data = std::move(it->second);
        g_devices.erase(it);
      }

      reflexDestroyDevice(data.get());

      // The layer's own framebuffers around Remix's bake / UI layer images.
      // The game's work has finished by now (vkDestroyDevice requires it).
      if (data->DestroyFramebuffer) {
        for (const auto& fb : data->bakeFramebuffers)
          data->DestroyFramebuffer(device, fb.second, nullptr);

        for (const auto& fb : data->retiredFramebuffers)
          data->DestroyFramebuffer(device, fb.second, nullptr);
      }

      data->DestroyDevice(device, pAllocator);
    }

  }


  InstanceData* findInstance(DispatchKey key) {
    std::shared_lock lock(g_registryMutex);
    auto it = g_instances.find(key);
    return it != g_instances.end() ? it->second.get() : nullptr;
  }


  InstanceData* findInstanceForPhysicalDevice(VkPhysicalDevice physical) {
    // Physical devices handed to layers carry their instance's dispatch
    // table pointer.
    return physical ? findInstance(keyOf(physical)) : nullptr;
  }


  DeviceData* findDevice(DispatchKey key) {
    std::shared_lock lock(g_registryMutex);
    auto it = g_devices.find(key);
    return it != g_devices.end() ? it->second.get() : nullptr;
  }


  PFN_vkVoidFunction instanceHook(const char* name) {
#define REMIX_VKLAYER_HOOK(fn) if (!strcmp(name, "vk" #fn)) return reinterpret_cast<PFN_vkVoidFunction>(&fn);
    REMIX_VKLAYER_HOOK(CreateInstance)
    REMIX_VKLAYER_HOOK(DestroyInstance)
    REMIX_VKLAYER_HOOK(CreateDevice)
    REMIX_VKLAYER_HOOK(EnumerateDeviceExtensionProperties)
    REMIX_VKLAYER_HOOK(GetPhysicalDeviceFeatures2)
    REMIX_VKLAYER_HOOK(GetPhysicalDeviceFeatures2KHR)
    REMIX_VKLAYER_HOOK(CreateWin32SurfaceKHR)
    REMIX_VKLAYER_HOOK(DestroySurfaceKHR)
#undef REMIX_VKLAYER_HOOK
    return nullptr;
  }


  PFN_vkVoidFunction deviceHook(const char* name) {
    if (!strcmp(name, "vkDestroyDevice"))
      return reinterpret_cast<PFN_vkVoidFunction>(&DestroyDevice);

    if (PFN_vkVoidFunction fn = trackHook(name))
      return fn;

    return commandHook(name);
  }

}
