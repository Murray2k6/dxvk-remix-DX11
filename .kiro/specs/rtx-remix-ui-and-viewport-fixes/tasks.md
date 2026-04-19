# Implementation Plan

- [x] 1. Write bug condition exploration test
  - **Property 1: Bug Condition** - Viewport Count, Matrix Inversion, UI Persistence, and Ref Count Bugs
  - **CRITICAL**: This test MUST FAIL on unfixed code — failure confirms the bugs exist
  - **DO NOT attempt to fix the tests or the code when they fail**
  - **NOTE**: These tests encode the expected behavior — they will validate the fixes when they pass after implementation
  - **GOAL**: Surface counterexamples that demonstrate the bugs exist
  - **Scoped PBT Approach**: Scope properties to concrete failing cases for reproducibility
  - Test 1a: Call `setViewports(n, ...)` for n in 1..16, then call `getCurrentViewportCount()` — assert returns n (from Bug Condition case 1: `activeViewportCount != m_state.vp.viewports.size()`)
  - Test 1b: Call `inverse()` on a zero-determinant matrix — assert output is identity matrix with all finite values (from Bug Condition case 7: `matrixDeterminant == 0.0`)
  - Test 1c: Set a stronger layer value on an RtxOption, toggle via UI checkbox, check resolved value next frame — assert user's value persists (from Bug Condition cases 2/3: `userChangedOptionViaUI AND strongerLayerExistsForOption`)
  - Test 1d: Call `CreateDevice()` with a failing adapter — assert `g_sharedDeviceRefCount` is 0 after failure (from Bug Condition case 5: `createDeviceFailed AND refCountIncrementedBeforeCheck`)
  - Run tests on UNFIXED code
  - **EXPECTED OUTCOME**: Tests FAIL (this is correct — it proves the bugs exist)
  - Document counterexamples found:
    - `getCurrentViewportCount()` returns 16 regardless of active viewport count
    - `inverse()` output contains NaN/Inf values for zero-determinant input
    - Checkbox resolved value reverts to stronger layer's value after one frame
    - `g_sharedDeviceRefCount` is 1 after a failed `CreateDevice()` call
  - Mark task complete when tests are written, run, and failures are documented
  - _Requirements: 1.1, 1.2, 1.19, 1.5, 1.11_

- [x] 2. Write preservation property tests (BEFORE implementing fix)
  - **Property 2: Preservation** - Invertible Matrix Inversion, Programmatic Layer Writes, Single-Swapchain Election, Unique Picking Values
  - **IMPORTANT**: Follow observation-first methodology
  - Observe: `inverse(rotationMatrix)` returns correct mathematical inverse on unfixed code
  - Observe: `inverse(translationMatrix)` returns correct mathematical inverse on unfixed code
  - Observe: `inverse(scaleMatrix)` returns correct mathematical inverse on unfixed code
  - Observe: RtxOption set via config file resolves correctly via layer priority on unfixed code
  - Observe: Single swapchain is elected primary immediately on unfixed code
  - Observe: Unique `objectPickingValue` maps 1:1 to `DrawCallMetaInfo` on unfixed code
  - Write property-based test: for all invertible 4×4 matrices M (det(M) != 0.0), `inverse(M)` returns the mathematically correct inverse such that `M * inverse(M) ≈ I` within floating-point tolerance (from Preservation Requirements 3.19)
  - Write property-based test: for all RtxOptions set programmatically (not via UI), layer priority resolution produces the same value before and after user-override tracking changes (from Preservation Requirements 3.4, 3.5)
  - Write property-based test: for a single swapchain, it is elected primary immediately without hysteresis delay (from Preservation Requirements 3.13)
  - Write property-based test: for draw calls with unique `objectPickingValue`, 1:1 mapping to `DrawCallMetaInfo` with no merging (from Preservation Requirements 3.24)
  - Verify all tests pass on UNFIXED code
  - **EXPECTED OUTCOME**: Tests PASS (this confirms baseline behavior to preserve)
  - Mark task complete when tests are written, run, and passing on unfixed code
  - _Requirements: 3.19, 3.4, 3.5, 3.13, 3.24_

- [x] 3. Fix Bug 1: Viewport crash — return active viewport count

  - [x] 3.1 Fix `getCurrentViewportCount()` to return active count
    - **File**: `src/dxvk/rtx_render/rtx_context.h`
    - Change `return static_cast<uint32_t>(m_state.vp.viewports.size());` to `return m_state.gp.state.rs.viewportCount();`
    - This matches the correct pattern already used by the terrain baker in `rtx_terrain_baker.cpp`
    - _Bug_Condition: isBugCondition(input) where calledGetCurrentViewportCount AND activeViewportCount != m_state.vp.viewports.size()_
    - _Expected_Behavior: getCurrentViewportCount() returns m_state.gp.state.rs.viewportCount()_
    - _Preservation: Terrain baker viewport save/restore continues to work; setViewports zero-width/height fallback remains_
    - _Requirements: 2.1, 2.2, 3.1, 3.2, 3.3_

  - [x] 3.2 Remove dead viewport save code in `setupRasterizerState`
    - **File**: `src/dxvk/rtx_render/rtx_dust_particles.cpp`
    - Remove the three unused local variable declarations (`prevViewportCount`, `prevViewportState`, `prevRenderTargets`) at the top of `setupRasterizerState`
    - The comment documents that the caller restores state via `dxvkCtxState = stateCopy` — these locals are never read
    - _Requirements: 2.3_

  - [x] 3.3 Clean up stale build log
    - **File**: `build.log`
    - Delete or regenerate `build.log` so it reflects the current build state without stale errors referencing renamed methods (`getSpecifiedViewportState`, `getSpecifiedRenderTargets`)
    - _Requirements: 2.4_

  - [x] 3.4 Verify bug condition exploration test now passes for viewport count
    - **Property 1: Expected Behavior** - Viewport Count Returns Active Count
    - **IMPORTANT**: Re-run the SAME viewport count test from task 1 — do NOT write a new test
    - The test from task 1 encodes the expected behavior: `getCurrentViewportCount()` returns n after `setViewports(n, ...)`
    - When this test passes, it confirms the expected behavior is satisfied
    - **EXPECTED OUTCOME**: Test PASSES (confirms bug is fixed)
    - _Requirements: 2.1, 2.2_

  - [x] 3.5 Verify preservation tests still pass
    - **Property 2: Preservation** - Terrain Baker and Viewport Fallback
    - **IMPORTANT**: Re-run the SAME tests from task 2 — do NOT write new tests
    - **EXPECTED OUTCOME**: Tests PASS (confirms no regressions)
    - Confirm all preservation tests still pass after fix

- [x] 4. Fix Bug 7: Non-invertible matrix — return identity on zero determinant

  - [x] 4.1 Add identity return on zero determinant in `inverse()`
    - **File**: `src/util/util_matrix.h`
    - **Function**: `inverse()`
    - After the `mathValidationAssert(dot1 != 0.0, ...)` check, add an early return of the identity matrix when `dot1 == 0.0`
    - The error log remains via `mathValidationAssert` — the division loop is skipped entirely
    - This prevents NaN/Inf propagation through camera matrices, normal matrices, portal transforms, and lighting calculations
    - _Bug_Condition: isBugCondition(input) where matrixDeterminant == 0.0_
    - _Expected_Behavior: inverse(M) returns identity matrix with all finite values when det(M) == 0.0_
    - _Preservation: inverse() for invertible matrices returns correct mathematical inverse (no identity fallback); mathValidationAssert error logging remains_
    - _Requirements: 2.19, 2.20, 2.21, 3.19, 3.20_

  - [x] 4.2 Verify bug condition exploration test now passes for matrix inversion
    - **Property 1: Expected Behavior** - Non-Invertible Matrix Returns Identity
    - **IMPORTANT**: Re-run the SAME matrix inversion test from task 1 — do NOT write a new test
    - When this test passes, it confirms `inverse()` returns identity for zero-determinant matrices
    - **EXPECTED OUTCOME**: Test PASSES (confirms bug is fixed)
    - _Requirements: 2.19, 2.20, 2.21_

  - [x] 4.3 Verify preservation tests still pass for invertible matrices
    - **Property 2: Preservation** - Invertible Matrix Inversion Correctness
    - **IMPORTANT**: Re-run the SAME invertible matrix tests from task 2 — do NOT write new tests
    - **EXPECTED OUTCOME**: Tests PASS (confirms no regressions for invertible matrices)
    - Confirm `inverse(M)` still returns correct inverse for all invertible matrices

- [x] 5. Fix Bug 4: Remove FocusSteal thread startup

  - [x] 5.1 Remove FocusSteal thread startup and implementation
    - **File**: `src/dxvk/imgui/dxvk_imgui.cpp`
    - Remove or comment out the `startFocusStealThread()` call at line 1274
    - Remove the entire FocusSteal namespace (thread function, helper window creation, message pump) or guard it behind `#if 0` to prevent accidental re-enablement
    - The code comments (lines 1981–1986) already document this feature as "PERMANENTLY DISABLED"
    - DI8 force-unacquire and IAT hooks already handle input isolation
    - _Bug_Condition: isBugCondition(input) where focusStealThreadRunning AND gameIsFullscreenExclusive_
    - _Expected_Behavior: No hidden helper windows or background message pumps that interfere with FSE swapchains_
    - _Preservation: WndProc filtering and IAT hooks continue to function; DI8 force-unacquire mechanism continues to work_
    - _Requirements: 2.8, 2.9, 3.9, 3.10_

- [x] 6. Fix Bug 5: Unity engine hangs — four sub-fixes

  - [x] 6.1 Move ref count increment after successful device creation
    - **File**: `src/d3d11/d3d11_device.cpp`
    - **Function**: `D3D11DXGIDevice::CreateDevice()`
    - Move `g_sharedDeviceRefCount++` from line 3465 (before success check) to after the `if (g_sharedDevice != nullptr)` check (for reuse) and after `m_dxvkAdapter->createDevice()` succeeds (for new creation)
    - If `createDevice` throws, the ref count is not incremented
    - _Bug_Condition: isBugCondition(input) where createDeviceFailed AND refCountIncrementedBeforeCheck_
    - _Expected_Behavior: g_sharedDeviceRefCount equals number of successful device creations/reuses_
    - _Preservation: Shared DxvkDevice lifecycle (reuse across D3D11 devices, keep alive for emulators) remains_
    - _Requirements: 2.11, 2.12, 3.11, 3.12_

  - [x] 6.2 Add timeout to swapchain destructor waits
    - **File**: `src/d3d11/d3d11_swapchain.cpp`
    - **Function**: `D3D11SwapChain::~D3D11SwapChain()`
    - Replace the unconditional `waitForIdle()` with a timed wait (e.g., 5 seconds)
    - If the timeout expires, log a warning and proceed with destruction rather than blocking indefinitely
    - Ensure WndProc hook is fully disengaged before any blocking waits
    - _Bug_Condition: isBugCondition(input) where swapchainDestructorWaitsIndefinitely_
    - _Expected_Behavior: Destructor completes within bounded time; logs warning on timeout_
    - _Requirements: 2.13_

  - [x] 6.3 Add hysteresis to primary swapchain election
    - **File**: `src/d3d11/d3d11_swapchain.cpp`
    - **Function**: Primary swapchain election logic (lines 600–660)
    - Add a frame counter or timestamp to the current primary
    - Only allow a new candidate to steal primary if it has been "clearly better" for N consecutive frames (e.g., 3–5)
    - Prevents single-frame thrashing between similar swapchains in Unity
    - _Bug_Condition: isBugCondition(input) where primaryElectionThrashing_
    - _Expected_Behavior: Primary election stabilizes; no flickering in multi-swapchain engines_
    - _Preservation: Single-swapchain games elect primary without overhead; Alt+X hotkey override continues to work_
    - _Requirements: 2.14, 3.13, 3.18_

  - [x] 6.4 Bound the Remix API shutdown Release loop
    - **File**: `src/dxvk/rtx_render/rtx_remix_api.cpp`
    - **Function**: Shutdown `Release()` loop (lines 1600–1603)
    - Add a maximum iteration count (e.g., 1000) to the `while(true) { Release(); }` loop
    - If the limit is reached, log an error and break rather than spinning forever
    - _Bug_Condition: isBugCondition(input) where shutdownReleaseLoopInfinite_
    - _Expected_Behavior: Shutdown completes within bounded iterations; logs error on limit_
    - _Preservation: remixapi_Shutdown releases cleanly when no external references exist_
    - _Requirements: 2.15, 3.14_

  - [x] 6.5 Verify bug condition exploration test now passes for ref count
    - **Property 1: Expected Behavior** - Shared Device Ref Count Tracks Successful Creations
    - **IMPORTANT**: Re-run the SAME ref count test from task 1 — do NOT write a new test
    - When this test passes, it confirms `g_sharedDeviceRefCount` is 0 after failed `CreateDevice()`
    - **EXPECTED OUTCOME**: Test PASSES (confirms bug is fixed)
    - _Requirements: 2.11, 2.12_

  - [x] 6.6 Verify preservation tests still pass for swapchain election
    - **Property 2: Preservation** - Single-Swapchain Election
    - **IMPORTANT**: Re-run the SAME single-swapchain election test from task 2 — do NOT write new tests
    - **EXPECTED OUTCOME**: Tests PASS (confirms no regressions)

- [x] 7. Fix Bug 8: Raytrace mode blocked — add fallback for Quality layer write

  - [x] 7.1 Add fallback mechanism for blocked Quality layer write
    - **File**: `src/dxvk/rtx_render/rtx_options.cpp`
    - **Function**: `applyAutoQualityRaytraceMode` lambda in `updateRaytraceModePresets()` (lines 826–886)
    - After the existing retry fails, add a fallback that writes to the Derived layer or directly sets the resolved value via a force mechanism that bypasses blend threshold checks
    - Log the fallback path so it is diagnosable
    - Ensure `renderPassIntegrateIndirectRaytraceMode()` resolves to `TraceRay` on NVIDIA GPUs with Auto preset
    - _Bug_Condition: isBugCondition(input) where qualityLayerWriteValue != resolvedValue AND noStrongerLayerExists_
    - _Expected_Behavior: Raytrace mode resolves to auto-detected value (TraceRay for NVIDIA) via Quality layer or fallback_
    - _Preservation: Non-Auto raytrace mode presets skip auto-detection; AMD/Intel defaults remain RayQuery; successful Quality layer writes don't trigger fallback_
    - _Requirements: 2.22, 2.23, 3.21, 3.22, 3.23_

- [x] 8. Fix Bugs 2 & 3: UI settings not persisting — add user-override tracking

  - [x] 8.1 Add user-override flag to RtxOption
    - **File**: `src/dxvk/rtx_render/rtx_option.h`
    - Add a `bool m_userOverridden = false;` flag to `RtxOptionImpl` (or the per-option state)
    - When `clearFromStrongerLayers` is called from a UI context (User layer), set this flag to `true`
    - _Requirements: 2.5, 2.6, 2.7_

  - [x] 8.2 Implement override guard in layer population
    - **File**: `src/dxvk/rtx_render/rtx_option.cpp`
    - When a layer value is being set during config re-read (not via `setImmediately` from UI), check the `m_userOverridden` flag and skip if set
    - This prevents stronger layers from overriding the user's explicit UI change on re-population
    - _Bug_Condition: isBugCondition(input) where userChangedOptionViaUI AND strongerLayerExistsForOption AND strongerLayerRePopulatedThisFrame_
    - _Expected_Behavior: User's UI change persists across frames; stronger layer re-population skips user-overridden options_
    - _Requirements: 2.5, 2.6, 2.7_

  - [x] 8.3 Reset override on explicit reset
    - **File**: `src/dxvk/rtx_render/rtx_option.cpp`
    - When the `RtxOptionUxWrapper` reset button is clicked, clear the `m_userOverridden` flag so the option returns to normal layer resolution
    - _Preservation: RtxOptionUxWrapper reset button continues to reset options to defaults_
    - _Requirements: 3.6_

  - [x] 8.4 Mark user override in IMGUI_RTXOPTION_WIDGET macro
    - **File**: `src/dxvk/rtx_render/rtx_imgui.h`
    - In the `IMGUI_RTXOPTION_WIDGET` macro, after `clearFromStrongerLayers` and `setImmediately`, call a method to mark the option as user-overridden
    - _Requirements: 2.7_

  - [x] 8.5 Mark user override in Checkbox function
    - **File**: `src/dxvk/rtx_render/rtx_imgui.cpp`
    - In `Checkbox(const char*, RtxOption<bool>*)`, after the `clearFromStrongerLayers` / `setImmediately` sequence, mark the option as user-overridden
    - _Requirements: 2.5_

  - [x] 8.6 Verify bug condition exploration test now passes for UI persistence
    - **Property 1: Expected Behavior** - UI Option Changes Persist Across Frames
    - **IMPORTANT**: Re-run the SAME UI persistence test from task 1 — do NOT write a new test
    - When this test passes, it confirms user's checkbox value persists across frames
    - **EXPECTED OUTCOME**: Test PASSES (confirms bug is fixed)
    - _Requirements: 2.5, 2.6, 2.7_

  - [x] 8.7 Verify preservation tests still pass for programmatic layer writes
    - **Property 2: Preservation** - Programmatic Layer Writes Respect Priority
    - **IMPORTANT**: Re-run the SAME programmatic layer write tests from task 2 — do NOT write new tests
    - **EXPECTED OUTCOME**: Tests PASS (confirms no regressions)
    - _Preservation: Config file, preset, and game-target layer values continue to be respected; layer priority system unchanged for non-UI writes_

- [x] 9. Fix Bug 6: Performance / memory leaks — three sub-fixes

  - [x] 9.1 Add periodic shrink-to-fit for texture cache
    - **File**: `src/dxvk/rtx_render/rtx_texture_manager.cpp`
    - After the texture cache's `clear()` / demotion pass, periodically call `shrink_to_fit()` on the underlying vector (e.g., every 60 seconds or when the eviction count exceeds a threshold)
    - _Bug_Condition: isBugCondition(input) where textureCacheEvicted AND NOT containerShrunk_
    - _Expected_Behavior: Container shrinks proportionally to demoted textures; memory usage stabilizes_
    - _Preservation: Texture cache budget-based demotion/promotion continues to work; preloadTextureAsset deduplication remains_
    - _Requirements: 2.16, 3.15, 3.16, 3.17_

  - [x] 9.2 Add time-based eviction for asset hash map
    - **File**: `src/dxvk/rtx_render/rtx_texture_manager.cpp`
    - Track last-access time for `m_assetHashToTextures` entries (lines 1449–1511)
    - Periodically evict entries that haven't been accessed within a configurable TTL (e.g., 5 minutes)
    - _Bug_Condition: isBugCondition(input) where assetMapEntryCount > maxAllowed_
    - _Expected_Behavior: Asset hash map has bounded growth with time-based eviction_
    - _Requirements: 2.17_

  - [x] 9.3 Handle sampler feedback overflow gracefully
    - **File**: `src/dxvk/rtx_render/rtx_texture_manager.cpp`
    - When the sampler feedback count exceeds `SAMPLER_FEEDBACK_MAX_TEXTURE_COUNT` (lines 1497–1502), evict the oldest feedback entry or cleanly reject the new entry instead of hitting `assert(0)` and continuing with `SAMPLER_FEEDBACK_INVALID`
    - _Bug_Condition: isBugCondition(input) where samplerFeedbackCount > SAMPLER_FEEDBACK_MAX_
    - _Expected_Behavior: Overflow handled gracefully; no undefined behavior from invalid sentinel_
    - _Requirements: 2.18_

- [x] 10. Fix Bug 9: Duplicate objectPickingValue — improve warning message

  - [x] 10.1 Improve warning message for duplicate picking values
    - **File**: `src/dxvk/rtx_render/rtx_scene_manager.cpp`
    - When multiple draw calls share the same `objectPickingValue` (lines 1048–1066), update the warning to include the actual picking value and collision count
    - New format: `"Found N draw calls with objectPickingValue=V. Using merged MetaInfo for object picking. Consider assigning unique picking values via remixapi_InstanceInfoObjectPickingEXT."`
    - _Bug_Condition: isBugCondition(input) where multipleDrawCallsSharePickingValue_
    - _Expected_Behavior: Warning includes picking value and collision count for diagnosis_
    - _Preservation: mergeFrom deduplication via addUniqueHash remains; g_allowMappingLegacyHashToObjectPickingValue gating remains; gatherObjectPickingValuesByTextureHash reverse lookup remains_
    - _Requirements: 2.27, 3.25, 3.26, 3.27_

  - [x] 10.2 (Stretch goal) Consider unique sub-IDs for disambiguation
    - **File**: `src/dxvk/rtx_render/rtx_scene_manager.cpp`
    - When a collision is detected, assign unique sub-IDs (e.g., `pickingValue * 1000 + collisionIndex`) to disambiguate instances
    - This is a stretch goal — the improved warning in 10.1 is the minimum fix
    - _Requirements: 2.24, 2.25, 2.26_

  - [x] 10.3 Verify preservation tests still pass for unique picking values
    - **Property 2: Preservation** - Unique Picking Value Mapping
    - **IMPORTANT**: Re-run the SAME unique picking value tests from task 2 — do NOT write new tests
    - **EXPECTED OUTCOME**: Tests PASS (confirms no regressions for unique picking values)

- [x] 11. Checkpoint — Ensure all tests pass
  - Run all bug condition exploration tests (Property 1) — all should PASS after fixes
  - Run all preservation property tests (Property 2) — all should still PASS after fixes
  - Run any existing project unit tests to confirm no regressions
  - Ensure all tests pass, ask the user if questions arise
