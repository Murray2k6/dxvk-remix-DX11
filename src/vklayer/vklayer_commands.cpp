#include <algorithm>
#include <cstring>

#include "vklayer.h"

// Command buffers, pipelines, draws, submission, swap chains and present.

namespace remix_vklayer {

  namespace {

    uint32_t stageMaskOf(VkShaderStageFlags flags) {
      uint32_t mask = 0;
      if (flags & VK_SHADER_STAGE_VERTEX_BIT)                  mask |= 1u << REMIX_VKFE_STAGE_VERTEX;
      if (flags & VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT)    mask |= 1u << REMIX_VKFE_STAGE_HULL;
      if (flags & VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT) mask |= 1u << REMIX_VKFE_STAGE_DOMAIN;
      if (flags & VK_SHADER_STAGE_GEOMETRY_BIT)                mask |= 1u << REMIX_VKFE_STAGE_GEOMETRY;
      if (flags & VK_SHADER_STAGE_FRAGMENT_BIT)                mask |= 1u << REMIX_VKFE_STAGE_PIXEL;
      if (flags & VK_SHADER_STAGE_MESH_BIT_EXT)                mask |= 1u << REMIX_VKFE_STAGE_MESH;
      if (flags & VK_SHADER_STAGE_TASK_BIT_EXT)                mask |= 1u << REMIX_VKFE_STAGE_TASK;
      return mask;
    }

    int stageIndexOf(VkShaderStageFlagBits stage) {
      switch (stage) {
        case VK_SHADER_STAGE_VERTEX_BIT:                  return REMIX_VKFE_STAGE_VERTEX;
        case VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT:    return REMIX_VKFE_STAGE_HULL;
        case VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT: return REMIX_VKFE_STAGE_DOMAIN;
        case VK_SHADER_STAGE_GEOMETRY_BIT:                return REMIX_VKFE_STAGE_GEOMETRY;
        case VK_SHADER_STAGE_FRAGMENT_BIT:                return REMIX_VKFE_STAGE_PIXEL;
        case VK_SHADER_STAGE_MESH_BIT_EXT:                return REMIX_VKFE_STAGE_MESH;
        case VK_SHADER_STAGE_TASK_BIT_EXT:                return REMIX_VKFE_STAGE_TASK;
        default:                                          return -1;
      }
    }

    bool isDynamicDescriptor(VkDescriptorType type) {
      return type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC || type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;
    }

    template<typename T>
    const T* findChain(const void* pNext, VkStructureType sType) {
      for (auto* s = reinterpret_cast<const VkBaseInStructure*>(pNext); s; s = s->pNext) {
        if (s->sType == sType)
          return reinterpret_cast<const T*>(s);
      }

      return nullptr;
    }

    // --------------------------------------------------------- command buffers

    VKAPI_ATTR VkResult VKAPI_CALL AllocateCommandBuffers(VkDevice device, const VkCommandBufferAllocateInfo* pAllocateInfo,
                                                          VkCommandBuffer* pCommandBuffers) {
      DeviceData* dev = deviceOf(device);
      VkResult result = dev->AllocateCommandBuffers(device, pAllocateInfo, pCommandBuffers);

      if (result == VK_SUCCESS && dev->active()) {
        for (uint32_t i = 0; i < pAllocateInfo->commandBufferCount; i++) {
          CommandState* st = commandState(*dev, pCommandBuffers[i]);
          st->pool = pAllocateInfo->commandPool;
          st->primary = pAllocateInfo->level == VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        }
      }

      return result;
    }

    VKAPI_ATTR void VKAPI_CALL FreeCommandBuffers(VkDevice device, VkCommandPool pool, uint32_t count, const VkCommandBuffer* pCommandBuffers) {
      DeviceData* dev = deviceOf(device);

      if (dev->active()) {
        for (uint32_t i = 0; i < count; i++) {
          if (pCommandBuffers[i]) {
            resetCommandState(*dev, pCommandBuffers[i]);
            std::lock_guard lock(dev->mutex);
            dev->commandBuffers.erase(pCommandBuffers[i]);
          }
        }
      }

      dev->FreeCommandBuffers(device, pool, count, pCommandBuffers);
    }

    void resetPool(DeviceData* dev, VkCommandPool pool, bool erase) {
      std::vector<VkCommandBuffer> members;

      {
        std::lock_guard lock(dev->mutex);

        for (const auto& c : dev->commandBuffers) {
          if (c.second->pool == pool)
            members.push_back(c.first);
        }
      }

      for (VkCommandBuffer cmd : members) {
        resetCommandState(*dev, cmd);

        if (erase) {
          std::lock_guard lock(dev->mutex);
          dev->commandBuffers.erase(cmd);
        }
      }
    }

    VKAPI_ATTR VkResult VKAPI_CALL ResetCommandPool(VkDevice device, VkCommandPool pool, VkCommandPoolResetFlags flags) {
      DeviceData* dev = deviceOf(device);

      if (dev->active())
        resetPool(dev, pool, false);

      return dev->ResetCommandPool(device, pool, flags);
    }

    VKAPI_ATTR void VKAPI_CALL DestroyCommandPool(VkDevice device, VkCommandPool pool, const VkAllocationCallbacks* pAllocator) {
      DeviceData* dev = deviceOf(device);

      if (dev->active() && pool)
        resetPool(dev, pool, true);

      dev->DestroyCommandPool(device, pool, pAllocator);
    }

    VKAPI_ATTR VkResult VKAPI_CALL BeginCommandBuffer(VkCommandBuffer cmd, const VkCommandBufferBeginInfo* pBeginInfo) {
      DeviceData* dev = deviceOf(cmd);

      if (dev->active()) {
        // Beginning a command buffer implicitly resets it.
        resetCommandState(*dev, cmd);

        // A secondary command buffer continuing a render pass draws into
        // the inherited framebuffer.
        if (pBeginInfo->pInheritanceInfo && (pBeginInfo->flags & VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT)) {
          CommandState* st = commandState(*dev, cmd);
          std::lock_guard lock(dev->mutex);
          auto fb = dev->framebuffers.find(pBeginInfo->pInheritanceInfo->framebuffer);
          auto rp = dev->renderPasses.find(pBeginInfo->pInheritanceInfo->renderPass);

          if (fb != dev->framebuffers.end() && rp != dev->renderPasses.end()) {
            const uint32_t sp = pBeginInfo->pInheritanceInfo->subpass;

            if (sp < rp->second.subpasses.size()) {
              const auto& subpass = rp->second.subpasses[sp];
              st->renderTargetCount = 0;

              for (uint32_t a : subpass.colors) {
                if (a < fb->second.views.size() && st->renderTargetCount < 8)
                  st->renderTargets[st->renderTargetCount++] = attachmentOf(*dev, fb->second.views[a]);
              }

              if (subpass.depth < fb->second.views.size())
                st->depth = attachmentOf(*dev, fb->second.views[subpass.depth]);
            }
          }
        }
      }

      return dev->BeginCommandBuffer(cmd, pBeginInfo);
    }

    VKAPI_ATTR VkResult VKAPI_CALL ResetCommandBuffer(VkCommandBuffer cmd, VkCommandBufferResetFlags flags) {
      DeviceData* dev = deviceOf(cmd);

      if (dev->active())
        resetCommandState(*dev, cmd);

      return dev->ResetCommandBuffer(cmd, flags);
    }

    // ----------------------------------------------------------------- state

    VKAPI_ATTR void VKAPI_CALL CmdBindPipeline(VkCommandBuffer cmd, VkPipelineBindPoint bindPoint, VkPipeline pipeline) {
      DeviceData* dev = deviceOf(cmd);

      if (dev->active() && bindPoint == VK_PIPELINE_BIND_POINT_GRAPHICS)
        commandState(*dev, cmd)->pipeline = pipeline;

      dev->CmdBindPipeline(cmd, bindPoint, pipeline);
    }

    VKAPI_ATTR void VKAPI_CALL CmdBindDescriptorSets(VkCommandBuffer cmd, VkPipelineBindPoint bindPoint, VkPipelineLayout layout,
                                                     uint32_t firstSet, uint32_t setCount, const VkDescriptorSet* pSets,
                                                     uint32_t dynamicOffsetCount, const uint32_t* pDynamicOffsets) {
      DeviceData* dev = deviceOf(cmd);

      if (dev->active() && bindPoint == VK_PIPELINE_BIND_POINT_GRAPHICS) {
        CommandState* st = commandState(*dev, cmd);
        std::lock_guard lock(dev->mutex);
        uint32_t nextOffset = 0;

        for (uint32_t i = 0; i < setCount && firstSet + i < kMaxSets; i++) {
          auto& bound = st->sets[firstSet + i];
          auto it = dev->sets.find(pSets[i]);
          bound.set = it != dev->sets.end() ? it->second : nullptr;
          bound.dynamicOffsets.clear();

          // Dynamic offsets are consumed in binding-number order across the
          // bound sets.
          if (bound.set && bound.set->layout) {
            std::vector<std::pair<uint32_t, uint32_t>> dynamic;

            for (const auto& b : bound.set->layout->bindings) {
              if (isDynamicDescriptor(b.second.type))
                dynamic.emplace_back(b.first, b.second.count);
            }

            std::sort(dynamic.begin(), dynamic.end());

            for (const auto& d : dynamic) {
              for (uint32_t k = 0; k < d.second && nextOffset < dynamicOffsetCount; k++)
                bound.dynamicOffsets.push_back(pDynamicOffsets[nextOffset++]);
            }
          }
        }
      }

      dev->CmdBindDescriptorSets(cmd, bindPoint, layout, firstSet, setCount, pSets, dynamicOffsetCount, pDynamicOffsets);
    }

    std::shared_ptr<SetInfo> pushSet(DeviceData& dev, CommandState& st, VkPipelineLayout layout, uint32_t set) {
      // Push descriptors replace only the bindings they write; earlier
      // draws keep the snapshot they captured.
      auto next = std::make_shared<SetInfo>();

      if (set < kMaxSets && st.sets[set].set)
        *next = *st.sets[set].set;

      auto pl = dev.pipelineLayouts.find(layout);

      if (pl != dev.pipelineLayouts.end() && set < pl->second->sets.size())
        next->layout = pl->second->sets[set];

      return next;
    }

    VKAPI_ATTR void VKAPI_CALL CmdPushDescriptorSetKHR(VkCommandBuffer cmd, VkPipelineBindPoint bindPoint, VkPipelineLayout layout,
                                                       uint32_t set, uint32_t writeCount, const VkWriteDescriptorSet* pWrites) {
      DeviceData* dev = deviceOf(cmd);

      if (dev->active() && bindPoint == VK_PIPELINE_BIND_POINT_GRAPHICS && set < kMaxSets) {
        CommandState* st = commandState(*dev, cmd);
        std::lock_guard lock(dev->mutex);
        auto target = pushSet(*dev, *st, layout, set);
        applyWrites(*dev, writeCount, pWrites, target.get());
        st->sets[set].set = std::move(target);
        st->sets[set].dynamicOffsets.clear();
      }

      dev->CmdPushDescriptorSetKHR(cmd, bindPoint, layout, set, writeCount, pWrites);
    }

    VKAPI_ATTR void VKAPI_CALL CmdPushDescriptorSetWithTemplateKHR(VkCommandBuffer cmd, VkDescriptorUpdateTemplate t,
                                                                   VkPipelineLayout layout, uint32_t set, const void* pData) {
      DeviceData* dev = deviceOf(cmd);

      if (dev->active() && set < kMaxSets) {
        CommandState* st = commandState(*dev, cmd);
        std::lock_guard lock(dev->mutex);
        auto ti = dev->templates.find(t);

        if (ti != dev->templates.end()) {
          auto target = pushSet(*dev, *st, layout, set);
          applyTemplate(*dev, ti->second, pData, *target);
          st->sets[set].set = std::move(target);
          st->sets[set].dynamicOffsets.clear();
        }
      }

      dev->CmdPushDescriptorSetWithTemplateKHR(cmd, t, layout, set, pData);
    }

    VKAPI_ATTR void VKAPI_CALL CmdPushConstants(VkCommandBuffer cmd, VkPipelineLayout layout, VkShaderStageFlags stages,
                                                uint32_t offset, uint32_t size, const void* pValues) {
      DeviceData* dev = deviceOf(cmd);

      if (dev->active() && offset < kMaxPushConstantBytes) {
        CommandState* st = commandState(*dev, cmd);
        const uint32_t n = std::min(size, kMaxPushConstantBytes - offset);
        std::memcpy(st->push + offset, pValues, n);
        st->pushSize = std::max(st->pushSize, offset + n);
        st->pushStages |= stages;
      }

      dev->CmdPushConstants(cmd, layout, stages, offset, size, pValues);
    }

    void bindVertexBuffers(DeviceData* dev, VkCommandBuffer cmd, uint32_t first, uint32_t count, const VkBuffer* buffers,
                           const VkDeviceSize* offsets, const VkDeviceSize* sizes, const VkDeviceSize* strides) {
      CommandState* st = commandState(*dev, cmd);

      for (uint32_t i = 0; i < count && first + i < kMaxVertexBuffers; i++) {
        auto& vb = st->vertexBuffers[first + i];
        vb.buffer = buffers[i];
        vb.offset = offsets[i];
        vb.size   = sizes ? sizes[i] : VK_WHOLE_SIZE;

        if (strides)
          vb.stride = strides[i];
      }
    }

    VKAPI_ATTR void VKAPI_CALL CmdBindVertexBuffers(VkCommandBuffer cmd, uint32_t first, uint32_t count,
                                                    const VkBuffer* pBuffers, const VkDeviceSize* pOffsets) {
      DeviceData* dev = deviceOf(cmd);

      if (dev->active())
        bindVertexBuffers(dev, cmd, first, count, pBuffers, pOffsets, nullptr, nullptr);

      dev->CmdBindVertexBuffers(cmd, first, count, pBuffers, pOffsets);
    }

    VKAPI_ATTR void VKAPI_CALL CmdBindVertexBuffers2(VkCommandBuffer cmd, uint32_t first, uint32_t count, const VkBuffer* pBuffers,
                                                     const VkDeviceSize* pOffsets, const VkDeviceSize* pSizes, const VkDeviceSize* pStrides) {
      DeviceData* dev = deviceOf(cmd);

      if (dev->active())
        bindVertexBuffers(dev, cmd, first, count, pBuffers, pOffsets, pSizes, pStrides);

      dev->CmdBindVertexBuffers2(cmd, first, count, pBuffers, pOffsets, pSizes, pStrides);
    }

    VKAPI_ATTR void VKAPI_CALL CmdBindVertexBuffers2EXT(VkCommandBuffer cmd, uint32_t first, uint32_t count, const VkBuffer* pBuffers,
                                                        const VkDeviceSize* pOffsets, const VkDeviceSize* pSizes, const VkDeviceSize* pStrides) {
      DeviceData* dev = deviceOf(cmd);

      if (dev->active())
        bindVertexBuffers(dev, cmd, first, count, pBuffers, pOffsets, pSizes, pStrides);

      dev->CmdBindVertexBuffers2EXT(cmd, first, count, pBuffers, pOffsets, pSizes, pStrides);
    }

    VKAPI_ATTR void VKAPI_CALL CmdBindIndexBuffer(VkCommandBuffer cmd, VkBuffer buffer, VkDeviceSize offset, VkIndexType type) {
      DeviceData* dev = deviceOf(cmd);

      if (dev->active()) {
        CommandState* st = commandState(*dev, cmd);
        st->indexBuffer = buffer;
        st->indexOffset = offset;
        st->indexType = type;
      }

      dev->CmdBindIndexBuffer(cmd, buffer, offset, type);
    }

    // Render targets of the current subpass. Caller holds dev->mutex.
    void applySubpass(DeviceData* dev, CommandState* st, const RenderPassInfo& rp) {
      st->renderTargetCount = 0;
      st->depth = {};
      st->target0FinalLayout = VK_IMAGE_LAYOUT_UNDEFINED;

      if (st->subpass >= rp.subpasses.size())
        return;

      const auto& subpass = rp.subpasses[st->subpass];
      const auto& views = st->passViews;

      for (uint32_t a : subpass.colors) {
        if (a < views.size() && st->renderTargetCount < 8)
          st->renderTargets[st->renderTargetCount++] = attachmentOf(*dev, views[a]);
      }

      if (subpass.depth < views.size())
        st->depth = attachmentOf(*dev, views[subpass.depth]);

      if (!subpass.colors.empty() && subpass.colors[0] < rp.finalLayouts.size())
        st->target0FinalLayout = rp.finalLayouts[subpass.colors[0]];
    }

    void beginRenderPass(DeviceData* dev, VkCommandBuffer cmd, const VkRenderPassBeginInfo* info, VkSubpassContents contents) {
      CommandState* st = commandState(*dev, cmd);
      std::lock_guard lock(dev->mutex);

      st->renderTargetCount = 0;
      st->depth = {};
      st->pass = CommandState::Pass::RenderPass;
      st->passSplittable = false;
      st->renderPass = info->renderPass;
      st->framebuffer = info->framebuffer;
      st->renderArea = info->renderArea;
      st->imagelessViews.clear();
      st->passViews.clear();
      st->subpass = 0;
      st->target0FinalLayout = VK_IMAGE_LAYOUT_UNDEFINED;

      auto rp = dev->renderPasses.find(info->renderPass);
      auto fb = dev->framebuffers.find(info->framebuffer);

      if (rp == dev->renderPasses.end() || fb == dev->framebuffers.end() || rp->second.subpasses.empty())
        return;

      st->passViews = fb->second.views;

      if (auto* imageless = findChain<VkRenderPassAttachmentBeginInfo>(info->pNext, VK_STRUCTURE_TYPE_RENDER_PASS_ATTACHMENT_BEGIN_INFO)) {
        st->passViews.assign(imageless->pAttachments, imageless->pAttachments + imageless->attachmentCount);
        st->imagelessViews = st->passViews;
      }

      // Subpass 0; vkCmdNextSubpass moves on (tiled deferred passes:
      // G-buffer subpass, then lighting reading it as input attachments).
      applySubpass(dev, st, rp->second);

      // Splittable for the UI snapshot: one subpass with a resume copy,
      // draws recorded here (not in secondaries), in a primary buffer.
      st->passSplittable = rp->second.resume != VK_NULL_HANDLE && contents == VK_SUBPASS_CONTENTS_INLINE
        && st->primary && st->target0FinalLayout != VK_IMAGE_LAYOUT_UNDEFINED;
    }

    void nextSubpass(DeviceData* dev, VkCommandBuffer cmd) {
      CommandState* st = commandState(*dev, cmd);
      std::lock_guard lock(dev->mutex);

      if (st->pass != CommandState::Pass::RenderPass)
        return;

      st->subpass++;
      st->passSplittable = false;

      auto rp = dev->renderPasses.find(st->renderPass);

      if (rp != dev->renderPasses.end())
        applySubpass(dev, st, rp->second);
    }

    VKAPI_ATTR void VKAPI_CALL CmdNextSubpass(VkCommandBuffer cmd, VkSubpassContents contents) {
      DeviceData* dev = deviceOf(cmd);

      if (dev->active())
        nextSubpass(dev, cmd);

      dev->CmdNextSubpass(cmd, contents);
    }

    VKAPI_ATTR void VKAPI_CALL CmdNextSubpass2(VkCommandBuffer cmd, const VkSubpassBeginInfo* pBegin, const VkSubpassEndInfo* pEnd) {
      DeviceData* dev = deviceOf(cmd);

      if (dev->active())
        nextSubpass(dev, cmd);

      dev->CmdNextSubpass2(cmd, pBegin, pEnd);
    }

    VKAPI_ATTR void VKAPI_CALL CmdNextSubpass2KHR(VkCommandBuffer cmd, const VkSubpassBeginInfo* pBegin, const VkSubpassEndInfo* pEnd) {
      DeviceData* dev = deviceOf(cmd);

      if (dev->active())
        nextSubpass(dev, cmd);

      dev->CmdNextSubpass2KHR(cmd, pBegin, pEnd);
    }

    VKAPI_ATTR void VKAPI_CALL CmdBeginRenderPass(VkCommandBuffer cmd, const VkRenderPassBeginInfo* pInfo, VkSubpassContents contents) {
      DeviceData* dev = deviceOf(cmd);

      if (dev->active())
        beginRenderPass(dev, cmd, pInfo, contents);

      dev->CmdBeginRenderPass(cmd, pInfo, contents);
    }

    VKAPI_ATTR void VKAPI_CALL CmdBeginRenderPass2(VkCommandBuffer cmd, const VkRenderPassBeginInfo* pInfo, const VkSubpassBeginInfo* pSubpass) {
      DeviceData* dev = deviceOf(cmd);

      if (dev->active())
        beginRenderPass(dev, cmd, pInfo, pSubpass ? pSubpass->contents : VK_SUBPASS_CONTENTS_INLINE);

      dev->CmdBeginRenderPass2(cmd, pInfo, pSubpass);
    }

    VKAPI_ATTR void VKAPI_CALL CmdBeginRenderPass2KHR(VkCommandBuffer cmd, const VkRenderPassBeginInfo* pInfo, const VkSubpassBeginInfo* pSubpass) {
      DeviceData* dev = deviceOf(cmd);

      if (dev->active())
        beginRenderPass(dev, cmd, pInfo, pSubpass ? pSubpass->contents : VK_SUBPASS_CONTENTS_INLINE);

      dev->CmdBeginRenderPass2KHR(cmd, pInfo, pSubpass);
    }

    void endRenderPass(DeviceData* dev, VkCommandBuffer cmd) {
      CommandState* st = commandState(*dev, cmd);
      st->renderTargetCount = 0;
      st->depth = {};
      st->pass = CommandState::Pass::None;
      st->passSplittable = false;
    }

    // Command-stream copies of generated-commands draws (remix_vkfe_draw_
    // capture::args_copy_buffer), outside any render pass instance: the
    // culling pass's compute writes are made visible to the copy, and the
    // copies to the host.
    void flushArgsCopies(DeviceData* dev, VkCommandBuffer cmd) {
      CommandState* st = commandState(*dev, cmd);

      if (st->argsCopies.empty() || st->pass != CommandState::Pass::None || st->renderingSuspended
       || !dev->CmdCopyBuffer || !dev->CmdPipelineBarrier || !dev->CmdUpdateBuffer)
        return;

      std::vector<CommandState::ArgsCopy> copies;
      copies.swap(st->argsCopies);

      VkMemoryBarrier before = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
      before.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
      before.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
      dev->CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                              VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &before, 0, nullptr, 0, nullptr);

      for (const auto& c : copies) {
        const VkBufferCopy region = { c.srcOffset, c.dstOffset, c.size };
        dev->CmdCopyBuffer(cmd, c.src, c.dst, 1, &region);

        // The count slot is always written: Remix presets it to a sentinel,
        // so a copy that was never recorded is recognised.
        if (c.countDst) {
          if (c.countSrc) {
            const VkBufferCopy count = { c.countSrcOffset, c.countDstOffset, sizeof(uint32_t) };
            dev->CmdCopyBuffer(cmd, c.countSrc, c.countDst, 1, &count);
          } else {
            dev->CmdUpdateBuffer(cmd, c.countDst, c.countDstOffset, sizeof(uint32_t), &c.maxCount);
          }
        }
      }

      VkMemoryBarrier after = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
      after.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
      after.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
      dev->CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                              0, 1, &after, 0, nullptr, 0, nullptr);
    }

    VKAPI_ATTR void VKAPI_CALL CmdEndRenderPass(VkCommandBuffer cmd) {
      DeviceData* dev = deviceOf(cmd);

      if (dev->active())
        endRenderPass(dev, cmd);

      dev->CmdEndRenderPass(cmd);

      if (dev->active())
        flushArgsCopies(dev, cmd);
    }

    VKAPI_ATTR void VKAPI_CALL CmdEndRenderPass2(VkCommandBuffer cmd, const VkSubpassEndInfo* pInfo) {
      DeviceData* dev = deviceOf(cmd);

      if (dev->active())
        endRenderPass(dev, cmd);

      dev->CmdEndRenderPass2(cmd, pInfo);

      if (dev->active())
        flushArgsCopies(dev, cmd);
    }

    VKAPI_ATTR void VKAPI_CALL CmdEndRenderPass2KHR(VkCommandBuffer cmd, const VkSubpassEndInfo* pInfo) {
      DeviceData* dev = deviceOf(cmd);

      if (dev->active())
        endRenderPass(dev, cmd);

      dev->CmdEndRenderPass2KHR(cmd, pInfo);

      if (dev->active())
        flushArgsCopies(dev, cmd);
    }

    VKAPI_ATTR VkResult VKAPI_CALL EndCommandBuffer(VkCommandBuffer cmd) {
      DeviceData* dev = deviceOf(cmd);

      // Copies still pending: the draw's pass ended in another way (or the
      // stream was annotated outside a pass).
      if (dev->active() && commandState(*dev, cmd)->pass == CommandState::Pass::None)
        flushArgsCopies(dev, cmd);

      return dev->EndCommandBuffer(cmd);
    }

    void beginRendering(DeviceData* dev, VkCommandBuffer cmd, const VkRenderingInfo* info) {
      CommandState* st = commandState(*dev, cmd);
      std::lock_guard lock(dev->mutex);

      st->renderTargetCount = 0;
      st->depth = {};
      st->renderingSuspended = false;

      // Kept so the instance can be ended and resumed around the UI
      // snapshot copy. Instances spanning command buffers (suspend/resume)
      // or recorded in secondaries are not split.
      st->pass = CommandState::Pass::DynamicRendering;
      st->rendering = *info;
      st->rendering.pNext = nullptr;
      st->renderingColors.assign(info->pColorAttachments, info->pColorAttachments + info->colorAttachmentCount);

      for (auto& c : st->renderingColors)
        c.pNext = nullptr;

      st->renderingHasDepth = info->pDepthAttachment != nullptr;
      st->renderingHasStencil = info->pStencilAttachment != nullptr;

      if (info->pDepthAttachment) {
        st->renderingDepth = *info->pDepthAttachment;
        st->renderingDepth.pNext = nullptr;
      }

      if (info->pStencilAttachment) {
        st->renderingStencil = *info->pStencilAttachment;
        st->renderingStencil.pNext = nullptr;
      }

      constexpr VkRenderingFlags kUnsplittable = VK_RENDERING_SUSPENDING_BIT | VK_RENDERING_RESUMING_BIT
        | VK_RENDERING_CONTENTS_SECONDARY_COMMAND_BUFFERS_BIT;
      st->passSplittable = st->primary && !(info->flags & kUnsplittable) && info->colorAttachmentCount > 0
        && info->pColorAttachments[0].imageView != VK_NULL_HANDLE && info->pNext == nullptr;

      for (uint32_t i = 0; i < info->colorAttachmentCount && st->renderTargetCount < 8; i++) {
        if (info->pColorAttachments[i].imageView)
          st->renderTargets[st->renderTargetCount++] = attachmentOf(*dev, info->pColorAttachments[i].imageView);
      }

      if (info->pDepthAttachment && info->pDepthAttachment->imageView)
        st->depth = attachmentOf(*dev, info->pDepthAttachment->imageView);
    }

    VKAPI_ATTR void VKAPI_CALL CmdBeginRendering(VkCommandBuffer cmd, const VkRenderingInfo* pInfo) {
      DeviceData* dev = deviceOf(cmd);

      if (dev->active())
        beginRendering(dev, cmd, pInfo);

      dev->CmdBeginRendering(cmd, pInfo);
    }

    VKAPI_ATTR void VKAPI_CALL CmdBeginRenderingKHR(VkCommandBuffer cmd, const VkRenderingInfo* pInfo) {
      DeviceData* dev = deviceOf(cmd);

      if (dev->active())
        beginRendering(dev, cmd, pInfo);

      dev->CmdBeginRenderingKHR(cmd, pInfo);
    }

    // A suspending dynamic-rendering instance resumes later, possibly in
    // another command buffer: nothing may be recorded in between.
    void endRendering(DeviceData* dev, VkCommandBuffer cmd) {
      CommandState* st = commandState(*dev, cmd);
      st->renderingSuspended = st->pass == CommandState::Pass::DynamicRendering
                            && (st->rendering.flags & VK_RENDERING_SUSPENDING_BIT);
      endRenderPass(dev, cmd);
    }

    VKAPI_ATTR void VKAPI_CALL CmdEndRendering(VkCommandBuffer cmd) {
      DeviceData* dev = deviceOf(cmd);

      if (dev->active())
        endRendering(dev, cmd);

      dev->CmdEndRendering(cmd);

      if (dev->active())
        flushArgsCopies(dev, cmd);
    }

    VKAPI_ATTR void VKAPI_CALL CmdEndRenderingKHR(VkCommandBuffer cmd) {
      DeviceData* dev = deviceOf(cmd);

      if (dev->active())
        endRendering(dev, cmd);

      dev->CmdEndRenderingKHR(cmd);

      if (dev->active())
        flushArgsCopies(dev, cmd);
    }

    void trackViewports(CommandState* st, uint32_t first, uint32_t count, const VkViewport* pViewports, bool withCount) {
      if (first == 0 && count > 0)
        st->viewport = pViewports[0];

      for (uint32_t i = 0; i < count && first + i < CommandState::kMaxViewports; i++)
        st->viewports[first + i] = pViewports[i];

      st->viewportCount = withCount ? std::min(count, CommandState::kMaxViewports)
                                    : std::max(st->viewportCount, std::min(first + count, CommandState::kMaxViewports));
      st->viewportWithCount = withCount;
    }

    void trackScissors(CommandState* st, uint32_t first, uint32_t count, const VkRect2D* pScissors, bool withCount) {
      if (first == 0 && count > 0)
        st->scissor = pScissors[0];

      for (uint32_t i = 0; i < count && first + i < CommandState::kMaxViewports; i++)
        st->scissors[first + i] = pScissors[i];

      st->scissorCount = withCount ? std::min(count, CommandState::kMaxViewports)
                                   : std::max(st->scissorCount, std::min(first + count, CommandState::kMaxViewports));
      st->scissorWithCount = withCount;
    }

    VKAPI_ATTR void VKAPI_CALL CmdSetViewport(VkCommandBuffer cmd, uint32_t first, uint32_t count, const VkViewport* pViewports) {
      DeviceData* dev = deviceOf(cmd);

      if (dev->active())
        trackViewports(commandState(*dev, cmd), first, count, pViewports, false);

      dev->CmdSetViewport(cmd, first, count, pViewports);
    }

    VKAPI_ATTR void VKAPI_CALL CmdSetScissor(VkCommandBuffer cmd, uint32_t first, uint32_t count, const VkRect2D* pScissors) {
      DeviceData* dev = deviceOf(cmd);

      if (dev->active())
        trackScissors(commandState(*dev, cmd), first, count, pScissors, false);

      dev->CmdSetScissor(cmd, first, count, pScissors);
    }

    VKAPI_ATTR void VKAPI_CALL CmdSetViewportWithCount(VkCommandBuffer cmd, uint32_t count, const VkViewport* pViewports) {
      DeviceData* dev = deviceOf(cmd);

      if (dev->active())
        trackViewports(commandState(*dev, cmd), 0, count, pViewports, true);

      dev->CmdSetViewportWithCount(cmd, count, pViewports);
    }

    VKAPI_ATTR void VKAPI_CALL CmdSetViewportWithCountEXT(VkCommandBuffer cmd, uint32_t count, const VkViewport* pViewports) {
      DeviceData* dev = deviceOf(cmd);

      if (dev->active())
        trackViewports(commandState(*dev, cmd), 0, count, pViewports, true);

      dev->CmdSetViewportWithCountEXT(cmd, count, pViewports);
    }

    VKAPI_ATTR void VKAPI_CALL CmdSetScissorWithCount(VkCommandBuffer cmd, uint32_t count, const VkRect2D* pScissors) {
      DeviceData* dev = deviceOf(cmd);

      if (dev->active())
        trackScissors(commandState(*dev, cmd), 0, count, pScissors, true);

      dev->CmdSetScissorWithCount(cmd, count, pScissors);
    }

    VKAPI_ATTR void VKAPI_CALL CmdSetScissorWithCountEXT(VkCommandBuffer cmd, uint32_t count, const VkRect2D* pScissors) {
      DeviceData* dev = deviceOf(cmd);

      if (dev->active())
        trackScissors(commandState(*dev, cmd), 0, count, pScissors, true);

      dev->CmdSetScissorWithCountEXT(cmd, count, pScissors);
    }

    VKAPI_ATTR void VKAPI_CALL CmdSetRasterizerDiscardEnable(VkCommandBuffer cmd, VkBool32 enable) {
      DeviceData* dev = deviceOf(cmd);

      if (dev->active())
        commandState(*dev, cmd)->discardEnable = enable;

      dev->CmdSetRasterizerDiscardEnable(cmd, enable);
    }

    VKAPI_ATTR void VKAPI_CALL CmdSetRasterizerDiscardEnableEXT(VkCommandBuffer cmd, VkBool32 enable) {
      DeviceData* dev = deviceOf(cmd);

      if (dev->active())
        commandState(*dev, cmd)->discardEnable = enable;

      dev->CmdSetRasterizerDiscardEnableEXT(cmd, enable);
    }

    VKAPI_ATTR void VKAPI_CALL CmdExecuteCommands(VkCommandBuffer cmd, uint32_t count, const VkCommandBuffer* pCommandBuffers) {
      DeviceData* dev = deviceOf(cmd);

      if (dev->active()) {
        CommandState* st = commandState(*dev, cmd);
        st->secondaries.insert(st->secondaries.end(), pCommandBuffers, pCommandBuffers + count);
      }

      dev->CmdExecuteCommands(cmd, count, pCommandBuffers);
    }

    VKAPI_ATTR void VKAPI_CALL CmdBeginTransformFeedbackEXT(VkCommandBuffer cmd, uint32_t first, uint32_t count,
                                                            const VkBuffer* pCounters, const VkDeviceSize* pOffsets) {
      DeviceData* dev = deviceOf(cmd);

      if (dev->active())
        commandState(*dev, cmd)->gameTransformFeedbackActive = true;

      dev->CmdBeginTransformFeedbackEXT(cmd, first, count, pCounters, pOffsets);
    }

    VKAPI_ATTR void VKAPI_CALL CmdEndTransformFeedbackEXT(VkCommandBuffer cmd, uint32_t first, uint32_t count,
                                                          const VkBuffer* pCounters, const VkDeviceSize* pOffsets) {
      DeviceData* dev = deviceOf(cmd);

      if (dev->active())
        commandState(*dev, cmd)->gameTransformFeedbackActive = false;

      dev->CmdEndTransformFeedbackEXT(cmd, first, count, pCounters, pOffsets);
    }

    // ----------------------------------------------------------------- draws

    struct DrawArgs {
      bool         indexed = false;
      uint32_t     count = 0;
      uint32_t     instances = 1;
      uint32_t     first = 0;
      int32_t      vertexOffset = 0;
      uint32_t     firstInstance = 0;
      VkBuffer     indirect = VK_NULL_HANDLE;
      VkDeviceSize indirectOffset = 0;
      uint32_t     drawCount = 0;
      uint32_t     stride = 0;
      VkBuffer     countBuffer = VK_NULL_HANDLE;
      VkDeviceSize countOffset = 0;
      // Generated commands (vkCmdExecuteGeneratedCommandsEXT): indirect /
      // indirectOffset are command 0, stride the command stride, drawCount
      // the maximum command count.
      bool            generated = false;
      VkDeviceAddress address = 0;
      uint32_t        argsOffset = 0;
    };

    template<typename Replay>
    void captureReplay(DeviceData* dev, VkCommandBuffer cmd, CommandState* st, const remix_vkfe_draw_capture& capture,
                       VkPipeline variant, bool dynamicDiscard, Replay&& replay);

    struct UiPipeline {
      bool       candidate = false;
      VkPipeline variant = VK_NULL_HANDLE;
      bool       blended = false;
    };

    template<typename Replay>
    void uiLayerReplay(DeviceData* dev, VkCommandBuffer cmd, CommandState* st, const DrawArgs& args,
                       const UiPipeline& ui, Replay&& replay);

    struct BakePipeline {
      VkPipeline variant = VK_NULL_HANDLE;
      uint32_t   binding = 0;
      bool       viewportWithCount = false;
      bool       dynamicStride = false;
      bool       dynamicDiscard = false;
    };

    // Records `body` into a render pass instance on Remix's images (terrain
    // bake, UI layer): ends the game's instance, begins a compatible one on
    // `views` (the subpass's colour attachments in order, then depth), all
    // GENERAL, loaded and stored; clears colour attachment clearTarget when
    // >= 0; runs body; and resumes the game's instance with every
    // attachment loaded. The caller restores the pipeline and the dynamic
    // state body changed. Returns false, recording nothing, when the
    // instance cannot be split.
    template<typename Body>
    bool redirect(DeviceData* dev, VkCommandBuffer cmd, CommandState* st, const VkImageView* views, uint32_t viewCount,
                  VkExtent2D extent, int32_t clearTarget, uint64_t generation, Body&& body) {
      auto endRendering   = dev->CmdEndRendering ? dev->CmdEndRendering : dev->CmdEndRenderingKHR;
      auto beginRendering = dev->CmdBeginRendering ? dev->CmdBeginRendering : dev->CmdBeginRenderingKHR;

      if (!st->passSplittable || !dev->CmdClearAttachments || !dev->CmdPipelineBarrier
       || viewCount == 0 || viewCount > REMIX_VKFE_MAX_BAKE_VIEWS)
        return false;

      const VkRect2D area = { { 0, 0 }, extent };
      VkRenderPass bakePass = VK_NULL_HANDLE, resume = VK_NULL_HANDLE;
      VkFramebuffer framebuffer = VK_NULL_HANDLE;
      uint32_t colorCount = 0;

      if (st->pass == CommandState::Pass::RenderPass) {
        std::lock_guard lock(dev->mutex);
        auto rp = dev->renderPasses.find(st->renderPass);

        if (rp == dev->renderPasses.end() || !rp->second.bake || !rp->second.resume || !rp->second.allStored
         || rp->second.subpasses.size() != 1 || st->subpass != 0)
          return false;

        const auto& sp = rp->second.subpasses[0];
        const bool hasDepth = sp.depth != VK_ATTACHMENT_UNUSED;
        colorCount = uint32_t(sp.colors.size());

        if (colorCount + (hasDepth ? 1u : 0u) != viewCount)
          return false;

        for (uint32_t a : sp.colors) {
          if (a == VK_ATTACHMENT_UNUSED)
            return false;
        }

        bakePass = rp->second.bake;
        resume = rp->second.resume;

        // Framebuffers around Remix's views, made once per pass and views.
        // Older ones may still be used by command buffers in flight: they
        // are destroyed some frames later (QueuePresentKHR).
        if (dev->bakeGeneration != generation) {
          for (const auto& fb : dev->bakeFramebuffers)
            dev->retiredFramebuffers.emplace_back(dev->presentCount, fb.second);

          dev->bakeFramebuffers.clear();
          dev->bakeGeneration = generation;
        }

        std::vector<uint64_t> key = { uint64_t(bakePass), extent.width, extent.height };

        for (uint32_t v = 0; v < viewCount; v++)
          key.push_back(uint64_t(views[v]));

        auto cached = dev->bakeFramebuffers.find(key);

        if (cached != dev->bakeFramebuffers.end()) {
          framebuffer = cached->second;
        } else {
          std::vector<VkImageView> attachments(rp->second.formats.size(), VK_NULL_HANDLE);

          for (uint32_t c = 0; c < colorCount; c++)
            attachments[sp.colors[c]] = views[c];

          if (hasDepth)
            attachments[sp.depth] = views[colorCount];

          VkFramebufferCreateInfo fbInfo = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
          fbInfo.renderPass      = bakePass;
          fbInfo.attachmentCount = uint32_t(attachments.size());
          fbInfo.pAttachments    = attachments.data();
          fbInfo.width           = extent.width;
          fbInfo.height          = extent.height;
          fbInfo.layers          = 1;

          if (dev->CreateFramebuffer(dev->device, &fbInfo, nullptr, &framebuffer) != VK_SUCCESS)
            return false;

          dev->bakeFramebuffers[key] = framebuffer;
        }
      } else {
        if (!endRendering || !beginRendering || st->rendering.viewMask)
          return false;

        const bool hasDepth = st->renderingHasDepth || st->renderingHasStencil;
        colorCount = uint32_t(st->renderingColors.size());

        if (colorCount + (hasDepth ? 1u : 0u) != viewCount)
          return false;

        auto keeps = [](const VkRenderingAttachmentInfo& a) {
          return (a.storeOp == VK_ATTACHMENT_STORE_OP_STORE || a.storeOp == VK_ATTACHMENT_STORE_OP_NONE)
              && a.resolveMode == VK_RESOLVE_MODE_NONE;
        };

        for (const auto& c : st->renderingColors) {
          if (c.imageView && !keeps(c))
            return false;
        }

        if ((st->renderingHasDepth && st->renderingDepth.imageView && !keeps(st->renderingDepth))
         || (st->renderingHasStencil && st->renderingStencil.imageView && !keeps(st->renderingStencil)))
          return false;
      }

      // 1. End the game's instance; its attachments are stored.
      if (st->pass == CommandState::Pass::RenderPass)
        dev->CmdEndRenderPass(cmd);
      else
        endRendering(cmd);

      // 2. Earlier redirects of this frame wrote the same images.
      VkMemoryBarrier barrier = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
      barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
      barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT
                            | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

      constexpr VkPipelineStageFlags kStages = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
        | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;

      dev->CmdPipelineBarrier(cmd, kStages, kStages, 0, 1, &barrier, 0, nullptr, 0, nullptr);

      // 3. The instance on Remix's images.
      std::vector<VkRenderingAttachmentInfo> bakeColors;
      VkRenderingAttachmentInfo bakeDepth = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };

      if (st->pass == CommandState::Pass::RenderPass) {
        VkRenderPassBeginInfo begin = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
        begin.renderPass  = bakePass;
        begin.framebuffer = framebuffer;
        begin.renderArea  = area;
        dev->CmdBeginRenderPass(cmd, &begin, VK_SUBPASS_CONTENTS_INLINE);
      } else {
        for (uint32_t c = 0; c < colorCount; c++) {
          VkRenderingAttachmentInfo a = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
          a.imageView   = st->renderingColors[c].imageView ? views[c] : VK_NULL_HANDLE;
          a.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
          a.loadOp      = VK_ATTACHMENT_LOAD_OP_LOAD;
          a.storeOp     = VK_ATTACHMENT_STORE_OP_STORE;
          bakeColors.push_back(a);
        }

        bakeDepth.imageView   = views[colorCount < viewCount ? colorCount : 0];
        bakeDepth.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        bakeDepth.loadOp      = VK_ATTACHMENT_LOAD_OP_LOAD;
        bakeDepth.storeOp     = VK_ATTACHMENT_STORE_OP_STORE;

        VkRenderingInfo info = { VK_STRUCTURE_TYPE_RENDERING_INFO };
        info.renderArea           = area;
        info.layerCount           = 1;
        info.colorAttachmentCount = uint32_t(bakeColors.size());
        info.pColorAttachments    = bakeColors.data();
        info.pDepthAttachment     = st->renderingHasDepth ? &bakeDepth : nullptr;
        info.pStencilAttachment   = st->renderingHasStencil ? &bakeDepth : nullptr;
        beginRendering(cmd, &info);
      }

      if (clearTarget >= 0 && uint32_t(clearTarget) < colorCount) {
        VkClearAttachment clear = {};
        clear.aspectMask      = VK_IMAGE_ASPECT_COLOR_BIT;
        clear.colorAttachment = uint32_t(clearTarget);

        VkClearRect rect = { area, 0, 1 };
        dev->CmdClearAttachments(cmd, 1, &clear, 1, &rect);
      }

      // 4. The caller's draws.
      body();

      // 5. End it and resume the game's instance with everything loaded.
      if (st->pass == CommandState::Pass::RenderPass) {
        dev->CmdEndRenderPass(cmd);

        VkRenderPassAttachmentBeginInfo imageless = { VK_STRUCTURE_TYPE_RENDER_PASS_ATTACHMENT_BEGIN_INFO };
        imageless.attachmentCount = uint32_t(st->imagelessViews.size());
        imageless.pAttachments    = st->imagelessViews.data();

        VkRenderPassBeginInfo begin = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
        begin.pNext       = st->imagelessViews.empty() ? nullptr : &imageless;
        begin.renderPass  = resume;
        begin.framebuffer = st->framebuffer;
        begin.renderArea  = st->renderArea;
        dev->CmdBeginRenderPass(cmd, &begin, VK_SUBPASS_CONTENTS_INLINE);
      } else {
        endRendering(cmd);

        std::vector<VkRenderingAttachmentInfo> colors = st->renderingColors;

        for (auto& c : colors)
          c.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;

        VkRenderingAttachmentInfo depth = st->renderingDepth;
        VkRenderingAttachmentInfo stencil = st->renderingStencil;
        depth.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        stencil.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;

        VkRenderingInfo info = st->rendering;
        info.colorAttachmentCount = uint32_t(colors.size());
        info.pColorAttachments    = colors.data();
        info.pDepthAttachment     = st->renderingHasDepth ? &depth : nullptr;
        info.pStencilAttachment   = st->renderingHasStencil ? &stencil : nullptr;
        beginRendering(cmd, &info);
      }

      return true;
    }

    // Terrain bake (remix_vkfe_draw_capture::bake): one replay of the draw
    // per cascade, with that cascade's matrix and tile, into Remix's cascade
    // images. Returns whether it was recorded.
    template<typename Replay>
    bool recordBake(DeviceData* dev, VkCommandBuffer cmd, CommandState* st, const BakePipeline& bake,
                    const remix_vkfe_draw_capture& capture, Replay&& replay) {
      auto setViewportN = dev->CmdSetViewportWithCount ? dev->CmdSetViewportWithCount : dev->CmdSetViewportWithCountEXT;
      auto setScissorN  = dev->CmdSetScissorWithCount ? dev->CmdSetScissorWithCount : dev->CmdSetScissorWithCountEXT;
      auto bindVertex2  = dev->CmdBindVertexBuffers2 ? dev->CmdBindVertexBuffers2 : dev->CmdBindVertexBuffers2EXT;
      auto setDiscard   = dev->CmdSetRasterizerDiscardEnable ? dev->CmdSetRasterizerDiscardEnable : dev->CmdSetRasterizerDiscardEnableEXT;

      if (!bake.variant || !capture.bake_matrix_buffer || capture.bake_cascade_count == 0
       || capture.bake_cascade_count > REMIX_VKFE_MAX_BAKE_CASCADES
       || (bake.viewportWithCount && (!setViewportN || !setScissorN))
       || (bake.dynamicStride && !bindVertex2)
       || (bake.dynamicDiscard && !setDiscard))
        return false;

      const int32_t clearTarget = capture.bake_clear ? int32_t(capture.bake_target) : -1;

      const bool recorded = redirect(dev, cmd, st, capture.bake_views, capture.bake_view_count, capture.bake_extent,
                                     clearTarget, capture.bake_generation, [&] {
        dev->CmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, bake.variant);

        if (bake.dynamicDiscard)
          setDiscard(cmd, VK_FALSE);

        for (uint32_t c = 0; c < capture.bake_cascade_count; c++) {
          if (bake.viewportWithCount) {
            setViewportN(cmd, 1, &capture.bake_viewports[c]);
            setScissorN(cmd, 1, &capture.bake_scissors[c]);
          } else {
            dev->CmdSetViewport(cmd, 0, 1, &capture.bake_viewports[c]);
            dev->CmdSetScissor(cmd, 0, 1, &capture.bake_scissors[c]);
          }

          const VkDeviceSize offset = capture.bake_matrix_offsets[c];

          if (bake.dynamicStride) {
            const VkDeviceSize size = 64, stride = 0;
            bindVertex2(cmd, bake.binding, 1, &capture.bake_matrix_buffer, &offset, &size, &stride);
          } else {
            dev->CmdBindVertexBuffers(cmd, bake.binding, 1, &capture.bake_matrix_buffer, &offset);
          }

          replay();
        }
      });

      if (!recorded)
        return false;

      // The game's pipeline and the state the bake changed.
      dev->CmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, st->pipeline);

      if (bake.dynamicDiscard)
        setDiscard(cmd, st->discardEnable);

      if (st->viewportCount) {
        if (st->viewportWithCount && setViewportN)
          setViewportN(cmd, st->viewportCount, st->viewports);
        else
          dev->CmdSetViewport(cmd, 0, st->viewportCount, st->viewports);
      }

      if (st->scissorCount) {
        if (st->scissorWithCount && setScissorN)
          setScissorN(cmd, st->scissorCount, st->scissors);
        else
          dev->CmdSetScissor(cmd, 0, st->scissorCount, st->scissors);
      }

      const auto& vb = st->vertexBuffers[bake.binding];

      if (vb.buffer) {
        if (bake.dynamicStride) {
          bindVertex2(cmd, bake.binding, 1, &vb.buffer, &vb.offset, &vb.size, &vb.stride);
        } else {
          dev->CmdBindVertexBuffers(cmd, bake.binding, 1, &vb.buffer, &vb.offset);
        }
      }

      return true;
    }

    // Reports the draw just recorded to Remix and, when asked, records the
    // capture replay right after it. `replay` re-issues the game's draw.
    template<typename Replay>
    void reportDraw(DeviceData* dev, VkCommandBuffer cmd, const DrawArgs& args, Replay&& replay) {
      const remix_vkfe_api* api = remixApi();
      CommandState* st = commandState(*dev, cmd);

      // An annotation belongs to exactly one draw.
      const bool annotated = st->annotated;
      st->annotated = false;

      if (!st->pipeline)
        return;

      remix_vkfe_draw_desc desc = {};
      // Per-thread scratch, filled for this draw and copied by Remix during
      // on_draw: no allocation per draw once they have grown.
      thread_local std::vector<remix_vkfe_vertex_buffer> vbsScratch;
      thread_local std::vector<remix_vkfe_binding> bindingsScratch;
      thread_local std::vector<uint32_t> dynamicScratch;
      std::vector<remix_vkfe_vertex_buffer>& vbs = vbsScratch;
      std::vector<remix_vkfe_binding>& bindings = bindingsScratch;
      vbs.clear();
      bindings.clear();
      VkPipeline variant = VK_NULL_HANDLE;
      bool dynamicDiscard = false;
      BakePipeline bake;
      UiPipeline ui;

      {
        std::lock_guard lock(dev->mutex);

        auto pipe = dev->pipelines.find(st->pipeline);

        if (pipe == dev->pipelines.end())
          return;

        variant = args.generated ? pipe->second.indirectVariant : pipe->second.variant;
        dynamicDiscard = pipe->second.dynamicDiscard;

        bake.variant           = pipe->second.bakeVariant;
        bake.binding           = pipe->second.bakeBinding;
        bake.viewportWithCount = pipe->second.bakeViewportWithCount;
        bake.dynamicStride     = pipe->second.bakeDynamicStride;
        bake.dynamicDiscard    = pipe->second.dynamicDiscard;

        ui.candidate = pipe->second.uiCandidate;
        ui.variant   = pipe->second.uiVariant;
        ui.blended   = pipe->second.blend;

        desc.pipeline_key          = pipe->second.key;
        desc.command_buffer        = cmd;
        desc.indexed               = args.indexed;
        desc.vertex_or_index_count = args.count;
        desc.instance_count        = args.instances;
        desc.first_vertex_or_index = args.first;
        desc.vertex_offset         = args.vertexOffset;
        desc.first_instance        = args.firstInstance;
        desc.indirect_buffer       = args.indirect;
        desc.indirect_offset       = args.indirectOffset;
        desc.indirect_draw_count   = args.drawCount;
        desc.indirect_stride       = args.stride;
        desc.indirect_count_buffer = args.countBuffer;
        desc.indirect_count_offset = args.countOffset;
        desc.generated_commands    = args.generated ? 1u : 0u;
        desc.indirect_address      = args.address;
        desc.indirect_args_offset  = args.argsOffset;

        if (args.indexed) {
          desc.index_buffer = st->indexBuffer;
          desc.index_offset = st->indexOffset;
          desc.index_type   = st->indexType;
        }

        for (uint32_t i = 0; i < kMaxVertexBuffers; i++) {
          const auto& vb = st->vertexBuffers[i];

          if (!vb.buffer)
            continue;

          remix_vkfe_vertex_buffer v = {};
          v.binding = i;
          v.buffer  = vb.buffer;
          v.offset  = vb.offset;
          v.size    = vb.size;
          v.stride  = uint32_t(vb.stride);
          vbs.push_back(v);
        }

        constexpr size_t kMaxBindings = 512;

        // DX12 through vkd3d-proton: its descriptor sets are one bindless
        // heap; the D3D12 view of the bindings came with the annotation.
        // Texture entries carry only the VkImageView: resolve the image.
        if (annotated) {
          for (const remix_vkfe_binding& a : st->annotation) {
            remix_vkfe_binding b = a;

            if (b.kind == REMIX_VKFE_BINDING_TEXTURE && b.view) {
              Descriptor d;
              d.type   = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
              d.view   = b.view;
              d.layout = b.layout;
              fillTextureBinding(*dev, d, b);
              b.stage_mask = a.stage_mask;
              b.slot       = a.slot;
              b.space      = a.space;
            }

            // Upload-heap constant / structured buffers: read at submission
            // (D3D12 applications may write them after recording, before
            // ExecuteCommandLists). Root constants and shadowed default-heap
            // bytes are per-draw state and stay as recorded.
            const bool buffer = b.kind == REMIX_VKFE_BINDING_CONSTANT_BUFFER || b.kind == REMIX_VKFE_BINDING_STORAGE_BUFFER;

            if (buffer && b.host_data && b.size && !b.host_data_per_draw && api->on_submit_bytes) {
              CommandState::SubmitRead r;
              r.draw    = st->reportedDraws;
              r.binding = uint32_t(bindings.size());
              r.size    = b.size;
              r.host    = b.host_data;
              st->submitReads.push_back(r);
              b.host_data = nullptr;
              b.host_data_at_submit = 1;
            }

            bindings.push_back(b);
          }
        }

        for (uint32_t s = 0; s < kMaxSets && !annotated && bindings.size() < kMaxBindings; s++) {
          const auto& bound = st->sets[s];

          if (!bound.set)
            continue;

          // Dynamic offset index per (binding, element), in binding order.
          std::vector<uint32_t>& dynamicBindings = dynamicScratch;
          dynamicBindings.clear();

          if (bound.set->layout) {
            for (const auto& b : bound.set->layout->bindings) {
              if (isDynamicDescriptor(b.second.type))
                dynamicBindings.push_back(b.first);
            }

            std::sort(dynamicBindings.begin(), dynamicBindings.end());
          }

          for (const auto& entry : bound.set->bindings) {
            VkShaderStageFlags stages = VK_SHADER_STAGE_ALL_GRAPHICS;
            uint32_t dynamicBase = ~0u;

            if (bound.set->layout) {
              auto lb = bound.set->layout->bindings.find(entry.first);

              if (lb != bound.set->layout->bindings.end())
                stages = lb->second.stages;

              uint32_t base = 0;

              for (uint32_t db : dynamicBindings) {
                if (db == entry.first) {
                  dynamicBase = base;
                  break;
                }

                base += bound.set->layout->bindings[db].count;
              }
            }

            for (uint32_t e = 0; e < entry.second.size() && bindings.size() < kMaxBindings; e++) {
              const Descriptor& d = entry.second[e];

              if (d.type == VK_DESCRIPTOR_TYPE_MAX_ENUM)
                continue;

              remix_vkfe_binding b = {};
              b.stage_mask    = stageMaskOf(stages);
              b.slot          = entry.first;
              b.space         = s;
              b.array_element = e;

              if (d.buffer) {
                uint32_t dyn = 0;

                if (isDynamicDescriptor(d.type) && dynamicBase != ~0u && dynamicBase + e < bound.dynamicOffsets.size())
                  dyn = bound.dynamicOffsets[dynamicBase + e];

                fillBufferBinding(*dev, d, dyn, b);

                // Mapped memory: read at submission, when the application
                // has written what this submission's GPU work will see.
                if (b.host_data && !b.host_data_per_draw && api->on_submit_bytes) {
                  st->submitReads.push_back({ st->reportedDraws, uint32_t(bindings.size()), b.buffer, b.offset, b.size });
                  b.host_data = nullptr;
                  b.host_data_at_submit = 1;
                }
              } else if (d.view) {
                fillTextureBinding(*dev, d, b);
              } else {
                continue;
              }

              bindings.push_back(b);
            }
          }
        }

        desc.render_target_count = st->renderTargetCount;
        std::memcpy(desc.render_targets, st->renderTargets, sizeof(desc.render_targets));
        desc.depth    = st->depth;
        desc.viewport = st->viewport;
        desc.scissor  = st->scissor;
      }

      if (st->pushSize) {
        remix_vkfe_binding b = {};
        b.kind       = REMIX_VKFE_BINDING_PUSH_CONSTANTS;
        b.stage_mask = stageMaskOf(st->pushStages);
        b.size       = st->pushSize;
        b.host_data  = st->push;
        bindings.push_back(b);
      }

      desc.vertex_buffer_count = uint32_t(vbs.size());
      desc.vertex_buffers      = vbs.data();
      desc.binding_count       = uint32_t(bindings.size());
      desc.bindings            = bindings.data();

      remix_vkfe_draw_capture capture = {};
      api->on_draw(dev->remix, &desc, &capture);
      st->reportedDraws++;

      captureReplay(dev, cmd, st, capture, variant, dynamicDiscard, replay);

      // GPU-only buffer bytes Remix reads back, after the render pass.
      for (uint32_t i = 0; i < std::min(capture.buffer_copy_count, 2u); i++) {
        const auto& c = capture.buffer_copies[i];

        if (!c.src || !c.dst || !c.size)
          continue;

        CommandState::ArgsCopy copy;
        copy.src       = c.src;
        copy.srcOffset = c.src_offset;
        copy.size      = c.size;
        copy.dst       = c.dst;
        copy.dstOffset = c.dst_offset;
        st->argsCopies.push_back(copy);
      }

      // Generated commands: Remix splits the capture per command from a copy
      // of the stream, recorded once the render pass instance has ended.
      if (args.generated) {
        if (capture.capture && capture.args_copy_buffer && capture.args_copy_size) {
          CommandState::ArgsCopy copy;
          copy.maxCount       = args.drawCount;
          copy.src            = args.indirect;
          copy.srcOffset      = args.indirectOffset;
          copy.size           = capture.args_copy_size;
          copy.dst            = capture.args_copy_buffer;
          copy.dstOffset      = capture.args_copy_offset;
          copy.countSrc       = args.countBuffer;
          copy.countSrcOffset = args.countOffset;
          copy.countDst       = capture.count_copy_buffer;
          copy.countDstOffset = capture.count_copy_offset;
          st->argsCopies.push_back(copy);
        }

        return;
      }

      // Terrain bake after the capture (both replay the same draw).
      if (capture.bake && api->on_bake && !st->gameTransformFeedbackActive && recordBake(dev, cmd, st, bake, capture, replay))
        api->on_bake(dev->remix, cmd);

      if (ui.candidate)
        uiLayerReplay(dev, cmd, st, args, ui, replay);
    }

    // UI layer (remix_vkfe_ui_layer): a UI draw into the composed frame is
    // replayed into Remix's layer with the UI variant.
    template<typename Replay>
    void uiLayerReplay(DeviceData* dev, VkCommandBuffer cmd, CommandState* st, const DrawArgs& args,
                       const UiPipeline& ui, Replay&& replay) {
      const remix_vkfe_api* api = remixApi();

      if (!api->get_ui_layer || !api->on_ui_layer || st->pass == CommandState::Pass::None || st->renderTargetCount != 1)
        return;

      const remix_vkfe_attachment target = st->renderTargets[0];
      bool composedFrame = false;

      {
        std::lock_guard lock(dev->mutex);

        for (const auto& sc : dev->swapchains) {
          composedFrame |= sc.second.extent.width == target.extent.width && sc.second.extent.height == target.extent.height
                        && sc.second.format == target.format;
        }
      }

      // Opaque full-screen passes are post-processing, not UI.
      const bool fullscreenPass = !args.indirect && args.instances <= 1 && args.count >= 3 && args.count <= 6 && !ui.blended;

      if (!composedFrame || fullscreenPass)
        return;

      // A dynamic-rendering depth attachment without a view cannot be
      // matched on Remix's side.
      const bool depthAttached = st->depth.image != VK_NULL_HANDLE;
      const bool depthUnknown = (st->renderingHasDepth || st->renderingHasStencil) && !depthAttached;

      remix_vkfe_ui_layer layer = {};

      if (!ui.variant || depthUnknown || !st->primary
       || api->get_ui_layer(dev->remix, cmd, target.format, depthAttached ? st->depth.format : VK_FORMAT_UNDEFINED,
                            VkExtent2D{ target.extent.width, target.extent.height }, &layer) != REMIX_VKFE_OK
       || !layer.color_view || (depthAttached && !layer.depth_view)) {
        api->on_ui_layer(dev->remix, cmd, 0);
        return;
      }

      const VkImageView views[2] = { layer.color_view, layer.depth_view };

      const bool recorded = redirect(dev, cmd, st, views, depthAttached ? 2u : 1u, layer.extent,
                                     layer.clear ? 0 : -1, layer.generation, [&] {
        dev->CmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, ui.variant);
        replay();
      });

      if (recorded)
        dev->CmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, st->pipeline);

      api->on_ui_layer(dev->remix, cmd, recorded ? 1u : 0u);
    }

    // Inline capture replay: the variant writes the post-VS vertices of the
    // same draw, with the same state, into Remix's buffer. The game's own
    // transform feedback, if active, takes precedence.
    template<typename Replay>
    void captureReplay(DeviceData* dev, VkCommandBuffer cmd, CommandState* st, const remix_vkfe_draw_capture& capture,
                       VkPipeline variant, bool dynamicDiscard, Replay&& replay) {
      if (!capture.capture || !variant || st->gameTransformFeedbackActive
       || !dev->CmdBindTransformFeedbackBuffersEXT || !dev->CmdBeginTransformFeedbackEXT || !dev->CmdEndTransformFeedbackEXT)
        return;

      // Dynamic discard: the variant keeps it dynamic (binding it leaves
      // the game's value intact); turn it on for the replay, then put the
      // game's value back.
      PFN_vkCmdSetRasterizerDiscardEnable setDiscard = dev->CmdSetRasterizerDiscardEnable
        ? dev->CmdSetRasterizerDiscardEnable : dev->CmdSetRasterizerDiscardEnableEXT;

      if (dynamicDiscard && !setDiscard)
        return;

      VkDeviceSize offset = capture.offset;
      VkDeviceSize size   = capture.size;

      dev->CmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, variant);

      if (dynamicDiscard)
        setDiscard(cmd, VK_TRUE);

      dev->CmdBindTransformFeedbackBuffersEXT(cmd, 0, 1, &capture.buffer, &offset, &size);
      dev->CmdBeginTransformFeedbackEXT(cmd, 0, 0, nullptr, nullptr);
      replay();

      if (capture.counter_buffer) {
        VkDeviceSize counterOffset = capture.counter_offset;
        dev->CmdEndTransformFeedbackEXT(cmd, 0, 1, &capture.counter_buffer, &counterOffset);
      } else {
        dev->CmdEndTransformFeedbackEXT(cmd, 0, 0, nullptr, nullptr);
      }

      dev->CmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, st->pipeline);

      if (dynamicDiscard)
        setDiscard(cmd, st->discardEnable);
    }

    // UI preservation (remix_vkfe_ui_snapshot): after a full-screen draw into
    // an image the size of the swap chain - the final post-process pass, the
    // frame as it is before the HUD - end the render pass, copy the image
    // into Remix's snapshot, and resume the pass with its contents loaded.
    // A later full-screen pass of the frame overwrites the snapshot, so the
    // last one before the UI is what Remix composites with.
    void snapshotBeforeUi(DeviceData* dev, VkCommandBuffer cmd, const DrawArgs& args) {
      CommandState* st = commandState(*dev, cmd);

      if (st->pass == CommandState::Pass::None || !st->renderTargetCount
       || st->depth.image || st->renderingHasDepth || st->renderingHasStencil)
        return;

      const remix_vkfe_attachment target = st->renderTargets[0];
      bool blended = false;
      bool swapchainSized = false;

      {
        std::lock_guard lock(dev->mutex);

        auto pipe = dev->pipelines.find(st->pipeline);
        blended = pipe != dev->pipelines.end() && pipe->second.blend;

        // Same size and encoding as a swap chain: the image the frame is
        // composed in (a linear / HDR intermediate would compare as a
        // different frame everywhere).
        for (const auto& sc : dev->swapchains) {
          swapchainSized |= sc.second.extent.width == target.extent.width && sc.second.extent.height == target.extent.height
                         && sc.second.format == target.format;
        }
      }

      if (!swapchainSized)
        return;

      const bool fullscreen = !args.indirect && args.instances <= 1 && args.count >= 3 && args.count <= 6 && !blended;

      // Anything else drawn into the composed frame after the snapshot is
      // UI: stop taking snapshots for this frame, so the present blit or a
      // copy of the finished frame cannot overwrite the pre-UI one.
      if (!fullscreen) {
        if (dev->snapshotTaken.load())
          dev->uiStarted.store(true);
        return;
      }

      if (dev->uiStarted.load() || !st->passSplittable)
        return;

      VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
      VkRenderPass resume = VK_NULL_HANDLE;

      {
        std::lock_guard lock(dev->mutex);

        auto img = dev->images.find(target.image);

        if (img == dev->images.end() || img->second.samples != VK_SAMPLE_COUNT_1_BIT || !img->second.transferSrc)
          return;

        if (st->pass == CommandState::Pass::RenderPass) {
          auto rp = dev->renderPasses.find(st->renderPass);
          resume = rp != dev->renderPasses.end() ? rp->second.resume : VK_NULL_HANDLE;
          layout = st->target0FinalLayout;
        } else if (!st->renderingColors.empty() && st->renderingColors[0].imageView == target.view) {
          layout = st->renderingColors[0].imageLayout;
        }
      }

      if (layout == VK_IMAGE_LAYOUT_UNDEFINED
       || (st->pass == CommandState::Pass::RenderPass && !resume)
       || !dev->CmdPipelineBarrier || !dev->CmdCopyImage)
        return;

      auto endRendering   = dev->CmdEndRendering ? dev->CmdEndRendering : dev->CmdEndRenderingKHR;
      auto beginRendering = dev->CmdBeginRendering ? dev->CmdBeginRendering : dev->CmdBeginRenderingKHR;

      if (st->pass == CommandState::Pass::DynamicRendering && (!endRendering || !beginRendering))
        return;

      remix_vkfe_ui_snapshot snapshot = {};

      if (remixApi()->get_ui_snapshot(dev->remix, cmd, target.format,
            VkExtent2D{ target.extent.width, target.extent.height }, &snapshot) != REMIX_VKFE_OK || !snapshot.image)
        return;

      // 1. End the instance: attachments are stored and in `layout`.
      if (st->pass == CommandState::Pass::RenderPass)
        dev->CmdEndRenderPass(cmd);
      else
        endRendering(cmd);

      // 2. Copy the target into the snapshot.
      VkImageMemoryBarrier toCopy[2] = {};
      toCopy[0].sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
      toCopy[0].srcAccessMask       = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
      toCopy[0].dstAccessMask       = VK_ACCESS_TRANSFER_READ_BIT;
      toCopy[0].oldLayout           = layout;
      toCopy[0].newLayout           = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
      toCopy[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      toCopy[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      toCopy[0].image               = target.image;
      toCopy[0].subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, target.base_layer, 1 };

      toCopy[1] = toCopy[0];
      toCopy[1].srcAccessMask    = 0;
      toCopy[1].dstAccessMask    = VK_ACCESS_TRANSFER_WRITE_BIT;
      toCopy[1].oldLayout        = snapshot.layout;
      toCopy[1].newLayout        = snapshot.layout;
      toCopy[1].image            = snapshot.image;
      toCopy[1].subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

      dev->CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 0, nullptr, 0, nullptr, 2, toCopy);

      VkImageCopy region = {};
      region.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, target.base_layer, 1 };
      region.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
      region.extent         = { target.extent.width, target.extent.height, 1 };

      dev->CmdCopyImage(cmd, target.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        snapshot.image, snapshot.layout, 1, &region);

      // 3. Back to the layout the instance left the target in.
      VkImageMemoryBarrier back = toCopy[0];
      back.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
      back.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
      back.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
      back.newLayout     = layout;

      dev->CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        0, 0, nullptr, 0, nullptr, 1, &back);

      // 4. Resume with every attachment loaded. Bound pipeline, descriptors
      // and dynamic state are command buffer state and carry over; the
      // resume pass is compatible with the original.
      if (st->pass == CommandState::Pass::RenderPass) {
        VkRenderPassAttachmentBeginInfo imageless = { VK_STRUCTURE_TYPE_RENDER_PASS_ATTACHMENT_BEGIN_INFO };
        imageless.attachmentCount = uint32_t(st->imagelessViews.size());
        imageless.pAttachments    = st->imagelessViews.data();

        VkRenderPassBeginInfo begin = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
        begin.pNext       = st->imagelessViews.empty() ? nullptr : &imageless;
        begin.renderPass  = resume;
        begin.framebuffer = st->framebuffer;
        begin.renderArea  = st->renderArea;
        dev->CmdBeginRenderPass(cmd, &begin, VK_SUBPASS_CONTENTS_INLINE);
      } else {
        std::vector<VkRenderingAttachmentInfo> colors = st->renderingColors;

        for (auto& c : colors)
          c.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;

        VkRenderingInfo info = st->rendering;
        info.colorAttachmentCount = uint32_t(colors.size());
        info.pColorAttachments    = colors.data();
        info.pDepthAttachment     = nullptr;
        info.pStencilAttachment   = nullptr;
        beginRendering(cmd, &info);
      }

      remixApi()->on_ui_snapshot(dev->remix, cmd);
      dev->snapshotTaken.store(true);
    }

    VKAPI_ATTR void VKAPI_CALL CmdDraw(VkCommandBuffer cmd, uint32_t vertexCount, uint32_t instanceCount,
                                       uint32_t firstVertex, uint32_t firstInstance) {
      DeviceData* dev = deviceOf(cmd);
      dev->CmdDraw(cmd, vertexCount, instanceCount, firstVertex, firstInstance);

      if (!dev->active())
        return;

      DrawArgs args;
      args.count = vertexCount;
      args.instances = instanceCount;
      args.first = firstVertex;
      args.firstInstance = firstInstance;

      reportDraw(dev, cmd, args, [&] {
        dev->CmdDraw(cmd, vertexCount, instanceCount, firstVertex, firstInstance);
      });

      snapshotBeforeUi(dev, cmd, args);
    }

    VKAPI_ATTR void VKAPI_CALL CmdDrawIndexed(VkCommandBuffer cmd, uint32_t indexCount, uint32_t instanceCount,
                                              uint32_t firstIndex, int32_t vertexOffset, uint32_t firstInstance) {
      DeviceData* dev = deviceOf(cmd);
      dev->CmdDrawIndexed(cmd, indexCount, instanceCount, firstIndex, vertexOffset, firstInstance);

      if (!dev->active())
        return;

      DrawArgs args;
      args.indexed = true;
      args.count = indexCount;
      args.instances = instanceCount;
      args.first = firstIndex;
      args.vertexOffset = vertexOffset;
      args.firstInstance = firstInstance;

      reportDraw(dev, cmd, args, [&] {
        dev->CmdDrawIndexed(cmd, indexCount, instanceCount, firstIndex, vertexOffset, firstInstance);
      });

      snapshotBeforeUi(dev, cmd, args);
    }

    VKAPI_ATTR void VKAPI_CALL CmdDrawIndirect(VkCommandBuffer cmd, VkBuffer buffer, VkDeviceSize offset,
                                               uint32_t drawCount, uint32_t stride) {
      DeviceData* dev = deviceOf(cmd);
      dev->CmdDrawIndirect(cmd, buffer, offset, drawCount, stride);

      if (!dev->active())
        return;

      DrawArgs args;
      args.indirect = buffer;
      args.indirectOffset = offset;
      args.drawCount = drawCount;
      args.stride = stride;

      reportDraw(dev, cmd, args, [&] {
        dev->CmdDrawIndirect(cmd, buffer, offset, drawCount, stride);
      });
    }

    VKAPI_ATTR void VKAPI_CALL CmdDrawIndexedIndirect(VkCommandBuffer cmd, VkBuffer buffer, VkDeviceSize offset,
                                                      uint32_t drawCount, uint32_t stride) {
      DeviceData* dev = deviceOf(cmd);
      dev->CmdDrawIndexedIndirect(cmd, buffer, offset, drawCount, stride);

      if (!dev->active())
        return;

      DrawArgs args;
      args.indexed = true;
      args.indirect = buffer;
      args.indirectOffset = offset;
      args.drawCount = drawCount;
      args.stride = stride;

      reportDraw(dev, cmd, args, [&] {
        dev->CmdDrawIndexedIndirect(cmd, buffer, offset, drawCount, stride);
      });
    }

    template<bool Indexed>
    void drawIndirectCount(PFN_vkCmdDrawIndirectCount next, VkCommandBuffer cmd, VkBuffer buffer, VkDeviceSize offset,
                           VkBuffer countBuffer, VkDeviceSize countOffset, uint32_t maxDrawCount, uint32_t stride) {
      DeviceData* dev = deviceOf(cmd);
      next(cmd, buffer, offset, countBuffer, countOffset, maxDrawCount, stride);

      if (!dev->active())
        return;

      DrawArgs args;
      args.indexed = Indexed;
      args.indirect = buffer;
      args.indirectOffset = offset;
      args.drawCount = maxDrawCount;
      args.stride = stride;
      args.countBuffer = countBuffer;
      args.countOffset = countOffset;

      reportDraw(dev, cmd, args, [&] {
        next(cmd, buffer, offset, countBuffer, countOffset, maxDrawCount, stride);
      });
    }

    VKAPI_ATTR void VKAPI_CALL CmdDrawIndirectCount(VkCommandBuffer cmd, VkBuffer buffer, VkDeviceSize offset, VkBuffer countBuffer,
                                                    VkDeviceSize countOffset, uint32_t maxDrawCount, uint32_t stride) {
      drawIndirectCount<false>(deviceOf(cmd)->CmdDrawIndirectCount, cmd, buffer, offset, countBuffer, countOffset, maxDrawCount, stride);
    }

    VKAPI_ATTR void VKAPI_CALL CmdDrawIndirectCountKHR(VkCommandBuffer cmd, VkBuffer buffer, VkDeviceSize offset, VkBuffer countBuffer,
                                                       VkDeviceSize countOffset, uint32_t maxDrawCount, uint32_t stride) {
      drawIndirectCount<false>(deviceOf(cmd)->CmdDrawIndirectCountKHR, cmd, buffer, offset, countBuffer, countOffset, maxDrawCount, stride);
    }

    VKAPI_ATTR void VKAPI_CALL CmdDrawIndexedIndirectCount(VkCommandBuffer cmd, VkBuffer buffer, VkDeviceSize offset, VkBuffer countBuffer,
                                                           VkDeviceSize countOffset, uint32_t maxDrawCount, uint32_t stride) {
      drawIndirectCount<true>(deviceOf(cmd)->CmdDrawIndexedIndirectCount, cmd, buffer, offset, countBuffer, countOffset, maxDrawCount, stride);
    }

    VKAPI_ATTR void VKAPI_CALL CmdDrawIndexedIndirectCountKHR(VkCommandBuffer cmd, VkBuffer buffer, VkDeviceSize offset, VkBuffer countBuffer,
                                                              VkDeviceSize countOffset, uint32_t maxDrawCount, uint32_t stride) {
      drawIndirectCount<true>(deviceOf(cmd)->CmdDrawIndexedIndirectCountKHR, cmd, buffer, offset, countBuffer, countOffset, maxDrawCount, stride);
    }

    // GPU-driven DX12 draws (remix_vklayer_annotate_indirect): replayed before
    // the game's vkCmdExecuteGeneratedCommandsEXT - all its state is bound
    // then - as a multi-draw indirect over the same stream with the indirect
    // capture variant, which reads each command's root constants from the
    // stream at the address pushed into the first constant range.
    void reportGenerated(DeviceData* dev, VkCommandBuffer cmd, CommandState* st) {
      const remix_vkfe_indirect_annotation a = st->indirect;
      IndirectLayout layout, expected;
      bool haveVariant = false;

      {
        std::lock_guard lock(dev->mutex);
        auto pipe = dev->pipelines.find(st->pipeline);

        if (pipe != dev->pipelines.end() && pipe->second.indirectVariant) {
          layout = pipe->second.indirectLayout;
          haveVariant = true;
        }
      }

      PFN_vkCmdDrawIndexedIndirectCount drawIndexedCount = dev->CmdDrawIndexedIndirectCount
        ? dev->CmdDrawIndexedIndirectCount : dev->CmdDrawIndexedIndirectCountKHR;
      PFN_vkCmdDrawIndirectCount drawCount = dev->CmdDrawIndirectCount
        ? dev->CmdDrawIndirectCount : dev->CmdDrawIndirectCountKHR;

      const bool usable = haveVariant && findIndirectLayout(a.layout, expected) && expected == layout
        && layout.stride == a.stride && layout.constantCount && layout.constants[0].size >= sizeof(uint64_t)
        && layout.constants[0].command_offset != REMIX_VKFE_INDIRECT_COMMAND_INDEX
        && st->pushStages && a.argument_address
        && (!a.count_buffer || (a.indexed ? drawIndexedCount != nullptr : drawCount != nullptr));

      if (!usable) {
        // The annotation of this execute's bindings stays unused.
        st->annotated = false;
        return;
      }

      DrawArgs args;
      args.indexed        = a.indexed != 0;
      args.indirect       = a.argument_buffer;
      args.indirectOffset = a.argument_offset;
      args.drawCount      = a.max_command_count;
      args.stride         = a.stride;
      args.countBuffer    = a.count_buffer;
      args.countOffset    = a.count_offset;
      args.generated      = true;
      args.address        = a.argument_address;
      args.argsOffset     = a.args_offset;

      const uint32_t pushOffset = layout.constants[0].push_offset;
      const VkShaderStageFlags stages = st->pushStages;

      reportDraw(dev, cmd, args, [&] {
        const uint64_t address = a.argument_address;
        dev->CmdPushConstants(cmd, a.layout, stages, pushOffset, sizeof(address), &address);

        const VkDeviceSize offset = a.argument_offset + a.args_offset;

        if (a.count_buffer) {
          if (a.indexed)
            drawIndexedCount(cmd, a.argument_buffer, offset, a.count_buffer, a.count_offset, a.max_command_count, a.stride);
          else
            drawCount(cmd, a.argument_buffer, offset, a.count_buffer, a.count_offset, a.max_command_count, a.stride);
        } else if (a.indexed) {
          dev->CmdDrawIndexedIndirect(cmd, a.argument_buffer, offset, a.max_command_count, a.stride);
        } else {
          dev->CmdDrawIndirect(cmd, a.argument_buffer, offset, a.max_command_count, a.stride);
        }

        // The game's bytes back (its execute sets this range per command
        // anyway, but later direct draws read what was pushed last).
        if (pushOffset + sizeof(address) <= st->pushSize)
          dev->CmdPushConstants(cmd, a.layout, stages, pushOffset, sizeof(address), st->push + pushOffset);
      });
    }

    VKAPI_ATTR void VKAPI_CALL CmdExecuteGeneratedCommandsEXT(VkCommandBuffer cmd, VkBool32 isPreprocessed,
                                                              const VkGeneratedCommandsInfoEXT* pInfo) {
      DeviceData* dev = deviceOf(cmd);

      if (dev->active()) {
        CommandState* st = commandState(*dev, cmd);
        const bool annotated = st->indirectAnnotated;
        st->indirectAnnotated = false;

        if (annotated)
          reportGenerated(dev, cmd, st);
        else
          st->annotated = false;
      }

      dev->CmdExecuteGeneratedCommandsEXT(cmd, isPreprocessed, pInfo);
    }

    // ------------------------------------------------------------- pipelines

    void describePipeline(DeviceData& dev, const VkGraphicsPipelineCreateInfo& info, uint64_t key,
                          remix_vkfe_pipeline_desc& desc,
                          std::vector<remix_vkfe_vertex_attribute>& attributes,
                          std::vector<remix_vkfe_vertex_binding>& bindings) {
      desc.key = key;

      for (uint32_t s = 0; s < info.stageCount; s++) {
        const auto& stage = info.pStages[s];
        const int index = stageIndexOf(stage.stage);

        if (index < 0)
          continue;

        const uint32_t* code = nullptr;
        size_t size = 0;

        if (stage.module) {
          auto mod = dev.shaderModules.find(stage.module);

          if (mod != dev.shaderModules.end()) {
            code = mod->second.data();
            size = mod->second.size() * sizeof(uint32_t);
          }
        } else if (auto* inl = findChain<VkShaderModuleCreateInfo>(stage.pNext, VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO)) {
          // VK_KHR_maintenance5: SPIR-V passed with the stage.
          code = inl->pCode;
          size = inl->codeSize;
        }

        desc.stages[index].format      = REMIX_VKFE_SHADER_SPIRV;
        desc.stages[index].code        = code;
        desc.stages[index].size        = size;
        desc.stages[index].entry_point = stage.pName;
      }

      if (info.pVertexInputState) {
        const auto& vi = *info.pVertexInputState;

        for (uint32_t i = 0; i < vi.vertexAttributeDescriptionCount; i++) {
          const auto& a = vi.pVertexAttributeDescriptions[i];
          remix_vkfe_vertex_attribute attr = {};
          attr.location = a.location;
          attr.binding  = a.binding;
          attr.format   = a.format;
          attr.offset   = a.offset;
          attributes.push_back(attr);
        }

        for (uint32_t i = 0; i < vi.vertexBindingDescriptionCount; i++) {
          const auto& b = vi.pVertexBindingDescriptions[i];
          remix_vkfe_vertex_binding bind = {};
          bind.binding      = b.binding;
          bind.stride       = b.stride;
          bind.per_instance = b.inputRate == VK_VERTEX_INPUT_RATE_INSTANCE;
          bind.divisor      = 1;
          bindings.push_back(bind);
        }
      }

      desc.attribute_count = uint32_t(attributes.size());
      desc.attributes      = attributes.data();
      desc.binding_count   = uint32_t(bindings.size());
      desc.bindings        = bindings.data();

      desc.topology = info.pInputAssemblyState ? info.pInputAssemblyState->topology : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

      // Topology set per draw (extended dynamic state): the vertex count
      // cannot be derived from the static value. MAX_ENUM tells Remix to
      // count captured vertices on the GPU instead.
      if (info.pDynamicState) {
        for (uint32_t i = 0; i < info.pDynamicState->dynamicStateCount; i++) {
          switch (info.pDynamicState->pDynamicStates[i]) {
            case VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY:
              desc.topology = VK_PRIMITIVE_TOPOLOGY_MAX_ENUM;
              break;

            // State the terrain bake overrides but the layer does not track,
            // so it could not put the game's value back afterwards.
            case VK_DYNAMIC_STATE_CULL_MODE:
            case VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE:
            case VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE:
            case VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE:
            case VK_DYNAMIC_STATE_DEPTH_BOUNDS_TEST_ENABLE:
            case VK_DYNAMIC_STATE_COLOR_BLEND_ENABLE_EXT:
            case VK_DYNAMIC_STATE_COLOR_WRITE_MASK_EXT:
            case VK_DYNAMIC_STATE_COLOR_WRITE_ENABLE_EXT:
            case VK_DYNAMIC_STATE_VERTEX_INPUT_EXT:
            case VK_DYNAMIC_STATE_RASTERIZATION_SAMPLES_EXT:
            case VK_DYNAMIC_STATE_POLYGON_MODE_EXT:
              desc.bake_blocking_dynamic_state = 1;
              break;

            default:
              break;
          }
        }
      }

      desc.samples = info.pMultisampleState ? info.pMultisampleState->rasterizationSamples : VK_SAMPLE_COUNT_1_BIT;

      if (info.pRasterizationState) {
        desc.cull_mode           = info.pRasterizationState->cullMode;
        desc.front_face          = info.pRasterizationState->frontFace;
        desc.depth_bias_enable   = info.pRasterizationState->depthBiasEnable;
        desc.depth_bias_constant = info.pRasterizationState->depthBiasConstantFactor;
        desc.depth_bias_slope    = info.pRasterizationState->depthBiasSlopeFactor;
      }

      if (info.pMultisampleState)
        desc.alpha_to_coverage = info.pMultisampleState->alphaToCoverageEnable;

      // No blend state: every colour channel is written.
      desc.color_write_mask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT
                            | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

      if (info.pDepthStencilState) {
        desc.depth_test    = info.pDepthStencilState->depthTestEnable;
        desc.depth_write   = info.pDepthStencilState->depthWriteEnable;
        desc.depth_compare = info.pDepthStencilState->depthCompareOp;
      }

      if (info.pColorBlendState && info.pColorBlendState->attachmentCount && info.pColorBlendState->pAttachments) {
        const auto& att = info.pColorBlendState->pAttachments[0];
        desc.blend_enable     = att.blendEnable;
        desc.src_color_blend  = att.srcColorBlendFactor;
        desc.dst_color_blend  = att.dstColorBlendFactor;
        desc.color_blend_op   = att.colorBlendOp;
        desc.src_alpha_blend  = att.srcAlphaBlendFactor;
        desc.dst_alpha_blend  = att.dstAlphaBlendFactor;
        desc.color_write_mask = att.colorWriteMask;
      }

      if (auto* rendering = findChain<VkPipelineRenderingCreateInfo>(info.pNext, VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO)) {
        desc.render_target_count = std::min(rendering->colorAttachmentCount, 8u);

        for (uint32_t i = 0; i < desc.render_target_count; i++)
          desc.render_target_formats[i] = rendering->pColorAttachmentFormats[i];

        desc.depth_format = rendering->depthAttachmentFormat;
      } else {
        auto rp = dev.renderPasses.find(info.renderPass);

        if (rp != dev.renderPasses.end() && info.subpass < rp->second.subpasses.size()) {
          const auto& sp = rp->second.subpasses[info.subpass];

          for (uint32_t a : sp.colors) {
            if (a < rp->second.formats.size() && desc.render_target_count < 8)
              desc.render_target_formats[desc.render_target_count++] = rp->second.formats[a];
          }

          if (sp.depth < rp->second.formats.size())
            desc.depth_format = rp->second.formats[sp.depth];
        }
      }
    }

    VkPipeline createCaptureVariant(DeviceData& dev, VkDevice device, VkPipelineCache cache,
                                    const VkGraphicsPipelineCreateInfo& info, const remix_vkfe_capture_plan& plan,
                                    const VkAllocationCallbacks* pAllocator) {
      // The variant needs discard on: static here, or, when the game sets
      // discard dynamically, kept dynamic (the same dynamic state list) and
      // switched on around the replay (reportDraw).
      VkShaderModuleCreateInfo moduleInfo = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
      moduleInfo.codeSize = plan.spirv_size;
      moduleInfo.pCode    = plan.spirv;

      VkShaderModule module = VK_NULL_HANDLE;

      if (dev.CreateShaderModule(device, &moduleInfo, nullptr, &module) != VK_SUCCESS)
        return VK_NULL_HANDLE;

      std::vector<VkPipelineShaderStageCreateInfo> stages(info.pStages, info.pStages + info.stageCount);

      for (auto& s : stages) {
        if (stageIndexOf(s.stage) == int(plan.stage)) {
          s.module = module;
          s.pNext  = nullptr;   // drops an inline VkShaderModuleCreateInfo
        }
      }

      VkPipelineRasterizationStateCreateInfo raster = {};

      if (info.pRasterizationState)
        raster = *info.pRasterizationState;
      else
        raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;

      raster.rasterizerDiscardEnable = VK_TRUE;

      VkGraphicsPipelineCreateInfo variantInfo = info;
      variantInfo.flags &= ~VkPipelineCreateFlags(VK_PIPELINE_CREATE_DERIVATIVE_BIT);
      variantInfo.basePipelineHandle  = VK_NULL_HANDLE;
      variantInfo.basePipelineIndex   = -1;
      variantInfo.stageCount          = uint32_t(stages.size());
      variantInfo.pStages             = stages.data();
      variantInfo.pRasterizationState = &raster;

      VkPipeline variant = VK_NULL_HANDLE;
      const VkResult result = dev.CreateGraphicsPipelines(device, cache, 1, &variantInfo, pAllocator, &variant);

      dev.DestroyShaderModule(device, module, nullptr);

      if (result != VK_SUCCESS) {
        log("capture variant creation failed (%d); draws of this pipeline stay uncaptured", result);
        return VK_NULL_HANDLE;
      }

      return variant;
    }

    // Terrain bake variant (remix_vkfe_capture_plan): the game's pipeline
    // with Remix's vertex stage, four extra vertex attributes fed from a
    // stride-0 binding (the cascade matrix), no culling, depth / stencil or
    // blending, and dynamic viewport and scissor.
    VkPipeline createBakeVariant(DeviceData& dev, VkDevice device, VkPipelineCache cache,
                                 const VkGraphicsPipelineCreateInfo& info, const remix_vkfe_capture_plan& plan,
                                 const VkAllocationCallbacks* pAllocator, PipelineInfo& pipeline) {
      if (!info.pVertexInputState || !info.pViewportState || !info.pRasterizationState)
        return VK_NULL_HANDLE;

      const VkPipelineVertexInputStateCreateInfo& vi = *info.pVertexInputState;

      // Divisors (pNext) would need extending too.
      if (vi.pNext)
        return VK_NULL_HANDLE;

      uint32_t binding = 0;

      for (uint32_t i = 0; i < vi.vertexBindingDescriptionCount; i++)
        binding = std::max(binding, vi.pVertexBindingDescriptions[i].binding + 1);

      // Sixteen bindings is the guaranteed minimum.
      if (binding >= 16)
        return VK_NULL_HANDLE;

      std::vector<VkVertexInputBindingDescription> bindings(vi.pVertexBindingDescriptions,
        vi.pVertexBindingDescriptions + vi.vertexBindingDescriptionCount);
      std::vector<VkVertexInputAttributeDescription> attributes(vi.pVertexAttributeDescriptions,
        vi.pVertexAttributeDescriptions + vi.vertexAttributeDescriptionCount);

      bindings.push_back({ binding, 0u, VK_VERTEX_INPUT_RATE_VERTEX });

      for (uint32_t r = 0; r < 4; r++)
        attributes.push_back({ plan.bake_matrix_location + r, binding, VK_FORMAT_R32G32B32A32_SFLOAT, 16u * r });

      VkPipelineVertexInputStateCreateInfo vertexInput = vi;
      vertexInput.vertexBindingDescriptionCount   = uint32_t(bindings.size());
      vertexInput.pVertexBindingDescriptions      = bindings.data();
      vertexInput.vertexAttributeDescriptionCount = uint32_t(attributes.size());
      vertexInput.pVertexAttributeDescriptions    = attributes.data();

      // Dynamic state: the game's, plus viewport and scissor.
      std::vector<VkDynamicState> dynamic;
      bool hasViewport = false, hasScissor = false, withCount = false, stride = false;

      if (info.pDynamicState) {
        dynamic.assign(info.pDynamicState->pDynamicStates,
          info.pDynamicState->pDynamicStates + info.pDynamicState->dynamicStateCount);
      }

      for (VkDynamicState s : dynamic) {
        hasViewport |= s == VK_DYNAMIC_STATE_VIEWPORT;
        hasScissor  |= s == VK_DYNAMIC_STATE_SCISSOR;
        withCount   |= s == VK_DYNAMIC_STATE_VIEWPORT_WITH_COUNT;
        stride      |= s == VK_DYNAMIC_STATE_VERTEX_INPUT_BINDING_STRIDE;
      }

      if (!withCount) {
        if (!hasViewport) dynamic.push_back(VK_DYNAMIC_STATE_VIEWPORT);
        if (!hasScissor)  dynamic.push_back(VK_DYNAMIC_STATE_SCISSOR);
      }

      VkPipelineDynamicStateCreateInfo dynamicState = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
      dynamicState.dynamicStateCount = uint32_t(dynamic.size());
      dynamicState.pDynamicStates    = dynamic.data();

      VkPipelineViewportStateCreateInfo viewport = *info.pViewportState;
      viewport.pNext = nullptr;

      if (!withCount) {
        viewport.viewportCount = 1;
        viewport.scissorCount  = 1;
        viewport.pViewports    = nullptr;
        viewport.pScissors     = nullptr;
      }

      VkPipelineRasterizationStateCreateInfo raster = *info.pRasterizationState;
      raster.cullMode                = VK_CULL_MODE_NONE;
      raster.rasterizerDiscardEnable = VK_FALSE;
      raster.polygonMode             = VK_POLYGON_MODE_FILL;

      VkPipelineDepthStencilStateCreateInfo depth = { VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
      depth.depthCompareOp = VK_COMPARE_OP_ALWAYS;

      std::vector<VkPipelineColorBlendAttachmentState> blendAttachments;
      VkPipelineColorBlendStateCreateInfo blend = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };

      if (info.pColorBlendState) {
        blend = *info.pColorBlendState;
        blend.pNext = nullptr;
        blend.logicOpEnable = VK_FALSE;
        blendAttachments.assign(info.pColorBlendState->pAttachments,
          info.pColorBlendState->pAttachments + info.pColorBlendState->attachmentCount);

        for (auto& a : blendAttachments) {
          a.blendEnable    = VK_FALSE;
          a.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT
                           | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        }

        blend.pAttachments = blendAttachments.data();
      }

      VkShaderModuleCreateInfo moduleInfo = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
      moduleInfo.codeSize = plan.bake_spirv_size;
      moduleInfo.pCode    = plan.bake_spirv;

      VkShaderModule module = VK_NULL_HANDLE;

      if (dev.CreateShaderModule(device, &moduleInfo, nullptr, &module) != VK_SUCCESS)
        return VK_NULL_HANDLE;

      std::vector<VkPipelineShaderStageCreateInfo> stages(info.pStages, info.pStages + info.stageCount);

      for (auto& s : stages) {
        if (s.stage == VK_SHADER_STAGE_VERTEX_BIT) {
          s.module = module;
          s.pNext  = nullptr;
        }
      }

      VkGraphicsPipelineCreateInfo variantInfo = info;
      variantInfo.flags &= ~VkPipelineCreateFlags(VK_PIPELINE_CREATE_DERIVATIVE_BIT);
      variantInfo.basePipelineHandle  = VK_NULL_HANDLE;
      variantInfo.basePipelineIndex   = -1;
      variantInfo.stageCount          = uint32_t(stages.size());
      variantInfo.pStages             = stages.data();
      variantInfo.pVertexInputState   = &vertexInput;
      variantInfo.pViewportState      = &viewport;
      variantInfo.pRasterizationState = &raster;
      variantInfo.pDepthStencilState  = &depth;
      variantInfo.pColorBlendState    = info.pColorBlendState ? &blend : nullptr;
      variantInfo.pDynamicState       = &dynamicState;

      VkPipeline variant = VK_NULL_HANDLE;
      const VkResult result = dev.CreateGraphicsPipelines(device, cache, 1, &variantInfo, pAllocator, &variant);

      dev.DestroyShaderModule(device, module, nullptr);

      if (result != VK_SUCCESS) {
        log("terrain bake variant creation failed (%d); this pipeline's terrain stays unbaked", result);
        return VK_NULL_HANDLE;
      }

      pipeline.bakeBinding           = binding;
      pipeline.bakeViewportWithCount = withCount;
      pipeline.bakeDynamicStride     = stride;
      return variant;
    }

    // Formats games present in (the composed frame the UI is drawn into).
    bool isSwapchainFormat(VkFormat format) {
      switch (format) {
        case VK_FORMAT_B8G8R8A8_UNORM: case VK_FORMAT_B8G8R8A8_SRGB:
        case VK_FORMAT_R8G8B8A8_UNORM: case VK_FORMAT_R8G8B8A8_SRGB:
        case VK_FORMAT_A2B10G10R10_UNORM_PACK32: case VK_FORMAT_A2R10G10B10_UNORM_PACK32:
        case VK_FORMAT_R16G16B16A16_SFLOAT:
          return true;
        default:
          return false;
      }
    }

    // UI layer variant (remix_vkfe_ui_layer): the game's pipeline with the
    // alpha channel written and blended so it accumulates coverage -
    // src ONE / dst ONE_MINUS_SRC_ALPHA under straight or premultiplied
    // alpha blending, src ZERO / dst ONE under additive blending. Opaque UI
    // writes its own alpha. Other colour blending has no layer equivalent.
    VkPipeline createUiVariant(DeviceData& dev, VkDevice device, VkPipelineCache cache,
                               const VkGraphicsPipelineCreateInfo& info, const VkAllocationCallbacks* pAllocator) {
      if (!info.pColorBlendState || info.pColorBlendState->attachmentCount != 1 || !info.pColorBlendState->pAttachments)
        return VK_NULL_HANDLE;

      VkPipelineColorBlendAttachmentState att = info.pColorBlendState->pAttachments[0];

      if (att.blendEnable) {
        const bool over = att.colorBlendOp == VK_BLEND_OP_ADD
          && (att.srcColorBlendFactor == VK_BLEND_FACTOR_SRC_ALPHA || att.srcColorBlendFactor == VK_BLEND_FACTOR_ONE)
          && att.dstColorBlendFactor == VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        const bool additive = att.colorBlendOp == VK_BLEND_OP_ADD
          && (att.srcColorBlendFactor == VK_BLEND_FACTOR_SRC_ALPHA || att.srcColorBlendFactor == VK_BLEND_FACTOR_ONE)
          && att.dstColorBlendFactor == VK_BLEND_FACTOR_ONE;

        if (over) {
          att.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
          att.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        } else if (additive) {
          att.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
          att.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        } else {
          return VK_NULL_HANDLE;
        }

        att.alphaBlendOp = VK_BLEND_OP_ADD;
      }

      // Colour as the game writes it, alpha always.
      att.colorWriteMask |= VK_COLOR_COMPONENT_A_BIT;

      VkPipelineColorBlendStateCreateInfo blend = *info.pColorBlendState;
      blend.pNext         = nullptr;
      blend.logicOpEnable = VK_FALSE;
      blend.pAttachments  = &att;

      VkGraphicsPipelineCreateInfo variantInfo = info;
      variantInfo.flags &= ~VkPipelineCreateFlags(VK_PIPELINE_CREATE_DERIVATIVE_BIT);
      variantInfo.basePipelineHandle = VK_NULL_HANDLE;
      variantInfo.basePipelineIndex  = -1;
      variantInfo.pColorBlendState   = &blend;

      VkPipeline variant = VK_NULL_HANDLE;

      if (dev.CreateGraphicsPipelines(device, cache, 1, &variantInfo, pAllocator, &variant) != VK_SUCCESS) {
        log("UI layer variant creation failed; frames with this UI use the snapshot");
        return VK_NULL_HANDLE;
      }

      return variant;
    }

    VKAPI_ATTR VkResult VKAPI_CALL CreateGraphicsPipelines(VkDevice device, VkPipelineCache cache, uint32_t count,
                                                           const VkGraphicsPipelineCreateInfo* pCreateInfos,
                                                           const VkAllocationCallbacks* pAllocator, VkPipeline* pPipelines) {
      DeviceData* dev = deviceOf(device);
      const VkResult result = dev->CreateGraphicsPipelines(device, cache, count, pCreateInfos, pAllocator, pPipelines);

      if (!dev->active() || result < 0)
        return result;

      const remix_vkfe_api* api = remixApi();

      for (uint32_t i = 0; i < count; i++) {
        const auto& info = pCreateInfos[i];

        if (!pPipelines[i] || (info.flags & VK_PIPELINE_CREATE_LIBRARY_BIT_KHR)
         || findChain<VkPipelineLibraryCreateInfoKHR>(info.pNext, VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR))
          continue;

        bool dynamicDiscard = false;

        if (info.pDynamicState) {
          for (uint32_t d = 0; d < info.pDynamicState->dynamicStateCount; d++)
            dynamicDiscard |= info.pDynamicState->pDynamicStates[d] == VK_DYNAMIC_STATE_RASTERIZER_DISCARD_ENABLE;
        }

        // A pipeline that never rasterizes draws nothing to trace. (With
        // discard dynamic, the static value is ignored.)
        if (!dynamicDiscard && info.pRasterizationState && info.pRasterizationState->rasterizerDiscardEnable)
          continue;

        remix_vkfe_pipeline_desc desc = {};
        std::vector<remix_vkfe_vertex_attribute> attributes;
        std::vector<remix_vkfe_vertex_binding> bindings;

        {
          std::lock_guard lock(dev->mutex);
          describePipeline(*dev, info, uint64_t(pPipelines[i]), desc, attributes, bindings);
        }

        // A layout a GPU-driven command stream sets push constants for:
        // Remix also returns the variant that reads them from the stream.
        IndirectLayout indirect;
        const bool indirectLayout = info.layout && findIndirectLayout(info.layout, indirect);

        if (indirectLayout) {
          desc.indirect_constant_count = indirect.constantCount;
          std::memcpy(desc.indirect_constants, indirect.constants, sizeof(desc.indirect_constants));
          desc.indirect_stride = indirect.stride;
        }

        remix_vkfe_capture_plan plan = {};
        api->on_pipeline(dev->remix, &desc, &plan);

        PipelineInfo pipeline;
        pipeline.key = uint64_t(pPipelines[i]);
        pipeline.blend = desc.blend_enable != 0;
        pipeline.dynamicDiscard = dynamicDiscard;

        if (plan.supported && plan.spirv && plan.spirv_size)
          pipeline.variant = createCaptureVariant(*dev, device, cache, info, plan, pAllocator);

        if (indirectLayout && plan.supported && plan.indirect_spirv && plan.indirect_spirv_size) {
          remix_vkfe_capture_plan indirectPlan = plan;
          indirectPlan.spirv      = plan.indirect_spirv;
          indirectPlan.spirv_size = plan.indirect_spirv_size;
          pipeline.indirectVariant = createCaptureVariant(*dev, device, cache, info, indirectPlan, pAllocator);
          pipeline.indirectLayout  = indirect;
        }

        if (plan.bake_spirv && plan.bake_spirv_size)
          pipeline.bakeVariant = createBakeVariant(*dev, device, cache, info, plan, pAllocator, pipeline);

        // UI: one colour target of a presentable format, no depth test
        // (decided per draw: dynamic depth test counts as unknown), one
        // sample. The variant only when blending allows a layer.
        bool dynamicDepthTest = false, dynamicBlend = false;

        if (info.pDynamicState) {
          for (uint32_t d = 0; d < info.pDynamicState->dynamicStateCount; d++) {
            const VkDynamicState s = info.pDynamicState->pDynamicStates[d];
            dynamicDepthTest |= s == VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE;
            dynamicBlend     |= s == VK_DYNAMIC_STATE_COLOR_BLEND_ENABLE_EXT || s == VK_DYNAMIC_STATE_COLOR_BLEND_EQUATION_EXT
                             || s == VK_DYNAMIC_STATE_COLOR_WRITE_MASK_EXT;
          }
        }

        pipeline.uiCandidate = desc.render_target_count == 1 && isSwapchainFormat(desc.render_target_formats[0])
          && !desc.depth_test && !dynamicDepthTest && desc.samples <= VK_SAMPLE_COUNT_1_BIT;

        if (pipeline.uiCandidate && !dynamicBlend)
          pipeline.uiVariant = createUiVariant(*dev, device, cache, info, pAllocator);

        std::lock_guard lock(dev->mutex);
        dev->pipelines[pPipelines[i]] = pipeline;
      }

      return result;
    }

    VKAPI_ATTR void VKAPI_CALL DestroyPipeline(VkDevice device, VkPipeline pipeline, const VkAllocationCallbacks* pAllocator) {
      DeviceData* dev = deviceOf(device);

      if (dev->active() && pipeline) {
        PipelineInfo info;
        bool found = false;

        {
          std::lock_guard lock(dev->mutex);
          auto it = dev->pipelines.find(pipeline);

          if (it != dev->pipelines.end()) {
            info = it->second;
            found = true;
            dev->pipelines.erase(it);
          }
        }

        if (found) {
          remixApi()->on_pipeline_destroy(dev->remix, info.key);

          if (info.variant)
            dev->DestroyPipeline(device, info.variant, pAllocator);

          if (info.bakeVariant)
            dev->DestroyPipeline(device, info.bakeVariant, pAllocator);

          if (info.uiVariant)
            dev->DestroyPipeline(device, info.uiVariant, pAllocator);

          if (info.indirectVariant)
            dev->DestroyPipeline(device, info.indirectVariant, pAllocator);
        }
      }

      dev->DestroyPipeline(device, pipeline, pAllocator);
    }

    // ------------------------------------------------------------ submission

    // Serializes a game queue call with Remix's submissions when Remix
    // shares that queue (GPUs with a single graphics queue).
    struct QueueLock {
      DeviceData* dev;
      VkQueue     queue;

      QueueLock(DeviceData* d, VkQueue q) : dev(d), queue(q) {
        if (dev->active())
          remixApi()->lock_queue(dev->remix, queue);
      }

      ~QueueLock() {
        if (dev->active())
          remixApi()->unlock_queue(dev->remix, queue);
      }
    };

    void reportSubmit(DeviceData* dev, VkQueue queue, std::vector<VkCommandBuffer>& buffers) {
      // Secondary command buffers run where the primary executes them.
      {
        std::lock_guard lock(dev->mutex);
        const size_t primaries = buffers.size();

        for (size_t i = 0; i < primaries; i++) {
          auto it = dev->commandBuffers.find(buffers[i]);

          if (it != dev->commandBuffers.end())
            buffers.insert(buffers.end(), it->second->secondaries.begin(), it->second->secondaries.end());
        }
      }

      // Mapped-memory bindings: their bytes as the application left them
      // for this submission (vulkan_layer_capture.md 1.10, item 4). A
      // command buffer submitted again without being re-recorded gets the
      // current bytes too.
      const remix_vkfe_api* api = remixApi();

      if (api->on_submit_bytes) {
        std::lock_guard lock(dev->mutex);

        for (VkCommandBuffer cmd : buffers) {
          auto it = dev->commandBuffers.find(cmd);

          if (it == dev->commandBuffers.end())
            continue;

          for (const auto& r : it->second->submitReads) {
            if (r.host) {
              api->on_submit_bytes(dev->remix, cmd, r.draw, r.binding, r.host, r.size);
              continue;
            }

            VkDeviceSize size = r.size;
            bool shadowed = false;
            const uint8_t* bytes = cpuBytes(*dev, r.buffer, r.offset, size, shadowed);

            if (bytes && !shadowed && size)
              api->on_submit_bytes(dev->remix, cmd, r.draw, r.binding, bytes, size);
          }
        }
      }

      remix_vkfe_submit_desc desc = {};
      desc.queue = queue;
      desc.command_buffer_count = uint32_t(buffers.size());
      desc.command_buffers = buffers.data();
      api->on_submit(dev->remix, &desc);
    }

    VKAPI_ATTR VkResult VKAPI_CALL QueueSubmit(VkQueue queue, uint32_t count, const VkSubmitInfo* pSubmits, VkFence fence) {
      DeviceData* dev = deviceOf(queue);

      if (dev->active()) {
        std::vector<VkCommandBuffer> buffers;

        for (uint32_t i = 0; i < count; i++)
          buffers.insert(buffers.end(), pSubmits[i].pCommandBuffers, pSubmits[i].pCommandBuffers + pSubmits[i].commandBufferCount);

        reportSubmit(dev, queue, buffers);

        if (!buffers.empty())
          reflexOnSubmit(dev);
      }

      QueueLock lock(dev, queue);
      return dev->QueueSubmit(queue, count, pSubmits, fence);
    }

    void collectSubmit2(uint32_t count, const VkSubmitInfo2* pSubmits, std::vector<VkCommandBuffer>& buffers) {
      for (uint32_t i = 0; i < count; i++) {
        for (uint32_t k = 0; k < pSubmits[i].commandBufferInfoCount; k++)
          buffers.push_back(pSubmits[i].pCommandBufferInfos[k].commandBuffer);
      }
    }

    VKAPI_ATTR VkResult VKAPI_CALL QueueSubmit2(VkQueue queue, uint32_t count, const VkSubmitInfo2* pSubmits, VkFence fence) {
      DeviceData* dev = deviceOf(queue);

      if (dev->active()) {
        std::vector<VkCommandBuffer> buffers;
        collectSubmit2(count, pSubmits, buffers);
        reportSubmit(dev, queue, buffers);

        if (!buffers.empty())
          reflexOnSubmit(dev);
      }

      QueueLock lock(dev, queue);
      return dev->QueueSubmit2(queue, count, pSubmits, fence);
    }

    VKAPI_ATTR VkResult VKAPI_CALL QueueSubmit2KHR(VkQueue queue, uint32_t count, const VkSubmitInfo2* pSubmits, VkFence fence) {
      DeviceData* dev = deviceOf(queue);

      if (dev->active()) {
        std::vector<VkCommandBuffer> buffers;
        collectSubmit2(count, pSubmits, buffers);
        reportSubmit(dev, queue, buffers);

        if (!buffers.empty())
          reflexOnSubmit(dev);
      }

      QueueLock lock(dev, queue);
      return dev->QueueSubmit2KHR(queue, count, pSubmits, fence);
    }

    VKAPI_ATTR VkResult VKAPI_CALL QueueWaitIdle(VkQueue queue) {
      DeviceData* dev = deviceOf(queue);
      QueueLock lock(dev, queue);
      return dev->QueueWaitIdle(queue);
    }

    VKAPI_ATTR VkResult VKAPI_CALL QueueBindSparse(VkQueue queue, uint32_t count, const VkBindSparseInfo* pBindInfo, VkFence fence) {
      DeviceData* dev = deviceOf(queue);
      QueueLock lock(dev, queue);
      return dev->QueueBindSparse(queue, count, pBindInfo, fence);
    }

    // ----------------------------------------------------------- swap chains

    VKAPI_ATTR VkResult VKAPI_CALL CreateSwapchainKHR(VkDevice device, const VkSwapchainCreateInfoKHR* pCreateInfo,
                                                      const VkAllocationCallbacks* pAllocator, VkSwapchainKHR* pSwapchain) {
      DeviceData* dev = deviceOf(device);

      if (!dev->active())
        return dev->CreateSwapchainKHR(device, pCreateInfo, pAllocator, pSwapchain);

      // Remix copies the game's frame in and its own frame out.
      VkSwapchainCreateInfoKHR info = *pCreateInfo;
      constexpr VkImageUsageFlags kTransfer = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;

      if (dev->instance->GetPhysicalDeviceSurfaceCapabilitiesKHR) {
        VkSurfaceCapabilitiesKHR caps = {};

        if (dev->instance->GetPhysicalDeviceSurfaceCapabilitiesKHR(dev->physical, info.surface, &caps) == VK_SUCCESS)
          info.imageUsage |= caps.supportedUsageFlags & kTransfer;
      }

      // Reflex (vklayer_reflex.cpp) needs latency mode on the swap chain.
      // vkd3d-proton chains its own when the driver has the extension.
      VkSwapchainLatencyCreateInfoNV latency = { VK_STRUCTURE_TYPE_SWAPCHAIN_LATENCY_CREATE_INFO_NV };
      latency.pNext = const_cast<void*>(pCreateInfo->pNext);
      latency.latencyModeEnable = VK_TRUE;
      bool latencyMode = dev->reflex.enabled;
      bool gameLatency = false;

      for (auto* s = reinterpret_cast<const VkBaseInStructure*>(pCreateInfo->pNext); s; s = s->pNext) {
        if (s->sType == VK_STRUCTURE_TYPE_SWAPCHAIN_LATENCY_CREATE_INFO_NV)
          gameLatency = reinterpret_cast<const VkSwapchainLatencyCreateInfoNV*>(s)->latencyModeEnable == VK_TRUE;
      }

      if (latencyMode && !gameLatency)
        info.pNext = &latency;

      VkResult result = dev->CreateSwapchainKHR(device, &info, pAllocator, pSwapchain);

      if (result != VK_SUCCESS && latencyMode && !gameLatency) {
        log("swap chain creation with Reflex latency mode failed (%d); creating it without", result);
        latencyMode = false;
        info.pNext = pCreateInfo->pNext;
        result = dev->CreateSwapchainKHR(device, &info, pAllocator, pSwapchain);
      }

      if (result != VK_SUCCESS && info.imageUsage != pCreateInfo->imageUsage) {
        log("swap chain creation with transfer usage failed (%d); creating the game's swap chain unchanged", result);
        info = *pCreateInfo;
        result = dev->CreateSwapchainKHR(device, &info, pAllocator, pSwapchain);
      }

      if (result != VK_SUCCESS)
        return result;

      uint32_t imageCount = 0;
      dev->GetSwapchainImagesKHR(device, *pSwapchain, &imageCount, nullptr);
      std::vector<VkImage> images(imageCount);
      dev->GetSwapchainImagesKHR(device, *pSwapchain, &imageCount, images.data());
      images.resize(imageCount);

      {
        std::lock_guard lock(dev->mutex);
        SwapchainInfo& sc = dev->swapchains[*pSwapchain];
        sc.images = images;
        sc.format = info.imageFormat;
        sc.extent = info.imageExtent;
        sc.latency = latencyMode && (gameLatency || info.pNext == &latency);
        sc.reflexMode = UINT32_MAX;
        sc.lastPresentId = 0;

        for (VkImage image : images) {
          ImageInfo ii;
          ii.format      = info.imageFormat;
          ii.extent      = { info.imageExtent.width, info.imageExtent.height, 1 };
          ii.layers      = info.imageArrayLayers;
          ii.swapchain   = true;
          ii.transferSrc = (info.imageUsage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0;
          dev->images[image] = ii;
        }
      }

      remixApi()->on_swapchain(dev->remix, *pSwapchain, &info, imageCount, images.data());

      if (HWND window = dev->instance->windowOf(info.surface))
        remixApi()->on_swapchain_window(dev->remix, *pSwapchain, window);

      return result;
    }

    VKAPI_ATTR void VKAPI_CALL DestroySwapchainKHR(VkDevice device, VkSwapchainKHR swapchain, const VkAllocationCallbacks* pAllocator) {
      DeviceData* dev = deviceOf(device);

      if (dev->active() && swapchain) {
        remixApi()->on_swapchain_destroy(dev->remix, swapchain);

        std::lock_guard lock(dev->mutex);
        auto it = dev->swapchains.find(swapchain);

        if (it != dev->swapchains.end()) {
          for (VkImage image : it->second.images)
            dev->images.erase(image);

          dev->swapchains.erase(it);
        }
      }

      dev->DestroySwapchainKHR(device, swapchain, pAllocator);
    }

    VKAPI_ATTR VkResult VKAPI_CALL QueuePresentKHR(VkQueue queue, const VkPresentInfoKHR* pPresentInfo) {
      DeviceData* dev = deviceOf(queue);

      if (!dev->active() || !pPresentInfo->swapchainCount)
        return dev->QueuePresentKHR(queue, pPresentInfo);

      remix_vkfe_present_desc desc = {};
      desc.queue       = queue;
      desc.swapchain   = pPresentInfo->pSwapchains[0];
      desc.image_index = pPresentInfo->pImageIndices[0];
      desc.wait_semaphore_count = pPresentInfo->waitSemaphoreCount;
      desc.wait_semaphores      = pPresentInfo->pWaitSemaphores;

      {
        std::lock_guard lock(dev->mutex);
        auto it = dev->swapchains.find(desc.swapchain);

        if (it == dev->swapchains.end() || desc.image_index >= it->second.images.size()) {
          QueueLock queueLock(dev, queue);
          return dev->QueuePresentKHR(queue, pPresentInfo);
        }

        desc.image  = it->second.images[desc.image_index];
        desc.format = it->second.format;
        desc.extent = it->second.extent;
      }

      remix_vkfe_present_result result = {};
      remixApi()->on_present(dev->remix, &desc, &result);

      // The next frame's pre-UI snapshot starts over.
      dev->snapshotTaken.store(false);
      dev->uiStarted.store(false);

      // Bake / UI layer framebuffers retired a few frames ago are no longer
      // referenced by command buffers in flight.
      {
        constexpr uint64_t kRetireFrames = 8;
        std::vector<VkFramebuffer> expired;

        {
          std::lock_guard mapLock(dev->mutex);
          dev->presentCount++;

          auto keep = dev->retiredFramebuffers.begin();

          for (const auto& r : dev->retiredFramebuffers) {
            if (r.first + kRetireFrames <= dev->presentCount)
              expired.push_back(r.second);
            else
              *keep++ = r;
          }

          dev->retiredFramebuffers.erase(keep, dev->retiredFramebuffers.end());
        }

        for (VkFramebuffer fb : expired)
          dev->DestroyFramebuffer(dev->device, fb, nullptr);
      }

      // Reflex markers and present ID (vklayer_reflex.cpp).
      uint64_t gamePresentId = 0;

      // VK_KHR_present_id2's VkPresentId2KHR (newer than this branch's
      // Vulkan headers) has VkPresentIdKHR's layout; vkd3d-proton uses either.
      constexpr VkStructureType kPresentId2 = VkStructureType(1000479001);

      for (auto* s = reinterpret_cast<const VkBaseInStructure*>(pPresentInfo->pNext); s; s = s->pNext) {
        if (s->sType == VK_STRUCTURE_TYPE_PRESENT_ID_KHR || s->sType == kPresentId2) {
          const auto* ids = reinterpret_cast<const VkPresentIdKHR*>(s);

          if (ids->swapchainCount && ids->pPresentIds)
            gamePresentId = ids->pPresentIds[0];
        }
      }

      const uint64_t layerPresentId = reflexBeforePresent(dev, desc.swapchain, gamePresentId);

      // Remix waited on the game's semaphores and signals its own when its
      // frame is in the image.
      VkPresentInfoKHR present = *pPresentInfo;

      if (result.wait_semaphore) {
        present.waitSemaphoreCount = 1;
        present.pWaitSemaphores    = &result.wait_semaphore;
      }

      VkPresentIdKHR presentId = { VK_STRUCTURE_TYPE_PRESENT_ID_KHR };

      if (layerPresentId && pPresentInfo->swapchainCount == 1) {
        presentId.pNext          = present.pNext;
        presentId.swapchainCount = 1;
        presentId.pPresentIds    = &layerPresentId;
        present.pNext            = &presentId;
      }

      VkResult presentResult;

      {
        QueueLock lock(dev, queue);
        presentResult = dev->QueuePresentKHR(queue, &present);
      }

      // Outside the queue lock: the sleep holds the game's thread back until
      // its next frame should start.
      reflexAfterPresent(dev, desc.swapchain, gamePresentId ? gamePresentId : layerPresentId);
      return presentResult;
    }

  }


  void annotateDraw(VkCommandBuffer cmd, const remix_vkfe_binding* bindings, uint32_t count) {
    DeviceData* dev = cmd ? deviceOf(cmd) : nullptr;

    if (!dev || !dev->active())
      return;

    // vkd3d calls this right before the Vulkan draw, and the draw reaches
    // Remix (which copies the bytes it keeps) within the same vkd3d call, so
    // the pointers - root constants in vkd3d's list state, upload-heap
    // memory - are still valid then. Nothing is copied here.
    CommandState* st = commandState(*dev, cmd);
    st->annotation.assign(bindings, bindings + count);
    st->annotated = true;
  }


  void annotateIndirect(VkCommandBuffer cmd, const remix_vkfe_indirect_annotation* annotation) {
    DeviceData* dev = cmd ? deviceOf(cmd) : nullptr;

    if (!dev || !dev->active() || !annotation)
      return;

    CommandState* st = commandState(*dev, cmd);
    st->indirect = *annotation;
    st->indirectAnnotated = true;
  }


  namespace {
    std::mutex                                            g_indirectMutex;
    std::unordered_map<VkPipelineLayout, IndirectLayout>  g_indirectLayouts;
  }

  void noteIndirectLayout(VkPipelineLayout layout, const IndirectLayout& indirect) {
    std::lock_guard lock(g_indirectMutex);
    auto it = g_indirectLayouts.find(layout);

    // One command layout per pipeline layout: the variant is built for it.
    if (it == g_indirectLayouts.end()) {
      g_indirectLayouts.emplace(layout, indirect);
      log("generated-commands layout: stride %u, %u constant range(s), first at push offset %u (%u bytes)",
          indirect.stride, indirect.constantCount, indirect.constants[0].push_offset, indirect.constants[0].size);
    } else if (!(it->second == indirect)) {
      log("generated-commands layout: a second command layout for one pipeline layout is not captured");
    }
  }

  bool findIndirectLayout(VkPipelineLayout layout, IndirectLayout& out) {
    std::lock_guard lock(g_indirectMutex);
    auto it = g_indirectLayouts.find(layout);

    if (it == g_indirectLayouts.end())
      return false;

    out = it->second;
    return true;
  }


  uint32_t describeView(VkDevice device, VkImageView view, VkImageLayout layout, remix_vkfe_binding* out) {
    DeviceData* dev = device && view && out ? findDevice(keyOf(device)) : nullptr;

    if (!dev)
      return 0;

    std::lock_guard lock(dev->mutex);

    if (dev->views.find(view) == dev->views.end())
      return 0;

    Descriptor d;
    d.type   = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    d.view   = view;
    d.layout = layout;

    *out = remix_vkfe_binding{};
    fillTextureBinding(*dev, d, *out);
    return out->image != VK_NULL_HANDLE ? 1u : 0u;
  }


  CommandState* commandState(DeviceData& dev, VkCommandBuffer cmd) {
    std::lock_guard lock(dev.mutex);
    auto& state = dev.commandBuffers[cmd];

    if (!state)
      state = std::make_unique<CommandState>();

    return state.get();
  }


  void resetCommandState(DeviceData& dev, VkCommandBuffer cmd) {
    {
      std::lock_guard lock(dev.mutex);
      auto it = dev.commandBuffers.find(cmd);

      if (it != dev.commandBuffers.end())
        it->second->reset();
    }

    if (dev.remix)
      remixApi()->on_command_buffer_reset(dev.remix, cmd);
  }


  PFN_vkVoidFunction commandHook(const char* name) {
#define REMIX_VKLAYER_HOOK(fn) if (!strcmp(name, "vk" #fn)) return reinterpret_cast<PFN_vkVoidFunction>(&fn);
    REMIX_VKLAYER_HOOK(AllocateCommandBuffers)
    REMIX_VKLAYER_HOOK(FreeCommandBuffers)
    REMIX_VKLAYER_HOOK(ResetCommandPool)
    REMIX_VKLAYER_HOOK(DestroyCommandPool)
    REMIX_VKLAYER_HOOK(BeginCommandBuffer)
    REMIX_VKLAYER_HOOK(ResetCommandBuffer)
    REMIX_VKLAYER_HOOK(EndCommandBuffer)
    REMIX_VKLAYER_HOOK(CmdExecuteGeneratedCommandsEXT)
    REMIX_VKLAYER_HOOK(CmdBindPipeline)
    REMIX_VKLAYER_HOOK(CmdBindDescriptorSets)
    REMIX_VKLAYER_HOOK(CmdPushDescriptorSetKHR)
    REMIX_VKLAYER_HOOK(CmdPushDescriptorSetWithTemplateKHR)
    REMIX_VKLAYER_HOOK(CmdPushConstants)
    REMIX_VKLAYER_HOOK(CmdBindVertexBuffers)
    REMIX_VKLAYER_HOOK(CmdBindVertexBuffers2)
    REMIX_VKLAYER_HOOK(CmdBindVertexBuffers2EXT)
    REMIX_VKLAYER_HOOK(CmdBindIndexBuffer)
    REMIX_VKLAYER_HOOK(CmdBeginRenderPass)
    REMIX_VKLAYER_HOOK(CmdBeginRenderPass2)
    REMIX_VKLAYER_HOOK(CmdBeginRenderPass2KHR)
    REMIX_VKLAYER_HOOK(CmdEndRenderPass)
    REMIX_VKLAYER_HOOK(CmdEndRenderPass2)
    REMIX_VKLAYER_HOOK(CmdEndRenderPass2KHR)
    REMIX_VKLAYER_HOOK(CmdNextSubpass)
    REMIX_VKLAYER_HOOK(CmdNextSubpass2)
    REMIX_VKLAYER_HOOK(CmdNextSubpass2KHR)
    REMIX_VKLAYER_HOOK(CmdSetRasterizerDiscardEnable)
    REMIX_VKLAYER_HOOK(CmdSetRasterizerDiscardEnableEXT)
    REMIX_VKLAYER_HOOK(CmdBeginRendering)
    REMIX_VKLAYER_HOOK(CmdBeginRenderingKHR)
    REMIX_VKLAYER_HOOK(CmdEndRendering)
    REMIX_VKLAYER_HOOK(CmdEndRenderingKHR)
    REMIX_VKLAYER_HOOK(CmdSetViewport)
    REMIX_VKLAYER_HOOK(CmdSetScissor)
    REMIX_VKLAYER_HOOK(CmdSetViewportWithCount)
    REMIX_VKLAYER_HOOK(CmdSetViewportWithCountEXT)
    REMIX_VKLAYER_HOOK(CmdSetScissorWithCount)
    REMIX_VKLAYER_HOOK(CmdSetScissorWithCountEXT)
    REMIX_VKLAYER_HOOK(CmdExecuteCommands)
    REMIX_VKLAYER_HOOK(CmdBeginTransformFeedbackEXT)
    REMIX_VKLAYER_HOOK(CmdEndTransformFeedbackEXT)
    REMIX_VKLAYER_HOOK(CmdDraw)
    REMIX_VKLAYER_HOOK(CmdDrawIndexed)
    REMIX_VKLAYER_HOOK(CmdDrawIndirect)
    REMIX_VKLAYER_HOOK(CmdDrawIndexedIndirect)
    REMIX_VKLAYER_HOOK(CmdDrawIndirectCount)
    REMIX_VKLAYER_HOOK(CmdDrawIndirectCountKHR)
    REMIX_VKLAYER_HOOK(CmdDrawIndexedIndirectCount)
    REMIX_VKLAYER_HOOK(CmdDrawIndexedIndirectCountKHR)
    REMIX_VKLAYER_HOOK(CreateGraphicsPipelines)
    REMIX_VKLAYER_HOOK(DestroyPipeline)
    REMIX_VKLAYER_HOOK(QueueSubmit)
    REMIX_VKLAYER_HOOK(QueueSubmit2)
    REMIX_VKLAYER_HOOK(QueueSubmit2KHR)
    REMIX_VKLAYER_HOOK(QueueWaitIdle)
    REMIX_VKLAYER_HOOK(QueueBindSparse)
    REMIX_VKLAYER_HOOK(CreateSwapchainKHR)
    REMIX_VKLAYER_HOOK(DestroySwapchainKHR)
    REMIX_VKLAYER_HOOK(QueuePresentKHR)
#undef REMIX_VKLAYER_HOOK
    return nullptr;
  }

}
