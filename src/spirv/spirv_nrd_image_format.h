#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace dxvk {

  struct NrdStorageImageCode {
    std::vector<uint32_t> code;
    bool requiresRead = false;
    bool requiresWrite = false;
  };

  // Adapt NRD's precompiled, HLSL-inferred storage formats to the actual
  // descriptor image formats. Throws std::invalid_argument on malformed input
  // or image constructs outside NRD's direct 2D storage-image contract.
  NrdStorageImageCode normalizeNrdStorageImageFormats(const void* bytecode, size_t bytes);

}
