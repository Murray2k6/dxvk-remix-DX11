#include "rtx/pass/common_bindings.slangh"
#include "rtx/algorithm/sharc/sharc.slangh"

[shader("compute")]
[numthreads(256, 1, 1)]
void main(uint index : SV_DispatchThreadID) {
  if (index >= sharcArgs.capacity)
    return;
  SharcResolveParameters parameters;
  parameters.cameraPositionPrev = cameraGetPreviousWorldPosition(cb.camera) * sharcArgs.worldToMeters;
  parameters.accumulationFrameNum = sharcArgs.accumulationFrames;
  parameters.responsiveFrameNum = 1;
  parameters.staleFrameNumMax = sharcArgs.staleFrames;
  parameters.frameIndex = cb.frameIdx;
  SharcResolveEntry(index, makeSharcParameters(), parameters);
}
