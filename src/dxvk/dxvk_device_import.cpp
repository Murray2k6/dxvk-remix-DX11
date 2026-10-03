#include <cstddef>
#include <cstring>
#include <iterator>
#include <optional>

#include <windows.h>

#include "dxvk_device_import.h"
#include "dxvk_scoped_annotation.h"

namespace dxvk {

  namespace {

    constexpr size_t kHeaderSize = sizeof(VkBaseOutStructure);

    // Number of VkBool32 fields in a feature struct: header to its last
    // member. sizeof() would also count the tail padding odd counts get.
#define REMIX_BOOLS(T, last) ((offsetof(T, last) + sizeof(VkBool32) - kHeaderSize) / sizeof(VkBool32))

    struct FeatureStructInfo {
      size_t size;    // bytes, for copying
      size_t bools;   // VkBool32 fields after the header
    };

    // Every feature struct Remix chains (DxvkDeviceFeatures and the
    // promoted-struct forms used when folding). Feature structs are a
    // VkBaseOutStructure header followed only by VkBool32 fields.
    FeatureStructInfo featureStructInfo(VkStructureType sType) {
#define REMIX_FEATURE(sType, T, last) case sType: return { sizeof(T), REMIX_BOOLS(T, last) };
      switch (sType) {
        REMIX_FEATURE(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, VkPhysicalDeviceVulkan11Features, shaderDrawParameters)
        REMIX_FEATURE(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, VkPhysicalDeviceVulkan12Features, subgroupBroadcastDynamicId)
        REMIX_FEATURE(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, VkPhysicalDeviceVulkan13Features, maintenance4)
        REMIX_FEATURE(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_4444_FORMATS_FEATURES_EXT, VkPhysicalDevice4444FormatsFeaturesEXT, formatA4B4G4R4)
        REMIX_FEATURE(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT, VkPhysicalDeviceCustomBorderColorFeaturesEXT, customBorderColorWithoutFormat)
        REMIX_FEATURE(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_ENABLE_FEATURES_EXT, VkPhysicalDeviceDepthClipEnableFeaturesEXT, depthClipEnable)
        REMIX_FEATURE(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FAULT_FEATURES_EXT, VkPhysicalDeviceFaultFeaturesEXT, deviceFaultVendorBinary)
        REMIX_FEATURE(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_FEATURES_EXT, VkPhysicalDeviceExtendedDynamicStateFeaturesEXT, extendedDynamicState)
        REMIX_FEATURE(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PRIORITY_FEATURES_EXT, VkPhysicalDeviceMemoryPriorityFeaturesEXT, memoryPriority)
        REMIX_FEATURE(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT, VkPhysicalDeviceRobustness2FeaturesEXT, nullDescriptor)
        REMIX_FEATURE(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DEMOTE_TO_HELPER_INVOCATION_FEATURES, VkPhysicalDeviceShaderDemoteToHelperInvocationFeatures, shaderDemoteToHelperInvocation)
        REMIX_FEATURE(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_FEATURES_EXT, VkPhysicalDeviceTransformFeedbackFeaturesEXT, geometryStreams)
        REMIX_FEATURE(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES_EXT, VkPhysicalDeviceVertexAttributeDivisorFeaturesEXT, vertexAttributeInstanceRateZeroDivisor)
        REMIX_FEATURE(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR, VkPhysicalDeviceRayQueryFeaturesKHR, rayQuery)
        REMIX_FEATURE(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR, VkPhysicalDeviceRayTracingPipelineFeaturesKHR, rayTraversalPrimitiveCulling)
        REMIX_FEATURE(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR, VkPhysicalDeviceAccelerationStructureFeaturesKHR, descriptorBindingAccelerationStructureUpdateAfterBind)
        REMIX_FEATURE(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_ATOMIC_FLOAT_FEATURES_EXT, VkPhysicalDeviceShaderAtomicFloatFeaturesEXT, sparseImageFloat32AtomicAdd)
        REMIX_FEATURE(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DIAGNOSTICS_CONFIG_FEATURES_NV, VkPhysicalDeviceDiagnosticsConfigFeaturesNV, diagnosticsConfig)
        REMIX_FEATURE(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES, VkPhysicalDeviceBufferDeviceAddressFeatures, bufferDeviceAddressMultiDevice)
        REMIX_FEATURE(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES, VkPhysicalDeviceSynchronization2Features, synchronization2)
        default: return { 0, 0 };
      }
#undef REMIX_FEATURE
    }

    // A struct promoted into VkPhysicalDeviceVulkan1xFeatures: its bools are
    // the contiguous run starting at `offset` in the 1.x struct.
    struct PromotedStruct {
      VkStructureType sType;
      size_t          offset;
      size_t          count;
    };

    const PromotedStruct kPromoted11[] = {
      { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES,              offsetof(VkPhysicalDeviceVulkan11Features, storageBuffer16BitAccess),      REMIX_BOOLS(VkPhysicalDevice16BitStorageFeatures, storageInputOutput16) },
      { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_FEATURES,                  offsetof(VkPhysicalDeviceVulkan11Features, multiview),                     REMIX_BOOLS(VkPhysicalDeviceMultiviewFeatures, multiviewTessellationShader) },
      { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VARIABLE_POINTERS_FEATURES,          offsetof(VkPhysicalDeviceVulkan11Features, variablePointersStorageBuffer), REMIX_BOOLS(VkPhysicalDeviceVariablePointersFeatures, variablePointers) },
      { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROTECTED_MEMORY_FEATURES,           offsetof(VkPhysicalDeviceVulkan11Features, protectedMemory),               REMIX_BOOLS(VkPhysicalDeviceProtectedMemoryFeatures, protectedMemory) },
      { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES,   offsetof(VkPhysicalDeviceVulkan11Features, samplerYcbcrConversion),        REMIX_BOOLS(VkPhysicalDeviceSamplerYcbcrConversionFeatures, samplerYcbcrConversion) },
      { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DRAW_PARAMETERS_FEATURES,     offsetof(VkPhysicalDeviceVulkan11Features, shaderDrawParameters),          REMIX_BOOLS(VkPhysicalDeviceShaderDrawParametersFeatures, shaderDrawParameters) },
    };

    // Vulkan12Features bits with no promoted struct (samplerMirrorClampToEdge,
    // drawIndirectCount, descriptorIndexing, samplerFilterMinmax,
    // shaderOutputViewportIndex, shaderOutputLayer, subgroupBroadcastDynamicId)
    // are implied by their extensions, which planDevice enables.
    const PromotedStruct kPromoted12[] = {
      { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_8BIT_STORAGE_FEATURES,                 offsetof(VkPhysicalDeviceVulkan12Features, storageBuffer8BitAccess),                    REMIX_BOOLS(VkPhysicalDevice8BitStorageFeatures, storagePushConstant8) },
      { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_ATOMIC_INT64_FEATURES,          offsetof(VkPhysicalDeviceVulkan12Features, shaderBufferInt64Atomics),                   REMIX_BOOLS(VkPhysicalDeviceShaderAtomicInt64Features, shaderSharedInt64Atomics) },
      { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES,          offsetof(VkPhysicalDeviceVulkan12Features, shaderFloat16),                              REMIX_BOOLS(VkPhysicalDeviceShaderFloat16Int8Features, shaderInt8) },
      { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES,          offsetof(VkPhysicalDeviceVulkan12Features, shaderInputAttachmentArrayDynamicIndexing),  REMIX_BOOLS(VkPhysicalDeviceDescriptorIndexingFeatures, runtimeDescriptorArray) },
      { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SCALAR_BLOCK_LAYOUT_FEATURES,          offsetof(VkPhysicalDeviceVulkan12Features, scalarBlockLayout),                          REMIX_BOOLS(VkPhysicalDeviceScalarBlockLayoutFeatures, scalarBlockLayout) },
      { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGELESS_FRAMEBUFFER_FEATURES,        offsetof(VkPhysicalDeviceVulkan12Features, imagelessFramebuffer),                       REMIX_BOOLS(VkPhysicalDeviceImagelessFramebufferFeatures, imagelessFramebuffer) },
      { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_UNIFORM_BUFFER_STANDARD_LAYOUT_FEATURES, offsetof(VkPhysicalDeviceVulkan12Features, uniformBufferStandardLayout),              REMIX_BOOLS(VkPhysicalDeviceUniformBufferStandardLayoutFeatures, uniformBufferStandardLayout) },
      { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_SUBGROUP_EXTENDED_TYPES_FEATURES, offsetof(VkPhysicalDeviceVulkan12Features, shaderSubgroupExtendedTypes),              REMIX_BOOLS(VkPhysicalDeviceShaderSubgroupExtendedTypesFeatures, shaderSubgroupExtendedTypes) },
      { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SEPARATE_DEPTH_STENCIL_LAYOUTS_FEATURES, offsetof(VkPhysicalDeviceVulkan12Features, separateDepthStencilLayouts),              REMIX_BOOLS(VkPhysicalDeviceSeparateDepthStencilLayoutsFeatures, separateDepthStencilLayouts) },
      { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_QUERY_RESET_FEATURES,             offsetof(VkPhysicalDeviceVulkan12Features, hostQueryReset),                             REMIX_BOOLS(VkPhysicalDeviceHostQueryResetFeatures, hostQueryReset) },
      { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES,           offsetof(VkPhysicalDeviceVulkan12Features, timelineSemaphore),                          REMIX_BOOLS(VkPhysicalDeviceTimelineSemaphoreFeatures, timelineSemaphore) },
      { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES,        offsetof(VkPhysicalDeviceVulkan12Features, bufferDeviceAddress),                        REMIX_BOOLS(VkPhysicalDeviceBufferDeviceAddressFeatures, bufferDeviceAddressMultiDevice) },
      { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_MEMORY_MODEL_FEATURES,          offsetof(VkPhysicalDeviceVulkan12Features, vulkanMemoryModel),                          REMIX_BOOLS(VkPhysicalDeviceVulkanMemoryModelFeatures, vulkanMemoryModelAvailabilityVisibilityChains) },
    };

    // Remix's 1.3-promoted structs, folded into a game's Vulkan13Features.
    const PromotedStruct kPromoted13[] = {
      { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DEMOTE_TO_HELPER_INVOCATION_FEATURES, offsetof(VkPhysicalDeviceVulkan13Features, shaderDemoteToHelperInvocation), 1 },
      { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES,                  offsetof(VkPhysicalDeviceVulkan13Features, synchronization2),               1 },
    };

#ifdef VK_VERSION_1_4
    // Remix's 1.4-promoted structs, folded into a game's Vulkan14Features.
    const PromotedStruct kPromoted14[] = {
      { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES_EXT, offsetof(VkPhysicalDeviceVulkan14Features, vertexAttributeInstanceRateDivisor), 2 },
    };
#endif

    VkBaseOutStructure* findInChain(const void* head, VkStructureType sType) {
      for (auto* s = reinterpret_cast<VkBaseOutStructure*>(const_cast<void*>(head)); s; s = s->pNext) {
        if (s->sType == sType)
          return s;
      }
      return nullptr;
    }

    bool anyBitSet(const void* bools, size_t count) {
      const VkBool32* b = reinterpret_cast<const VkBool32*>(bools);
      for (size_t i = 0; i < count; i++) {
        if (b[i])
          return true;
      }
      return false;
    }

    bool isWritable(const void* p, size_t size) {
      const uint8_t* bytes = reinterpret_cast<const uint8_t*>(p);

      for (const uint8_t* q : { bytes, bytes + size - 1 }) {
        MEMORY_BASIC_INFORMATION mbi = {};

        if (!VirtualQuery(q, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT || (mbi.Protect & PAGE_GUARD))
          return false;

        constexpr DWORD kWritable = PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;

        if (!(mbi.Protect & kWritable))
          return false;
      }

      return true;
    }

  }


  DxvkDeviceImporter::DxvkDeviceImporter(
    const Rc<DxvkInstance>& instance,
    const Rc<DxvkAdapter>&  adapter)
  : m_instance(instance), m_adapter(adapter) { }


  DxvkDeviceImporter::~DxvkDeviceImporter() {
    restore();
  }


  void DxvkDeviceImporter::setBaselineFeatures() {
    // What Remix's own passes (path tracer, NRD, DLSS, composite, the DXBC
    // capture replays) need from the device, each gated on support. Raster
    // state features the game uses are already in the game's create info.
    const DxvkDeviceFeatures& supported = m_adapter->features();

    auto& core = m_features.core.features;
    core.shaderStorageImageWriteWithoutFormat = supported.core.features.shaderStorageImageWriteWithoutFormat;
    core.shaderStorageImageReadWithoutFormat  = supported.core.features.shaderStorageImageReadWithoutFormat;
    core.fragmentStoresAndAtomics             = supported.core.features.fragmentStoresAndAtomics;
    core.vertexPipelineStoresAndAtomics       = supported.core.features.vertexPipelineStoresAndAtomics;
    core.shaderImageGatherExtended            = supported.core.features.shaderImageGatherExtended;
    core.shaderInt64                          = supported.core.features.shaderInt64;
    core.samplerAnisotropy                    = supported.core.features.samplerAnisotropy;
    core.textureCompressionBC                 = supported.core.features.textureCompressionBC;
    core.geometryShader                       = supported.core.features.geometryShader;
    core.depthClamp                           = supported.core.features.depthClamp;
    core.independentBlend                     = supported.core.features.independentBlend;

    m_features.vulkan11Features.shaderDrawParameters = supported.vulkan11Features.shaderDrawParameters;

    m_features.extRobustness2.nullDescriptor = supported.extRobustness2.nullDescriptor;
    m_features.extDeviceFault.deviceFault    = supported.extDeviceFault.deviceFault;
    m_features.extMemoryPriority.memoryPriority = supported.extMemoryPriority.memoryPriority;

    m_features.extShaderDemoteToHelperInvocation.shaderDemoteToHelperInvocation =
      supported.extShaderDemoteToHelperInvocation.shaderDemoteToHelperInvocation;

    // Post-VS capture replays game shaders through stream-out.
    m_features.extTransformFeedback.transformFeedback = supported.extTransformFeedback.transformFeedback;
    m_features.extTransformFeedback.geometryStreams   = supported.extTransformFeedback.geometryStreams;

    if (supported.extCustomBorderColor.customBorderColorWithoutFormat) {
      m_features.extCustomBorderColor.customBorderColors             = VK_TRUE;
      m_features.extCustomBorderColor.customBorderColorWithoutFormat = VK_TRUE;
    }
  }


  bool DxvkDeviceImporter::prepare(
    const VkDeviceCreateInfo&  gameInfo,
    const DxvkDeviceFeatures*  baseline) {
    ScopedCpuProfileZone();

    restore();

    m_info = gameInfo;
    m_features = {};
    m_extensions = DxvkDeviceExtensions();
    m_extensionsEnabled = DxvkNameSet();
    m_structStorage.clear();

    if (baseline) {
      m_features = *baseline;
      // planDevice rebuilds the chain from the struct members.
      m_features.core.pNext = nullptr;
      m_features.vulkan12Features.pNext = nullptr;
      // robustBufferAccess applies to every pipeline on the device, the
      // game's included, and costs it performance. Remix's passes do not
      // need it; D3D11's out-of-bounds rules only matter for D3D11 draws,
      // which Remix does not issue on an imported device.
      m_features.core.features.robustBufferAccess = VK_FALSE;
    } else {
      setBaselineFeatures();
    }

    try {
      m_adapter->planDevice(m_instance, m_extensions, m_features, m_extensionsEnabled);
      m_adapter->checkDriverVersion(m_instance);
    } catch (const DxvkError& e) {
      Logger::err(str::format("[Remix-Import] device does not meet Remix requirements: ", e.message()));
      return false;
    }

    if (!mergeExtensions(gameInfo) || !mergeFeatures(gameInfo) || !mergeQueues(gameInfo)) {
      restore();
      return false;
    }

    Logger::info(str::format("[Remix-Import] merged device create info: ",
      m_info.enabledExtensionCount, " extensions (game ", gameInfo.enabledExtensionCount, "), ",
      m_info.queueCreateInfoCount, " queue families, ",
      m_backups.size(), " game feature structs extended, ",
      m_structStorage.size(), " structs added"));

    for (uint32_t i = 0; i < m_info.enabledExtensionCount; i++)
      Logger::info(str::format("  ", m_info.ppEnabledExtensionNames[i]));

    DxvkAdapter::logFeatures(m_features);
    return true;
  }


  bool DxvkDeviceImporter::mergeExtensions(const VkDeviceCreateInfo& gameInfo) {
    m_extensionStorage.clear();
    m_extensionNames.clear();

    DxvkNameSet game;

    for (uint32_t i = 0; i < gameInfo.enabledExtensionCount; i++) {
      game.add(gameInfo.ppEnabledExtensionNames[i]);
      m_extensionStorage.emplace_back(gameInfo.ppEnabledExtensionNames[i]);
    }

    // VK_EXT_buffer_device_address may not be enabled together with
    // VK_KHR_buffer_device_address or Vulkan12Features::bufferDeviceAddress,
    // which Remix's acceleration structures need.
    if (game.supports(VK_EXT_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME)) {
      Logger::err("[Remix-Import] the game enabled VK_EXT_buffer_device_address; Remix needs the KHR/1.2 form and the two cannot be enabled together");
      return false;
    }

    DxvkNameList remix = m_extensionsEnabled.toNameList();

    for (uint32_t i = 0; i < remix.count(); i++) {
      if (!game.supports(remix.name(i)))
        m_extensionStorage.emplace_back(remix.name(i));
    }

    for (const auto& name : m_extensionStorage)
      m_extensionNames.push_back(name.c_str());

    m_info.enabledExtensionCount   = uint32_t(m_extensionNames.size());
    m_info.ppEnabledExtensionNames = m_extensionNames.data();
    return true;
  }


  bool DxvkDeviceImporter::orInto(
          VkBaseOutStructure* gameStruct,
    const void*               remixBools,
          size_t              boolOffset,
          size_t              boolCount) {
    VkBool32*       dst = reinterpret_cast<VkBool32*>(reinterpret_cast<uint8_t*>(gameStruct) + boolOffset);
    const VkBool32* src = reinterpret_cast<const VkBool32*>(remixBools);

    bool needed = false;

    for (size_t i = 0; i < boolCount && !needed; i++)
      needed = src[i] && !dst[i];

    if (!needed)
      return true;

    const size_t bytes = boolCount * sizeof(VkBool32);

    if (!isWritable(dst, bytes)) {
      Logger::err(str::format("[Remix-Import] game feature struct (sType ", uint32_t(gameStruct->sType),
        ") is in read-only memory; cannot add Remix's feature bits"));
      return false;
    }

    Backup backup;
    backup.bytes = dst;
    backup.original.assign(reinterpret_cast<uint8_t*>(dst), reinterpret_cast<uint8_t*>(dst) + bytes);
    m_backups.push_back(std::move(backup));

    for (size_t i = 0; i < boolCount; i++)
      dst[i] |= src[i];

    return true;
  }


  void DxvkDeviceImporter::prepend(const void* remixStruct, size_t size) {
    auto storage = std::make_unique<uint8_t[]>(size);
    std::memcpy(storage.get(), remixStruct, size);

    auto* s = reinterpret_cast<VkBaseOutStructure*>(storage.get());
    s->pNext = reinterpret_cast<VkBaseOutStructure*>(const_cast<void*>(m_info.pNext));
    m_info.pNext = s;

    m_structStorage.push_back(std::move(storage));
  }


  bool DxvkDeviceImporter::mergeFeatures(const VkDeviceCreateInfo& gameInfo) {
    const void* gameChain = gameInfo.pNext;

    // Core features: keep the game's choice of pEnabledFeatures or Features2.
    const size_t coreCount = sizeof(VkPhysicalDeviceFeatures) / sizeof(VkBool32);

    if (VkBaseOutStructure* f2 = findInChain(gameChain, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2)) {
      if (!orInto(f2, &m_features.core.features, offsetof(VkPhysicalDeviceFeatures2, features), coreCount))
        return false;
    } else {
      m_coreFeatures = gameInfo.pEnabledFeatures ? *gameInfo.pEnabledFeatures : VkPhysicalDeviceFeatures{};

      VkBool32*       dst = reinterpret_cast<VkBool32*>(&m_coreFeatures);
      const VkBool32* src = reinterpret_cast<const VkBool32*>(&m_features.core.features);

      for (size_t i = 0; i < coreCount; i++)
        dst[i] |= src[i];

      m_info.pEnabledFeatures = &m_coreFeatures;
    }

    VkBaseOutStructure* game11 = findInChain(gameChain, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES);
    VkBaseOutStructure* game12 = findInChain(gameChain, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES);
    VkBaseOutStructure* game13 = findInChain(gameChain, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES);
#ifdef VK_VERSION_1_4
    VkBaseOutStructure* game14 = findInChain(gameChain, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES);
#endif

    // A Vulkan1x struct of Remix's: OR into the game's, else split into the
    // promoted structs when the game used those, else add it whole.
    auto mergeVersioned = [&](const VkBaseOutStructure* remix, const FeatureStructInfo& info, VkBaseOutStructure* gameVersioned,
                              const PromotedStruct* promoted, size_t promotedCount) -> bool {
      const uint8_t* remixBytes = reinterpret_cast<const uint8_t*>(remix);
      const size_t size = info.size;

      if (gameVersioned)
        return orInto(gameVersioned, remixBytes + kHeaderSize, kHeaderSize, info.bools);

      bool gameUsesPromoted = false;

      for (size_t i = 0; i < promotedCount; i++)
        gameUsesPromoted |= findInChain(gameChain, promoted[i].sType) != nullptr;

      if (!gameUsesPromoted) {
        prepend(remix, size);
        return true;
      }

      for (size_t i = 0; i < promotedCount; i++) {
        const uint8_t* bits = remixBytes + promoted[i].offset;

        if (!anyBitSet(bits, promoted[i].count))
          continue;

        if (VkBaseOutStructure* g = findInChain(gameChain, promoted[i].sType)) {
          if (!orInto(g, bits, kHeaderSize, promoted[i].count))
            return false;
        } else {
          // Rounded like the real struct (8-byte aligned for its pNext).
          const size_t structSize = align(kHeaderSize + promoted[i].count * sizeof(VkBool32), sizeof(void*));
          std::vector<uint8_t> temp(structSize, 0);
          reinterpret_cast<VkBaseOutStructure*>(temp.data())->sType = promoted[i].sType;
          std::memcpy(temp.data() + kHeaderSize, bits, promoted[i].count * sizeof(VkBool32));
          prepend(temp.data(), structSize);
        }
      }

      return true;
    };

    // An individual struct of Remix's that a game's Vulkan1x struct covers.
    auto foldInto = [&](const VkBaseOutStructure* remix, VkBaseOutStructure* gameVersioned,
                        const PromotedStruct* promoted, size_t promotedCount, bool& folded) -> bool {
      folded = false;

      if (!gameVersioned)
        return true;

      for (size_t i = 0; i < promotedCount; i++) {
        if (promoted[i].sType == remix->sType) {
          folded = true;
          return orInto(gameVersioned, reinterpret_cast<const uint8_t*>(remix) + kHeaderSize,
            promoted[i].offset, promoted[i].count);
        }
      }

      return true;
    };

    // Collect Remix's chain first; prepend() rewrites m_info.pNext.
    std::vector<const VkBaseOutStructure*> remixChain;

    for (auto* s = reinterpret_cast<const VkBaseOutStructure*>(m_features.core.pNext); s; s = s->pNext)
      remixChain.push_back(s);

    for (const VkBaseOutStructure* remix : remixChain) {
      const FeatureStructInfo info = featureStructInfo(remix->sType);
      const size_t size = info.size;

      if (!size) {
        // RTX IO may chain structs this table does not know; their sizes are
        // unknown, so they cannot be copied.
        Logger::err(str::format("[Remix-Import] unknown Remix feature struct, sType ", uint32_t(remix->sType)));
        return false;
      }

      bool ok = true;

      if (remix->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES) {
        ok = mergeVersioned(remix, info, game11, kPromoted11, std::size(kPromoted11));
      } else if (remix->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES) {
        ok = mergeVersioned(remix, info, game12, kPromoted12, std::size(kPromoted12));
      } else {
        bool folded = false;
        ok = foldInto(remix, game13, kPromoted13, std::size(kPromoted13), folded);
#ifdef VK_VERSION_1_4
        if (ok && !folded)
          ok = foldInto(remix, game14, kPromoted14, std::size(kPromoted14), folded);
#endif
        if (ok && !folded) {
          if (VkBaseOutStructure* g = findInChain(gameChain, remix->sType))
            ok = orInto(g, reinterpret_cast<const uint8_t*>(remix) + kHeaderSize, kHeaderSize, info.bools);
          else
            prepend(remix, size);
        }
      }

      if (!ok)
        return false;
    }

    return true;
  }


  bool DxvkDeviceImporter::mergeQueues(const VkDeviceCreateInfo& gameInfo) {
    const DxvkAdapterQueueIndices families = m_adapter->findQueueFamilies();
    const auto& familyProps = m_adapter->m_queueFamilies;

    if (families.graphics == VK_QUEUE_FAMILY_IGNORED) {
      Logger::err("[Remix-Import] no graphics queue family");
      return false;
    }

    m_queueInfos.assign(gameInfo.pQueueCreateInfos, gameInfo.pQueueCreateInfos + gameInfo.queueCreateInfoCount);
    m_queuePriorities.clear();

    for (const auto& q : m_queueInfos)
      m_queuePriorities.emplace_back(q.pQueuePriorities, q.pQueuePriorities + q.queueCount);

    // Adds one queue for Remix to the game's create info for this family.
    auto addQueue = [&](uint32_t family) -> std::optional<DxvkAdapterQueueInfo> {
      size_t entry = m_queueInfos.size();

      for (size_t i = 0; i < m_queueInfos.size(); i++) {
        if (m_queueInfos[i].queueFamilyIndex == family && m_queueInfos[i].flags == 0)
          entry = i;
      }

      if (entry == m_queueInfos.size()) {
        VkDeviceQueueCreateInfo q = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
        q.queueFamilyIndex = family;
        m_queueInfos.push_back(q);
        m_queuePriorities.emplace_back();
      }

      auto& q = m_queueInfos[entry];

      if (q.queueCount >= familyProps[family].queueCount)
        return std::nullopt;

      const uint32_t index = q.queueCount++;
      m_queuePriorities[entry].push_back(1.0f);
      return DxvkAdapterQueueInfo{ family, index };
    };

    auto graphics = addQueue(families.graphics);

    m_queues = {};
    m_sharesGameQueue = false;

    if (!graphics) {
      // Typical of AMD and Intel, whose graphics family has one queue.
      // Remix then submits to the game's queue 0 of that family, and every
      // submission to it - the game's through the front end, Remix's through
      // DXVK's queue thread - goes through DXVK's queue mutex
      // (remix_vkfe_api::lock_queue).
      Logger::warn(str::format("[Remix-Import] graphics queue family ", families.graphics,
        " has no queue left for Remix (", familyProps[families.graphics].queueCount,
        " in total); sharing the game's queue 0 under a lock"));

      m_sharesGameQueue = true;
      m_queues.graphics = DxvkAdapterQueueInfo{ families.graphics, 0 };
      m_queues.transfer = m_queues.graphics;
      m_queues.present  = m_queues.graphics;

      // A game whose create info never asked for that family: request it.
      bool requested = false;

      for (const auto& q : m_queueInfos)
        requested |= q.queueFamilyIndex == families.graphics && q.flags == 0 && q.queueCount > 0;

      if (!requested && !addQueue(families.graphics))
        return false;

      for (size_t i = 0; i < m_queueInfos.size(); i++)
        m_queueInfos[i].pQueuePriorities = m_queuePriorities[i].data();

      m_info.queueCreateInfoCount = uint32_t(m_queueInfos.size());
      m_info.pQueueCreateInfos    = m_queueInfos.data();
      return true;
    }

    m_queues.graphics = *graphics;

    // Transfer and async compute get their own queue when one is free, and
    // share Remix's graphics queue otherwise (never the game's).
    std::optional<DxvkAdapterQueueInfo> transfer;

    if (families.transfer != VK_QUEUE_FAMILY_IGNORED && families.transfer != families.graphics)
      transfer = addQueue(families.transfer);

    m_queues.transfer = transfer ? *transfer : *graphics;

    if (families.asyncCompute != VK_QUEUE_FAMILY_IGNORED)
      m_queues.asyncCompute = addQueue(families.asyncCompute);

    // Remix composites into the game's swapchain image and never presents.
    m_queues.present = *graphics;

    for (size_t i = 0; i < m_queueInfos.size(); i++)
      m_queueInfos[i].pQueuePriorities = m_queuePriorities[i].data();

    m_info.queueCreateInfoCount = uint32_t(m_queueInfos.size());
    m_info.pQueueCreateInfos    = m_queueInfos.data();

    Logger::info(str::format("[Remix-Import] Remix queues: graphics ", m_queues.graphics.queueFamilyIndex, ":", m_queues.graphics.queueIndex,
      ", transfer ", m_queues.transfer.queueFamilyIndex, ":", m_queues.transfer.queueIndex,
      m_queues.asyncCompute ? str::format(", compute ", m_queues.asyncCompute->queueFamilyIndex, ":", m_queues.asyncCompute->queueIndex) : std::string()));
    return true;
  }


  void DxvkDeviceImporter::restore() {
    for (auto i = m_backups.rbegin(); i != m_backups.rend(); i++)
      std::memcpy(i->bytes, i->original.data(), i->original.size());

    m_backups.clear();
  }


  Rc<DxvkDevice> DxvkDeviceImporter::import(
          VkDevice                device,
          PFN_vkGetDeviceProcAddr getDeviceProcAddr) {
    ScopedCpuProfileZone();

    restore();

    if (device == VK_NULL_HANDLE)
      throw DxvkError("DxvkDeviceImporter: null VkDevice");

    // owned=false: the game destroys its device. The front end must tear
    // Remix down before it does (vkDestroyDevice / ID3D12Device release).
    Rc<vk::DeviceFn> vkd = new vk::DeviceFn(false, m_instance->handle(), device, getDeviceProcAddr);

    Rc<DxvkDevice> result = new DxvkDevice(m_adapter->vki(), m_instance, m_adapter, vkd,
      m_extensions, m_features, m_queues);
    result->initResources();

    Logger::info("[Remix-Import] Remix is running on the game's VkDevice");
    return result;
  }

}
