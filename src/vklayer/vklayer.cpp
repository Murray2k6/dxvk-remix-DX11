#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "vklayer.h"

namespace remix_vklayer {

  namespace {

    std::once_flag          g_decideOnce;
    const remix_vkfe_api*   g_api = nullptr;
    std::mutex              g_logMutex;
    FILE*                   g_logFile = nullptr;

    std::wstring exeDirectory() {
      wchar_t path[MAX_PATH] = {};

      if (!GetModuleFileNameW(nullptr, path, MAX_PATH))
        return std::wstring();

      wchar_t* sep = wcsrchr(path, L'\\');

      if (sep)
        *sep = L'\0';

      return path;
    }

    bool exists(const std::wstring& path) {
      return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
    }

    // Decides once per process whether Remix runs here. The layer is
    // implicit, so it is loaded into every Vulkan application; it must stay
    // inert unless this game was set up for Remix.
    void decide() {
      const std::wstring dir = exeDirectory();

      if (dir.empty())
        return;

      // Remix is installed for this game: its d3d11.dll and a Remix config
      // sit next to the executable.
      const std::wstring d3d11 = dir + L"\\d3d11.dll";
      const bool config = exists(dir + L"\\rtx.conf") || exists(dir + L"\\rtx-remix");

      if (!exists(d3d11) || !config)
        return;

      // Kernel and user-mode anti-cheat treat an injected layer as a cheat
      // (documentation/engine_knowledge/vulkan_layer_capture.md, part 2).
      static const wchar_t* kAntiCheat[] = {
        L"\\EasyAntiCheat", L"\\EasyAntiCheat_EOS", L"\\BattlEye", L"\\BEService",
        L"\\EAAntiCheat.GameServiceLauncher.exe", L"\\start_protected_game.exe",
      };

      for (const wchar_t* name : kAntiCheat) {
        if (exists(dir + name)) {
          OutputDebugStringA("[remix-vk-layer] anti-cheat found next to the game; Remix stays off\n");
          return;
        }
      }

      HMODULE module = LoadLibraryExW(d3d11.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);

      if (!module)
        return;

      auto getApi = reinterpret_cast<PFN_remix_vkfe_get_api>(GetProcAddress(module, REMIX_VKFE_ENTRY_POINT));

      if (!getApi) {
        // Someone else's d3d11.dll (or an older Remix build).
        FreeLibrary(module);
        return;
      }

      const remix_vkfe_api* api = nullptr;

      if (getApi(REMIX_VKFE_VERSION, &api) != REMIX_VKFE_OK || !api)
        return;

      const std::wstring logPath = dir + L"\\remix_vk_layer.log";
      g_logFile = _wfopen(logPath.c_str(), L"w");
      g_api = api;
    }

  }


  void log(const char* fmt, ...) {
    char line[1024];

    va_list args;
    va_start(args, fmt);
    vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);

    std::lock_guard lock(g_logMutex);
    OutputDebugStringA("[remix-vk-layer] ");
    OutputDebugStringA(line);
    OutputDebugStringA("\n");

    if (g_logFile) {
      fprintf(g_logFile, "%s\n", line);
      fflush(g_logFile);
    }
  }


  const remix_vkfe_api* remixApi() {
    std::call_once(g_decideOnce, decide);
    return g_api;
  }

}


using namespace remix_vklayer;

extern "C" {

  VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice device, const char* name);

  VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance instance, const char* name) {
    if (!name)
      return nullptr;

    // Global functions and everything this layer intercepts at instance
    // level.
    if (PFN_vkVoidFunction hook = instanceHook(name)) {
      if (!instance || !strcmp(name, "vkCreateInstance") || !strcmp(name, "vkGetInstanceProcAddr"))
        return hook;

      InstanceData* inst = findInstance(keyOf(instance));

      // Report a function only if the layers below implement it.
      if (inst && inst->gipa(instance, name))
        return hook;

      return nullptr;
    }

    if (!instance)
      return nullptr;

    InstanceData* inst = findInstance(keyOf(instance));

    if (!inst)
      return nullptr;

    // Device functions requested through the instance: the app may call
    // them on any device, so hand out the hook only where the next layer has
    // the function; the hook forwards on devices Remix does not run on.
    if (PFN_vkVoidFunction hook = deviceHook(name))
      return inst->gipa(instance, name) ? hook : nullptr;

    return inst->gipa(instance, name);
  }


  VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice device, const char* name) {
    if (!name || !device)
      return nullptr;

    if (!strcmp(name, "vkGetDeviceProcAddr"))
      return reinterpret_cast<PFN_vkVoidFunction>(&vkGetDeviceProcAddr);

    DeviceData* dev = findDevice(keyOf(device));

    if (!dev)
      return nullptr;

    PFN_vkVoidFunction next = dev->gdpa(device, name);

    // Always intercepted, so the registry entry is released.
    if (!strcmp(name, "vkDestroyDevice"))
      return deviceHook(name);

    // Devices without Remix get the next layer's functions directly: zero
    // overhead for applications Remix does not run on.
    if (!dev->active() || !next)
      return next;

    if (PFN_vkVoidFunction hook = deviceHook(name))
      return hook;

    return next;
  }


  // remix_vk_frontend.h, "DX12 draw annotation": called by Remix's
  // vkd3d-proton right before each draw.
  void remix_vklayer_annotate_draw(VkCommandBuffer commandBuffer, const remix_vkfe_binding* bindings, uint32_t count) {
    annotateDraw(commandBuffer, bindings, count);
  }


  // remix_vk_frontend.h, "GPU-driven DX12 draws".
  void remix_vklayer_note_indirect_layout(VkPipelineLayout layout, uint32_t stride, uint32_t constantCount,
                                          const remix_vkfe_indirect_constant* constants) {
    if (!layout || !constants || !constantCount || constantCount > REMIX_VKFE_MAX_INDIRECT_CONSTANTS)
      return;

    IndirectLayout indirect;
    indirect.stride = stride;
    indirect.constantCount = constantCount;
    std::memcpy(indirect.constants, constants, constantCount * sizeof(*constants));
    noteIndirectLayout(layout, indirect);
  }


  void remix_vklayer_annotate_indirect(VkCommandBuffer commandBuffer, const remix_vkfe_indirect_annotation* annotation) {
    annotateIndirect(commandBuffer, annotation);
  }


  uint32_t remix_vklayer_describe_view(VkDevice device, VkImageView view, VkImageLayout layout, remix_vkfe_binding* out) {
    return describeView(device, view, layout, out);
  }


  VKAPI_ATTR VkResult VKAPI_CALL vkNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface* pVersionStruct) {
    if (!pVersionStruct || pVersionStruct->sType != LAYER_NEGOTIATE_INTERFACE_STRUCT)
      return VK_ERROR_INITIALIZATION_FAILED;

    if (pVersionStruct->loaderLayerInterfaceVersion > 2)
      pVersionStruct->loaderLayerInterfaceVersion = 2;

    pVersionStruct->pfnGetInstanceProcAddr       = &vkGetInstanceProcAddr;
    pVersionStruct->pfnGetDeviceProcAddr         = &vkGetDeviceProcAddr;
    pVersionStruct->pfnGetPhysicalDeviceProcAddr = nullptr;
    return VK_SUCCESS;
  }

}
