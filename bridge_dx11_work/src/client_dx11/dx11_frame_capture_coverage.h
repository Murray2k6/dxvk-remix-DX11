#pragma once

#include <cstdint>

namespace dx11_capture {

// Access is serialized with the capture state. Frame IDs keep a late draw or
// stale camera from authorizing presentation of a different frame.
//
// Coverage is proportional, not all-or-nothing: draws the bridge cannot
// represent (multi-instance, indirect, DrawAuto, command lists) are simply
// absent from the ray-traced scene, the same policy the native x64 path uses.
// Requiring zero such draws kept nearly every modern engine on native raster,
// because they all issue at least one instanced or indirect draw per frame.
// The Remix output takes over once captured geometry is at least as large as
// the uncaptured remainder, so a frame that is mostly unrepresented stays native.
class FrameCaptureCoverage {
public:
  void recordCapturedDraw(uint64_t frame, uint32_t primitiveElements = 1) {
    resetIfNewFrame(frame);
    m_hasCaptured = true;
    m_capturedElements = saturatingAdd(m_capturedElements, primitiveElements);
  }

  void recordUncapturedDraw(uint64_t frame, uint32_t primitiveElements, uint32_t instances) {
    if (primitiveElements == 0 || instances == 0)
      return;
    resetIfNewFrame(frame);
    m_uncapturedElements = saturatingAdd(m_uncapturedElements,
      uint64_t(primitiveElements) * uint64_t(instances));
  }

  bool canPresent(uint64_t frame, bool cameraValid, uint64_t cameraFrame) const {
    return cameraValid && cameraFrame == frame
      && m_hasFrame && m_frame == frame
      && m_hasCaptured
      && m_capturedElements >= m_uncapturedElements;
  }

private:
  static uint64_t saturatingAdd(uint64_t a, uint64_t b) {
    return a > UINT64_MAX - b ? UINT64_MAX : a + b;
  }

  void resetIfNewFrame(uint64_t frame) {
    if (m_hasFrame && m_frame == frame)
      return;
    m_hasFrame = true;
    m_frame = frame;
    m_hasCaptured = false;
    m_capturedElements = 0;
    m_uncapturedElements = 0;
  }

  uint64_t m_frame = 0;
  uint64_t m_capturedElements = 0;
  uint64_t m_uncapturedElements = 0;
  bool m_hasFrame = false;
  bool m_hasCaptured = false;
};

}
