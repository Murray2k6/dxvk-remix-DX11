#pragma once

// DX11_V225: the DXSO constant-buffer binding enums (DxsoConstantBuffers) and
// binding-slot helpers live in dxso_util.h. This header exists so the
// "#include ../d3d11/d3d11_constant_set.h" reference resolves and forwards to the
// canonical definitions. DX11 has no D3D11-style constant-set state; the binding
// indices are only used to lay out the DXSO compiler's Vulkan descriptor slots.
#include "../dxso/dxso_util.h"
