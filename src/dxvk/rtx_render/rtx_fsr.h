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
#pragma once

#include "rtx_common_object.h"
#include "rtx_options.h"

namespace dxvk {

  class DxvkDevice;

  class DxvkFSR : public CommonDeviceObject {

  public:

    explicit DxvkFSR(DxvkDevice* device);
    ~DxvkFSR();

    bool supportsFSR4();
    bool hasDx12Provider() const;
    bool hasRayRegenerationProvider() const;
    bool hasRadianceCacheProvider() const;
    bool hasFrameGenerationProvider() const;
    const char* getNotSupportedReason() const;
    void logStatusOnce();

  private:

    void checkSupport();

    bool m_supportChecked = false;
    bool m_loaderAvailable = false;
    bool m_upscalerProviderAvailable = false;
    bool m_rayRegenerationProviderAvailable = false;
    bool m_radianceCacheProviderAvailable = false;
    bool m_frameGenerationProviderAvailable = false;
    bool m_dx12ProviderAvailable = false;
    const char* m_notSupportedReason = "FSR SDK support has not been checked yet.";
    void* m_loaderModule = nullptr;
  };

}
