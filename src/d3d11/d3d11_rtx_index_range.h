#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>

namespace dxvk::rtx {

  struct CaptureVertexSpan {
    uint64_t first = 0;
    uint64_t count = 0;
  };

  // Indexed XFB emits indexCount vertices, but may fetch entirely different
  // source addresses. Cache validation must cover the fetched source interval.
  template<typename Index>
  CaptureVertexSpan captureVertexSpan(const void* data, uint32_t count, int32_t base) {
    if (data == nullptr || count == 0)
      return {};
    const auto* bytes = static_cast<const uint8_t*>(data);
    uint64_t first = UINT64_MAX;
    uint64_t last = 0;
    for (uint32_t i = 0; i < count; ++i) {
      Index index;
      std::memcpy(&index, bytes + size_t(i) * sizeof(Index), sizeof(Index));
      const int64_t vertex = int64_t(index) + base;
      if (vertex < 0 || uint64_t(vertex) > UINT32_MAX)
        return {};
      first = std::min(first, uint64_t(vertex));
      last = std::max(last, uint64_t(vertex));
    }
    return { first, last - first + 1u };
  }

  constexpr uint64_t captureInstanceElementCount(uint32_t count, uint32_t stepRate) {
    return count == 0 ? 0 : stepRate == 0 ? 1 : (uint64_t(count) - 1u) / stepRate + 1u;
  }

  // BaseVertexLocation is already folded into the vertex slice. A 16-bit
  // index cannot address the rest of a large shared allocation beyond 65535.
  template<typename Index>
  constexpr uint32_t addressableVertexCount(uint32_t available, bool primitiveRestart) {
    const uint64_t count = uint64_t(std::numeric_limits<Index>::max())
      + (primitiveRestart ? 0u : 1u);
    return uint32_t(std::min<uint64_t>(available, count));
  }

  struct IndexRange {
    bool valid;
    uint32_t vertexCount;
  };

  template<typename Index>
  IndexRange scanIndexRange(const void* data, uint32_t count,
                           uint32_t availableVertices, bool primitiveRestart) {
    const auto* bytes = static_cast<const uint8_t*>(data);
    uint32_t vertices = 0;
    for (uint32_t i = 0; i < count; ++i) {
      Index index;
      // D3D11 buffer offsets need not satisfy the host pointer's alignment.
      std::memcpy(&index, bytes + size_t(i) * sizeof(Index), sizeof(Index));
      if (primitiveRestart && index == std::numeric_limits<Index>::max())
        continue;
      if (uint64_t(index) >= availableVertices)
        return { false, 0 };
      vertices = std::max(vertices, uint32_t(index) + 1u);
    }
    return { vertices != 0, vertices };
  }
}
