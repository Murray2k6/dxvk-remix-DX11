#pragma once

// rtx_fork_tonemap.h — Tonemap operator enum and per-operator parameter
// classes. Adds Hable Filmic, AgX, and Lottes 2016 operators alongside
// the existing ACES options. Each operator has its own RtxOption class
// for global and local tonemapping paths.

#include <cstdint>
#include "rtx_option.h"

namespace dxvk {

  // Tonemapping operator applied after the dynamic tone curve.
  // Shader-side constants live in shaders/rtx/pass/tonemap/tonemapping.h
  // as tonemapOperator* uints; these MUST stay in lockstep.
  enum class TonemapOperator : uint32_t {
    None        = 0, // Dynamic curve only; no additional operator.
    ACES        = 1,
    ACESLegacy  = 2,
    HableFilmic = 3,
    AgX         = 4,
    Lottes      = 5,
  };

  // Global-tonemapper operator selection. Defaults to None to preserve the
  // upstream pre-refactor `finalizeWithACES = false` behavior.
  class RtxForkGlobalTonemap {
    RTX_OPTION_ENV("rtx.tonemap", TonemapOperator, tonemapOperator, TonemapOperator::None, "DXVK_TONEMAP_OPERATOR",
                   "Tonemapping operator applied after the dynamic tone curve.\n"
                   "Supported values: 0 = None (dynamic curve only), 1 = ACES, 2 = ACES (Legacy), "
                   "3 = Hable Filmic, 4 = AgX, 5 = Lottes 2016.");
  };

  // Local-tonemapper operator selection. Defaults to ACESLegacy because the
  // pre-refactor local tonemapper had `finalizeWithACES = true` and
  // `useLegacyACES = true` by default.
  class RtxForkLocalTonemap {
    RTX_OPTION("rtx.localtonemap", TonemapOperator, tonemapOperator, TonemapOperator::ACESLegacy,
               "Tonemapping operator applied at the local tonemapper's final combine stage.\n"
               "Defaults to ACES (Legacy) to preserve the pre-refactor behavior.\n"
               "Supported values: 0 = None, 1 = ACES, 2 = ACES (Legacy), 3 = Hable Filmic, 4 = AgX, 5 = Lottes 2016.");
  };

  // Hable Filmic (Uncharted 2) operator parameters. Shared between global
  // and local tonemap paths. Defaults use Half-Life: Alyx values (W=4.0).
  class RtxForkHableFilmic {
    RTX_OPTION("rtx.tonemap.hable", float, exposureBias,     2.00f, "Hable Filmic: pre-operator exposure multiplier.");
    RTX_OPTION("rtx.tonemap.hable", float, shoulderStrength, 0.15f, "Hable Filmic: A — shoulder strength.");
    RTX_OPTION("rtx.tonemap.hable", float, linearStrength,   0.50f, "Hable Filmic: B — linear strength.");
    RTX_OPTION("rtx.tonemap.hable", float, linearAngle,      0.10f, "Hable Filmic: C — linear angle.");
    RTX_OPTION("rtx.tonemap.hable", float, toeStrength,      0.20f, "Hable Filmic: D — toe strength.");
    RTX_OPTION("rtx.tonemap.hable", float, toeNumerator,     0.02f, "Hable Filmic: E — toe numerator.");
    RTX_OPTION("rtx.tonemap.hable", float, toeDenominator,   0.30f, "Hable Filmic: F — toe denominator.");
    RTX_OPTION("rtx.tonemap.hable", float, whitePoint,       4.00f, "Hable Filmic: W — linear-scene white point.");
  };

  // AgX operator parameters (global path). Defaults from gmod reference.
  class RtxForkAgX {
    RTX_OPTION("rtx.tonemap.agx", float, gamma,          2.0f, "AgX gamma adjustment. Range [0.5, 3.0].");
    RTX_OPTION("rtx.tonemap.agx", float, saturation,     1.1f, "AgX saturation multiplier. Range [0.5, 2.0].");
    RTX_OPTION("rtx.tonemap.agx", float, exposureOffset, 0.0f, "AgX exposure offset in EV stops. Range [-2.0, 2.0].");
    RTX_OPTION("rtx.tonemap.agx", int,   look,           0,    "AgX look preset: 0 = None, 1 = Punchy, 2 = Golden, 3 = Greyscale.");
    RTX_OPTION("rtx.tonemap.agx", float, contrast,       1.0f, "AgX contrast adjustment. Range [0.5, 2.0].");
    RTX_OPTION("rtx.tonemap.agx", float, slope,          1.0f, "AgX slope adjustment. Range [0.5, 2.0].");
    RTX_OPTION("rtx.tonemap.agx", float, power,          1.0f, "AgX power adjustment. Range [0.5, 2.0].");
  };

  // Lottes 2016 operator parameters (global path).
  class RtxForkLottes {
    RTX_OPTION("rtx.tonemap.lottes", float, hdrMax,   16.0f, "Lottes: peak HDR white value. Range [1.0, 64.0].");
    RTX_OPTION("rtx.tonemap.lottes", float, contrast,  2.0f, "Lottes: contrast control. Range [1.0, 3.0].");
    RTX_OPTION("rtx.tonemap.lottes", float, shoulder,  1.0f, "Lottes: shoulder strength. Range [0.5, 2.0].");
    RTX_OPTION("rtx.tonemap.lottes", float, midIn,    0.18f, "Lottes: mid-grey input. Range [0.01, 1.0].");
    RTX_OPTION("rtx.tonemap.lottes", float, midOut,   0.18f, "Lottes: mid-grey output. Range [0.01, 1.0].");
  };

  // Local-tonemapper AgX operator parameters. Separate from global because
  // the local tonemapper operates on a different dynamic range.
  class RtxForkLocalAgX {
    RTX_OPTION("rtx.localtonemap.agx", float, gamma,          0.45f, "AgX gamma (local path). Range [0.0, 3.0].");
    RTX_OPTION("rtx.localtonemap.agx", float, saturation,     1.0f,  "AgX saturation (local path). Range [0.0, 2.0].");
    RTX_OPTION("rtx.localtonemap.agx", float, exposureOffset, 0.0f,  "AgX exposure offset (local path). Range [-2.0, 2.0].");
    RTX_OPTION("rtx.localtonemap.agx", int,   look,           0,     "AgX look preset (local path).");
    RTX_OPTION("rtx.localtonemap.agx", float, contrast,       0.8f,  "AgX contrast (local path). Range [0.0, 2.0].");
    RTX_OPTION("rtx.localtonemap.agx", float, slope,          1.2f,  "AgX slope (local path). Range [0.0, 2.0].");
    RTX_OPTION("rtx.localtonemap.agx", float, power,          1.0f,  "AgX power (local path). Range [0.0, 2.0].");
  };

  // Local-tonemapper Lottes operator parameters.
  class RtxForkLocalLottes {
    RTX_OPTION("rtx.localtonemap.lottes", float, hdrMax,   16.0f, "Lottes: peak HDR white (local path). Range [1.0, 64.0].");
    RTX_OPTION("rtx.localtonemap.lottes", float, contrast,  2.0f, "Lottes: contrast (local path). Range [1.0, 3.0].");
    RTX_OPTION("rtx.localtonemap.lottes", float, shoulder,  1.0f, "Lottes: shoulder (local path). Range [0.5, 2.0].");
    RTX_OPTION("rtx.localtonemap.lottes", float, midIn,    0.18f, "Lottes: mid-grey input (local path). Range [0.01, 1.0].");
    RTX_OPTION("rtx.localtonemap.lottes", float, midOut,   0.18f, "Lottes: mid-grey output (local path). Range [0.01, 1.0].");
  };

} // namespace dxvk

// Forward declarations for tonemap hook functions used by the tonemapping dispatch code.
// Include the shader args headers so the struct types are visible.
#include "rtx/pass/tonemap/tonemapping.h"
#include "rtx/pass/local_tonemap/local_tonemapping.h"

namespace dxvk {
  namespace fork_tonemap {
    void populateTonemapOperatorArgs(ToneMappingApplyToneMappingArgs& args);
    void populateLocalTonemapOperatorArgs(FinalCombineArgs& args);
    void showTonemapOperatorUI();
    void showLocalTonemapOperatorUI();
    bool shouldSkipToneCurve();
  } // namespace fork_tonemap
} // namespace dxvk
