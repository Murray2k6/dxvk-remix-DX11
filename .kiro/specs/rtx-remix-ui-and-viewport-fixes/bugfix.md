# Bugfix Requirements Document

## Introduction

This document covers nine bugs in the DXVK Remix (RTX Remix DX11) project spanning viewport crashes, UI persistence, game focus/minimize issues, engine compatibility hangs, performance/memory leaks, non-invertible matrix errors, raytrace mode auto-resolution failures, and object picking ambiguity.

**Bug 1 (Viewport Crash):** A runtime crash affecting Unreal Engine 4 games caused by `getCurrentViewportCount()` in `rtx_context.h` returning the fixed array size (always 16) instead of the actual active viewport count, which corrupts rasterizer state when the dust particle system saves and restores viewports. Additionally, `setupRasterizerState` in `rtx_dust_particles.cpp` contains dead code that saves viewport/render-target state to unused local variables, and the build log (`build.log`) is stale.

**Bugs 2 & 3 (UI Checkboxes / UI Settings):** Share a common root cause in the RtxOption priority-based layer system: when a user changes a checkbox or combo/dropdown selection in the RTX Remix settings UI, the change is written to the User layer but a stronger layer (preset, quality, config-file, or game-target layer) that is re-populated each frame overrides the resolved value, causing the UI to revert on the next frame.

**Bug 4 (Game Minimize):** The FocusSteal system in `dxvk_imgui.cpp` creates a hidden helper window and a dedicated thread that runs continuously even though focus stealing is permanently disabled. The thread's hidden `WS_POPUP` window and message pump can interfere with fullscreen-exclusive games, causing them to minimize. Additionally, the WndProc hook installed by `D3D11SwapChain` via `SetWindowLongPtrW` can trigger `WM_ACTIVATE` messages that cause fullscreen games to minimize.

**Bug 5 (Unity Engine Hangs):** Multiple issues contribute to Unity and other engines hanging during initialization or gameplay: a shared device ref count leak in `CreateDevice()` that increments before checking success, swapchain destruction blocking the message queue via `waitForIdle()`, primary swapchain election thrashing between similar swapchains, and an infinite `Release()` loop in the Remix API shutdown path.

**Bug 6 (Performance / Memory Leaks):** Several unbounded growth patterns cause degraded performance over long play sessions: the texture cache's underlying vector never shrinks, the asset hash map grows monotonically without eviction, the disabled FocusSteal thread wastes CPU cycles, and sampler feedback array overflow leads to undefined behavior.

**Bug 7 (Non-Invertible Matrix Error):** The `inverse()` function in `src/util/util_matrix.h` logs an error via `mathValidationAssert` when the determinant is zero but continues execution, performing division by zero. This produces NaN/Inf values that propagate through camera matrices, transform matrices, and lighting calculations, causing visual corruption, cascading NaN propagation, and potential GPU hangs.

**Bug 8 (Raytrace Mode Auto-Resolution Blocked):** The `updateRaytraceModePresets()` function in `src/dxvk/rtx_render/rtx_options.cpp` attempts to auto-detect the optimal raytrace mode based on GPU vendor and writes it to the Quality layer (priority `0xFFFFFFFF`). However, `setImmediately` to the Quality layer does not produce the expected resolved value — the resolved value remains 0 (RayQuery) instead of the written 1 (TraceRay). Since the Quality layer is already the highest priority, `clearFromStrongerLayers` has no effect, and the retry also fails, leaving NVIDIA GPUs running with suboptimal RayQuery mode. This is a fundamental problem that prevents Remix from auto-configuring optimal settings — nothing should be blocked by the layer system, yet the very mechanism designed to set the best raytrace mode for the user's GPU is defeated by the layer priority logic, undermining Remix's ability to function properly out of the box.

**Bug 9 (Duplicate objectPickingValue Across Draw Calls):** When multiple draw calls share the same `objectPickingValue`, their `DrawCallMetaInfo` is merged via `mergeFrom()` into a single map entry in `m_drawCallMeta.infos`. This causes object picking in the Remix UI to become ambiguous — clicking one object may select a different one, and highlighting via `requestHighlighting()` highlights all objects sharing the same picking value. The duplicates arise when the Remix API explicitly sets the same `objectPickingValue` on multiple instances via `remixapi_InstanceInfoObjectPickingEXT`, or when multiple instances share the same underlying draw call state. The warning is logged but lacks the actual picking value and collision count, making it difficult for users to diagnose.

## Bug Analysis

### Current Behavior (Defect)

1.1 WHEN `getCurrentViewportCount()` is called on `RtxContext` THEN the system returns `m_state.vp.viewports.size()` which is always 16 because `DxvkViewportState::viewports` is a `std::array<VkViewport, 16>` (fixed-size), instead of returning the actual active viewport count stored in `m_state.gp.state.rs.viewportCount()`

1.2 WHEN `simulateAndDraw` in `rtx_dust_particles.cpp` saves the viewport count via `ctx->getCurrentViewportCount()` and later restores it with `ctx->setViewports(prevViewportCount, ...)` THEN the system passes 16 viewports to `setViewports` — 15 of which are zero-initialized — corrupting the rasterizer state's viewport count and causing crashes or rendering corruption in Unreal Engine 4 games that only set 1 active viewport

1.3 WHEN `setupRasterizerState` in `rtx_dust_particles.cpp` is called THEN the system saves viewport count, viewport state, and render targets to local variables (`prevViewportCount`, `prevViewportState`, `prevRenderTargets`) that are never used — the comment says "restored by the caller via dxvkCtxState = stateCopy" but the dead save code is misleading and wasteful

1.4 WHEN the project is built THEN the build log (`build.log`) contains stale compilation errors referencing `getSpecifiedViewportState()` and `getSpecifiedRenderTargets()` as missing members of `RtxContext`, even though the source code has already been renamed to `getCurrentViewportState()` and `getCurrentRenderTargets()`

1.5 WHEN a user clicks a checkbox in the RTX Remix settings UI and a stronger RtxOption layer (e.g., a preset/quality layer, rtx.conf layer, or game-target layer) holds an opinion on the same option THEN the checkbox toggles momentarily but reverts to its previous state on the next frame because the stronger layer's value overrides the User layer during `resolveValue()`

1.6 WHEN a user selects a different option in a dropdown/combo box in the RTX Remix settings UI and a stronger RtxOption layer holds an opinion on the same option THEN the selection does not persist and reverts to the previous value on the next frame due to the same layer-priority override mechanism

1.7 WHEN a user modifies any RtxOption widget (DragFloat, SliderInt, ColorEdit, etc.) via the `IMGUI_RTXOPTION_WIDGET` macro and a stronger layer is re-populated each frame from config files or game targets THEN the user's change is overwritten on the next frame because `clearFromStrongerLayers` only removes the layer entry for the current frame but the layer is re-read and re-populated before the next resolve

1.8 WHEN the Remix overlay is initialized THEN the system starts the FocusSteal thread (`startFocusStealThread()` at line 1274 of `dxvk_imgui.cpp`) which creates a hidden `WS_POPUP` helper window and runs a message pump continuously, even though `g_focusStealWanted` is permanently set to `false` and the focus steal feature is documented as "PERMANENTLY DISABLED" (lines 1981–1986)

1.9 WHEN a fullscreen-exclusive game is running and the FocusSteal helper window exists THEN the hidden window's presence and its message pump can interfere with the fullscreen-exclusive swapchain, causing Windows to minimize the game — as explicitly documented in the code comments: "calling SetForegroundWindow on an off-screen helper window forces Windows to minimise the FSE swapchain and the game vanishes"

1.10 WHEN `D3D11SwapChain` is constructed THEN the system installs a WndProc hook via `SetWindowLongPtrW(hWnd, GWLP_WNDPROC, ...)` (line 239–250 of `d3d11_swapchain.cpp`) which can trigger `WM_ACTIVATE` messages that cause fullscreen-exclusive games to minimize when the subclassing changes the window's message processing chain

1.11 WHEN `D3D11DXGIDevice::CreateDevice()` is called THEN the system increments `g_sharedDeviceRefCount` (line 3465 of `d3d11_device.cpp`) BEFORE checking if a shared device already exists or if `m_dxvkAdapter->createDevice()` succeeds — if device creation fails or throws, the ref count is incremented but no device is created, causing `ReleaseSharedDevice()` to never reach zero and leaking the device

1.12 WHEN Unity or other engines create multiple D3D11 devices during initialization probing and one or more `createDevice()` calls fail THEN the leaked ref count prevents proper cleanup, causing the engine to hang or fail to initialize correctly

1.13 WHEN `D3D11SwapChain` is destroyed THEN the destructor calls `m_device->waitForSubmission()` and `m_device->waitForIdle()` (lines 265–283 of `d3d11_swapchain.cpp`) which can block indefinitely if the GPU is stalled, while the WndProc hook has been removed but the game's WndProc may still be processing messages that reference the swapchain

1.14 WHEN Unity creates multiple swapchains (one per camera/render target) with similar characteristics THEN the primary swapchain election logic (lines 600–660 of `d3d11_swapchain.cpp`) can thrash back and forth between candidates every frame, causing the Remix UI to flicker or not render at all

1.15 WHEN `remixapi_Shutdown()` is called and another thread holds a reference to `s_d3d11Device` THEN the `while(true) { ULONG left = s_d3d11Device->Release(); if (left == 0) break; }` loop (lines 1600–1603 of `rtx_remix_api.cpp`) spins forever, hanging the shutdown process

1.16 WHEN the game runs for an extended play session THEN the texture cache's underlying `SparseUniqueCache<TextureRef>` vector in `rtx_texture_manager.cpp` only grows and never releases memory — the `clear()` method demotes textures when over budget but the container itself never shrinks, causing monotonic memory growth

1.17 WHEN replacement textures are loaded over time THEN the `m_assetHashToTextures` map (lines 1449–1511 of `rtx_texture_manager.cpp`) grows unbounded because entries are only removed when a new asset with the same hash is loaded — there is no eviction based on memory pressure or time

1.18 WHEN the number of textures with sampler feedback exceeds `SAMPLER_FEEDBACK_MAX_TEXTURE_COUNT` THEN the code hits `assert(0)` and logs an error but continues with `SAMPLER_FEEDBACK_INVALID` (lines 1497–1502 of `rtx_texture_manager.cpp`), which can cause undefined behavior in the feedback system

1.19 WHEN the `inverse()` function in `src/util/util_matrix.h` (line 389) is called with a non-invertible matrix (determinant `dot1 == 0.0`) THEN the `mathValidationAssert` macro logs the error "Attempted invert a non-invertible matrix." once via `ONCE(Logger::err(...))` but continues execution, performing `inverse[i/4][i%4] / dot1` where `dot1 == 0.0`, producing NaN/Inf values in the output matrix

1.20 WHEN the NaN/Inf values from a failed matrix inversion propagate into camera matrices (`rtx_camera.cpp` lines 301, 306, 333, 334, 359, 649, 663, 686, 696, 698, 721, 763, 768, 929), normal matrix computations (`rtx_instance_manager.cpp` lines 190, 208, 217, 235, 249), camera position extraction (`rtx_context.cpp` line 2614), or portal transform inversions (`rtx_ray_portal_manager.cpp` lines 302, 310, 598) THEN the system exhibits visual corruption (flickering, black screens, geometry disappearing), cascading NaN propagation through the entire frame, and potential GPU hangs on some drivers when NaN values reach shader inputs

1.21 WHEN games provide degenerate matrices (zero scale, collapsed geometry, uninitialized transforms) during scene loading/transitions, object culling, or camera transitions THEN the `inverse()` function produces NaN/Inf output instead of a safe fallback, causing rendering pipeline corruption

1.22 WHEN `updateRaytraceModePresets()` in `src/dxvk/rtx_render/rtx_options.cpp` (lines 826–886) runs during initialization on an NVIDIA GPU with `RaytraceModePreset::Auto` THEN the `applyAutoQualityRaytraceMode` lambda calls `option.setImmediately(preferredValue, RtxOptionLayer::getQualityLayer())` to write `TraceRay` (value 1) to the Quality layer (priority `0xFFFFFFFF`), but the subsequent check `option() == preferredValue` fails because the resolved value remains 0 (RayQuery) instead of the written value

1.23 WHEN the `applyAutoQualityRaytraceMode` lambda detects the mismatch and calls `option.clearFromStrongerLayers(RtxOptionLayer::getQualityLayer())` THEN the call has no effect because the Quality layer is already the highest priority layer (priority `0xFFFFFFFF`) — there are no stronger layers to clear — and the retry `setImmediately` also fails, logging "renderPassIntegrateIndirectRaytraceMode is still not using the auto-selected value after retry. Current value=0" and leaving the game running with suboptimal RayQuery mode instead of TraceRay on NVIDIA GPUs

1.24 WHEN the Remix API sets the same `objectPickingValue` on multiple instances via `remixapi_InstanceInfoObjectPickingEXT` (line 808 of `rtx_remix_api.cpp`), overriding the auto-incrementing `drawCallID` THEN the system merges all their `DrawCallMetaInfo` into a single map entry via `mergeFrom()` in `rtx_scene_manager.cpp` (lines 1048–1066), accumulating all legacy texture hashes, geometry hashes, and material hashes from all colliding draw calls into one entry — `getPrimaryLegacyTextureHash()` returns the hash from whichever draw call was processed first, making object selection ambiguous

1.25 WHEN a user clicks on an object in the Remix UI viewport to select it and that object's `objectPickingValue` is shared by multiple draw calls THEN `findLegacyTextureHashByObjectPickingValue()` returns the primary legacy texture hash from the merged `DrawCallMetaInfo`, which may correspond to a different object than the one the user intended to select — the selection is non-deterministic with respect to the user's click target

1.26 WHEN `requestHighlighting()` is called for an object whose `objectPickingValue` is shared by multiple draw calls THEN the highlighting shader highlights ALL objects with that picking value, not just the intended one, because the GPU-side picking buffer contains the same value for all colliding instances

1.27 WHEN multiple draw calls share the same `objectPickingValue` THEN the system logs a warning but the message does not include the actual picking value or the number of colliding draw calls, making it difficult for users and developers to diagnose which objects are affected and why object selection is behaving incorrectly

### Expected Behavior (Correct)

2.1 WHEN `getCurrentViewportCount()` is called on `RtxContext` THEN the system SHALL return `m_state.gp.state.rs.viewportCount()` — the actual number of active viewports set by the game — matching the correct pattern already used in `rtx_terrain_baker.cpp`

2.2 WHEN `simulateAndDraw` in `rtx_dust_particles.cpp` saves and restores viewport state THEN the system SHALL save the correct active viewport count (e.g., 1 for UE4 games) and restore exactly that many viewports, preventing rasterizer state corruption and crashes

2.3 WHEN `setupRasterizerState` in `rtx_dust_particles.cpp` is called THEN the system SHALL NOT contain dead code that saves viewport/render-target state to unused local variables — the dead save code SHALL be removed to avoid confusion since the caller (`simulateAndDraw`) is responsible for save/restore

2.4 WHEN the project is built with the current source code THEN the build log SHALL reflect the current build state and SHALL NOT contain stale errors referencing renamed methods

2.5 WHEN a user clicks a checkbox in the RTX Remix settings UI THEN the system SHALL persist the user's chosen value across frames by ensuring the User layer write is not overridden by stronger layers that are re-populated each frame — the checkbox SHALL remain in the state the user set it to

2.6 WHEN a user selects a different option in a dropdown/combo box in the RTX Remix settings UI THEN the system SHALL persist the user's selection across frames by ensuring the User layer write is not overridden by stronger layers — the combo box SHALL display the user's chosen selection on subsequent frames

2.7 WHEN a user modifies any RtxOption widget via the `IMGUI_RTXOPTION_WIDGET` macro THEN the system SHALL ensure the user's change persists across frames by preventing stronger layers from overriding the User layer value after the user has explicitly made a change through the UI

2.8 WHEN the Remix overlay is initialized THEN the system SHALL NOT start the FocusSteal thread or create the hidden helper window, since the focus steal feature is permanently disabled and the DI8 force-unacquire plus IAT hooks already handle input isolation

2.9 WHEN a fullscreen-exclusive game is running THEN the system SHALL NOT have any hidden helper windows or background message pumps that can interfere with the fullscreen-exclusive swapchain — games SHALL remain in fullscreen without being minimized by Remix infrastructure

2.10 WHEN `D3D11SwapChain` installs its WndProc hook THEN the system SHALL do so in a way that does not trigger spurious `WM_ACTIVATE` messages that cause fullscreen-exclusive games to minimize — the hook installation SHALL be safe for FSE swapchains

2.11 WHEN `D3D11DXGIDevice::CreateDevice()` is called THEN the system SHALL only increment `g_sharedDeviceRefCount` AFTER confirming that a device was successfully created or reused — if `createDevice()` fails or throws, the ref count SHALL NOT be incremented

2.12 WHEN Unity or other engines create multiple D3D11 devices during initialization probing THEN the system SHALL correctly track the shared device ref count so that failed device creation attempts do not leak references, allowing proper cleanup and preventing engine hangs

2.13 WHEN `D3D11SwapChain` is destroyed THEN the system SHALL ensure that `waitForSubmission()` and `waitForIdle()` cannot block indefinitely — the destructor SHALL use a timeout or ensure the GPU is not stalled before waiting, and the WndProc hook SHALL be fully disengaged before any blocking waits

2.14 WHEN Unity creates multiple swapchains with similar characteristics THEN the primary swapchain election logic SHALL include hysteresis or a stability mechanism to prevent thrashing between candidates — once a primary is elected, it SHALL remain primary unless a clearly better candidate appears with a sufficient margin

2.15 WHEN `remixapi_Shutdown()` is called THEN the system SHALL NOT spin indefinitely releasing `s_d3d11Device` — the shutdown loop SHALL have a bounded iteration count or timeout to prevent hangs when another thread holds a reference

2.16 WHEN the game runs for an extended play session THEN the texture cache SHALL periodically release memory from its underlying container when textures are evicted — the container SHALL shrink or reclaim memory proportionally to the number of demoted textures

2.17 WHEN replacement textures are loaded over time THEN the `m_assetHashToTextures` map SHALL implement an eviction policy based on memory pressure, time since last access, or a maximum entry count to prevent unbounded growth

2.18 WHEN the number of textures with sampler feedback exceeds `SAMPLER_FEEDBACK_MAX_TEXTURE_COUNT` THEN the system SHALL handle the overflow gracefully without undefined behavior — either by evicting the oldest feedback entries, expanding the capacity, or cleanly rejecting new entries with a well-defined fallback

2.19 WHEN the `inverse()` function in `src/util/util_matrix.h` is called with a non-invertible matrix (determinant `dot1 == 0.0`) THEN the system SHALL return an identity matrix instead of performing division by zero — the error SHALL still be logged via `mathValidationAssert`, but the output matrix SHALL contain only valid finite values to prevent NaN/Inf propagation

2.20 WHEN camera matrices, normal matrices, camera position extraction, or portal transform inversions call `inverse()` with a degenerate matrix THEN the system SHALL receive a valid identity matrix fallback, preventing visual corruption, cascading NaN propagation, and GPU hangs — the rendering pipeline SHALL continue with a safe default rather than corrupted values

2.21 WHEN games provide degenerate matrices during scene loading/transitions, object culling, or camera transitions THEN the `inverse()` function SHALL produce a valid identity matrix output and log the error, allowing the rendering pipeline to gracefully handle the degenerate input without corruption

2.22 WHEN `updateRaytraceModePresets()` runs during initialization on an NVIDIA GPU with `RaytraceModePreset::Auto` THEN the system SHALL successfully set `renderPassIntegrateIndirectRaytraceMode` to `TraceRay` (value 1) so that the resolved value matches the auto-detected optimal mode — the Quality layer write SHALL produce the expected resolved value

2.23 WHEN the Quality layer write for raytrace mode does not produce the expected resolved value THEN the system SHALL fall back to writing the value to the Derived layer or directly resolving the conflict, ensuring the auto-detected optimal raytrace mode is applied — NVIDIA GPUs SHALL run with `TraceRay` mode for indirect lighting as intended by the auto-detection logic

2.24 WHEN the Remix API sets the same `objectPickingValue` on multiple instances via `remixapi_InstanceInfoObjectPickingEXT` THEN the system SHALL either assign unique sub-IDs to disambiguate the instances (e.g., appending a per-frame collision counter to the base picking value) or warn more prominently so that the user can assign unique picking values — each instance SHALL be individually selectable in the Remix UI

2.25 WHEN a user clicks on an object in the Remix UI viewport to select it THEN the system SHALL select only the specific object under the cursor, even if multiple draw calls share the same `objectPickingValue` — the selection SHALL be unambiguous and correspond to the user's click target

2.26 WHEN `requestHighlighting()` is called for a specific object THEN the system SHALL highlight only the intended object, not all objects that happen to share the same `objectPickingValue` — highlighting SHALL be scoped to the individual instance the user selected

2.27 WHEN multiple draw calls share the same `objectPickingValue` THEN the warning message SHALL include the actual picking value and the number of colliding draw calls (e.g., "Found 3 draw calls with objectPickingValue=42. Using the most recent MetaInfo for object picking. Consider assigning unique picking values via remixapi_InstanceInfoObjectPickingEXT.") to help users diagnose and resolve the ambiguity

### Unchanged Behavior (Regression Prevention)

3.1 WHEN the terrain baker in `rtx_terrain_baker.cpp` reads the viewport count via `dxvkCtxState.gp.state.rs.viewportCount()` THEN the system SHALL CONTINUE TO correctly save and restore viewport state during terrain baking operations

3.2 WHEN `setViewports` is called with viewports that have zero width or height THEN the system SHALL CONTINUE TO replace them with 1x1 dummy viewports and empty scissor rects as a safety fallback

3.3 WHEN `simulateAndDraw` saves and restores render targets via `ctx->getCurrentRenderTargets()` and `ctx->bindRenderTargets()` THEN the system SHALL CONTINUE TO correctly restore render target state after dust particle rendering

3.4 WHEN no stronger layer holds an opinion on an RtxOption being edited via the UI THEN the system SHALL CONTINUE TO write the value to the User layer and resolve it correctly without any additional overhead

3.5 WHEN an RtxOption value is set programmatically (not through the UI) via a config file, preset, or game-target layer THEN the system SHALL CONTINUE TO respect the layer priority system and resolve values according to layer strength ordering

3.6 WHEN the `RtxOptionUxWrapper` reset button is clicked THEN the system SHALL CONTINUE TO reset the option to its default value correctly

3.7 WHEN the full-row click-through area of a checkbox is clicked (clicking on the label text rather than the checkbox square) THEN the system SHALL CONTINUE TO toggle the checkbox value as expected

3.8 WHEN the `ComboWithKey::getKey(RtxOption<R>*)` method is used THEN the system SHALL CONTINUE TO log selection changes via the `[Settings] Combo` logger and track the last logged index correctly

3.9 WHEN the Remix UI is opened in a borderless-windowed or windowed game THEN the system SHALL CONTINUE TO block game input (mouse, keyboard, raw input) while the overlay is active via the existing WndProc filtering and IAT hooks

3.10 WHEN the Remix UI is opened and DirectInput devices are acquired with `DISCL_FOREGROUND` THEN the system SHALL CONTINUE TO isolate game input via the DI8 force-unacquire mechanism and IAT `SetCursorPos`/`ClipCursor` hooks without relying on the FocusSteal thread

3.11 WHEN a single D3D11 device is created successfully THEN the system SHALL CONTINUE TO share the `DxvkDevice` across all `D3D11DXGIDevice` instances and release it only when the last instance is destroyed

3.12 WHEN an emulator (Dolphin, RPCS3) rapidly creates and destroys D3D11 devices during game switching THEN the system SHALL CONTINUE TO keep the shared `DxvkDevice` alive to avoid the ~2s cost of re-creating the Vulkan device and Remix state

3.13 WHEN `D3D11SwapChain` is the sole swapchain for a single-window game THEN the system SHALL CONTINUE TO elect it as primary without any election overhead or delay

3.14 WHEN `remixapi_Shutdown()` is called and no other thread holds a reference to `s_d3d11Device` THEN the system SHALL CONTINUE TO release the device cleanly and return `REMIXAPI_ERROR_CODE_SUCCESS`

3.15 WHEN textures are loaded within the configured memory budget THEN the system SHALL CONTINUE TO cache and serve them efficiently without unnecessary eviction or memory reclamation overhead

3.16 WHEN the `SparseUniqueCache` demotes textures that exceed the memory budget THEN the system SHALL CONTINUE TO correctly demote and re-promote textures based on sampler feedback and access patterns

3.17 WHEN `preloadTextureAsset` is called with an asset whose hash matches an existing entry with identical `AssetInfo` THEN the system SHALL CONTINUE TO return the cached `ManagedTexture` without creating a duplicate

3.18 WHEN the Alt+X hotkey is pressed to force primary swapchain election THEN the system SHALL CONTINUE TO override the automatic election and assign primary status to the visible swapchain associated with the pressed window

3.19 WHEN the `inverse()` function is called with an invertible matrix (non-zero determinant) THEN the system SHALL CONTINUE TO compute and return the correct mathematical inverse without any identity matrix fallback — the existing inversion logic for valid matrices SHALL remain unchanged

3.20 WHEN `mathValidationAssert` is triggered for a non-invertible matrix THEN the system SHALL CONTINUE TO log the error message "Attempted invert a non-invertible matrix." via `ONCE(Logger::err(...))` at the `ErrorOnce` validation level — the logging behavior SHALL remain unchanged

3.21 WHEN `updateRaytraceModePresets()` runs with `RaytraceModePreset` set to a value other than `Auto` THEN the system SHALL CONTINUE TO skip the automatic raytrace mode detection and use the explicitly configured preset

3.22 WHEN `updateRaytraceModePresets()` runs on AMD GPUs (proprietary driver) or Intel/other GPUs THEN the system SHALL CONTINUE TO set all raytrace modes to `RayQuery` as the default for those architectures — the vendor-specific detection logic SHALL remain unchanged

3.23 WHEN `updateRaytraceModePresets()` successfully writes the preferred raytrace mode to the Quality layer and the resolved value matches THEN the system SHALL CONTINUE TO use that value without any fallback mechanism — the fallback SHALL only activate when the Quality layer write fails to produce the expected resolved value

3.24 WHEN each draw call has a unique `objectPickingValue` (the common case with auto-incrementing `drawCallID`) THEN the system SHALL CONTINUE TO map each picking value to its own `DrawCallMetaInfo` entry without any merging — object picking and highlighting SHALL work exactly as before with no additional overhead

3.25 WHEN `g_allowMappingLegacyHashToObjectPickingValue` is `false` (editor mode via `remixapi_StartupInfo::editorModeEnabled`) THEN the system SHALL CONTINUE TO skip the legacy-hash-to-picking-value mapping entirely — the `DrawCallMetaInfo` table SHALL NOT be populated and the object picking path SHALL remain unchanged

3.26 WHEN `gatherObjectPickingValuesByTextureHash()` is called to find all picking values associated with a legacy texture hash THEN the system SHALL CONTINUE TO return all matching picking values from the current or previous tick's info table — the reverse lookup from texture hash to picking values SHALL remain functional

3.27 WHEN the `mergeFrom()` function merges `DrawCallMetaInfo` from draw calls with the same picking value THEN the system SHALL CONTINUE TO deduplicate hashes via `addUniqueHash()` — duplicate legacy texture hashes, geometry hashes, and material hashes SHALL NOT be added to the vectors more than once
