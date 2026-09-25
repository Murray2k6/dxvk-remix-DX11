#include "../../../src/spirv/spirv_binding_remap.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <unordered_map>

static void check(bool passed, const char* description) {
  if (!passed) {
    std::cerr << description << '\n';
    std::exit(1);
  }
}

int main(int argc, char** argv) {
  constexpr uint32_t decorate = (4u << 16) | spv::OpDecorate;
  const std::array<std::array<uint32_t, 4>, 8> decorations = {{
    { decorate, 10, spv::DecorationBinding, 0 },
    { decorate, 10, spv::DecorationDescriptorSet, 0 },
    { decorate, 11, spv::DecorationBinding, 0 },
    { decorate, 11, spv::DecorationDescriptorSet, 1 },
    { decorate, 12, spv::DecorationBinding, 0 },
    { decorate, 12, spv::DecorationDescriptorSet, 3 },
    { decorate, 13, spv::DecorationSpecId, 7 },
    { decorate, 14, spv::DecorationBinding, 9 }, // Implicit/default set zero.
  }};
  std::array<unsigned, decorations.size()> order = { 0,1,2,3,4,5,6,7 };
  unsigned permutations = 0;
  do {
    std::vector<uint32_t> code = { spv::MagicNumber, 0x00010600, 0, 15, 0 };
    for (unsigned index : order)
      code.insert(code.end(), decorations[index].begin(), decorations[index].end());
    const auto offsets = dxvk::spirvResourceSlotRemapOffsets(code.data(), static_cast<uint32_t>(code.size()));
    check(offsets.size() == 3, "Must select only set-zero bindings and specialization IDs");
    for (size_t offset : offsets) code[offset] += 4;
    for (dxvk::SpirvInstructionIterator it(code.data(), 0, static_cast<uint32_t>(code.size())), end; it != end; ++it) {
      const auto ins = *it;
      if (ins.arg(2) == spv::DecorationBinding) {
        const uint32_t expected = ins.arg(1) == 10 ? 4 : ins.arg(1) == 14 ? 13 : 0;
        check(ins.arg(3) == expected, "Nonzero descriptor sets must retain binding zero");
      }
      if (ins.arg(2) == spv::DecorationSpecId)
        check(ins.arg(3) == 11, "Specialization IDs must preserve DXVK slot remapping");
    }
    ++permutations;
  } while (std::next_permutation(order.begin(), order.end()));

  for (int argument = 1; argument < argc; ++argument) {
    std::ifstream input(argv[argument], std::ios::binary | std::ios::ate);
    check(input.good(), "Cannot read compiled SPIR-V regression input");
    const auto bytes = input.tellg();
    check(bytes > 0 && bytes % 4 == 0, "Invalid SPIR-V input size");
    std::vector<uint32_t> code(static_cast<size_t>(bytes) / 4);
    input.seekg(0);
    input.read(reinterpret_cast<char*>(code.data()), bytes);
    check(input.good(), "Incomplete SPIR-V input read");
    const auto original = code;
    const auto offsets = dxvk::spirvResourceSlotRemapOffsets(code.data(), static_cast<uint32_t>(code.size()));
    for (size_t offset : offsets) code[offset] += 4;
    std::unordered_map<uint32_t, uint32_t> sets;
    for (dxvk::SpirvInstructionIterator it(code.data(), 0, static_cast<uint32_t>(code.size())), end; it != end; ++it) {
      const auto ins = *it;
      if (ins.opCode() == spv::OpDecorate && ins.arg(2) == spv::DecorationDescriptorSet)
        sets[ins.arg(1)] = ins.arg(3);
    }
    unsigned externalBindings = 0;
    for (dxvk::SpirvInstructionIterator it(code.data(), 0, static_cast<uint32_t>(code.size())), end; it != end; ++it) {
      const auto ins = *it;
      if (ins.opCode() == spv::OpDecorate && ins.arg(2) == spv::DecorationBinding && sets[ins.arg(1)] != 0) {
        check(ins.arg(3) == original[ins.offset() + 3], "Compiled shader bindless binding changed");
        ++externalBindings;
      }
    }
    check(externalBindings > 0, "Regression input must contain real bindless descriptors");
    std::cout << argv[argument] << ": " << externalBindings << " fixed external bindings preserved\n";
  }
  std::cout << "Binding remap: " << permutations << " decoration-order permutations passed\n";
}
