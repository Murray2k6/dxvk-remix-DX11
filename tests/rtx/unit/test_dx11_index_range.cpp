#include "../../../src/d3d11/d3d11_rtx_index_range.h"

#include <array>
#include <cstdlib>
#include <iostream>
#include <random>
#include <vector>

static void check(bool passed, const char* description) {
  if (!passed) {
    std::cerr << description << '\n';
    std::exit(1);
  }
}

int main() {
  using namespace dxvk::rtx;
  // A 128 MiB shared vertex buffer with a 20-byte stride must retain valid
  // 16-bit draws even though its allocation exceeds the old 4M-vertex cap.
  check(addressableVertexCount<uint16_t>((128u << 20) / 20u, false) == 65536u,
        "large shared buffer must be bounded by the index width");
  check(addressableVertexCount<uint16_t>(100u, false) == 100u,
        "small vertex slices must remain bounded by their allocation");
  check(addressableVertexCount<uint16_t>(100000u, true) == 65535u,
        "strip restart is not a vertex");
  check(addressableVertexCount<uint32_t>(UINT32_MAX, false) == UINT32_MAX,
        "32-bit address range must not overflow");

  const std::array<uint16_t, 3> edge = { 0, 65535, 7 };
  auto range = scanIndexRange<uint16_t>(edge.data(), uint32_t(edge.size()), 65536u, false);
  check(range.valid && range.vertexCount == 65536u, "65535 is valid in a triangle list");
  range = scanIndexRange<uint16_t>(edge.data(), uint32_t(edge.size()), 10u, true);
  check(range.valid && range.vertexCount == 8u, "strip restart must be skipped");
  check(!scanIndexRange<uint16_t>(edge.data(), uint32_t(edge.size()), 10u, false).valid,
        "out-of-bounds triangle-list indices must be rejected");
  const uint32_t restart = UINT32_MAX;
  check(!scanIndexRange<uint32_t>(&restart, 1u, UINT32_MAX, true).valid,
        "a restart-only draw has no geometry");
  check(!scanIndexRange<uint32_t>(&restart, 1u, UINT32_MAX, false).valid,
        "unrepresentable maximum vertex count must be rejected");

  const std::array<uint16_t, 3> distant = { 50000, 50002, 50001 };
  auto span = captureVertexSpan<uint16_t>(distant.data(), 3u, -49000);
  check(span.first == 1000u && span.count == 3u,
        "indexed capture must hash actual source vertices, including signed base");
  check(captureVertexSpan<uint16_t>(distant.data(), 3u, -50001).count == 0,
        "negative effective index cannot be reused");
  check(captureVertexSpan<uint32_t>(&restart, 1u, 1).count == 0,
        "effective index addition must not wrap");
  check(captureVertexSpan<uint32_t>(nullptr, 3u, 0).count == 0,
        "unreadable indices require GPU replay");
  check(captureInstanceElementCount(0u, 0u) == 0u
        && captureInstanceElementCount(17u, 0u) == 1u
        && captureInstanceElementCount(17u, 4u) == 5u
        && captureInstanceElementCount(UINT32_MAX, 1u) == UINT32_MAX,
        "instance input coverage must honor zero and non-unit step rates");

  // Compare real production scanning with an independent reference over
  // unaligned byte buffers and randomized capacities, including tiny slices.
  std::mt19937 rng(0x5060u);
  for (uint32_t trial = 0; trial < 10000u; ++trial) {
    const uint32_t count = 1u + rng() % 100u;
    const uint32_t available = rng() % 65537u;
    const bool strip = (trial & 1u) != 0;
    std::vector<uint8_t> bytes(count * sizeof(uint16_t) + 1u);
    uint32_t expectedCount = 0;
    bool expectedValid = true;
    for (uint32_t i = 0; i < count; ++i) {
      const uint16_t index = uint16_t(rng());
      std::memcpy(bytes.data() + 1u + i * sizeof(index), &index, sizeof(index));
      if (strip && index == UINT16_MAX)
        continue;
      expectedValid &= index < available;
      expectedCount = std::max(expectedCount, uint32_t(index) + 1u);
    }
    expectedValid &= expectedCount > 0;
    range = scanIndexRange<uint16_t>(bytes.data() + 1u, count, available, strip);
    check(range.valid == expectedValid, "randomized bounds validation disagrees");
    if (range.valid)
      check(range.vertexCount == expectedCount, "randomized maximum disagrees");

    const int32_t base = int32_t(rng() % 131073u) - 65536;
    int64_t minimum = INT64_MAX;
    int64_t maximum = INT64_MIN;
    for (uint32_t i = 0; i < count; ++i) {
      uint16_t index;
      std::memcpy(&index, bytes.data() + 1u + i * sizeof(index), sizeof(index));
      minimum = std::min(minimum, int64_t(index) + base);
      maximum = std::max(maximum, int64_t(index) + base);
    }
    span = captureVertexSpan<uint16_t>(bytes.data() + 1u, count, base);
    if (minimum < 0)
      check(span.count == 0, "negative source address escaped capture validation");
    else
      check(span.first == uint64_t(minimum) && span.count == uint64_t(maximum - minimum + 1),
            "randomized unaligned capture source coverage disagrees");
  }
  std::cout << "DX11 index range: edge cases and 10000 randomized cases passed\n";
}
