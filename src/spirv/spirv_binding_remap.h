#pragma once

#include <cstddef>
#include <unordered_set>
#include <vector>

#include "spirv_instruction.h"

namespace dxvk {

  // DXVK resource slots describe descriptor set zero. Other sets have their
  // own fixed layouts (including Remix's bindless tables) and must retain
  // their original Binding values. Decorations may appear in either order.
  inline std::vector<size_t> spirvResourceSlotRemapOffsets(uint32_t* code, uint32_t dwords) {
    std::unordered_set<uint32_t> externalSetIds;
    const SpirvInstructionIterator end;
    for (SpirvInstructionIterator it(code, 0, dwords); it != end; ++it) {
      const auto ins = *it;
      if (ins.opCode() == spv::OpDecorate
       && ins.arg(2) == spv::DecorationDescriptorSet && ins.arg(3) != 0)
        externalSetIds.insert(ins.arg(1));
    }

    std::vector<size_t> offsets;
    for (SpirvInstructionIterator it(code, 0, dwords); it != end; ++it) {
      const auto ins = *it;
      if (ins.opCode() != spv::OpDecorate)
        continue;
      if (ins.arg(2) == spv::DecorationSpecId
       || (ins.arg(2) == spv::DecorationBinding && externalSetIds.count(ins.arg(1)) == 0))
        offsets.push_back(ins.offset() + 3);
    }
    return offsets;
  }
}
