/*
* Copyright (c) 2022-2026, NVIDIA CORPORATION. All rights reserved.
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
#include "rtx_nrd_context.h"
#include "dxvk_device.h"
#include "dxvk_scoped_annotation.h"
#include "rtx.h"
#include "rtx_options.h"
#include "rtx/pass/nrd_args.h"
#include "../../util/util_string.h"
#include "../../util/util_global_time.h"
#include "../../util/util_env.h"
#include "../../spirv/spirv_nrd_image_format.h"
#include <Shlwapi.h>
#include <filesystem>
#include <vector>
#include <algorithm>
#include <string>
#include <string_view>
#include <malloc.h>

namespace nrd {
  using pfnCreateInstance = Result (NRD_CALL *)(const InstanceCreationDesc& instanceCreationDesc, Instance*& instance);
  using pfnDestroyInstance = void (NRD_CALL *)(Instance& instance);
  using pfnGetLibraryDesc = const LibraryDesc& (NRD_CALL *)();
  using pfnGetInstanceDesc = const InstanceDesc& (NRD_CALL *)(const Instance& instance);
  using pfnSetCommonSettings = Result (NRD_CALL *)(Instance& instance, const CommonSettings& commonSettings);
  using pfnSetDenoiserSettings = Result (NRD_CALL *)(Instance& instance, Identifier identifier, const void* denoiserSettings);
  using pfnGetComputeDispatches = Result (NRD_CALL *)(Instance& instance, const Identifier* identifiers, uint32_t identifiersNum, const DispatchDesc*& dispatchDescs, uint32_t& dispatchDescsNum);
  using pfnGetResourceTypeString = const char* (NRD_CALL *)(ResourceType resourceType);
  using pfnGetDenoiserString = const char* (NRD_CALL *)(Denoiser denoiser);

  struct DispatchNRD {
    pfnCreateInstance CreateInstance;
    pfnDestroyInstance DestroyInstance;
    pfnGetLibraryDesc GetLibraryDesc;
    pfnGetInstanceDesc GetInstanceDesc;
    pfnSetCommonSettings SetCommonSettings;
    pfnSetDenoiserSettings SetDenoiserSettings;
    pfnGetComputeDispatches GetComputeDispatches;
    pfnGetResourceTypeString GetResourceTypeString;
    pfnGetDenoiserString GetDenoiserString;
  };

  struct Library {
    HMODULE module = nullptr;
    DispatchNRD dispatch = {};

    ~Library() {
      if (module)
        FreeLibrary(module);
    }
  };

  // DX11_V270_NRD_ROBUST_LOAD: locate NRD.dll across every plausible layout.
  // The old loader only tried the runtime DLL's OWN directory; when the
  // runtime lives in .trex but NRD.dll landed a level up (or vice versa, or
  // the game dir differs from the module dir under the bridge), the load
  // failed and the denoiser silently became a no-op ("denoiser is null" =
  // raw, noisy path tracing). Try, in order: the runtime module's dir, its
  // .trex subdir, its parent dir, the process exe's dir, and finally a bare
  // name (PATH/CWD). First successful load wins; each attempt is logged.
  static HMODULE loadNrdLibrary() {
    HMODULE hModule = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&loadNrdLibrary), &hModule);

    wchar_t moduleDir[MAX_PATH] = {};
    if (hModule != nullptr && GetModuleFileNameW(hModule, moduleDir, MAX_PATH) != 0)
      PathRemoveFileSpecW(moduleDir);

    wchar_t exeDir[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, exeDir, MAX_PATH) != 0)
      PathRemoveFileSpecW(exeDir);

    std::vector<std::filesystem::path> candidates;
    // DX11_V290_RUNTIME_DIR: the dedicated Remix runtime directory is the
    // preferred home for the satellite payload; try it before the flat
    // game-folder locations so both layouts work.
    wchar_t runtimeDir[MAX_PATH] = {};
    if (dxvk::env::remixResolveRuntimeDirectoryW(runtimeDir, MAX_PATH) != 0)
      candidates.push_back(std::filesystem::path(runtimeDir) / L"NRD.dll");
    auto addDir = [&candidates](const wchar_t* dir) {
      if (dir == nullptr || dir[0] == L'\0')
        return;
      std::filesystem::path base(dir);
      candidates.push_back(base / L"NRD.dll");
      candidates.push_back(base / L".trex" / L"NRD.dll");
      candidates.push_back(base.parent_path() / L"NRD.dll");
    };
    addDir(moduleDir);
    addDir(exeDir);
    candidates.push_back(std::filesystem::path(L"NRD.dll"));  // PATH / CWD

    std::vector<std::wstring> tried;
    for (const auto& cand : candidates) {
      const std::wstring w = cand.wstring();
      if (std::find(tried.begin(), tried.end(), w) != tried.end())
        continue;
      tried.push_back(w);

      HMODULE hNRD = LoadLibraryExW(w.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
      if (hNRD != nullptr) {
        dxvk::Logger::info(dxvk::str::format("[Remix-DX11] NRD.dll loaded from: ", dxvk::str::fromws(w.c_str())));
        return hNRD;
      }
    }

    dxvk::Logger::err(dxvk::str::format(
      "[Remix-DX11] Unable to load NRD.dll from any known location (lastError=",
      (uint32_t) GetLastError(), ") - DENOISER DISABLED (path tracing will be noisy). ",
      "Copy NRD.dll next to the runtime d3d11.dll."));
    return NULL;
  }

  static void initialize(Library& library) {
    HMODULE hNRD = loadNrdLibrary();
    if (hNRD == NULL) {
      return;
    }
    DispatchNRD dispatch = {};

    // Resolve and VALIDATE every entry point. A null proc that the old code
    // called unchecked (GetLibraryDesc at minimum) was an access violation;
    // now a missing export disables the denoiser cleanly instead of crashing.
    bool allProcs = true;
    auto getProc = [&](const char* name, void** out) {
      *out = reinterpret_cast<void*>(GetProcAddress(hNRD, name));
      if (*out == nullptr) {
        dxvk::Logger::err(dxvk::str::format("[Remix-DX11] NRD.dll missing export '", name, "' - denoiser disabled."));
        allProcs = false;
      }
    };
    getProc("CreateInstance",       reinterpret_cast<void**>(&dispatch.CreateInstance));
    getProc("DestroyInstance",      reinterpret_cast<void**>(&dispatch.DestroyInstance));
    getProc("GetLibraryDesc",       reinterpret_cast<void**>(&dispatch.GetLibraryDesc));
    getProc("GetInstanceDesc",      reinterpret_cast<void**>(&dispatch.GetInstanceDesc));
    getProc("SetCommonSettings",    reinterpret_cast<void**>(&dispatch.SetCommonSettings));
    getProc("SetDenoiserSettings",  reinterpret_cast<void**>(&dispatch.SetDenoiserSettings));
    getProc("GetComputeDispatches", reinterpret_cast<void**>(&dispatch.GetComputeDispatches));
    getProc("GetResourceTypeString",reinterpret_cast<void**>(&dispatch.GetResourceTypeString));
    getProc("GetDenoiserString",    reinterpret_cast<void**>(&dispatch.GetDenoiserString));

    if (!allProcs) {
      FreeLibrary(hNRD);
      return;
    }

    const LibraryDesc& desc = dispatch.GetLibraryDesc();
    // ABI compatibility is major.minor; the build number is a patch level.
    // Reject on major/minor mismatch (would crash or misbehave), but only
    // WARN on a build-number mismatch so a compatible patch build still
    // denoises instead of silently disabling.
    if (desc.versionMajor != NRD_VERSION_MAJOR || desc.versionMinor != NRD_VERSION_MINOR) {
      dxvk::Logger::err(dxvk::str::format(
        "[Remix-DX11] NRD.dll ABI mismatch: loaded v", uint32_t(desc.versionMajor), ".", uint32_t(desc.versionMinor), ".", uint32_t(desc.versionBuild),
        " but built against v", NRD_VERSION_MAJOR, ".", NRD_VERSION_MINOR, ".", NRD_VERSION_BUILD,
        " - denoiser disabled. Ship the matching NRD.dll."));
      FreeLibrary(hNRD);
      return;
    }
    if (desc.versionBuild != NRD_VERSION_BUILD) {
      dxvk::Logger::warn(dxvk::str::format(
        "[Remix-DX11] NRD.dll build mismatch: loaded v", uint32_t(desc.versionMajor), ".", uint32_t(desc.versionMinor), ".", uint32_t(desc.versionBuild),
        " vs built v", NRD_VERSION_MAJOR, ".", NRD_VERSION_MINOR, ".", NRD_VERSION_BUILD,
        " - proceeding (ABI-compatible)."));
    }

    // DX11_V264: positive confirmation - every log now states denoiser status.
    dxvk::Logger::info(dxvk::str::format(
      "[Remix-DX11] NRD denoiser loaded: v",
      uint32_t(desc.versionMajor), ".", uint32_t(desc.versionMinor), ".", uint32_t(desc.versionBuild)));

    library.module = hNRD;
    library.dispatch = dispatch;
  }

  static std::shared_ptr<Library> acquireLibrary() {
    static std::mutex mutex;
    static std::weak_ptr<Library> sharedLibrary;
    std::lock_guard<std::mutex> lock(mutex);
    auto library = sharedLibrary.lock();
    if (!library) {
      library = std::make_shared<Library>();
      initialize(*library);
      sharedLibrary = library;
    }
    // A failed load is also retained by the contexts. Do not probe the
    // filesystem and repeat an identical error on every rendered frame.
    return library;
  }
}

namespace dxvk {

  static bool isNativeNrdClear(const nrd::PipelineDesc& pipeline) {
    const std::string_view name = pipeline.shaderFileName ? pipeline.shaderFileName : "";
    // These NRD passes only store zero. Their SPIR-V declares RGBA32 images,
    // which cannot legally clear the R8/R16/RGBA16 textures in NRD's pools.
    // A transfer clear encodes zero using the actual image format instead.
    return (name == "Clear_Float.cs" || name == "Clear_Uint.cs")
      && !pipeline.hasConstantData
      && pipeline.resourceRangesNum == 1
      && pipeline.resourceRanges[0].descriptorType == nrd::DescriptorType::STORAGE_TEXTURE
      && pipeline.resourceRanges[0].descriptorsNum == 1;
  }
  static void* NrdAllocate(void* userArg, size_t size, size_t alignment) {
    return _aligned_malloc(size, alignment);
  }

  static void* NrdReallocate(void* userArg, void* memory, size_t size, size_t alignment) {
    return _aligned_realloc(memory, size, alignment);
  }

  static void NrdFree(void* userArg, void* memory) {
    _aligned_free(memory);
  }

  static VkFormat TranslateFormat(nrd::Format format) {
    switch (format) {
    case nrd::Format::R16_UINT:
      return VK_FORMAT_R16_UINT;
    case nrd::Format::R16_UNORM:
      return VK_FORMAT_R16_UNORM;
    case nrd::Format::R32_SFLOAT:
      return VK_FORMAT_R32_SFLOAT;
    case nrd::Format::R16_SFLOAT:
      return VK_FORMAT_R16_SFLOAT;
    case nrd::Format::RG16_SFLOAT:
      return VK_FORMAT_R16G16_SFLOAT;
    case nrd::Format::RG32_SFLOAT:
      return VK_FORMAT_R32G32_SFLOAT;
    case nrd::Format::R8_UNORM:
      return VK_FORMAT_R8_UNORM;
    case nrd::Format::RG8_UNORM:
      return VK_FORMAT_R8G8_UNORM;
    case nrd::Format::RGBA8_UNORM:
      return VK_FORMAT_R8G8B8A8_UNORM;
    case nrd::Format::RGBA16_SFLOAT:
      return VK_FORMAT_R16G16B16A16_SFLOAT;
    case nrd::Format::RGBA32_SFLOAT:
      return VK_FORMAT_R32G32B32A32_SFLOAT;
    case nrd::Format::RGBA32_UINT:
      return VK_FORMAT_R32G32B32A32_UINT;
    case nrd::Format::R32_UINT:
      return VK_FORMAT_R32_UINT;
    case nrd::Format::RG32_UINT:
      return VK_FORMAT_R32G32_UINT;
    case nrd::Format::R11_G11_B10_UFLOAT:
      return VK_FORMAT_B10G11R11_UFLOAT_PACK32;
    case nrd::Format::R10_G10_B10_A2_UNORM:
      return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    case nrd::Format::R10_G10_B10_A2_UINT:
      return VK_FORMAT_A2B10G10R10_UINT_PACK32;
    default:
      assert(!"Unknown/Unsupported format.");
      return VK_FORMAT_UNDEFINED;
    }
  }

  NRDContext::NRDContext(DxvkDevice* device, DenoiserType type)
    : CommonDeviceObject(device), m_vkd(device->vkd()), m_type(type) {
    // Settings and ray-tracing arguments are needed even when this signal
    // never runs NRD. Loading the DLL and creating an SDK instance are deferred
    // until the first dispatch; all active contexts share the same library.
    nrd::LibraryDesc libraryDesc = {};
    libraryDesc.versionMajor = NRD_VERSION_MAJOR;
    libraryDesc.versionMinor = NRD_VERSION_MINOR;
    libraryDesc.versionBuild = NRD_VERSION_BUILD;
    m_settings.initialize(libraryDesc, device->instance()->config(), type);
    
    // Disable the replace direct specular HitT with indirect specular HitT if we are using combined denoiser.
    // Because in combined denoiser the direct and indirect signals are denoised together,
    // in such case we will break the denoiser if replace the direct with indirect specular HitT.
    RtxOptions::replaceDirectSpecularHitTWithIndirectSpecularHitT.setDeferred(RtxOptions::denoiseDirectAndIndirectLightingSeparately());
  }

  NRDContext::~NRDContext() {
    release();
  }

  void NRDContext::onDestroy() {
    release();
  }

  const char* NRDContext::getDenoiserName() const {
    switch (m_type) {
    case DenoiserType::DirectAndIndirectLight: return "Direct And Indirect Light";
    case DenoiserType::DirectLight: return "Direct Light";
    case DenoiserType::IndirectLight: return "Indirect Light";
    case DenoiserType::Secondaries: return "Secondaries";
    case DenoiserType::Reference: return "Reference";
    default: assert(0); return "<invalid argument>";  break;
    }
  }

  static DxvkSamplerCreateInfo getSamplerInfo(const nrd::Sampler& nrdSampler);

  void NRDContext::prepareResources(
    Rc<DxvkContext> ctx,
    const Resources::RaytracingOutput& rtOutput) {

    if (m_type != DenoiserType::Reference)
      m_settings.updateDenoiserMode();

    const auto extent = rtOutput.m_compositeOutputExtent;
    if (!extent.width || !extent.height || extent.width > UINT16_MAX || extent.height > UINT16_MAX)
      throw DxvkError("NRD render extent is outside the SDK's supported range");

    // An NRD instance describes one algorithm, independently of resolution.
    // Recreate it on algorithm changes, but retain its pipelines on a resize.
    if (m_denoiser != m_settings.m_denoiserDesc.denoiser || !m_denoiserInstance) {
      release();
      m_denoiser = m_settings.m_denoiserDesc.denoiser;
      // Identifiers are local to an SDK instance, which contains one denoiser.
      m_settings.m_denoiserDesc.identifier = 0;
      nrd::InstanceCreationDesc instanceCreationDesc = {};
      instanceCreationDesc.allocationCallbacks.Allocate = NrdAllocate;
      instanceCreationDesc.allocationCallbacks.Reallocate = NrdReallocate;
      instanceCreationDesc.allocationCallbacks.Free = NrdFree;
      instanceCreationDesc.denoisersNum = 1;
      instanceCreationDesc.denoisers = &m_settings.m_denoiserDesc;
      try {
        THROW_IF_FALSE(m_library->dispatch.CreateInstance(instanceCreationDesc, m_denoiserInstance) == nrd::Result::SUCCESS);
        const auto& instanceDesc = m_library->dispatch.GetInstanceDesc(*m_denoiserInstance);
        m_computePipelines.resize(instanceDesc.pipelinesNum);
        for (uint32_t i = 0; i < instanceDesc.samplersNum; ++i)
          m_staticSamplers.emplace_back(device()->createSampler(getSamplerInfo(instanceDesc.samplers[i])));
      } catch (...) {
        release();
        throw;
      }
      Logger::debug(str::format("[RTX] NRD: created ", getDenoiserName(), " instance (",
        m_library->dispatch.GetDenoiserString(m_denoiser), "); pipelines compile on first use"));
    }

    if (m_resourceExtent.width != extent.width || m_resourceExtent.height != extent.height) {
      destroyResources();
      const auto width = static_cast<uint16_t>(extent.width);
      const auto height = static_cast<uint16_t>(extent.height);
      m_settings.m_commonSettings.resourceSizePrev[0] = width;
      m_settings.m_commonSettings.resourceSizePrev[1] = height;
      m_settings.m_commonSettings.resourceSize[0] = width;
      m_settings.m_commonSettings.resourceSize[1] = height;
      m_settings.m_commonSettings.rectSizePrev[0] = width;
      m_settings.m_commonSettings.rectSizePrev[1] = height;
      m_settings.m_commonSettings.rectSize[0] = width;
      m_settings.m_commonSettings.rectSize[1] = height;
      createResources(ctx, rtOutput);
      m_resourceExtent = extent;
      m_settings.m_resetHistory = true;
    }

    if (!m_cbData) {
      m_cbData = std::make_unique<RtxStagingDataAlloc>(
        device(), "RtxStagingDataAlloc: NRD CB",
        (VkMemoryPropertyFlagBits) (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT),
        VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    }

    if (m_settings.m_commonSettings.enableValidation && !m_validationTex.isValid()) {
      m_validationTex = Resources::createImageResource(ctx, "nrd validation texture", rtOutput.m_compositeOutputExtent, VK_FORMAT_R32G32B32A32_SFLOAT);
    } else if (!m_settings.m_commonSettings.enableValidation) {
      m_validationTex.reset();
    }
  }

  static DxvkSamplerCreateInfo getSamplerInfo(const nrd::Sampler& nrdSampler) {

    DxvkSamplerCreateInfo samplerInfo;

    if (nrdSampler == nrd::Sampler::NEAREST_CLAMP) {
      samplerInfo.magFilter = VK_FILTER_NEAREST;
      samplerInfo.minFilter = VK_FILTER_NEAREST;
      samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    } else {
      samplerInfo.magFilter = VK_FILTER_LINEAR;
      samplerInfo.minFilter = VK_FILTER_LINEAR;
      samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    }

    samplerInfo.mipmapLodBias = 0.0f;
    samplerInfo.mipmapLodMin = 0.0f;
    samplerInfo.mipmapLodMax = FLT_MAX;
    samplerInfo.useAnisotropy = VK_FALSE;
    samplerInfo.maxAnisotropy = 1.0f;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;

    if (nrdSampler == nrd::Sampler::NEAREST_CLAMP || nrdSampler == nrd::Sampler::LINEAR_CLAMP) {
      samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
      samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
      samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    } else {
      samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
      samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
      samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    }

    samplerInfo.compareToDepth = VK_FALSE;
    samplerInfo.compareOp = VK_COMPARE_OP_ALWAYS;
    samplerInfo.borderColor = { 0.f, 0.f, 0.f, 1.f }; // Opaque black
    samplerInfo.usePixelCoord = VK_FALSE;

    return samplerInfo;
  }

  void NRDContext::createResources(
    Rc<DxvkContext> ctx,
    const Resources::RaytracingOutput& rtOutput) {
    const nrd::InstanceDesc& instanceDesc = m_library->dispatch.GetInstanceDesc(*m_denoiserInstance);

    DxvkImageCreateInfo desc;
    desc.type = VK_IMAGE_TYPE_2D;
    desc.flags = 0;
    desc.sampleCount = VK_SAMPLE_COUNT_1_BIT;
    desc.numLayers = 1;
    desc.mipLevels = 1;
    // VK_IMAGE_USAGE_TRANSFER_DST_BIT needed for clears in NRD
    desc.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    desc.stages = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    desc.access = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    desc.tiling = VK_IMAGE_TILING_OPTIMAL;
    desc.layout = VK_IMAGE_LAYOUT_UNDEFINED;

    DxvkImageViewCreateInfo viewInfo;
    viewInfo.type = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
    viewInfo.aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.minLevel = 0;
    viewInfo.minLayer = 0;
    viewInfo.numLayers = 1;
    viewInfo.numLevels = 1;

    const uint32_t textureCount = instanceDesc.permanentPoolSize + instanceDesc.transientPoolSize;

    std::lock_guard<std::mutex> poolLock(m_sharedTransientMutex);
    auto& sharedPool = m_sharedTransientTex[device()];
    sharedPool.erase(std::remove_if(sharedPool.begin(), sharedPool.end(),
      [](const auto& resource) { return resource.expired(); }), sharedPool.end());
    // Each pool slot is used at most once by this context. Different slots of
    // the same dimensions/format must not alias within a denoiser dispatch.
    SharedTransientPool available = sharedPool;
    uint32_t reusedTransientCount = 0;

    for (uint32_t i = 0; i < textureCount; i++) {

      const bool isPermanent = (i < instanceDesc.permanentPoolSize);

      const nrd::TextureDesc& nrdTextureDesc = isPermanent
        ? instanceDesc.permanentPool[i]
        : instanceDesc.transientPool[i - instanceDesc.permanentPoolSize];

      viewInfo.format = desc.format = TranslateFormat(nrdTextureDesc.format);
      VkFormatProperties3 formatFeatures = { VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3 };
      VkFormatProperties2 formatProperties = { VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2, &formatFeatures };
      const auto adapter = device()->adapter();
      adapter->vki()->vkGetPhysicalDeviceFormatProperties2(adapter->handle(), desc.format, &formatProperties);
      constexpr VkFormatFeatureFlags2 required = VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_BIT
        | VK_FORMAT_FEATURE_2_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_2_STORAGE_WRITE_WITHOUT_FORMAT_BIT;
      if ((formatFeatures.optimalTilingFeatures & required) != required)
        throw DxvkError(str::format("NRD pool format ", desc.format,
          " lacks sampled/storage/formatless-write support"));
      
      desc.extent = { 
        util::ceilDivide(m_settings.m_commonSettings.resourceSize[0], nrdTextureDesc.downsampleFactor),
        util::ceilDivide(m_settings.m_commonSettings.resourceSize[1], nrdTextureDesc.downsampleFactor),
        1 };

      if (isPermanent) {
        // Always allocate these
        Resources::Resource resource;
        resource.image = device()->createImage(desc, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, DxvkMemoryStats::Category::RTXRenderTarget, "nrd permament tex");
        resource.view = device()->createImageView(resource.image, viewInfo);

        ctx->changeImageLayout(resource.image, VK_IMAGE_LAYOUT_GENERAL);
        m_permanentTex.emplace_back(std::move(resource));
      }
      else {
        std::shared_ptr<Resource> shared;
        for (auto it = available.begin(); it != available.end(); ++it) {
          auto candidate = it->lock();
          if (!candidate)
            continue;
          // All other image/view properties are fixed for NRD pools above.
          const auto& candidateInfo = candidate->image->info();
          if (candidateInfo.format == desc.format && candidateInfo.extent.width == desc.extent.width
              && candidateInfo.extent.height == desc.extent.height) {
            shared = std::move(candidate);
            available.erase(it);
            break;
          }
        }
        if (shared) {
          m_transientTex.emplace_back(std::move(shared));
          ++reusedTransientCount;
        } else {
          Resources::Resource resource;
          resource.image = device()->createImage(desc, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, DxvkMemoryStats::Category::RTXRenderTarget, "nrd transient tex");
          resource.view = device()->createImageView(resource.image, viewInfo);

          ctx->changeImageLayout(resource.image, VK_IMAGE_LAYOUT_GENERAL);

          // Create in this instance
          m_transientTex.emplace_back(std::make_shared<Resource>(resource));

          // NOTE: Insert into the main pool (not copy)
          sharedPool.emplace_back(m_transientTex.back());
        }
      }
    }
    Logger::debug(str::format("[RTX] NRD: ", getDenoiserName(), " resources ",
      m_settings.m_commonSettings.resourceSize[0], "x", m_settings.m_commonSettings.resourceSize[1],
      ", permanent=", m_permanentTex.size(), ", transient=", m_transientTex.size(),
      ", reusedTransient=", reusedTransientCount));
  }

  Resources::Resource NRDContext::getValidationTexture() const {
    return m_validationTex;
  }

  NRDContext::ComputePipeline::~ComputePipeline() {
    vkd->vkDestroyPipeline(vkd->device(), pipeline, nullptr);
    vkd->vkDestroyPipelineLayout(vkd->device(), pipelineLayout, nullptr);
    vkd->vkDestroyDescriptorSetLayout(vkd->device(), descriptorSetLayout, nullptr);
  }

  void NRDContext::createPipeline(uint32_t index) {

    const nrd::InstanceDesc& instanceDesc = m_library->dispatch.GetInstanceDesc(*m_denoiserInstance);

    const nrd::SPIRVBindingOffsets spirvOffsets = m_library->dispatch.GetLibraryDesc().spirvBindingOffsets;

    // Create static sampler binding infos
    std::vector<VkDescriptorSetLayoutBinding> samplersBindInfo;
    {
      samplersBindInfo.resize(instanceDesc.samplersNum);

      for (uint32_t i = 0; i < instanceDesc.samplersNum; i++) {

        // Bind info
        const uint32_t reg = static_cast<uint32_t>(instanceDesc.samplers[i]);
        samplersBindInfo[i].binding = spirvOffsets.samplerOffset + reg;
        samplersBindInfo[i].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
        samplersBindInfo[i].descriptorCount = 1;
        samplersBindInfo[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        samplersBindInfo[i].pImmutableSamplers = nullptr;
      }
    }

    // Only compile permutations actually selected by GetComputeDispatches.
    // ReLAX/ReBLUR list many variants that a given configuration never uses.
    {

      const nrd::PipelineDesc& nrdPipelineDesc = instanceDesc.pipelines[index];
      const nrd::ComputeShaderDesc& nrdComputeShader = nrdPipelineDesc.computeShaderSPIRV;

      if (isNativeNrdClear(nrdPipelineDesc)) {
        // Keep NRD's pipeline indices stable; native clears need no pipeline
        // or descriptor allocation.
        return;
      }

      // Start with static samplers bind infos
      std::vector<VkDescriptorSetLayoutBinding> bindInfo(samplersBindInfo.begin(), samplersBindInfo.end());
      uint32_t cbBindInfoIndex = ComputePipeline::kInvalidIndex;
      uint32_t resourcesStartIndex = ComputePipeline::kInvalidIndex;
      {
        // Constant Buffer
        if (nrdPipelineDesc.hasConstantData)
        {
          VkDescriptorSetLayoutBinding binding{};
          binding.binding = spirvOffsets.constantBufferOffset + instanceDesc.constantBufferRegisterIndex;
          binding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
          binding.descriptorCount = 1;
          binding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
          binding.pImmutableSamplers = nullptr;

          cbBindInfoIndex = (uint32_t)bindInfo.size();
          bindInfo.emplace_back(std::move(binding));
        }

        // Textures
        resourcesStartIndex = (uint32_t)bindInfo.size();
        for (uint32_t j = 0; j < nrdPipelineDesc.resourceRangesNum; j++) {

          const nrd::ResourceRangeDesc& nrdDescriptorRange = nrdPipelineDesc.resourceRanges[j];

          const bool isSRV = nrdDescriptorRange.descriptorType == nrd::DescriptorType::TEXTURE;
          VkDescriptorType descType = isSRV ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
          uint32_t vkBaseOffset = isSRV ? spirvOffsets.textureOffset : spirvOffsets.storageTextureAndBufferOffset;

          assert(nrdDescriptorRange.baseRegisterIndex == 0);
          for (uint32_t k = 0; k < nrdDescriptorRange.descriptorsNum; k++) {

            VkDescriptorSetLayoutBinding binding{};
            binding.binding = vkBaseOffset + k;
            binding.descriptorType = descType;
            binding.descriptorCount = 1;
            binding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
            binding.pImmutableSamplers = nullptr;

            bindInfo.emplace_back(std::move(binding));
          }
        }
      }

      // Create descriptor set layout   
      Rc<ComputePipeline> computePipeline = new ComputePipeline(m_vkd);
      {
        VkDescriptorSetLayoutCreateInfo dsetInfo;
        dsetInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        dsetInfo.pNext = nullptr;
        dsetInfo.flags = 0;
        dsetInfo.bindingCount = (uint32_t)bindInfo.size();
        dsetInfo.pBindings = bindInfo.data();

        VK_THROW_IF_FAILED(m_vkd->vkCreateDescriptorSetLayout(m_vkd->device(), &dsetInfo, nullptr, &computePipeline->descriptorSetLayout));
      }

      // Create pipeline
      computePipeline->pipelineLayout = createPipelineLayout(computePipeline->descriptorSetLayout);
      computePipeline->pipeline = createPipeline(nrdComputeShader, nrdPipelineDesc, computePipeline->pipelineLayout);
      computePipeline->constantBufferIndex = cbBindInfoIndex;
      computePipeline->resourcesStartIndex = resourcesStartIndex;
      computePipeline->bindings = std::move(bindInfo);
      m_computePipelines[index] = std::move(computePipeline);
      ++m_compiledPipelineCount;
    }
  }

  VkPipelineLayout NRDContext::createPipelineLayout(VkDescriptorSetLayout dsetLayout) {

    VkPipelineLayoutCreateInfo pipeInfo;
    pipeInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipeInfo.pNext = nullptr;
    pipeInfo.flags = 0;
    pipeInfo.setLayoutCount = 1;
    pipeInfo.pSetLayouts = &dsetLayout;
    pipeInfo.pushConstantRangeCount = 0;
    pipeInfo.pPushConstantRanges = nullptr;

    VkPipelineLayout result = VK_NULL_HANDLE;
    VK_THROW_IF_FAILED(m_vkd->vkCreatePipelineLayout(m_vkd->device(), &pipeInfo, nullptr, &result));

    return result;
  }

  VkPipeline NRDContext::createPipeline(const nrd::ComputeShaderDesc& nrdCS,
    const nrd::PipelineDesc& nrdPipelineDesc,
    const VkPipelineLayout& pipelineLayout) {

    // The packaged SDK infers RGBA32/R32 storage formats from HLSL float
    // declarations, while its dispatches bind half/normalized pool textures.
    // Apply DXC's formatless-storage semantics without changing pool precision.
    const NrdStorageImageCode shaderCode = normalizeNrdStorageImageFormats(
      nrdCS.bytecode, static_cast<size_t>(nrdCS.size));
    const auto& features = device()->features().core.features;
    if ((shaderCode.requiresRead && !features.shaderStorageImageReadWithoutFormat)
        || (shaderCode.requiresWrite && !features.shaderStorageImageWriteWithoutFormat))
      throw DxvkError(str::format("NRD pipeline ", nrdPipelineDesc.shaderFileName,
        " requires unsupported formatless storage-image access"));

    VkShaderModuleCreateInfo shaderInfo;
    shaderInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    shaderInfo.pNext = nullptr;
    shaderInfo.flags = 0;
    shaderInfo.codeSize = shaderCode.code.size() * sizeof(uint32_t);
    shaderInfo.pCode = shaderCode.code.data();

    VkShaderModule shaderModule = VK_NULL_HANDLE;
    VK_THROW_IF_FAILED(m_vkd->vkCreateShaderModule(m_vkd->device(), &shaderInfo, nullptr, &shaderModule));

    VkPipelineShaderStageCreateInfo stageInfo;
    stageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stageInfo.pNext = nullptr;
    stageInfo.flags = 0;
    stageInfo.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    stageInfo.module = shaderModule;
    stageInfo.pName = "main";
    stageInfo.pSpecializationInfo = nullptr;

    VkComputePipelineCreateInfo pipeInfo;
    pipeInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipeInfo.pNext = nullptr;
    pipeInfo.flags = 0;
    pipeInfo.stage = stageInfo;
    pipeInfo.layout = pipelineLayout;
    pipeInfo.basePipelineHandle = VK_NULL_HANDLE;
    pipeInfo.basePipelineIndex = -1;

    VkPipeline result = VK_NULL_HANDLE;
    const VkResult status = m_vkd->vkCreateComputePipelines(m_vkd->device(),
      device()->getCommon()->pipelineManager().pipelineCache(), 1, &pipeInfo, nullptr, &result);

    m_vkd->vkDestroyShaderModule(m_vkd->device(), shaderModule, nullptr);

    if (status != VK_SUCCESS) {
      if (result)
        m_vkd->vkDestroyPipeline(m_vkd->device(), result, nullptr);
      throw DxvkError(str::format("NRD: Failed to create pipeline ", nrdPipelineDesc.shaderFileName,
        " (Vulkan result ", status, ")"));
    }

    return result;
  }

  const Resources::Resource* NRDContext::getTexture(const nrd::ResourceDesc& resource, const DxvkDenoise::Input& inputs, const DxvkDenoise::Output& outputs) {

    switch (resource.type)
    {
    case nrd::ResourceType::IN_MV:
      return inputs.motionVector;
    case nrd::ResourceType::IN_NORMAL_ROUGHNESS:
      return inputs.normal_roughness;
    case nrd::ResourceType::IN_VIEWZ:
      return inputs.linearViewZ;
    case nrd::ResourceType::IN_DIFF_CONFIDENCE:
      return inputs.confidence;
    case nrd::ResourceType::IN_SPEC_CONFIDENCE:
      return inputs.confidence;
    case nrd::ResourceType::IN_DISOCCLUSION_THRESHOLD_MIX:
      return inputs.disocclusionThresholdMix;
    case nrd::ResourceType::IN_DIFF_RADIANCE_HITDIST:
      return inputs.diffuse_hitT;
    case nrd::ResourceType::IN_SPEC_RADIANCE_HITDIST:
      return inputs.specular_hitT;
    case nrd::ResourceType::IN_SIGNAL:
      return inputs.reference;
    case nrd::ResourceType::OUT_DIFF_RADIANCE_HITDIST:
      return outputs.diffuse_hitT;
    case nrd::ResourceType::OUT_SPEC_RADIANCE_HITDIST:
      return outputs.specular_hitT;
    case nrd::ResourceType::OUT_SIGNAL:
      return outputs.reference;
    case nrd::ResourceType::TRANSIENT_POOL:
      assert(resource.indexInPool < m_transientTex.size());
      return m_transientTex[resource.indexInPool].get();
    case nrd::ResourceType::PERMANENT_POOL:
      assert(resource.indexInPool < m_permanentTex.size());
      return &m_permanentTex[resource.indexInPool];
    case nrd::ResourceType::OUT_VALIDATION:
      return &m_validationTex;
    default:
      throw DxvkError("Unavailable resource type");
    }
    return nullptr;
  }

  void NRDContext::dispatch(
    Rc<DxvkContext> ctx,
    DxvkBarrierSet& barriers,
    const SceneManager& sceneManager,
    const Resources::RaytracingOutput& rtOutput,
    const DxvkDenoise::Input& inputs,
    const DxvkDenoise::Output& outputs) {
    if (!m_library)
      m_library = nrd::acquireLibrary();
    if (!m_library->module) {
      return;
    }
    m_settings.m_libraryDesc = m_library->dispatch.GetLibraryDesc();

    const uint32_t frameId = device()->getCurrentFrameId();
    // Only reset on an explicit reset or the first dispatch. Resetting on any
    // non-consecutive frame restarted accumulation after every skipped frame,
    // leaving output permanently unconverged; NRD's own disocclusion handling
    // rejects history that no longer matches.
    m_settings.m_resetHistory |= inputs.reset || m_lastDispatchFrame == UINT32_MAX;

    ScopedGpuProfileZone(ctx, "NRD");
    static_cast<RtxContext*>(ctx.ptr())->setFramePassStage(RtxFramePassStage::NRD);

    prepareResources(ctx, rtOutput);

    updateNRDSettings(sceneManager, inputs, rtOutput);

    std::vector<Rc<DxvkImageView>> pInputs, pOutputs;
    if (m_settings.m_denoiserDesc.denoiser == nrd::Denoiser::REFERENCE) {
      pInputs = { inputs.reference->view, inputs.normal_roughness->view, inputs.linearViewZ->view, inputs.motionVector->view };
      pOutputs = { outputs.reference->view };
    } else {
      pInputs = { inputs.diffuse_hitT->view, inputs.specular_hitT->view, inputs.normal_roughness->view, inputs.linearViewZ->view, inputs.motionVector->view };
      pOutputs = { outputs.diffuse_hitT->view, outputs.specular_hitT->view };
    }

    for (auto input : pInputs) {
      barriers.accessImage(
        input->image(), input->imageSubresources(),
        input->imageInfo().layout, input->imageInfo().stages, input->imageInfo().access,
        input->imageInfo().layout, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
    }

    for (auto output : pOutputs) {
      barriers.accessImage(
        output->image(), output->imageSubresources(),
        output->imageInfo().layout, output->imageInfo().stages, output->imageInfo().access,
        output->imageInfo().layout, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT);
    }
    barriers.recordCommands(ctx->getCommandList());

    auto needsCustomView = [&](const Rc<DxvkImageView>& view, bool bStorage) {

      VkImageUsageFlags usage = view->info().usage;
      bool usageMatches = 
        bStorage 
        ? (usage & VK_IMAGE_USAGE_STORAGE_BIT && usage & VK_IMAGE_USAGE_SAMPLED_BIT)
        : (usage & VK_IMAGE_USAGE_SAMPLED_BIT);
      return !usageMatches;
    };

    auto createImageViewCreateInfo = [&](DxvkImage& image, bool bStorage) {

      DxvkImageViewCreateInfo viewInfo = {};
      viewInfo.type = VK_IMAGE_VIEW_TYPE_2D;
      viewInfo.format = image.info().format;
      viewInfo.usage = bStorage ? VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT : VK_IMAGE_USAGE_SAMPLED_BIT;
      viewInfo.aspect = VK_IMAGE_ASPECT_COLOR_BIT;
      viewInfo.minLevel = 0;
      viewInfo.numLevels = 1;
      viewInfo.minLayer = 0;
      viewInfo.numLayers = 1;

      return viewInfo;
    };

    // Prepare and run dispatches
    const nrd::InstanceDesc& instanceDesc = m_library->dispatch.GetInstanceDesc(*m_denoiserInstance);
    {
      uint32_t dispatchDescNum = 0;
      const nrd::DispatchDesc* dispatchDescs = nullptr;

      THROW_IF_FALSE(m_library->dispatch.GetComputeDispatches(*m_denoiserInstance,
        &m_settings.m_denoiserDesc.identifier, 1, dispatchDescs, dispatchDescNum) == nrd::Result::SUCCESS);

      for (uint32_t i = 0; i < dispatchDescNum; i++) {

        const nrd::DispatchDesc& dispatchDesc = dispatchDescs[i];
        if (dispatchDesc.pipelineIndex >= m_computePipelines.size())
          throw DxvkError("NRD dispatch references an invalid pipeline index");
        const nrd::PipelineDesc& pipelineDesc = instanceDesc.pipelines[dispatchDesc.pipelineIndex];

        ScopedGpuProfileZoneDynamicZ(ctx, dispatchDesc.name);

        if (isNativeNrdClear(pipelineDesc)) {
          if (dispatchDesc.resourcesNum != 1 || dispatchDesc.constantBufferDataSize != 0
              || dispatchDesc.resources[0].descriptorType != nrd::DescriptorType::STORAGE_TEXTURE)
            throw DxvkError("NRD clear dispatch has an unexpected resource signature");

          const Resources::Resource* texture = getTexture(dispatchDesc.resources[0], inputs, outputs);
          const auto& image = texture->image;
          const auto& info = image->info();
          if (!(info.usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT))
            throw DxvkError("NRD clear destination is missing transfer usage");

          const VkImageSubresourceRange subresources = texture->view->imageSubresources();
          const VkImageLayout clearLayout = image->pickLayout(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
          const VkPipelineStageFlags shaderStages = info.stages | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
          const VkAccessFlags shaderAccess = info.access | VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
          barriers.accessImage(image, subresources,
            info.layout, shaderStages, shaderAccess,
            clearLayout, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
          barriers.recordCommands(ctx->getCommandList());

          const VkClearColorValue zero = {};
          ctx->getCommandList()->cmdClearColorImage(image->handle(), clearLayout, &zero, 1, &subresources);
          ctx->getCommandList()->trackResource<DxvkAccess::Write>(image);

          barriers.accessImage(image, subresources,
            clearLayout, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
            info.layout, shaderStages, shaderAccess);
          barriers.recordCommands(ctx->getCommandList());
          continue;
        }

        if (m_computePipelines[dispatchDesc.pipelineIndex] == nullptr)
          createPipeline(dispatchDesc.pipelineIndex);
        const auto& pipelineResource = m_computePipelines[dispatchDesc.pipelineIndex];
        const ComputePipeline& computePipeline = *pipelineResource;
        ctx->getCommandList()->trackResource<DxvkAccess::None>(pipelineResource);
        VkDescriptorSet descriptorSet = ctx->allocateDescriptorSet(computePipeline.descriptorSetLayout, "NRD descriptor set");

        auto& descriptorWriteSets = m_descriptorWrites;
        descriptorWriteSets.clear();
        descriptorWriteSets.reserve(instanceDesc.samplersNum + dispatchDesc.resourcesNum + 1);

        // Variables referenced inside descriptorWriteSets must have the same lifetime, so preallocate
        auto& samplerDescs = m_samplerDescriptors;
        samplerDescs.resize(instanceDesc.samplersNum);
        VkDescriptorBufferInfo             cbDesc{};
        auto& imageDesc = m_resourceDescriptors;
        imageDesc.resize(dispatchDesc.resourcesNum);

        // Static sampler descriptors
        for (size_t i = 0; i < instanceDesc.samplersNum; i++) {

          const VkDescriptorSetLayoutBinding& binding = computePipeline.bindings[i];
          samplerDescs[i].sampler = m_staticSamplers[i]->handle();
          samplerDescs[i].imageView = VK_NULL_HANDLE;
          samplerDescs[i].imageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
          descriptorWriteSets.emplace_back(DxvkDescriptor::texture(descriptorSet, &samplerDescs[i], binding.descriptorType, binding.binding));
          
          ctx->getCommandList()->trackResource<DxvkAccess::None>(m_staticSamplers[i]);
        }

        // Update constants
        // The ReLAX A-trous passes use the same shader pipeline with different constant values.
        // In this case, the default constant buffer cannot guarantee values got updated in each pass.
        // Use RtxStagingDataAlloc to fix this issue.
        if (dispatchDesc.constantBufferDataSize > 0) {
          // Setting alignment to device limit minUniformBufferOffsetAlignment because the offset value should be its multiple.
          // See https://vulkan.lunarg.com/doc/view/1.2.189.2/windows/1.2-extensions/vkspec.html#VUID-VkWriteDescriptorSet-descriptorType-00327
          const auto& devInfo = device()->properties().core.properties;
          VkDeviceSize alignment = devInfo.limits.minUniformBufferOffsetAlignment;
          DxvkBufferSlice cbSlice = m_cbData->alloc(alignment, dispatchDesc.constantBufferDataSize);
          ctx->getCommandList()->trackResource<DxvkAccess::Write>(cbSlice.buffer());
          memcpy(cbSlice.mapPtr(0), dispatchDesc.constantBufferData, dispatchDesc.constantBufferDataSize);

          const VkDescriptorSetLayoutBinding& cb = computePipeline.bindings[computePipeline.constantBufferIndex];
          assert(cb.descriptorCount == 1);

          cbDesc = cbSlice.getDescriptor().buffer;
          descriptorWriteSets.emplace_back(DxvkDescriptor::buffer(descriptorSet, &cbDesc, cb.descriptorType, cb.binding));

          barriers.accessBuffer(cbSlice.getSliceHandle(),
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_ACCESS_TRANSFER_WRITE_BIT,
            cbSlice.buffer()->info().stages,
            cbSlice.buffer()->info().access);
        }

        // Gather needed resource infos for the pipeline
        for (size_t i = 0; i < dispatchDesc.resourcesNum; i++) {

          const VkDescriptorSetLayoutBinding& binding = computePipeline.bindings[computePipeline.resourcesStartIndex + i];
          assert(binding.descriptorCount == 1);

          const nrd::ResourceDesc& resource = dispatchDesc.resources[i];

          const Resources::Resource* texture = getTexture(resource, inputs, outputs);

          const bool bStorage = resource.descriptorType == nrd::DescriptorType::STORAGE_TEXTURE;

          Rc<DxvkImageView> imageView;

          if (needsCustomView(texture->view, bStorage)) {
            DxvkImageViewCreateInfo viewCreateInfo = createImageViewCreateInfo(*texture->image.ptr(), bStorage);
            imageView = device()->createImageView(texture->image, viewCreateInfo);
          } else {
            imageView = texture->view;
          }

          // Ensure resources are kept alive
          ctx->getCommandList()->trackResource<DxvkAccess::None>(imageView);
          if (bStorage) {
            ctx->getCommandList()->trackResource<DxvkAccess::Write>(texture->image);
          } else {
            ctx->getCommandList()->trackResource<DxvkAccess::Read>(texture->image);
          }

          descriptorWriteSets.emplace_back(DxvkDescriptor::texture(descriptorSet, &imageDesc[i], *imageView, binding.descriptorType, binding.binding));

          // Create a barrier
          barriers.accessImage(
            texture->image, imageView->imageSubresources(),
            imageView->imageInfo().layout, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, imageView->imageInfo().access,
            imageView->imageInfo().layout, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            bStorage ? VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT : VK_ACCESS_SHADER_READ_BIT);

#ifdef REMIX_DEVELOPMENT
          // Cache NRD image views
          static_cast<RtxContext*>(ctx.ptr())->cacheResourceAliasingImageView(imageView);
#endif
        }

        barriers.recordCommands(ctx->getCommandList());

        ctx->getCommandList()->updateDescriptorSets(descriptorWriteSets.size(), descriptorWriteSets.data());

        ctx->getCommandList()->cmdBindPipeline(VK_PIPELINE_BIND_POINT_COMPUTE, computePipeline.pipeline);
        ctx->getCommandList()->cmdBindDescriptorSet(VK_PIPELINE_BIND_POINT_COMPUTE, computePipeline.pipelineLayout, descriptorSet, 0, nullptr);

        ctx->getCommandList()->cmdDispatch(dispatchDesc.gridWidth, dispatchDesc.gridHeight, 1);

        for (auto output : pOutputs) {
          ctx->getCommandList()->trackResource<DxvkAccess::None>(output);
          ctx->getCommandList()->trackResource<DxvkAccess::Write>(output->image());
        }
      }
    }

    // Transition external resources back
    {
      for (auto input : pInputs) {
        barriers.accessImage(
          input->image(), input->imageSubresources(),
          input->imageInfo().layout, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT,
          input->imageInfo().layout, input->imageInfo().stages, input->imageInfo().access);
      }

      for (auto output : pOutputs) {
        barriers.accessImage(
          output->image(), output->imageSubresources(),
          output->imageInfo().layout, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
          output->imageInfo().layout, output->imageInfo().stages, output->imageInfo().access);
      }
    }

    m_settings.m_resetHistory = false;
    m_lastDispatchFrame = frameId;
  }

  void NRDContext::updateNRDSettings(
    const SceneManager& sceneManager,
    const DxvkDenoise::Input& inputs,
    const Resources::RaytracingOutput& rtOutput) {

    if (m_settings.m_denoiserDesc.denoiser != nrd::Denoiser::REFERENCE) {
      // Don't allow adaptive scaling for direct light in ReBlur
      if (m_settings.m_denoiserDesc.denoiser != nrd::Denoiser::REBLUR_DIFFUSE_SPECULAR || m_settings.m_type != dxvk::DenoiserType::DirectLight) {
        updateAdaptiveScaling(inputs.diffuse_hitT->image->info().extent);
      }

      if (RtxOptions::adaptiveAccumulation()) {
        m_settings.updateAdaptiveAccumulation(GlobalTime::get().deltaTimeMs());
      }
    }

    // nrd::SetCommonSettings
    nrd::CommonSettings& commonSettings = m_settings.m_commonSettings;
    {
      const auto& camera = sceneManager.getCamera();

      const uint16_t width = static_cast<uint16_t>(rtOutput.m_compositeOutputExtent.width);
      const uint16_t height = static_cast<uint16_t>(rtOutput.m_compositeOutputExtent.height);

      m_settings.m_commonSettings.resourceSizePrev[0] = width;
      m_settings.m_commonSettings.resourceSizePrev[1] = height;
      m_settings.m_commonSettings.resourceSize[0] = width;
      m_settings.m_commonSettings.resourceSize[1] = height;
      m_settings.m_commonSettings.rectSizePrev[0] = width;
      m_settings.m_commonSettings.rectSizePrev[1] = height;
      m_settings.m_commonSettings.rectSize[0] = width;
      m_settings.m_commonSettings.rectSize[1] = height;

      // Note: Convert camera matrices to Matrix4 for the sake of NRD (only accepts float matrices).
      const Matrix4 viewMatrix = camera.getWorldToView();
      const Matrix4 prevViewMatrix = camera.getPreviousWorldToView();
      const Matrix4 viewToProjectionMatrix = camera.getViewToProjection();
      const Matrix4 prevViewToProjectionMatrix = camera.getPreviousViewToProjection();

      // Check whether camera has changed
      if (m_settings.m_denoiserDesc.denoiser == nrd::Denoiser::REFERENCE &&
          (memcmp(commonSettings.worldToViewMatrix, viewMatrix.data, sizeof(Matrix4)) != 0 ||
           memcmp(commonSettings.viewToClipMatrix, viewToProjectionMatrix.data, sizeof(Matrix4)) != 0)) {
        m_settings.m_resetHistory = true;
      }

      // Pass non-jittered camera matrices
      memcpy(commonSettings.worldToViewMatrix, viewMatrix.data, sizeof(Matrix4));
      memcpy(commonSettings.worldToViewMatrixPrev, prevViewMatrix.data, sizeof(Matrix4));
      memcpy(commonSettings.viewToClipMatrix, viewToProjectionMatrix.data, sizeof(Matrix4));
      memcpy(commonSettings.viewToClipMatrixPrev, prevViewToProjectionMatrix.data, sizeof(Matrix4));

      // Note: Ensure matrix sizes are compatible (this could be done better with C++20's std::bit_cast rather than using
      // std::memcpy).
      static_assert(sizeof(commonSettings.worldToViewMatrix) == sizeof(viewMatrix));
      static_assert(sizeof(commonSettings.worldToViewMatrixPrev) == sizeof(prevViewMatrix));
      static_assert(sizeof(commonSettings.viewToClipMatrix) == sizeof(viewToProjectionMatrix));
      static_assert(sizeof(commonSettings.viewToClipMatrixPrev) == sizeof(prevViewToProjectionMatrix));

      float jitterVec[2];
      camera.getJittering(jitterVec);
      commonSettings.isMotionVectorInWorldSpace = true;
      commonSettings.motionVectorScale[0] = commonSettings.isMotionVectorInWorldSpace ? 1.0f : 1.0f / width;
      commonSettings.motionVectorScale[1] = commonSettings.isMotionVectorInWorldSpace ? 1.0f : 1.0f / height;
      commonSettings.motionVectorScale[2] = commonSettings.motionVectorScale[1]; // Enable 2.5D Motion Vector in NRD, we use the scale that matches previous default NRD scale on Z (mv = mv.xyz * mvScale.xyy)
      commonSettings.cameraJitterPrev[0] = commonSettings.cameraJitter[0];
      commonSettings.cameraJitterPrev[1] = commonSettings.cameraJitter[1];
      commonSettings.cameraJitter[0] = jitterVec[0] / static_cast<float>(width);
      commonSettings.cameraJitter[1] = jitterVec[1] / static_cast<float>(height);
      commonSettings.timeDeltaBetweenFrames = GlobalTime::get().deltaTimeMs();
      commonSettings.frameIndex = device()->getCurrentFrameId();
      commonSettings.accumulationMode = m_settings.m_resetHistory ? nrd::AccumulationMode::CLEAR_AND_RESTART : nrd::AccumulationMode::CONTINUE;

      auto* cameraTeleportDirectionInfo = sceneManager.getRayPortalManager().getCameraTeleportationRayPortalDirectionInfo();

      if (cameraTeleportDirectionInfo && RtxOptions::useVirtualShadingNormalsForDenoising()) {
        memcpy(commonSettings.worldPrevToWorldMatrix, &cameraTeleportDirectionInfo->portalToOpposingPortalDirection, sizeof(Matrix4));
      } else {
        static const auto identity = Matrix4{};
        memcpy(commonSettings.worldPrevToWorldMatrix, &identity, sizeof(Matrix4));
      }

      commonSettings.isHistoryConfidenceAvailable = inputs.confidence != nullptr;
      commonSettings.isDisocclusionThresholdMixAvailable = inputs.disocclusionThresholdMix != nullptr;

      THROW_IF_FALSE(m_library->dispatch.SetCommonSettings(*m_denoiserInstance, commonSettings) == nrd::Result::SUCCESS);
    }

    // nrd::SetDenoiserSettings
    {
      const void* denoiserSettings = nullptr;

      switch (m_settings.m_denoiserDesc.denoiser) {
      case nrd::Denoiser::REBLUR_DIFFUSE_SPECULAR:
        denoiserSettings = static_cast<void*>(&m_settings.m_reblurSettings);
        break;
      case nrd::Denoiser::RELAX_DIFFUSE_SPECULAR:
        denoiserSettings = static_cast<void*>(&m_settings.m_relaxSettings);
        break;
      case nrd::Denoiser::REFERENCE:
        denoiserSettings = static_cast<void*>(&m_settings.m_referenceSettings);
        break;
      default:
        assert("Invalid option");
      };

      THROW_IF_FALSE(m_library->dispatch.SetDenoiserSettings(*m_denoiserInstance, m_settings.m_denoiserDesc.identifier, denoiserSettings) == nrd::Result::SUCCESS);
    }
  }

  void NRDContext::updateAdaptiveScaling(const VkExtent3D& renderSize) {
    // This default height is hard-code to align with NRD default settings (1440p),
    // we probably need to move this to settings later
    constexpr float defaultScreenHeight = 1440.0f;
    float radiusResolutionScale = RtxOptions::adaptiveResolutionDenoising() ? static_cast<float>(std::min(renderSize.width, renderSize.height)) / defaultScreenHeight : 1.0f;
    if (m_settings.m_denoiserDesc.denoiser == nrd::Denoiser::REBLUR_DIFFUSE_SPECULAR) {
      m_settings.m_reblurSettings.maxBlurRadius = m_settings.m_reblurInternalBlurRadius.maxBlurRadius > 0.0f ?
        std::max(1.0f, round(m_settings.m_reblurInternalBlurRadius.maxBlurRadius * radiusResolutionScale)) : 0.0f;
      m_settings.m_reblurSettings.diffusePrepassBlurRadius = m_settings.m_reblurInternalBlurRadius.diffusePrepassBlurRadius > 0.0f ?
        std::max(1.0f, round(m_settings.m_reblurInternalBlurRadius.diffusePrepassBlurRadius * radiusResolutionScale)) : 0.0f;
      m_settings.m_reblurSettings.specularPrepassBlurRadius = m_settings.m_reblurInternalBlurRadius.specularPrepassBlurRadius > 0.0f ?
        std::max(1.0f, round(m_settings.m_reblurInternalBlurRadius.specularPrepassBlurRadius * radiusResolutionScale)) : 0.0f;
    }
    else if (m_settings.m_denoiserDesc.denoiser == nrd::Denoiser::RELAX_DIFFUSE_SPECULAR) {
      m_settings.m_relaxSettings.diffusePrepassBlurRadius = m_settings.m_relaxInternalBlurRadius.diffusePrepassBlurRadius > 0.0f ?
        std::max(1.0f, round(m_settings.m_relaxInternalBlurRadius.diffusePrepassBlurRadius * radiusResolutionScale)) : 0.0f;
      m_settings.m_relaxSettings.specularPrepassBlurRadius = m_settings.m_relaxInternalBlurRadius.specularPrepassBlurRadius > 0.0f ?
        std::max(1.0f, round(m_settings.m_relaxInternalBlurRadius.specularPrepassBlurRadius * radiusResolutionScale)) : 0.0f;
    }
  }

  void NRDContext::destroyResources() {
    m_transientTex.clear();
    m_permanentTex.clear();
    m_validationTex.reset();
    m_resourceExtent = {};
    std::lock_guard<std::mutex> lock(m_sharedTransientMutex);
    auto pool = m_sharedTransientTex.find(device());
    if (pool != m_sharedTransientTex.end()) {
      auto& resources = pool->second;
      resources.erase(std::remove_if(resources.begin(), resources.end(),
        [](const auto& resource) { return resource.expired(); }), resources.end());
      if (resources.empty())
        m_sharedTransientTex.erase(pool);
    }
  }

  void NRDContext::destroyPipelines() {
    // Recorded command lists retain the pipelines until GPU completion. A
    // configuration change can safely drop our references without waiting idle.
    m_computePipelines.clear();
    m_staticSamplers.clear();
  }

  void NRDContext::showImguiSettings() {
    m_settings.showImguiSettings();
  }

  NrdArgs NRDContext::getNrdArgs() {
    if (m_type != DenoiserType::Reference)
      m_settings.updateDenoiserMode();
    static_assert(nrd::CommonSettings{}.denoisingRange == 500000.0f, "NRD's default settings has changed, denoisingRange must be re-evaluated");
    constexpr float denoisingRangeLimit = nrd::CommonSettings{}.denoisingRange;

    const float missLinearViewZ = denoisingRangeLimit + 1.0f;
    static_assert(missLinearViewZ > denoisingRangeLimit && missLinearViewZ - denoisingRangeLimit > 0.01f);

    // Note: Ensure the denoising range is at least 1 ulp less than the miss linear view Z value, otherwise it will not
    // function properly.
    assert(m_settings.m_commonSettings.denoisingRange <= denoisingRangeLimit);

    NrdArgs args;

    args.isReblurEnabled = m_settings.m_denoiserDesc.denoiser == nrd::Denoiser::REBLUR_DIFFUSE_SPECULAR;
    args.missLinearViewZ = missLinearViewZ;
    args.maxDirectHitTContribution = m_settings.m_groupedSettings.maxDirectHitTContribution;

    auto getHitDistanceParameters = [](const nrd::HitDistanceParameters& params) {
      return Vector4(params.A, params.B, params.C, params.D);
    };

    args.hitDistanceParams = getHitDistanceParameters(m_settings.m_reblurSettings.hitDistanceParameters);

    return args;
  }

  bool NRDContext::isReferenceDenoiserEnabled() {
    if (m_type != DenoiserType::Reference)
      m_settings.updateDenoiserMode();
    return m_settings.m_denoiserDesc.denoiser == nrd::Denoiser::REFERENCE;
  }

  const NrdSettings& NRDContext::getNrdSettings() const {
    return m_settings;
  }

  void NRDContext::setNrdSettings(const NrdSettings& refSettings) {
    const auto commonSettings = m_settings.m_commonSettings;
    const auto identifier = m_settings.m_denoiserDesc.identifier;
    const bool resetHistory = m_settings.m_resetHistory || m_settings.m_type != refSettings.m_type;
    m_settings = refSettings;
    // A second reference lobe copies user tuning, not the first lobe's
    // independently accumulated camera and jitter bookkeeping. The other
    // common fields include user tuning (validation, split screen, thresholds).
    memcpy(m_settings.m_commonSettings.worldToViewMatrix, commonSettings.worldToViewMatrix,
      sizeof(commonSettings.worldToViewMatrix));
    memcpy(m_settings.m_commonSettings.viewToClipMatrix, commonSettings.viewToClipMatrix,
      sizeof(commonSettings.viewToClipMatrix));
    memcpy(m_settings.m_commonSettings.cameraJitter, commonSettings.cameraJitter,
      sizeof(commonSettings.cameraJitter));
    m_settings.m_denoiserDesc.identifier = identifier;
    m_settings.m_resetHistory |= resetHistory;
  }

  void NRDContext::release() {
    if (!m_denoiserInstance && !m_cbData && m_computePipelines.empty())
      return;
    if (m_denoiserInstance)
      Logger::debug(str::format("[RTX] NRD: releasing ", getDenoiserName(), ", compiledPipelines=",
        m_compiledPipelineCount, "/", m_computePipelines.size()));
    destroyResources();
    destroyPipelines();
    m_cbData = nullptr;
    
    if (m_denoiserInstance) {
      m_library->dispatch.DestroyInstance(*m_denoiserInstance);
      m_denoiserInstance = nullptr;
    }
    m_denoiser = nrd::Denoiser::MAX_NUM;
    m_lastDispatchFrame = UINT32_MAX;
    m_settings.m_resetHistory = true;
    m_compiledPipelineCount = 0;
  }
} // namespace dxvk
