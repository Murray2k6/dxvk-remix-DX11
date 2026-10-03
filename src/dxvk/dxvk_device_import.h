#pragma once

#include <memory>
#include <string>
#include <vector>

#include "dxvk_adapter.h"
#include "dxvk_device.h"
#include "dxvk_instance.h"

namespace dxvk {

  /**
   * \brief Runs Remix on a VkDevice created by the game
   *
   * DX12 games (through vkd3d-proton) and native Vulkan games create their own
   * VkDevice. Remix cannot create a second device and share images with it
   * cheaply, so it renders on the game's device. That device must be created
   * with everything Remix needs, which this class adds to the game's create
   * info:
   *
   *   1. prepare(gameInfo)   merges Remix's device extensions, feature bits
   *                          and queues into a copy of the game's create info
   *   2. the caller runs vkCreateDevice with createInfo()
   *   3. restore()           undoes in-place feature edits (always, even when
   *                          vkCreateDevice failed)
   *   4. import(device)      wraps the new VkDevice in a DxvkDevice
   *
   * Feature merge rules (Vulkan spec, VkDeviceCreateInfo valid usage):
   *   - A feature struct the game already chains is ORed in place. Its bytes
   *     are saved first and put back by restore(), so the game never sees the
   *     change. Read-only memory is never written; prepare() fails instead.
   *   - A struct the game lacks is prepended from storage owned here.
   *   - VkPhysicalDeviceVulkan1xFeatures and the structs promoted into it may
   *     not appear together. Remix's bits are folded into whichever form the
   *     game used.
   *   - pEnabledFeatures and VkPhysicalDeviceFeatures2 are exclusive; the
   *     game's choice is kept.
   *
   * Queues: Remix gets queues of its own, added to the game's queue create
   * infos, so DXVK's submission thread never races the game's submissions on
   * one VkQueue (vkQueueSubmit requires external synchronization). If the
   * graphics family has no spare queue (AMD, Intel), Remix shares the
   * game's queue 0 and sharesGameQueue() tells the front end to serialize.
   */
  class DxvkDeviceImporter {

  public:

    DxvkDeviceImporter(
      const Rc<DxvkInstance>& instance,
      const Rc<DxvkAdapter>&  adapter);

    ~DxvkDeviceImporter();

    DxvkDeviceImporter(const DxvkDeviceImporter&) = delete;
    DxvkDeviceImporter& operator = (const DxvkDeviceImporter&) = delete;

    /**
     * \brief Builds the merged create info
     *
     * \param [in] gameInfo The game's create info
     * \param [in] baseline Optional feature set the client API needs (the
     *    D3D11 front end passes D3D11Device::GetDeviceFeatures); without it
     *    Remix's own minimum is used. pNext pointers in it are ignored.
     * \returns \c false if Remix cannot run on a device created from
     *    \c gameInfo; the reason is logged.
     */
    bool prepare(
      const VkDeviceCreateInfo&  gameInfo,
      const DxvkDeviceFeatures*  baseline = nullptr);

    /**
     * \brief Create info to pass to vkCreateDevice
     *
     * Valid after a successful prepare() until this object is destroyed.
     */
    const VkDeviceCreateInfo& createInfo() const {
      return m_info;
    }

    /**
     * \brief Puts back the game's feature structs
     *
     * Call right after vkCreateDevice returns. Safe to call twice.
     */
    void restore();

    /**
     * \brief Wraps the created device
     *
     * \param [in] device Device created from createInfo()
     * \param [in] getDeviceProcAddr Optional; a Vulkan layer passes the
     *    next layer's vkGetDeviceProcAddr
     * \returns Remix device; Remix never destroys the VkDevice
     */
    Rc<DxvkDevice> import(
            VkDevice                device,
            PFN_vkGetDeviceProcAddr getDeviceProcAddr);

    /**
     * \brief Remix's queues inside the merged create info
     */
    const DxvkAdapterQueueInfos& queues() const {
      return m_queues;
    }

    /**
     * \brief Whether Remix submits to one of the game's queues
     *
     * True when the graphics family had no spare queue. Every submission
     * to that queue must then hold DXVK's queue lock.
     */
    bool sharesGameQueue() const {
      return m_sharesGameQueue;
    }

  private:

    struct Backup {
      void*                bytes;
      std::vector<uint8_t> original;
    };

    Rc<DxvkInstance>      m_instance;
    Rc<DxvkAdapter>       m_adapter;

    DxvkDeviceExtensions  m_extensions;
    DxvkNameSet           m_extensionsEnabled;
    DxvkDeviceFeatures    m_features = {};

    VkDeviceCreateInfo    m_info = {};
    VkPhysicalDeviceFeatures m_coreFeatures = {};

    std::vector<std::string>              m_extensionStorage;
    std::vector<const char*>              m_extensionNames;
    std::vector<std::unique_ptr<uint8_t[]>> m_structStorage;
    std::vector<VkDeviceQueueCreateInfo>  m_queueInfos;
    std::vector<std::vector<float>>       m_queuePriorities;
    std::vector<Backup>                   m_backups;
    DxvkAdapterQueueInfos                 m_queues = {};
    bool                                  m_sharesGameQueue = false;

    void setBaselineFeatures();

    bool mergeExtensions(const VkDeviceCreateInfo& gameInfo);

    bool mergeFeatures(const VkDeviceCreateInfo& gameInfo);

    bool mergeQueues(const VkDeviceCreateInfo& gameInfo);

    bool orInto(
            VkBaseOutStructure* gameStruct,
      const void*               remixBools,
            size_t              boolOffset,
            size_t              boolCount);

    void prepend(const void* remixStruct, size_t size);

  };

}
