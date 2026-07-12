#include <cctype>
#include <algorithm>
#include <filesystem>

#include "d3d11_device.h"
#include "d3d11_shader.h"

namespace dxvk {

  static D3D11PositionTransformBinding findPositionTransformBinding(const DxbcModule& module) {
    D3D11PositionTransformBinding result;

    const Rc<DxbcIsgn> outputSignature = module.osgn();
    if (outputSignature == nullptr)
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

    struct TransformComponents {
      std::array<int32_t, 4> cbSlots = { -1, -1, -1, -1 };
      std::array<int32_t, 4> cbRegisters = { -1, -1, -1, -1 };
    } position;
    std::unordered_map<uint32_t, TransformComponents> temporaryTransforms;

    auto staticRegisterIndex = [](const DxbcRegister& reg, uint32_t dimension, int32_t& index) {
      if (reg.idxDim <= dimension || reg.idx[dimension].relReg != nullptr)
        return false;
      index = reg.idx[dimension].offset;
      return index >= 0;
    };

    auto invalidateWrittenComponents = [](TransformComponents& transform, const DxbcRegMask& mask) {
      for (uint32_t component = 0; component < 4; ++component) {
        if (mask[component]) {
          transform.cbSlots[component] = -1;
          transform.cbRegisters[component] = -1;
        }
      }
    };

    auto recordDp4 = [&](TransformComponents& transform, const DxbcRegister& dst,
                         const DxbcShaderInstruction& ins) {
      if (dst.mask.popCount() != 1)
        return false;

      const DxbcRegister* cb = nullptr;
      if (ins.src[0].type == DxbcOperandType::ConstantBuffer)
        cb = &ins.src[0];
      else if (ins.src[1].type == DxbcOperandType::ConstantBuffer)
        cb = &ins.src[1];
      if (cb == nullptr || !cb->modifiers.isClear()
       || cb->swizzle != DxbcRegSwizzle(0, 1, 2, 3))
        return false;

      int32_t slot = -1;
      int32_t cbRegister = -1;
      if (!staticRegisterIndex(*cb, 0, slot)
       || !staticRegisterIndex(*cb, 1, cbRegister))
        return false;

      const uint32_t component = dst.mask.firstSet();
      transform.cbSlots[component] = slot;
      transform.cbRegisters[component] = cbRegister;
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
        }
        continue;
      }

      // Most optimized SM5 vertex shaders calculate clip position into a
      // temporary and end with `mov oN, rM`. Propagate the four proven dp4
      // components through that exact move, including scalar write masks and
      // source swizzles. No arithmetic or dynamic indexing is guessed.
      if (isPositionOutput && ins.op == DxbcOpcode::Mov
       && ins.dstCount == 1 && ins.srcCount == 1
       && ins.src[0].type == DxbcOperandType::Temp
       && ins.src[0].modifiers.isClear()) {
        int32_t sourceRegister = -1;
        if (staticRegisterIndex(ins.src[0], 0, sourceRegister)) {
          const auto source = temporaryTransforms.find(uint32_t(sourceRegister));
          if (source != temporaryTransforms.end()) {
            for (uint32_t component = 0; component < 4; ++component) {
              if (!dst.mask[component])
                continue;
              const uint32_t sourceComponent = ins.src[0].swizzle[component];
              position.cbSlots[component] = source->second.cbSlots[sourceComponent];
              position.cbRegisters[component] = source->second.cbRegisters[sourceComponent];
            }
            continue;
          }
        }
      }

      if (isPositionOutput)
        invalidateWrittenComponents(position, dst.mask);
      if (isTemporary)
        invalidateWrittenComponents(temporaryTransforms[uint32_t(dstRegister)], dst.mask);
    }

    const int32_t slot = position.cbSlots[0];
    if (slot < 0)
      return result;
    for (uint32_t component = 0; component < 4; ++component) {
      if (position.cbSlots[component] != slot || position.cbRegisters[component] < 0)
        return result;
    }

    // Require four distinct consecutive registers. This rejects coincidental
    // dp4s used for lighting and accepts both ascending and descending packing.
    std::array<int32_t, 4> sortedRegisters = position.cbRegisters;
    std::sort(sortedRegisters.begin(), sortedRegisters.end());
    for (uint32_t i = 1; i < 4; ++i) {
      if (sortedRegisters[i] != sortedRegisters[0] + int32_t(i))
        return result;
    }

    result.valid = true;
    result.constantBufferSlot = uint32_t(slot);
    for (uint32_t component = 0; component < 4; ++component)
      result.constantRegisters[component] = uint32_t(position.cbRegisters[component]);
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
    uint32_t&    outIndex) {
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

    // DX11_V281_FIXED_FUNCTION: pixel shaders that discard are this API
    // generation's alpha test; parse once so draw capture can mark cutout
    // geometry (FillMaterialData).
    if (pShaderKey->type() == VK_SHADER_STAGE_FRAGMENT_BIT)
      m_usesDiscard = parseDxbcUsesDiscard(pShaderBytecode, BytecodeLength);
    
    DxbcReader reader(
      reinterpret_cast<const char*>(pShaderBytecode),
      BytecodeLength);
    
    DxbcModule module(reader);

    if (pShaderKey->type() == VK_SHADER_STAGE_VERTEX_BIT)
      m_positionTransform = findPositionTransformBinding(module);
    
    // If requested by the user, dump both the raw DXBC
    // shader and the compiled SPIR-V module to a file.
    std::string dumpPath = env::getEnvVar("DXVK_SHADER_DUMP_PATH");
    // Steam often launches the actual game through an already-running client,
    // so per-launch environment variables never reach the game process. Allow
    // an explicit marker beside the executable to enable the same raw DXBC/SPV
    // dump path without a registry or global environment mutation. The marker
    // is opt-in and has zero runtime cost after this creation-time check.
    if (dumpPath.empty()
     && pShaderKey->type() == VK_SHADER_STAGE_VERTEX_BIT
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

    // DX11_V280_TEXCOORD_CAPTURE: for plain vertex shaders whose output
    // signature declares a texcoord-like element, retain the bytecode and
    // compile options so a stream-out capture GS can be built on demand.
    // Bounded: only VS, only when a candidate output exists, and oversized
    // blobs are skipped; the bytecode is released after the (single) build.
    if (pShaderKey->type() == VK_SHADER_STAGE_VERTEX_BIT
     && pDxbcModuleInfo->xfb == nullptr
     && BytecodeLength <= (1u << 20)) {
      std::string semanticName;
      uint32_t semanticIndex = 0;
      if (parseDxbcOutputTexcoord(pShaderBytecode, BytecodeLength, semanticName, semanticIndex)) {
        m_texcoordCapture = std::make_shared<D3D11TexcoordCaptureState>();
        m_texcoordCapture->bytecode.assign(
          reinterpret_cast<const char*>(pShaderBytecode),
          reinterpret_cast<const char*>(pShaderBytecode) + BytecodeLength);
        m_texcoordCapture->options = pDxbcModuleInfo->options;
        m_texcoordCapture->semanticName = std::move(semanticName);
        m_texcoordCapture->semanticIndex = semanticIndex;
      }
    }
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
      gs->setShaderKey(DxvkShaderKey(VK_SHADER_STAGE_GEOMETRY_BIT,
        Sha1Hash::compute(state->bytecode.data(), state->bytecode.size())));
      state->shader = gs;

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


  D3D11ShaderModuleSet:: D3D11ShaderModuleSet() { }
  D3D11ShaderModuleSet::~D3D11ShaderModuleSet() { }
  
  
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
