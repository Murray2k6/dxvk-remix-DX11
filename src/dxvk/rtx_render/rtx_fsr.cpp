/*
* Copyright (c) 2026, NVIDIA CORPORATION. All rights reserved.
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

#include "rtx_fsr.h"

#include "../util/log/log.h"
#include "../util/util_string.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#if defined(WITH_AMD_FFX_SDK)
#if defined(_WIN32) && !defined(_WINDOWS) && !defined(PLATFORM_WINDOWS)
#define PLATFORM_WINDOWS
#endif
#include "ffx_api_loader.h"
#include "ffx_upscale.h"
#endif

namespace dxvk {

  DxvkFSR::DxvkFSR(DxvkDevice* device)
  : CommonDeviceObject(device) {
  }

  DxvkFSR::~DxvkFSR() {
#if defined(_WIN32)
    if (m_loaderModule != nullptr) {
      FreeLibrary(reinterpret_cast<HMODULE>(m_loaderModule));
      m_loaderModule = nullptr;
    }
#endif
  }

  bool DxvkFSR::supportsFSR4() {
    checkSupport();
    return m_loaderAvailable && m_upscalerProviderAvailable && m_dx12ProviderAvailable;
  }

  bool DxvkFSR::hasDx12Provider() const {
    return m_dx12ProviderAvailable;
  }

  bool DxvkFSR::hasRayRegenerationProvider() const {
    return m_rayRegenerationProviderAvailable;
  }

  bool DxvkFSR::hasRadianceCacheProvider() const {
    return m_radianceCacheProviderAvailable;
  }

  bool DxvkFSR::hasFrameGenerationProvider() const {
    return m_frameGenerationProviderAvailable;
  }

  const char* DxvkFSR::getNotSupportedReason() const {
    return m_notSupportedReason;
  }

  void DxvkFSR::logStatusOnce() {
    checkSupport();

    static bool s_logged = false;
    if (s_logged) {
      return;
    }
    s_logged = true;

    if (supportsFSR4()) {
      Logger::info(str::format(
        "AMD FSR SDK: DX12 providers staged: FSR Upscaling 4.1=yes, Ray Regeneration 1.1=",
        hasRayRegenerationProvider() ? "yes" : "no",
        ", Radiance Caching 0.9=",
        hasRadianceCacheProvider() ? "yes" : "no",
        ", Frame Generation 4.0=",
        hasFrameGenerationProvider() ? "yes" : "no",
        ". FSR4 remains selectable; this Vulkan renderer uses its safe temporal compatibility backend until native FSR4 dispatch is implemented."));
    } else {
      Logger::info(str::format("AMD FSR SDK: FSR4 unavailable for native dispatch: ", m_notSupportedReason));
    }
  }

  void DxvkFSR::checkSupport() {
    if (m_supportChecked) {
      return;
    }
    m_supportChecked = true;

#if !defined(WITH_AMD_FFX_SDK)
    m_notSupportedReason = "FidelityFX SDK headers were not present at build time.";
    return;
#elif !defined(_WIN32)
    m_notSupportedReason = "FidelityFX SDK DLL loader integration is only implemented for Windows.";
    return;
#else
    HMODULE loaderModule = LoadLibraryW(L"amd_fidelityfx_loader_dx12.dll");
    if (loaderModule == nullptr) {
      m_notSupportedReason = "amd_fidelityfx_loader_dx12.dll was not found beside the runtime.";
      return;
    }

    ffxFunctions functions = {};
    ffxLoadFunctions(&functions, loaderModule);

    if (functions.CreateContext == nullptr ||
        functions.DestroyContext == nullptr ||
        functions.Configure == nullptr ||
        functions.Query == nullptr ||
        functions.Dispatch == nullptr) {
      FreeLibrary(loaderModule);
      m_notSupportedReason = "amd_fidelityfx_loader_dx12.dll did not expose the required FSR API entry points.";
      return;
    }

    m_loaderModule = loaderModule;
    m_loaderAvailable = true;

    auto probeProvider = [](const wchar_t* dllName) {
      HMODULE providerModule = LoadLibraryW(dllName);
      if (providerModule == nullptr) {
        return false;
      }
      FreeLibrary(providerModule);
      return true;
    };

    m_upscalerProviderAvailable = probeProvider(L"amd_fidelityfx_upscaler_dx12.dll");
    m_rayRegenerationProviderAvailable = probeProvider(L"amd_fidelityfx_denoiser_dx12.dll");
    m_radianceCacheProviderAvailable = probeProvider(L"amd_fidelityfx_radiancecache_dx12.dll");
    m_frameGenerationProviderAvailable = probeProvider(L"amd_fidelityfx_framegeneration_dx12.dll");

    if (!m_upscalerProviderAvailable) {
      m_notSupportedReason = "amd_fidelityfx_upscaler_dx12.dll was not found beside the runtime.";
      return;
    }

    m_dx12ProviderAvailable = true;
    m_notSupportedReason = "FSR Upscaling 4.1 DX12 provider is available.";
#endif
  }

}
