# Renderer Viewport Correctness — Bugfix Design

## Overview

This design addresses six interrelated bugs in the DXVK Remix primary swapchain election and resolution handling logic. The core problem is that the election heuristics in `isClearlyBetterCandidate` over-weight client-rect matching, lack foreground-window awareness, and use insufficient hysteresis — causing the wrong swapchain to be elected primary in multi-swapchain games. Additionally, the resize detection in `D3D11Rtx::EndFrame` and `OnPresent` conflates the window client rect with the backbuffer extent, misinterpreting stable low-resolution games as continuously resizing. The fix strategy refines the existing election logic (building on the 3-frame hysteresis already added), adds foreground-window preference, removes the client-rect-match penalty, and makes resize detection compare backbuffer-to-backbuffer instead of backbuffer-to-client-rect.

## Glossary

- **Bug_Condition (C)**: The set of conditions that trigger incorrect primary election or stale resolution — e.g., a secondary swapchain wins election because its backbuffer matches its client rect better, or a stable 720p game triggers resize transitions every frame because the client rect is 1080p.
- **Property (P)**: The desired correct behavior — e.g., the foreground window's swapchain wins election regardless of resolution mismatch, or resize detection only fires when the backbuffer extent actually changes.
- **Preservation**: Existing behaviors that must remain unchanged — e.g., single-swapchain games elect primary immediately, Alt+X forced override works, DLSS derives render resolution from the primary backbuffer extent.
- **`isClearlyBetterCandidate`**: Lambda in `D3D11SwapChain::PresentImage` (d3d11_swapchain.cpp) that compares two `PrimaryCandidateInfo` structs to decide if a challenger should steal primary. Currently checks: visibility > exactClientMatch > nearClientMatch > hasDraws > area > draws.
- **`describeCandidate`**: Lambda in `D3D11SwapChain::PresentImage` that builds a `PrimaryCandidateInfo` from a swapchain's window state, backbuffer dimensions, and draw count.
- **`PrimaryCandidateInfo`**: Struct holding election-relevant properties: visible, exactClientMatch, nearClientMatch, hasDraws, area, draws, clientWidth, clientHeight.
- **`noteResizeTransition`**: Lambda in `D3D11Rtx::EndFrame` and `OnPresent` that detects when an extent changes and sets `m_resizeTransitionFramesRemaining` to trigger camera carry-over grace period.
- **`kPrimaryHysteresisFrames`**: Currently 3 — the number of consecutive frames a challenger must be "clearly better" before stealing primary. Added by the previous bugfix spec.
- **`m_lastOutputExtent`**: Tracked backbuffer extent in `D3D11Rtx`, updated each frame from the present image.
- **`m_lastClientExtent`**: Tracked window client rect extent in `D3D11Rtx`, updated each frame from `getClientExtent()`.

## Bug Details

### Bug Condition

The bugs manifest when a game creates multiple swapchains and the election heuristics select the wrong one as primary, or when the resize detection logic misinterprets a stable low-resolution game as continuously resizing.

**Formal Specification:**
```
FUNCTION isBugCondition(input)
  INPUT: input of type {swapchains: SwapchainState[], resizeState: ResizeState}
  OUTPUT: boolean

  // Bug 1: Wrong window elected primary
  LET main := swapchain with foreground/topmost window
  LET elected := swapchain selected by isClearlyBetterCandidate
  IF main != elected
     AND main.visible AND main.hasDraws
     AND elected won due to exactClientMatch or nearClientMatch advantage
  THEN RETURN true

  // Bug 2: Client-rect match penalizes non-native resolution
  IF main.backbufferSize != main.clientRectSize
     AND main loses election because exactClientMatch = false
     AND competitor.exactClientMatch = true
  THEN RETURN true

  // Bug 3: Stale target dimensions on resize
  IF main.backbufferResolutionChangedThisFrame
     AND onFrameBegin receives previous frame's extent
  THEN RETURN true

  // Bug 4: Client rect vs backbuffer confusion in resize detection
  IF main.backbufferExtent is stable across frames
     AND main.clientRectExtent != main.backbufferExtent
     AND noteResizeTransition fires due to clientRect comparison
  THEN RETURN true

  // Bug 5: Insufficient hysteresis for marginal candidates
  IF challenger is only marginally better (e.g., 10% more area)
     AND challenger steals primary within kPrimaryHysteresisFrames
     AND primary was still presenting valid frames
  THEN RETURN true

  // Bug 6: No foreground window distinction
  IF two swapchains have similar visibility, area, and draws
     AND main window is foreground but election picks the other
     AND isClearlyBetterCandidate does not check foreground status
  THEN RETURN true

  RETURN false
END FUNCTION
```

### Examples

- **Bug 1 (Wrong window)**: Unity game creates a 1920×1080 main swapchain and a 640×480 debug overlay swapchain. The debug window's backbuffer exactly matches its client rect (640×480). The main game renders at 1280×720 internal resolution in a 1920×1080 window, so `exactClientMatch = false`. The debug swapchain wins on the `exactClientMatch` tiebreaker despite being smaller and less important.
- **Bug 2 (Resolution penalty)**: Skyrim SE renders at 1280×720 with DLSS upscaling to 1920×1080 window. The swapchain backbuffer is 1280×720, client rect is 1920×1080. `exactClientMatch = false`, `nearClientMatch = false` (720 vs 1080 exceeds the 25%+64px tolerance). A loading-screen swapchain at 800×600 in an 800×600 window gets `exactClientMatch = true` and steals primary.
- **Bug 3 (Stale dimensions)**: Player toggles fullscreen from 1920×1080 to 2560×1440. `ChangeProperties` updates `m_desc` but the current frame's `injectRTX` already captured `targetImage->info().extent` as 1920×1080. `onFrameBegin` allocates ray tracing resources at the old resolution, then `validateRaytracingOutput` fails on the next frame, forcing an expensive inline `resetScreenResolution`.
- **Bug 4 (Client rect confusion)**: A retro game renders at 640×480 in a maximized 1920×1080 window. Every frame, `noteResizeTransition(clientExtent={1920,1080}, m_lastClientExtent)` sees the client rect is different from the backbuffer-derived `m_lastOutputExtent`, and the comparison between the two tracked extents triggers `m_resizeTransitionFramesRemaining = 16` repeatedly, keeping the camera carry-over grace period permanently active.
- **Bug 5 (Marginal hysteresis)**: An emulator creates two swapchains: main at 1024×768 with 200 draws, and a status bar at 1024×100 with 50 draws. The status bar occasionally gets a burst of 250 draws during a UI update. With only 3 frames of hysteresis, the status bar can steal primary during the burst, then the main swapchain steals it back — causing 2 resolution transitions and visible flicker.
- **Bug 6 (No foreground check)**: A game creates two equal-sized 1920×1080 swapchains on two monitors. Both are visible, both have draws. The user's game window is the foreground window, but `isClearlyBetterCandidate` has no foreground check, so whichever swapchain presents first claims primary — which may be the wrong monitor.

## Expected Behavior

### Preservation Requirements

**Unchanged Behaviors:**
- Single-swapchain games must continue to elect primary immediately on the first present call without any hysteresis delay
- Alt+X forced primary override must continue to immediately claim primary, bypassing all election heuristics and hysteresis
- When the primary swapchain is destroyed, the next swapchain to present must continue to be elected primary immediately (no hysteresis for the "no current primary" case)
- The Remix UI overlay must continue to render only on the primary swapchain via `gui.render()`, and only the primary chain must drive RT frame boundaries via `endFrame` / `onPresent`
- DLSS, NIS, XeSS, and TAAU upscaling must continue to derive the render resolution from the primary swapchain's backbuffer extent via `targetImage->info().extent` in `onFrameBegin`
- `incrementPresentCount` must continue to be called only for the primary chain
- WndProc hook installation on the primary swapchain's window must continue to function correctly
- UI state mirroring from old primary to new primary on device change must continue to work
- The 3-frame hysteresis mechanism (from the previous bugfix) must remain as the baseline — this design raises the bar but does not remove hysteresis

**Scope:**
All inputs that do NOT involve multi-swapchain election contention or backbuffer/client-rect size mismatches should be completely unaffected by this fix. This includes:
- Games with a single swapchain
- Games where the backbuffer matches the client rect exactly
- Games that do not resize during gameplay
- All non-election code paths (draw call processing, texture management, RT pipeline)

## Hypothesized Root Cause

Based on the bug description and code analysis, the most likely issues are:

1. **Client-rect match dominates election priority**: In `isClearlyBetterCandidate`, `exactClientMatch` and `nearClientMatch` are checked immediately after `visible`, before `hasDraws` and `area`. This means a tiny debug window whose backbuffer matches its client rect will beat a large game window whose backbuffer doesn't match. The fix is to remove or deprioritize client-rect matching — backbuffer area and draw count are more reliable signals of the "main" swapchain.

2. **No foreground window signal**: `describeCandidate` checks `IsWindowVisible` and `!IsIconic` but never calls `GetForegroundWindow()`. In multi-window games, the foreground window is the strongest signal of which window the user is interacting with. Adding a `isForeground` field to `PrimaryCandidateInfo` and checking it early in `isClearlyBetterCandidate` would resolve bugs 1 and 6.

3. **Resize detection compares heterogeneous extents**: In `D3D11Rtx::EndFrame`, `noteResizeTransition` is called separately for `clientExtent` (window size) and backbuffer extent, each tracking their own `m_lastClientExtent` / `m_lastOutputExtent`. But the resize transition flag `m_resizeTransitionFramesRemaining` is shared — a change in *either* extent triggers the grace period. For a stable low-res game, the client rect is always different from the backbuffer, but neither is *changing* frame-to-frame. The issue is that the initial population of `m_lastClientExtent` (from 0,0 to the real value) triggers a false resize on the first frame, and if the client rect is queried inconsistently (e.g., during a window move), it can re-trigger. The fix is to make resize detection compare backbuffer-to-previous-backbuffer only, and use the client extent solely for viewport fallback — not for resize detection.

4. **Hysteresis threshold too low for marginal candidates**: The current `kPrimaryHysteresisFrames = 3` was chosen to prevent single-frame thrashing, but 3 frames is only ~50ms at 60fps. A UI burst that lasts 100ms (6 frames) can still cause a steal. Raising the threshold to 5–8 frames and adding an "incumbency advantage" (the current primary gets a bonus in the comparison) would make the election more stable.

5. **Stale target dimensions are inherent to the frame pipeline**: `ChangeProperties` updates `m_desc` and recreates the backbuffer, but the current frame's `injectRTX` may have already captured the old `targetImage`. This is not a bug in the election logic but in the timing of `ChangeProperties` relative to `PresentImage`. The fix is to ensure `onFrameBegin` always reads the *current* `targetImage->info().extent` (which it already does), and that `validateRaytracingOutput` handles the mismatch gracefully by calling `resetScreenResolution` exactly once — which the current code already does. The real issue is that the client-rect-based resize detection *also* fires, causing a double transition. Fixing bug 4 (client rect confusion) resolves this.

## Correctness Properties

Property 1: Bug Condition - Election Prefers Foreground Window With Most Draws

_For any_ set of swapchains where the main game swapchain is on the foreground window, has draws, and is visible, the primary election SHALL select the main game swapchain as primary regardless of whether its backbuffer dimensions match its window's client rect, provided no other swapchain has both a larger backbuffer area AND significantly more draw calls.

**Validates: Requirements 2.1, 2.2, 2.6**

Property 2: Preservation - Single-Swapchain and Existing Election Behavior

_For any_ game with a single swapchain, or where the primary is stable and no challenger is clearly better, the election logic SHALL produce the same result as the original code — single-swapchain games elect immediately, Alt+X overrides work, destroyed primaries are replaced immediately, and the 3-frame hysteresis baseline is preserved.

**Validates: Requirements 3.1, 3.2, 3.3, 3.4, 3.5, 3.6, 3.7, 3.8**

Property 3: Bug Condition - Resize Detection Uses Backbuffer Extent Only

_For any_ game where the backbuffer extent is stable across frames (no actual resize), the resize transition detection SHALL NOT fire, regardless of the difference between the backbuffer extent and the window client rect extent.

**Validates: Requirements 2.4**

Property 4: Preservation - Resize Detection Fires on Actual Backbuffer Change

_For any_ game where the backbuffer extent actually changes between frames (resize, fullscreen toggle), the resize transition detection SHALL fire exactly once, setting the camera carry-over grace period, and `resetScreenResolution` SHALL be called with the new extent.

**Validates: Requirements 2.3**

Property 5: Bug Condition - Hysteresis Prevents Marginal Candidate Steals

_For any_ challenger swapchain that is only marginally better than the current primary (e.g., slightly more draws during a UI burst), the hysteresis mechanism SHALL prevent the challenger from stealing primary unless the advantage persists for a sustained period (more than the baseline 3 frames) AND the current primary has degraded (stopped presenting or lost visibility).

**Validates: Requirements 2.5**

Property 6: Bug Condition - Non-Native Resolution Not Penalized

_For any_ game rendering at an internal resolution different from its window's client rect, the primary election SHALL NOT penalize the swapchain for the resolution mismatch — backbuffer area and draw count SHALL be the dominant election signals.

**Validates: Requirements 2.2**

## Fix Implementation

### Changes Required

Assuming our root cause analysis is correct:

**File**: `src/d3d11/d3d11_swapchain.cpp`

**Struct**: `PrimaryCandidateInfo`

**Specific Changes**:
1. **Add foreground window field**: Add `bool isForeground = false;` to `PrimaryCandidateInfo`. In `describeCandidate`, set it to `true` when `sc->m_window == GetForegroundWindow()`. This is the strongest signal for the "main game window" in multi-window games.

2. **Remove exactClientMatch and nearClientMatch from election priority**: Remove the `exactClientMatch` and `nearClientMatch` fields from `PrimaryCandidateInfo` (or keep them for logging but remove them from `isClearlyBetterCandidate`). These checks penalize games that render at non-native resolution. The backbuffer area and draw count are sufficient to identify the main swapchain.

3. **Restructure `isClearlyBetterCandidate` priority order**: Change the comparison order to:
   - `visible` (unchanged — invisible windows never win)
   - `isForeground` (new — foreground window gets strong preference)
   - `hasDraws` (promoted — a swapchain with draws is more likely the game)
   - `area` with incumbency bonus (the current primary needs to be beaten by a significant margin, not just 12.5%)
   - `draws` with higher threshold (require more than 32 extra draws to differentiate)

4. **Add incumbency advantage to area comparison**: Instead of `candidate.area > current.area + current.area / 8` (12.5% margin), require `candidate.area > current.area * 2` (100% margin) when the current primary is still presenting. This prevents marginal candidates from stealing primary.

5. **Raise hysteresis threshold**: Increase `kPrimaryHysteresisFrames` from 3 to 5. Additionally, require the current primary to have stopped presenting (0 draws for N frames) OR the challenger to be dramatically better (2x area AND 2x draws) before allowing a steal within the hysteresis window.

**File**: `src/d3d11/d3d11_rtx.cpp`

**Function**: `D3D11Rtx::EndFrame`

**Specific Changes**:
6. **Remove client-rect resize detection**: Remove the `noteResizeTransition(clientExtent, m_lastClientExtent)` call from `EndFrame`. The client extent should still be stored in `m_lastClientExtent` for viewport fallback purposes, but it should NOT trigger resize transitions. Only the backbuffer extent change should trigger `m_resizeTransitionFramesRemaining`.

**Function**: `D3D11Rtx::OnPresent`

**Specific Changes**:
7. **Remove client-rect resize detection from OnPresent**: Same change as in `EndFrame` — store the client extent but do not call `noteResizeTransition` on it. Only the swapchain image extent should trigger resize detection.

**File**: `src/d3d11/d3d11_rtx.h`

**Specific Changes**:
8. **Document the separation of concerns**: Add a comment to `m_lastClientExtent` clarifying that it is used only for viewport fallback, not for resize detection. Add a comment to `m_lastOutputExtent` clarifying that it is the sole source of truth for resize transition detection.

## Testing Strategy

### Validation Approach

The testing strategy follows a two-phase approach: first, surface counterexamples that demonstrate the bugs on unfixed code, then verify the fixes work correctly and preserve existing behavior.

### Exploratory Bug Condition Checking

**Goal**: Surface counterexamples that demonstrate the bugs BEFORE implementing the fix. Confirm or refute the root cause analysis. If we refute, we will need to re-hypothesize.

**Test Plan**: Write tests that construct `PrimaryCandidateInfo` structs simulating the bug conditions and verify that `isClearlyBetterCandidate` produces incorrect results. Also test `noteResizeTransition` with stable backbuffer but differing client rect. Run these tests on the UNFIXED code to observe failures and understand the root cause.

**Test Cases**:
1. **Non-native resolution penalty test**: Construct a main swapchain candidate with 1280×720 backbuffer in a 1920×1080 window (exactClientMatch=false, nearClientMatch=false) and a secondary with 800×600 backbuffer in an 800×600 window (exactClientMatch=true). Verify the secondary wins on unfixed code despite being smaller (will fail on unfixed code — secondary incorrectly wins).
2. **Foreground window ignored test**: Construct two equal-area, equal-draw candidates where one is on the foreground window. Verify the foreground one does NOT win on unfixed code because there is no foreground check (will fail on unfixed code — no preference for foreground).
3. **Marginal candidate steal test**: Simulate a challenger that is only 10% larger in area than the current primary. Verify it steals primary within 3 frames on unfixed code (will fail on unfixed code — steal happens too easily).
4. **Client rect resize false positive test**: Call `noteResizeTransition` with a stable backbuffer extent but a client rect that differs from the backbuffer. Verify resize transition fires on unfixed code (will fail on unfixed code — false resize detected).

**Expected Counterexamples**:
- `isClearlyBetterCandidate` returns true for a smaller swapchain that has `exactClientMatch=true` over a larger one that doesn't
- Two equal candidates with no foreground distinction — election is arbitrary (first to present wins)
- A 10% area advantage steals primary in 3 frames despite the current primary still presenting
- `m_resizeTransitionFramesRemaining` is set to 16 on a frame where only the client rect differs from the backbuffer

### Fix Checking

**Goal**: Verify that for all inputs where the bug condition holds, the fixed function produces the expected behavior.

**Pseudocode:**
```
FOR ALL input WHERE isBugCondition(input) DO
  result := isClearlyBetterCandidate_fixed(candidate, current)
  ASSERT expectedElectionOutcome(result, input)
END FOR
```

Specifically:
- For Bug 1/6: The foreground window's swapchain wins election when candidates are otherwise similar
- For Bug 2: A swapchain with a larger area and more draws wins regardless of client-rect match
- For Bug 4: `noteResizeTransition` does not fire when only the client rect differs from the backbuffer
- For Bug 5: A marginal challenger (10% better) does not steal primary within 5 frames

### Preservation Checking

**Goal**: Verify that for all inputs where the bug condition does NOT hold, the fixed function produces the same result as the original function.

**Pseudocode:**
```
FOR ALL input WHERE NOT isBugCondition(input) DO
  ASSERT isClearlyBetterCandidate_original(input) = isClearlyBetterCandidate_fixed(input)
END FOR
```

**Testing Approach**: Property-based testing is recommended for preservation checking because:
- It generates many test cases automatically across the input domain
- It catches edge cases that manual unit tests might miss
- It provides strong guarantees that behavior is unchanged for all non-buggy inputs

**Test Plan**: Observe behavior on UNFIXED code first for single-swapchain games, stable primaries, and non-resize scenarios, then write property-based tests capturing that behavior.

**Test Cases**:
1. **Single-swapchain election preservation**: Observe that a single swapchain is elected primary immediately on unfixed code, then write test to verify this continues after fix
2. **Alt+X override preservation**: Observe that Alt+X forces primary on unfixed code, then write test to verify this continues after fix
3. **Destroyed primary replacement preservation**: Observe that destroying the primary causes the next presenter to claim primary immediately on unfixed code, then write test to verify this continues after fix
4. **Actual resize detection preservation**: Observe that a real backbuffer resize triggers `m_resizeTransitionFramesRemaining` on unfixed code, then write test to verify this continues after fix
5. **DLSS resolution derivation preservation**: Observe that `onFrameBegin` receives `targetImage->info().extent` on unfixed code, then write test to verify this continues after fix

### Unit Tests

- Test `isClearlyBetterCandidate` with foreground vs non-foreground candidates of equal size
- Test `isClearlyBetterCandidate` with non-native resolution main swapchain vs native-resolution secondary
- Test `isClearlyBetterCandidate` with marginal area differences (10%, 50%, 100%, 200%)
- Test `noteResizeTransition` with stable backbuffer and differing client rect
- Test `noteResizeTransition` with actual backbuffer resize
- Test single-swapchain immediate election (no hysteresis)
- Test Alt+X forced primary override

### Property-Based Tests

- Generate random `PrimaryCandidateInfo` pairs with varying visibility, foreground status, area, and draw counts — verify the foreground visible swapchain with the most draws wins election
- Generate random backbuffer/client-rect size combinations — verify resize detection only fires when the backbuffer extent actually changes between frames
- Generate random sequences of challenger frames — verify hysteresis prevents steals for marginal advantages but allows steals for dramatic advantages

### Integration Tests

- Test full multi-swapchain game scenario: create two swapchains, verify the foreground one with more draws is elected primary
- Test resolution change scenario: resize the backbuffer, verify `resetScreenResolution` is called exactly once and camera carry-over works
- Test stable low-resolution game: 640×480 backbuffer in 1920×1080 window, verify no resize transitions fire over 100 frames
- Test hysteresis with UI burst: secondary swapchain gets a burst of draws for 4 frames, verify primary does not switch
