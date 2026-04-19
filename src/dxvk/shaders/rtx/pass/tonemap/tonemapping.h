/*
* Copyright (c) 2022, NVIDIA CORPORATION. All rights reserved.
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
#ifndef TONEMAPPING_H
#define TONEMAPPING_H

#include "rtx/utility/shader_types.h"

#define AUTO_EXPOSURE_HISTOGRAM_INPUT_OUTPUT              0
#define AUTO_EXPOSURE_EXPOSURE_INPUT_OUTPUT               1
#define AUTO_EXPOSURE_COLOR_INPUT                         2
#define AUTO_EXPOSURE_DEBUG_VIEW_OUTPUT                   3
#define AUTO_EXPOSURE_EC_INPUT                            4

#define TONEMAPPING_HISTOGRAM_COLOR_INPUT                 0
#define TONEMAPPING_HISTOGRAM_HISTOGRAM_INPUT_OUTPUT      1
#define TONEMAPPING_HISTOGRAM_EXPOSURE_INPUT              2

#define TONEMAPPING_TONE_CURVE_HISTOGRAM_INPUT_OUTPUT     0
#define TONEMAPPING_TONE_CURVE_TONE_CURVE_INPUT_OUTPUT    1

#define TONEMAPPING_APPLY_BLUE_NOISE_TEXTURE_INPUT         0
#define TONEMAPPING_APPLY_TONEMAPPING_COLOR_INPUT          1
#define TONEMAPPING_APPLY_TONEMAPPING_TONE_CURVE_INPUT     2
#define TONEMAPPING_APPLY_TONEMAPPING_EXPOSURE_INPUT       3
#define TONEMAPPING_APPLY_TONEMAPPING_COLOR_OUTPUT         4

#define TONEMAPPING_TONE_CURVE_SAMPLE_COUNT               256

#define EXPOSURE_HISTOGRAM_SIZE                           256

// Constants

static const uint32_t ditherModeNone = 0;
static const uint32_t ditherModeSpatialOnly = 1;
static const uint32_t ditherModeSpatialTemporal = 2;

// Constant buffers

struct ToneMappingAutoExposureArgs {
  uint numPixels;
  float autoExposureSpeed;
  float evMinValue;
  float evRange;

  uint debugMode;
  uint enableCenterMetering;
  float centerMeteringSize;
  uint averageMode; // 0 = Mean, 1 = Median

  uint useExposureCompensation;
  uint pad0;
  uint pad1;
  uint pad2;
};

struct ToneMappingHistogramArgs {
  float toneCurveMinStops;
  float toneCurveMaxStops;
  uint enableAutoExposure;
  float exposureFactor;
};

struct ToneMappingCurveArgs {
  // Range [0, inf). Without further adjustments, the tone curve will try to fit the entire luminance of the scene into the range
  // [-dynamicRange, 0] in linear photographic stops. Higher values adjust for ambient monitor lighting; perfect conditions -> 17.587 stops.
  float dynamicRange;
  float shadowMinSlope;       // Range [0, inf). Forces the tone curve below a linear value of 0.18 to have at least this slope, making the tone darker.
  float shadowContrast;       // Range [0, inf). Additional gamma power to apply to the tone of the tone curve below shadowContrastEnd
  float shadowContrastEnd;    // Range (-inf, 0]. High endpoint for the shadow contrast effect in linear stops; values above this are unaffected

  float maxExposureIncrease;  // Range [0, inf). Forces the tone curve to not increase luminance values at any point more than this value
  float curveShift;           // Range [0, inf). Amount by which to shift the tone curve up or down. Nonzero values will cause additional clipping!
  uint needsReset;            // Invalidates tone curve history
  float toneCurveMinStops;
  
  float toneCurveMaxStops;
  uint pad0;
  uint pad1;
  uint pad2;
};

// Tonemap operator constants — must match TonemapOperator enum in rtx_fork_tonemap.h.
static const uint32_t tonemapOperatorNone        = 0;
static const uint32_t tonemapOperatorACES        = 1;
static const uint32_t tonemapOperatorACESLegacy  = 2;
static const uint32_t tonemapOperatorHableFilmic = 3;
static const uint32_t tonemapOperatorAgX         = 4;
static const uint32_t tonemapOperatorLottes      = 5;

// Operator parameter slots — shared between mutually exclusive operators.
// Hable Filmic uses all 8 slots (A-F + exposureBias + whitePoint).
// AgX maps: slot0=gamma, slot1=saturation, slot2=exposureOffset, slot3=look(as float),
//           slot4=contrast, slot5=slope, slot6=power, slot7=unused.
// Lottes maps: slot0=hdrMax, slot1=contrast, slot2=shoulder, slot3=midIn, slot4=midOut,
//              slot5-7=unused.

struct ToneMappingApplyToneMappingArgs {
  uint toneMappingEnabled;
  uint debugMode;
  uint performSRGBConversion;
  uint enableAutoExposure;

  float shadowContrast;
  float shadowContrastEnd;
  float exposureFactor;
  float contrast;

  vec3 colorBalance;
  uint colorGradingEnabled;

  float saturation;
  float toneCurveMinStops;
  float toneCurveMaxStops;
  uint finalizeWithACES;

  uint ditherMode;
  uint frameIndex;
  uint useLegacyACES;
  uint tonemapOperator;

  uint directOperatorMode;
  float operatorSlot0;        // Hable: exposureBias | AgX: gamma      | Lottes: hdrMax
  float operatorSlot1;        // Hable: A            | AgX: saturation | Lottes: contrast
  float operatorSlot2;        // Hable: B            | AgX: expOffset  | Lottes: shoulder

  float operatorSlot3;        // Hable: C            | AgX: look(float)| Lottes: midIn
  float operatorSlot4;        // Hable: D            | AgX: contrast   | Lottes: midOut
  float operatorSlot5;        // Hable: E            | AgX: slope      | unused
  float operatorSlot6;        // Hable: F            | AgX: power      | unused
  float operatorSlot7;        // Hable: W (white pt) | unused          | unused
  uint  pad1;
};


#endif  // TONEMAPPING_H