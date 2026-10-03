#include <cctype>
#include <cstring>
#include <algorithm>
#include <filesystem>
#include <fstream>

#include "d3d11_device.h"
#include "d3d11_shader.h"

namespace dxvk {

  // DX11_V298_SILENT_PREWARMER: shader-cache writes emit no log output unless
  // DXVK_REMIX_PREWARM_LOG=1 (matches the prewarm logging gate elsewhere).
  static bool shaderCacheLoggingEnabled() {
    static const bool enabled =
      env::getEnvVar("DXVK_REMIX_PREWARM_LOG") == "1";
    return enabled;
  }

  static void persistGameShaderBytecode(
      const DxvkShaderKey& shaderKey,
      const void* shaderBytecode,
      size_t bytecodeLength) {
    if (env::getEnvVar("DXVK_GAME_SHADER_CACHE") == "0"
     || shaderBytecode == nullptr
     || bytecodeLength == 0u
     || bytecodeLength > (8u << 20))
      return;

    static dxvk::mutex cacheMutex;
    std::lock_guard<dxvk::mutex> lock(cacheMutex);

    const std::filesystem::path cacheDirectory =
      std::filesystem::path(env::getExePath()).parent_path()
      / "rtx-remix" / "cache" / "d3d11-shaders";
    std::error_code error;
    std::filesystem::create_directories(cacheDirectory, error);
    if (error) {
      if (shaderCacheLoggingEnabled()) {
        Logger::warn(str::format(
          "[Remix-DX11][game-shader-cache] could not create '",
          cacheDirectory.string(), "': ", error.message()));
      }
      return;
    }

    const std::filesystem::path target =
      cacheDirectory / (shaderKey.toString() + ".dxbc");
    const uintmax_t existingSize = std::filesystem::file_size(target, error);
    if (!error && existingSize == bytecodeLength) {
      std::vector<char> existing(bytecodeLength);
      std::ifstream input(target, std::ios::in | std::ios::binary);
      input.read(existing.data(), static_cast<std::streamsize>(existing.size()));
      if (input
       && static_cast<size_t>(input.gcount()) == existing.size()
       && std::memcmp(existing.data(), shaderBytecode, bytecodeLength) == 0)
        return;
    }
    error.clear();

    std::filesystem::path temporary = target;
    temporary += str::format(".tmp.", GetCurrentProcessId());
    {
      std::ofstream output(
        temporary, std::ios::out | std::ios::binary | std::ios::trunc);
      output.write(
        reinterpret_cast<const char*>(shaderBytecode),
        static_cast<std::streamsize>(bytecodeLength));
      output.flush();
      if (!output) {
        output.close();
        std::filesystem::remove(temporary, error);
        if (shaderCacheLoggingEnabled()) {
          Logger::warn(str::format(
            "[Remix-DX11][game-shader-cache] failed to write '",
            target.string(), "'."));
        }
        return;
      }
    }

    std::filesystem::remove(target, error);
    error.clear();
    std::filesystem::rename(temporary, target, error);
    if (error) {
      std::error_code cleanupError;
      std::filesystem::remove(temporary, cleanupError);
      if (shaderCacheLoggingEnabled()) {
        Logger::warn(str::format(
          "[Remix-DX11][game-shader-cache] failed to publish '",
          target.string(), "': ", error.message()));
      }
    }
  }

  // DX11_V291_EXECUTABLE_SHADER_PROFILES: compatibility behavior is selected
  // from an executable-scoped database keyed by the exact VS SHA-1. Signature
  // analysis is only the discovery mechanism for a previously unseen shader;
  // once discovered, its decision is persisted and every later run/draw uses
  // the profile entry. A user can change any entry to "disabled" without
  // recompiling, and unrelated games never inherit each other's decisions.
  //
  // File format (append-only; the last duplicate entry wins):
  //   VS_<sha1>|world|POSITION|1|auto
  //   VS_<sha1>|view|VIEWPOSITION|0|manual
  //   VS_<sha1>|disabled|||manual
  struct D3D11PositionProfileRule {
    bool enabled = false;
    std::string semanticName;
    uint32_t semanticIndex = 0;
    D3D11CapturedPositionSpace positionSpace = D3D11CapturedPositionSpace::View;
  };

  class D3D11ExecutableShaderProfile {
  public:
    static D3D11ExecutableShaderProfile& instance() {
      static D3D11ExecutableShaderProfile profile;
      return profile;
    }

    bool resolve(
      const std::string& shaderKey,
      const std::string& discoveredSemantic,
      uint32_t discoveredIndex,
      D3D11CapturedPositionSpace discoveredSpace,
      D3D11PositionProfileRule& result,
      bool& loadedFromProfile) {
      std::lock_guard<dxvk::mutex> lock(m_mutex);
      ensureLoaded();

      const auto existing = m_positionRules.find(shaderKey);
      if (existing != m_positionRules.end()) {
        result = existing->second;
        loadedFromProfile = true;
        return result.enabled;
      }

      loadedFromProfile = false;
      if (discoveredSemantic.empty())
        return false;

      result.enabled = true;
      result.semanticName = discoveredSemantic;
      result.semanticIndex = discoveredIndex;
      result.positionSpace = discoveredSpace;
      m_positionRules.insert({ shaderKey, result });
      appendAutoRule(shaderKey, result);
      return true;
    }

  private:
    static std::string trim(std::string value) {
      const auto notSpace = [](unsigned char c) { return !std::isspace(c); };
      value.erase(value.begin(), std::find_if(value.begin(), value.end(), notSpace));
      value.erase(std::find_if(value.rbegin(), value.rend(), notSpace).base(), value.end());
      return value;
    }

    static std::vector<std::string> splitProfileLine(const std::string& line) {
      std::vector<std::string> fields;
      size_t begin = 0;
      while (begin <= line.size()) {
        const size_t end = line.find('|', begin);
        fields.push_back(trim(line.substr(begin,
          end == std::string::npos ? std::string::npos : end - begin)));
        if (end == std::string::npos)
          break;
        begin = end + 1;
      }
      return fields;
    }

    void ensureLoaded() {
      if (m_loaded)
        return;
      m_loaded = true;

      std::string exeName = env::getExeNameNoSuffix();
      if (exeName.empty())
        exeName = "unknown";
      for (char& c : exeName) {
        const unsigned char uc = static_cast<unsigned char>(c);
        if (!std::isalnum(uc) && c != '-' && c != '_' && c != '.')
          c = '_';
      }

      std::error_code ec;
      const std::filesystem::path directory =
        std::filesystem::path("rtx-remix") / "dx11-profiles";
      std::filesystem::create_directories(directory, ec);
      m_path = directory / (exeName + ".profile");

      std::ifstream input(m_path, std::ios::in);
      std::string line;
      uint32_t loadedRules = 0;
      std::unordered_map<std::string, bool> migrateAutoViewToWorld;
      while (std::getline(input, line)) {
        line = trim(std::move(line));
        if (line.empty() || line[0] == '#')
          continue;
        const std::vector<std::string> fields = splitProfileLine(line);
        if (fields.size() < 2 || fields[0].rfind("VS_", 0) != 0)
          continue;

        D3D11PositionProfileRule rule;
        std::string mode = fields[1];
        std::transform(mode.begin(), mode.end(), mode.begin(),
          [](unsigned char c) { return char(std::tolower(c)); });
        if (mode == "disabled") {
          rule.enabled = false;
          migrateAutoViewToWorld[fields[0]] = false;
        } else if ((mode == "view" || mode == "world")
                && fields.size() >= 4 && !fields[2].empty()) {
          try {
            const unsigned long parsedIndex = std::stoul(fields[3]);
            if (parsedIndex > UINT32_MAX)
              continue;
            rule.enabled = true;
            rule.semanticName = fields[2];
            rule.semanticIndex = static_cast<uint32_t>(parsedIndex);
            rule.positionSpace = mode == "world"
              ? D3D11CapturedPositionSpace::World
              : D3D11CapturedPositionSpace::View;

            // Version-1 auto discovery classified plain POSITION1 as view
            // space. In Skyrim/Bethesda-style deferred shaders it is a world
            // position that is consumed by a later view-projection multiply.
            // Upgrade only generated rules; a manual view rule always wins.
            std::string semanticUpper = rule.semanticName;
            std::transform(semanticUpper.begin(), semanticUpper.end(), semanticUpper.begin(),
              [](unsigned char c) { return char(std::toupper(c)); });
            const bool generatedRule = fields.size() >= 5
              && fields[4].rfind("auto", 0) == 0;
            const bool migrate = mode == "view"
              && generatedRule
              && semanticUpper == "POSITION"
              && rule.semanticIndex > 0;
            if (migrate)
              rule.positionSpace = D3D11CapturedPositionSpace::World;
            migrateAutoViewToWorld[fields[0]] = migrate;
          } catch (...) {
            continue;
          }
        } else {
          continue;
        }

        m_positionRules[fields[0]] = std::move(rule);
        ++loadedRules;
      }
      input.close();

      uint32_t migratedRules = 0;
      for (const auto& migration : migrateAutoViewToWorld) {
        if (!migration.second)
          continue;
        const auto rule = m_positionRules.find(migration.first);
        if (rule == m_positionRules.end())
          continue;
        appendAutoRule(migration.first, rule->second, "auto-v2-migrated");
        ++migratedRules;
      }

      Logger::info(str::format(
        "[Remix-DX11][profile] executable='", env::getExeName(),
        "' file='", m_path.string(), "' positionRules=", loadedRules,
        " migratedWorldRules=", migratedRules));
    }

    void appendAutoRule(
      const std::string& shaderKey,
      const D3D11PositionProfileRule& rule,
      const char* source = "auto") {
      if (m_path.empty())
        return;

      const bool needsHeader = !std::filesystem::exists(m_path);
      std::ofstream output(m_path, std::ios::out | std::ios::app);
      if (!output)
        return;
      if (needsHeader) {
        output << "# DXVK Remix DX11 executable shader profile v2\n";
        output << "# executable=" << env::getExeName() << "\n";
        output << "# shader|position-space|semantic|index|source\n";
      }
      output << shaderKey << "|"
             << (rule.positionSpace == D3D11CapturedPositionSpace::World ? "world" : "view")
             << "|" << rule.semanticName << "|"
             << rule.semanticIndex << "|" << source << "\n";
      output.flush();
    }

    dxvk::mutex m_mutex;
    bool m_loaded = false;
    std::filesystem::path m_path;
    std::unordered_map<std::string, D3D11PositionProfileRule> m_positionRules;
  };

  static D3D11PositionTransformBinding findPositionTransformBinding(const DxbcModule& module) {
    D3D11PositionTransformBinding result;

    const Rc<DxbcIsgn> outputSignature = module.osgn();
    const Rc<DxbcIsgn> inputSignature = module.isgn();
    if (outputSignature == nullptr || inputSignature == nullptr)
      return result;

    uint32_t positionRegister = UINT32_MAX;
    for (const DxbcSgnEntry& entry : *outputSignature) {
      std::string semantic = entry.semanticName;
      std::transform(semantic.begin(), semantic.end(), semantic.begin(),
        [](unsigned char c) { return char(std::toupper(c)); });
      if (entry.systemValue == DxbcSystemValue::Position
       || semantic == "SV_POSITION"
       || semantic == "POSITION") {
        positionRegister = entry.registerId;
        break;
      }
    }
    if (positionRegister == UINT32_MAX)
      return result;

    uint32_t positionInputRegister = UINT32_MAX;
    bool positionInputHasW = false;
    for (const DxbcSgnEntry& entry : *inputSignature) {
      std::string semantic = entry.semanticName;
      std::transform(semantic.begin(), semantic.end(), semantic.begin(),
        [](unsigned char c) { return char(std::toupper(c)); });
      if ((semantic == "POSITION" || semantic == "SV_POSITION")
       && entry.semanticIndex == 0) {
        positionInputRegister = entry.registerId;
        positionInputHasW = entry.componentMask[3];
        break;
      }
    }
    if (positionInputRegister == UINT32_MAX)
      return result;

    constexpr int32_t kInvalidRegister = -1;
    constexpr int32_t kSyntheticAffineRow = -2;

    struct TransformComponents {
      std::array<int32_t, 4> cbSlots = {
        kInvalidRegister, kInvalidRegister, kInvalidRegister, kInvalidRegister };
      std::array<int32_t, 4> cbRegisters = {
        kInvalidRegister, kInvalidRegister, kInvalidRegister, kInvalidRegister };
      std::array<uint32_t, 4> prefixCounts = { 0, 0, 0, 0 };
      std::array<D3D11PositionTransformMatrixBinding, 4> prefixes;
    } position;
    std::unordered_map<uint32_t, TransformComponents> temporaryTransforms;

    enum class PositionOrigin : uint8_t {
      Unknown,
      PositionX,
      PositionY,
      PositionZ,
      One,
    };
    using PositionOrigins = std::array<PositionOrigin, 4>;
    std::unordered_map<uint32_t, PositionOrigins> temporaryOrigins;

    auto staticRegisterIndex = [](const DxbcRegister& reg, uint32_t dimension, int32_t& index) {
      if (reg.idxDim <= dimension || reg.idx[dimension].relReg != nullptr)
        return false;
      index = reg.idx[dimension].offset;
      return index >= 0;
    };

    auto matrixBindingEqual = [](const D3D11PositionTransformMatrixBinding& a,
                                 const D3D11PositionTransformMatrixBinding& b) {
      return a.constantBufferSlot == b.constantBufferSlot
          && a.constantRegisters == b.constantRegisters
          && a.columns == b.columns;
    };

    auto collapseTransform = [&](const TransformComponents& transform,
                                 D3D11PositionTransformBinding& chain) {
      const bool syntheticW = transform.cbRegisters[3] == kSyntheticAffineRow;
      const uint32_t realRowCount = syntheticW ? 3u : 4u;
      const int32_t slot = transform.cbSlots[0];
      if (slot < 0)
        return false;

      for (uint32_t component = 0; component < realRowCount; ++component) {
        if (transform.cbSlots[component] != slot || transform.cbRegisters[component] < 0)
          return false;
      }
      if (syntheticW) {
        if (transform.cbSlots[3] != kSyntheticAffineRow)
          return false;
      } else if (transform.cbSlots[3] != slot || transform.cbRegisters[3] < 0) {
        return false;
      }

      std::array<int32_t, 4> sortedRegisters = transform.cbRegisters;
      std::sort(sortedRegisters.begin(), sortedRegisters.begin() + realRowCount);
      for (uint32_t i = 1; i < realRowCount; ++i) {
        if (sortedRegisters[i] != sortedRegisters[0] + int32_t(i))
          return false;
      }

      const uint32_t prefixCount = transform.prefixCounts[0];
      if (prefixCount > 1)
        return false;
      for (uint32_t component = 1; component < 4; ++component) {
        if (transform.prefixCounts[component] != prefixCount)
          return false;
        if (prefixCount != 0
         && !matrixBindingEqual(transform.prefixes[component], transform.prefixes[0]))
          return false;
      }

      chain = {};
      chain.valid = true;
      chain.matrixCount = prefixCount + 1;
      if (prefixCount != 0)
        chain.matrices[0] = transform.prefixes[0];

      D3D11PositionTransformMatrixBinding& current = chain.matrices[prefixCount];
      current.constantBufferSlot = uint32_t(slot);
      for (uint32_t component = 0; component < 4; ++component) {
        current.constantRegisters[component] = transform.cbRegisters[component] == kSyntheticAffineRow
          ? UINT32_MAX
          : uint32_t(transform.cbRegisters[component]);
      }
      return true;
    };

    auto invalidateWrittenComponents = [=](TransformComponents& transform, const DxbcRegMask& mask) {
      for (uint32_t component = 0; component < 4; ++component) {
        if (mask[component]) {
          transform.cbSlots[component] = kInvalidRegister;
          transform.cbRegisters[component] = kInvalidRegister;
          transform.prefixCounts[component] = 0;
        }
      }
    };

    auto invalidateOrigins = [](PositionOrigins& origins, const DxbcRegMask& mask) {
      for (uint32_t component = 0; component < 4; ++component) {
        if (mask[component])
          origins[component] = PositionOrigin::Unknown;
      }
    };

    auto sourceOrigin = [&](const DxbcRegister& source, uint32_t destinationComponent) {
      if (!source.modifiers.isClear())
        return PositionOrigin::Unknown;
      const uint32_t sourceComponent = source.swizzle[destinationComponent];

      int32_t sourceRegister = -1;
      if (source.type == DxbcOperandType::Input
       && staticRegisterIndex(source, 0, sourceRegister)
       && uint32_t(sourceRegister) == positionInputRegister) {
        switch (sourceComponent) {
          case 0: return PositionOrigin::PositionX;
          case 1: return PositionOrigin::PositionY;
          case 2: return PositionOrigin::PositionZ;
          default: return PositionOrigin::Unknown;
        }
      }

      if (source.type == DxbcOperandType::Temp
       && staticRegisterIndex(source, 0, sourceRegister)) {
        const auto entry = temporaryOrigins.find(uint32_t(sourceRegister));
        if (entry != temporaryOrigins.end())
          return entry->second[sourceComponent];
      }

      if (source.type == DxbcOperandType::Imm32) {
        const uint32_t bits = source.componentCount == DxbcComponentCount::Component1
          ? source.imm.u32_1
          : source.imm.u32_4[sourceComponent];
        if (bits == 0x3f800000u)
          return PositionOrigin::One;
      }

      return PositionOrigin::Unknown;
    };

    auto isCanonicalPositionVector = [&](const DxbcRegister& vector) {
      if (!vector.modifiers.isClear())
        return false;

      int32_t vectorRegister = -1;
      if (vector.type == DxbcOperandType::Input
       && positionInputHasW
       && staticRegisterIndex(vector, 0, vectorRegister)
       && uint32_t(vectorRegister) == positionInputRegister) {
        return vector.swizzle == DxbcRegSwizzle(0, 1, 2, 3);
      }

      if (vector.type != DxbcOperandType::Temp
       || !staticRegisterIndex(vector, 0, vectorRegister))
        return false;
      const auto origins = temporaryOrigins.find(uint32_t(vectorRegister));
      if (origins == temporaryOrigins.end())
        return false;

      const std::array<PositionOrigin, 4> expected = {
        PositionOrigin::PositionX,
        PositionOrigin::PositionY,
        PositionOrigin::PositionZ,
        PositionOrigin::One,
      };
      for (uint32_t component = 0; component < 4; ++component) {
        if (origins->second[vector.swizzle[component]] != expected[component])
          return false;
      }
      return true;
    };

    auto recordDp4 = [&](TransformComponents& transform, const DxbcRegister& dst,
                         const DxbcShaderInstruction& ins) {
      if (dst.mask.popCount() != 1)
        return false;

      const DxbcRegister* cb = nullptr;
      const DxbcRegister* vector = nullptr;
      if (ins.src[0].type == DxbcOperandType::ConstantBuffer) {
        cb = &ins.src[0];
        vector = &ins.src[1];
      } else if (ins.src[1].type == DxbcOperandType::ConstantBuffer) {
        cb = &ins.src[1];
        vector = &ins.src[0];
      }
      if (cb == nullptr || vector == nullptr || !cb->modifiers.isClear()
       || cb->swizzle != DxbcRegSwizzle(0, 1, 2, 3))
        return false;

      D3D11PositionTransformBinding prefix;
      if (isCanonicalPositionVector(*vector)) {
        prefix.valid = true;
        prefix.matrixCount = 0;
      } else {
        int32_t vectorRegister = -1;
        if (vector->type != DxbcOperandType::Temp
         || !vector->modifiers.isClear()
         || vector->swizzle != DxbcRegSwizzle(0, 1, 2, 3)
         || !staticRegisterIndex(*vector, 0, vectorRegister))
          return false;
        const auto source = temporaryTransforms.find(uint32_t(vectorRegister));
        if (source == temporaryTransforms.end()
         || !collapseTransform(source->second, prefix)
         || prefix.matrixCount != 1)
          return false;
      }

      int32_t slot = -1;
      int32_t cbRegister = -1;
      if (!staticRegisterIndex(*cb, 0, slot)
       || !staticRegisterIndex(*cb, 1, cbRegister))
        return false;

      const uint32_t component = dst.mask.firstSet();
      transform.cbSlots[component] = slot;
      transform.cbRegisters[component] = cbRegister;
      transform.prefixCounts[component] = prefix.matrixCount;
      if (prefix.matrixCount != 0)
        transform.prefixes[component] = prefix.matrices[0];
      return true;
    };

    DxbcCodeSlice code = module.instructionSlice();
    DxbcDecodeContext decoder;
    while (!code.atEnd()) {
      decoder.decodeInstruction(code);
      const DxbcShaderInstruction& ins = decoder.getInstruction();
      if (ins.dstCount == 0)
        continue;

      const DxbcRegister& dst = ins.dst[0];
      int32_t dstRegister = -1;
      const bool isPositionOutput = dst.type == DxbcOperandType::Output
        && staticRegisterIndex(dst, 0, dstRegister)
        && uint32_t(dstRegister) == positionRegister;
      const bool isTemporary = dst.type == DxbcOperandType::Temp
        && staticRegisterIndex(dst, 0, dstRegister);

      if (ins.op == DxbcOpcode::Dp4 && ins.dstCount == 1 && ins.srcCount == 2) {
        if (isPositionOutput) {
          if (!recordDp4(position, dst, ins))
            invalidateWrittenComponents(position, dst.mask);
        } else if (isTemporary) {
          TransformComponents& temporary = temporaryTransforms[uint32_t(dstRegister)];
          if (!recordDp4(temporary, dst, ins))
            invalidateWrittenComponents(temporary, dst.mask);
          invalidateOrigins(temporaryOrigins[uint32_t(dstRegister)], dst.mask);
        }
        continue;
      }

      // Most optimized SM5 vertex shaders calculate clip position into a
      // temporary and end with `mov oN, rM`. Propagate the four proven dp4
      // components through that exact move, including scalar write masks and
      // source swizzles. No arithmetic or dynamic indexing is guessed.
      if (ins.op == DxbcOpcode::Mov && ins.dstCount == 1 && ins.srcCount == 1) {
        TransformComponents* destinationTransform = isPositionOutput
          ? &position
          : (isTemporary ? &temporaryTransforms[uint32_t(dstRegister)] : nullptr);

        bool copiedTransform = false;
        int32_t sourceRegister = -1;
        if (destinationTransform != nullptr
         && ins.src[0].type == DxbcOperandType::Temp
         && ins.src[0].modifiers.isClear()
         && staticRegisterIndex(ins.src[0], 0, sourceRegister)) {
          const auto source = temporaryTransforms.find(uint32_t(sourceRegister));
          if (source != temporaryTransforms.end()) {
            for (uint32_t component = 0; component < 4; ++component) {
              if (!dst.mask[component])
                continue;
              const uint32_t sourceComponent = ins.src[0].swizzle[component];
              destinationTransform->cbSlots[component] = source->second.cbSlots[sourceComponent];
              destinationTransform->cbRegisters[component] = source->second.cbRegisters[sourceComponent];
              destinationTransform->prefixCounts[component] = source->second.prefixCounts[sourceComponent];
              destinationTransform->prefixes[component] = source->second.prefixes[sourceComponent];
            }
            copiedTransform = true;
          }
        }
        if (destinationTransform != nullptr && !copiedTransform)
          invalidateWrittenComponents(*destinationTransform, dst.mask);

        if (isTemporary) {
          PositionOrigins& origins = temporaryOrigins[uint32_t(dstRegister)];
          for (uint32_t component = 0; component < 4; ++component) {
            if (!dst.mask[component])
              continue;
            origins[component] = sourceOrigin(ins.src[0], component);
            if (component == 3 && origins[component] == PositionOrigin::One) {
              TransformComponents& temporary = temporaryTransforms[uint32_t(dstRegister)];
              temporary.cbSlots[3] = kSyntheticAffineRow;
              temporary.cbRegisters[3] = kSyntheticAffineRow;
              temporary.prefixCounts[3] = 0;
            }
          }
        }
        continue;
      }

      if (isPositionOutput)
        invalidateWrittenComponents(position, dst.mask);
      if (isTemporary) {
        invalidateWrittenComponents(temporaryTransforms[uint32_t(dstRegister)], dst.mask);
        invalidateOrigins(temporaryOrigins[uint32_t(dstRegister)], dst.mask);
      }
    }

    collapseTransform(position, result);
    return result;
  }

  // Column form of the same proof: `out = Σ_k in_k · cb[C_k]` written as
  //   mul t, cb[C0], v.xxxx  /  mad t, cb[C1], v.yyyy, t  /  mad t, cb[C2], v.zzzz, t
  //   add t, t, cb[C3]       (or mad t, cb[C3], v.wwww, t)
  // which is what fxc emits for Unity's mul(M, v) (column-major matrices) and
  // for UE's mul(v, M) with row_major packing. A second chain whose broadcast
  // operands come from a finished first chain is the view-projection stage.
  // Columns must be consecutive registers of one cbuffer, in input order.
  static D3D11PositionTransformBinding findColumnPositionTransformBinding(const DxbcModule& module) {
    D3D11PositionTransformBinding result;
    const Rc<DxbcIsgn> osgn = module.osgn();
    const Rc<DxbcIsgn> isgn = module.isgn();
    if (osgn == nullptr || isgn == nullptr)
      return result;

    int32_t posOut = -1, posIn = -1;
    for (const DxbcSgnEntry& e : *osgn) {
      if (e.systemValue == DxbcSystemValue::Position) { posOut = int32_t(e.registerId); break; }
    }
    for (const DxbcSgnEntry& e : *isgn) {
      std::string s = e.semanticName;
      std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::toupper(c)); });
      if (s == "POSITION" && e.semanticIndex == 0) { posIn = int32_t(e.registerId); break; }
    }
    if (posOut < 0 || posIn < 0)
      return result;

    auto staticIndex = [](const DxbcRegister& r, uint32_t dim, int32_t& out) {
      if (r.idxDim <= dim || r.idx[dim].relReg != nullptr || r.idx[dim].offset < 0)
        return false;
      out = r.idx[dim].offset;
      return true;
    };

    // Per temp component: which POSITION component (0..2), constant one (3),
    // or nothing (-1) it holds, from `mov r.xyz, v0.xyz` / `mov r.w, l(1.0)`.
    std::unordered_map<int32_t, std::array<int8_t, 4>> origins;

    struct Chain {
      bool active = false;
      int32_t slot = -1;
      std::array<int32_t, 4> columns = { -1, -1, -1, -1 };
      uint32_t mask = 0;
      int32_t sourceTemp = -1;  // -1: POSITION input; else temp holding stage 1
    };
    std::unordered_map<int32_t, Chain> chains;
    Chain outputChain;
    std::unordered_map<int32_t, Chain> finished;  // stage-1 chains by temp, snapshot

    auto writtenMask = [](const DxbcRegister& dst) {
      uint32_t m = 0;
      for (uint32_t c = 0; c < 4; ++c) if (dst.mask[c]) m |= 1u << c;
      return m;
    };
    auto firstComp = [](uint32_t mask) {
      for (uint32_t c = 0; c < 4; ++c) if (mask & (1u << c)) return c;
      return 0u;
    };
    // cb operand read component-for-component (swizzle c -> c on written comps).
    auto asColumn = [&](const DxbcRegister& r, uint32_t mask, int32_t& slot, int32_t& reg) {
      if (r.type != DxbcOperandType::ConstantBuffer || !r.modifiers.isClear()
       || !staticIndex(r, 0, slot) || !staticIndex(r, 1, reg))
        return false;
      for (uint32_t c = 0; c < 4; ++c)
        if ((mask & (1u << c)) && r.swizzle[c] != c)
          return false;
      return true;
    };
    // Broadcast vector operand: returns the input component k (0..3) it feeds
    // and the source (-1 = POSITION, else the stage-1 temp).
    auto asBroadcast = [&](const DxbcRegister& r, uint32_t mask, int32_t& k, int32_t& sourceTemp) {
      if (!r.modifiers.isClear())
        return false;
      const uint32_t c0 = firstComp(mask);
      const uint32_t comp = r.swizzle[c0];
      for (uint32_t c = 0; c < 4; ++c)
        if ((mask & (1u << c)) && r.swizzle[c] != comp)
          return false;
      int32_t index = -1;
      if (r.type == DxbcOperandType::Input && staticIndex(r, 0, index) && index == posIn) {
        if (comp > 3) return false;
        k = int32_t(comp);
        sourceTemp = -1;
        return true;
      }
      if (r.type != DxbcOperandType::Temp || !staticIndex(r, 0, index))
        return false;
      auto o = origins.find(index);
      if (o != origins.end() && o->second[comp] >= 0) {
        k = o->second[comp];
        sourceTemp = -1;
        return true;
      }
      auto f = finished.find(index);
      if (f != finished.end() && (f->second.mask & (1u << comp))) {
        k = int32_t(comp);
        sourceTemp = index;
        return true;
      }
      return false;
    };
    auto addTerm = [&](Chain& chain, int32_t slot, int32_t reg, int32_t k, int32_t sourceTemp) {
      if (chain.slot != slot || chain.sourceTemp != sourceTemp || chain.columns[k] >= 0)
        return false;
      chain.columns[k] = reg;
      return true;
    };
    auto chainFor = [&](const DxbcRegister& dst, int32_t& tempIndex) -> Chain* {
      int32_t index = -1;
      if (dst.type == DxbcOperandType::Output && staticIndex(dst, 0, index) && index == posOut) {
        tempIndex = -1;
        return &outputChain;
      }
      if (dst.type == DxbcOperandType::Temp && staticIndex(dst, 0, index)) {
        tempIndex = index;
        return &chains[index];
      }
      return nullptr;
    };
    auto accumulatorOf = [&](const DxbcRegister& r, uint32_t mask) -> const Chain* {
      int32_t index = -1;
      if (r.type != DxbcOperandType::Temp || !r.modifiers.isClear() || !staticIndex(r, 0, index))
        return nullptr;
      for (uint32_t c = 0; c < 4; ++c)
        if ((mask & (1u << c)) && r.swizzle[c] != c)
          return nullptr;
      auto it = chains.find(index);
      return it != chains.end() && it->second.active ? &it->second : nullptr;
    };
    auto snapshotIfStage1 = [&](int32_t tempIndex, const Chain& chain) {
      if (tempIndex >= 0 && chain.active && chain.sourceTemp < 0
       && chain.columns[0] >= 0 && chain.columns[1] >= 0 && chain.columns[2] >= 0)
        finished[tempIndex] = chain;
    };

    DxbcCodeSlice code = module.instructionSlice();
    DxbcDecodeContext decoder;
    while (!code.atEnd()) {
      decoder.decodeInstruction(code);
      const DxbcShaderInstruction& ins = decoder.getInstruction();
      if (ins.dstCount == 0)
        continue;
      const DxbcRegister& dst = ins.dst[0];
      int32_t tempIndex = -1;
      Chain* chain = chainFor(dst, tempIndex);
      const uint32_t mask = writtenMask(dst);
      bool handled = false;

      if (chain != nullptr && ins.op == DxbcOpcode::Mul && ins.srcCount == 2) {
        for (uint32_t order = 0; order < 2 && !handled; ++order) {
          int32_t slot, reg, k, src;
          if (asColumn(ins.src[order], mask, slot, reg) && asBroadcast(ins.src[1 - order], mask, k, src)) {
            *chain = Chain();
            chain->active = true;
            chain->slot = slot;
            chain->columns[k] = reg;
            chain->mask = mask;
            chain->sourceTemp = src;
            handled = true;
          }
        }
      } else if (chain != nullptr && ins.op == DxbcOpcode::Mad && ins.srcCount == 3) {
        if (const Chain* acc = accumulatorOf(ins.src[2], mask)) {
          Chain next = *acc;
          for (uint32_t order = 0; order < 2 && !handled; ++order) {
            int32_t slot, reg, k, src;
            if (asColumn(ins.src[order], mask, slot, reg) && asBroadcast(ins.src[1 - order], mask, k, src)
             && (next.mask & mask) == mask && addTerm(next, slot, reg, k, src)) {
              next.mask = mask;
              *chain = next;
              handled = true;
            }
          }
        }
      } else if (chain != nullptr && ins.op == DxbcOpcode::Add && ins.srcCount == 2) {
        for (uint32_t order = 0; order < 2 && !handled; ++order) {
          int32_t slot, reg;
          const Chain* acc = accumulatorOf(ins.src[order], mask);
          if (acc != nullptr && asColumn(ins.src[1 - order], mask, slot, reg)
           && (acc->mask & mask) == mask) {
            Chain next = *acc;
            if (addTerm(next, slot, reg, 3, next.sourceTemp)) {
              next.mask = mask;
              *chain = next;
              handled = true;
            }
          }
        }
      } else if (chain != nullptr && ins.op == DxbcOpcode::Mov && ins.srcCount == 1) {
        if (const Chain* acc = accumulatorOf(ins.src[0], mask)) {
          if ((acc->mask & mask) == mask) {
            Chain copy = *acc;
            copy.mask = mask;
            *chain = copy;
            handled = true;
          }
        }
      }

      if (handled) {
        snapshotIfStage1(tempIndex, *chain);
      } else if (chain != nullptr) {
        // Any other write ends whatever chain this register carried.
        if (tempIndex >= 0) {
          chains.erase(tempIndex);
          finished.erase(tempIndex);
        } else {
          outputChain = Chain();
        }
      }

      // Origin tracking for `mov r.xyz, v0.xyz` and `mov r.w, l(1.0)`.
      if (dst.type == DxbcOperandType::Temp && staticIndex(dst, 0, tempIndex) && !handled) {
        auto& o = origins[tempIndex];
        for (uint32_t c = 0; c < 4; ++c) {
          if (!(mask & (1u << c)))
            continue;
          o[c] = -1;
          if (ins.op != DxbcOpcode::Mov || ins.srcCount != 1 || !ins.src[0].modifiers.isClear())
            continue;
          const DxbcRegister& s = ins.src[0];
          int32_t index = -1;
          if (s.type == DxbcOperandType::Input && staticIndex(s, 0, index) && index == posIn && s.swizzle[c] < 3)
            o[c] = int8_t(s.swizzle[c]);
          else if (s.type == DxbcOperandType::Imm32) {
            const uint32_t bits = s.componentCount == DxbcComponentCount::Component1 ? s.imm.u32_1 : s.imm.u32_4[c];
            if (bits == 0x3f800000u)
              o[c] = 3;  // w = 1: the translation column
          }
        }
      }
    }

    // SV_Position must be the full xyzw result of a chain.
    const Chain& out = outputChain;
    if (!out.active || out.mask != 0xFu || out.columns[0] < 0 || out.columns[1] < 0 || out.columns[2] < 0)
      return result;

    auto toBinding = [](const Chain& c, D3D11PositionTransformMatrixBinding& b) {
      // Present columns must be consecutive registers in input order.
      for (uint32_t k = 1; k < 4; ++k)
        if (c.columns[k] >= 0 && c.columns[k] != c.columns[0] + int32_t(k))
          return false;
      b.constantBufferSlot = uint32_t(c.slot);
      b.columns = true;
      b.affineW = (c.mask & 0x8u) == 0u;
      for (uint32_t k = 0; k < 4; ++k)
        b.constantRegisters[k] = c.columns[k] >= 0 ? uint32_t(c.columns[k]) : UINT32_MAX;
      return true;
    };

    if (out.sourceTemp < 0) {
      if (!toBinding(out, result.matrices[0]))
        return result;
      result.matrixCount = 1;
    } else {
      auto stage1 = finished.find(out.sourceTemp);
      if (stage1 == finished.end() || out.columns[3] < 0
       || !toBinding(stage1->second, result.matrices[0])
       || !toBinding(out, result.matrices[1]))
        return result;
      result.matrixCount = 2;
    }
    result.valid = true;
    return result;
  }

  static D3D11ConstantBufferDependencyProfile findConstantBufferDependencies(
      const DxbcModule& module) {
    D3D11ConstantBufferDependencyProfile result;
    result.complete = true;

    DxbcCodeSlice code = module.instructionSlice();
    DxbcDecodeContext decoder;
    while (!code.atEnd()) {
      decoder.decodeInstruction(code);
      const DxbcShaderInstruction& ins = decoder.getInstruction();
      for (uint32_t sourceIndex = 0; sourceIndex < ins.srcCount; ++sourceIndex) {
        const DxbcRegister& source = ins.src[sourceIndex];
        if (source.type != DxbcOperandType::ConstantBuffer)
          continue;

        if (source.idxDim < 1 || source.idx[0].relReg != nullptr
         || source.idx[0].offset < 0) {
          // A dynamically selected cbuffer slot cannot be represented by a
          // bounded slot profile. Retain the conservative all-bound fallback.
          result.complete = false;
          result.dependencies.clear();
          return result;
        }

        D3D11ConstantBufferDependency dependency;
        dependency.slot = uint32_t(source.idx[0].offset);
        dependency.wholeBuffer = source.idxDim < 2
          || source.idx[1].relReg != nullptr
          || source.idx[1].offset < 0;
        dependency.constantRegister = dependency.wholeBuffer
          ? 0u : uint32_t(source.idx[1].offset);

        auto sameDependency = [&](const D3D11ConstantBufferDependency& existing) {
          if (existing.slot != dependency.slot)
            return false;
          return existing.wholeBuffer || dependency.wholeBuffer
            || existing.constantRegister == dependency.constantRegister;
        };
        auto existing = std::find_if(
          result.dependencies.begin(), result.dependencies.end(), sameDependency);
        if (existing != result.dependencies.end()) {
          if (dependency.wholeBuffer) {
            result.dependencies.erase(
              std::remove_if(result.dependencies.begin(), result.dependencies.end(),
                [&](const D3D11ConstantBufferDependency& candidate) {
                  return candidate.slot == dependency.slot;
                }),
              result.dependencies.end());
            result.dependencies.push_back(dependency);
          }
          continue;
        }
        result.dependencies.push_back(dependency);
      }
    }

    std::sort(result.dependencies.begin(), result.dependencies.end(),
      [](const D3D11ConstantBufferDependency& a,
         const D3D11ConstantBufferDependency& b) {
        if (a.slot != b.slot)
          return a.slot < b.slot;
        if (a.wholeBuffer != b.wholeBuffer)
          return a.wholeBuffer > b.wholeBuffer;
        return a.constantRegister < b.constantRegister;
      });
    return result;
  }

  enum class D3D11CaptureExpressionKind : uint8_t {
    Unknown,
    Identity,
    MatrixRow,
  };

  // Proves an exact dataflow relationship between a named non-system VS output
  // and SV_Position. Both outputs may be direct views of the same temporary, or
  // may be four-row constant-buffer transforms of it. Optimized SM5 shaders may
  // also calculate the dp4 rows in a temporary and MOV them to an output. This
  // deliberately does not infer position space from semantic names.
  //
  // Result layout is specific to position capture:
  //   one matrix:  matrices[0] maps captured output directly to clip
  //   two matrices: matrices[0] maps a shared base to captured output and
  //                 matrices[1] maps that same base to clip
  // At draw time the second form is factored as clipFromBase *
  // inverse(captureFromBase). Exact component versions prevent a reused temp
  // register or an intervening partial write from creating a false proof.
  static D3D11PositionTransformBinding findOutputToClipTransformBinding(
      const DxbcModule& module,
      const std::string& captureSemanticName,
      uint32_t captureSemanticIndex,
      std::string* rejectionReason) {
    D3D11PositionTransformBinding result;
    auto reject = [&](const std::string& reason) {
      if (rejectionReason != nullptr)
        *rejectionReason = reason;
      return result;
    };
    const Rc<DxbcIsgn> outputSignature = module.osgn();
    if (outputSignature == nullptr || captureSemanticName.empty())
      return reject("missing output signature or capture semantic");

    auto upper = [](std::string value) {
      std::transform(value.begin(), value.end(), value.begin(),
        [](unsigned char c) { return char(std::toupper(c)); });
      return value;
    };

    const std::string wantedSemantic = upper(captureSemanticName);
    uint32_t clipRegister = UINT32_MAX;
    uint32_t captureRegister = UINT32_MAX;
    for (const DxbcSgnEntry& entry : *outputSignature) {
      const std::string semantic = upper(entry.semanticName);
      if (entry.systemValue == DxbcSystemValue::Position
       || semantic == "SV_POSITION") {
        clipRegister = entry.registerId;
      }
      if (entry.systemValue == DxbcSystemValue::None
       && semantic == wantedSemantic
       && entry.semanticIndex == captureSemanticIndex) {
        captureRegister = entry.registerId;
      }
    }
    if (clipRegister == UINT32_MAX || captureRegister == UINT32_MAX
     || clipRegister == captureRegister)
      return reject(str::format(
        "output signature did not provide distinct clip/capture registers: clip=",
        clipRegister, " capture=", captureRegister));

    struct ComponentExpression {
      D3D11CaptureExpressionKind kind = D3D11CaptureExpressionKind::Unknown;
      int32_t baseTemp = -1;
      std::array<uint32_t, 4> baseVersions = { 0, 0, 0, 0 };
      uint32_t sourceComponent = 0;
      int32_t cbSlot = -1;
      int32_t cbRegister = -1;
    };
    using VectorExpression = std::array<ComponentExpression, 4>;

    VectorExpression clip;
    VectorExpression capture;
    std::unordered_map<uint32_t, VectorExpression> temporaryExpressions;
    std::unordered_map<uint32_t, std::array<uint32_t, 4>> tempVersions;

    auto staticRegisterIndex = [](const DxbcRegister& reg, uint32_t dimension, int32_t& index) {
      if (reg.idxDim <= dimension || reg.idx[dimension].relReg != nullptr)
        return false;
      index = reg.idx[dimension].offset;
      return index >= 0;
    };
    auto versionsFor = [&](uint32_t temp) -> std::array<uint32_t, 4>& {
      return tempVersions[temp];
    };
    auto invalidate = [](VectorExpression& expression, const DxbcRegMask& mask) {
      for (uint32_t component = 0; component < 4; ++component) {
        if (mask[component])
          expression[component] = ComponentExpression();
      }
    };

    auto recordIdentity = [&](ComponentExpression& destination,
                              uint32_t sourceTemp,
                              uint32_t sourceComponent) {
      destination = {};
      destination.kind = D3D11CaptureExpressionKind::Identity;
      destination.baseTemp = int32_t(sourceTemp);
      destination.baseVersions = versionsFor(sourceTemp);
      destination.sourceComponent = sourceComponent;
    };

    auto recordDp4 = [&](ComponentExpression& destination,
                         const DxbcShaderInstruction& ins) {
      const DxbcRegister* cb = nullptr;
      const DxbcRegister* vector = nullptr;
      if (ins.src[0].type == DxbcOperandType::ConstantBuffer) {
        cb = &ins.src[0];
        vector = &ins.src[1];
      } else if (ins.src[1].type == DxbcOperandType::ConstantBuffer) {
        cb = &ins.src[1];
        vector = &ins.src[0];
      }

      int32_t vectorTemp = -1;
      int32_t cbSlot = -1;
      int32_t cbRegister = -1;
      if (cb == nullptr || vector == nullptr
       || !cb->modifiers.isClear() || !vector->modifiers.isClear()
       || cb->swizzle != DxbcRegSwizzle(0, 1, 2, 3)
       || vector->type != DxbcOperandType::Temp
       || vector->swizzle != DxbcRegSwizzle(0, 1, 2, 3)
       || !staticRegisterIndex(*vector, 0, vectorTemp)
       || !staticRegisterIndex(*cb, 0, cbSlot)
       || !staticRegisterIndex(*cb, 1, cbRegister)) {
        destination = {};
        return false;
      }

      destination = {};
      destination.kind = D3D11CaptureExpressionKind::MatrixRow;
      destination.baseTemp = vectorTemp;
      destination.baseVersions = versionsFor(uint32_t(vectorTemp));
      destination.cbSlot = cbSlot;
      destination.cbRegister = cbRegister;
      return true;
    };

    DxbcCodeSlice code = module.instructionSlice();
    DxbcDecodeContext decoder;
    while (!code.atEnd()) {
      decoder.decodeInstruction(code);
      const DxbcShaderInstruction& ins = decoder.getInstruction();
      if (ins.dstCount == 0)
        continue;

      const DxbcRegister& dst = ins.dst[0];
      int32_t dstRegister = -1;
      const bool isClipOutput = dst.type == DxbcOperandType::Output
        && staticRegisterIndex(dst, 0, dstRegister)
        && uint32_t(dstRegister) == clipRegister;
      const bool isCaptureOutput = dst.type == DxbcOperandType::Output
        && staticRegisterIndex(dst, 0, dstRegister)
        && uint32_t(dstRegister) == captureRegister;
      const bool isTemporary = dst.type == DxbcOperandType::Temp
        && staticRegisterIndex(dst, 0, dstRegister);

      VectorExpression* destination = isClipOutput
        ? &clip
        : (isCaptureOutput
          ? &capture
          : (isTemporary
            ? &temporaryExpressions[uint32_t(dstRegister)]
            : nullptr));

      bool handledWrite = false;
      if (destination != nullptr
       && ins.op == DxbcOpcode::Dp4
       && ins.dstCount == 1 && ins.srcCount == 2
       && dst.mask.popCount() == 1) {
        handledWrite = recordDp4((*destination)[dst.mask.firstSet()], ins);
      }

      if (destination != nullptr
       && ins.op == DxbcOpcode::Mov
       && ins.dstCount == 1 && ins.srcCount == 1
       && ins.src[0].type == DxbcOperandType::Temp
       && ins.src[0].modifiers.isClear()) {
        int32_t sourceTemp = -1;
        if (staticRegisterIndex(ins.src[0], 0, sourceTemp)) {
          const auto sourceExpression = temporaryExpressions.find(uint32_t(sourceTemp));
          for (uint32_t component = 0; component < 4; ++component) {
            if (!dst.mask[component])
              continue;
            const uint32_t sourceComponent = ins.src[0].swizzle[component];

            // A capture MOV defines the captured temporary itself. Do not
            // chase older arithmetic that happened to produce individual
            // components of that temporary. Clip-output MOVs, however, must
            // propagate dp4 rows calculated in an optimized temporary.
            if (!isCaptureOutput
             && sourceExpression != temporaryExpressions.end()
             && sourceExpression->second[sourceComponent].kind != D3D11CaptureExpressionKind::Unknown) {
              (*destination)[component] = sourceExpression->second[sourceComponent];
            } else {
              recordIdentity((*destination)[component],
                uint32_t(sourceTemp), sourceComponent);
            }
          }
          handledWrite = true;
        }
      }

      if (destination != nullptr && !handledWrite)
        invalidate(*destination, dst.mask);

      // Advance component versions after all source operands for this
      // instruction have been observed.
      if (isTemporary) {
        auto& versions = versionsFor(uint32_t(dstRegister));
        for (uint32_t component = 0; component < 4; ++component) {
          if (dst.mask[component])
            ++versions[component];
        }
      }
    }

    struct CollapsedExpression {
      bool identity = false;
      int32_t baseTemp = -1;
      std::array<uint32_t, 4> baseVersions = { 0, 0, 0, 0 };
      D3D11PositionTransformMatrixBinding matrix;
    };
    auto collapse = [](const VectorExpression& expression,
                       CollapsedExpression& collapsed) {
      const D3D11CaptureExpressionKind kind = expression[0].kind;
      if (kind == D3D11CaptureExpressionKind::Unknown)
        return false;

      collapsed = {};
      collapsed.identity = kind == D3D11CaptureExpressionKind::Identity;
      collapsed.baseTemp = expression[0].baseTemp;
      collapsed.baseVersions = expression[0].baseVersions;
      const int32_t cbSlot = expression[0].cbSlot;
      if (!collapsed.identity) {
        if (kind != D3D11CaptureExpressionKind::MatrixRow || cbSlot < 0)
          return false;
        collapsed.matrix.constantBufferSlot = uint32_t(cbSlot);
      }

      for (uint32_t component = 0; component < 4; ++component) {
        const ComponentExpression& source = expression[component];
        if (source.kind != kind
         || source.baseTemp != collapsed.baseTemp
         || source.baseVersions != collapsed.baseVersions)
          return false;
        if (collapsed.identity) {
          if (source.sourceComponent != component)
            return false;
        } else {
          if (source.cbSlot != cbSlot || source.cbRegister < 0)
            return false;
          collapsed.matrix.constantRegisters[component] = uint32_t(source.cbRegister);
        }
      }
      return collapsed.baseTemp >= 0;
    };

    CollapsedExpression capturedExpression;
    CollapsedExpression clipExpression;
    if (!collapse(capture, capturedExpression))
      return reject(str::format(
        "capture output o", captureRegister,
        " was not a complete identity/DP4 transform of one temporary"));
    if (!collapse(clip, clipExpression))
      return reject(str::format(
        "SV_Position o", clipRegister,
        " was not a complete DP4 transform (including MOV propagation)"));
    if (capturedExpression.baseTemp != clipExpression.baseTemp
     || capturedExpression.baseVersions != clipExpression.baseVersions)
      return reject(str::format(
        "capture and clip outputs do not consume the same temporary version: captureTemp=",
        capturedExpression.baseTemp, " clipTemp=", clipExpression.baseTemp));
    if (clipExpression.identity)
      return reject("SV_Position is already the captured vector; clip-space geometry is unsafe");

    result.valid = true;
    if (capturedExpression.identity) {
      result.matrixCount = 1;
      result.matrices[0] = clipExpression.matrix;
    } else {
      result.matrixCount = 2;
      result.matrices[0] = capturedExpression.matrix;
      result.matrices[1] = clipExpression.matrix;
    }
    return result;
  }

  // DX11_V277_REAL_SHADER_MODEL: parse the shader model version from the raw
  // DXBC container. Layout: 'DXBC' magic (4) + checksum (16) + one (4) +
  // totalSize (4) + chunkCount (4) + chunkCount x uint32 chunk offsets; each
  // chunk = fourCC (4) + size (4) + data. The SHDR (SM4) or SHEX (SM5) chunk's
  // first DWORD is the version token: bits [3:0] = minor, [7:4] = major.
  // Fully bounds-checked; returns false (caller keeps the 4.0 default) on any
  // malformed input.
  static bool parseDxbcShaderModel(
    const void* pBytecode,
    size_t      length,
    uint32_t&   outMajor,
    uint32_t&   outMinor) {
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(pBytecode);
    if (bytes == nullptr || length < 0x20)
      return false;

    auto readU32 = [&](size_t offset) -> uint32_t {
      uint32_t v = 0;
      std::memcpy(&v, bytes + offset, sizeof(v));
      return v;
    };

    // 'DXBC' magic
    if (readU32(0) != 0x43425844u)
      return false;

    const uint32_t chunkCount = readU32(0x1C);
    if (chunkCount == 0 || chunkCount > 64)
      return false;
    if (0x20 + size_t(chunkCount) * 4 > length)
      return false;

    constexpr uint32_t kFourCcShdr = 0x52444853u; // 'SHDR'
    constexpr uint32_t kFourCcShex = 0x58454853u; // 'SHEX'

    for (uint32_t i = 0; i < chunkCount; ++i) {
      const uint32_t chunkOffset = readU32(0x20 + size_t(i) * 4);
      // Chunk header (fourCC + size) plus the version DWORD must fit.
      if (size_t(chunkOffset) + 12 > length)
        continue;

      const uint32_t fourCc = readU32(chunkOffset);
      if (fourCc != kFourCcShdr && fourCc != kFourCcShex)
        continue;

      const uint32_t versionToken = readU32(size_t(chunkOffset) + 8);
      const uint32_t minor = versionToken & 0xFu;
      const uint32_t major = (versionToken >> 4) & 0xFu;
      // D3D11 shader models are 4.0 - 5.1; reject garbage tokens.
      if (major < 4 || major > 6 || minor > 1)
        return false;

      outMajor = major;
      outMinor = minor;
      return true;
    }

    return false;
  }

  // DX11_V281_FIXED_FUNCTION: walk the SHDR/SHEX instruction stream for the
  // discard opcode (13 - covers both discard_z and discard_nz, i.e. HLSL
  // clip() and explicit discard). D3D10+ removed the fixed-function alpha
  // test; a pixel shader that discards IS this API generation's alpha test,
  // so the capture layer needs to know. Instruction skipping uses the
  // per-instruction DWORD length in OpcodeToken0 bits [30:24]; custom-data
  // blocks (opcode 53) carry their full DWORD count in the following token
  // instead. Fully bounds-checked with a hard iteration cap; returns false
  // on any malformed input.
  static bool parseDxbcUsesDiscard(
    const void* pBytecode,
    size_t      length) {
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(pBytecode);
    if (bytes == nullptr || length < 0x20)
      return false;

    auto readU32 = [&](size_t offset) -> uint32_t {
      uint32_t v = 0;
      std::memcpy(&v, bytes + offset, sizeof(v));
      return v;
    };

    if (readU32(0) != 0x43425844u) // 'DXBC'
      return false;

    const uint32_t chunkCount = readU32(0x1C);
    if (chunkCount == 0 || chunkCount > 64)
      return false;
    if (0x20 + size_t(chunkCount) * 4 > length)
      return false;

    constexpr uint32_t kFourCcShdr = 0x52444853u; // 'SHDR'
    constexpr uint32_t kFourCcShex = 0x58454853u; // 'SHEX'

    for (uint32_t i = 0; i < chunkCount; ++i) {
      const uint32_t chunkOffset = readU32(0x20 + size_t(i) * 4);
      if (size_t(chunkOffset) + 16 > length)
        continue;

      const uint32_t fourCc = readU32(chunkOffset);
      if (fourCc != kFourCcShdr && fourCc != kFourCcShex)
        continue;

      const uint32_t chunkSize = readU32(size_t(chunkOffset) + 4);
      const size_t dataStart = size_t(chunkOffset) + 8;
      if (dataStart + chunkSize > length || chunkSize < 8)
        return false;

      // Program header: version token, then total program length in DWORDs
      // (including these two tokens). Instructions follow.
      const uint32_t programLength = readU32(dataStart + 4);
      const size_t programEnd = std::min(
        dataStart + size_t(programLength) * 4,
        dataStart + chunkSize);

      size_t pos = dataStart + 8;
      uint32_t iterations = 0;
      while (pos + 4 <= programEnd && ++iterations < (1u << 20)) {
        const uint32_t token0 = readU32(pos);
        const uint32_t opcode = token0 & 0x7FFu;

        if (opcode == 13u) // discard
          return true;

        size_t instrDwords;
        if (opcode == 53u) { // custom data: next token holds the full length
          if (pos + 8 > programEnd)
            return false;
          instrDwords = readU32(pos + 4);
          if (instrDwords < 2)
            return false;
        } else {
          instrDwords = (token0 >> 24) & 0x7Fu;
          if (instrDwords == 0)
            return false;
        }
        pos += instrDwords * 4;
      }
      return false;
    }

    return false;
  }

  // DX11_V280_TEXCOORD_CAPTURE: scan the DXBC OUTPUT signature chunk
  // (OSGN = SM4/5, OSG5 = SM5 with streams, OSG1 = SM5.1) for a texcoord-like
  // element the stream-out capture can read back. Engine-agnostic on purpose:
  // semantic names in DXBC signatures are free-form strings chosen by each
  // engine's HLSL ("TEXCOORD", "UV", "TexUV", ...), so this matches by
  // substring preference rather than any fixed per-engine table. Requirements
  // are structural: not a system value, float components, at least .xy
  // written, stream 0. Fully bounds-checked; returns false on any malformed
  // input (caller simply skips capture support for that shader).
  static bool parseDxbcOutputTexcoord(
    const void*  pBytecode,
    size_t       length,
    std::string& outName,
    uint32_t&    outIndex,
    std::vector<D3D11TexcoordSemantic>* outSemantics = nullptr) {
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(pBytecode);
    if (bytes == nullptr || length < 0x20)
      return false;

    auto readU32 = [&](size_t offset) -> uint32_t {
      uint32_t v = 0;
      std::memcpy(&v, bytes + offset, sizeof(v));
      return v;
    };

    // 'DXBC' magic
    if (readU32(0) != 0x43425844u)
      return false;

    const uint32_t chunkCount = readU32(0x1C);
    if (chunkCount == 0 || chunkCount > 64)
      return false;
    if (0x20 + size_t(chunkCount) * 4 > length)
      return false;

    constexpr uint32_t kFourCcOsgn = 0x4E47534Fu; // 'OSGN'
    constexpr uint32_t kFourCcOsg5 = 0x3547534Fu; // 'OSG5'
    constexpr uint32_t kFourCcOsg1 = 0x3147534Fu; // 'OSG1'

    for (uint32_t i = 0; i < chunkCount; ++i) {
      const uint32_t chunkOffset = readU32(0x20 + size_t(i) * 4);
      if (size_t(chunkOffset) + 16 > length)
        continue;

      const uint32_t fourCc = readU32(chunkOffset);
      if (fourCc != kFourCcOsgn && fourCc != kFourCcOsg5 && fourCc != kFourCcOsg1)
        continue;

      const uint32_t chunkSize = readU32(size_t(chunkOffset) + 4);
      const size_t dataStart = size_t(chunkOffset) + 8;
      if (dataStart + chunkSize > length || chunkSize < 8)
        return false;

      const uint32_t elementCount = readU32(dataStart);
      if (elementCount == 0 || elementCount > 64)
        return false;

      // OSG5/OSG1 elements lead with a uint32 stream id; OSG1 trails a
      // uint32 min-precision field. The shared fields sit at the same
      // relative offsets once the leading stream id is skipped.
      const size_t elemSize     = (fourCc == kFourCcOsgn) ? 24 : (fourCc == kFourCcOsg5 ? 28 : 32);
      const size_t nameFieldOff = (fourCc == kFourCcOsgn) ? 0 : 4;
      const size_t tableStart   = dataStart + 8;
      if (8 + size_t(elementCount) * elemSize > chunkSize)
        return false;

      bool found = false;
      int bestScore = 0;
      uint32_t bestIndex = 0;
      std::string bestName;

      for (uint32_t e = 0; e < elementCount; ++e) {
        const size_t el = tableStart + size_t(e) * elemSize;

        if (fourCc != kFourCcOsgn && readU32(el) != 0)
          continue; // only stream 0 is capturable here

        const uint32_t nameOffset    = readU32(el + nameFieldOff + 0);
        const uint32_t semanticIdx   = readU32(el + nameFieldOff + 4);
        const uint32_t systemValue   = readU32(el + nameFieldOff + 8);
        const uint32_t componentType = readU32(el + nameFieldOff + 12);
        const uint8_t  mask          = bytes[el + nameFieldOff + 20];

        if (systemValue != 0)   // skip SV_Position & friends
          continue;
        if (componentType != 3) // D3D_REGISTER_COMPONENT_FLOAT32
          continue;
        if ((mask & 0x3u) != 0x3u) // needs at least .xy written
          continue;

        if (size_t(nameOffset) >= chunkSize)
          continue;
        const char* name = reinterpret_cast<const char*>(bytes + dataStart + nameOffset);
        const size_t maxLen = chunkSize - nameOffset;
        size_t n = 0;
        while (n < maxLen && name[n] != '\0')
          ++n;
        if (n == 0 || n >= maxLen || n > 63)
          continue;

        std::string upper(name, n);
        for (auto& c : upper)
          c = char(::toupper(static_cast<unsigned char>(c)));

        if (outSemantics != nullptr) {
          D3D11TexcoordSemantic semantic;
          semantic.semanticName.assign(name, n);
          semantic.semanticIndex = semanticIdx;
          outSemantics->push_back(std::move(semantic));
        }

        int score = 0;
        if (upper.find("TEXCOORD") != std::string::npos)
          score = 3;
        else if (upper.compare(0, 2, "UV") == 0)
          score = 2;
        else if (upper.find("TEX") != std::string::npos)
          score = 1;
        if (score == 0)
          continue;

        // Prefer the strongest name match, then the lowest semantic index
        // (TEXCOORD0/UV0 is the diffuse UV set in every engine convention).
        if (!found || score > bestScore || (score == bestScore && semanticIdx < bestIndex)) {
          found = true;
          bestScore = score;
          bestIndex = semanticIdx;
          bestName.assign(name, n);
        }
      }

      if (found) {
        outName = std::move(bestName);
        outIndex = bestIndex;
        return true;
      }
      return false; // signature present, nothing texcoord-like in it
    }

    return false;
  }

  // Follow pixel-shader dataflow from texture sample coordinates back to the
  // declared input register. Optimizing HLSL compilers routinely place the
  // diffuse UV in TEXCOORD1/2 while TEXCOORD0 carries fog, lighting, or world
  // data, so selecting the lowest VS output cannot be correct across engines.
  // The analysis is deliberately conservative: a temporary carries the union
  // of input registers that feed it, and the most frequently sampled matching
  // float input wins for each texture resource slot.
  // Far-plane geometry (sky domes, skyboxes, sun/moon/cloud layers) is pushed
  // to depth 1 by writing SV_Position.z from the same value as .w (the common
  // `pos.xyww` idiom). Detect it statically: plain movs into the position
  // output whose z and w come from the same source register component.
  // Camera-relative world transform, proven from the shader's data flow
  // (reverse-engineered from Fallout 4's world shaders, the Creation Engine
  // pattern):
  //   row_i      = ( cbW[R+i].xyz, cbW[R+i].w - cbC[E].{x,y,z}[i] )   i = 0..2
  //   world.i    = dp4(row_i, (POSITION.xyz, 1))
  //   SV_Position.c = dp4(cbC[V+c], world)                              c = 0..3
  // cbW[R..R+2] then hold the object's ABSOLUTE world matrix and cbC[E] the
  // eye the engine subtracted. Optional: the UV scale/offset applied to
  // TEXCOORD (mad uv, v.xy, cbU[k].zw, cbU[k].xy).
  static D3D11CameraRelativeWorldBinding parseCameraRelativeWorldBinding(const DxbcModule& module) {
    D3D11CameraRelativeWorldBinding result;
    const Rc<DxbcIsgn> isgn = module.isgn();
    const Rc<DxbcIsgn> osgn = module.osgn();
    if (isgn == nullptr || osgn == nullptr)
      return result;
    int32_t posIn = -1, posOut = -1, texIn = -1;
    for (const DxbcSgnEntry& e : *isgn) {
      std::string s = e.semanticName;
      std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::toupper(c)); });
      if (s == "POSITION" && e.semanticIndex == 0) posIn = int32_t(e.registerId);
      if (s == "TEXCOORD" && e.semanticIndex == 0) texIn = int32_t(e.registerId);
      if (s == "BLENDINDICES" || s == "BLENDWEIGHT") return result;  // skinned
    }
    for (const DxbcSgnEntry& e : *osgn) {
      if (e.systemValue == DxbcSystemValue::Position) { posOut = int32_t(e.registerId); break; }
    }
    if (posIn < 0 || posOut < 0)
      return result;

    // Dynamically indexed reads (cb[s][r + index], e.g. CRYENGINE SPIData[800])
    // are not a fixed register and must never be taken for one.
    auto isCb = [](const DxbcRegister& r) {
      return r.type == DxbcOperandType::ConstantBuffer && r.idxDim >= 2
          && r.idx[0].relReg == nullptr && r.idx[1].relReg == nullptr;
    };
    auto cbSlot = [](const DxbcRegister& r) { return uint32_t(r.idx[0].offset); };
    auto cbReg = [](const DxbcRegister& r) { return uint32_t(r.idx[1].offset); };
    auto singleComp = [](const DxbcRegMask& m) -> int32_t {
      int32_t found = -1;
      for (uint32_t c = 0; c < 4; ++c) {
        if (m[c]) { if (found >= 0) return -1; found = int32_t(c); }
      }
      return found;
    };

    struct TempState {
      std::array<int8_t, 4> posComp = { -1, -1, -1, -1 };  // component of POSITION, 3 = literal one
      bool rowXyzValid = false; uint32_t rowSlot = 0, rowReg = 0;   // mov t.xyz, cbW[R].xyz
      bool eyeWValid = false; uint32_t eyeSlot = 0, eyeReg = 0, eyeComp = 0, eyeRowSlot = 0, eyeRowReg = 0;
      bool worldValid = false;                                        // t holds the world position
      std::array<int32_t, 3> worldRows = { -1, -1, -1 };
      // Second form (Dunia/Disrupt/Frostbite crViewProj): absolute world rows
      // dp4'd straight from the cbuffer, then `add t.xyz, t.xyz, -eye.xyz`.
      std::array<int32_t, 3> absRows = { -1, -1, -1 };
      uint32_t absSlot = 0;
      bool relValid = false; uint32_t relSlot = 0, relRow0 = 0, relEyeSlot = 0, relEyeReg = 0;
    };
    std::unordered_map<uint32_t, TempState> temps;

    std::array<int32_t, 4> vpRows = { -1, -1, -1, -1 };
    uint32_t vpSlot = UINT32_MAX, worldTempUsed = UINT32_MAX;
    int32_t vpWorldRow = -1;

    DxbcCodeSlice slice = module.instructionSlice();
    DxbcDecodeContext decoder;
    while (!slice.atEnd()) {
      decoder.decodeInstruction(slice);
      const DxbcShaderInstruction& ins = decoder.getInstruction();
      if (ins.dstCount < 1)
        continue;
      const DxbcRegister& dst = ins.dst[0];

      if (dst.type == DxbcOperandType::Temp && dst.idxDim >= 1) {
        TempState& t = temps[uint32_t(dst.idx[0].offset)];
        if (ins.op == DxbcOpcode::Mov && ins.srcCount == 1) {
          const DxbcRegister& s = ins.src[0];
          for (uint32_t c = 0; c < 4; ++c) {
            if (!dst.mask[c]) continue;
            t.posComp[c] = -1;
            if (s.type == DxbcOperandType::Input && s.idxDim >= 1 && int32_t(s.idx[0].offset) == posIn
             && s.modifiers.isClear() && s.swizzle[c] < 3)
              t.posComp[c] = int8_t(s.swizzle[c]);
            if (s.type == DxbcOperandType::Imm32) {
              float f; std::memcpy(&f, &s.imm.u32_4[s.componentCount == DxbcComponentCount::Component1 ? 0 : c], sizeof(f));
              if (f == 1.0f) t.posComp[c] = 3;
            }
          }
          if (dst.mask[0] && dst.mask[1] && dst.mask[2] && isCb(s)
           && s.swizzle[0] == 0 && s.swizzle[1] == 1 && s.swizzle[2] == 2) {
            t.rowXyzValid = true; t.rowSlot = cbSlot(s); t.rowReg = cbReg(s);
          } else if (dst.mask[0] || dst.mask[1] || dst.mask[2]) {
            t.rowXyzValid = false;
          }
          if (dst.mask[3]) t.eyeWValid = false;
          t.worldValid = false;
        } else if (ins.op == DxbcOpcode::Add && ins.srcCount == 2 && singleComp(dst.mask) == 3) {
          // w = cbW[R].w - cbC[E].c (either operand order)
          t.eyeWValid = false;
          for (uint32_t a = 0; a < 2; ++a) {
            const DxbcRegister& eye = ins.src[a];
            const DxbcRegister& row = ins.src[1 - a];
            if (isCb(eye) && isCb(row) && eye.modifiers.test(DxbcRegModifier::Neg)
             && row.modifiers.isClear() && row.swizzle[3] == 3 && eye.swizzle[3] < 3) {
              ++result.debugEyeAdds;
              t.eyeWValid = true;
              t.eyeSlot = cbSlot(eye); t.eyeReg = cbReg(eye); t.eyeComp = eye.swizzle[3];
              t.eyeRowSlot = cbSlot(row); t.eyeRowReg = cbReg(row);
            }
          }
          t.posComp[3] = -1;
          t.worldValid = false;
        } else if (ins.op == DxbcOpcode::Add && ins.srcCount == 2 && dst.mask[0] && dst.mask[1] && dst.mask[2]
                && !dst.mask[3]) {
          // t.xyz = absWorld.xyz - eye.xyz (either operand order)
          bool matched = false;
          for (uint32_t a = 0; a < 2 && !matched; ++a) {
            const DxbcRegister& w = ins.src[a];
            const DxbcRegister& eye = ins.src[1 - a];
            if (w.type != DxbcOperandType::Temp || !w.modifiers.isClear()
             || w.swizzle[0] != 0 || w.swizzle[1] != 1 || w.swizzle[2] != 2)
              continue;
            if (!isCb(eye) || !eye.modifiers.test(DxbcRegModifier::Neg)
             || eye.swizzle[0] != 0 || eye.swizzle[1] != 1 || eye.swizzle[2] != 2)
              continue;
            const TempState src = temps[uint32_t(w.idx[0].offset)];
            if (src.absRows[0] >= 0 && src.absRows[1] == src.absRows[0] + 1 && src.absRows[2] == src.absRows[0] + 2) {
              ++result.debugEyeAdds;
              const std::array<int8_t, 4> keepPos = t.posComp;
              t = TempState();
              t.posComp = keepPos;
              t.posComp[0] = t.posComp[1] = t.posComp[2] = -1;
              t.relValid = true;
              t.relSlot = src.absSlot;
              t.relRow0 = uint32_t(src.absRows[0]);
              t.relEyeSlot = cbSlot(eye);
              t.relEyeReg = cbReg(eye);
              matched = true;
            }
          }
          if (!matched) {
            for (uint32_t c = 0; c < 3; ++c) { t.posComp[c] = -1; t.worldRows[c] = -1; t.absRows[c] = -1; }
            t.worldValid = false;
            t.rowXyzValid = false;
            t.relValid = false;
          }
        } else if (ins.op == DxbcOpcode::Dp4 && ins.srcCount == 2) {
          const int32_t k = singleComp(dst.mask);
          // Absolute world row: dp4 t.k, cbW[R+k], (POSITION.xyz, 1).
          bool isAbsRow = false;
          for (uint32_t a = 0; a < 2 && k >= 0 && k < 3; ++a) {
            const DxbcRegister& m = ins.src[a];
            const DxbcRegister& v = ins.src[1 - a];
            if (!isCb(m) || !m.modifiers.isClear() || m.swizzle != DxbcRegSwizzle(0, 1, 2, 3))
              continue;
            bool posOk = false;
            if (v.type == DxbcOperandType::Temp && v.modifiers.isClear() && v.swizzle == DxbcRegSwizzle(0, 1, 2, 3)) {
              const TempState& pos = temps[uint32_t(v.idx[0].offset)];
              posOk = pos.posComp[0] == 0 && pos.posComp[1] == 1 && pos.posComp[2] == 2 && pos.posComp[3] == 3;
            }
            if (posOk) {
              isAbsRow = true;
              t.absRows[k] = int32_t(cbReg(m));
              t.absSlot = cbSlot(m);
            }
          }
          if (!isAbsRow && k >= 0 && k < 3)
            t.absRows[k] = -1;
          if (k >= 0 && k < 3)
            t.relValid = false;
          bool isWorldRow = false;
          for (uint32_t a = 0; a < 2 && k >= 0 && k < 3; ++a) {
            if (ins.src[a].type != DxbcOperandType::Temp || ins.src[1 - a].type != DxbcOperandType::Temp)
              continue;
            const TempState& row = temps[uint32_t(ins.src[a].idx[0].offset)];
            const TempState& pos = temps[uint32_t(ins.src[1 - a].idx[0].offset)];
            const bool posOk = pos.posComp[0] == 0 && pos.posComp[1] == 1 && pos.posComp[2] == 2 && pos.posComp[3] == 3;
            const bool rowOk = row.rowXyzValid && row.eyeWValid && row.eyeRowSlot == row.rowSlot
              && row.eyeRowReg == row.rowReg && int32_t(row.eyeComp) == k;
            if (posOk && rowOk) {
              ++result.debugWorldRows;
              isWorldRow = true;
              t.worldRows[k] = int32_t(row.rowReg);
              result.worldSlot = row.rowSlot;
              result.cameraSlot = row.eyeSlot;
              result.eyeRegister = row.eyeReg;
            }
          }
          if (k >= 0)
            t.posComp[k] = -1;
          if (!isWorldRow && k >= 0 && k < 3)
            t.worldRows[k] = -1;
          t.worldValid = t.worldRows[0] >= 0 && t.worldRows[1] == t.worldRows[0] + 1 && t.worldRows[2] == t.worldRows[0] + 2;
          if (t.worldValid)
            ++result.debugWorldComplete;
        } else {
          for (uint32_t c = 0; c < 4; ++c)
            if (dst.mask[c]) { t.posComp[c] = -1; if (c < 3) t.worldRows[c] = -1; }
          t.worldValid = false;
          if (dst.mask[0] || dst.mask[1] || dst.mask[2]) t.rowXyzValid = false;
          if (dst.mask[3]) t.eyeWValid = false;
        }
      } else if (dst.type == DxbcOperandType::Output && dst.idxDim >= 1 && int32_t(dst.idx[0].offset) == posOut) {
        const int32_t k = singleComp(dst.mask);
        if (ins.op == DxbcOpcode::Dp4 && ins.srcCount == 2 && k >= 0) {
          for (uint32_t a = 0; a < 2; ++a) {
            const DxbcRegister& m = ins.src[a];
            const DxbcRegister& v = ins.src[1 - a];
            if (isCb(m) && m.modifiers.isClear() && v.type == DxbcOperandType::Temp
             && temps[uint32_t(v.idx[0].offset)].worldValid) {
              vpRows[k] = int32_t(cbReg(m));
              ++result.debugVpRows;
              vpSlot = cbSlot(m);
              worldTempUsed = uint32_t(v.idx[0].offset);
              // Snapshot now: engines reuse the temp afterwards (Fallout 4
              // rebuilds the previous-frame position in the same register).
              vpWorldRow = temps[worldTempUsed].worldRows[0];
            } else if (isCb(m) && m.modifiers.isClear() && v.type == DxbcOperandType::Temp
                    && temps[uint32_t(v.idx[0].offset)].relValid
                    && temps[uint32_t(v.idx[0].offset)].posComp[3] == 3) {
              // Camera-relative position (world - eye, w = 1) times ViewProj.
              const TempState& rel = temps[uint32_t(v.idx[0].offset)];
              vpRows[k] = int32_t(cbReg(m));
              ++result.debugVpRows;
              vpSlot = cbSlot(m);
              worldTempUsed = uint32_t(v.idx[0].offset);
              vpWorldRow = int32_t(rel.relRow0);
              result.worldSlot = rel.relSlot;
              result.cameraSlot = rel.relEyeSlot;
              result.eyeRegister = rel.relEyeReg;
            }
          }
        }
      }

      // UV scale/offset: mad uv.xy, TEXCOORD0.xy, cbU[k].zw, cbU[k].xy
      if (ins.op == DxbcOpcode::Mad && ins.srcCount == 3 && texIn >= 0 && !result.hasUvTransform) {
        const DxbcRegister& v = ins.src[0];
        const DxbcRegister& s = ins.src[1];
        const DxbcRegister& o = ins.src[2];
        if (v.type == DxbcOperandType::Input && int32_t(v.idx[0].offset) == texIn
         && v.swizzle[0] == 0 && v.swizzle[1] == 1
         && isCb(s) && isCb(o) && cbSlot(s) == cbSlot(o) && cbReg(s) == cbReg(o)
         && s.swizzle[0] == 2 && s.swizzle[1] == 3 && o.swizzle[0] == 0 && o.swizzle[1] == 1) {
          result.hasUvTransform = true;
          result.uvSlot = cbSlot(s);
          result.uvRegister = cbReg(s);
        }
      }
    }

    // The eye and the ViewProj may live in different cbuffers (CRYENGINE 3:
    // eye in PER_FRAME b2, matrices elsewhere); both locations are recorded.
    const bool vpOk = vpRows[0] >= 0 && vpRows[1] == vpRows[0] + 1 && vpRows[2] == vpRows[0] + 2
                   && vpRows[3] == vpRows[0] + 3 && vpSlot != UINT32_MAX;
    if (worldTempUsed != UINT32_MAX && vpWorldRow >= 0 && vpOk) {
      result.valid = true;
      result.worldRegister = uint32_t(vpWorldRow);
      result.viewProjRegister = uint32_t(vpRows[0]);
      result.viewProjSlot = vpSlot;
    }
    return result;
  }

  // Vertex shaders that evaluate sin/cos animate their vertices over time:
  // wind sway on foliage and grass (Fallout 4: sincos over per-object wind
  // parameters feeding SV_Position), water waves, flags, cloth.
  // Vertex texture fetch moves vertices too: CRYENGINE vegetation bending
  // samples a wind grid, Frostbite terrain and tessellated water displace by
  // height maps. Their positions are not the proven matrix transform of the
  // input, so exact-world placement must not be used for them.
  static bool parseDxbcUsesTrigonometry(const DxbcModule& module) {
    DxbcCodeSlice slice = module.instructionSlice();
    DxbcDecodeContext decoder;
    while (!slice.atEnd()) {
      decoder.decodeInstruction(slice);
      const DxbcShaderInstruction& ins = decoder.getInstruction();
      if (ins.op == DxbcOpcode::SinCos
       || ins.opClass == DxbcInstClass::TextureSample
       || ins.opClass == DxbcInstClass::TextureGather)
        return true;
    }
    return false;
  }

  static bool parseDxbcWritesPositionAtFarPlane(const DxbcModule& module, std::string& outSummary,
                                                bool& outConstantW) {
    outConstantW = false;
    const Rc<DxbcIsgn> outputSignature = module.osgn();
    if (outputSignature == nullptr)
      return false;
    int32_t positionRegister = -1;
    for (const DxbcSgnEntry& entry : *outputSignature) {
      if (entry.systemValue == DxbcSystemValue::Position) {
        positionRegister = int32_t(entry.registerId);
        break;
      }
    }
    if (positionRegister < 0)
      return false;

    struct ComponentSource {
      bool     known = false;
      uint32_t type = 0;
      int32_t  reg = -1;
      uint32_t component = 0;
    };
    ComponentSource zSource, wSource;

    DxbcCodeSlice slice = module.instructionSlice();
    DxbcDecodeContext decoder;
    while (!slice.atEnd()) {
      decoder.decodeInstruction(slice);
      const DxbcShaderInstruction& ins = decoder.getInstruction();
      for (uint32_t d = 0; d < ins.dstCount; ++d) {
        const DxbcRegister& dst = ins.dst[d];
        if (dst.type != DxbcOperandType::Output || dst.idxDim == 0
         || dst.idx[0].offset != positionRegister)
          continue;
        if ((dst.mask[2] || dst.mask[3]) && outSummary.size() < 400) {
          outSummary += str::format(" op", uint32_t(ins.op), " mask",
            dst.mask[0] ? "x" : "", dst.mask[1] ? "y" : "", dst.mask[2] ? "z" : "", dst.mask[3] ? "w" : "");
          for (uint32_t si = 0; si < ins.srcCount; ++si) {
            const DxbcRegister& src = ins.src[si];
            outSummary += str::format(" s", uint32_t(src.type), ":",
              src.idxDim > 0 ? src.idx[0].offset : -1, ".",
              uint32_t(src.swizzle[0]), uint32_t(src.swizzle[1]), uint32_t(src.swizzle[2]), uint32_t(src.swizzle[3]));
            if (src.type == DxbcOperandType::Imm32)
{
              float f[4];
              std::memcpy(f, src.imm.u32_4, sizeof(f));
              outSummary += str::format("=", f[0], ",", f[1], ",", f[2], ",", f[3]);
            }
          }
          outSummary += ";";
        }
        const bool plainMov = ins.op == DxbcOpcode::Mov && ins.srcCount == 1u
          && ins.src[0].modifiers.isClear() && ins.src[0].idxDim > 0
          && !ins.modifiers.saturate;
        // SV_Position.w taken from an immediate: the vertices are already in
        // screen space (post-process triangles, UI quads). The last write wins.
        if (dst.mask[3]) {
          outConstantW = ins.op == DxbcOpcode::Mov && ins.srcCount == 1u
            && ins.src[0].type == DxbcOperandType::Imm32;
        }
        for (uint32_t c = 2; c < 4; ++c) {
          if (!dst.mask[c])
            continue;
          ComponentSource& out = c == 2 ? zSource : wSource;
          out = ComponentSource();
          if (plainMov) {
            out.known = true;
            out.type = uint32_t(ins.src[0].type);
            out.reg = ins.src[0].idx[0].offset;
            out.component = ins.src[0].swizzle[c];
          }
        }
      }
    }
    return zSource.known && wSource.known
      && zSource.type == wSource.type
      && zSource.reg == wSource.reg
      && zSource.component == wSource.component;
  }

  // Texture decodes proven from the pixel shader's dataflow (METHODS.md,
  // Materials). A normal map is whatever the shader unpacks with
  // "sample * 2 - 1" (mad x, 2, -1, or mul 2 then add -1); the channel order
  // of that unpack gives its encoding. A smoothness map is a sampled channel
  // (optionally scaled by a constant) that the shader turns into roughness
  // with "1 - x". Engine-independent: works on stripped shaders.
  static void parseDxbcTextureDecodes(
    const DxbcModule& module,
    std::array<D3D11CommonShader::TextureDecode,
      D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT>& out) {
    // Per temp component: which texture channel it carries.
    // stage 0 raw sample, 1 sample * 2 (awaiting -1), 2 scaled by a constant.
    struct Origin { int16_t slot = -1; int8_t channel = -1; uint8_t stage = 0; };
    std::unordered_map<uint32_t, std::array<Origin, 4>> temps;

    auto originOf = [&](const DxbcRegister& r, uint32_t component) -> Origin {
      if (r.type != DxbcOperandType::Temp || r.idxDim == 0 || r.idx[0].relReg != nullptr)
        return {};
      const auto it = temps.find(uint32_t(r.idx[0].offset));
      return it != temps.end() ? it->second[r.swizzle[component]] : Origin();
    };
    auto immIs = [](const DxbcRegister& r, uint32_t component, float value) {
      if (r.type != DxbcOperandType::Imm32 || !r.modifiers.isClear())
        return false;
      const uint32_t bits = r.componentCount == DxbcComponentCount::Component4
        ? r.imm.u32_4[component] : r.imm.u32_1;
      float f;
      std::memcpy(&f, &bits, sizeof(f));
      return f == value;
    };
    auto isConstant = [](const DxbcRegister& r) {
      return r.type == DxbcOperandType::ConstantBuffer || r.type == DxbcOperandType::Imm32
          || r.type == DxbcOperandType::ImmediateConstantBuffer;
    };
    // Normal unpacks per slot: texture channel feeding each output component.
    std::array<std::array<int8_t, 4>, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT> unpack;
    for (auto& u : unpack)
      u.fill(-1);

    DxbcCodeSlice slice = module.instructionSlice();
    DxbcDecodeContext decoder;
    while (!slice.atEnd()) {
      decoder.decodeInstruction(slice);
      const DxbcShaderInstruction& ins = decoder.getInstruction();
      if (ins.dstCount == 0 || ins.dst[0].type != DxbcOperandType::Temp || ins.dst[0].idxDim == 0)
        continue;
      const DxbcRegister& dst = ins.dst[0];
      std::array<Origin, 4> written = temps[uint32_t(dst.idx[0].offset)];

      const bool sample = (ins.opClass == DxbcInstClass::TextureSample
                        || ins.opClass == DxbcInstClass::TextureGather)
        && ins.srcCount >= 2u && ins.src[1].type == DxbcOperandType::Resource
        && ins.src[1].idxDim > 0 && ins.src[1].idx[0].relReg == nullptr
        && uint32_t(ins.src[1].idx[0].offset) < out.size();

      for (uint32_t c = 0; c < 4u; ++c) {
        if (!dst.mask[c])
          continue;
        Origin result;
        if (sample) {
          result = { int16_t(ins.src[1].idx[0].offset), int8_t(ins.src[1].swizzle[c]), 0 };
        } else if (ins.op == DxbcOpcode::Mov && ins.srcCount == 1 && ins.src[0].modifiers.isClear()) {
          result = originOf(ins.src[0], c);
        } else if (ins.op == DxbcOpcode::Mad && ins.srcCount == 3) {
          // tex * 2 - 1: the unpack itself.
          for (uint32_t a = 0; a < 2u; ++a) {
            const Origin o = originOf(ins.src[a], c);
            if (o.slot >= 0 && o.stage == 0 && ins.src[a].modifiers.isClear()
             && immIs(ins.src[1u - a], c, 2.0f) && immIs(ins.src[2], c, -1.0f)
             && unpack[size_t(o.slot)][c] < 0)
              unpack[size_t(o.slot)][c] = o.channel;
          }
        } else if (ins.op == DxbcOpcode::Mul && ins.srcCount == 2) {
          for (uint32_t a = 0; a < 2u; ++a) {
            const Origin o = originOf(ins.src[a], c);
            if (o.slot < 0 || o.stage != 0 || !ins.src[a].modifiers.isClear())
              continue;
            if (immIs(ins.src[1u - a], c, 2.0f))
              result = { o.slot, o.channel, 1 };
            else if (isConstant(ins.src[1u - a]))
              result = { o.slot, o.channel, 2 };
          }
        } else if (ins.op == DxbcOpcode::Add && ins.srcCount == 2) {
          for (uint32_t a = 0; a < 2u; ++a) {
            const DxbcRegister& v = ins.src[a];
            const Origin o = originOf(v, c);
            if (o.slot < 0)
              continue;
            // (tex * 2) - 1
            if (o.stage == 1 && v.modifiers.isClear() && immIs(ins.src[1u - a], c, -1.0f)
             && unpack[size_t(o.slot)][c] < 0)
              unpack[size_t(o.slot)][c] = o.channel;
            // 1 - smoothness
            if (o.stage != 1 && v.modifiers.test(DxbcRegModifier::Neg) && !v.modifiers.test(DxbcRegModifier::Abs)
             && immIs(ins.src[1u - a], c, 1.0f) && out[size_t(o.slot)].smoothnessChannel < 0)
              out[size_t(o.slot)].smoothnessChannel = o.channel;
          }
        }
        written[c] = result;
      }
      temps[uint32_t(dst.idx[0].offset)] = written;
    }

    for (size_t slot = 0; slot < out.size(); ++slot) {
      const auto& u = unpack[slot];
      if (u[0] < 0 || u[1] < 0)
        continue;
      if (u[0] == 3 && u[1] == 1)
        out[slot].normalEncoding = 4;          // DXT5nm: X in alpha, Y in green
      else if (u[0] == 1 && u[1] == 0)
        out[slot].normalEncoding = 5;          // X and Y swapped
      else if (u[0] == 0 && u[1] == 1)
        out[slot].normalEncoding = u[2] == 2 ? 2 : 3;  // RGB, or XY with z rebuilt
    }
  }

  // The pixel-shader input that multiplies a texture sample is the vertex
  // colour (tint, baked lighting): the same rule as the DX12 / Vulkan SPIR-V
  // analysis (d3d11_vk_spirv.cpp). Per temp component the pass tracks whether
  // it carries a sample and which single input register it comes from, and
  // which channel of each; a mul / mad votes for the input only when it pairs
  // the sample's and the input's channels one to one (r*r, g*g, b*b) on at
  // least three components. A tint is that; a tangent-space transform is not:
  // it broadcasts one channel of the sampled normal over an interpolated
  // basis vector (Unreal's TangentToWorld in TEXCOORD10 / 11), which read as a
  // colour paints surfaces blue. The most-voted float input with at least
  // three components wins, COLOR-named inputs breaking ties.
  static void parseDxbcVertexColorInput(
    const DxbcModule&                 module,
    D3D11SampledTexcoordSemantic&     out,
    uint32_t&                         outComponents) {
    out = D3D11SampledTexcoordSemantic();
    outComponents = 0;

    const Rc<DxbcIsgn> isgn = module.isgn();
    if (isgn == nullptr)
      return;

    // static: used by a capture-less lambda. Taint spells kNone as -1: MSVC
    // rejects a local constant in a local struct's member initializer.
    static constexpr int32_t kNone = -1, kMixed = -2;
    // sampleChannel / inputChannel: the source channel this component holds
    // (-1 none, -2 mixed).
    struct Taint { bool sample = false; int32_t input = -1; int32_t sampleChannel = -1; int32_t inputChannel = -1; };
    std::unordered_map<uint32_t, std::array<Taint, 4>> temps;
    std::unordered_map<int32_t, uint32_t> votes;

    auto taintOf = [&](const DxbcRegister& r, uint32_t component) -> Taint {
      if (r.idxDim == 0 || r.idx[0].relReg != nullptr || r.idx[0].offset < 0)
        return {};
      if (r.type == DxbcOperandType::Input)
        return { false, int32_t(r.idx[0].offset), -1, int32_t(r.swizzle[component]) };
      if (r.type == DxbcOperandType::Temp) {
        const auto it = temps.find(uint32_t(r.idx[0].offset));
        return it != temps.end() ? it->second[r.swizzle[component]] : Taint();
      }
      return {};
    };
    auto merge = [](int32_t a, int32_t b) {
      if (a == kNone) return b;
      if (b == kNone || a == b) return a;
      return kMixed;
    };

    DxbcCodeSlice slice = module.instructionSlice();
    DxbcDecodeContext decoder;
    while (!slice.atEnd()) {
      decoder.decodeInstruction(slice);
      const DxbcShaderInstruction& ins = decoder.getInstruction();
      if (ins.dstCount == 0 || ins.dst[0].type != DxbcOperandType::Temp || ins.dst[0].idxDim == 0)
        continue;

      const DxbcRegister& dst = ins.dst[0];
      std::array<Taint, 4> written = temps[uint32_t(dst.idx[0].offset)];
      const bool sample = ins.opClass == DxbcInstClass::TextureSample || ins.opClass == DxbcInstClass::TextureGather;
      const bool product = (ins.op == DxbcOpcode::Mul || ins.op == DxbcOpcode::Mad) && ins.srcCount >= 2u;

      // Channel-matched sample * input pairs of this instruction, per input.
      std::unordered_map<int32_t, uint32_t> matched;

      for (uint32_t c = 0; c < 4u; ++c) {
        if (!dst.mask[c])
          continue;

        Taint result;

        if (sample) {
          result.sample = true;
          // The texel channel written to this component: the resource
          // operand's swizzle.
          result.sampleChannel = ins.srcCount >= 2u ? int32_t(ins.src[1].swizzle[c]) : int32_t(c);
        } else if (product) {
          const Taint a = taintOf(ins.src[0], c);
          const Taint b = taintOf(ins.src[1], c);

          // sample * input (either order), channel for channel: a colour.
          if (a.sample && !b.sample && b.input >= 0 && a.sampleChannel >= 0 && a.sampleChannel == b.inputChannel)
            matched[b.input]++;
          else if (b.sample && !a.sample && a.input >= 0 && b.sampleChannel >= 0 && b.sampleChannel == a.inputChannel)
            matched[a.input]++;

          result.sample = a.sample || b.sample;
          result.sampleChannel = a.sample && b.sample ? merge(a.sampleChannel, b.sampleChannel)
                               : a.sample ? a.sampleChannel : b.sample ? b.sampleChannel : kNone;
          result.input = result.sample ? kNone : merge(a.input, b.input);
          result.inputChannel = result.sample ? kNone : merge(a.inputChannel, b.inputChannel);
        } else {
          // Moves, saturates, adds and the like keep both kinds of origin.
          for (uint32_t s = 0; s < ins.srcCount; ++s) {
            const Taint t = taintOf(ins.src[s], c);
            result.sample |= t.sample;
            if (t.sample)
              result.sampleChannel = merge(result.sampleChannel, t.sampleChannel);
            result.input = merge(result.input, t.input);
            result.inputChannel = merge(result.inputChannel, t.inputChannel);
          }
        }

        written[c] = result;
      }

      for (const auto& m : matched) {
        if (m.second >= 3u)
          votes[m.first]++;
      }

      temps[uint32_t(dst.idx[0].offset)] = written;
    }

    int bestScore = -1;

    for (const auto& v : votes) {
      const DxbcSgnEntry* entry = isgn->findByRegister(uint32_t(v.first));
      if (entry == nullptr || entry->systemValue != DxbcSystemValue::None
       || entry->componentType != DxbcScalarType::Float32
       || !entry->componentMask[0] || !entry->componentMask[1] || !entry->componentMask[2])
        continue;

      std::string upper = entry->semanticName;
      for (auto& ch : upper)
        ch = char(::toupper(static_cast<unsigned char>(ch)));

      const int score = int(v.second) * 4 + (upper.compare(0, 5, "COLOR") == 0 ? 2 : 0);
      if (score <= bestScore)
        continue;

      bestScore = score;
      out.semanticName = entry->semanticName;
      out.semanticIndex = entry->semanticIndex;
      out.componentIndex = 0;
      out.valid = true;
      outComponents = entry->componentMask[3] ? 4u : 3u;
    }
  }

  // Legacy cubemap reflections (Bethesda BSLightingShader / BSEffectShader
  // envmaps, and any engine that adds "cube sample * strength * mask" to its
  // colour): which TextureCube is sampled, the constant that scales the
  // sample and up to two 2D texture channels that mask it, from the PS
  // dataflow. Also the Creation deferred G-buffer envmap word, written as
  // cb[N].x / 255 into a render target (fo4-decomp MODLOG.md: the pre-pass
  // never samples the cube; a fullscreen composite adds it).
  static void parseDxbcEnvmapReflection(
    const DxbcModule&                     module,
    D3D11CommonShader::EnvmapReflection&  out) {
    out = D3D11CommonShader::EnvmapReflection();

    std::array<bool, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT> cube = {};

    // Per temp component: the factors multiplied into the value so far.
    struct Term {
      bool    cube = false;
      int8_t  cbSlot = -1;
      uint16_t cbReg = 0;
      uint8_t cbComponent = 0;
      int8_t  texSlot[2] = { -1, -1 };
      int8_t  texChannel[2] = { -1, -1 };
      // A raw 2D sample (not yet multiplied by anything).
      int8_t  sampleSlot = -1;
      int8_t  sampleChannel = -1;
    };
    std::unordered_map<uint32_t, std::array<Term, 4>> temps;

    auto termOf = [&](const DxbcRegister& r, uint32_t component) -> Term {
      Term t;
      if (r.idxDim == 0)
        return t;
      if (r.type == DxbcOperandType::Temp && r.idx[0].relReg == nullptr) {
        const auto it = temps.find(uint32_t(r.idx[0].offset));
        return it != temps.end() ? it->second[r.swizzle[component]] : t;
      }
      if (r.type == DxbcOperandType::ConstantBuffer && r.idxDim >= 2
       && r.idx[0].relReg == nullptr && r.idx[1].relReg == nullptr
       && r.idx[0].offset >= 0 && r.idx[0].offset < 14 && r.idx[1].offset >= 0 && r.idx[1].offset < 4096) {
        t.cbSlot = int8_t(r.idx[0].offset);
        t.cbReg = uint16_t(r.idx[1].offset);
        t.cbComponent = uint8_t(r.swizzle[component]);
      }
      return t;
    };

    auto addTex = [](Term& t, int8_t slot, int8_t channel) {
      for (uint32_t i = 0; i < 2; ++i) {
        if (t.texSlot[i] == slot && t.texChannel[i] == channel)
          return;
        if (t.texSlot[i] < 0) {
          t.texSlot[i] = slot;
          t.texChannel[i] = channel;
          return;
        }
      }
    };

    // Product of two terms: the union of their factors.
    auto product = [&](const Term& a, const Term& b) {
      Term r;
      r.cube = a.cube || b.cube;
      const Term& cbSrc = a.cbSlot >= 0 ? a : b;
      r.cbSlot = cbSrc.cbSlot;
      r.cbReg = cbSrc.cbReg;
      r.cbComponent = cbSrc.cbComponent;
      for (const Term* t : { &a, &b }) {
        for (uint32_t i = 0; i < 2; ++i) {
          if (t->texSlot[i] >= 0)
            addTex(r, t->texSlot[i], t->texChannel[i]);
        }
        if (t->sampleSlot >= 0)
          addTex(r, t->sampleSlot, t->sampleChannel);
      }
      return r;
    };

    int best = 0;

    DxbcCodeSlice slice = module.instructionSlice();
    DxbcDecodeContext decoder;
    while (!slice.atEnd()) {
      decoder.decodeInstruction(slice);
      const DxbcShaderInstruction& ins = decoder.getInstruction();

      if (ins.op == DxbcOpcode::DclResource && ins.dstCount > 0 && ins.dst[0].idxDim > 0) {
        const uint32_t slot = uint32_t(ins.dst[0].idx[0].offset);
        const DxbcResourceDim dim = ins.controls.resourceDim();
        if (slot < cube.size())
          cube[slot] = dim == DxbcResourceDim::TextureCube || dim == DxbcResourceDim::TextureCubeArr;
        continue;
      }

      if (ins.dstCount == 0 || ins.dst[0].idxDim == 0)
        continue;

      const DxbcRegister& dst = ins.dst[0];

      // Creation deferred envmap word: output.c = cb[N].x * (1 / 255).
      if (dst.type == DxbcOperandType::Output && ins.op == DxbcOpcode::Mul && ins.srcCount == 2) {
        for (uint32_t a = 0; a < 2u; ++a) {
          const DxbcRegister& cbReg = ins.src[a];
          const DxbcRegister& imm = ins.src[1u - a];
          if (cbReg.type != DxbcOperandType::ConstantBuffer || cbReg.idxDim < 2
           || cbReg.idx[0].relReg != nullptr || cbReg.idx[1].relReg != nullptr
           || imm.type != DxbcOperandType::Imm32)
            continue;
          for (uint32_t c = 0; c < 4u; ++c) {
            if (!dst.mask[c] || cbReg.swizzle[c] != 0)
              continue;
            const uint32_t bits = imm.componentCount == DxbcComponentCount::Component4 ? imm.imm.u32_4[c] : imm.imm.u32_1;
            float f;
            std::memcpy(&f, &bits, sizeof(f));
            if (std::abs(f - 1.0f / 255.0f) < 1.0e-5f && out.gbufferCb < 0) {
              out.gbufferCb = int8_t(cbReg.idx[0].offset);
              out.gbufferReg = uint16_t(cbReg.idx[1].offset);
            }
          }
        }
        continue;
      }

      if (dst.type != DxbcOperandType::Temp || dst.idx[0].relReg != nullptr)
        continue;

      std::array<Term, 4> written = temps[uint32_t(dst.idx[0].offset)];

      const bool sample = (ins.opClass == DxbcInstClass::TextureSample || ins.opClass == DxbcInstClass::TextureGather)
        && ins.srcCount >= 2u && ins.src[1].type == DxbcOperandType::Resource && ins.src[1].idxDim > 0
        && ins.src[1].idx[0].relReg == nullptr && uint32_t(ins.src[1].idx[0].offset) < cube.size();

      for (uint32_t c = 0; c < 4u; ++c) {
        if (!dst.mask[c])
          continue;

        Term result;

        if (sample) {
          const uint32_t slot = uint32_t(ins.src[1].idx[0].offset);
          if (cube[slot]) {
            result.cube = true;
          } else {
            result.sampleSlot = int8_t(slot);
            result.sampleChannel = int8_t(ins.src[1].swizzle[c]);
          }
          if (cube[slot] && out.cubeSlot < 0)
            out.cubeSlot = int8_t(slot);
        } else if ((ins.op == DxbcOpcode::Mul || ins.op == DxbcOpcode::Mad) && ins.srcCount >= 2u) {
          result = product(termOf(ins.src[0], c), termOf(ins.src[1], c));
          if (result.cube) {
            // The best-supported reflection term: a strength constant, then masks.
            const int score = (result.cbSlot >= 0 ? 4 : 0)
              + (result.texSlot[0] >= 0 ? 1 : 0) + (result.texSlot[1] >= 0 ? 1 : 0);
            if (score > best) {
              best = score;
              out.scaleCb = result.cbSlot;
              out.scaleReg = result.cbReg;
              out.scaleComponent = result.cbComponent;
              for (uint32_t i = 0; i < 2; ++i) {
                out.maskSlot[i] = result.texSlot[i];
                out.maskChannel[i] = result.texChannel[i];
              }
            }
          }
        } else if (ins.op == DxbcOpcode::Mov && ins.srcCount == 1) {
          result = termOf(ins.src[0], c);
        } else {
          // Adds and the like: a cube value stays a cube value (factors kept
          // from the cube side); anything else loses its factor history.
          for (uint32_t s = 0; s < ins.srcCount; ++s) {
            const Term t = termOf(ins.src[s], c);
            if (t.cube) {
              result = t;
              break;
            }
          }
        }

        written[c] = result;
      }

      temps[uint32_t(dst.idx[0].offset)] = written;
    }

    // A cube sampled without a strength constant is not a material reflection
    // (skyboxes, ambient probes).
    if (out.scaleCb < 0)
      out.cubeSlot = -1;
  }

  static void parseDxbcSampledTexcoords(
    const DxbcModule& module,
    std::array<D3D11SampledTexcoordSemantic,
      D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT>& outSemantics,
    std::array<bool,
      D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT>& outSampledResources,
    bool& outResourceProfileComplete) {
    outResourceProfileComplete = true;
    const Rc<DxbcIsgn> inputSignature = module.isgn();

    static constexpr uint32_t kMaxTrackedInputs = 64u;
    struct InputOrigin {
      int32_t registerId = -1;
      int32_t component = -1;

      bool valid() const {
        return registerId >= 0 && component >= 0;
      }
    };
    using ComponentOrigins = std::array<InputOrigin, 4>;
    std::unordered_map<uint32_t, ComponentOrigins> tempOrigins;
    // Diagnostics: last opcode that wrote each temp, to report why a sample
    // coordinate could not be traced back to a pixel-shader input.
    std::unordered_map<uint32_t, uint32_t> tempWriterOp;
    std::string untracedReport;
    std::array<std::array<std::array<uint16_t, 3>, kMaxTrackedInputs>,
      D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT> directSampleCounts = {};

    auto originFor = [&](const DxbcRegister& reg,
                         uint32_t destinationComponent) -> InputOrigin {
      if (reg.idxDim == 0 || !reg.modifiers.isClear())
        return {};
      const uint32_t sourceComponent = reg.swizzle[destinationComponent];
      const int32_t registerId = reg.idx[0].offset;
      if (registerId < 0)
        return {};
      if (reg.type == DxbcOperandType::Input
       && uint32_t(registerId) < kMaxTrackedInputs)
        return { registerId, int32_t(sourceComponent) };
      if (reg.type == DxbcOperandType::Temp) {
        const auto entry = tempOrigins.find(uint32_t(registerId));
        if (entry != tempOrigins.end())
          return entry->second[sourceComponent];
      }
      return {};
    };

    const bool vertexStage = module.programInfo().type() == DxbcProgramType::VertexShader;

    DxbcCodeSlice slice = module.instructionSlice();
    DxbcDecodeContext decoder;
    while (!slice.atEnd()) {
      decoder.decodeInstruction(slice);
      const DxbcShaderInstruction& ins = decoder.getInstruction();

      const bool samplesTexture =
           ins.opClass == DxbcInstClass::TextureSample
        || ins.opClass == DxbcInstClass::TextureGather;

      // Vertex shaders that read SRVs without sampling (ld, ld_structured,
      // ld_raw, resinfo): UE GPUScene/skin cache, FO4 precombines (t5..t8),
      // Source 2 transform buffers. The resource is then part of the draw's
      // state, so a capture is never reused after its contents change.
      if (vertexStage && !samplesTexture) {
        for (uint32_t s = 0; s < ins.srcCount; ++s) {
          const DxbcRegister& r = ins.src[s];
          if (r.type != DxbcOperandType::Resource)
            continue;
          if (r.idxDim == 0 || r.idx[0].relReg != nullptr || r.idx[0].offset < 0)
            outResourceProfileComplete = false;
          else if (uint32_t(r.idx[0].offset) < outSampledResources.size())
            outSampledResources[uint32_t(r.idx[0].offset)] = true;
        }
      }
      if (samplesTexture && ins.srcCount >= 3u
       && ins.src[1].type == DxbcOperandType::Resource) {
        if (ins.src[1].idxDim == 0
         || ins.src[1].idx[0].relReg != nullptr
         || ins.src[1].idx[0].offset < 0) {
          outResourceProfileComplete = false;
        } else {
          const uint32_t resourceSlot = uint32_t(ins.src[1].idx[0].offset);
          if (resourceSlot < outSampledResources.size()) {
            outSampledResources[resourceSlot] = true;

            // Only a direct input or a chain of plain MOVs is an exact Remix
            // UV contract. Arithmetic in the pixel shader (atlas scale/bias,
            // projection, animation) cannot be reproduced by capturing the
            // original VS output and must fall back to an untextured material.
            const InputOrigin u = originFor(ins.src[0], 0u);
            const InputOrigin v = originFor(ins.src[0], 1u);
            if ((!u.valid() || !v.valid()) && untracedReport.size() < 200u) {
              const DxbcRegister& coord = ins.src[0];
              uint32_t writer = UINT32_MAX;
              if (coord.type == DxbcOperandType::Temp && coord.idxDim > 0) {
                auto w = tempWriterOp.find(uint32_t(coord.idx[0].offset));
                if (w != tempWriterOp.end())
                  writer = w->second;
              }
              untracedReport += str::format(" t", resourceSlot, ":coordType=", uint32_t(coord.type),
                ",writerOp=", writer == UINT32_MAX ? std::string("none") : std::to_string(writer),
                ",mods=", coord.modifiers.isClear() ? 0 : 1, ";");
            }
            if (u.valid() && v.valid()
             && u.registerId == v.registerId
             && v.component == u.component + 1
             && u.component >= 0 && u.component <= 2
             && directSampleCounts[resourceSlot][uint32_t(u.registerId)]
                  [uint32_t(u.component)] != UINT16_MAX) {
              ++directSampleCounts[resourceSlot][uint32_t(u.registerId)]
                  [uint32_t(u.component)];
            }
          }
        }
      }

      for (uint32_t destination = 0; destination < ins.dstCount; ++destination) {
        const DxbcRegister& dst = ins.dst[destination];
        if (dst.type != DxbcOperandType::Temp || dst.idxDim == 0)
          continue;
        const uint32_t registerId = uint32_t(dst.idx[0].offset);
        tempWriterOp[registerId] = uint32_t(ins.op);
        ComponentOrigins& origins = tempOrigins[registerId];
        const bool exactMove = ins.op == DxbcOpcode::Mov
          && ins.srcCount == 1u && ins.src[0].modifiers.isClear();

        // Single-input affine UV transforms (material tiling scale/offset:
        // mul/mad/add of ONE varying with constant or cbuffer operands) keep
        // the varying's origin. The sampled texture is still the material's
        // own; only the tiling of the captured UVs may differ when the scale
        // is not identity. Rejecting these left most Fallout 4 materials
        // untextured (white). Two varyings or any other op still break it.
        int32_t affineVaryingSource = -1;
        if (!exactMove && (ins.op == DxbcOpcode::Mul || ins.op == DxbcOpcode::Mad
         || ins.op == DxbcOpcode::Add)) {
          auto isConstantOperand = [](const DxbcRegister& r) {
            return r.type == DxbcOperandType::ConstantBuffer
                || r.type == DxbcOperandType::Imm32
                || r.type == DxbcOperandType::ImmediateConstantBuffer;
          };
          // mad dst, a, b, c: the varying may be a or b; c must be constant.
          const uint32_t varyingCandidates = ins.op == DxbcOpcode::Mad ? 2u : ins.srcCount;
          bool valid = ins.srcCount >= 2u;
          for (uint32_t i = 0; valid && i < ins.srcCount; ++i) {
            const DxbcRegister& src = ins.src[i];
            if (isConstantOperand(src))
              continue;
            const bool varying = (src.type == DxbcOperandType::Input
                               || src.type == DxbcOperandType::Temp)
                              && src.modifiers.isClear();
            if (!varying || i >= varyingCandidates || affineVaryingSource >= 0)
              valid = false;
            else
              affineVaryingSource = int32_t(i);
          }
          if (!valid)
            affineVaryingSource = -1;
        }

        for (uint32_t component = 0; component < 4u; ++component) {
          if (!dst.mask[component])
            continue;
          origins[component] = exactMove
            ? originFor(ins.src[0], component)
            : (affineVaryingSource >= 0
              ? originFor(ins.src[uint32_t(affineVaryingSource)], component)
              : InputOrigin());
        }
      }
    }

    if (!untracedReport.empty()) {
      static std::atomic<uint32_t> s_untracedLogs { 0u };
      if (s_untracedLogs.fetch_add(1u) < 40u)
        Logger::info(str::format("[D3D11Shader][uv-trace] untraced sample coordinates:", untracedReport));
    }

    if (inputSignature == nullptr)
      return;

    for (uint32_t resourceSlot = 0; resourceSlot < directSampleCounts.size(); ++resourceSlot) {
      const DxbcSgnEntry* best = nullptr;
      uint16_t bestCount = 0;
      int bestNameScore = -1;
      uint32_t bestComponent = 0;
      for (uint32_t input = 0; input < kMaxTrackedInputs; ++input) {
        for (uint32_t component = 0; component <= 2u; ++component) {
          const uint16_t count = directSampleCounts[resourceSlot][input][component];
          if (count == 0)
            continue;
          const DxbcSgnEntry* candidate = inputSignature->findByRegister(input);
          if (candidate == nullptr
           || candidate->systemValue != DxbcSystemValue::None
           || candidate->componentType != DxbcScalarType::Float32
           || !candidate->componentMask[component]
           || !candidate->componentMask[component + 1u])
            continue;

          std::string upper = candidate->semanticName;
          for (auto& c : upper)
            c = char(::toupper(static_cast<unsigned char>(c)));
          const int nameScore = upper.find("TEXCOORD") != std::string::npos ? 3
            : (upper.compare(0, 2, "UV") == 0 ? 2
            : (upper.find("TEX") != std::string::npos ? 1 : 0));
          if (best == nullptr || count > bestCount
           || (count == bestCount && nameScore > bestNameScore)) {
            best = candidate;
            bestCount = count;
            bestNameScore = nameScore;
            bestComponent = component;
          }
        }
      }
      if (best != nullptr) {
        outSemantics[resourceSlot].semanticName = best->semanticName;
        outSemantics[resourceSlot].semanticIndex = best->semanticIndex;
        outSemantics[resourceSlot].componentIndex = bestComponent;
        outSemantics[resourceSlot].valid = true;
      }
    }

    // Sampled slots whose coordinate could not be traced exactly (UVs built
    // with arithmetic through temps: tiling, parallax, detail scales) fall
    // back to the shader's primary UV input - the lowest-index TEXCOORD with
    // two float components. The texture identity (hash, tagging, replacement)
    // stays exact; only the UV mapping may be approximate for unusual
    // materials. Leaving these untraced rejected every such texture and
    // rendered most Fallout 4 materials untextured (white).
    const DxbcSgnEntry* primaryUv = nullptr;
    for (const DxbcSgnEntry& entry : *inputSignature) {
      if (entry.systemValue != DxbcSystemValue::None
       || entry.componentType != DxbcScalarType::Float32
       || !entry.componentMask[0] || !entry.componentMask[1])
        continue;
      std::string upper = entry.semanticName;
      for (auto& c : upper)
        c = char(::toupper(static_cast<unsigned char>(c)));
      if (upper != "TEXCOORD")
        continue;
      if (primaryUv == nullptr || entry.semanticIndex < primaryUv->semanticIndex)
        primaryUv = &entry;
    }
    if (primaryUv != nullptr) {
      for (uint32_t resourceSlot = 0; resourceSlot < outSemantics.size(); ++resourceSlot) {
        if (!outSampledResources[resourceSlot] || outSemantics[resourceSlot].valid)
          continue;
        outSemantics[resourceSlot].semanticName = primaryUv->semanticName;
        outSemantics[resourceSlot].semanticIndex = primaryUv->semanticIndex;
        outSemantics[resourceSlot].componentIndex = 0;
        outSemantics[resourceSlot].valid = true;
      }
    }
  }

  D3D11CommonShader:: D3D11CommonShader() { }
  D3D11CommonShader::~D3D11CommonShader() { }


  D3D11CommonShader::D3D11CommonShader(
          D3D11Device*    pDevice,
    const DxvkShaderKey*  pShaderKey,
    const DxbcModuleInfo* pDxbcModuleInfo,
    const void*           pShaderBytecode,
          size_t          BytecodeLength) {
    const std::string name = pShaderKey->toString();
    Logger::debug(str::format("Compiling shader ", name));

    // DX11_V277_REAL_SHADER_MODEL: record the true shader model for this
    // shader so draw capture reports it instead of a hardcoded 4.0.
    parseDxbcShaderModel(pShaderBytecode, BytecodeLength,
                         m_shaderModelMajor, m_shaderModelMinor);

    // Cache the bytecode hash once so per-draw RTX lookups (shader-keyed material
    // identity, camera diagnostics) never rehash the container.
    if (pShaderBytecode != nullptr && BytecodeLength != 0)
      m_bytecodeHash = XXH3_64bits(pShaderBytecode, BytecodeLength);

    // DX11_V281_FIXED_FUNCTION: pixel shaders that discard are this API
    // generation's alpha test; parse once so draw capture can mark cutout
    // geometry (FillMaterialData).
    if (pShaderKey->type() == VK_SHADER_STAGE_FRAGMENT_BIT)
      m_usesDiscard = parseDxbcUsesDiscard(pShaderBytecode, BytecodeLength);
    
    DxbcReader reader(
      reinterpret_cast<const char*>(pShaderBytecode),
      BytecodeLength);
    
    DxbcModule module(reader);

    // Retain the reflection chunk (resource / constant-buffer names) so the RTX
    // capture layer can tell material inputs apart from engine-wide ones without
    // reparsing the container per draw. Null when the shader shipped stripped.
    m_reflection = module.rdef();

    if (pShaderKey->type() == VK_SHADER_STAGE_FRAGMENT_BIT
     || pShaderKey->type() == VK_SHADER_STAGE_VERTEX_BIT)
      parseDxbcSampledTexcoords(
        module, m_sampledTexcoordSemantics, m_sampledResourceSlots,
        m_sampledResourceProfileComplete);

    if (pShaderKey->type() == VK_SHADER_STAGE_FRAGMENT_BIT) {
      parseDxbcTextureDecodes(module, m_textureDecodes);
      parseDxbcVertexColorInput(module, m_vertexColorSemantic, m_vertexColorComponents);
      parseDxbcEnvmapReflection(module, m_envmapReflection);

      // The colour input is never the input a texture is sampled with.
      for (const auto& uv : m_sampledTexcoordSemantics) {
        if (m_vertexColorSemantic.valid && uv.valid && uv.semanticIndex == m_vertexColorSemantic.semanticIndex
         && uv.semanticName == m_vertexColorSemantic.semanticName)
          m_vertexColorSemantic = D3D11SampledTexcoordSemantic();
      }
    }

    if (pShaderKey->type() == VK_SHADER_STAGE_VERTEX_BIT) {
      m_positionTransform = findPositionTransformBinding(module);
      if (!m_positionTransform.valid)
        m_positionTransform = findColumnPositionTransformBinding(module);
      m_writesPositionAtFarPlane = parseDxbcWritesPositionAtFarPlane(module, m_positionWriteSummary,
                                                                     m_writesScreenSpacePosition);
      if (m_positionTransform.valid && m_positionTransform.matrices[0].columns) {
        static uint32_t s_columnLogs = 0;
        if (s_columnLogs++ < 12u) {
          const auto& m0 = m_positionTransform.matrices[0];
          Logger::info(str::format("[D3D11Shader] column-chain transform proven: ", name,
            " matrices=", m_positionTransform.matrixCount, " cb", m0.constantBufferSlot,
            "[", m0.constantRegisters[0], "..] affineW=", m0.affineW ? 1 : 0));
        }
      }
      m_animatesVertices = parseDxbcUsesTrigonometry(module);
      if (!m_animatesVertices)
        m_cameraRelativeWorld = parseCameraRelativeWorldBinding(module);
      if (!m_cameraRelativeWorld.valid && m_cameraRelativeWorld.debugEyeAdds >= 3u) {
        static uint32_t s_worldMissLogs = 0;
        if (s_worldMissLogs++ < 12u) {
          Logger::info(str::format("[D3D11Shader] camera-relative world pattern NOT proven: ", name,
            " hash=0x", std::hex, m_bytecodeHash, std::dec,
            " eyeAdds=", m_cameraRelativeWorld.debugEyeAdds, " worldRows=", m_cameraRelativeWorld.debugWorldRows,
            " worldComplete=", m_cameraRelativeWorld.debugWorldComplete, " vpRows=", m_cameraRelativeWorld.debugVpRows,
            " animates=", m_animatesVertices ? 1 : 0));
        }
      }
      if (m_cameraRelativeWorld.valid) {
        static uint32_t s_worldBindingLogs = 0;
        if (s_worldBindingLogs++ < 8u) {
          Logger::info(str::format("[D3D11Shader] camera-relative world transform proven: ", name,
            " world=cb", m_cameraRelativeWorld.worldSlot, "[", m_cameraRelativeWorld.worldRegister, "]",
            " eye=cb", m_cameraRelativeWorld.cameraSlot, "[", m_cameraRelativeWorld.eyeRegister, "]",
            " viewProj=cb", m_cameraRelativeWorld.viewProjSlot, "[", m_cameraRelativeWorld.viewProjRegister, "]",
            m_cameraRelativeWorld.hasUvTransform ? str::format(" uv=cb", m_cameraRelativeWorld.uvSlot, "[", m_cameraRelativeWorld.uvRegister, "]") : std::string()));
        }
      }
      m_constantBufferDependencies = findConstantBufferDependencies(module);
    }
    
    // If requested by the user, dump both the raw DXBC
    // shader and the compiled SPIR-V module to a file.
    std::string dumpPath = env::getEnvVar("DXVK_SHADER_DUMP_PATH");
    // Steam often launches the actual game through an already-running client,
    // so per-launch environment variables never reach the game process. Allow
    // an explicit marker beside the executable to enable the same raw DXBC/SPV
    // dump path without a registry or global environment mutation. The marker
    // is opt-in and has zero runtime cost after this creation-time check.
    // Compute shaders are included: tiled/clustered deferred engines read
    // their light lists there, and the reflection data names the layout.
    if (dumpPath.empty()
     && (pShaderKey->type() == VK_SHADER_STAGE_VERTEX_BIT
      || pShaderKey->type() == VK_SHADER_STAGE_COMPUTE_BIT)
     && std::filesystem::exists("dx11-camera-shader-dump.flag")) {
      dumpPath = "rtx-remix/logs/dx11-camera-shaders";
      std::error_code createError;
      std::filesystem::create_directories(dumpPath, createError);
      if (createError) {
        Logger::warn(str::format(
          "[Remix-DX11] Could not create camera shader dump directory: ",
          createError.message()));
        dumpPath.clear();
      }
    }
    
    if (dumpPath.size() != 0) {
      reader.store(std::ofstream(str::tows(str::format(dumpPath, "/", name, ".dxbc").c_str()).c_str(),
        std::ios_base::binary | std::ios_base::trunc));
    }
    
    // Decide whether we need to create a pass-through
    // geometry shader for vertex shader stream output
    bool passthroughShader = pDxbcModuleInfo->xfb != nullptr
      && (module.programInfo().type() == DxbcProgramType::VertexShader
       || module.programInfo().type() == DxbcProgramType::DomainShader);

    if (module.programInfo().shaderStage() != pShaderKey->type() && !passthroughShader)
      throw DxvkError("Mismatching shader type.");

    m_shader = passthroughShader
      ? module.compilePassthroughShader(*pDxbcModuleInfo, name)
      : module.compile                 (*pDxbcModuleInfo, name);
    m_shader->setShaderKey(*pShaderKey);

    // Preserve only the game's original shader bytecode. Stream-output
    // capture variants are generated by the compatibility layer and must not
    // be replayed as application shaders on the next launch.
    if (pDxbcModuleInfo->xfb == nullptr)
      persistGameShaderBytecode(
        *pShaderKey, pShaderBytecode, BytecodeLength);
    
    if (dumpPath.size() != 0) {
      std::ofstream dumpStream(
        str::tows(str::format(dumpPath, "/", name, ".spv").c_str()).c_str(),
        std::ios_base::binary | std::ios_base::trunc);
      
      m_shader->dump(dumpStream);
    }
    
    // Create shader constant buffer if necessary
    if (m_shader->shaderConstants().data() != nullptr) {
      DxvkBufferCreateInfo info;
      info.size   = m_shader->shaderConstants().sizeInBytes();
      info.usage  = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
      info.stages = util::pipelineStages(m_shader->stage());
      info.access = VK_ACCESS_UNIFORM_READ_BIT;
      
      VkMemoryPropertyFlags memFlags
        = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT
        | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
        | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
      
      m_buffer = pDevice->GetDXVKDevice()->createBuffer(info, memFlags, DxvkMemoryStats::Category::AppBuffer, "d3d11 shader constants");

      std::memcpy(m_buffer->mapPtr(0),
        m_shader->shaderConstants().data(),
        m_shader->shaderConstants().sizeInBytes());
    }

    pDevice->GetDXVKDevice()->registerShader(m_shader);

    // DX11_V280_TEXCOORD_CAPTURE / DX11_V290_POST_VS_POSITION_CAPTURE: for
    // plain vertex shaders with recoverable output streams, retain the bytecode
    // and compile options so stream-output capture GSes can be built on demand.
    // Bounded: only VS, only when a candidate output exists, and oversized
    // blobs are skipped; the bytecode is released after the (single) build.
    if (pShaderKey->type() == VK_SHADER_STAGE_VERTEX_BIT
     && pDxbcModuleInfo->xfb == nullptr
     && BytecodeLength <= (1u << 20)) {
      std::string semanticName;
      uint32_t semanticIndex = 0;
      std::vector<D3D11TexcoordSemantic> outputSemantics;
      if (parseDxbcOutputTexcoord(pShaderBytecode, BytecodeLength,
                                  semanticName, semanticIndex,
                                  &outputSemantics)) {
        m_texcoordCapture = std::make_shared<D3D11TexcoordCaptureState>();
        m_texcoordCapture->bytecode.assign(
          reinterpret_cast<const char*>(pShaderBytecode),
          reinterpret_cast<const char*>(pShaderBytecode) + BytecodeLength);
        m_texcoordCapture->options = pDxbcModuleInfo->options;
        m_texcoordCapture->semanticName = semanticName;
        m_texcoordCapture->semanticIndex = semanticIndex;
      }

      // Every graphics VS must produce SV_Position, and it is the only output
      // guaranteed to include the engine's complete skinning, morphing,
      // instancing and object/view transforms. Capture its homogeneous xyzw and
      // unproject it in the RT interleaver. This removes semantic-name/profile
      // guesses from geometry reconstruction while leaving executable profiles
      // available for material/camera policy.
      m_positionCapture = std::make_shared<D3D11PositionCaptureState>();
      m_positionCapture->bytecode.assign(
        reinterpret_cast<const char*>(pShaderBytecode),
        reinterpret_cast<const char*>(pShaderBytecode) + BytecodeLength);
      m_positionCapture->options = pDxbcModuleInfo->options;
      m_positionCapture->shaderName = name;
      m_positionCapture->semanticName = "SV_Position";
      m_positionCapture->semanticIndex = 0;
      m_positionCapture->positionSpace = D3D11CapturedPositionSpace::View;
      m_positionCapture->homogeneousClipSpace = true;
      m_positionCapture->loadedFromProfile = false;
      m_positionCapture->texcoordSemantics = std::move(outputSemantics);
      if (m_texcoordCapture != nullptr) {
        m_positionCapture->texcoordSemanticName = std::move(semanticName);
        m_positionCapture->texcoordSemanticIndex = semanticIndex;
      }
    }

    // Geometry shaders that emit triangles (point-sprite / GPU particle
    // expansion, AC4 rain, fur shells): recompile the game's own GS with a
    // stream-output entry on SV_Position, as CreateGeometryShaderWithStreamOutput
    // does with GS bytecode. Point or line output gives nothing a BLAS can use.
    if (pShaderKey->type() == VK_SHADER_STAGE_GEOMETRY_BIT
     && pDxbcModuleInfo->xfb == nullptr
     && BytecodeLength <= (1u << 20)) {
      bool trianglesOut = false;
      DxbcCodeSlice gsCode = module.instructionSlice();
      DxbcDecodeContext gsDecoder;
      while (!gsCode.atEnd()) {
        gsDecoder.decodeInstruction(gsCode);
        const DxbcShaderInstruction& ins = gsDecoder.getInstruction();
        if (ins.op == DxbcOpcode::DclGsOutputPrimitiveTopology)
          trianglesOut = ins.controls.primitiveTopology() == DxbcPrimitiveTopology::TriangleStrip;
      }
      if (trianglesOut) {
        m_positionCapture = std::make_shared<D3D11PositionCaptureState>();
        m_positionCapture->bytecode.assign(
          reinterpret_cast<const char*>(pShaderBytecode),
          reinterpret_cast<const char*>(pShaderBytecode) + BytecodeLength);
        m_positionCapture->options = pDxbcModuleInfo->options;
        m_positionCapture->shaderName = name;
        m_positionCapture->semanticName = "SV_Position";
        m_positionCapture->semanticIndex = 0;
        m_positionCapture->positionSpace = D3D11CapturedPositionSpace::View;
        m_positionCapture->homogeneousClipSpace = true;
        m_positionCapture->loadedFromProfile = false;
        m_positionCapture->recompileGeometryShader = true;
        // The pixel shader reads its UVs from the GS outputs (stream 0).
        std::string uvName;
        uint32_t uvIndex = 0;
        if (parseDxbcOutputTexcoord(pShaderBytecode, BytecodeLength, uvName, uvIndex,
                                    &m_positionCapture->texcoordSemantics)) {
          m_positionCapture->texcoordSemanticName = std::move(uvName);
          m_positionCapture->texcoordSemanticIndex = uvIndex;
        }
      }
    }

    // Tessellated geometry (Frostbite terrain, Void water, UE/RAGE/ACU
    // displacement): the domain shader writes the final SV_Position, so the
    // capture GS is built from its output signature, as D3D11 stream output
    // with DS bytecode does. Triangle input (see processXfbPassthrough).
    if (pShaderKey->type() == VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT
     && pDxbcModuleInfo->xfb == nullptr
     && BytecodeLength <= (1u << 20)) {
      m_positionCapture = std::make_shared<D3D11PositionCaptureState>();
      m_positionCapture->bytecode.assign(
        reinterpret_cast<const char*>(pShaderBytecode),
        reinterpret_cast<const char*>(pShaderBytecode) + BytecodeLength);
      m_positionCapture->options = pDxbcModuleInfo->options;
      m_positionCapture->shaderName = name;
      m_positionCapture->semanticName = "SV_Position";
      m_positionCapture->semanticIndex = 0;
      m_positionCapture->positionSpace = D3D11CapturedPositionSpace::View;
      m_positionCapture->homogeneousClipSpace = true;
      m_positionCapture->loadedFromProfile = false;
      m_positionCapture->triangleInput = true;
      // Without a GS the pixel shader reads its UVs from the DS outputs.
      std::string uvName;
      uint32_t uvIndex = 0;
      if (parseDxbcOutputTexcoord(pShaderBytecode, BytecodeLength, uvName, uvIndex,
                                  &m_positionCapture->texcoordSemantics)) {
        m_positionCapture->texcoordSemanticName = std::move(uvName);
        m_positionCapture->texcoordSemanticIndex = uvIndex;
      }
    }
  }

  bool D3D11CommonShader::GetSampledTexcoordSemantic(
      uint32_t resourceSlot,
      std::string& semanticName,
      uint32_t& semanticIndex,
      uint32_t& componentIndex) const {
    if (resourceSlot >= m_sampledTexcoordSemantics.size()
     || !m_sampledTexcoordSemantics[resourceSlot].valid)
      return false;
    semanticName = m_sampledTexcoordSemantics[resourceSlot].semanticName;
    semanticIndex = m_sampledTexcoordSemantics[resourceSlot].semanticIndex;
    componentIndex = m_sampledTexcoordSemantics[resourceSlot].componentIndex;
    return true;
  }

  bool D3D11CommonShader::ResolvePositionCaptureTexcoord(
      const std::string& requestedName,
      uint32_t requestedIndex,
      uint32_t requestedComponent,
      std::string& semanticName,
      uint32_t& semanticIndex,
      uint32_t& componentIndex) const {
    if (m_positionCapture == nullptr)
      return false;

    auto namesEqual = [](const std::string& a, const std::string& b) {
      if (a.size() != b.size())
        return false;
      for (size_t i = 0; i < a.size(); ++i) {
        if (::toupper(static_cast<unsigned char>(a[i]))
         != ::toupper(static_cast<unsigned char>(b[i])))
          return false;
      }
      return true;
    };

    if (!requestedName.empty()) {
      for (const auto& candidate : m_positionCapture->texcoordSemantics) {
        if (candidate.semanticIndex == requestedIndex
         && namesEqual(candidate.semanticName, requestedName)) {
          semanticName = candidate.semanticName;
          semanticIndex = candidate.semanticIndex;
          componentIndex = requestedComponent;
          return true;
        }
      }

      // The pixel shader identified an exact input semantic for this sampled
      // resource slot. Substituting the vertex shader's generic "best" UV
      // output when that semantic is absent associates the texture/hash with
      // unrelated geometry data (a common TEXCOORD0-vs-TEXCOORD4 Unreal
      // mismatch). An explicit request is therefore exact-or-fail.
      return false;
    }

    // No pixel-shader contract was available. Keep the generic fallback only
    // for the legacy standalone UV-capture path, which has no resource slot to
    // match and never claims that the fallback belongs to a specific hash.
    if (m_positionCapture->texcoordSemanticName.empty())
      return false;
    semanticName = m_positionCapture->texcoordSemanticName;
    semanticIndex = m_positionCapture->texcoordSemanticIndex;
    componentIndex = 0u;
    return true;
  }


  uint32_t D3D11CommonShader::ResolvePositionCaptureColor(const std::string& requestedName, uint32_t requestedIndex) const {
    const std::shared_ptr<D3D11PositionCaptureState>& state = m_positionCapture;
    if (state == nullptr || requestedName.empty())
      return 0;

    std::lock_guard<dxvk::mutex> lock(state->mutex);

    if (!state->colorOutputsParsed) {
      state->colorOutputsParsed = true;
      try {
        DxbcReader reader(state->bytecode.data(), state->bytecode.size());
        DxbcModule module(reader);
        const Rc<DxbcIsgn> osgn = module.osgn();
        if (osgn != nullptr) {
          for (const DxbcSgnEntry& e : *osgn) {
            if (e.systemValue != DxbcSystemValue::None || e.componentType != DxbcScalarType::Float32
             || e.streamId != 0 || !e.componentMask[0] || !e.componentMask[1] || !e.componentMask[2])
              continue;
            state->colorOutputs.push_back({ e.semanticName, e.semanticIndex, e.componentMask[3] ? 4u : 3u });
          }
        }
      } catch (const DxvkError&) {
        state->colorOutputs.clear();
      }
    }

    for (const auto& o : state->colorOutputs) {
      if (o.semanticIndex != requestedIndex || o.semanticName.size() != requestedName.size())
        continue;
      bool same = true;
      for (size_t i = 0; i < o.semanticName.size() && same; ++i)
        same = ::toupper(static_cast<unsigned char>(o.semanticName[i])) == ::toupper(static_cast<unsigned char>(requestedName[i]));
      if (same)
        return o.components;
    }

    return 0;
  }


  Rc<DxvkShader> D3D11CommonShader::GetTexcoordCaptureShader() const {
    const std::shared_ptr<D3D11TexcoordCaptureState>& state = m_texcoordCapture;
    if (state == nullptr)
      return nullptr;

    std::lock_guard<dxvk::mutex> lock(state->mutex);
    if (state->attempted)
      return state->shader;
    state->attempted = true;

    try {
      DxbcReader reader(state->bytecode.data(), state->bytecode.size());
      DxbcModule module(reader);

      // Single xfb entry: the VS's texcoord output, .xy, into buffer 0 at
      // offset 0 with stride 8. rasterizedStream = -1 turns the replay
      // pipeline into a pure capture pass: dxvk keys rasterizer discard off
      // the GS xfb stream, so the replay can never touch color or depth.
      DxbcXfbInfo xfb = {};
      xfb.entryCount = 1;
      xfb.entries[0].semanticName   = state->semanticName.c_str();
      xfb.entries[0].semanticIndex  = state->semanticIndex;
      xfb.entries[0].componentIndex = 0;
      xfb.entries[0].componentCount = 2;
      xfb.entries[0].streamId       = 0;
      xfb.entries[0].bufferId       = 0;
      xfb.entries[0].offset         = 0;
      xfb.strides[0] = 8;
      xfb.rasterizedStream = -1;

      DxbcModuleInfo info;
      info.options = state->options;
      info.tess = nullptr;
      info.xfb = &xfb;

      Rc<DxvkShader> gs = module.compilePassthroughShader(info, "dx11_texcoord_capture_gs");
      static constexpr char kTexcoordCaptureKey[] = "dx11-texcoord-capture-v1";
      const Sha1Data shaderKeyData[] = {
        { state->bytecode.data(), state->bytecode.size() },
        { kTexcoordCaptureKey, sizeof(kTexcoordCaptureKey) - 1 },
      };
      gs->setShaderKey(DxvkShaderKey(VK_SHADER_STAGE_GEOMETRY_BIT,
        Sha1Hash::compute(2, shaderKeyData)));
      state->shader = gs;

      const std::string dumpPath = env::getEnvVar("DXVK_SHADER_DUMP_PATH");
      if (!dumpPath.empty()) {
        std::ofstream dump(std::filesystem::path(dumpPath) / (gs->getShaderKey().toString() + ".spv"),
          std::ios::binary | std::ios::trunc);
        gs->dump(dump);
      }

      Logger::info(str::format(
        "[Remix-DX11] V280: texcoord capture GS built (semantic=",
        state->semanticName, state->semanticIndex, ")"));
    } catch (const DxvkError& e) {
      Logger::warn(str::format(
        "[Remix-DX11] V280: texcoord capture GS compile failed: ", e.message()));
    }

    // One attempt per shader either way; the bytecode is no longer needed.
    state->bytecode.clear();
    state->bytecode.shrink_to_fit();
    return state->shader;
  }


  Rc<DxvkShader> D3D11CommonShader::GetPositionCaptureShader(
      const std::string& texcoordSemanticName,
      uint32_t texcoordSemanticIndex,
      uint32_t texcoordComponentIndex,
      const std::string& colorSemanticName,
      uint32_t colorSemanticIndex,
      uint32_t colorComponents) const {
    const std::shared_ptr<D3D11PositionCaptureState>& state = m_positionCapture;
    if (state == nullptr)
      return nullptr;

    std::lock_guard<dxvk::mutex> lock(state->mutex);
    uint64_t variantKey = XXH3_64bits(
      texcoordSemanticName.data(), texcoordSemanticName.size());
    variantKey = XXH3_64bits_withSeed(
      &texcoordSemanticIndex, sizeof(texcoordSemanticIndex), variantKey);
    variantKey = XXH3_64bits_withSeed(
      &texcoordComponentIndex, sizeof(texcoordComponentIndex), variantKey);
    const bool captureColor = colorComponents >= 3u && !colorSemanticName.empty();
    if (captureColor) {
      variantKey = XXH3_64bits_withSeed(colorSemanticName.data(), colorSemanticName.size(), variantKey);
      variantKey = XXH3_64bits_withSeed(&colorSemanticIndex, sizeof(colorSemanticIndex), variantKey);
      variantKey = XXH3_64bits_withSeed(&colorComponents, sizeof(colorComponents), variantKey);
    }
    D3D11PositionCaptureVariant& variant = state->variants[variantKey];
    if (variant.attempted)
      return variant.shader;
    variant.attempted = true;

    try {
      DxbcReader reader(state->bytecode.data(), state->bytecode.size());
      DxbcModule module(reader);

      DxbcXfbInfo xfb = {};
      const uint32_t positionBytes = state->homogeneousClipSpace ? 16u : 12u;
      const bool captureTexcoord = !texcoordSemanticName.empty();
      xfb.entryCount = captureTexcoord ? 2 : 1;
      const uint32_t colorOffset = positionBytes + (captureTexcoord ? 8u : 0u);
      xfb.entries[0].semanticName   = state->semanticName.c_str();
      xfb.entries[0].semanticIndex  = state->semanticIndex;
      xfb.entries[0].componentIndex = 0;
      xfb.entries[0].componentCount = state->homogeneousClipSpace ? 4 : 3;
      xfb.entries[0].streamId       = 0;
      xfb.entries[0].bufferId       = 0;
      xfb.entries[0].offset         = 0;
      if (captureTexcoord) {
        xfb.entries[1].semanticName   = texcoordSemanticName.c_str();
        xfb.entries[1].semanticIndex  = texcoordSemanticIndex;
        xfb.entries[1].componentIndex = texcoordComponentIndex;
        xfb.entries[1].componentCount = 2;
        xfb.entries[1].streamId       = 0;
        xfb.entries[1].bufferId       = 0;
        xfb.entries[1].offset         = positionBytes;
      }
      if (captureColor) {
        // Vertex colour after position (and texcoord) in the same record.
        DxbcXfbEntry& color = xfb.entries[xfb.entryCount++];
        color.semanticName   = colorSemanticName.c_str();
        color.semanticIndex  = colorSemanticIndex;
        color.componentIndex = 0;
        color.componentCount = colorComponents;
        color.streamId       = 0;
        color.bufferId       = 0;
        color.offset         = colorOffset;
      }
      xfb.strides[0] = colorOffset + (captureColor ? colorComponents * 4u : 0u);
      xfb.rasterizedStream = -1;

      DxbcModuleInfo info;
      info.options = state->options;
      info.tess = nullptr;
      info.xfb = &xfb;

      Rc<DxvkShader> gs = state->recompileGeometryShader
        ? module.compile(info, "dx11_position_capture_game_gs")
        : module.compilePassthroughShader(
            info, "dx11_position_capture_gs", true, state->triangleInput ? 3u : 1u);
      const std::string captureContract = str::format(
        "dx11-position-texcoord-capture-system-value-v3:",
        texcoordSemanticName, ":", texcoordSemanticIndex, ":",
        texcoordComponentIndex, state->triangleInput ? ":tri" : "",
        state->recompileGeometryShader ? ":gs" : "",
        captureColor ? str::format(":color:", colorSemanticName, ":", colorSemanticIndex, ":", colorComponents) : std::string());
      const Sha1Data shaderKeyData[] = {
        { state->bytecode.data(), state->bytecode.size() },
        { captureContract.data(), captureContract.size() },
      };
      gs->setShaderKey(DxvkShaderKey(VK_SHADER_STAGE_GEOMETRY_BIT,
        Sha1Hash::compute(2, shaderKeyData)));
      variant.shader = gs;

      const std::string dumpPath = env::getEnvVar("DXVK_SHADER_DUMP_PATH");
      if (!dumpPath.empty()) {
        std::ofstream dump(std::filesystem::path(dumpPath) / (gs->getShaderKey().toString() + ".spv"),
          std::ios::binary | std::ios::trunc);
        gs->dump(dump);
      }

      Logger::info(str::format(
        "[Remix-DX11] V290: post-VS position capture GS built (vs=",
        state->shaderName, ", semantic=",
        state->semanticName, state->semanticIndex,
        ", space=", state->positionSpace == D3D11CapturedPositionSpace::World
          ? "world" : "view",
        ", source=", state->homogeneousClipSpace
          ? "exact-sv-position"
          : (state->loadedFromProfile ? "profile" : "auto-discovery"),
        ", texcoord=", captureTexcoord
          ? str::format(texcoordSemanticName, texcoordSemanticIndex)
          : "none",
        ", color=", captureColor
          ? str::format(colorSemanticName, colorSemanticIndex, "x", colorComponents)
          : "none", ")"));
    } catch (const DxvkError& e) {
      Logger::warn(str::format(
        "[Remix-DX11] V290: position capture GS compile failed: ", e.message()));
    }

    return variant.shader;
  }


  D3D11ShaderModuleSet:: D3D11ShaderModuleSet() { }
  D3D11ShaderModuleSet::~D3D11ShaderModuleSet() { }

  void D3D11ShaderModuleSet::Clear() {
    std::unique_lock<dxvk::mutex> lock(m_mutex);
    m_modules.clear();
  }
  
  
  HRESULT D3D11ShaderModuleSet::GetShaderModule(
          D3D11Device*        pDevice,
    const DxvkShaderKey*      pShaderKey,
    const DxbcModuleInfo*     pDxbcModuleInfo,
    const void*               pShaderBytecode,
          size_t              BytecodeLength,
          D3D11CommonShader*  pShader) {
    // Use the shader's unique key for the lookup
    { std::unique_lock<dxvk::mutex> lock(m_mutex);
      
      auto entry = m_modules.find(*pShaderKey);
      if (entry != m_modules.end()) {
        *pShader = entry->second;
        return S_OK;
      }
    }
    
    // This shader has not been compiled yet, so we have to create a
    // new module. This takes a while, so we won't lock the structure.
    D3D11CommonShader module;
    
    try {
      module = D3D11CommonShader(pDevice, pShaderKey,
        pDxbcModuleInfo, pShaderBytecode, BytecodeLength);
    } catch (const DxvkError& e) {
      Logger::err(e.message());
      return E_INVALIDARG;
    }
    
    // Insert the new module into the lookup table. If another thread
    // has compiled the same shader in the meantime, we should return
    // that object instead and discard the newly created module.
    { std::unique_lock<dxvk::mutex> lock(m_mutex);
      
      auto status = m_modules.insert({ *pShaderKey, module });
      if (!status.second) {
        *pShader = status.first->second;
        return S_OK;
      }
    }
    
    *pShader = std::move(module);
    return S_OK;
  }
  
}
