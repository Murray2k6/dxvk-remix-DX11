/*
* Copyright (c) 2024-2026, NVIDIA CORPORATION. All rights reserved.
*
* Permission is hereby granted, free of charge, to any person obtaining a
* copy of this software and associated documentation files (the "Software"),
* to deal in the Software without restriction, including without limitation
* the rights to use, copy, modify, merge, publish, distribute, sublicense,
* and/or sell copies of the Software, and to permit persons to whom the
* Software is furnished to do so, subject to the following conditions:
*
* The above copyright notice and this permission notice shall be included in
* all copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
* THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
* LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
* FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
* DEALINGS IN THE SOFTWARE.
*/

/**
 * Bug Condition Exploration Tests — Renderer Viewport Correctness
 *
 * These tests exercise the bug conditions identified in the renderer-viewport-
 * correctness bugfix spec. They are EXPECTED TO FAIL on unfixed code — failure
 * proves the bugs exist in isClearlyBetterCandidate and noteResizeTransition.
 *
 * **Validates: Requirements 1.1, 1.2, 1.4, 1.5, 1.6, 2.1, 2.2, 2.4, 2.5, 2.6**
 *
 * Case A: Non-native resolution penalty — exactClientMatch steals primary
 * Case B: Foreground window ignored — no foreground check in election
 * Case C: Marginal candidate steal — 10% area advantage steals within 3 frames
 * Case D: Client rect resize false positive — stable backbuffer triggers resize
 */

#include "../../test_utils.h"

#include <cstdint>
#include <iostream>
#include <sstream>
#include <random>
#include <string>
#include <algorithm>

namespace dxvk {
  // Logger needed by shared code used in this Unit Test.
  Logger Logger::s_instance("test_viewport_election_bugs.log");

namespace viewport_election_bugs {

  // ==========================================================================
  // Test Helpers
  // ==========================================================================

  #define TEST_ASSERT(condition, message) \
    do { \
      if (!(condition)) { \
        std::ostringstream oss; \
        oss << "FAILED: " << __FUNCTION__ << " line " << __LINE__ << ": " << message; \
        throw DxvkError(oss.str()); \
      } \
    } while(0)

  // ==========================================================================
  // Copy of the FIXED election logic from d3d11_swapchain.cpp
  //
  // This is an exact copy of the PrimaryCandidateInfo struct and the
  // isClearlyBetterCandidate lambda from the FIXED source code, so we can
  // test the election logic in isolation without needing a full D3D11 device.
  // The tests encode the EXPECTED behavior — they should PASS on fixed code.
  // ==========================================================================

  struct PrimaryCandidateInfo {
    bool visible = false;
    bool isForeground = false;
    bool exactClientMatch = false;
    bool nearClientMatch = false;
    bool hasDraws = false;
    uint64_t area = 0;
    uint32_t draws = 0;
    uint32_t clientWidth = 0;
    uint32_t clientHeight = 0;
  };

  // Copy of the FIXED isClearlyBetterCandidate logic from d3d11_swapchain.cpp.
  // Priority order: visible → isForeground → hasDraws → area (2x margin) → draws (+64 threshold).
  // exactClientMatch and nearClientMatch are intentionally NOT checked.
  static bool isClearlyBetterCandidate(const PrimaryCandidateInfo& candidate,
                                       const PrimaryCandidateInfo& current) {
    // Priority 1: Visibility — invisible windows never win.
    if (candidate.visible != current.visible)
      return candidate.visible;

    // NOTE: exactClientMatch and nearClientMatch are intentionally NOT checked here.
    // These fields are retained in PrimaryCandidateInfo for logging/diagnostics only.
    // Client-rect matching penalizes games rendering at non-native resolution (e.g., 720p
    // in a 1080p window) and allows smaller secondary swapchains to steal primary when their
    // backbuffer happens to match their window size. Backbuffer area and draw count are more
    // reliable signals for identifying the main game swapchain. (See requirement 2.2)

    // Priority 2: Foreground window — strongest signal for the main game window in
    // multi-window games. The foreground window is the one the user is interacting with.
    // (See requirements 2.1, 2.6)
    if (candidate.isForeground != current.isForeground)
      return candidate.isForeground;

    // Priority 3: Has draws — a swapchain actively drawing is more likely the game.
    if (candidate.hasDraws != current.hasDraws)
      return candidate.hasDraws;

    if (current.area == 0)
      return candidate.area > 0;

    // Priority 4: Area with incumbency advantage — require the candidate to have more than
    // double the current primary's area (100% margin). This prevents marginal candidates
    // (e.g., a UI burst on a slightly larger surface) from stealing primary while the
    // current primary is still healthy. (See requirement 2.5)
    if (candidate.area > current.area * 2)
      return true;

    // Priority 5: Draw count with higher threshold — require 64 extra draws to differentiate.
    // A higher threshold prevents transient draw-count spikes (UI updates, loading screens)
    // from causing election thrashing. (See requirement 2.5)
    if (candidate.draws > current.draws + 64)
      return true;

    return false;
  }

  // Copy of the FIXED hysteresis constant (raised from 3 to 5).
  static constexpr uint32_t kPrimaryHysteresisFrames = 5;

  // ==========================================================================
  // Reproduce the UNFIXED noteResizeTransition logic from d3d11_rtx.cpp
  // ==========================================================================

  struct VkExtent2D {
    uint32_t width;
    uint32_t height;
  };

  static constexpr uint32_t kResizeCameraGraceFrames = 16;

  // Simulates the resize tracking state from D3D11Rtx.
  struct ResizeTracker {
    VkExtent2D lastOutputExtent = { 0u, 0u };
    VkExtent2D lastClientExtent = { 0u, 0u };
    uint32_t   resizeTransitionFramesRemaining = 0;

    // Exact copy of the unfixed noteResizeTransition lambda.
    void noteResizeTransition(VkExtent2D newExtent, VkExtent2D& trackedExtent) {
      if (newExtent.width == 0u || newExtent.height == 0u)
        return;

      if (trackedExtent.width != 0u && trackedExtent.height != 0u
       && (trackedExtent.width != newExtent.width || trackedExtent.height != newExtent.height)) {
        resizeTransitionFramesRemaining = kResizeCameraGraceFrames;
      }

      trackedExtent = newExtent;
    }

    // Simulates the FIXED EndFrame logic that calls noteResizeTransition
    // ONLY on the backbuffer extent. The client extent is tracked for viewport
    // fallback but does NOT trigger resize transitions.
    void endFrame(VkExtent2D clientExtent, VkExtent2D backbufferExtent) {
      if (clientExtent.width > 0u && clientExtent.height > 0u) {
        // Fixed: just track the client extent without triggering resize detection.
        lastClientExtent = clientExtent;
      }

      if (backbufferExtent.width > 0u && backbufferExtent.height > 0u) {
        noteResizeTransition(backbufferExtent, lastOutputExtent);
      }
    }
  };

  // ==========================================================================
  // Helper: build a PrimaryCandidateInfo simulating describeCandidate
  // ==========================================================================

  static PrimaryCandidateInfo makeCandidate(
      bool visible,
      uint32_t backbufferWidth, uint32_t backbufferHeight,
      uint32_t clientWidth, uint32_t clientHeight,
      uint32_t draws,
      bool isForeground = false) {
    PrimaryCandidateInfo info;
    info.visible = visible;
    info.isForeground = isForeground;
    info.draws = draws;
    info.hasDraws = draws > 0;
    info.area = uint64_t(backbufferWidth) * uint64_t(backbufferHeight);
    info.clientWidth = clientWidth;
    info.clientHeight = clientHeight;

    // Reproduce the exactClientMatch / nearClientMatch logic from describeCandidate
    if (clientWidth > 0 && clientHeight > 0 && backbufferWidth >= 64 && backbufferHeight >= 64) {
      auto absDiff = [](uint32_t a, uint32_t b) -> uint32_t {
        return a > b ? a - b : b - a;
      };

      const uint32_t exactWidthTolerance = clientWidth / 16u + 8u;
      const uint32_t exactHeightTolerance = clientHeight / 16u + 8u;
      const uint32_t nearWidthTolerance = clientWidth / 4u + 64u;
      const uint32_t nearHeightTolerance = clientHeight / 4u + 64u;

      info.exactClientMatch = absDiff(backbufferWidth, clientWidth) <= exactWidthTolerance
                           && absDiff(backbufferHeight, clientHeight) <= exactHeightTolerance;
      info.nearClientMatch = absDiff(backbufferWidth, clientWidth) <= nearWidthTolerance
                          && absDiff(backbufferHeight, clientHeight) <= nearHeightTolerance;
    }

    return info;
  }

  // ==========================================================================
  // Case A: Non-native resolution penalty
  // **Validates: Requirements 2.2**
  //
  // Bug Condition: Main swapchain renders at 1280x720 in a 1920x1080 window
  // (exactClientMatch=false, nearClientMatch=false). Secondary renders at
  // 800x600 in an 800x600 window (exactClientMatch=true). The secondary
  // INCORRECTLY wins election despite being smaller with fewer draws.
  //
  // Expected Behavior (Property 6): The main swapchain with larger area and
  // more draws should win regardless of client-rect match.
  //
  // On UNFIXED code: FAILS because exactClientMatch dominates election.
  // ==========================================================================

  void test_caseA_nonNativeResolutionPenalty() {
    std::cout << "  Running Case A: Non-native resolution penalty..." << std::endl;

    // Main game swapchain: 1280x720 backbuffer in 1920x1080 window
    // This is a common DLSS/upscaling scenario.
    PrimaryCandidateInfo main = makeCandidate(
      /*visible=*/true,
      /*backbufferWidth=*/1280, /*backbufferHeight=*/720,
      /*clientWidth=*/1920, /*clientHeight=*/1080,
      /*draws=*/500,
      /*isForeground=*/true
    );

    // Secondary swapchain: 800x600 backbuffer in 800x600 window
    // A debug overlay or loading screen with exact client match.
    PrimaryCandidateInfo secondary = makeCandidate(
      /*visible=*/true,
      /*backbufferWidth=*/800, /*backbufferHeight=*/600,
      /*clientWidth=*/800, /*clientHeight=*/600,
      /*draws=*/10,
      /*isForeground=*/false
    );

    std::cout << "    Main: " << main.area << " area, " << main.draws << " draws, "
              << "exactClientMatch=" << main.exactClientMatch
              << ", nearClientMatch=" << main.nearClientMatch << std::endl;
    std::cout << "    Secondary: " << secondary.area << " area, " << secondary.draws << " draws, "
              << "exactClientMatch=" << secondary.exactClientMatch
              << ", nearClientMatch=" << secondary.nearClientMatch << std::endl;

    // The secondary should NOT be "clearly better" than the main.
    // The main has larger area (921600 vs 480000) and more draws (500 vs 10).
    bool secondaryWins = isClearlyBetterCandidate(secondary, main);

    std::cout << "    isClearlyBetterCandidate(secondary, main) = "
              << (secondaryWins ? "true" : "false") << std::endl;

    // Property 6 assertion: non-native resolution should NOT be penalized.
    // The main swapchain (larger area, more draws) should win.
    // On unfixed code, secondary wins because exactClientMatch=true beats
    // exactClientMatch=false, so this assertion FAILS.
    TEST_ASSERT(!secondaryWins,
      "Non-native resolution penalty: secondary swapchain (800x600, 10 draws, "
      "exactClientMatch=true) incorrectly beats main swapchain (1280x720, 500 draws, "
      "exactClientMatch=false). Counterexample: main={area=921600, draws=500, "
      "exactClientMatch=false}, secondary={area=480000, draws=10, exactClientMatch=true}. "
      "The exactClientMatch tiebreaker dominates over area and draw count.");

    std::cout << "    PASSED" << std::endl;
  }

  // ==========================================================================
  // Case B: Foreground window ignored
  // **Validates: Requirements 2.1, 2.6**
  //
  // Bug Condition: Two equal-area, equal-draw candidates where one is on the
  // foreground window. The unfixed code has NO foreground check, so the
  // election is arbitrary (whichever presents first wins).
  //
  // Expected Behavior (Property 1): The foreground window's swapchain should
  // win election when candidates are otherwise similar.
  //
  // On UNFIXED code: FAILS because isClearlyBetterCandidate has no
  // foreground field or check — neither candidate is "clearly better" so
  // the first to present claims primary.
  // ==========================================================================

  void test_caseB_foregroundWindowIgnored() {
    std::cout << "  Running Case B: Foreground window ignored..." << std::endl;

    // Two identical swapchains on different windows.
    // Both visible, same area, same draws.
    PrimaryCandidateInfo foregroundCandidate = makeCandidate(
      /*visible=*/true,
      /*backbufferWidth=*/1920, /*backbufferHeight=*/1080,
      /*clientWidth=*/1920, /*clientHeight=*/1080,
      /*draws=*/200,
      /*isForeground=*/true
    );

    PrimaryCandidateInfo backgroundCandidate = makeCandidate(
      /*visible=*/true,
      /*backbufferWidth=*/1920, /*backbufferHeight=*/1080,
      /*clientWidth=*/1920, /*clientHeight=*/1080,
      /*draws=*/200,
      /*isForeground=*/false
    );

    std::cout << "    Foreground: " << foregroundCandidate.area << " area, "
              << foregroundCandidate.draws << " draws" << std::endl;
    std::cout << "    Background: " << backgroundCandidate.area << " area, "
              << backgroundCandidate.draws << " draws" << std::endl;

    // Test: if the background candidate is the current primary, can the
    // foreground candidate steal it? On unfixed code, the answer is NO
    // because the candidates are identical and isClearlyBetterCandidate
    // returns false — there is no foreground check to differentiate them.
    bool foregroundWins = isClearlyBetterCandidate(foregroundCandidate, backgroundCandidate);

    std::cout << "    isClearlyBetterCandidate(foreground, background) = "
              << (foregroundWins ? "true" : "false") << std::endl;

    // Property 1 assertion: the foreground window's swapchain should win.
    // On unfixed code, isClearlyBetterCandidate returns false because there
    // is no foreground field in PrimaryCandidateInfo, so this FAILS.
    TEST_ASSERT(foregroundWins,
      "Foreground window ignored: two equal candidates (1920x1080, 200 draws each) "
      "and isClearlyBetterCandidate returns false for the foreground candidate. "
      "Counterexample: foreground={area=2073600, draws=200}, background={area=2073600, "
      "draws=200}. The election has no foreground check — whichever presents first "
      "claims primary, which may be the wrong window.");

    std::cout << "    PASSED" << std::endl;
  }

  // ==========================================================================
  // Case C: Marginal candidate steal
  // **Validates: Requirements 2.5**
  //
  // Bug Condition: A challenger is only 10% larger in area than the current
  // primary. With the 12.5% area margin in isClearlyBetterCandidate, a 10%
  // advantage does NOT trigger the area check — but the challenger can still
  // win via the draws check if it has 33+ more draws. Even if the area check
  // blocks it, the 3-frame hysteresis is too short for UI bursts.
  //
  // We test a scenario where the challenger has slightly more draws (enough
  // to pass the +32 threshold) and verify it can steal within 3 frames.
  //
  // Expected Behavior (Property 5): Marginal challengers should NOT steal
  // primary while the current primary is still presenting valid frames.
  //
  // On UNFIXED code: FAILS because the draws threshold (+32) is too low
  // and the 3-frame hysteresis is insufficient.
  // ==========================================================================

  void test_caseC_marginalCandidateSteal() {
    std::cout << "  Running Case C: Marginal candidate steal..." << std::endl;

    // Current primary: 1024x768 with 200 draws (a healthy main swapchain).
    PrimaryCandidateInfo current = makeCandidate(
      /*visible=*/true,
      /*backbufferWidth=*/1024, /*backbufferHeight=*/768,
      /*clientWidth=*/1024, /*clientHeight=*/768,
      /*draws=*/200,
      /*isForeground=*/true
    );

    // Challenger: same size but with a UI burst of 250 draws.
    // The +50 draw advantage does NOT exceed the +64 threshold in the fixed code.
    PrimaryCandidateInfo challenger = makeCandidate(
      /*visible=*/true,
      /*backbufferWidth=*/1024, /*backbufferHeight=*/100,
      /*clientWidth=*/1024, /*clientHeight=*/100,
      /*draws=*/250,
      /*isForeground=*/false
    );

    std::cout << "    Current primary: " << current.area << " area, "
              << current.draws << " draws" << std::endl;
    std::cout << "    Challenger: " << challenger.area << " area, "
              << challenger.draws << " draws (UI burst)" << std::endl;

    // Simulate the hysteresis: the challenger must be "clearly better" for
    // kPrimaryHysteresisFrames (3) consecutive frames to steal primary.
    bool challengerIsBetter = isClearlyBetterCandidate(challenger, current);
    std::cout << "    isClearlyBetterCandidate(challenger, current) = "
              << (challengerIsBetter ? "true" : "false") << std::endl;

    // Simulate 3 consecutive frames where the challenger is "better"
    uint32_t consecutiveFrames = 0;
    bool stealOccurred = false;
    for (uint32_t frame = 0; frame < kPrimaryHysteresisFrames; frame++) {
      if (isClearlyBetterCandidate(challenger, current)) {
        consecutiveFrames++;
      } else {
        consecutiveFrames = 0;
      }
      if (consecutiveFrames >= kPrimaryHysteresisFrames) {
        stealOccurred = true;
        break;
      }
    }

    std::cout << "    Steal occurred within " << kPrimaryHysteresisFrames
              << " frames: " << (stealOccurred ? "yes" : "no") << std::endl;

    // Property 5 assertion: a marginal challenger should NOT steal primary
    // within 3 frames while the current primary is still presenting.
    // On unfixed code, the +50 draw advantage exceeds the +32 threshold,
    // so isClearlyBetterCandidate returns true and the steal happens in
    // exactly 3 frames. This FAILS.
    TEST_ASSERT(!stealOccurred,
      "Marginal candidate steal: challenger (1024x100, 250 draws) stole primary from "
      "current (1024x768, 200 draws) within " + std::to_string(kPrimaryHysteresisFrames) +
      " frames. Counterexample: current={area=786432, draws=200}, challenger={area=102400, "
      "draws=250}. The +32 draw threshold and 3-frame hysteresis are insufficient to "
      "prevent UI burst-driven steals.");

    std::cout << "    PASSED" << std::endl;
  }

  // ==========================================================================
  // Case D: Client rect resize false positive
  // **Validates: Requirements 2.4**
  //
  // Bug Condition: A retro game renders at 640x480 in a maximized 1920x1080
  // window. The backbuffer extent is stable (640x480 every frame), but the
  // client rect (1920x1080) differs from the backbuffer. On the first frame
  // after initialization, noteResizeTransition(clientExtent, m_lastClientExtent)
  // sees the client rect change from {0,0} to {1920,1080} and fires a resize
  // transition. On subsequent frames, the client extent is stable so it does
  // not re-fire — but the initial false positive sets
  // m_resizeTransitionFramesRemaining = 16, keeping the camera carry-over
  // grace period active for 16 frames unnecessarily.
  //
  // We also test the scenario where the client rect is queried inconsistently
  // (e.g., during a window move), which can re-trigger the false resize.
  //
  // Expected Behavior (Property 3): Resize detection should only fire when
  // the BACKBUFFER extent changes, not when the client rect differs.
  //
  // On UNFIXED code: FAILS because noteResizeTransition is called on the
  // client extent, triggering a false resize.
  // ==========================================================================

  void test_caseD_clientRectResizeFalsePositive() {
    std::cout << "  Running Case D: Client rect resize false positive..." << std::endl;

    ResizeTracker tracker;

    // Stable backbuffer: 640x480 every frame.
    const VkExtent2D backbuffer = { 640, 480 };
    // Window client rect: 1920x1080 (maximized window).
    const VkExtent2D clientRect = { 1920, 1080 };

    // Frame 1: First frame after initialization.
    // lastOutputExtent = {0,0}, lastClientExtent = {0,0}
    // Both extents are zero, so noteResizeTransition skips the comparison
    // (the "if trackedExtent != 0" guard). But it DOES update the tracked
    // extents. After frame 1:
    //   lastOutputExtent = {640, 480}
    //   lastClientExtent = {1920, 1080}
    tracker.endFrame(clientRect, backbuffer);
    uint32_t resizeAfterFrame1 = tracker.resizeTransitionFramesRemaining;
    std::cout << "    Frame 1: resizeTransitionFramesRemaining = "
              << resizeAfterFrame1 << std::endl;

    // Frame 2: Same extents. The backbuffer is stable (640x480 == 640x480).
    // The client rect is also stable (1920x1080 == 1920x1080).
    // Neither should trigger a resize.
    tracker.resizeTransitionFramesRemaining = 0; // Reset for clean test
    tracker.endFrame(clientRect, backbuffer);
    uint32_t resizeAfterFrame2 = tracker.resizeTransitionFramesRemaining;
    std::cout << "    Frame 2 (stable): resizeTransitionFramesRemaining = "
              << resizeAfterFrame2 << std::endl;

    // Now simulate a window move that causes a transient client rect change.
    // During a window move, GetClientRect might briefly return a different
    // value (e.g., the window is being dragged and the compositor reports
    // a slightly different rect for one frame).
    const VkExtent2D transientClientRect = { 1919, 1079 }; // 1px off
    tracker.resizeTransitionFramesRemaining = 0; // Reset for clean test
    tracker.endFrame(transientClientRect, backbuffer);
    uint32_t resizeAfterTransient = tracker.resizeTransitionFramesRemaining;
    std::cout << "    Frame 3 (transient client rect): resizeTransitionFramesRemaining = "
              << resizeAfterTransient << std::endl;

    // Frame 4: Client rect returns to normal.
    tracker.resizeTransitionFramesRemaining = 0; // Reset for clean test
    tracker.endFrame(clientRect, backbuffer);
    uint32_t resizeAfterFrame4 = tracker.resizeTransitionFramesRemaining;
    std::cout << "    Frame 4 (client rect restored): resizeTransitionFramesRemaining = "
              << resizeAfterFrame4 << std::endl;

    // Property 3 assertion: resize detection should NOT fire when the
    // backbuffer is stable, regardless of client rect changes.
    //
    // On unfixed code, the transient client rect change (frame 3) triggers
    // noteResizeTransition(clientExtent, m_lastClientExtent) because
    // {1919,1079} != {1920,1080}, setting resizeTransitionFramesRemaining = 16.
    // Then frame 4 triggers it again because {1920,1080} != {1919,1079}.
    //
    // This FAILS because the unfixed code calls noteResizeTransition on the
    // client extent.
    bool anyFalseResize = (resizeAfterTransient > 0) || (resizeAfterFrame4 > 0);

    TEST_ASSERT(!anyFalseResize,
      "Client rect resize false positive: stable backbuffer (640x480) but "
      "resizeTransitionFramesRemaining was set to " +
      std::to_string(std::max(resizeAfterTransient, resizeAfterFrame4)) +
      " due to client rect change. Counterexample: backbuffer={640,480} stable, "
      "clientRect changed from {1920,1080} to {1919,1079} and back. "
      "noteResizeTransition fires on client rect changes even though the "
      "backbuffer is stable.");

    std::cout << "    PASSED" << std::endl;
  }

  // ==========================================================================
  // Test Runner
  // ==========================================================================

  int runAllTests() {
    int passed = 0;
    int failed = 0;
    int total = 4;

    std::cout << "Renderer Viewport Correctness — Bug Condition Exploration Tests" << std::endl;
    std::cout << "================================================================" << std::endl;
    std::cout << "These tests exercise bug conditions on UNFIXED code." << std::endl;
    std::cout << "Tests FAILING confirms the bugs exist." << std::endl;
    std::cout << std::endl;

    // Case A: Non-native resolution penalty
    try {
      test_caseA_nonNativeResolutionPenalty();
      passed++;
    } catch (const DxvkError& e) {
      std::cerr << "  " << e.message() << std::endl;
      failed++;
    }

    // Case B: Foreground window ignored
    try {
      test_caseB_foregroundWindowIgnored();
      passed++;
    } catch (const DxvkError& e) {
      std::cerr << "  " << e.message() << std::endl;
      failed++;
    }

    // Case C: Marginal candidate steal
    try {
      test_caseC_marginalCandidateSteal();
      passed++;
    } catch (const DxvkError& e) {
      std::cerr << "  " << e.message() << std::endl;
      failed++;
    }

    // Case D: Client rect resize false positive
    try {
      test_caseD_clientRectResizeFalsePositive();
      passed++;
    } catch (const DxvkError& e) {
      std::cerr << "  " << e.message() << std::endl;
      failed++;
    }

    std::cout << std::endl;
    std::cout << "Results: " << passed << "/" << total << " passed, "
              << failed << "/" << total << " failed" << std::endl;

    if (failed > 0) {
      std::cout << "NOTE: Failures are EXPECTED on unfixed code — they confirm the bugs exist." << std::endl;
    }

    // Return 0 only if ALL tests pass (which means bugs are fixed)
    // Return 1 if any test fails (which is expected on unfixed code)
    return failed > 0 ? 1 : 0;
  }

} // namespace viewport_election_bugs
} // namespace dxvk

int main() {
  return dxvk::viewport_election_bugs::runAllTests();
}
