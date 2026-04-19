# Bugfix Requirements Document

## Introduction

The DXVK Remix (RTX Remix DX11) renderer has three interrelated viewport and resolution correctness bugs that prevent games from path tracing correctly. The primary swapchain election logic can pick a smaller or secondary swapchain (debug window, side panel, video overlay) instead of the main game window, causing the Remix UI and ray tracing output to render on the wrong surface. Additionally, the in-game resolution does not need to match the user's desktop/monitor resolution for Remix to work, but the current election heuristics penalize swapchains whose backbuffer dimensions differ from their window's client rect — a common scenario when games render at a lower internal resolution and let the display driver or compositor upscale. Finally, path tracing initialization can fail or produce incorrect output when the elected primary's backbuffer dimensions change between frames (resize, fullscreen toggle) or when the backbuffer resolution is significantly smaller than the window, because the ray tracing resource allocation and camera jitter calculations assume a stable, correctly-sized target image.

## Bug Analysis

### Current Behavior (Defect)

1.1 WHEN a game creates multiple swapchains (e.g., Unity, UE4, emulators) and a secondary swapchain has a backbuffer that more closely matches its window's client rect than the main game swapchain does, THEN the primary election picks the secondary swapchain as primary, causing the Remix UI overlay and ray tracing output to render on the wrong surface (a debug window, video overlay, or loading screen).

1.2 WHEN a game renders at an internal resolution lower than its window size (e.g., 720p game in a 1080p window) THEN the `exactClientMatch` and `nearClientMatch` checks in `isClearlyBetterCandidate` penalize the main game swapchain because its backbuffer dimensions do not closely match the window's client rect, making it easier for a secondary swapchain to steal primary.

1.3 WHEN the primary swapchain's backbuffer resolution changes between frames (due to a resize, fullscreen toggle, or resolution setting change) THEN the ray tracing resource manager may use stale target dimensions for one or more frames, causing resource validation to fail and forcing an expensive inline re-creation of all ray tracing output resources via `resetScreenResolution`.

1.4 WHEN a game's backbuffer resolution is significantly smaller than the window client rect (e.g., a 640×480 game in a 1920×1080 window) THEN the `getClientExtent()` passed to `EndFrame` and `OnPresent` reports the window size rather than the backbuffer size, and downstream systems that compare or combine these two extents (resize transition detection, camera carry-over grace period) can misinterpret a stable low-resolution game as continuously resizing.

1.5 WHEN the primary election thrashes between two swapchains of similar quality across consecutive frames (even with the 3-frame hysteresis) THEN the ray tracing pipeline alternates between two different backbuffer resolutions, causing repeated resource re-creation, camera jitter resets, and visible flickering or black frames.

1.6 WHEN a game creates a swapchain on a secondary/debug window that is not the main game window but that window is visible and has draw calls THEN the primary election may select it because `isClearlyBetterCandidate` does not distinguish between the main game window and auxiliary windows — it only checks visibility, client match, draws, and area.

### Expected Behavior (Correct)

2.1 WHEN a game creates multiple swapchains THEN the primary election SHALL prefer the swapchain whose window is the foreground/topmost game window, using window Z-order or foreground status as a tiebreaker when visibility, size, and draw count are similar.

2.2 WHEN a game renders at an internal resolution that does not match its window's client rect THEN the primary election SHALL NOT penalize the swapchain for the resolution mismatch — the backbuffer dimensions alone (area and draw count) SHALL be sufficient to identify the main game swapchain without requiring a client rect match.

2.3 WHEN the primary swapchain's backbuffer resolution changes between frames THEN the ray tracing pipeline SHALL detect the change at the start of the next frame and perform a single, clean resolution transition — re-creating resources exactly once and carrying forward the last valid camera for a grace period to avoid black frames.

2.4 WHEN the game's backbuffer resolution differs from the window client rect THEN the resize transition detection in `EndFrame` and `OnPresent` SHALL compare the current backbuffer extent against the previous backbuffer extent (not the client rect), so a stable low-resolution game is not misinterpreted as continuously resizing.

2.5 WHEN two swapchains are of similar quality and the current primary is still presenting valid frames THEN the hysteresis mechanism SHALL require a stronger and more sustained advantage (e.g., the challenger must be clearly better for more consecutive frames, or the current primary must have stopped presenting) before allowing a primary switch, to prevent flickering from marginal candidates.

2.6 WHEN a game creates swapchains on multiple windows THEN the primary election SHALL factor in whether the window is the foreground window or the window that received the most recent user input, so that auxiliary/debug windows do not steal primary from the main game window.

### Unchanged Behavior (Regression Prevention)

3.1 WHEN a single-swapchain game presents THEN the swapchain SHALL CONTINUE TO be elected primary immediately on the first present call without any hysteresis delay.

3.2 WHEN the user presses Alt+X THEN the forced primary override SHALL CONTINUE TO immediately claim primary for the current swapchain, bypassing all election heuristics and hysteresis.

3.3 WHEN the primary swapchain is destroyed THEN the next swapchain to present SHALL CONTINUE TO be elected primary immediately (no hysteresis for the "no current primary" case).

3.4 WHEN the primary swapchain is correctly elected and stable THEN the Remix UI overlay SHALL CONTINUE TO render on the primary swapchain's present image via `gui.render()`, and only the primary chain SHALL drive RT frame boundaries via `EndFrame` / `OnPresent`.

3.5 WHEN DLSS, NIS, XeSS, or TAAU upscaling is active THEN the downscale/upscale resolution pipeline SHALL CONTINUE TO derive the render resolution from the primary swapchain's backbuffer extent via `targetImage->info().extent` in `onFrameBegin`, and the upscaler SHALL CONTINUE TO produce output at the backbuffer resolution.

3.6 WHEN the primary swapchain presents THEN `incrementPresentCount` SHALL CONTINUE TO be called only for the primary chain, and secondary chains SHALL CONTINUE TO present their own images without bumping `getCurrentFrameId`.

3.7 WHEN the WndProc hook is installed on the primary swapchain's window THEN ImGui input handling, system key blocking, and raw input blocking SHALL CONTINUE TO function correctly for the primary window.

3.8 WHEN the primary swapchain changes to a different DxvkDevice THEN the UI state (menu type) SHALL CONTINUE TO be mirrored from the old primary's device to the new primary's device.
