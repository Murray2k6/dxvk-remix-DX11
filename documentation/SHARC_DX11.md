# SHARC indirect lighting

Select **SHARC** in the advanced indirect illumination mode control, or set
`rtx.integrateIndirectMode = 3`. Existing modes keep their numeric values.
Graphics presets preserve an explicitly selected SHARC mode.

The implementation uses NVIDIA SHARC 1.8.3, pinned to commit
`4e21b585c33c83d723ca9a1e11bbb1090d145793`. Its source, revision and NVIDIA RTX
SDK license are in `src/dxvk/shaders/rtx/algorithm/sharc/sdk`.

The indirect pass performs three GPU stages:

1. **Sparse update:** one pixel per 5×5 tile, with the offset advancing each
   frame, traces the existing complete continuation path. At the first eligible
   rough opaque hit, it restarts radiance/throughput accumulation and submits
   the resulting outgoing radiance to `SharcUpdateHit`. There is no cache
   feedback or screen-radiance output in this stage.
2. **Resolve:** `SharcResolveEntry` processes the hash table, combines samples
   over a 16-frame window, clears per-frame accumulation and evicts entries
   after 32 stale frames.
3. **Query:** full-resolution ray-query integration calls
   `SharcGetCachedRadiance` for opaque hits with roughness at least 0.6 and ray
   segment length greater than a voxel. Successful queries terminate the path;
   cold, glossy and translucent hits continue ordinary integration.

Full continuation samples allow the existing emission, sky, volumetric,
translucency and portal logic to contribute to cache training without
duplicating its per-bounce bookkeeping. Only one surface is trained per sparse
path. The SDK's cache-resampling/backpropagation option is deliberately disabled
for these complete samples; treating them as direct-only samples and then
propagating them again would count indirect lighting twice.

The cache has 2²⁰ entries: 8-byte keys, 16-byte accumulation, 16-byte resolved
radiance and 4-byte hash locks, totaling **44 MiB**. It uses bounded 32-bit lock
operations instead of requiring 64-bit atomics. Positions and camera motion
are converted to meters using the game's configured scene scale. Material
demodulation preserves albedo variation within a voxel. SHARC remains a biased
spatial approximation, particularly around discontinuities and changing lights.

Each entry accepts at most 64 independent samples per frame. A complete sample
is reserved atomically before its RGB components are added; excess reservations
are rolled back. Demodulated radiance is bounded to the representable
accumulator/float16 range, preventing bright lights or concentrated updates from
wrapping the SDK's uint32 sums. Two additional SDK fixes initialize failed query
outputs and return a nonmatching key when the bounded lock cannot be acquired.

The resources are zeroed before first use and reset on history invalidation,
camera cuts, scene-scale changes and gaps in rendered frames. Leaving SHARC
releases its resources. Failed GPU dispatch setup stops dependent cache stages
and invalidates the cache for the next attempt. Cache stages use DXVK's tracked
storage-buffer barriers. SHARC runs in compute ray-query variants, keeping its
update state out of the other modes' ray-tracing payloads.

## ReSTIR GI corrections

Disabling temporal reuse now forwards the initial reservoir before reflection
tracing, reprojection and temporal resampling. Disabling spatial reuse writes
the required GBuffer data and forwards the temporal reservoir before neighbor
sampling and visibility rays. These passes still provide valid output for the
following stage; their disabled work is no longer performed and discarded.

History resets, camera cuts and gaps in ray-traced frames disable temporal reuse
and sample stealing until compatible history exists. Adaptive history handles
zero and non-finite frame durations, and both history modes respect the packed
reservoir's count limits. Reservoir buffers use device-sized byte arithmetic,
declare their storage/transfer accesses and are cleared on creation.

Sample stealing terminates a path only after it accepts a valid prior composite
or a nonempty, positively weighted, visible reservoir. Failed candidates leave
ordinary path integration active. The gradient test reads the current-frame
gradient at the current pixel rather than at the reprojected previous pixel.

These changes remove specific unnecessary work and invalid reuse. They do not
replace RTXDI's light-selection algorithm with the light BVH described in the
reference screenshot. That comparison concerns a particular scene and light
sampling workload; it does not establish the cost of ReSTIR GI, SHARC or these
fixes in another game. Performance claims require GPU timings from comparable
scenes, settings and hardware.

## Validation

`_validation/validate_sharc.py` compiles eight SHARC update/query variants
(NEE cache and WBOIT on/off), resolve, the existing NRC/non-NRC indirect compute
variants and the three ReSTIR GI passes, then runs SPIR-V validation. Results
are written to `_validation/sharc/results.json`.

This verifies shader compilation and SPIR-V validity. GPU timing and visual
quality still require representative game captures on supported hardware;
no game-specific performance gain is assumed.

`tests/d3d11/test_remix_rendering.cpp` provides a bounded real-scene smoke test
for compute ray-query integration, SHARC, ReSTIR GI and TraceRay integration. Each phase
requires 32 consecutive complete frames and finite, nonblack final-color
readback. `_validation/run_remix_rendering.ps1` records the exact runtime DLL
hashes and checks that the Khronos validation layer loaded without API errors
or undefined storage-image format warnings. This test exercises a small diffuse
room; it is not a cross-game image-quality or performance benchmark.

`_validation/run_remix_initialization.ps1` checks overlapping devices, API-owned
references, release and recreation, and two parallel device-creation loops with
exact-color GPU readback. Both smoke runners reject retained common-object
diagnostics as well as recording the actual process exit. The initialization
runner also checks that each created shared-device generation retires.

On 2026-09-24, the final x64 runtime passed these checks on an NVIDIA GeForce
RTX 5060 with Khronos core Vulkan validation enabled. The
[native lifecycle result](../_validation/runtime-init/run-20260924-205707/status.json)
includes partial 2D clears with preserved exterior pixels, 3D attachment slice
preservation, sampled-depth readback, overlapping devices, recreation and
parallel creation; five shared-device generations retired cleanly. The
[four-mode rendering result](../_validation/rendering/run-20260924-205821/status.json)
records 32 consecutive complete frames per mode and finite, nonblack readback
of all 76,800 output pixels. Both processes exited zero, with no validation
errors, storage-image format warnings or retained common objects. The result
files record the identical tested DLL SHA-256 hashes. These are bounded runtime
regressions, not evidence of performance or visual quality across all games.
