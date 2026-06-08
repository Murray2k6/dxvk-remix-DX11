#pragma once

#include "../util/config/config.h"

#include "../dxvk/dxvk_include.h"

#include "dxgi_include.h"

namespace dxvk {

  /**
   * \brief DXGI options
   * 
   * Per-app options that control the
   * behaviour of some DXGI classes.
   */
  struct DxgiOptions {
    DxgiOptions(const Config& config);

    /// Override PCI vendor and device IDs reported to the
    /// application. This may make apps think they are running
    /// on a different GPU than they do and behave differently.
    int32_t customVendorId;
    int32_t customDeviceId;
    std::string customDeviceDesc;
    
    /// Override maximum reported VRAM size. This may be
    /// useful for some 64-bit games which do not support
    /// more than 4 GiB of VRAM.
    VkDeviceSize maxDeviceMemory;
    VkDeviceSize maxSharedMemory;

    /// Emulate UMA
    bool emulateUMA;

    /// Enables the legacy NvAPI vendor spoof workaround.
    /// Disabled by default in this DX11 Remix fork so adapters
    /// report their real vendor unless explicitly overridden.
    bool nvapiHack;
  };
  
}
