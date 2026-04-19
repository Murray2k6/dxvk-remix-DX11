# RTX Remix UI and Viewport Fixes — Bugfix Design

## Overview

This design addresses nine bugs in the DXVK Remix (RTX Remix DX11) project spanning viewport crashes, UI persistence, game focus/minimize issues, engine compatibility hangs, performance/memory leaks, non-invertible matrix errors, raytrace mode auto-resolution failures, and object picking ambiguity. The fix strategy is minimal and targeted: each bug is addressed at its root cause with the smallest change that restores correct behavior, and preservation requirements ensure no regressions in the surrounding systems.

## Glossary

- **Bug_Condition (C)**: The set of conditions that trigger one of the nine bugs — e.g., calling `getCurrentViewportCount()` returns 16 instead of the active count, or a UI checkbox toggle is overridden by a stronger RtxOption layer on the next frame.
- **Property (P)**: The desired correct behavior for each bug condition — e.g., `getCurrentViewportCount()` returns the rasterizer state's active viewport count, or a UI change persists across frames.
- **Preservation**: Existing behaviors that must remain unchanged by the fixes — e.g., terrain baker viewport save/restore, layer priority for programmatic option writes, invertible matrix inversion correctness.
- **`getCurrentViewportCount()`**: Method on `RtxContext` in `rtx_context.h` that returns the number of active viewports. Currently returns `m_state.vp.viewports.size()` (always 16) instead of `m_state.gp.state.rs.viewportCount()`.
- **`RtxOptionLayer`**: Priority-based layer system in `rtx_option.h` / `rtx_option.cpp` that resolves option values by blending/overriding across layers (Default < DXVK Config < Remix Config < ... < User < Quality).
- **`resolveValue()`**: Method in `rtx_option.cpp` that iterates layers from strongest to weakest to compute the final resolved value for an RtxOption.
- **`FocusSteal`**: A background thread system in `dxvk_imgui.cpp` that creates a hidden helper window to steal foreground focus from the game. Permanently disabled but still started.
- **`g_sharedDeviceRefCount`**: Atomic counter in `d3d11_device.cpp` tracking how many D3D11 devices share the single `DxvkDevice`.
- **`inverse()`**: Template function in `util_matrix.h` that computes the 4×4 matrix inverse. Divides by zero when the determinant is zero.
- **`applyAutoQualityRaytraceMode`**: Lambda in `rtx_options.cpp` that writes the auto-detected raytrace mode to the Quality layer.
- **`objectPickingValue`**: Per-instance identifier used by the Remix UI for object selection and highlighting. Stored in `RtSurface` and indexed in `m_drawCallMeta.infos`.

## Bug Details

### Bug Condition

The nine bugs manifest under distinct conditions that can be grouped into four categories:

1. **Viewport/Rasterizer corruption** (Bugs 1): `getCurrentViewportCount()` returns the fixed array size (16) instead of the active viewport count, causing save/restore to corrupt rasterizer state.
2. **UI persistence failure** (Bugs 2, 3, 8): User changes via the settings UI or auto-quality writes are overridden by stronger RtxOption layers that are re-populated each frame.
3. **Stability/hang issues** (Bugs 4, 5): The FocusSteal thread interferes with fullscreen games; shared device ref count leaks prevent cleanup; swapchain destruction blocks indefinitely; primary election thrashes; shutdown loops infinitely.
4. **Data integrity issues** (Bugs 6, 7, 9): Texture cache never shrinks; asset map grows unbounded; sampler feedback overflows; matrix inversion produces NaN/Inf; object picking values collide silently.

**Formal Specification:**
```
FUNCTION isBugCondition(input)
  INPUT: input of type {bugId: int, context: BugContext}
  OUTPUT: boolean

  SWITCH input.bugId:
    CASE 1:  // Viewport crash
      RETURN input.context.calledGetCurrentViewportCount
             AND input.context.activeViewportCount != m_state.vp.viewports.size()
    CASE 2,3:  // UI persistence
      RETURN input.context.userChangedOptionViaUI
             AND input.context.strongerLayerExistsForOption
             AND input.context.strongerLayerRePopulatedThisFrame
    CASE 4:  // Game minimize
      RETURN input.context.focusStealThreadRunning
             AND input.context.gameIsFullscreenExclusive
    CASE 5:  // Unity hangs
      RETURN input.context.createDeviceFailed AND input.context.refCountIncrementedBeforeCheck
             OR input.context.swapchainDestructorWaitsIndefinitely
             OR input.context.primaryElectionThrashing
             OR input.context.shutdownReleaseLoopInfinite
    CASE 6:  // Memory leaks
      RETURN input.context.textureCacheEvicted AND NOT input.context.containerShrunk
             OR input.context.assetMapEntryCount > maxAllowed
             OR input.context.samplerFeedbackCount > SAMPLER_FEEDBACK_MAX
    CASE 7:  // Non-invertible matrix
      RETURN input.context.matrixDeterminant == 0.0
    CASE 8:  // Raytrace mode blocked
      RETURN input.context.qualityLayerWriteValue != input.context.resolvedValue
             AND input.context.noStrongerLayerExists
    CASE 9:  // Duplicate picking value
      RETURN input.context.multipleDrawCallsSharePickingValue
  END SWITCH
END FUNCTION
```

### Examples

- **Bug 1**: UE4 game sets 1 active viewport. `getCurrentViewportCount()` returns 16. Dust particles save/restore passes 16 viewports (15 zero-initialized) to `setViewports`, corrupting rasterizer state → crash.
- **Bug 2/3**: User toggles "Enable Dust Particles" checkbox. Quality layer holds `false`. `clearFromStrongerLayers` removes Quality entry this frame. Next frame, Quality layer is re-populated from preset config → checkbox reverts to `false`.
- **Bug 4**: Skyrim SE running fullscreen-exclusive. FocusSteal thread's hidden `WS_POPUP` window exists → Windows minimizes the FSE swapchain.
- **Bug 5**: Unity creates 3 D3D11 devices during init probing. First two fail. `g_sharedDeviceRefCount` is 3 but only 1 device exists → `ReleaseSharedDevice` never reaches 0 → hang.
- **Bug 7**: Game provides a zero-scale matrix during scene transition. `inverse()` computes `dot1 == 0.0`, divides by zero → NaN propagates through camera matrices → black screen.
- **Bug 8**: NVIDIA GPU, Auto preset. `setImmediately(TraceRay, QualityLayer)` writes to Quality (priority `0xFFFFFFFF`). But `resolveValue` iterates from strongest to weakest — Quality IS the strongest, so the write should work. The issue is likely that the Quality layer's `blendThreshold` or `blendStrength` prevents the value from being applied during resolution.
- **Bug 9**: Remix API sets `objectPickingValue=42` on 3 instances. All 3 merge into one `DrawCallMetaInfo` entry. User clicks instance B but `findLegacyTextureHashByObjectPickingValue(42)` returns instance A's texture hash.

## Expected Behavior

### Preservation Requirements

**Unchanged Behaviors:**
- Terrain baker viewport save/restore via `dxvkCtxState.gp.state.rs.viewportCount()` must continue to work correctly
- `setViewports` zero-width/height fallback to 1×1 dummy viewports must remain
- Render target save/restore after dust particle rendering must remain functional
- RtxOption layer priority system must continue to resolve values correctly for programmatic (non-UI) writes
- Config file, preset, and game-target layer values must continue to be respected when no UI override exists
- `RtxOptionUxWrapper` reset button must continue to reset options to defaults
- Full-row click-through on checkboxes must continue to work
- `ComboWithKey::getKey` logging must remain unchanged
- WndProc filtering and IAT hooks for input isolation must continue to function
- DI8 force-unacquire mechanism must continue to work for DirectInput games
- Shared `DxvkDevice` lifecycle (reuse across D3D11 devices, keep alive for emulators) must remain
- Single-swapchain games must elect primary without overhead
- `remixapi_Shutdown` must release cleanly when no external references exist
- Texture cache budget-based demotion/promotion must continue to work
- `preloadTextureAsset` deduplication by hash must remain
- Alt+X hotkey for forcing primary swapchain must continue to work
- `inverse()` for invertible matrices must return the correct mathematical inverse (no identity fallback)
- `mathValidationAssert` error logging for non-invertible matrices must remain
- Non-Auto raytrace mode presets must skip auto-detection
- AMD/Intel vendor-specific raytrace mode defaults must remain unchanged
- Successful Quality layer writes must not trigger the fallback
- Unique `objectPickingValue` per draw call must continue to map 1:1 without merging
- `g_allowMappingLegacyHashToObjectPickingValue` gating must remain
- `gatherObjectPickingValuesByTextureHash` reverse lookup must remain functional
- `mergeFrom` deduplication via `addUniqueHash` must remain

**Scope:**
All inputs that do NOT match the bug conditions above should be completely unaffected by these fixes. The fixes are surgical: each targets a specific code path and does not alter the general architecture of the viewport system, option layer system, swapchain management, texture cache, matrix math, or object picking.

## Hypothesized Root Cause

### Bug 1: Viewport Crash
`getCurrentViewportCount()` in `rtx_context.h` returns `m_state.vp.viewports.size()` which is `std::array<VkViewport, 16>::size()` — always 16. The correct source of truth is `m_state.gp.state.rs.viewportCount()`, which tracks the actual count set by `setViewports()`. The terrain baker already uses the correct pattern. Additionally, `setupRasterizerState` in `rtx_dust_particles.cpp` saves viewport state to local variables that are never used (the comment says the caller restores via `dxvkCtxState = stateCopy`), creating confusion.

### Bugs 2 & 3: UI Settings Not Persisting
The `IMGUI_RTXOPTION_WIDGET` macro and `Checkbox(label, RtxOption<bool>*)` correctly call `clearFromStrongerLayers` and `setImmediately` to the User layer. However, stronger layers (preset, quality, config-file, game-target) are re-populated each frame from their source configs. When the layer is re-populated, it re-inserts its value into the option's layer queue, overriding the User layer on the next `resolveValue()` call. The fix must mark options as "user-overridden" so that layer re-population skips options the user has explicitly changed via the UI.

### Bug 4: Games Minimize When UI Opens
The `startFocusStealThread()` call at line 1274 of `dxvk_imgui.cpp` starts a background thread that creates a hidden `WS_POPUP` window and runs a message pump. The code comments (lines 1981–1986) explicitly document that this feature is "PERMANENTLY DISABLED" and causes two failure modes: FSE game minimization and borderless-windowed crashes. The thread should never be started.

### Bug 5: Unity Engine Hangs
Four sub-issues: (a) `g_sharedDeviceRefCount++` at line 3465 of `d3d11_device.cpp` runs before the device creation check — if creation fails, the count leaks. (b) `D3D11SwapChain::~D3D11SwapChain` calls `waitForIdle()` without a timeout, blocking the message queue. (c) Primary swapchain election in `d3d11_swapchain.cpp` uses `isClearlyBetterCandidate` without hysteresis — similar swapchains can thrash. (d) The Remix API shutdown `Release()` loop has no iteration bound.

### Bug 6: Performance / Memory Leaks
The texture cache's underlying vector only grows via `push_back` and never calls `shrink_to_fit`. The `m_assetHashToTextures` map only erases entries when a new asset with the same hash is loaded — no time-based or pressure-based eviction. Sampler feedback overflow hits `assert(0)` but continues with an invalid sentinel.

### Bug 7: Non-Invertible Matrix
In `inverse()` in `util_matrix.h`, when `dot1 == 0.0`, `mathValidationAssert` logs the error but execution continues to the division loop `inverse[i/4][i%4] / dot1`, producing NaN/Inf. The fix is to return identity when `dot1 == 0.0`.

### Bug 8: Raytrace Mode Blocked by Layer
The Quality layer has priority `0xFFFFFFFF` (highest). `setImmediately` writes the value and calls `resolveValue`. But `resolveValue` iterates from strongest to weakest, and for non-float types, it checks `blendStrength < blendThreshold` — if the Quality layer entry has a blend strength below threshold (e.g., from initialization), the value is skipped. The fallback should write to the Derived layer or force the resolved value directly.

### Bug 9: Duplicate objectPickingValue
When the Remix API sets the same `objectPickingValue` on multiple instances, `m_drawCallMeta.infos[ticker][pickingValue]` merges all their metadata via `mergeFrom()`. The warning message doesn't include the picking value or collision count, making diagnosis difficult.

## Correctness Properties

Property 1: Bug Condition - Viewport Count Returns Active Count

_For any_ call to `getCurrentViewportCount()` after `setViewports(n, ...)` has been called with `n` active viewports, the method SHALL return `n` (matching `m_state.gp.state.rs.viewportCount()`), not the fixed array size 16.

**Validates: Requirements 2.1, 2.2**

Property 2: Preservation - Invertible Matrix Inversion Correctness

_For any_ invertible 4×4 matrix M (where `det(M) != 0.0`), `inverse(M)` SHALL return the mathematically correct inverse such that `M * inverse(M) ≈ I` within floating-point tolerance, with no identity fallback applied.

**Validates: Requirements 3.19**

Property 3: Bug Condition - Non-Invertible Matrix Returns Identity

_For any_ non-invertible 4×4 matrix M (where `det(M) == 0.0`), `inverse(M)` SHALL return the identity matrix and log the error, producing only finite values (no NaN/Inf).

**Validates: Requirements 2.19, 2.20, 2.21**

Property 4: Bug Condition - UI Option Changes Persist Across Frames

_For any_ RtxOption modified via the UI (checkbox, combo, drag, slider) where a stronger layer holds an opinion, the user's chosen value SHALL be the resolved value on subsequent frames — the stronger layer re-population SHALL NOT override the user's explicit UI change.

**Validates: Requirements 2.5, 2.6, 2.7**

Property 5: Preservation - Programmatic Layer Writes Respect Priority

_For any_ RtxOption set programmatically (not through the UI) via config file, preset, or game-target layer, the layer priority system SHALL continue to resolve values according to layer strength ordering, with no interference from the user-override tracking.

**Validates: Requirements 3.4, 3.5**

Property 6: Bug Condition - Shared Device Ref Count Tracks Successful Creations

_For any_ sequence of `CreateDevice()` calls where some succeed and some fail, `g_sharedDeviceRefCount` SHALL equal the number of successful device creations/reuses, not the total number of calls.

**Validates: Requirements 2.11, 2.12**

Property 7: Bug Condition - Raytrace Mode Auto-Resolution Succeeds

_For any_ NVIDIA GPU with `RaytraceModePreset::Auto`, after `updateRaytraceModePresets()` completes, `renderPassIntegrateIndirectRaytraceMode()` SHALL resolve to `TraceRay`, either via the Quality layer write or the fallback mechanism.

**Validates: Requirements 2.22, 2.23**

Property 8: Preservation - Non-Auto Raytrace Presets Unchanged

_For any_ GPU where `RaytraceModePreset` is not `Auto`, `updateRaytraceModePresets()` SHALL skip auto-detection entirely, and for AMD/Intel GPUs with Auto, all modes SHALL resolve to `RayQuery`.

**Validates: Requirements 3.21, 3.22, 3.23**

Property 9: Bug Condition - Object Picking Warning Includes Details

_For any_ set of draw calls sharing the same `objectPickingValue`, the warning message SHALL include the actual picking value and the number of colliding draw calls.

**Validates: Requirements 2.27**

## Fix Implementation

### Changes Required

#### Bug 1: Viewport Crash

**File**: `src/dxvk/rtx_render/rtx_context.h`
**Function**: `getCurrentViewportCount()`

**Specific Changes**:
1. **Return active viewport count**: Change `return static_cast<uint32_t>(m_state.vp.viewports.size());` to `return m_state.gp.state.rs.viewportCount();` — this matches the pattern used by the terrain baker in `rtx_terrain_baker.cpp`.

**File**: `src/dxvk/rtx_render/rtx_dust_particles.cpp`
**Function**: `setupRasterizerState()`

**Specific Changes**:
2. **Remove dead viewport save code**: Remove the three local variable declarations (`prevViewportCount`, `prevViewportState`, `prevRenderTargets`) at the top of `setupRasterizerState` that save state to unused locals. The comment already documents that the caller restores state.

**File**: `build.log`

**Specific Changes**:
3. **Regenerate build log**: Delete or regenerate `build.log` so it reflects the current build state without stale errors referencing renamed methods.

#### Bugs 2 & 3: UI Settings Not Persisting

**File**: `src/dxvk/rtx_render/rtx_option.h`

**Specific Changes**:
4. **Add user-override tracking**: Add a `bool m_userOverridden = false;` flag to `RtxOptionImpl` (or the per-option state). When `clearFromStrongerLayers` is called from a UI context (User layer), set this flag to `true`.
5. **Guard layer re-population**: In the code path that re-populates layers from config files each frame, check `m_userOverridden` — if true, skip re-inserting the stronger layer's value for that option. This ensures the user's UI change persists.

**File**: `src/dxvk/rtx_render/rtx_option.cpp`

**Specific Changes**:
6. **Implement override guard in layer population**: When a layer value is being set during config re-read (not via `setImmediately` from UI), check the user-override flag and skip if set.
7. **Reset override on explicit reset**: When the `RtxOptionUxWrapper` reset button is clicked, clear the `m_userOverridden` flag so the option returns to normal layer resolution.

**File**: `src/dxvk/rtx_render/rtx_imgui.h`

**Specific Changes**:
8. **Mark user override in macro**: In the `IMGUI_RTXOPTION_WIDGET` macro, after `clearFromStrongerLayers` and `setImmediately`, call a method to mark the option as user-overridden.

**File**: `src/dxvk/rtx_render/rtx_imgui.cpp`

**Specific Changes**:
9. **Mark user override in Checkbox**: In `Checkbox(const char*, RtxOption<bool>*)`, after the `clearFromStrongerLayers` / `setImmediately` sequence, mark the option as user-overridden.

#### Bug 4: Games Minimize When UI Opens

**File**: `src/dxvk/imgui/dxvk_imgui.cpp`

**Specific Changes**:
10. **Remove FocusSteal thread startup**: Remove or comment out the `startFocusStealThread()` call at line 1274. The code comments already document this feature as permanently disabled.
11. **Remove FocusSteal thread implementation**: Remove the entire FocusSteal namespace (thread, helper window, message pump) or guard it behind a compile-time `#if 0` to prevent accidental re-enablement.

#### Bug 5: Unity Engine Hangs

**File**: `src/d3d11/d3d11_device.cpp`
**Function**: `D3D11DXGIDevice::CreateDevice()`

**Specific Changes**:
12. **Move ref count increment after success**: Move `g_sharedDeviceRefCount++` to after the `if (g_sharedDevice != nullptr)` check (for reuse) and after `m_dxvkAdapter->createDevice()` succeeds (for new creation). If `createDevice` throws, the ref count is not incremented.

**File**: `src/d3d11/d3d11_swapchain.cpp`
**Function**: `D3D11SwapChain::~D3D11SwapChain()`

**Specific Changes**:
13. **Add timeout to destructor waits**: Replace the unconditional `waitForIdle()` with a timed wait (e.g., 5 seconds). If the timeout expires, log a warning and proceed with destruction rather than blocking indefinitely.

**Function**: Primary swapchain election logic

**Specific Changes**:
14. **Add hysteresis to primary election**: Add a frame counter or timestamp to the current primary. Only allow a new candidate to steal primary if it has been "clearly better" for N consecutive frames (e.g., 3–5), preventing single-frame thrashing.

**File**: `src/dxvk/rtx_render/rtx_remix_api.cpp`
**Function**: Shutdown `Release()` loop

**Specific Changes**:
15. **Bound the Release loop**: Add a maximum iteration count (e.g., 1000) to the `while(true) { Release(); }` loop. If the limit is reached, log an error and break rather than spinning forever.

#### Bug 6: Performance / Memory Leaks

**File**: `src/dxvk/rtx_render/rtx_texture_manager.cpp`

**Specific Changes**:
16. **Add periodic shrink-to-fit**: After the texture cache's `clear()` / demotion pass, periodically call `shrink_to_fit()` on the underlying vector (e.g., every 60 seconds or when the eviction count exceeds a threshold).
17. **Add time-based eviction for asset hash map**: Track last-access time for `m_assetHashToTextures` entries. Periodically evict entries that haven't been accessed within a configurable TTL (e.g., 5 minutes).
18. **Handle sampler feedback overflow gracefully**: When the sampler feedback count exceeds `SAMPLER_FEEDBACK_MAX_TEXTURE_COUNT`, evict the oldest feedback entry or cleanly reject the new entry instead of hitting `assert(0)` and continuing with an invalid sentinel.

#### Bug 7: Non-Invertible Matrix

**File**: `src/util/util_matrix.h`
**Function**: `inverse()`

**Specific Changes**:
19. **Return identity on zero determinant**: After the `mathValidationAssert(dot1 != 0.0, ...)` check, add an early return of the identity matrix when `dot1 == 0.0`. The error log remains via `mathValidationAssert`. The division loop is skipped entirely.

#### Bug 8: Raytrace Mode Blocked by Layer

**File**: `src/dxvk/rtx_render/rtx_options.cpp`
**Function**: `applyAutoQualityRaytraceMode` lambda

**Specific Changes**:
20. **Add fallback for blocked Quality layer write**: After the existing retry fails, add a fallback that writes to the Derived layer (`RtxOptionLayer::getDerivedLayer()`) or directly sets the resolved value via a force mechanism (e.g., `setDeferred` with a force flag that bypasses blend threshold checks). Log the fallback path.

#### Bug 9: Duplicate objectPickingValue

**File**: `src/dxvk/rtx_render/rtx_scene_manager.cpp`

**Specific Changes**:
21. **Improve warning message**: When multiple draw calls share the same `objectPickingValue`, update the warning to include the actual picking value and collision count: `"Found N draw calls with objectPickingValue=V. Using merged MetaInfo for object picking. Consider assigning unique picking values via remixapi_InstanceInfoObjectPickingEXT."`.
22. **Consider unique sub-IDs**: When a collision is detected, assign unique sub-IDs (e.g., `pickingValue * 1000 + collisionIndex`) to disambiguate instances. This is a stretch goal — the improved warning is the minimum fix.

## Testing Strategy

### Validation Approach

The testing strategy follows a two-phase approach: first, surface counterexamples that demonstrate the bugs on unfixed code, then verify the fixes work correctly and preserve existing behavior.

### Exploratory Bug Condition Checking

**Goal**: Surface counterexamples that demonstrate the bugs BEFORE implementing the fixes. Confirm or refute the root cause analysis. If we refute, we will need to re-hypothesize.

**Test Plan**: Write tests that exercise each bug condition on the unfixed code to observe failures and confirm root causes.

**Test Cases**:
1. **Viewport Count Test**: Call `setViewports(1, ...)` then `getCurrentViewportCount()` — expect 1, get 16 (will fail on unfixed code)
2. **Dust Particle Save/Restore Test**: Run `simulateAndDraw` with 1 active viewport, check viewport count after restore — expect 1, get 16 (will fail on unfixed code)
3. **Checkbox Persistence Test**: Set a stronger layer value, toggle checkbox via UI, check resolved value next frame — expect user's value, get stronger layer's value (will fail on unfixed code)
4. **Matrix Inversion Zero Det Test**: Call `inverse()` on a zero-determinant matrix, check output for NaN — expect identity, get NaN (will fail on unfixed code)
5. **Raytrace Mode Auto Test**: Run `updateRaytraceModePresets()` on simulated NVIDIA GPU, check resolved value — expect TraceRay, get RayQuery (will fail on unfixed code)
6. **Ref Count Leak Test**: Call `CreateDevice()` with a failing adapter, check ref count — expect 0, get 1 (will fail on unfixed code)

**Expected Counterexamples**:
- `getCurrentViewportCount()` returns 16 regardless of active viewport count
- Checkbox resolved value reverts to stronger layer's value after one frame
- `inverse()` output contains NaN/Inf values for zero-determinant input
- `g_sharedDeviceRefCount` is 1 after a failed `CreateDevice()` call

### Fix Checking

**Goal**: Verify that for all inputs where the bug condition holds, the fixed functions produce the expected behavior.

**Pseudocode:**
```
FOR ALL input WHERE isBugCondition(input) DO
  result := fixedFunction(input)
  ASSERT expectedBehavior(result)
END FOR
```

Specifically:
- For Bug 1: `getCurrentViewportCount()` returns `m_state.gp.state.rs.viewportCount()` for any viewport count 1–16
- For Bugs 2/3: Resolved value matches user's UI choice after frame advance, for any option with any stronger layer configuration
- For Bug 7: `inverse(M)` returns identity for any matrix with `det(M) == 0.0`, with all output values finite
- For Bug 8: Raytrace mode resolves to the auto-detected value for any GPU vendor

### Preservation Checking

**Goal**: Verify that for all inputs where the bug condition does NOT hold, the fixed functions produce the same result as the original functions.

**Pseudocode:**
```
FOR ALL input WHERE NOT isBugCondition(input) DO
  ASSERT originalFunction(input) = fixedFunction(input)
END FOR
```

**Testing Approach**: Property-based testing is recommended for preservation checking because:
- It generates many test cases automatically across the input domain
- It catches edge cases that manual unit tests might miss
- It provides strong guarantees that behavior is unchanged for all non-buggy inputs

**Test Plan**: Observe behavior on UNFIXED code first for non-bug inputs, then write property-based tests capturing that behavior.

**Test Cases**:
1. **Invertible Matrix Preservation**: For any invertible matrix M, verify `inverse_fixed(M) == inverse_original(M)` — the fix only affects the `dot1 == 0.0` path
2. **Programmatic Option Write Preservation**: For any RtxOption set via config/preset (not UI), verify layer resolution produces the same value before and after the user-override tracking change
3. **Single-Swapchain Election Preservation**: For a single swapchain, verify it is elected primary immediately without hysteresis delay
4. **Unique Picking Value Preservation**: For draw calls with unique `objectPickingValue`, verify 1:1 mapping to `DrawCallMetaInfo` with no merging or sub-ID assignment

### Unit Tests

- Test `getCurrentViewportCount()` returns correct count for viewport counts 1, 2, 4, 16
- Test `inverse()` returns identity for zero matrix, zero-row matrix, zero-determinant matrix
- Test `inverse()` returns correct inverse for identity, rotation, translation, scale matrices
- Test `Checkbox(label, RtxOption<bool>*)` persists value when stronger layer exists
- Test `CreateDevice()` ref count is 0 after failed creation, 1 after successful creation
- Test `applyAutoQualityRaytraceMode` fallback activates when Quality layer write fails
- Test object picking warning message format includes picking value and collision count
- Test FocusSteal thread is not started after initialization

### Property-Based Tests

- Generate random viewport counts (1–16), verify `getCurrentViewportCount()` matches after `setViewports`
- Generate random 4×4 matrices, verify `inverse()` returns identity for singular matrices and correct inverse for non-singular matrices, with no NaN/Inf in any output
- Generate random RtxOption configurations with varying layer priorities, verify UI changes persist and programmatic changes respect priority
- Generate random sequences of `CreateDevice()` success/failure, verify ref count equals successful count
- Generate random swapchain configurations, verify primary election stabilizes within N frames

### Integration Tests

- Test full dust particle render pass with 1 active viewport: verify no crash and correct viewport restoration
- Test UI checkbox toggle with Quality preset active: verify checkbox state persists across 10 frames
- Test Unity-style multi-device initialization: verify no hang and correct cleanup
- Test extended play session texture loading: verify memory usage stabilizes after cache eviction
- Test NVIDIA auto raytrace mode: verify TraceRay is active after initialization
- Test object picking with duplicate picking values: verify warning message is actionable
