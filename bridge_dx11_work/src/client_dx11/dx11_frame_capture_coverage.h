#pragma once

#include <cstdint>

namespace dx11_capture {

// Access is serialized with the capture state. Frame IDs keep a late draw or
// stale camera from authorizing presentation of a different frame.
class FrameCaptureCoverage {
public:
  void recordCapturedDraw(uint64_t frame) {
    m_capturedFrame = frame;
    m_hasCaptured = true;
  }

  void recordUncapturedDraw(uint64_t frame, uint32_t primitiveElements, uint32_t instances) {
    if (primitiveElements == 0 || instances == 0)
      return;
    m_uncapturedFrame = frame;
    m_hasUncaptured = true;
  }

  bool canPresent(uint64_t frame, bool cameraValid, uint64_t cameraFrame) const {
    return cameraValid && cameraFrame == frame
      && m_hasCaptured && m_capturedFrame == frame
      && !(m_hasUncaptured && m_uncapturedFrame == frame);
  }

private:
  uint64_t m_capturedFrame = 0;
  uint64_t m_uncapturedFrame = 0;
  bool m_hasCaptured = false;
  bool m_hasUncaptured = false;
};

}
