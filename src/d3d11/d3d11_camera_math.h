#pragma once

#include <cstddef>
#include <cstdint>

#include "../util/util_matrix.h"

namespace dxvk {

  // Defined in d3d11_rtx.cpp, which uses the same functions for D3D11.

  // 0 = not a perspective projection, 1 = row-major, 2 = column-major
  // read as row-major (transpose it).
  int D3D11ClassifyPerspective(const Matrix4& m);

  // Splits a perspective view-projection into P * V with V rigid.
  bool D3D11FactorViewProjection(const Matrix4& viewProjection, Matrix4& projection, Matrix4& view);

  // Positive X/Y scale, reporting which axes were flipped.
  Matrix4 D3D11CanonicalizeProjection(const Matrix4& projection, bool* flippedX, bool* flippedY);

  // 16 floats at ptr + offset as four rows, bounds-checked against size.
  Matrix4 D3D11ReadMatrix(const uint8_t* ptr, size_t offset, size_t size);

}
