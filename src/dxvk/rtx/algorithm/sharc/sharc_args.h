#pragma once

#include "rtx/utility/shader_types.h"

// Scalar layout, shared by host push constants and all three cache passes.
struct SharcArgs {
  uint capacity;
  uint updateStride;
  uint accumulationFrames;
  uint staleFrames;
  float sceneScale;
  float minRoughness;
  float worldToMeters;
  uint pad1;
};

#ifdef __cplusplus
static_assert(sizeof(SharcArgs) == 32, "SHARC push constants must match the shader scalar layout");
#endif

#define SHARC_BINDING_HASH_ENTRIES 230
#define SHARC_BINDING_ACCUMULATION 231
#define SHARC_BINDING_RESOLVED 232
#define SHARC_BINDING_LOCKS 233
