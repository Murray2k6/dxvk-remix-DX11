#include <algorithm>
#include <cstring>

#include "vklayer.h"

// Object tracking. Every hook calls the next layer first and records the
// result only on devices Remix runs on.

namespace remix_vklayer {

  namespace {

    bool isImageDescriptor(VkDescriptorType type) {
      return type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER || type == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE
          || type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE || type == VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;
    }

    bool isBufferDescriptor(VkDescriptorType type) {
      return type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER || type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC
          || type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER || type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;
    }

    Descriptor* slot(SetInfo& set, uint32_t binding, uint32_t element) {
      if (element >= kMaxArrayDescriptors)
        return nullptr;

      auto& array = set.bindings[binding];

      if (array.size() <= element)
        array.resize(element + 1);

      return &array[element];
    }

    void store(SetInfo& set, uint32_t binding, uint32_t element, VkDescriptorType type,
               const VkDescriptorImageInfo* image, const VkDescriptorBufferInfo* buffer) {
      Descriptor* d = slot(set, binding, element);

      if (!d)
        return;

      *d = Descriptor();
      d->type = type;

      if (image && isImageDescriptor(type)) {
        d->view   = image->imageView;
        d->layout = image->imageLayout;
      }

      if (buffer && isBufferDescriptor(type)) {
        d->buffer = buffer->buffer;
        d->offset = buffer->offset;
        d->range  = buffer->range;
      }
    }

    // ---------------------------------------------------------------- images

    VKAPI_ATTR VkResult VKAPI_CALL CreateImage(VkDevice device, const VkImageCreateInfo* pCreateInfo,
                                               const VkAllocationCallbacks* pAllocator, VkImage* pImage) {
      DeviceData* dev = deviceOf(device);

      // Remix copies from game images: textures once for their content hash,
      // the composed frame for the UI snapshot. Transfer-source usage costs
      // nothing on desktop GPUs; retried without it if the driver refuses.
      VkImageCreateInfo createInfo = *pCreateInfo;

      if (dev->active() && (createInfo.usage & (VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT))
       && !(createInfo.usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT))
        createInfo.usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;

      VkResult result = dev->CreateImage(device, &createInfo, pAllocator, pImage);

      if (result != VK_SUCCESS && createInfo.usage != pCreateInfo->usage) {
        createInfo.usage = pCreateInfo->usage;
        result = dev->CreateImage(device, &createInfo, pAllocator, pImage);
      }

      if (result == VK_SUCCESS && dev->active()) {
        ImageInfo info;
        info.format  = pCreateInfo->format;
        info.extent  = pCreateInfo->extent;
        info.mips    = pCreateInfo->mipLevels;
        info.layers  = pCreateInfo->arrayLayers;
        info.samples = pCreateInfo->samples;
        info.transferSrc = (createInfo.usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0;

        std::lock_guard lock(dev->mutex);
        dev->images[*pImage] = info;
      }

      return result;
    }

    VKAPI_ATTR void VKAPI_CALL DestroyImage(VkDevice device, VkImage image, const VkAllocationCallbacks* pAllocator) {
      DeviceData* dev = deviceOf(device);

      if (dev->active() && image) {
        remixApi()->on_image_destroy(dev->remix, image);

        std::lock_guard lock(dev->mutex);
        dev->images.erase(image);
      }

      dev->DestroyImage(device, image, pAllocator);
    }

    VKAPI_ATTR VkResult VKAPI_CALL CreateImageView(VkDevice device, const VkImageViewCreateInfo* pCreateInfo,
                                                   const VkAllocationCallbacks* pAllocator, VkImageView* pView) {
      DeviceData* dev = deviceOf(device);
      VkResult result = dev->CreateImageView(device, pCreateInfo, pAllocator, pView);

      if (result == VK_SUCCESS && dev->active()) {
        std::lock_guard lock(dev->mutex);

        ViewInfo info;
        info.image     = pCreateInfo->image;
        info.format    = pCreateInfo->format;
        info.baseLayer = pCreateInfo->subresourceRange.baseArrayLayer;

        uint32_t mips = pCreateInfo->subresourceRange.levelCount;

        if (mips == VK_REMAINING_MIP_LEVELS) {
          auto img = dev->images.find(pCreateInfo->image);
          const uint32_t total = img != dev->images.end() ? img->second.mips : 1u;
          mips = total > pCreateInfo->subresourceRange.baseMipLevel ? total - pCreateInfo->subresourceRange.baseMipLevel : 1u;
        }

        info.mips = mips;
        dev->views[*pView] = info;
      }

      return result;
    }

    VKAPI_ATTR void VKAPI_CALL DestroyImageView(VkDevice device, VkImageView view, const VkAllocationCallbacks* pAllocator) {
      DeviceData* dev = deviceOf(device);

      if (dev->active() && view) {
        std::lock_guard lock(dev->mutex);
        dev->views.erase(view);
      }

      dev->DestroyImageView(device, view, pAllocator);
    }

    // --------------------------------------------------------------- buffers

    VKAPI_ATTR VkResult VKAPI_CALL CreateBuffer(VkDevice device, const VkBufferCreateInfo* pCreateInfo,
                                                const VkAllocationCallbacks* pAllocator, VkBuffer* pBuffer) {
      DeviceData* dev = deviceOf(device);
      VkResult result = dev->CreateBuffer(device, pCreateInfo, pAllocator, pBuffer);

      if (result == VK_SUCCESS && dev->active()) {
        std::lock_guard lock(dev->mutex);
        dev->buffers[*pBuffer].size = pCreateInfo->size;
      }

      return result;
    }

    VKAPI_ATTR void VKAPI_CALL DestroyBuffer(VkDevice device, VkBuffer buffer, const VkAllocationCallbacks* pAllocator) {
      DeviceData* dev = deviceOf(device);

      if (dev->active() && buffer) {
        std::lock_guard lock(dev->mutex);
        dev->buffers.erase(buffer);
        dev->shadows.erase(buffer);
      }

      dev->DestroyBuffer(device, buffer, pAllocator);
    }

    // ------------------------------------------------- uniform buffer shadow

    // Copies `size` bytes into the shadow of a device-local buffer. Only
    // constant-buffer sized writes: geometry and texture uploads are not
    // duplicated. Caller holds dev->mutex.
    void writeShadow(DeviceData* dev, VkBuffer dst, VkDeviceSize offset, const uint8_t* src, VkDeviceSize size) {
      constexpr VkDeviceSize kMaxShadowedWrite = 64ull << 10;

      if (!src || !size || size > kMaxShadowedWrite)
        return;

      auto buf = dev->buffers.find(dst);

      if (buf == dev->buffers.end() || offset + size > buf->second.size || dev->mapped.count(buf->second.memory))
        return;

      BufferShadow& shadow = dev->shadows[dst];
      VkDeviceSize done = 0;

      while (done < size) {
        const VkDeviceSize page = (offset + done) / kShadowPageSize;
        const VkDeviceSize inPage = (offset + done) % kShadowPageSize;
        const VkDeviceSize n = std::min(size - done, kShadowPageSize - inPage);
        auto& bytes = shadow.pages[page];

        if (!bytes) {
          bytes = std::make_unique<uint8_t[]>(size_t(kShadowPageSize));
          std::memset(bytes.get(), 0, size_t(kShadowPageSize));
        }

        // memmove: a copy within one buffer reads its own shadow.
        std::memmove(bytes.get() + inPage, src + done, size_t(n));
        done += n;
      }
    }

    VKAPI_ATTR void VKAPI_CALL CmdUpdateBuffer(VkCommandBuffer cmd, VkBuffer dst, VkDeviceSize offset,
                                               VkDeviceSize size, const void* pData) {
      DeviceData* dev = deviceOf(cmd);

      if (dev->active()) {
        std::lock_guard lock(dev->mutex);
        writeShadow(dev, dst, offset, reinterpret_cast<const uint8_t*>(pData), size);
      }

      dev->CmdUpdateBuffer(cmd, dst, offset, size, pData);
    }

    void shadowCopy(DeviceData* dev, VkBuffer src, VkBuffer dst, VkDeviceSize srcOffset, VkDeviceSize dstOffset, VkDeviceSize size) {
      std::lock_guard lock(dev->mutex);
      VkDeviceSize available = size;
      bool perDraw = false;
      const uint8_t* bytes = cpuBytes(*dev, src, srcOffset, available, perDraw);

      if (bytes && available >= size)
        writeShadow(dev, dst, dstOffset, bytes, size);
    }

    VKAPI_ATTR void VKAPI_CALL CmdCopyBuffer(VkCommandBuffer cmd, VkBuffer src, VkBuffer dst,
                                             uint32_t count, const VkBufferCopy* pRegions) {
      DeviceData* dev = deviceOf(cmd);

      if (dev->active()) {
        for (uint32_t i = 0; i < count; i++)
          shadowCopy(dev, src, dst, pRegions[i].srcOffset, pRegions[i].dstOffset, pRegions[i].size);
      }

      dev->CmdCopyBuffer(cmd, src, dst, count, pRegions);
    }

    void shadowCopy2(DeviceData* dev, const VkCopyBufferInfo2* info) {
      for (uint32_t i = 0; i < info->regionCount; i++) {
        const VkBufferCopy2& r = info->pRegions[i];
        shadowCopy(dev, info->srcBuffer, info->dstBuffer, r.srcOffset, r.dstOffset, r.size);
      }
    }

    VKAPI_ATTR void VKAPI_CALL CmdCopyBuffer2(VkCommandBuffer cmd, const VkCopyBufferInfo2* pInfo) {
      DeviceData* dev = deviceOf(cmd);

      if (dev->active())
        shadowCopy2(dev, pInfo);

      dev->CmdCopyBuffer2(cmd, pInfo);
    }

    VKAPI_ATTR void VKAPI_CALL CmdCopyBuffer2KHR(VkCommandBuffer cmd, const VkCopyBufferInfo2* pInfo) {
      DeviceData* dev = deviceOf(cmd);

      if (dev->active())
        shadowCopy2(dev, pInfo);

      dev->CmdCopyBuffer2KHR(cmd, pInfo);
    }

    VKAPI_ATTR VkResult VKAPI_CALL BindBufferMemory(VkDevice device, VkBuffer buffer, VkDeviceMemory memory, VkDeviceSize offset) {
      DeviceData* dev = deviceOf(device);
      VkResult result = dev->BindBufferMemory(device, buffer, memory, offset);

      if (result == VK_SUCCESS && dev->active()) {
        std::lock_guard lock(dev->mutex);
        auto& info = dev->buffers[buffer];
        info.memory = memory;
        info.memoryOffset = offset;
      }

      return result;
    }

    void trackBind2(DeviceData* dev, uint32_t count, const VkBindBufferMemoryInfo* infos) {
      std::lock_guard lock(dev->mutex);

      for (uint32_t i = 0; i < count; i++) {
        auto& info = dev->buffers[infos[i].buffer];
        info.memory = infos[i].memory;
        info.memoryOffset = infos[i].memoryOffset;
      }
    }

    VKAPI_ATTR VkResult VKAPI_CALL BindBufferMemory2(VkDevice device, uint32_t count, const VkBindBufferMemoryInfo* infos) {
      DeviceData* dev = deviceOf(device);
      VkResult result = dev->BindBufferMemory2(device, count, infos);

      if (result == VK_SUCCESS && dev->active())
        trackBind2(dev, count, infos);

      return result;
    }

    VKAPI_ATTR VkResult VKAPI_CALL BindBufferMemory2KHR(VkDevice device, uint32_t count, const VkBindBufferMemoryInfo* infos) {
      DeviceData* dev = deviceOf(device);
      VkResult result = dev->BindBufferMemory2KHR(device, count, infos);

      if (result == VK_SUCCESS && dev->active())
        trackBind2(dev, count, infos);

      return result;
    }

    VKAPI_ATTR VkResult VKAPI_CALL MapMemory(VkDevice device, VkDeviceMemory memory, VkDeviceSize offset,
                                             VkDeviceSize size, VkMemoryMapFlags flags, void** ppData) {
      DeviceData* dev = deviceOf(device);
      VkResult result = dev->MapMemory(device, memory, offset, size, flags, ppData);

      if (result == VK_SUCCESS && dev->active() && ppData && *ppData) {
        std::lock_guard lock(dev->mutex);
        dev->mapped[memory] = reinterpret_cast<uint8_t*>(*ppData) - offset;
      }

      return result;
    }

    VKAPI_ATTR void VKAPI_CALL UnmapMemory(VkDevice device, VkDeviceMemory memory) {
      DeviceData* dev = deviceOf(device);

      if (dev->active()) {
        std::lock_guard lock(dev->mutex);
        dev->mapped.erase(memory);
      }

      dev->UnmapMemory(device, memory);
    }

    VKAPI_ATTR void VKAPI_CALL FreeMemory(VkDevice device, VkDeviceMemory memory, const VkAllocationCallbacks* pAllocator) {
      DeviceData* dev = deviceOf(device);

      if (dev->active() && memory) {
        std::lock_guard lock(dev->mutex);
        dev->mapped.erase(memory);
      }

      dev->FreeMemory(device, memory, pAllocator);
    }

    // ----------------------------------------------------------- descriptors

    VKAPI_ATTR VkResult VKAPI_CALL CreateDescriptorSetLayout(VkDevice device, const VkDescriptorSetLayoutCreateInfo* pCreateInfo,
                                                             const VkAllocationCallbacks* pAllocator, VkDescriptorSetLayout* pLayout) {
      DeviceData* dev = deviceOf(device);
      VkResult result = dev->CreateDescriptorSetLayout(device, pCreateInfo, pAllocator, pLayout);

      if (result == VK_SUCCESS && dev->active()) {
        auto info = std::make_shared<SetLayoutInfo>();

        for (uint32_t i = 0; i < pCreateInfo->bindingCount; i++) {
          const auto& b = pCreateInfo->pBindings[i];
          info->bindings[b.binding] = { b.descriptorType, b.descriptorCount, b.stageFlags };
        }

        std::lock_guard lock(dev->mutex);
        dev->setLayouts[*pLayout] = std::move(info);
      }

      return result;
    }

    VKAPI_ATTR void VKAPI_CALL DestroyDescriptorSetLayout(VkDevice device, VkDescriptorSetLayout layout, const VkAllocationCallbacks* pAllocator) {
      DeviceData* dev = deviceOf(device);

      if (dev->active() && layout) {
        std::lock_guard lock(dev->mutex);
        dev->setLayouts.erase(layout);
      }

      dev->DestroyDescriptorSetLayout(device, layout, pAllocator);
    }

    VKAPI_ATTR VkResult VKAPI_CALL CreatePipelineLayout(VkDevice device, const VkPipelineLayoutCreateInfo* pCreateInfo,
                                                        const VkAllocationCallbacks* pAllocator, VkPipelineLayout* pLayout) {
      DeviceData* dev = deviceOf(device);
      VkResult result = dev->CreatePipelineLayout(device, pCreateInfo, pAllocator, pLayout);

      if (result == VK_SUCCESS && dev->active()) {
        std::lock_guard lock(dev->mutex);
        auto info = std::make_shared<PipelineLayoutInfo>();

        for (uint32_t i = 0; i < pCreateInfo->setLayoutCount; i++) {
          auto it = dev->setLayouts.find(pCreateInfo->pSetLayouts[i]);
          info->sets.push_back(it != dev->setLayouts.end() ? it->second : nullptr);
        }

        dev->pipelineLayouts[*pLayout] = std::move(info);
      }

      return result;
    }

    VKAPI_ATTR void VKAPI_CALL DestroyPipelineLayout(VkDevice device, VkPipelineLayout layout, const VkAllocationCallbacks* pAllocator) {
      DeviceData* dev = deviceOf(device);

      if (dev->active() && layout) {
        std::lock_guard lock(dev->mutex);
        dev->pipelineLayouts.erase(layout);
      }

      dev->DestroyPipelineLayout(device, layout, pAllocator);
    }

    VKAPI_ATTR VkResult VKAPI_CALL AllocateDescriptorSets(VkDevice device, const VkDescriptorSetAllocateInfo* pAllocateInfo,
                                                          VkDescriptorSet* pSets) {
      DeviceData* dev = deviceOf(device);
      VkResult result = dev->AllocateDescriptorSets(device, pAllocateInfo, pSets);

      if (result == VK_SUCCESS && dev->active()) {
        std::lock_guard lock(dev->mutex);

        for (uint32_t i = 0; i < pAllocateInfo->descriptorSetCount; i++) {
          auto set = std::make_shared<SetInfo>();
          auto layout = dev->setLayouts.find(pAllocateInfo->pSetLayouts[i]);
          set->layout = layout != dev->setLayouts.end() ? layout->second : nullptr;
          set->pool = pAllocateInfo->descriptorPool;
          dev->sets[pSets[i]] = std::move(set);
        }
      }

      return result;
    }

    VKAPI_ATTR VkResult VKAPI_CALL FreeDescriptorSets(VkDevice device, VkDescriptorPool pool, uint32_t count, const VkDescriptorSet* pSets) {
      DeviceData* dev = deviceOf(device);

      if (dev->active()) {
        std::lock_guard lock(dev->mutex);

        for (uint32_t i = 0; i < count; i++)
          dev->sets.erase(pSets[i]);
      }

      return dev->FreeDescriptorSets(device, pool, count, pSets);
    }

    void dropPoolSets(DeviceData* dev, VkDescriptorPool pool) {
      std::lock_guard lock(dev->mutex);

      for (auto it = dev->sets.begin(); it != dev->sets.end(); ) {
        if (it->second->pool == pool)
          it = dev->sets.erase(it);
        else
          ++it;
      }
    }

    VKAPI_ATTR VkResult VKAPI_CALL ResetDescriptorPool(VkDevice device, VkDescriptorPool pool, VkDescriptorPoolResetFlags flags) {
      DeviceData* dev = deviceOf(device);

      if (dev->active())
        dropPoolSets(dev, pool);

      return dev->ResetDescriptorPool(device, pool, flags);
    }

    VKAPI_ATTR void VKAPI_CALL DestroyDescriptorPool(VkDevice device, VkDescriptorPool pool, const VkAllocationCallbacks* pAllocator) {
      DeviceData* dev = deviceOf(device);

      if (dev->active() && pool)
        dropPoolSets(dev, pool);

      dev->DestroyDescriptorPool(device, pool, pAllocator);
    }

    VKAPI_ATTR void VKAPI_CALL UpdateDescriptorSets(VkDevice device, uint32_t writeCount, const VkWriteDescriptorSet* pWrites,
                                                    uint32_t copyCount, const VkCopyDescriptorSet* pCopies) {
      DeviceData* dev = deviceOf(device);
      dev->UpdateDescriptorSets(device, writeCount, pWrites, copyCount, pCopies);

      if (!dev->active())
        return;

      std::lock_guard lock(dev->mutex);
      applyWrites(*dev, writeCount, pWrites, nullptr);

      for (uint32_t i = 0; i < copyCount; i++) {
        const auto& c = pCopies[i];
        auto src = dev->sets.find(c.srcSet);
        auto dst = dev->sets.find(c.dstSet);

        if (src == dev->sets.end() || dst == dev->sets.end())
          continue;

        auto from = src->second->bindings.find(c.srcBinding);

        for (uint32_t k = 0; k < c.descriptorCount; k++) {
          const uint32_t s = c.srcArrayElement + k;
          Descriptor* d = slot(*dst->second, c.dstBinding, c.dstArrayElement + k);

          if (d)
            *d = (from != src->second->bindings.end() && s < from->second.size()) ? from->second[s] : Descriptor();
        }
      }
    }

    VkResult createTemplate(DeviceData* dev, PFN_vkCreateDescriptorUpdateTemplate next, VkDevice device,
                            const VkDescriptorUpdateTemplateCreateInfo* pCreateInfo,
                            const VkAllocationCallbacks* pAllocator, VkDescriptorUpdateTemplate* pTemplate) {
      VkResult result = next(device, pCreateInfo, pAllocator, pTemplate);

      if (result == VK_SUCCESS && dev->active()) {
        TemplateInfo info;
        info.type = pCreateInfo->templateType;
        info.set = pCreateInfo->set;
        info.entries.assign(pCreateInfo->pDescriptorUpdateEntries,
                            pCreateInfo->pDescriptorUpdateEntries + pCreateInfo->descriptorUpdateEntryCount);

        std::lock_guard lock(dev->mutex);
        dev->templates[*pTemplate] = std::move(info);
      }

      return result;
    }

    VKAPI_ATTR VkResult VKAPI_CALL CreateDescriptorUpdateTemplate(VkDevice device, const VkDescriptorUpdateTemplateCreateInfo* pCreateInfo,
                                                                  const VkAllocationCallbacks* pAllocator, VkDescriptorUpdateTemplate* pTemplate) {
      DeviceData* dev = deviceOf(device);
      return createTemplate(dev, dev->CreateDescriptorUpdateTemplate, device, pCreateInfo, pAllocator, pTemplate);
    }

    VKAPI_ATTR VkResult VKAPI_CALL CreateDescriptorUpdateTemplateKHR(VkDevice device, const VkDescriptorUpdateTemplateCreateInfo* pCreateInfo,
                                                                     const VkAllocationCallbacks* pAllocator, VkDescriptorUpdateTemplate* pTemplate) {
      DeviceData* dev = deviceOf(device);
      return createTemplate(dev, dev->CreateDescriptorUpdateTemplateKHR, device, pCreateInfo, pAllocator, pTemplate);
    }

    VKAPI_ATTR void VKAPI_CALL DestroyDescriptorUpdateTemplate(VkDevice device, VkDescriptorUpdateTemplate t, const VkAllocationCallbacks* pAllocator) {
      DeviceData* dev = deviceOf(device);

      if (dev->active() && t) {
        std::lock_guard lock(dev->mutex);
        dev->templates.erase(t);
      }

      dev->DestroyDescriptorUpdateTemplate(device, t, pAllocator);
    }

    VKAPI_ATTR void VKAPI_CALL DestroyDescriptorUpdateTemplateKHR(VkDevice device, VkDescriptorUpdateTemplate t, const VkAllocationCallbacks* pAllocator) {
      DeviceData* dev = deviceOf(device);

      if (dev->active() && t) {
        std::lock_guard lock(dev->mutex);
        dev->templates.erase(t);
      }

      dev->DestroyDescriptorUpdateTemplateKHR(device, t, pAllocator);
    }

    void updateWithTemplate(DeviceData* dev, VkDescriptorSet set, VkDescriptorUpdateTemplate t, const void* data) {
      std::lock_guard lock(dev->mutex);
      auto ti = dev->templates.find(t);
      auto si = dev->sets.find(set);

      if (ti != dev->templates.end() && si != dev->sets.end())
        applyTemplate(*dev, ti->second, data, *si->second);
    }

    VKAPI_ATTR void VKAPI_CALL UpdateDescriptorSetWithTemplate(VkDevice device, VkDescriptorSet set,
                                                               VkDescriptorUpdateTemplate t, const void* pData) {
      DeviceData* dev = deviceOf(device);
      dev->UpdateDescriptorSetWithTemplate(device, set, t, pData);

      if (dev->active())
        updateWithTemplate(dev, set, t, pData);
    }

    VKAPI_ATTR void VKAPI_CALL UpdateDescriptorSetWithTemplateKHR(VkDevice device, VkDescriptorSet set,
                                                                  VkDescriptorUpdateTemplate t, const void* pData) {
      DeviceData* dev = deviceOf(device);
      dev->UpdateDescriptorSetWithTemplateKHR(device, set, t, pData);

      if (dev->active())
        updateWithTemplate(dev, set, t, pData);
    }

    // ----------------------------------------------------------- render passes

    bool hasStencil(VkFormat format) {
      return format == VK_FORMAT_S8_UINT || format == VK_FORMAT_D16_UNORM_S8_UINT
          || format == VK_FORMAT_D24_UNORM_S8_UINT || format == VK_FORMAT_D32_SFLOAT_S8_UINT;
    }

    // Contents survive ending the instance early (RenderPassInfo::allStored).
    bool storeKeeps(VkAttachmentStoreOp op) {
      return op == VK_ATTACHMENT_STORE_OP_STORE || op == VK_ATTACHMENT_STORE_OP_NONE;
    }

    template<typename Attachment>
    bool attachmentStored(const Attachment& a) {
      return storeKeeps(a.storeOp) && (!hasStencil(a.format) || storeKeeps(a.stencilStoreOp));
    }

    // Compatible single-subpass pass on Remix's terrain bake images
    // (RenderPassInfo::bake): same formats, sample counts and attachment
    // references, everything loaded, stored and kept in GENERAL. Bakes of
    // one frame write the same images back to back, so writes are ordered
    // against the previous instance.
    VkRenderPass createBakePass(DeviceData* dev, VkDevice device, const std::vector<VkFormat>& formats,
                                const std::vector<VkSampleCountFlagBits>& samples, const RenderPassInfo::Subpass& subpass,
                                const VkAllocationCallbacks* pAllocator) {
      std::vector<VkAttachmentDescription> attachments(formats.size());

      for (size_t i = 0; i < formats.size(); i++) {
        VkAttachmentDescription& a = attachments[i];
        a.format         = formats[i];
        a.samples        = samples[i];
        a.loadOp         = VK_ATTACHMENT_LOAD_OP_LOAD;
        a.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
        a.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_LOAD;
        a.stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE;
        a.initialLayout  = VK_IMAGE_LAYOUT_GENERAL;
        a.finalLayout    = VK_IMAGE_LAYOUT_GENERAL;
      }

      std::vector<VkAttachmentReference> colors;

      for (uint32_t a : subpass.colors)
        colors.push_back({ a, a == VK_ATTACHMENT_UNUSED ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_GENERAL });

      const VkAttachmentReference depth = { subpass.depth, VK_IMAGE_LAYOUT_GENERAL };

      VkSubpassDescription sp = {};
      sp.pipelineBindPoint       = VK_PIPELINE_BIND_POINT_GRAPHICS;
      sp.colorAttachmentCount    = uint32_t(colors.size());
      sp.pColorAttachments       = colors.data();
      sp.pDepthStencilAttachment = subpass.depth != VK_ATTACHMENT_UNUSED ? &depth : nullptr;

      constexpr VkPipelineStageFlags kStages = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
        | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
      constexpr VkAccessFlags kWrites = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
      constexpr VkAccessFlags kAccess = kWrites | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;

      VkSubpassDependency deps[2] = {};
      deps[0].srcSubpass    = VK_SUBPASS_EXTERNAL;
      deps[0].dstSubpass    = 0;
      deps[0].srcStageMask  = kStages;
      deps[0].dstStageMask  = kStages;
      deps[0].srcAccessMask = kWrites;
      deps[0].dstAccessMask = kAccess;
      deps[1].srcSubpass    = 0;
      deps[1].dstSubpass    = VK_SUBPASS_EXTERNAL;
      deps[1].srcStageMask  = kStages;
      deps[1].dstStageMask  = kStages;
      deps[1].srcAccessMask = kWrites;
      deps[1].dstAccessMask = kAccess;

      VkRenderPassCreateInfo info = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
      info.attachmentCount = uint32_t(attachments.size());
      info.pAttachments    = attachments.data();
      info.subpassCount    = 1;
      info.pSubpasses      = &sp;
      info.dependencyCount = 2;
      info.pDependencies   = deps;

      VkRenderPass pass = VK_NULL_HANDLE;

      if (dev->CreateRenderPass(device, &info, pAllocator, &pass) != VK_SUCCESS)
        return VK_NULL_HANDLE;

      return pass;
    }

    // The one subpass uses every attachment exactly once, as colour or
    // depth, and nothing else (no input, resolve or preserve attachments).
    template<typename Subpass>
    bool bakeableSubpass(const Subpass& sp, uint32_t attachmentCount) {
      if (sp.inputAttachmentCount || sp.pResolveAttachments || sp.preserveAttachmentCount)
        return false;

      std::vector<bool> used(attachmentCount, false);
      uint32_t count = 0;

      auto use = [&](uint32_t a) {
        if (a == VK_ATTACHMENT_UNUSED)
          return true;
        if (a >= attachmentCount || used[a])
          return false;
        used[a] = true;
        count++;
        return true;
      };

      for (uint32_t c = 0; c < sp.colorAttachmentCount; c++) {
        if (!use(sp.pColorAttachments[c].attachment))
          return false;
      }

      if (sp.pDepthStencilAttachment && !use(sp.pDepthStencilAttachment->attachment))
        return false;

      return count == attachmentCount;
    }

    VKAPI_ATTR VkResult VKAPI_CALL CreateRenderPass(VkDevice device, const VkRenderPassCreateInfo* pCreateInfo,
                                                    const VkAllocationCallbacks* pAllocator, VkRenderPass* pRenderPass) {
      DeviceData* dev = deviceOf(device);
      VkResult result = dev->CreateRenderPass(device, pCreateInfo, pAllocator, pRenderPass);

      if (result == VK_SUCCESS && dev->active()) {
        RenderPassInfo info;
        std::vector<VkSampleCountFlagBits> samples;

        for (uint32_t i = 0; i < pCreateInfo->attachmentCount; i++) {
          const auto& a = pCreateInfo->pAttachments[i];
          info.formats.push_back(a.format);
          info.finalLayouts.push_back(a.finalLayout);
          samples.push_back(a.samples);
          info.allStored &= attachmentStored(a);
        }

        for (uint32_t s = 0; s < pCreateInfo->subpassCount; s++) {
          const auto& sp = pCreateInfo->pSubpasses[s];
          RenderPassInfo::Subpass subpass;

          for (uint32_t c = 0; c < sp.colorAttachmentCount; c++)
            subpass.colors.push_back(sp.pColorAttachments[c].attachment);

          if (sp.pDepthStencilAttachment)
            subpass.depth = sp.pDepthStencilAttachment->attachment;

          info.subpasses.push_back(std::move(subpass));
        }

        // Resume pass for the UI snapshot split: same attachments and
        // subpass, contents loaded, starting where the original ends.
        if (pCreateInfo->subpassCount == 1) {
          std::vector<VkAttachmentDescription> attachments(pCreateInfo->pAttachments,
            pCreateInfo->pAttachments + pCreateInfo->attachmentCount);

          for (auto& a : attachments) {
            a.loadOp        = VK_ATTACHMENT_LOAD_OP_LOAD;
            a.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
            a.initialLayout = a.finalLayout;
          }

          VkRenderPassCreateInfo resumeInfo = *pCreateInfo;
          resumeInfo.pAttachments = attachments.data();
          dev->CreateRenderPass(device, &resumeInfo, pAllocator, &info.resume);

          // Multiview (pNext) passes are not baked.
          if (!pCreateInfo->pNext && bakeableSubpass(pCreateInfo->pSubpasses[0], pCreateInfo->attachmentCount))
            info.bake = createBakePass(dev, device, info.formats, samples, info.subpasses[0], pAllocator);
        }

        std::lock_guard lock(dev->mutex);
        dev->renderPasses[*pRenderPass] = std::move(info);
      }

      return result;
    }

    VkResult createRenderPass2(DeviceData* dev, PFN_vkCreateRenderPass2 next, VkDevice device,
                               const VkRenderPassCreateInfo2* pCreateInfo, const VkAllocationCallbacks* pAllocator,
                               VkRenderPass* pRenderPass) {
      VkResult result = next(device, pCreateInfo, pAllocator, pRenderPass);

      if (result == VK_SUCCESS && dev->active()) {
        RenderPassInfo info;
        std::vector<VkSampleCountFlagBits> samples;

        for (uint32_t i = 0; i < pCreateInfo->attachmentCount; i++) {
          const auto& a = pCreateInfo->pAttachments[i];
          info.formats.push_back(a.format);
          info.finalLayouts.push_back(a.finalLayout);
          samples.push_back(a.samples);
          info.allStored &= attachmentStored(a);
        }

        for (uint32_t s = 0; s < pCreateInfo->subpassCount; s++) {
          const auto& sp = pCreateInfo->pSubpasses[s];
          RenderPassInfo::Subpass subpass;

          for (uint32_t c = 0; c < sp.colorAttachmentCount; c++)
            subpass.colors.push_back(sp.pColorAttachments[c].attachment);

          if (sp.pDepthStencilAttachment)
            subpass.depth = sp.pDepthStencilAttachment->attachment;

          info.subpasses.push_back(std::move(subpass));
        }

        // Resume pass for the UI snapshot split (see CreateRenderPass).
        if (pCreateInfo->subpassCount == 1) {
          std::vector<VkAttachmentDescription2> attachments(pCreateInfo->pAttachments,
            pCreateInfo->pAttachments + pCreateInfo->attachmentCount);

          for (auto& a : attachments) {
            a.loadOp        = VK_ATTACHMENT_LOAD_OP_LOAD;
            a.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
            a.initialLayout = a.finalLayout;
          }

          VkRenderPassCreateInfo2 resumeInfo = *pCreateInfo;
          resumeInfo.pAttachments = attachments.data();
          next(device, &resumeInfo, pAllocator, &info.resume);

          // Multiview, depth resolve and shading-rate attachments (pNext)
          // are not baked.
          const auto& sp = pCreateInfo->pSubpasses[0];

          if (!sp.viewMask && !sp.pNext && bakeableSubpass(sp, pCreateInfo->attachmentCount))
            info.bake = createBakePass(dev, device, info.formats, samples, info.subpasses[0], pAllocator);
        }

        std::lock_guard lock(dev->mutex);
        dev->renderPasses[*pRenderPass] = std::move(info);
      }

      return result;
    }

    VKAPI_ATTR VkResult VKAPI_CALL CreateRenderPass2(VkDevice device, const VkRenderPassCreateInfo2* pCreateInfo,
                                                     const VkAllocationCallbacks* pAllocator, VkRenderPass* pRenderPass) {
      DeviceData* dev = deviceOf(device);
      return createRenderPass2(dev, dev->CreateRenderPass2, device, pCreateInfo, pAllocator, pRenderPass);
    }

    VKAPI_ATTR VkResult VKAPI_CALL CreateRenderPass2KHR(VkDevice device, const VkRenderPassCreateInfo2* pCreateInfo,
                                                        const VkAllocationCallbacks* pAllocator, VkRenderPass* pRenderPass) {
      DeviceData* dev = deviceOf(device);
      return createRenderPass2(dev, dev->CreateRenderPass2KHR, device, pCreateInfo, pAllocator, pRenderPass);
    }

    VKAPI_ATTR void VKAPI_CALL DestroyRenderPass(VkDevice device, VkRenderPass renderPass, const VkAllocationCallbacks* pAllocator) {
      DeviceData* dev = deviceOf(device);

      if (dev->active() && renderPass) {
        VkRenderPass resume = VK_NULL_HANDLE;
        VkRenderPass bake = VK_NULL_HANDLE;
        std::vector<VkFramebuffer> bakeFramebuffers;

        {
          std::lock_guard lock(dev->mutex);
          auto it = dev->renderPasses.find(renderPass);

          if (it != dev->renderPasses.end()) {
            resume = it->second.resume;
            bake = it->second.bake;
            dev->renderPasses.erase(it);
          }

          // Framebuffers made for the bake pass (keyed by it first).
          for (auto fb = dev->bakeFramebuffers.begin(); bake && fb != dev->bakeFramebuffers.end(); ) {
            if (!fb->first.empty() && fb->first[0] == uint64_t(bake)) {
              bakeFramebuffers.push_back(fb->second);
              fb = dev->bakeFramebuffers.erase(fb);
            } else {
              ++fb;
            }
          }
        }

        for (VkFramebuffer fb : bakeFramebuffers)
          dev->DestroyFramebuffer(device, fb, nullptr);

        if (resume)
          dev->DestroyRenderPass(device, resume, pAllocator);

        if (bake)
          dev->DestroyRenderPass(device, bake, pAllocator);
      }

      dev->DestroyRenderPass(device, renderPass, pAllocator);
    }

    VKAPI_ATTR VkResult VKAPI_CALL CreateFramebuffer(VkDevice device, const VkFramebufferCreateInfo* pCreateInfo,
                                                     const VkAllocationCallbacks* pAllocator, VkFramebuffer* pFramebuffer) {
      DeviceData* dev = deviceOf(device);
      VkResult result = dev->CreateFramebuffer(device, pCreateInfo, pAllocator, pFramebuffer);

      if (result == VK_SUCCESS && dev->active()) {
        FramebufferInfo info;
        info.extent = { pCreateInfo->width, pCreateInfo->height };

        // Imageless framebuffers name their views at vkCmdBeginRenderPass.
        if (!(pCreateInfo->flags & VK_FRAMEBUFFER_CREATE_IMAGELESS_BIT) && pCreateInfo->pAttachments)
          info.views.assign(pCreateInfo->pAttachments, pCreateInfo->pAttachments + pCreateInfo->attachmentCount);

        std::lock_guard lock(dev->mutex);
        dev->framebuffers[*pFramebuffer] = std::move(info);
      }

      return result;
    }

    VKAPI_ATTR void VKAPI_CALL DestroyFramebuffer(VkDevice device, VkFramebuffer framebuffer, const VkAllocationCallbacks* pAllocator) {
      DeviceData* dev = deviceOf(device);

      if (dev->active() && framebuffer) {
        std::lock_guard lock(dev->mutex);
        dev->framebuffers.erase(framebuffer);
      }

      dev->DestroyFramebuffer(device, framebuffer, pAllocator);
    }

    // --------------------------------------------------------------- shaders

    VKAPI_ATTR VkResult VKAPI_CALL CreateShaderModule(VkDevice device, const VkShaderModuleCreateInfo* pCreateInfo,
                                                      const VkAllocationCallbacks* pAllocator, VkShaderModule* pModule) {
      DeviceData* dev = deviceOf(device);
      VkResult result = dev->CreateShaderModule(device, pCreateInfo, pAllocator, pModule);

      // Pipelines are compiled from modules by handle; the SPIR-V is kept
      // for Remix's analysis until the module is destroyed.
      if (result == VK_SUCCESS && dev->active()) {
        std::vector<uint32_t> code(pCreateInfo->pCode, pCreateInfo->pCode + pCreateInfo->codeSize / 4);
        std::lock_guard lock(dev->mutex);
        dev->shaderModules[*pModule] = std::move(code);
      }

      return result;
    }

    VKAPI_ATTR void VKAPI_CALL DestroyShaderModule(VkDevice device, VkShaderModule module, const VkAllocationCallbacks* pAllocator) {
      DeviceData* dev = deviceOf(device);

      if (dev->active() && module) {
        std::lock_guard lock(dev->mutex);
        dev->shaderModules.erase(module);
      }

      dev->DestroyShaderModule(device, module, pAllocator);
    }

  }


  void applyWrites(DeviceData& dev, uint32_t count, const VkWriteDescriptorSet* writes, SetInfo* pushTarget) {
    for (uint32_t i = 0; i < count; i++) {
      const auto& w = writes[i];
      SetInfo* set = pushTarget;

      if (!set) {
        auto it = dev.sets.find(w.dstSet);

        if (it == dev.sets.end())
          continue;

        set = it->second.get();
      }

      for (uint32_t k = 0; k < w.descriptorCount; k++) {
        store(*set, w.dstBinding, w.dstArrayElement + k, w.descriptorType,
              w.pImageInfo ? &w.pImageInfo[k] : nullptr,
              w.pBufferInfo ? &w.pBufferInfo[k] : nullptr);
      }
    }
  }


  void applyTemplate(DeviceData& dev, const TemplateInfo& t, const void* data, SetInfo& set) {
    const uint8_t* base = reinterpret_cast<const uint8_t*>(data);

    for (const auto& e : t.entries) {
      for (uint32_t k = 0; k < e.descriptorCount; k++) {
        const uint8_t* p = base + e.offset + size_t(k) * e.stride;

        store(set, e.dstBinding, e.dstArrayElement + k, e.descriptorType,
              isImageDescriptor(e.descriptorType) ? reinterpret_cast<const VkDescriptorImageInfo*>(p) : nullptr,
              isBufferDescriptor(e.descriptorType) ? reinterpret_cast<const VkDescriptorBufferInfo*>(p) : nullptr);
      }
    }
  }


  const uint8_t* cpuBytes(DeviceData& dev, VkBuffer buffer, VkDeviceSize offset, VkDeviceSize& size, bool& perDraw) {
    perDraw = false;

    auto buf = dev.buffers.find(buffer);

    if (buf == dev.buffers.end() || offset >= buf->second.size)
      return nullptr;

    size = std::min(size, buf->second.size - offset);

    auto map = dev.mapped.find(buf->second.memory);

    if (map != dev.mapped.end())
      return map->second + buf->second.memoryOffset + offset;

    // Device-local: the shadow page holding the range's start.
    auto shadow = dev.shadows.find(buffer);

    if (shadow == dev.shadows.end())
      return nullptr;

    auto page = shadow->second.pages.find(offset / kShadowPageSize);

    if (page == shadow->second.pages.end())
      return nullptr;

    const VkDeviceSize inPage = offset % kShadowPageSize;
    size = std::min(size, kShadowPageSize - inPage);
    perDraw = true;
    return page->second.get() + inPage;
  }


  void fillBufferBinding(DeviceData& dev, const Descriptor& d, uint32_t dynamicOffset, remix_vkfe_binding& out) {
    const bool uniform = d.type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER || d.type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;

    out.kind   = uniform ? REMIX_VKFE_BINDING_CONSTANT_BUFFER : REMIX_VKFE_BINDING_STORAGE_BUFFER;
    out.buffer = d.buffer;
    out.offset = d.offset + dynamicOffset;

    auto buf = dev.buffers.find(d.buffer);

    if (buf == dev.buffers.end())
      return;

    out.size = d.range == VK_WHOLE_SIZE
      ? (buf->second.size > out.offset ? buf->second.size - out.offset : 0)
      : d.range;

    VkDeviceSize size = out.size;
    bool perDraw = false;

    if (const uint8_t* bytes = cpuBytes(dev, d.buffer, out.offset, size, perDraw)) {
      out.host_data          = bytes;
      out.host_data_per_draw = perDraw ? 1u : 0u;

      // A shadow covers one page from the start of the range.
      if (perDraw)
        out.size = size;
    }
  }


  void fillTextureBinding(DeviceData& dev, const Descriptor& d, remix_vkfe_binding& out) {
    out.kind   = REMIX_VKFE_BINDING_TEXTURE;
    out.view   = d.view;
    out.layout = d.layout;

    auto view = dev.views.find(d.view);

    if (view == dev.views.end())
      return;

    out.image      = view->second.image;
    out.format     = view->second.format;
    out.mip_levels = view->second.mips;

    auto img = dev.images.find(view->second.image);

    if (img != dev.images.end()) {
      out.extent       = img->second.extent;
      out.array_layers = img->second.layers;
      out.transfer_src = img->second.transferSrc ? 1u : 0u;
    }
  }


  remix_vkfe_attachment attachmentOf(DeviceData& dev, VkImageView view) {
    remix_vkfe_attachment a = {};
    a.view = view;

    auto v = dev.views.find(view);

    if (v == dev.views.end())
      return a;

    a.image      = v->second.image;
    a.format     = v->second.format;
    a.base_layer = v->second.baseLayer;

    auto img = dev.images.find(v->second.image);

    if (img != dev.images.end()) {
      a.extent = img->second.extent;
      a.is_swapchain_image = img->second.swapchain ? 1u : 0u;
    }

    return a;
  }


  PFN_vkVoidFunction trackHook(const char* name) {
#define REMIX_VKLAYER_HOOK(fn) if (!strcmp(name, "vk" #fn)) return reinterpret_cast<PFN_vkVoidFunction>(&fn);
    REMIX_VKLAYER_HOOK(CreateImage)
    REMIX_VKLAYER_HOOK(DestroyImage)
    REMIX_VKLAYER_HOOK(CreateImageView)
    REMIX_VKLAYER_HOOK(DestroyImageView)
    REMIX_VKLAYER_HOOK(CreateBuffer)
    REMIX_VKLAYER_HOOK(DestroyBuffer)
    REMIX_VKLAYER_HOOK(CmdUpdateBuffer)
    REMIX_VKLAYER_HOOK(CmdCopyBuffer)
    REMIX_VKLAYER_HOOK(CmdCopyBuffer2)
    REMIX_VKLAYER_HOOK(CmdCopyBuffer2KHR)
    REMIX_VKLAYER_HOOK(BindBufferMemory)
    REMIX_VKLAYER_HOOK(BindBufferMemory2)
    REMIX_VKLAYER_HOOK(BindBufferMemory2KHR)
    REMIX_VKLAYER_HOOK(MapMemory)
    REMIX_VKLAYER_HOOK(UnmapMemory)
    REMIX_VKLAYER_HOOK(FreeMemory)
    REMIX_VKLAYER_HOOK(CreateDescriptorSetLayout)
    REMIX_VKLAYER_HOOK(DestroyDescriptorSetLayout)
    REMIX_VKLAYER_HOOK(CreatePipelineLayout)
    REMIX_VKLAYER_HOOK(DestroyPipelineLayout)
    REMIX_VKLAYER_HOOK(AllocateDescriptorSets)
    REMIX_VKLAYER_HOOK(FreeDescriptorSets)
    REMIX_VKLAYER_HOOK(ResetDescriptorPool)
    REMIX_VKLAYER_HOOK(DestroyDescriptorPool)
    REMIX_VKLAYER_HOOK(UpdateDescriptorSets)
    REMIX_VKLAYER_HOOK(CreateDescriptorUpdateTemplate)
    REMIX_VKLAYER_HOOK(CreateDescriptorUpdateTemplateKHR)
    REMIX_VKLAYER_HOOK(DestroyDescriptorUpdateTemplate)
    REMIX_VKLAYER_HOOK(DestroyDescriptorUpdateTemplateKHR)
    REMIX_VKLAYER_HOOK(UpdateDescriptorSetWithTemplate)
    REMIX_VKLAYER_HOOK(UpdateDescriptorSetWithTemplateKHR)
    REMIX_VKLAYER_HOOK(CreateRenderPass)
    REMIX_VKLAYER_HOOK(CreateRenderPass2)
    REMIX_VKLAYER_HOOK(CreateRenderPass2KHR)
    REMIX_VKLAYER_HOOK(DestroyRenderPass)
    REMIX_VKLAYER_HOOK(CreateFramebuffer)
    REMIX_VKLAYER_HOOK(DestroyFramebuffer)
    REMIX_VKLAYER_HOOK(CreateShaderModule)
    REMIX_VKLAYER_HOOK(DestroyShaderModule)
#undef REMIX_VKLAYER_HOOK
    return nullptr;
  }

}
