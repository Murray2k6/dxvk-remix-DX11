// This file is part of the FidelityFX SDK.
//
// Copyright (C) 2026 Advanced Micro Devices, Inc.
// 
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files(the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and /or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions :
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.

#pragma once

//------------------------------------------------------------------------------
// FFX Includes
//------------------------------------------------------------------------------

// ffx::InitHelper
// ffx::struct_type
#include "../../api/include/ffx_api.hpp"

//------------------------------------------------------------------------------
// FFX Denoiser Includes
//------------------------------------------------------------------------------

// FFX_API_CREATE_CONTEXT_DESC_TYPE_DENOISER
// FFX_API_CONFIGURE_DESC_TYPE_DENOISER_KEYVALUE
// FFX_API_DISPATCH_DESC_DEBUG_VIEW_TYPE_DENOISER
// FFX_API_DISPATCH_DESC_INPUT_DOMINANT_LIGHT_TYPE_DENOISER
// FFX_API_DISPATCH_DESC_INPUT_4_SIGNALS_TYPE_DENOISER
// FFX_API_DISPATCH_DESC_INPUT_2_SIGNALS_TYPE_DENOISER
// FFX_API_DISPATCH_DESC_INPUT_1_SIGNAL_TYPE_DENOISER
// FFX_API_DISPATCH_DESC_TYPE_DENOISER
// FFX_API_QUERY_DESC_TYPE_DENOISER_GET_DEFAULT_KEYVALUE
// FFX_API_QUERY_DESC_TYPE_DENOISER_GPU_MEMORY_USAGE
// FFX_API_QUERY_DESC_TYPE_DENOISER_GET_VERSION
// ffxCreateContextDescDenoiser
// ffxConfigureDescDenoiserKeyValue
// ffxDispatchDescDenoiser
// ffxDispatchDescDenoiserDebugView
// ffxDispatchDescDenoiserInputDominantLight
// ffxDispatchDescDenoiserInput4Signals
// ffxDispatchDescDenoiserInput2Signals
// ffxDispatchDescDenoiserInput1Signal
// ffxQueryDescDenoiserGetDefaultKeyValue
// ffxQueryDescDenoiserGetGPUMemoryUsage
// ffxQueryDescDenoiserGetVersion
#include "ffx_denoiser.h"

//------------------------------------------------------------------------------
// External Includes
//------------------------------------------------------------------------------

// uint64_t
#include <stdint.h>
// std::integral_constant
#include <type_traits>

//------------------------------------------------------------------------------
// FFX Denoiser Declarations
//------------------------------------------------------------------------------

// Helper types for header initialization. Api definition is in .h file.

namespace ffx
{
    //--------------------------------------------------------------------------
    // FFX Denoiser Descriptions: Create Context
    //--------------------------------------------------------------------------
    
    template<>
    struct struct_type< ffxCreateContextDescDenoiser >
        : std::integral_constant< uint64_t, FFX_API_CREATE_CONTEXT_DESC_TYPE_DENOISER >
    {};

    struct CreateContextDescDenoiser
        : InitHelper< ffxCreateContextDescDenoiser >
    {};

    //--------------------------------------------------------------------------
    // FFX Denoiser Descriptions: Configure
    //--------------------------------------------------------------------------

    template<>
    struct struct_type< ffxConfigureDescDenoiserKeyValue >
        : std::integral_constant< uint64_t, FFX_API_CONFIGURE_DESC_TYPE_DENOISER_KEYVALUE >
    {};

    struct ConfigureDescDenoiserKeyValue
        : InitHelper< ffxConfigureDescDenoiserKeyValue >
    {};
    
    //--------------------------------------------------------------------------
    // FFX Denoiser Descriptions: Dispatch
    //--------------------------------------------------------------------------

    template<>
    struct struct_type< ffxDispatchDescDenoiser >
        : std::integral_constant< uint64_t, FFX_API_DISPATCH_DESC_TYPE_DENOISER >
    {};

    struct DispatchDescDenoiser
        : InitHelper< ffxDispatchDescDenoiser >
    {};

    template<>
    struct struct_type< ffxDispatchDescDenoiserDebugView >
        : std::integral_constant< uint64_t, FFX_API_DISPATCH_DESC_DEBUG_VIEW_TYPE_DENOISER >
    {};

    struct DispatchDescDenoiserDebugView
        : InitHelper< ffxDispatchDescDenoiserDebugView >
    {};

    template<>
    struct struct_type< ffxDispatchDescDenoiserInputDominantLight >
        : std::integral_constant< uint64_t, FFX_API_DISPATCH_DESC_INPUT_DOMINANT_LIGHT_TYPE_DENOISER >
    {};

    struct DispatchDescDenoiserInputDominantLight
        : InitHelper< ffxDispatchDescDenoiserInputDominantLight >
    {};

    template<>
    struct struct_type< ffxDispatchDescDenoiserInput4Signals >
        : std::integral_constant< uint64_t, FFX_API_DISPATCH_DESC_INPUT_4_SIGNALS_TYPE_DENOISER >
    {};

    struct DispatchDescDenoiserInput4Signals
        : InitHelper< ffxDispatchDescDenoiserInput4Signals >
    {};

    template<>
    struct struct_type< ffxDispatchDescDenoiserInput2Signals >
        : std::integral_constant< uint64_t, FFX_API_DISPATCH_DESC_INPUT_2_SIGNALS_TYPE_DENOISER >
    {};

    struct DispatchDescDenoiserInput2Signals
        : InitHelper< ffxDispatchDescDenoiserInput2Signals >
    {};

    template<>
    struct struct_type< ffxDispatchDescDenoiserInput1Signal >
        : std::integral_constant< uint64_t, FFX_API_DISPATCH_DESC_INPUT_1_SIGNAL_TYPE_DENOISER >
    {};

    struct DispatchDescDenoiserInput1Signal
        : InitHelper< ffxDispatchDescDenoiserInput1Signal >
    {};

    //--------------------------------------------------------------------------
    // FFX Denoiser Descriptions: Query
    //--------------------------------------------------------------------------

    template<>
    struct struct_type< ffxQueryDescDenoiserGetDefaultKeyValue >
        : std::integral_constant< uint64_t, FFX_API_QUERY_DESC_TYPE_DENOISER_GET_DEFAULT_KEYVALUE >
    {};

    struct QueryDescDenoiserGetDefaultKeyValue
        : InitHelper< ffxQueryDescDenoiserGetDefaultKeyValue >
    {};

    template<>
    struct struct_type< ffxQueryDescDenoiserGetGPUMemoryUsage >
        : std::integral_constant< uint64_t, FFX_API_QUERY_DESC_TYPE_DENOISER_GPU_MEMORY_USAGE >
    {};

    struct QueryDescDenoiserGetGPUMemoryUsage
        : InitHelper< ffxQueryDescDenoiserGetGPUMemoryUsage >
    {};

    template<>
    struct struct_type< ffxQueryDescDenoiserGetVersion >
        : std::integral_constant< uint64_t, FFX_API_QUERY_DESC_TYPE_DENOISER_GET_VERSION >
    {};

    struct QueryDescDenoiserGetVersion
        : InitHelper< ffxQueryDescDenoiserGetVersion >
    {};

}  // namespace ffx
