#pragma once

#include <algorithm>
#include <cstdint>

namespace dxvk {

  // Driver compilers have their own parallel work. Keep the host job count
  // bounded, including on machines with many logical CPUs or little RAM.
  inline uint32_t pipelineCompilerThreads(uint32_t cpuThreads, uint32_t requested,
                                         uint64_t availableMemoryBytes) {
    const uint32_t cpus = std::max(1u, cpuThreads);
    const uint32_t memoryLimit = availableMemoryBytes
      ? uint32_t(std::min<uint64_t>(16, std::max<uint64_t>(1, availableMemoryBytes >> 30)))
      : 4u;
    const uint32_t automatic = std::clamp(cpus / 4u, 1u, 4u);
    return std::clamp(requested ? requested : automatic, 1u,
      std::max(1u, std::min({ cpus, memoryLimit, 16u })));
  }

  inline uint32_t remixCompilerThreads(uint32_t cpuThreads, uint32_t requested) {
    return std::clamp(requested ? requested : std::max(1u, cpuThreads / 8u),
                      1u, std::max(1u, std::min(4u, cpuThreads)));
  }

  // UINT32_MAX is the Vulkan "unknown concurrency" sentinel, not a job count.
  // The caller participates, so only the remaining joins need helper tasks.
  inline uint32_t deferredCompilerHelpers(uint32_t driverConcurrency,
                                         uint32_t cpuThreads) {
    const uint32_t useful = std::min(driverConcurrency,
      std::clamp(std::max(1u, cpuThreads) / 8u, 1u, 3u));
    return useful ? useful - 1u : 0u;
  }

  class ShaderPreloadBudget {
  public:
    static constexpr uint32_t MaximumModules = 512;
    static constexpr uint64_t MaximumBytecodeBytes = 16ull << 20;
    static constexpr uint32_t MaximumPendingPipelines = 32;

    bool admit(uint64_t bytes) {
      if (!bytes || m_modules >= MaximumModules || bytes > MaximumBytecodeBytes - m_bytes)
        return false;
      ++m_modules;
      m_bytes += bytes;
      return true;
    }

  private:
    uint32_t m_modules = 0;
    uint64_t m_bytes = 0;
  };

}
