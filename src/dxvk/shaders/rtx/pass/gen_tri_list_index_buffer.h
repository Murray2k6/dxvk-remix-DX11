/*
* Copyright (c) 2021-2023, NVIDIA CORPORATION. All rights reserved.
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

// Reads a single index from the source buffer, handling both 16-bit
// (packed two-per-uint32_t) and native 32-bit storage.
#ifdef __cplusplus
static inline uint32_t readSrcIndex(const uint32_t* src, uint32_t pos, uint32_t isU16)
#else
uint32_t readSrcIndex(StructuredBuffer<uint32_t> src, uint32_t pos, uint32_t isU16)
#endif
{
  if (isU16 != 0) {
    uint32_t packed = src[pos >> 1];
    return (pos & 1) ? (packed >> 16) : (packed & 0xFFFF);
  }
  return src[pos];
}

// Converts arbitrary topology to triangle list indices.
// Uses uint32_t for all index I/O — no vertex count limitation on modern APIs.
#ifdef __cplusplus
void generateIndices(const uint32_t idx, uint32_t* dst, const uint32_t* src, const GenTriListArgs& cb)
#else
void generateIndices(const uint32_t idx, RWStructuredBuffer<uint32_t> dst, StructuredBuffer<uint32_t> src, GenTriListArgs cb)
#endif
{
  uint32_t i0 = 0;
  uint32_t i1 = 0;
  uint32_t i2 = 0;
    
  switch (cb.topology)
  {
  case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN:
      i0 = 0;
      i1 = idx + 1;
      i2 = idx + 2;
      break;
  case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP:
      i0 = idx + 0;
      i1 = idx + 1 + (idx & 1);
      i2 = idx + 2 - (idx & 1);
      break;
  case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST:
      i0 = idx * 3 + 0;
      i1 = idx * 3 + 1;
      i2 = idx * 3 + 2;
      break;
  }
  
  i0 += cb.firstIndex;
  i1 += cb.firstIndex;
  i2 += cb.firstIndex;

  if (cb.useIndexBuffer != 0) 
  {
    uint32_t idx0 = readSrcIndex(src, i0, cb.inputIsU16);
    uint32_t idx1 = readSrcIndex(src, i1, cb.inputIsU16);
    uint32_t idx2 = readSrcIndex(src, i2, cb.inputIsU16);

    // Degenerate or out-of-bounds — collapse to a zero-area triangle.
    if (idx0 == idx1 || idx0 == idx2 || idx1 == idx2 ||
        idx0 > cb.maxVertex || idx1 > cb.maxVertex || idx2 > cb.maxVertex)
    {
      idx0 = cb.minVertex;
      idx1 = cb.minVertex;
      idx2 = cb.minVertex;
    }
    
    dst[idx * 3 + 0] = idx0 - cb.minVertex;
    dst[idx * 3 + 1] = idx1 - cb.minVertex;
    dst[idx * 3 + 2] = idx2 - cb.minVertex;
  } 
  else 
  {
    dst[idx * 3 + 0] = i0 - cb.minVertex;
    dst[idx * 3 + 1] = i1 - cb.minVertex;
    dst[idx * 3 + 2] = i2 - cb.minVertex;
  }
}