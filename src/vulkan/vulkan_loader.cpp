#include "vulkan_loader.h"

namespace dxvk::vk {

  static const PFN_vkGetInstanceProcAddr GetInstanceProcAddr = vkGetInstanceProcAddr;

  PFN_vkVoidFunction LibraryLoader::sym(const char* name) const {
    return dxvk::vk::GetInstanceProcAddr(nullptr, name);
  }


  InstanceLoader::InstanceLoader(bool owned, VkInstance instance, PFN_vkGetInstanceProcAddr getInstanceProcAddr)
  : m_getInstanceProcAddr(getInstanceProcAddr ? getInstanceProcAddr : dxvk::vk::GetInstanceProcAddr),
    m_instance(instance), m_owned(owned) { }


  PFN_vkVoidFunction InstanceLoader::sym(const char* name) const {
    return m_getInstanceProcAddr(m_instance, name);
  }


  DeviceLoader::DeviceLoader(bool owned, VkInstance instance, VkDevice device, PFN_vkGetDeviceProcAddr getDeviceProcAddr)
  : m_getDeviceProcAddr(getDeviceProcAddr ? getDeviceProcAddr : reinterpret_cast<PFN_vkGetDeviceProcAddr>(
      dxvk::vk::GetInstanceProcAddr(instance, "vkGetDeviceProcAddr"))),
    m_device(device), m_owned(owned) { }


  PFN_vkVoidFunction DeviceLoader::sym(const char* name) const {
    return m_getDeviceProcAddr(m_device, name);
  }


  LibraryFn::LibraryFn() { }
  LibraryFn::~LibraryFn() { }


  InstanceFn::InstanceFn(bool owned, VkInstance instance, PFN_vkGetInstanceProcAddr getInstanceProcAddr)
  : InstanceLoader(owned, instance, getInstanceProcAddr) { }
  InstanceFn::~InstanceFn() {
    if (m_owned)
      this->vkDestroyInstance(m_instance, nullptr);
  }


  DeviceFn::DeviceFn(bool owned, VkInstance instance, VkDevice device, PFN_vkGetDeviceProcAddr getDeviceProcAddr)
  : DeviceLoader(owned, instance, device, getDeviceProcAddr) { }
  DeviceFn::~DeviceFn() {
    if (m_owned)
      this->vkDestroyDevice(m_device, nullptr);
  }

}
