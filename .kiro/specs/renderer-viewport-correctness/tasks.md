# Implementation Plan

- [x] 1. Write bug condition exploration tests
  - **Property 1: Bug Condition** - Election Prefers Foreground Window and Ignores Client-Rect Match
  - **CRITICAL**: This test MUST FAIL on unfixed code — failure confirms the bugs exist
  - **DO NOT attempt to fix the test or the code when it fails**
  - **NOTE**: This test encodes the expected behavior — it will validate the fix when it passes after implementation
  - **GOAL**: Surface counterexamples that demonstrate the election bugs exist in `isClearlyBetterCandidate`
  - **Scoped PBT Approach**: Scope the property to concrete failing cases from the design:
    - Case A (Non-native resolution penalty): Construct a main swapchain candidate with 1280×720 backbuffer in a 1920×1080 window (`exactClientMatch=false`, `nearClientMatch=false`) and a secondary with 800×600 backbuffer in an 800×600 window (`exactClientMatch=true`). Assert the main swapchain (larger area, more draws) wins election — will FAIL on unfixed code because the secondary wins on `exactClientMatch`.
    - Case B (Foreground window ignored): Construct two equal-area, equal-draw candidates where one is on the foreground window. Assert the foreground one wins — will FAIL on unfixed code because `isClearlyBetterCandidate` has no foreground check.
    - Case C (Marginal candidate steal): Simulate a challenger that is only 10% larger in area than the current primary. Assert the challenger does NOT steal primary within 3 frames — will FAIL on unfixed code because the 12.5% margin and 3-frame hysteresis are insufficient.
    - Case D (Client rect resize false positive): Call `noteResizeTransition` with a stable backbuffer extent but a client rect that differs from the backbuffer. Assert resize transition does NOT fire — will FAIL on unfixed code because `noteResizeTransition(clientExtent, m_lastClientExtent)` triggers a false resize.
  - Write property-based tests for each case using the Bug Condition pseudocode (`isBugCondition`) from the design
  - The test assertions should match the Expected Behavior Properties from the design (Properties 1, 3, 5, 6)
  - Run tests on UNFIXED code
  - **EXPECTED OUTCOME**: Tests FAIL (this is correct — it proves the bugs exist)
  - Document counterexamples found to understand root cause
  - Mark task complete when tests are written, run, and failures are documented
  - _Requirements: 1.1, 1.2, 1.4, 1.5, 1.6, 2.1, 2.2, 2.4, 2.5, 2.6_

- [x] 2. Write preservation property tests (BEFORE implementing fix)
  - **Property 2: Preservation** - Single-Swapchain Election and Existing Behavior
  - **IMPORTANT**: Follow observation-first methodology — run UNFIXED code first, observe behavior, then write tests
  - Observe on UNFIXED code:
    - Single-swapchain game: a lone swapchain is elected primary immediately on first present (no hysteresis delay)
    - Alt+X override: forced primary override immediately claims primary, bypassing all election heuristics
    - Destroyed primary: when the primary swapchain is destroyed, the next presenter claims primary immediately
    - Actual backbuffer resize: when the backbuffer extent changes between frames, `noteResizeTransition` fires and sets `m_resizeTransitionFramesRemaining`
    - Dominant candidate: when one swapchain is dramatically better (2x+ area AND 2x+ draws), it wins election regardless of other factors
  - Write property-based tests capturing observed behavior patterns from Preservation Requirements:
    - For all single-swapchain inputs, election produces the same result (immediate primary)
    - For all Alt+X override inputs, forced primary is respected
    - For all destroyed-primary inputs, next presenter claims primary immediately
    - For all actual-backbuffer-resize inputs, resize transition fires exactly once
    - For all non-buggy multi-swapchain inputs (where the dominant candidate is clearly better by area AND draws), election produces the same winner as unfixed code
  - Property-based testing generates many test cases for stronger preservation guarantees
  - Run tests on UNFIXED code
  - **EXPECTED OUTCOME**: Tests PASS (this confirms baseline behavior to preserve)
  - Mark task complete when tests are written, run, and passing on unfixed code
  - _Requirements: 3.1, 3.2, 3.3, 3.4, 3.5, 3.6, 3.7, 3.8_

- [x] 3. Fix election logic in `d3d11_swapchain.cpp`

  - [x] 3.1 Add `isForeground` field to `PrimaryCandidateInfo` and populate it in `describeCandidate`
    - Add `bool isForeground = false;` to the `PrimaryCandidateInfo` struct
    - In `describeCandidate`, set `isForeground = (sc->m_window == GetForegroundWindow())`
    - This provides the strongest signal for identifying the main game window in multi-window games
    - _Bug_Condition: isBugCondition(input) where two swapchains have similar visibility/area/draws but only one is on the foreground window_
    - _Expected_Behavior: The foreground window's swapchain wins election when candidates are otherwise similar_
    - _Preservation: Single-swapchain games unaffected (only one candidate, foreground check is irrelevant)_
    - _Requirements: 2.1, 2.6_

  - [x] 3.2 Remove `exactClientMatch` and `nearClientMatch` from election priority in `isClearlyBetterCandidate`
    - Remove or comment out the `exactClientMatch` and `nearClientMatch` comparisons from `isClearlyBetterCandidate`
    - These fields may be kept in the struct for logging/diagnostics but MUST NOT influence the election outcome
    - Backbuffer area and draw count are more reliable signals than client-rect matching
    - _Bug_Condition: isBugCondition(input) where main swapchain has non-native resolution (backbuffer != client rect) and secondary has exactClientMatch=true_
    - _Expected_Behavior: Election does not penalize non-native resolution; area and draws dominate_
    - _Preservation: Games where backbuffer matches client rect are unaffected (the removed check was a bonus they no longer need)_
    - _Requirements: 2.2_

  - [x] 3.3 Restructure `isClearlyBetterCandidate` priority order and add incumbency advantage
    - New priority order: `visible` → `isForeground` → `hasDraws` → `area` (with incumbency bonus) → `draws` (with higher threshold)
    - For area comparison: require `candidate.area > current.area * 2` (100% margin) when the current primary is still presenting, instead of the current 12.5% margin
    - For draws comparison: require more than 32 extra draws to differentiate (raise from current threshold)
    - The incumbency advantage prevents marginal candidates from stealing primary while the current primary is still healthy
    - _Bug_Condition: isBugCondition(input) where challenger is only marginally better (10% more area) and steals primary within 3 frames_
    - _Expected_Behavior: Marginal challengers cannot steal primary; only dramatically better candidates (2x area) can overcome incumbency_
    - _Preservation: When one candidate is dramatically better, it still wins — incumbency does not prevent legitimate transitions_
    - _Requirements: 2.1, 2.2, 2.5, 2.6_

  - [x] 3.4 Raise `kPrimaryHysteresisFrames` from 3 to 5
    - Change the constant from 3 to 5 to require a longer sustained advantage before allowing a primary switch
    - At 60fps, this raises the threshold from ~50ms to ~83ms, preventing UI bursts (which typically last 4–6 frames) from causing election thrashing
    - _Bug_Condition: isBugCondition(input) where a UI burst lasting 4 frames causes election thrashing with 3-frame hysteresis_
    - _Expected_Behavior: 5-frame hysteresis prevents marginal burst-driven steals_
    - _Preservation: The hysteresis mechanism itself is preserved (raised bar, not removed); single-swapchain games bypass hysteresis entirely_
    - _Requirements: 2.5_

  - [x] 3.5 Verify bug condition exploration tests now pass (election logic)
    - **Property 1: Expected Behavior** - Election Prefers Foreground Window and Ignores Client-Rect Match
    - **IMPORTANT**: Re-run the SAME tests from task 1 (Cases A, B, C) — do NOT write new tests
    - The tests from task 1 encode the expected behavior for election
    - When these tests pass, it confirms the election bugs are fixed
    - Run bug condition exploration tests from step 1 (Cases A, B, C only — Case D is resize-related)
    - **EXPECTED OUTCOME**: Tests PASS (confirms election bugs are fixed)
    - _Requirements: 2.1, 2.2, 2.5, 2.6_

  - [x] 3.6 Verify preservation tests still pass (election logic)
    - **Property 2: Preservation** - Single-Swapchain Election and Existing Behavior
    - **IMPORTANT**: Re-run the SAME tests from task 2 — do NOT write new tests
    - Run preservation property tests from step 2
    - **EXPECTED OUTCOME**: Tests PASS (confirms no regressions in election behavior)
    - Confirm all preservation tests still pass after election logic changes

- [x] 4. Fix resize detection in `d3d11_rtx.cpp`

  - [x] 4.1 Remove `noteResizeTransition(clientExtent, m_lastClientExtent)` from `D3D11Rtx::EndFrame`
    - Remove the `noteResizeTransition` call that compares client rect extents in `EndFrame`
    - Keep the `m_lastClientExtent` assignment so the client extent is still tracked for viewport fallback
    - Only the backbuffer extent comparison (`noteResizeTransition` on `m_lastOutputExtent`) should trigger resize transitions
    - _Bug_Condition: isBugCondition(input) where backbuffer is stable but client rect differs, causing false resize detection_
    - _Expected_Behavior: noteResizeTransition does not fire when only the client rect differs from the backbuffer_
    - _Preservation: Actual backbuffer resizes still trigger resize transitions via the backbuffer extent comparison_
    - _Requirements: 2.4_

  - [x] 4.2 Remove `noteResizeTransition(clientExtent, m_lastClientExtent)` from `D3D11Rtx::OnPresent`
    - Same change as 4.1 but in the `OnPresent` function
    - Remove the `noteResizeTransition` call that compares client rect extents
    - Keep the `m_lastClientExtent` assignment for viewport fallback
    - _Bug_Condition: Same as 4.1 — client rect comparison triggers false resize in OnPresent path_
    - _Expected_Behavior: Same as 4.1 — only backbuffer extent changes trigger resize transitions_
    - _Preservation: Same as 4.1 — actual backbuffer resizes still detected correctly_
    - _Requirements: 2.4_

  - [x] 4.3 Verify bug condition exploration test now passes (resize detection)
    - **Property 1: Expected Behavior** - Resize Detection Uses Backbuffer Extent Only
    - **IMPORTANT**: Re-run the SAME test from task 1 (Case D) — do NOT write a new test
    - The test from task 1 Case D encodes the expected behavior for resize detection
    - When this test passes, it confirms the resize detection bug is fixed
    - Run bug condition exploration test from step 1 (Case D only)
    - **EXPECTED OUTCOME**: Test PASSES (confirms resize detection bug is fixed)
    - _Requirements: 2.4_

  - [x] 4.4 Verify preservation tests still pass (resize detection)
    - **Property 2: Preservation** - Actual Backbuffer Resize Still Detected
    - **IMPORTANT**: Re-run the SAME tests from task 2 — do NOT write new tests
    - Run preservation property tests from step 2
    - **EXPECTED OUTCOME**: Tests PASS (confirms actual backbuffer resizes still trigger transitions correctly)
    - Confirm all preservation tests still pass after resize detection changes

- [x] 5. Document separation of concerns in `d3d11_rtx.h`

  - [x] 5.1 Add documentation comments to `m_lastClientExtent` and `m_lastOutputExtent`
    - Add a comment to `m_lastClientExtent` clarifying: "Used only for viewport fallback — NOT for resize detection. The client rect may differ from the backbuffer extent (e.g., 640×480 game in a 1920×1080 window) and must not trigger resize transitions."
    - Add a comment to `m_lastOutputExtent` clarifying: "Sole source of truth for resize transition detection. Only changes to this extent trigger `m_resizeTransitionFramesRemaining` and `resetScreenResolution`."
    - _Requirements: 2.3, 2.4_

- [x] 6. Checkpoint — Ensure all tests pass
  - Run the full test suite (bug condition exploration tests + preservation property tests)
  - Verify all bug condition tests from task 1 now PASS (confirming all 6 bugs are fixed)
  - Verify all preservation tests from task 2 still PASS (confirming no regressions)
  - Verify the build compiles cleanly with no warnings related to the changed files
  - Ensure all tests pass, ask the user if questions arise
