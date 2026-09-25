// This translation unit deliberately includes the generated opcode metadata
// before any other SPIR-V header. The spirv target does not use a PCH.
#define SPV_ENABLE_UTILITY_CODE
#include <spirv/spirv.hpp>
#undef SPV_ENABLE_UTILITY_CODE

#include "spirv_nrd_image_format.h"

#include <cstring>
#include <map>
#include <set>
#include <stdexcept>
#include <unordered_map>

namespace dxvk {
  namespace {
    struct NrdSpirvInstruction {
      std::vector<uint32_t> words;
      spv::Op opcode;
      bool hasResult;
      bool hasResultType;
      bool removed = false;
    };

    [[noreturn]] void rejectNrdSpirv(const char* reason) {
      throw std::invalid_argument(reason);
    }
  }

  NrdStorageImageCode normalizeNrdStorageImageFormats(const void* bytecode, size_t bytes) {
    if (!bytecode || bytes < 5 * sizeof(uint32_t) || bytes % sizeof(uint32_t))
      rejectNrdSpirv("NRD SPIR-V has an invalid byte length");

    NrdStorageImageCode result;
    result.code.resize(bytes / sizeof(uint32_t));
    std::memcpy(result.code.data(), bytecode, bytes);
    if (result.code[0] != spv::MagicNumber || !result.code[3] || result.code[4]
        || result.code[1] < 0x00010000 || result.code[1] > 0x00010600)
      rejectNrdSpirv("NRD SPIR-V has an invalid header");

    std::vector<NrdSpirvInstruction> instructions;
    std::unordered_map<uint32_t, uint32_t> valueTypes;
    std::set<uint32_t> definedIds;
    std::set<uint32_t> storageImages;
    std::set<uint32_t> imagePointers;
    std::set<uint32_t> capabilities;
    std::unordered_map<uint32_t, uint32_t> aliases;
    std::map<std::vector<uint32_t>, uint32_t> imageTypes;
    std::map<std::vector<uint32_t>, uint32_t> pointerTypes;

    const auto checkId = [&](uint32_t id) {
      if (!id || id >= result.code[3])
        rejectNrdSpirv("NRD SPIR-V contains an out-of-bound ID");
    };
    const auto canonical = [&](uint32_t id) {
      auto alias = aliases.find(id);
      while (alias != aliases.end()) {
        id = alias->second;
        alias = aliases.find(id);
      }
      return id;
    };

    for (size_t offset = 5; offset < result.code.size();) {
      const uint32_t length = result.code[offset] >> spv::WordCountShift;
      if (!length || length > result.code.size() - offset)
        rejectNrdSpirv("NRD SPIR-V contains a truncated instruction");
      NrdSpirvInstruction instruction;
      instruction.opcode = spv::Op(result.code[offset] & spv::OpCodeMask);
      instruction.words.assign(result.code.begin() + offset, result.code.begin() + offset + length);
      spv::HasResultAndType(instruction.opcode, &instruction.hasResult, &instruction.hasResultType);
      const uint32_t required = 1u + uint32_t(instruction.hasResult) + uint32_t(instruction.hasResultType);
      if (length < required)
        rejectNrdSpirv("NRD SPIR-V instruction is missing its result operands");
      if (instruction.hasResult) {
        const uint32_t id = instruction.words[instruction.hasResultType ? 2 : 1];
        checkId(id);
        if (!definedIds.insert(id).second)
          rejectNrdSpirv("NRD SPIR-V contains a duplicate result ID");
        if (instruction.hasResultType) {
          checkId(instruction.words[1]);
          valueTypes.emplace(id, instruction.words[1]);
        }
      }
      if (instruction.opcode == spv::OpCapability) {
        if (length != 2) rejectNrdSpirv("NRD SPIR-V has an invalid capability");
        capabilities.insert(instruction.words[1]);
      }
      if (instruction.opcode == spv::OpImageTexelPointer)
        rejectNrdSpirv("NRD formatless storage images cannot be used for image atomics");
      if (instruction.opcode == spv::OpImageSparseRead)
        rejectNrdSpirv("NRD sparse storage-image reads are unsupported");
      instructions.emplace_back(std::move(instruction));
      offset += length;
    }

    // Types precede their uses in the shipped NRD modules. Only merge image
    // and pointer declarations, never value IDs or integer/string literals.
    for (auto& instruction : instructions) {
      auto& words = instruction.words;
      if (instruction.opcode == spv::OpTypeImage) {
        if (words.size() < 9 || words.size() > 10)
          rejectNrdSpirv("NRD SPIR-V has an invalid image type");
        checkId(words[2]);
        if (words[7] != 2) continue;
        if (words.size() != 9 || words[3] != spv::Dim2D || words[5] || words[6])
          rejectNrdSpirv("NRD storage images must be non-arrayed, non-MS 2D images");
        storageImages.insert(words[1]);
        words[8] = spv::ImageFormatUnknown;
        std::vector<uint32_t> signature(words.begin() + 2, words.end());
        const auto inserted = imageTypes.emplace(std::move(signature), words[1]);
        if (!inserted.second) {
          aliases.emplace(words[1], inserted.first->second);
          instruction.removed = true;
        }
      } else if (instruction.opcode == spv::OpTypePointer) {
        if (words.size() != 4) rejectNrdSpirv("NRD SPIR-V has an invalid pointer type");
        checkId(words[3]);
        if (!storageImages.count(words[3])) continue;
        if (words[2] != spv::StorageClassUniformConstant)
          rejectNrdSpirv("NRD storage images must use UniformConstant pointers");
        imagePointers.insert(words[1]);
        words[3] = canonical(words[3]);
        std::vector<uint32_t> signature(words.begin() + 2, words.end());
        const auto inserted = pointerTypes.emplace(std::move(signature), words[1]);
        if (!inserted.second) {
          aliases.emplace(words[1], inserted.first->second);
          instruction.removed = true;
        }
      }
    }

    const auto isStorageType = [&](uint32_t id) {
      return storageImages.count(id) || imagePointers.count(id);
    };
    const auto storageOperand = [&](const NrdSpirvInstruction& instruction, size_t index) {
      if (instruction.words.size() <= index)
        rejectNrdSpirv("NRD SPIR-V image operation has no image operand");
      const auto type = valueTypes.find(instruction.words[index]);
      if (type == valueTypes.end() || !storageImages.count(type->second))
        rejectNrdSpirv("NRD SPIR-V image operation has an unsupported image operand");
    };

    for (auto& instruction : instructions) {
      auto& words = instruction.words;
      // Arrays, aggregates and functions containing storage-image types would
      // need further type interning. Reject them rather than partially rewrite
      // a module outside this adapter's direct-resource contract.
      size_t typeBegin = 0, typeEnd = 0;
      switch (instruction.opcode) {
        case spv::OpTypeVector:
        case spv::OpTypeMatrix:
        case spv::OpTypeSampledImage:
        case spv::OpTypeArray:
        case spv::OpTypeRuntimeArray:
          typeBegin = 2; typeEnd = 3; break;
        case spv::OpTypeStruct:
        case spv::OpTypeFunction:
          typeBegin = 2; typeEnd = words.size(); break;
        case spv::OpTypePointer:
          if (words.size() != 4) rejectNrdSpirv("NRD SPIR-V has an invalid pointer type");
          if (imagePointers.count(words[3]))
            rejectNrdSpirv("NRD pointers to storage-image pointers are unsupported");
          break;
        case spv::OpTypeForwardPointer:
          rejectNrdSpirv("NRD forward pointer declarations are unsupported");
        case spv::OpImageRead:
          if (words.size() < 5) rejectNrdSpirv("NRD SPIR-V has a truncated image read");
          storageOperand(instruction, 3);
          result.requiresRead = true;
          break;
        case spv::OpImageWrite:
          if (words.size() < 4) rejectNrdSpirv("NRD SPIR-V has a truncated image write");
          storageOperand(instruction, 1);
          result.requiresWrite = true;
          break;
        default: break;
      }
      if (typeEnd > words.size()) rejectNrdSpirv("NRD SPIR-V has a truncated type declaration");
      for (size_t i = typeBegin; i < typeEnd; ++i) {
        if (isStorageType(words[i]))
          rejectNrdSpirv("NRD aggregate/function storage-image types are unsupported");
      }

      if (instruction.hasResultType)
        words[1] = canonical(words[1]);

      // All remaining references to removed type IDs are debug/annotation
      // targets. Decoration arguments remain unchanged: they may be literals.
      switch (instruction.opcode) {
        case spv::OpName:
          if (words.size() < 3) rejectNrdSpirv("NRD SPIR-V has a truncated name");
          if (aliases.count(words[1])) instruction.removed = true;
          break;
        case spv::OpDecorate:
        case spv::OpDecorateId:
          if (words.size() < 3) rejectNrdSpirv("NRD SPIR-V has a truncated decoration");
          if (isStorageType(words[1]))
            rejectNrdSpirv("NRD decorated storage-image types are unsupported");
          break;
        case spv::OpGroupDecorate:
          if (words.size() < 3) rejectNrdSpirv("NRD SPIR-V has a truncated decoration group");
          for (size_t i = 2; i < words.size(); ++i) {
            if (isStorageType(words[i]))
              rejectNrdSpirv("NRD group-decorated storage-image types are unsupported");
          }
          break;
        default: break;
      }
    }

    result.code.resize(5);
    const auto addCapability = [&](spv::Capability capability, bool required) {
      if (required && !capabilities.count(capability)) {
        result.code.push_back((2u << spv::WordCountShift) | spv::OpCapability);
        result.code.push_back(capability);
      }
    };
    addCapability(spv::CapabilityStorageImageReadWithoutFormat, result.requiresRead);
    addCapability(spv::CapabilityStorageImageWriteWithoutFormat, result.requiresWrite);
    for (const auto& instruction : instructions) {
      if (!instruction.removed)
        result.code.insert(result.code.end(), instruction.words.begin(), instruction.words.end());
    }
    return result;
  }
}
