#include "../../../bridge_dx11_work/src/client_dx11/dx11_frame_capture_coverage.h"

#define CINTERFACE
#define D3D11_NO_HELPERS
#define NOMINMAX
#include <d3d11.h>
#include <cstddef>
#include <array>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <limits>

// Check the new detours against the SDK's actual COM ABI on each architecture.
static_assert(offsetof(ID3D11DeviceContextVtbl, DrawIndexedInstanced) / sizeof(void*) == 20);
static_assert(offsetof(ID3D11DeviceContextVtbl, DrawInstanced) / sizeof(void*) == 21);
static_assert(offsetof(ID3D11DeviceContextVtbl, DrawAuto) / sizeof(void*) == 38);
static_assert(offsetof(ID3D11DeviceContextVtbl, DrawIndexedInstancedIndirect) / sizeof(void*) == 39);
static_assert(offsetof(ID3D11DeviceContextVtbl, DrawInstancedIndirect) / sizeof(void*) == 40);
static_assert(offsetof(ID3D11DeviceContextVtbl, ExecuteCommandList) / sizeof(void*) == 58);

static void require(bool value, const char* message) {
  if (!value) { std::fprintf(stderr, "%s\n", message); std::exit(1); }
}

int main() {
  using dx11_capture::FrameCaptureCoverage;
  for (const uint64_t frame : { uint64_t(0), uint64_t(7), std::numeric_limits<uint64_t>::max() }) {
    FrameCaptureCoverage empty;
    require(!empty.canPresent(frame, true, frame), "empty frame cannot take over");

    // Every order of a normal draw, an unsupported draw and known-empty draws
    // must keep a mixed scene native. A later captured draw cannot erase the gap.
    std::array<int, 4> actions { 0, 1, 2, 3 };
    do {
      FrameCaptureCoverage mixed;
      for (const int action : actions) {
        if (action == 0) mixed.recordCapturedDraw(frame);
        if (action == 1) mixed.recordUncapturedDraw(frame, 3, 2);
        if (action == 2) mixed.recordUncapturedDraw(frame, 0, 10);
        if (action == 3) mixed.recordUncapturedDraw(frame, 10, 0);
      }
      require(!mixed.canPresent(frame, true, frame), "mixed scene dominated by uncaptured draws must retain native output");
      mixed.recordCapturedDraw(frame + 1);
      require(mixed.canPresent(frame + 1, true, frame + 1), "unsupported draw cannot poison next frame");
      require(!mixed.canPresent(frame + 1, true, frame), "stale camera cannot take over");
      require(!mixed.canPresent(frame + 1, false, frame + 1), "invalid camera cannot take over");
      require(!mixed.canPresent(frame + 2, true, frame + 2), "stale captured geometry cannot take over");
    } while (std::next_permutation(actions.begin(), actions.end()));

    // A mostly captured frame takes over even when a few instanced/indirect
    // draws are absent from the RT scene; a mostly uncaptured frame does not.
    FrameCaptureCoverage mostlyCaptured;
    mostlyCaptured.recordCapturedDraw(frame, 3000);
    mostlyCaptured.recordUncapturedDraw(frame, 36, 4);
    require(mostlyCaptured.canPresent(frame, true, frame), "mostly captured frame must take over");
    FrameCaptureCoverage mostlyUncaptured;
    mostlyUncaptured.recordCapturedDraw(frame, 36);
    mostlyUncaptured.recordUncapturedDraw(frame, 3000, 2);
    require(!mostlyUncaptured.canPresent(frame, true, frame), "mostly uncaptured frame must stay native");

    FrameCaptureCoverage zeros;
    zeros.recordCapturedDraw(frame);
    zeros.recordUncapturedDraw(frame, 0, 1);
    zeros.recordUncapturedDraw(frame, 1, 0);
    require(zeros.canPresent(frame, true, frame), "known zero-count draws must not disable captured output");
    zeros.recordUncapturedDraw(frame, UINT32_MAX, UINT32_MAX);
    require(!zeros.canPresent(frame, true, frame), "large nonzero counts must not overflow into empty");
  }
  std::puts("DX11 frame capture coverage passed: mixed/empty draws, all orderings, frame rollover and camera freshness.");
}
