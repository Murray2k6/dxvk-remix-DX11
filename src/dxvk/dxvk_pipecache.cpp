#include "dxvk_pipecache.h"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

#include "../util/log/log.h"
#include "../util/util_string.h"

namespace dxvk {

  // DX11_V298_PERSISTENT_PIPELINE_CACHE: the serialized driver blob lives with
  // the other per-game caches (raw DXBC + pipeline-state map). Together the
  // three files let a later launch recreate every previously seen pipeline
  // from cached binaries before the game's first frame.
  static std::filesystem::path pipelineCacheFilePath() {
    return std::filesystem::path(env::getExePath()).parent_path()
      / "rtx-remix" / "cache" / (env::getExeBaseName() + ".vk-pipeline-cache");
  }

  DxvkPipelineCache::DxvkPipelineCache(
    const Rc<vk::DeviceFn>& vkd)
  : m_vkd(vkd) {
    // Seed the cache with the previous session's serialized data when
    // present. The driver validates the blob header (vendor/device/cache
    // UUID) itself and simply ignores data from another driver version,
    // so a stale file degrades to an empty cache instead of an error.
    std::vector<char> initialData;
    {
      std::error_code fileError;
      const std::filesystem::path path = pipelineCacheFilePath();
      const uintmax_t fileSize = std::filesystem::file_size(path, fileError);
      if (!fileError && fileSize > 32u && fileSize <= (512ull << 20)) {
        initialData.resize(static_cast<size_t>(fileSize));
        std::ifstream input(path, std::ios::in | std::ios::binary);
        input.read(initialData.data(),
          static_cast<std::streamsize>(initialData.size()));
        if (!input || static_cast<size_t>(input.gcount()) != initialData.size())
          initialData.clear();
      }
    }

    VkPipelineCacheCreateInfo info;
    info.sType            = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
    info.pNext            = nullptr;
    info.flags            = 0;
    info.initialDataSize  = initialData.size();
    info.pInitialData     = initialData.empty() ? nullptr : initialData.data();

    VkResult result = m_vkd->vkCreatePipelineCache(m_vkd->device(),
      &info, nullptr, &m_handle);

    if (result != VK_SUCCESS && !initialData.empty()) {
      // A corrupt blob may fail creation outright on some drivers;
      // fall back to an empty cache rather than failing device creation.
      info.initialDataSize = 0;
      info.pInitialData    = nullptr;
      initialData.clear();
      result = m_vkd->vkCreatePipelineCache(m_vkd->device(),
        &info, nullptr, &m_handle);
    }

    if (result != VK_SUCCESS)
      throw DxvkError("DxvkPipelineCache: Failed to create cache");

    // Until pipelines compile, the serialized size matches what was loaded;
    // remembering it suppresses pointless identical rewrites on save().
    m_lastSavedSize = initialData.size();
  }


  DxvkPipelineCache::~DxvkPipelineCache() {
    save();
    m_vkd->vkDestroyPipelineCache(
      m_vkd->device(), m_handle, nullptr);
  }


  void DxvkPipelineCache::save() {
    std::lock_guard<dxvk::mutex> lock(m_saveMutex);

    size_t dataSize = 0;
    if (m_vkd->vkGetPipelineCacheData(m_vkd->device(),
          m_handle, &dataSize, nullptr) != VK_SUCCESS
     || dataSize == 0)
      return;

    // Serialized caches only grow within a session; an unchanged size means
    // no new pipeline binaries since the last save (or the initial load).
    if (dataSize == m_lastSavedSize)
      return;

    std::vector<char> data(dataSize);
    if (m_vkd->vkGetPipelineCacheData(m_vkd->device(),
          m_handle, &dataSize, data.data()) != VK_SUCCESS)
      return;
    data.resize(dataSize);

    const std::filesystem::path path = pipelineCacheFilePath();
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);

    // Atomic publish: write a temporary sibling, then rename over the target
    // so a crash mid-write can never leave a truncated cache behind.
    std::filesystem::path temporary = path;
    temporary += str::format(".tmp.", ::GetCurrentProcessId());
    {
      std::ofstream output(temporary,
        std::ios::out | std::ios::binary | std::ios::trunc);
      output.write(data.data(), static_cast<std::streamsize>(data.size()));
      output.flush();
      if (!output) {
        output.close();
        std::filesystem::remove(temporary, error);
        static bool s_saveFailureLogged = false;
        if (!s_saveFailureLogged) {
          s_saveFailureLogged = true;
          Logger::err(str::format(
            "DxvkPipelineCache: failed to write ", temporary.string()));
        }
        return;
      }
    }

    std::filesystem::remove(path, error);
    error.clear();
    std::filesystem::rename(temporary, path, error);
    if (error) {
      std::error_code cleanupError;
      std::filesystem::remove(temporary, cleanupError);
      return;
    }

    m_lastSavedSize = dataSize;
  }

}
