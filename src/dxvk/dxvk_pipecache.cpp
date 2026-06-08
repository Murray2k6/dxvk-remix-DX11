#include "dxvk_pipecache.h"

namespace dxvk {
  
  DxvkPipelineCache::DxvkPipelineCache(
    const Rc<vk::DeviceFn>& vkd)
  : m_vkd(vkd),
    m_cacheFileName(getCacheFileName()) {
    std::vector<char> initialData = readCacheFile();

    VkPipelineCacheCreateInfo info;
    info.sType            = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
    info.pNext            = nullptr;
    info.flags            = 0;
    info.initialDataSize  = initialData.size();
    info.pInitialData     = initialData.empty() ? nullptr : initialData.data();
    
    VkResult status = m_vkd->vkCreatePipelineCache(m_vkd->device(),
      &info, nullptr, &m_handle);

    if (status != VK_SUCCESS && !initialData.empty()) {
      Logger::warn("DxvkPipelineCache: Existing Vulkan pipeline cache was rejected, retrying with an empty cache");
      info.initialDataSize = 0;
      info.pInitialData = nullptr;
      status = m_vkd->vkCreatePipelineCache(m_vkd->device(),
        &info, nullptr, &m_handle);
    }

    if (status != VK_SUCCESS)
      throw DxvkError("DxvkPipelineCache: Failed to create cache");
  }
  
  
  DxvkPipelineCache::~DxvkPipelineCache() {
    writeCacheFile();

    m_vkd->vkDestroyPipelineCache(
      m_vkd->device(), m_handle, nullptr);
  }


  std::vector<char> DxvkPipelineCache::readCacheFile() const {
    if (m_cacheFileName.empty())
      return {};

    std::ifstream file(m_cacheFileName, std::ios_base::binary | std::ios_base::ate);
    if (!file)
      return {};

    const std::streamsize size = file.tellg();
    if (size <= 0)
      return {};

    std::vector<char> data(static_cast<size_t>(size));
    file.seekg(0, std::ios_base::beg);
    if (!file.read(data.data(), size))
      return {};

    Logger::info("DxvkPipelineCache: Loaded Vulkan pipeline cache from disk");
    return data;
  }


  void DxvkPipelineCache::writeCacheFile() const {
    if (m_cacheFileName.empty() || m_handle == VK_NULL_HANDLE)
      return;

    size_t size = 0;
    VkResult status = m_vkd->vkGetPipelineCacheData(m_vkd->device(), m_handle, &size, nullptr);
    if (status != VK_SUCCESS || size == 0)
      return;

    std::vector<char> data(size);
    status = m_vkd->vkGetPipelineCacheData(m_vkd->device(), m_handle, &size, data.data());
    if (status != VK_SUCCESS || size == 0)
      return;

    std::ofstream file(m_cacheFileName, std::ios_base::binary | std::ios_base::trunc);
    if (!file) {
      const std::string dir = getCacheDir();
      if (!dir.empty() && env::createDirectory(dir)) {
        file = std::ofstream(m_cacheFileName, std::ios_base::binary | std::ios_base::trunc);
      }
    }

    if (!file)
      return;

    file.write(data.data(), static_cast<std::streamsize>(size));
  }


  std::string DxvkPipelineCache::getCacheFileName() const {
    std::string path = getCacheDir();

    if (!path.empty() && *path.rbegin() != '/' && *path.rbegin() != '\\')
      path += '/';

    path += env::getExeBaseName() + ".vk-pipeline-cache";
    return path;
  }


  std::string DxvkPipelineCache::getCacheDir() const {
    std::string path = env::getEnvVar("DXVK_PIPELINE_CACHE_PATH");
    if (path.empty())
      path = env::getEnvVar("DXVK_STATE_CACHE_PATH");
    return path;
  }
  
}
