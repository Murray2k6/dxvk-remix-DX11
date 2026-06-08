#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
#include <winternl.h>
#include <d3dkmthk.h>

namespace dxvk {
  // Slightly modified definitions...
  struct D3DKMT_CREATEDCFROMMEMORY {
    void*         pMemory;
    D3DDDIFORMAT  Format;
    UINT          Width;
    UINT          Height;
    UINT          Pitch;
    HDC           hDeviceDc;
    PALETTEENTRY* pColorTable;
    HDC           hDc;
    HANDLE        hBitmap;
  };

  struct D3DKMT_DESTROYDCFROMMEMORY {
    HDC    hDC     = nullptr;
    HANDLE hBitmap = nullptr;
  };

  using D3DKMTCreateDCFromMemoryType  = NTSTATUS(STDMETHODCALLTYPE*) (D3DKMT_CREATEDCFROMMEMORY*);
  NTSTATUS D3DKMTCreateDCFromMemory (D3DKMT_CREATEDCFROMMEMORY*  Arg1);

  using D3DKMTDestroyDCFromMemoryType = NTSTATUS(STDMETHODCALLTYPE*) (D3DKMT_DESTROYDCFROMMEMORY*);
  NTSTATUS D3DKMTDestroyDCFromMemory(D3DKMT_DESTROYDCFROMMEMORY* Arg1);

}