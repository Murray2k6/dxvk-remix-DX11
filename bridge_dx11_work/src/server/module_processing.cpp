#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

extern "C" __declspec(dllexport) int NvRemixBridgeDx11ModuleProcessingAnchor() {
  return 0;
}
