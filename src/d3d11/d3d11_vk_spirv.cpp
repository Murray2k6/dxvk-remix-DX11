#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <functional>
#include <map>
#include <unordered_map>
#include <unordered_set>

#include "d3d11_vk_spirv.h"
#include "d3d11_vk_frontend.h"

namespace dxvk {

  namespace {

    // SPIR-V specification, section 3 (binary form) and the opcode tables;
    // GLSL.std.450 for extended instructions.
    constexpr uint32_t kMagic = 0x07230203u;

    enum Op : uint32_t {
      OpName                    = 5,
      OpExtension               = 10,
      OpExtInst                 = 12,
      OpMemoryModel             = 14,
      OpEntryPoint              = 15,
      OpExecutionMode           = 16,
      OpCapability              = 17,
      OpTypeVoid                = 19,
      OpTypeInt                 = 21,
      OpTypeFloat               = 22,
      OpTypeVector              = 23,
      OpTypeArray               = 28,
      OpTypeRuntimeArray        = 29,
      OpTypeStruct              = 30,
      OpTypePointer             = 32,
      OpConstant                = 43,
      OpConstantComposite       = 44,
      OpFunction                = 54,
      OpFunctionEnd             = 56,
      OpVariable                = 59,
      OpLoad                    = 61,
      OpStore                   = 62,
      OpAccessChain             = 65,
      OpInBoundsAccessChain     = 66,
      OpDecorate                = 71,
      OpMemberDecorate          = 72,
      OpDecorationGroup         = 73,
      OpGroupDecorate           = 74,
      OpGroupMemberDecorate     = 75,
      OpVectorShuffle           = 79,
      OpCompositeConstruct      = 80,
      OpCompositeExtract        = 81,
      OpCompositeInsert         = 82,
      OpCopyObject              = 83,
      OpSampledImage            = 86,
      OpImageSampleImplicitLod  = 87,
      OpImageSampleProjDrefExplicitLod = 94,
      OpImageGather             = 96,
      OpImageDrefGather         = 97,
      OpImage                   = 100,
      OpUConvert                = 113,
      OpFConvert                = 115,
      OpConvertUToPtr           = 120,
      OpBitcast                 = 124,
      OpIAdd                    = 128,
      OpIMul                    = 132,
      OpFNegate                 = 127,
      OpFAdd                    = 129,
      OpFSub                    = 131,
      OpFMul                    = 133,
      OpFDiv                    = 136,
      OpFMod                    = 141,
      OpVectorTimesScalar       = 142,
      OpDot                     = 148,
      OpSelect                  = 169,
      OpPhi                     = 245,
      OpReturn                  = 253,
      OpImageSparseSampleImplicitLod = 305,
      OpImageSparseSampleExplicitLod = 306,
      OpExecutionModeId         = 331,
      OpDecorateId              = 332,
      OpDecorateString          = 5632,
      OpMemberDecorateString    = 5633,
    };

    constexpr uint32_t kGlslFClamp          = 43;   // GLSL.std.450 FClamp
    constexpr uint32_t kGlslFMix            = 46;   // GLSL.std.450 FMix
    constexpr uint32_t kGlslFma             = 50;   // GLSL.std.450 Fma

    constexpr uint32_t kDecorationBuiltIn   = 11;
    constexpr uint32_t kDecorationLocation  = 30;
    constexpr uint32_t kDecorationBinding   = 33;
    constexpr uint32_t kDecorationDescriptorSet = 34;
    constexpr uint32_t kDecorationOffset    = 35;
    constexpr uint32_t kDecorationXfbBuffer = 36;
    constexpr uint32_t kDecorationXfbStride = 37;

    constexpr uint32_t kBuiltInPosition     = 0;
    constexpr uint32_t kStorageInput        = 1;
    constexpr uint32_t kStorageOutput       = 3;

    constexpr uint32_t kModelVertex         = 0;
    constexpr uint32_t kModelTessEval       = 2;
    constexpr uint32_t kModelGeometry       = 3;

    constexpr uint32_t kExecutionModeXfb    = 11;
    constexpr uint32_t kCapabilityTransformFeedback = 53;

    constexpr size_t   kHeaderWords         = 5;

    uint32_t opcode(uint32_t word)    { return word & 0xffffu; }
    uint32_t wordCount(uint32_t word) { return word >> 16; }

    bool isAnnotation(uint32_t op) {
      return op == OpDecorate || op == OpMemberDecorate || op == OpDecorationGroup
          || op == OpGroupDecorate || op == OpGroupMemberDecorate || op == OpDecorateId
          || op == OpDecorateString || op == OpMemberDecorateString;
    }

    bool isSample(uint32_t op) {
      return (op >= OpImageSampleImplicitLod && op <= OpImageSampleProjDrefExplicitLod)
          || op == OpImageGather || op == OpImageDrefGather
          || op == OpImageSparseSampleImplicitLod || op == OpImageSparseSampleExplicitLod;
    }

    // Walks a module's instructions. Returns false on a malformed stream.
    template<typename Fn>
    bool forEachInstruction(const uint32_t* code, size_t words, Fn&& fn) {
      if (words < kHeaderWords || code[0] != kMagic)
        return false;

      size_t i = kHeaderWords;

      while (i < words) {
        const uint32_t n = wordCount(code[i]);

        if (n == 0 || i + n > words)
          return false;

        fn(i, opcode(code[i]), n);
        i += n;
      }

      return true;
    }

    // Value operands (ids, not literals) of the instructions the analysis
    // follows. Result type and result id are words 1 and 2.
    void valueOperands(const uint32_t* w, uint32_t op, uint32_t n, std::vector<uint32_t>& out) {
      out.clear();

      auto range = [&](uint32_t first, uint32_t last, uint32_t step = 1) {
        for (uint32_t k = first; k < std::min(last, n); k += step)
          out.push_back(w[k]);
      };

      switch (op) {
        case OpLoad: case OpAccessChain: case OpInBoundsAccessChain:
        case OpCopyObject: case OpFConvert: case OpFNegate: case OpCompositeExtract:
        case OpImage:
          range(3, 4); break;
        case OpVectorShuffle: case OpCompositeInsert:
        case OpFAdd: case OpFSub: case OpFMul: case OpFDiv: case OpFMod:
        case OpVectorTimesScalar: case OpSampledImage:
          range(3, 5); break;
        case OpCompositeConstruct:
          range(3, n); break;
        case OpExtInst:
          range(5, n); break;
        case OpSelect:
          range(4, 6); break;
        case OpPhi:
          range(3, n, 2); break;
        default:
          break;
      }
    }

    bool hasTypedResult(uint32_t op) {
      switch (op) {
        case OpVariable: case OpLoad: case OpAccessChain: case OpInBoundsAccessChain:
        case OpVectorShuffle: case OpCompositeConstruct: case OpCompositeExtract:
        case OpCompositeInsert: case OpCopyObject: case OpFConvert: case OpFNegate:
        case OpFAdd: case OpFSub: case OpFMul: case OpFDiv: case OpFMod:
        case OpVectorTimesScalar: case OpSelect: case OpPhi: case OpExtInst:
        case OpConstant: case OpConstantComposite: case OpSampledImage: case OpImage:
          return true;
        default:
          return isSample(op);
      }
    }


    // Whole-module view of a fragment shader for the analysis.
    struct PixelModule {
      const uint32_t*                             code = nullptr;
      std::unordered_map<uint32_t, size_t>        defs;           // result id -> instruction offset
      std::unordered_map<uint32_t, std::vector<size_t>> uses;     // id -> instructions using it
      std::unordered_map<uint32_t, uint32_t>      locations;      // variable -> Location
      std::unordered_map<uint32_t, uint32_t>      sets;           // variable -> DescriptorSet
      std::unordered_map<uint32_t, uint32_t>      bindings;       // variable -> Binding
      std::unordered_set<uint32_t>                inputs;         // Input variables
      std::unordered_set<uint32_t>                outputs;        // Output variables
      std::unordered_map<uint32_t, uint32_t>      floatTypes;     // type id -> width
      std::unordered_map<uint32_t, std::string>   names;          // id -> OpName
      std::vector<size_t>                         samples;        // sample instruction offsets
      std::vector<size_t>                         dielectricMixes; // mix(~0.04, x, t) offsets

      const uint32_t* inst(uint32_t id) const {
        auto it = defs.find(id);
        return it == defs.end() ? nullptr : code + it->second;
      }

      // Scalar float constant value, or all-equal composite.
      bool constantValue(uint32_t id, float& value) const {
        const uint32_t* w = inst(id);

        if (!w)
          return false;

        const uint32_t op = opcode(w[0]);

        if (op == OpConstant) {
          auto ft = floatTypes.find(w[1]);

          if (ft == floatTypes.end() || ft->second != 32)
            return false;

          std::memcpy(&value, &w[3], sizeof(float));
          return true;
        }

        if (op == OpConstantComposite) {
          const uint32_t n = wordCount(w[0]);
          bool first = true;

          for (uint32_t k = 3; k < n; k++) {
            float v = 0.0f;

            if (!constantValue(w[k], v) || (!first && v != value))
              return false;

            value = v;
            first = false;
          }

          return !first;
        }

        return false;
      }

      bool isConstant(uint32_t id, float expected) const {
        float v = 0.0f;
        return constantValue(id, v) && std::abs(v - expected) < 1.0e-6f;
      }

      // Input location a value is read from, through loads and arithmetic.
      uint32_t traceInput(uint32_t id, uint32_t depth, std::unordered_set<uint32_t>& visited) const {
        if (depth > 24 || !visited.insert(id).second)
          return ~0u;

        const uint32_t* w = inst(id);

        if (!w)
          return ~0u;

        const uint32_t op = opcode(w[0]);

        if (op == OpVariable) {
          auto loc = locations.find(id);
          return (inputs.count(id) && loc != locations.end()) ? loc->second : ~0u;
        }

        std::vector<uint32_t> operands;
        valueOperands(w, op, wordCount(w[0]), operands);

        for (uint32_t operand : operands) {
          const uint32_t loc = traceInput(operand, depth + 1, visited);

          if (loc != ~0u)
            return loc;
        }

        return ~0u;
      }

      // Descriptor a sampled image comes from.
      void traceImage(uint32_t id, D3D11VkTextureRole& role, uint32_t depth = 0) const {
        if (depth > 8)
          return;

        const uint32_t* w = inst(id);

        if (!w)
          return;

        switch (opcode(w[0])) {
          case OpSampledImage:
          case OpImage:
          case OpCopyObject:
            traceImage(w[3], role, depth + 1);
            return;

          case OpLoad:
            traceImage(w[3], role, depth + 1);
            return;

          case OpAccessChain:
          case OpInBoundsAccessChain: {
            traceImage(w[3], role, depth + 1);

            // Single-level array of descriptors: the first index selects
            // the element. An index computed at run time (bindless) leaves
            // the element unknown.
            const uint32_t* index = wordCount(w[0]) > 4 ? inst(w[4]) : nullptr;
            role.element = (index && opcode(index[0]) == OpConstant) ? index[3] : ~0u;
            return;
          }

          case OpVariable: {
            auto s = sets.find(id);
            auto b = bindings.find(id);
            auto n = names.find(id);
            role.set     = s != sets.end() ? s->second : ~0u;
            role.binding = b != bindings.end() ? b->second : ~0u;
            role.name    = n != names.end() ? n->second : std::string();
            // A plain descriptor; an access chain above overrides this.
            role.element = 0;
            return;
          }

          default:
            return;
        }
      }
    };


    // Components a value selects from a sample result: the shuffle literals
    // or extract index between the sample and `use`; all four when used
    // directly.
    std::vector<uint32_t> selectedComponents(const PixelModule& m, uint32_t sampleResult, uint32_t value) {
      if (value == sampleResult)
        return { 0, 1, 2, 3 };

      const uint32_t* w = m.inst(value);

      if (!w)
        return {};

      const uint32_t op = opcode(w[0]);
      const uint32_t n = wordCount(w[0]);

      if (op == OpVectorShuffle && w[3] == sampleResult) {
        std::vector<uint32_t> comps;

        for (uint32_t k = 5; k < n; k++)
          comps.push_back(w[k]);

        return comps;
      }

      if (op == OpCompositeExtract && w[3] == sampleResult && n > 4)
        return { w[4] };

      return {};
    }

    // The single component of a sample a scalar factor carries, through
    // splats, clamps and scaling; -1 when it is not one channel of it.
    int componentOf(const PixelModule& m, uint32_t sampleResult, uint32_t id, uint32_t depth = 0) {
      if (depth > 8)
        return -1;

      const auto comps = selectedComponents(m, sampleResult, id);

      if (comps.size() == 1)
        return int(comps[0]);

      const uint32_t* w = m.inst(id);

      if (!w)
        return -1;

      const uint32_t op = opcode(w[0]);
      const uint32_t n = wordCount(w[0]);

      switch (op) {
        case OpCompositeConstruct: {
          // Splat of one scalar.
          for (uint32_t k = 4; k < n; k++) {
            if (w[k] != w[3])
              return -1;
          }
          return n > 3 ? componentOf(m, sampleResult, w[3], depth + 1) : -1;
        }

        case OpCopyObject:
        case OpFConvert:
          return componentOf(m, sampleResult, w[3], depth + 1);

        case OpFMul:
        case OpVectorTimesScalar: {
          const int a = n > 3 ? componentOf(m, sampleResult, w[3], depth + 1) : -1;
          return a >= 0 ? a : (n > 4 ? componentOf(m, sampleResult, w[4], depth + 1) : -1);
        }

        case OpExtInst:
          if (n > 5 && w[4] == kGlslFClamp)
            return componentOf(m, sampleResult, w[5], depth + 1);
          return -1;

        default:
          return -1;
      }
    }

    // Reflection-name rules of the DX11 path (FillMaterialData): Unity HDRP
    // mask / Standard metallic-gloss map (R metallic, A smoothness), Unreal
    // ORM (G roughness, B metallic), roughness, smoothness / gloss,
    // metallic, emissive.
    void applyNameRules(D3D11VkTextureRole& role) {
      if (role.name.empty())
        return;

      std::string n = role.name;
      std::transform(n.begin(), n.end(), n.begin(), [](unsigned char c) { return char(std::tolower(c)); });
      auto has = [&](const char* w) { return n.find(w) != std::string::npos; };

      const bool unityMask = has("maskmap") || has("metallicgloss");
      const bool orm = (has("_orm") || has("occlusionroughness")
        || (n.size() >= 3 && n.compare(n.size() - 3, 3, "orm") == 0)) && !has("normal");

      if (unityMask) {
        role.metallicChannel = 0;
        if (role.smoothnessChannel < 0)
          role.smoothnessChannel = 3;
      } else if (orm) {
        role.metallicChannel = 2;
        role.roughnessChannel = 1;
      } else if (has("rough")) {
        role.roughnessChannel = 0;
      } else if (has("smoothness") || has("gloss")) {
        if (role.smoothnessChannel < 0)
          role.smoothnessChannel = 0;
      } else if (has("metallic") || has("metalness")) {
        if (role.metallicChannel < 0)
          role.metallicChannel = 0;
      } else if (has("emissive") || has("emission") || has("glow") || has("illum") || has("emittance")) {
        role.emissive = true;
      }
    }

    uint8_t encodingFromComponents(const std::vector<uint32_t>& comps) {
      if (comps.size() >= 3)
        return 2;   // RGB
      if (comps.size() == 2) {
        if (comps[0] == 0 && comps[1] == 1) return 3;   // XY
        if (comps[0] == 1 && comps[1] == 0) return 5;   // XY swapped
        if (comps[0] == 3 && comps[1] == 1) return 4;   // DXT5nm (wy)
        if (comps[0] == 1 && comps[1] == 3) return 4;
      }
      return 0;
    }


    // Ids forward-reachable from a value through arithmetic.
    std::unordered_set<uint32_t> forwardReach(const PixelModule& m, uint32_t start) {
      std::unordered_set<uint32_t> seen = { start };
      std::vector<std::pair<uint32_t, uint32_t>> queue = { { start, 0u } };

      while (!queue.empty()) {
        auto [id, depth] = queue.back();
        queue.pop_back();

        auto it = m.uses.find(id);

        if (depth > 16 || it == m.uses.end())
          continue;

        for (size_t at : it->second) {
          const uint32_t* u = m.code + at;
          const uint32_t op = opcode(u[0]);

          if (op != OpStore && hasTypedResult(op) && wordCount(u[0]) > 2 && seen.insert(u[2]).second)
            queue.push_back({ u[2], depth + 1 });
        }
      }

      return seen;
    }


    void analyzeSample(
      const PixelModule&                                 m,
            size_t                                       index,
      const std::vector<std::unordered_set<uint32_t>>&   reach,
            D3D11VkShaderAnalysis&                       out) {
      const uint32_t* s = m.code + m.samples[index];
      const uint32_t result = s[2];

      D3D11VkTextureRole role;
      m.traceImage(s[3], role);

      // Values directly derived from the sample result: the result itself
      // and its shuffles / extracts.
      std::vector<uint32_t> derived = { result };

      auto usesOf = [&](uint32_t id) -> const std::vector<size_t>& {
        static const std::vector<size_t> kNone;
        auto it = m.uses.find(id);
        return it == m.uses.end() ? kNone : it->second;
      };

      for (size_t at : usesOf(result)) {
        const uint32_t* u = m.code + at;
        const uint32_t op = opcode(u[0]);

        if ((op == OpVectorShuffle || op == OpCompositeExtract) && u[3] == result)
          derived.push_back(u[2]);
      }

      for (uint32_t value : derived) {
        for (size_t at : usesOf(value)) {
          const uint32_t* u = m.code + at;
          const uint32_t op = opcode(u[0]);
          const uint32_t n = wordCount(u[0]);

          // x * 2 then - 1 (or + -1), or fma(x, 2, -1): tangent-space unpack.
          bool unpack = false;

          if ((op == OpFMul || op == OpVectorTimesScalar) && n > 4) {
            const uint32_t other = u[3] == value ? u[4] : u[3];

            if (m.isConstant(other, 2.0f)) {
              for (size_t at2 : usesOf(u[2])) {
                const uint32_t* v = m.code + at2;
                const uint32_t op2 = opcode(v[0]);

                if (op2 == OpFSub && wordCount(v[0]) > 4 && v[3] == u[2] && m.isConstant(v[4], 1.0f))
                  unpack = true;
                if (op2 == OpFAdd && wordCount(v[0]) > 4
                 && (m.isConstant(v[3], -1.0f) || m.isConstant(v[4], -1.0f)))
                  unpack = true;
              }
            }
          }

          if (op == OpExtInst && n > 7 && u[4] == kGlslFma && u[5] == value
           && m.isConstant(u[6], 2.0f) && m.isConstant(u[7], -1.0f))
            unpack = true;

          if (unpack) {
            const uint8_t encoding = encodingFromComponents(selectedComponents(m, result, value));

            if (encoding) {
              role.normal = true;
              role.normalEncoding = encoding;
            }
          }

          // 1 - x on one channel: smoothness (roughness = 1 - smoothness).
          if (op == OpFSub && n > 4 && u[4] == value && m.isConstant(u[3], 1.0f)) {
            const auto comps = selectedComponents(m, result, value);

            if (comps.size() == 1)
              role.smoothnessChannel = int8_t(comps[0]);
          }

          // sample * input: vertex colour modulating the texture.
          if (op == OpFMul && n > 4) {
            const uint32_t other = u[3] == value ? u[4] : u[3];
            std::unordered_set<uint32_t> visited;
            const uint32_t loc = m.traceInput(other, 0, visited);

            if (loc != ~0u && loc != out.texcoordLocation && out.vertexColorLocation == ~0u)
              out.vertexColorLocation = loc;
          }
        }
      }

      // Albedo: the sample reaches a colour output through arithmetic
      // without being unpacked as a normal.
      if (!role.normal) {
        std::vector<std::pair<uint32_t, uint32_t>> queue = { { result, 0u } };
        std::unordered_set<uint32_t> seen = { result };

        while (!queue.empty() && !role.albedo) {
          auto [id, depth] = queue.back();
          queue.pop_back();

          if (depth > 12)
            continue;

          for (size_t at : usesOf(id)) {
            const uint32_t* u = m.code + at;
            const uint32_t op = opcode(u[0]);

            if (op == OpStore) {
              if (m.outputs.count(u[1]) && m.locations.count(u[1]))
                role.albedo = true;
              continue;
            }

            if (hasTypedResult(op) && wordCount(u[0]) > 2 && seen.insert(u[2]).second)
              queue.push_back({ u[2], depth + 1 });
          }
        }
      }

      // Metallic: one channel is the factor of the F0 blend between the
      // dielectric reflectance (~0.04) and the albedo. Its 1 - m in the
      // diffuse term is not smoothness.
      if (!role.normal) {
        for (size_t at : m.dielectricMixes) {
          const int c = componentOf(m, result, m.code[at + 7]);

          if (c >= 0) {
            role.metallicChannel = int8_t(c);

            if (role.smoothnessChannel == c)
              role.smoothnessChannel = -1;
            break;
          }
        }
      }

      // Emission: the sample reaches the output through an add to a lit
      // value (another texture or interpolated data) without being lit
      // itself; scaling by constants or uniforms (intensity) is allowed.
      if (role.albedo && role.metallicChannel < 0) {
        auto otherSample = [&](uint32_t id) {
          for (size_t j = 0; j < reach.size(); j++) {
            if (j != index && reach[j].count(id))
              return true;
          }
          return false;
        };

        auto lit = [&](uint32_t id) {
          if (otherSample(id))
            return true;
          std::unordered_set<uint32_t> visited;
          return m.traceInput(id, 0, visited) != ~0u;
        };

        struct Node { uint32_t id; uint32_t depth; bool added; };
        std::vector<Node> queue = { { result, 0u, false } };
        std::unordered_set<uint64_t> seen;

        while (!queue.empty() && !role.emissive) {
          const Node node = queue.back();
          queue.pop_back();

          if (node.depth > 12 || !seen.insert((uint64_t(node.id) << 1) | (node.added ? 1u : 0u)).second)
            continue;

          for (size_t at : usesOf(node.id)) {
            const uint32_t* u = m.code + at;
            const uint32_t op = opcode(u[0]);
            const uint32_t n = wordCount(u[0]);

            if (op == OpStore) {
              if (node.added && m.outputs.count(u[1]) && m.locations.count(u[1]))
                role.emissive = true;
              continue;
            }

            if (!hasTypedResult(op) || n <= 2)
              continue;

            bool added = node.added;

            if ((op == OpFMul || op == OpVectorTimesScalar || op == OpFDiv) && n > 4) {
              // Modulated by lighting or another texture before any add:
              // a lit surface term, not emission.
              if (!added && lit(u[3] == node.id ? u[4] : u[3]))
                continue;
            } else if (op == OpFAdd && n > 4) {
              if (lit(u[3] == node.id ? u[4] : u[3]))
                added = true;
            } else if (op == OpExtInst && n > 5) {
              if (u[4] == kGlslFMix && !added)
                continue;

              if (u[4] == kGlslFma && n > 7) {
                if (u[7] == node.id) {
                  if (lit(u[5]) || lit(u[6]))
                    added = true;
                } else if (!added && lit(u[5] == node.id ? u[6] : u[5])) {
                  continue;
                }
              }
            }

            queue.push_back({ u[2], node.depth + 1, added });
          }
        }
      }

      applyNameRules(role);

      if (role.normal && !out.sampledNormal) {
        out.sampledNormal = true;
        out.normalEncoding = role.normalEncoding;
      }

      if (role.smoothnessChannel >= 0 && out.smoothnessChannel < 0)
        out.smoothnessChannel = role.smoothnessChannel;

      out.textures.push_back(role);
    }


    struct ModuleInfo {
      size_t lastCapabilityEnd = kHeaderWords;
      bool   hasXfbCapability = false;
      size_t lastEntryPointEnd = 0;
      size_t firstExecutionMode = 0;
      bool   hasXfbMode = false;
      size_t annotationEnd = 0;
      size_t firstDeclaration = 0;

      uint32_t entryId = 0;
      uint32_t entryModel = ~0u;

      std::unordered_map<uint32_t, uint32_t> locations;               // id -> Location
      std::unordered_map<uint32_t, uint32_t> builtins;                // id -> BuiltIn
      std::map<std::pair<uint32_t, uint32_t>, uint32_t> memberBuiltins; // (struct, member) -> BuiltIn
      std::unordered_set<uint32_t> structsWithMemberOffsets;
      bool hasXfbDecorations = false;

      struct Pointer { uint32_t storage; uint32_t pointee; };
      struct Vector  { uint32_t component; uint32_t count; };
      std::unordered_map<uint32_t, Pointer> pointers;
      std::unordered_map<uint32_t, Vector>  vectors;
      std::unordered_map<uint32_t, uint32_t> floats;                  // id -> width

      struct Variable { uint32_t type; uint32_t storage; };
      std::unordered_map<uint32_t, Variable> variables;
    };


    bool scanModule(const uint32_t* code, size_t words, ModuleInfo& m) {
      return forEachInstruction(code, words, [&](size_t i, uint32_t op, uint32_t n) {
        const uint32_t* w = code + i;

        switch (op) {
          case OpCapability:
            m.lastCapabilityEnd = i + n;
            m.hasXfbCapability |= n > 1 && w[1] == kCapabilityTransformFeedback;
            break;

          case OpEntryPoint:
            m.lastEntryPointEnd = i + n;

            if (n > 2 && m.entryModel == ~0u
             && (w[1] == kModelVertex || w[1] == kModelTessEval || w[1] == kModelGeometry)) {
              m.entryModel = w[1];
              m.entryId = w[2];
            }
            break;

          case OpExecutionMode:
          case OpExecutionModeId:
            if (!m.firstExecutionMode)
              m.firstExecutionMode = i;
            m.hasXfbMode |= n > 2 && w[2] == kExecutionModeXfb;
            break;

          case OpDecorate:
            if (n > 3 && w[2] == kDecorationLocation) m.locations[w[1]] = w[3];
            if (n > 3 && w[2] == kDecorationBuiltIn)  m.builtins[w[1]]  = w[3];
            if (n > 2 && (w[2] == kDecorationXfbBuffer || w[2] == kDecorationXfbStride)) m.hasXfbDecorations = true;
            break;

          case OpMemberDecorate:
            if (n > 4 && w[3] == kDecorationBuiltIn) m.memberBuiltins[{ w[1], w[2] }] = w[4];
            if (n > 3 && w[3] == kDecorationOffset)  m.structsWithMemberOffsets.insert(w[1]);
            break;

          case OpTypeFloat:
            m.floats[w[1]] = w[2];
            break;

          case OpTypeVector:
            m.vectors[w[1]] = { w[2], w[3] };
            break;

          case OpTypePointer:
            m.pointers[w[1]] = { w[2], w[3] };
            break;

          case OpVariable:
            m.variables[w[2]] = { w[1], w[3] };
            break;

          default:
            break;
        }

        if (isAnnotation(op))
          m.annotationEnd = i + n;

        if (!m.firstDeclaration && op >= OpTypeVoid && op <= OpTypePointer + 1)
          m.firstDeclaration = i;
      });
    }


    bool isFloat32Vector(const ModuleInfo& m, uint32_t type, uint32_t& components) {
      auto v = m.vectors.find(type);

      if (v != m.vectors.end()) {
        auto f = m.floats.find(v->second.component);
        components = v->second.count;
        return f != m.floats.end() && f->second == 32;
      }

      auto f = m.floats.find(type);
      components = 1;
      return f != m.floats.end() && f->second == 32;
    }


    // Position: a standalone BuiltIn Position output (HLSL / dxil-spirv), or
    // a gl_PerVertex member (GLSL), then struct and member are set.
    bool findPositionOutput(const ModuleInfo& m, uint32_t& var, uint32_t& structId, uint32_t& member) {
      var = structId = member = 0;

      for (const auto& b : m.builtins) {
        if (b.second != kBuiltInPosition)
          continue;

        auto v = m.variables.find(b.first);

        if (v == m.variables.end() || v->second.storage != kStorageOutput)
          continue;

        auto ptr = m.pointers.find(v->second.type);
        uint32_t components = 0;

        if (ptr != m.pointers.end() && isFloat32Vector(m, ptr->second.pointee, components) && components == 4)
          var = b.first;
      }

      if (var)
        return true;

      for (const auto& mb : m.memberBuiltins) {
        if (mb.second != kBuiltInPosition)
          continue;

        for (const auto& v : m.variables) {
          if (v.second.storage != kStorageOutput)
            continue;

          auto ptr = m.pointers.find(v.second.type);

          if (ptr != m.pointers.end() && ptr->second.pointee == mb.first.first) {
            var      = v.first;
            structId = mb.first.first;
            member   = mb.first.second;
          }
        }
      }

      return var != 0;
    }

  }


  D3D11VkShaderAnalysis D3D11VkAnalyzePixelShader(const uint32_t* code, size_t words) {
    D3D11VkShaderAnalysis out;
    PixelModule m;
    m.code = code;

    std::vector<uint32_t> operands;

    const bool ok = forEachInstruction(code, words, [&](size_t i, uint32_t op, uint32_t n) {
      const uint32_t* w = code + i;

      if (op == OpDecorate && n > 3) {
        if (w[2] == kDecorationLocation)      m.locations[w[1]] = w[3];
        if (w[2] == kDecorationDescriptorSet) m.sets[w[1]] = w[3];
        if (w[2] == kDecorationBinding)       m.bindings[w[1]] = w[3];
      }

      if (op == OpTypeFloat && n > 2)
        m.floatTypes[w[1]] = w[2];

      // OpName: target id, then a nul-terminated UTF-8 literal.
      if (op == OpName && n > 2) {
        const char* text = reinterpret_cast<const char*>(&w[2]);
        m.names[w[1]] = std::string(text, strnlen(text, (n - 2) * sizeof(uint32_t)));
      }

      if (op == OpVariable && n > 3) {
        if (w[3] == kStorageInput)  m.inputs.insert(w[2]);
        if (w[3] == kStorageOutput) m.outputs.insert(w[2]);
      }

      if (hasTypedResult(op) && n > 2)
        m.defs[w[2]] = i;

      if (isSample(op) && n > 4)
        m.samples.push_back(i);

      // mix(F0 dielectric, albedo, metallic). Constants precede the code,
      // so the first operand is already known.
      if (op == OpExtInst && n > 7 && w[4] == kGlslFMix) {
        float f0 = 0.0f;

        if (m.constantValue(w[5], f0) && f0 > 0.02f && f0 < 0.08f)
          m.dielectricMixes.push_back(i);
      }

      // Uses, for following values forward.
      if (op == OpStore && n > 2) {
        m.uses[w[2]].push_back(i);
      } else if (hasTypedResult(op)) {
        valueOperands(w, op, n, operands);

        if (isSample(op) && n > 4)
          operands = { w[3], w[4] };

        for (uint32_t id : operands)
          m.uses[id].push_back(i);
      }
    });

    if (!ok)
      return out;

    // Texcoord: the input location most sample coordinates come from.
    std::map<uint32_t, uint32_t> votes;

    for (size_t at : m.samples) {
      std::unordered_set<uint32_t> visited;
      const uint32_t loc = m.traceInput(code[at + 4], 0, visited);

      if (loc != ~0u)
        votes[loc]++;
    }

    uint32_t bestVotes = 0;

    for (const auto& v : votes) {
      if (v.second > bestVotes) {
        out.texcoordLocation = v.first;
        bestVotes = v.second;
      }
    }

    std::vector<std::unordered_set<uint32_t>> reach;
    reach.reserve(m.samples.size());

    for (size_t at : m.samples)
      reach.push_back(forwardReach(m, code[at + 2]));

    for (size_t i = 0; i < m.samples.size(); i++)
      analyzeSample(m, i, reach, out);

    return out;
  }


  bool D3D11VkPatchSpirvForCapture(
    const uint32_t*               code,
          size_t                  words,
          uint32_t                texcoordLocation,
          uint32_t                colorLocation,
          std::vector<uint32_t>&  patched,
          D3D11VkCaptureLayout&   layout) {
    ModuleInfo m;
    layout = D3D11VkCaptureLayout();

    if (!scanModule(code, words, m) || m.entryModel == ~0u)
      return false;

    // The game's own transform feedback.
    if (m.hasXfbMode || m.hasXfbDecorations)
      return false;

    auto isFloat32Vec = [&](uint32_t type, uint32_t& components) {
      auto v = m.vectors.find(type);

      if (v != m.vectors.end()) {
        auto f = m.floats.find(v->second.component);
        components = v->second.count;
        return f != m.floats.end() && f->second == 32;
      }

      auto f = m.floats.find(type);
      components = 1;
      return f != m.floats.end() && f->second == 32;
    };

    uint32_t positionVar = 0;
    uint32_t positionStruct = 0;
    uint32_t positionMember = 0;

    if (!findPositionOutput(m, positionVar, positionStruct, positionMember))
      return false;

    // Other members of the block carrying Offset would be captured too and
    // overlap.
    if (positionStruct && m.structsWithMemberOffsets.count(positionStruct))
      return false;

    // Plain float-vector Output variable at a location.
    auto findOutput = [&](uint32_t location, uint32_t minComponents, uint32_t& var, uint32_t& components) {
      var = 0;

      if (location == ~0u)
        return;

      for (const auto& loc : m.locations) {
        if (loc.second != location)
          continue;

        auto v = m.variables.find(loc.first);

        if (v == m.variables.end() || v->second.storage != kStorageOutput)
          continue;

        auto ptr = m.pointers.find(v->second.type);
        uint32_t c = 0;

        if (ptr != m.pointers.end() && isFloat32Vec(ptr->second.pointee, c) && c >= minComponents) {
          var = loc.first;
          components = c;
        }
      }
    };

    uint32_t texVar = 0, texComponents = 0;
    uint32_t colorVar = 0, colorComponents = 0;
    findOutput(texcoordLocation, 2, texVar, texComponents);
    findOutput(colorLocation, 3, colorVar, colorComponents);

    if (colorVar == texVar)
      colorVar = 0;

    layout.texcoord        = texVar != 0;
    layout.texcoordOffset  = 16u;
    layout.color           = colorVar != 0;
    layout.colorOffset     = 16u + texComponents * 4u;
    layout.colorComponents = colorVar ? colorComponents : 0u;
    layout.stride          = 16u + texComponents * 4u + (colorVar ? colorComponents * 4u : 0u);

    // New instructions, by insertion point.
    std::vector<uint32_t> capability;
    std::vector<uint32_t> mode;
    std::vector<uint32_t> decorations;

    if (!m.hasXfbCapability)
      capability = { (2u << 16) | OpCapability, kCapabilityTransformFeedback };

    mode = { (3u << 16) | OpExecutionMode, m.entryId, kExecutionModeXfb };

    auto decorate = [&](uint32_t target, uint32_t decoration, uint32_t value) {
      decorations.insert(decorations.end(), { (4u << 16) | OpDecorate, target, decoration, value });
    };

    decorate(positionVar, kDecorationXfbBuffer, 0);
    decorate(positionVar, kDecorationXfbStride, layout.stride);

    if (positionStruct)
      decorations.insert(decorations.end(), { (5u << 16) | OpMemberDecorate, positionStruct, positionMember, kDecorationOffset, 0 });
    else
      decorate(positionVar, kDecorationOffset, 0);

    if (texVar) {
      decorate(texVar, kDecorationXfbBuffer, 0);
      decorate(texVar, kDecorationXfbStride, layout.stride);
      decorate(texVar, kDecorationOffset, layout.texcoordOffset);
    }

    if (colorVar) {
      decorate(colorVar, kDecorationXfbBuffer, 0);
      decorate(colorVar, kDecorationXfbStride, layout.stride);
      decorate(colorVar, kDecorationOffset, layout.colorOffset);
    }

    const size_t capabilityAt  = m.lastCapabilityEnd;
    const size_t modeAt        = m.firstExecutionMode ? m.firstExecutionMode : m.lastEntryPointEnd;
    const size_t decorationsAt = m.annotationEnd ? m.annotationEnd : m.firstDeclaration;

    if (!modeAt || !decorationsAt)
      return false;

    patched.clear();
    patched.reserve(words + capability.size() + mode.size() + decorations.size());

    for (size_t i = 0; i < words; i++) {
      if (i == capabilityAt)  patched.insert(patched.end(), capability.begin(), capability.end());
      if (i == modeAt)        patched.insert(patched.end(), mode.begin(), mode.end());
      if (i == decorationsAt) patched.insert(patched.end(), decorations.begin(), decorations.end());
      patched.push_back(code[i]);
    }

    return true;
  }


  bool D3D11VkPatchSpirvForBake(
    const uint32_t*               code,
          size_t                  words,
          std::vector<uint32_t>&  patched,
          uint32_t&               matrixLocation) {
    ModuleInfo m;

    if (!scanModule(code, words, m) || m.entryModel != kModelVertex)
      return false;

    uint32_t positionVar = 0, positionStruct = 0, positionMember = 0;

    if (!findPositionOutput(m, positionVar, positionStruct, positionMember))
      return false;

    // What the inserted code needs, found or declared: float, vec4, the
    // Input / Output vec4 pointers, and for a gl_PerVertex block an integer
    // constant naming the position member.
    uint32_t floatType = 0, vec4Type = 0, inPtr = 0, outPtr = 0, intType = 0, memberConst = 0;

    for (const auto& f : m.floats) {
      if (f.second == 32)
        floatType = f.first;
    }

    if (!floatType)
      return false;

    for (const auto& v : m.vectors) {
      if (v.second.component == floatType && v.second.count == 4)
        vec4Type = v.first;
    }

    size_t entryPointAt = 0, entryFunctionBegin = 0, entryFunctionEnd = 0, firstFunction = 0;
    uint32_t maxInputLocation = 0;
    bool anyInputLocation = false;
    std::vector<size_t> returns;

    const bool ok = forEachInstruction(code, words, [&](size_t i, uint32_t op, uint32_t n) {
      const uint32_t* w = code + i;

      switch (op) {
        case OpEntryPoint:
          if (n > 2 && w[2] == m.entryId && !entryPointAt)
            entryPointAt = i;
          break;

        case OpTypeInt:
          if (n > 3 && w[2] == 32 && !intType)
            intType = w[1];
          break;

        case OpTypePointer:
          if (n > 3 && vec4Type && w[3] == vec4Type) {
            if (w[2] == kStorageInput  && !inPtr)  inPtr  = w[1];
            if (w[2] == kStorageOutput && !outPtr) outPtr = w[1];
          }
          break;

        case OpFunction:
          if (!firstFunction)
            firstFunction = i;
          if (n > 2 && w[2] == m.entryId)
            entryFunctionBegin = i;
          break;

        case OpFunctionEnd:
          if (entryFunctionBegin && !entryFunctionEnd)
            entryFunctionEnd = i;
          break;

        case OpReturn:
          if (entryFunctionBegin && !entryFunctionEnd)
            returns.push_back(i);
          break;

        default:
          break;
      }
    });

    if (!ok || !entryPointAt || !entryFunctionBegin || !firstFunction || returns.empty())
      return false;

    // Integer constant equal to the member index (any 32-bit int type).
    if (positionStruct && intType) {
      forEachInstruction(code, words, [&](size_t i, uint32_t op, uint32_t n) {
        if (op == OpConstant && n == 4 && code[i + 1] == intType && code[i + 3] == positionMember && !memberConst)
          memberConst = code[i + 2];
      });
    }

    // Vertex inputs: the matrix goes past the highest location in use. Only
    // scalar / vector inputs are counted as one location each; anything
    // wider (matrices, arrays) is left alone.
    for (const auto& loc : m.locations) {
      auto v = m.variables.find(loc.first);

      if (v == m.variables.end() || v->second.storage != kStorageInput)
        continue;

      auto ptr = m.pointers.find(v->second.type);
      uint32_t components = 0;

      if (ptr == m.pointers.end())
        return false;

      if (!isFloat32Vector(m, ptr->second.pointee, components) && !m.vectors.count(ptr->second.pointee)) {
        // Integer scalars are fine too; aggregates are not.
        bool scalar = false;
        forEachInstruction(code, words, [&](size_t i, uint32_t op, uint32_t n) {
          if (op == OpTypeInt && n > 1 && code[i + 1] == ptr->second.pointee)
            scalar = true;
        });

        if (!scalar)
          return false;
      }

      maxInputLocation = std::max(maxInputLocation, loc.second);
      anyInputLocation = true;
    }

    matrixLocation = anyInputLocation ? maxInputLocation + 1 : 0;

    // Sixteen vertex attributes is the guaranteed minimum.
    if (matrixLocation + 3 >= 16)
      return false;

    uint32_t bound = code[3];
    auto newId = [&]() { return bound++; };

    std::vector<uint32_t> declarations;

    if (!vec4Type) {
      vec4Type = newId();
      declarations.insert(declarations.end(), { (4u << 16) | OpTypeVector, vec4Type, floatType, 4u });
    }

    if (!inPtr) {
      inPtr = newId();
      declarations.insert(declarations.end(), { (4u << 16) | OpTypePointer, inPtr, kStorageInput, vec4Type });
    }

    if (positionStruct) {
      if (!outPtr) {
        outPtr = newId();
        declarations.insert(declarations.end(), { (4u << 16) | OpTypePointer, outPtr, kStorageOutput, vec4Type });
      }

      if (!intType) {
        intType = newId();
        declarations.insert(declarations.end(), { (4u << 16) | OpTypeInt, intType, 32u, 1u });
      }

      if (!memberConst) {
        memberConst = newId();
        declarations.insert(declarations.end(), { (4u << 16) | OpConstant, intType, memberConst, positionMember });
      }
    }

    uint32_t rows[4];
    std::vector<uint32_t> decorations;

    for (uint32_t r = 0; r < 4; r++) {
      rows[r] = newId();
      declarations.insert(declarations.end(), { (4u << 16) | OpVariable, inPtr, rows[r], kStorageInput });
      decorations.insert(decorations.end(), { (4u << 16) | OpDecorate, rows[r], kDecorationLocation, matrixLocation + r });
    }

    // Before every return of the entry function:
    //   pos = load(position); out = (dot(row0, pos), .., dot(row3, pos))
    auto transform = [&]() {
      std::vector<uint32_t> s;
      uint32_t pointer = positionVar;

      if (positionStruct) {
        pointer = newId();
        s.insert(s.end(), { (5u << 16) | OpAccessChain, outPtr, pointer, positionVar, memberConst });
      }

      const uint32_t pos = newId();
      s.insert(s.end(), { (4u << 16) | OpLoad, vec4Type, pos, pointer });

      uint32_t dots[4];

      for (uint32_t r = 0; r < 4; r++) {
        const uint32_t row = newId();
        dots[r] = newId();
        s.insert(s.end(), { (4u << 16) | OpLoad, vec4Type, row, rows[r] });
        s.insert(s.end(), { (5u << 16) | OpDot, floatType, dots[r], row, pos });
      }

      const uint32_t result = newId();
      s.insert(s.end(), { (7u << 16) | OpCompositeConstruct, vec4Type, result, dots[0], dots[1], dots[2], dots[3] });
      s.insert(s.end(), { (3u << 16) | OpStore, pointer, result });
      return s;
    };

    const size_t decorationsAt = m.annotationEnd ? m.annotationEnd : m.firstDeclaration;

    if (!decorationsAt)
      return false;

    patched.clear();
    patched.reserve(words + declarations.size() + decorations.size() + returns.size() * 40 + 4);

    size_t nextReturn = 0;
    size_t i = 0;

    while (i < words) {
      if (i == decorationsAt) patched.insert(patched.end(), decorations.begin(), decorations.end());
      if (i == firstFunction) patched.insert(patched.end(), declarations.begin(), declarations.end());

      const uint32_t n = i >= kHeaderWords ? wordCount(code[i]) : 1u;

      if (i == entryPointAt) {
        // The interface lists the new inputs too.
        patched.push_back(((n + 4u) << 16) | OpEntryPoint);
        patched.insert(patched.end(), code + i + 1, code + i + n);
        patched.insert(patched.end(), rows, rows + 4);
        i += n;
        continue;
      }

      if (nextReturn < returns.size() && i == returns[nextReturn]) {
        const auto s = transform();
        patched.insert(patched.end(), s.begin(), s.end());
        nextReturn++;
      }

      patched.insert(patched.end(), code + i, code + i + n);
      i += n;
    }

    patched[3] = bound;
    return true;
  }


  bool D3D11VkPatchSpirvForIndirect(
    const uint32_t*                     code,
          size_t                        words,
    const remix_vkfe_indirect_constant* constants,
          uint32_t                      constantCount,
          uint32_t                      stride,
          std::vector<uint32_t>&        patched) {
    // Enums beyond the analysis' table (SPIR-V specification).
    constexpr uint32_t kStoragePushConstant = 9;
    constexpr uint32_t kStoragePhysical     = 5349;
    constexpr uint32_t kDecorationArrayStride = 6;
    constexpr uint32_t kBuiltInDrawIndex    = 4426;
    constexpr uint32_t kCapabilityInt64     = 11;
    constexpr uint32_t kCapabilityPhysical  = 5347;
    constexpr uint32_t kCapabilityDrawParameters = 4427;
    constexpr uint32_t kAddressingPhysical64 = 5348;
    constexpr uint32_t kMemoryAccessAligned = 2;

    if (!constants || !constantCount || constantCount > REMIX_VKFE_MAX_INDIRECT_CONSTANTS || !stride
     || constants[0].size < 8 || constants[0].command_offset == REMIX_VKFE_INDIRECT_COMMAND_INDEX)
      return false;

    ModuleInfo m;

    if (!scanModule(code, words, m) || m.entryModel != kModelVertex)
      return false;

    struct IntType { uint32_t width; uint32_t sign; };
    std::unordered_map<uint32_t, IntType> ints;
    std::unordered_map<uint32_t, std::vector<uint32_t>> structs;
    std::map<std::pair<uint32_t, uint32_t>, uint32_t> memberOffsets;
    std::unordered_map<uint32_t, uint32_t> arrayStrides, arrayElements, arrayLengths;
    std::unordered_map<uint32_t, uint32_t> intConstants;              // id -> value (32-bit)
    std::unordered_set<uint32_t> capabilities;
    uint32_t pcVar = 0, pcStruct = 0, addressing = 0;
    size_t memoryModelAt = 0, entryPointAt = 0, firstFunction = 0;

    const bool ok = forEachInstruction(code, words, [&](size_t i, uint32_t op, uint32_t n) {
      const uint32_t* w = code + i;

      switch (op) {
        case OpCapability:
          if (n > 1) capabilities.insert(w[1]);
          break;
        case OpMemoryModel:
          if (n > 2) { memoryModelAt = i; addressing = w[1]; }
          break;
        case OpEntryPoint:
          if (n > 2 && w[2] == m.entryId && !entryPointAt) entryPointAt = i;
          break;
        case OpDecorate:
          if (n > 3 && w[2] == kDecorationArrayStride) arrayStrides[w[1]] = w[3];
          break;
        case OpMemberDecorate:
          if (n > 4 && w[3] == kDecorationOffset) memberOffsets[{ w[1], w[2] }] = w[4];
          break;
        case OpTypeInt:
          if (n > 3) ints[w[1]] = { w[2], w[3] };
          break;
        case OpTypeStruct:
          if (n > 1) structs[w[1]].assign(w + 2, w + n);
          break;
        case OpTypeArray:
          if (n > 3) {
            arrayElements[w[1]] = w[2];
            auto c = intConstants.find(w[3]);
            if (c != intConstants.end()) arrayLengths[w[1]] = c->second;
          }
          break;
        case OpTypeRuntimeArray:
          if (n > 2) arrayElements[w[1]] = w[2];
          break;
        case OpConstant:
          if (n == 4) {
            auto t = ints.find(w[1]);
            if (t != ints.end() && t->second.width == 32) intConstants[w[2]] = w[3];
          }
          break;
        case OpVariable:
          if (n > 3 && w[3] == kStoragePushConstant) {
            auto p = m.pointers.find(w[1]);
            if (p != m.pointers.end()) { pcVar = w[2]; pcStruct = p->second.pointee; }
          }
          break;
        case OpFunction:
          if (!firstFunction) firstFunction = i;
          break;
        default:
          break;
      }
    });

    if (!ok || !entryPointAt || !memoryModelAt || !firstFunction)
      return false;

    // No push constants: nothing reads the stream's constants.
    if (!pcVar) {
      patched.assign(code, code + words);
      return true;
    }

    auto sb = structs.find(pcStruct);

    if (sb == structs.end() || (addressing != 0 && addressing != kAddressingPhysical64))
      return false;

    const std::vector<uint32_t>& members = sb->second;

    std::function<uint32_t(uint32_t)> sizeOf = [&](uint32_t type) -> uint32_t {
      auto it = ints.find(type);
      if (it != ints.end()) return it->second.width / 8;
      auto f = m.floats.find(type);
      if (f != m.floats.end()) return f->second / 8;
      auto v = m.vectors.find(type);
      if (v != m.vectors.end()) return v->second.count * sizeOf(v->second.component);
      auto a = arrayLengths.find(type);
      auto s = arrayStrides.find(type);
      if (a != arrayLengths.end() && s != arrayStrides.end()) return a->second * s->second;
      return 0;
    };

    auto is32BitScalar = [&](uint32_t type) {
      auto it = ints.find(type);
      if (it != ints.end()) return it->second.width == 32;
      auto f = m.floats.find(type);
      return f != m.floats.end() && f->second == 32;
    };

    // Range index fully containing [begin, begin + size), -1 if none; and
    // whether it overlaps any range at all.
    auto rangeOf = [&](uint32_t begin, uint32_t size, bool& overlaps) -> int {
      overlaps = false;
      for (uint32_t r = 0; r < constantCount; r++) {
        const uint32_t lo = constants[r].push_offset, hi = lo + constants[r].size;
        if (begin < hi && begin + size > lo) overlaps = true;
        if (begin >= lo && begin + size <= hi) return int(r);
      }
      return -1;
    };

    // The members holding the stream address.
    auto memberAt = [&](uint32_t offset) -> int {
      for (uint32_t k = 0; k < members.size(); k++) {
        auto o = memberOffsets.find({ pcStruct, k });
        auto t = ints.find(members[k]);
        if (o != memberOffsets.end() && o->second == offset && t != ints.end() && t->second.width == 32)
          return int(k);
      }
      return -1;
    };

    const int loMember = memberAt(constants[0].push_offset);
    const int hiMember = memberAt(constants[0].push_offset + 4);

    if (loMember < 0 || hiMember < 0)
      return false;

    struct Chain { int range; uint32_t offset; uint32_t dynamicIndex; uint32_t dynamicStride; };
    struct Rewrite { size_t at; uint32_t resultType; uint32_t result; Chain chain; };
    std::unordered_map<uint32_t, Chain> chains;
    std::vector<Rewrite> rewrites;
    bool unsupported = false;

    forEachInstruction(code, words, [&](size_t i, uint32_t op, uint32_t n) {
      const uint32_t* w = code + i;

      if ((op == OpAccessChain || op == OpInBoundsAccessChain) && n > 4 && w[3] == pcVar) {
        auto mi = intConstants.find(w[4]);

        if (mi == intConstants.end() || mi->second >= members.size()) {
          unsupported = true;   // dynamic member index into the block
          return;
        }

        auto mo = memberOffsets.find({ pcStruct, mi->second });

        if (mo == memberOffsets.end()) {
          unsupported = true;
          return;
        }

        uint32_t type = members[mi->second];
        const uint32_t memberSize = sizeOf(type);
        Chain c = { -1, mo->second, 0, 0 };
        bool dynamic = false;

        for (uint32_t k = 5; k < n; k++) {
          uint32_t element = 0, elementStride = 0;
          auto a = arrayElements.find(type);
          auto v = m.vectors.find(type);

          if (a != arrayElements.end() && arrayStrides.count(type)) {
            element = a->second;
            elementStride = arrayStrides[type];
          } else if (v != m.vectors.end()) {
            element = v->second.component;
            elementStride = sizeOf(element);
          } else {
            unsupported = true;
            return;
          }

          auto index = intConstants.find(w[k]);

          if (index != intConstants.end()) {
            c.offset += index->second * elementStride;
          } else if (!dynamic) {
            dynamic = true;
            c.dynamicIndex = w[k];
            c.dynamicStride = elementStride;
          } else {
            unsupported = true;
            return;
          }

          type = element;
        }

        // A dynamic index may land anywhere in the member.
        bool overlaps = false;
        const int range = dynamic ? rangeOf(mo->second, memberSize, overlaps) : rangeOf(c.offset, sizeOf(type), overlaps);

        if (!overlaps)
          return;

        if (range < 0 || !is32BitScalar(type) || !memberSize) {
          unsupported = true;
          return;
        }

        c.range = range;
        chains[w[2]] = c;
        return;
      }

      if (op == OpLoad && n >= 4) {
        auto c = chains.find(w[3]);

        if (c != chains.end()) {
          rewrites.push_back({ i, w[1], w[2], c->second });
          return;
        }

        if (w[3] == pcVar)
          unsupported = true;   // the whole block at once
        return;
      }

      // Any other use of a rewritten pointer (copies, calls) or of the block.
      // Declarations and debug instructions are skipped: their literals can
      // equal ids.
      constexpr uint32_t OpSource_ = 3, OpMemberName_ = 6, OpString_ = 7, OpLine_ = 8;
      constexpr uint32_t OpTypeForwardPointer_ = 39, OpConstantTrue_ = 41, OpSpecConstantOp_ = 52;

      if (op == OpName || op == OpSource_ || op == OpMemberName_ || op == OpString_ || op == OpLine_
       || isAnnotation(op) || op == OpEntryPoint || op == OpExecutionMode || op == OpExecutionModeId
       || (op >= OpTypeVoid && op <= OpTypeForwardPointer_) || (op >= OpConstantTrue_ && op <= OpSpecConstantOp_)
       || op == OpVariable)
        return;

      for (uint32_t k = 1; k < n; k++) {
        if (chains.count(w[k]) || (w[k] == pcVar && op != OpVariable)) {
          unsupported = true;
          return;
        }
      }
    });

    if (unsupported)
      return false;

    // The vertex shader does not read the stream's constants: the regular
    // capture stage serves (the address pushed into the range is ignored).
    if (rewrites.empty()) {
      patched.assign(code, code + words);
      return true;
    }

    uint32_t bound = code[3];
    auto newId = [&]() { return bound++; };
    std::vector<uint32_t> declarations;

    // Types: u32, u64, uvec2, and the pointers the new code uses.
    uint32_t u32 = 0, u64 = 0, uvec2 = 0;

    for (const auto& t : ints) {
      if (t.second.width == 32 && t.second.sign == 0 && !u32) u32 = t.first;
      if (t.second.width == 64 && t.second.sign == 0 && !u64) u64 = t.first;
    }

    if (!u32) { u32 = newId(); declarations.insert(declarations.end(), { (4u << 16) | OpTypeInt, u32, 32u, 0u }); }
    if (!u64) { u64 = newId(); declarations.insert(declarations.end(), { (4u << 16) | OpTypeInt, u64, 64u, 0u }); }

    for (const auto& v : m.vectors) {
      if (v.second.component == u32 && v.second.count == 2 && !uvec2)
        uvec2 = v.first;
    }

    if (!uvec2) { uvec2 = newId(); declarations.insert(declarations.end(), { (4u << 16) | OpTypeVector, uvec2, u32, 2u }); }

    auto findPointer = [&](uint32_t storage, uint32_t pointee) -> uint32_t {
      for (const auto& p : m.pointers) {
        if (p.second.storage == storage && p.second.pointee == pointee)
          return p.first;
      }
      return 0;
    };

    std::unordered_map<uint64_t, uint32_t> pointerTypes;   // (storage << 32 | pointee) -> id

    auto pointerTo = [&](uint32_t storage, uint32_t pointee) -> uint32_t {
      const uint64_t key = (uint64_t(storage) << 32) | pointee;
      auto it = pointerTypes.find(key);

      if (it != pointerTypes.end())
        return it->second;

      uint32_t id = findPointer(storage, pointee);

      if (!id) {
        id = newId();
        declarations.insert(declarations.end(), { (4u << 16) | OpTypePointer, id, storage, pointee });
      }

      pointerTypes[key] = id;
      return id;
    };

    // Constants.
    std::map<std::pair<uint32_t, uint64_t>, uint32_t> constantIds;

    auto constant32 = [&](uint32_t value) -> uint32_t {
      auto it = constantIds.find({ 32u, value });
      if (it != constantIds.end()) return it->second;
      const uint32_t id = newId();
      declarations.insert(declarations.end(), { (4u << 16) | OpConstant, u32, id, value });
      constantIds[{ 32u, value }] = id;
      return id;
    };

    auto constant64 = [&](uint64_t value) -> uint32_t {
      auto it = constantIds.find({ 64u, value });
      if (it != constantIds.end()) return it->second;
      const uint32_t id = newId();
      declarations.insert(declarations.end(), { (5u << 16) | OpConstant, u64, id, uint32_t(value), uint32_t(value >> 32) });
      constantIds[{ 64u, value }] = id;
      return id;
    };

    const uint32_t loPointer = pointerTo(kStoragePushConstant, members[loMember]);
    const uint32_t hiPointer = pointerTo(kStoragePushConstant, members[hiMember]);
    const uint32_t loIndex   = constant32(uint32_t(loMember));
    const uint32_t hiIndex   = constant32(uint32_t(hiMember));
    const uint32_t strideId  = constant64(stride);
    const uint32_t inputU32  = pointerTo(kStorageInput, u32);

    for (const auto& r : rewrites) {
      pointerTo(kStoragePhysical, r.resultType);

      const auto& range = constants[r.chain.range];

      if (range.command_offset != REMIX_VKFE_INDIRECT_COMMAND_INDEX)
        constant64(uint64_t(range.command_offset) + (r.chain.offset - range.push_offset));

      if (r.chain.dynamicStride)
        constant64(r.chain.dynamicStride);
    }

    const uint32_t drawIndex = newId();
    declarations.insert(declarations.end(), { (4u << 16) | OpVariable, inputU32, drawIndex, kStorageInput });

    std::vector<uint32_t> decorations = { (4u << 16) | OpDecorate, drawIndex, kDecorationBuiltIn, kBuiltInDrawIndex };

    // Capabilities and extensions, after the existing capabilities.
    std::vector<uint32_t> header;

    for (uint32_t cap : { kCapabilityInt64, kCapabilityPhysical, kCapabilityDrawParameters }) {
      if (!capabilities.count(cap))
        header.insert(header.end(), { (2u << 16) | OpCapability, cap });
    }

    auto extension = [&](const char* name) {
      const size_t length = std::strlen(name) + 1;
      const uint32_t literalWords = uint32_t((length + 3) / 4);
      std::vector<uint32_t> literal(literalWords, 0u);
      std::memcpy(literal.data(), name, length - 1);
      header.push_back(((1u + literalWords) << 16) | OpExtension);
      header.insert(header.end(), literal.begin(), literal.end());
    };

    extension("SPV_KHR_physical_storage_buffer");
    extension("SPV_KHR_shader_draw_parameters");

    // The replacement for each load: the stream address from the first
    // range, command gl_DrawID, the value's offset in the command.
    auto replacement = [&](const Rewrite& r) {
      std::vector<uint32_t> s;
      const auto& range = constants[r.chain.range];

      const uint32_t di = newId();
      s.insert(s.end(), { (4u << 16) | OpLoad, u32, di, drawIndex });

      if (range.command_offset == REMIX_VKFE_INDIRECT_COMMAND_INDEX) {
        // INCREMENTING_CONSTANT: the command index itself.
        s.insert(s.end(), { (4u << 16) | (r.resultType == u32 ? OpCopyObject : OpBitcast), r.resultType, r.result, di });
        return s;
      }

      const uint32_t loPtr = newId(), lo = newId(), hiPtr = newId(), hi = newId();
      s.insert(s.end(), { (5u << 16) | OpAccessChain, loPointer, loPtr, pcVar, loIndex });
      s.insert(s.end(), { (4u << 16) | OpLoad, members[loMember], lo, loPtr });
      s.insert(s.end(), { (5u << 16) | OpAccessChain, hiPointer, hiPtr, pcVar, hiIndex });
      s.insert(s.end(), { (4u << 16) | OpLoad, members[hiMember], hi, hiPtr });

      // The members may be signed: build the vector from u32 values.
      auto asU32 = [&](uint32_t type, uint32_t value) {
        if (type == u32)
          return value;
        const uint32_t id = newId();
        s.insert(s.end(), { (4u << 16) | OpBitcast, u32, id, value });
        return id;
      };

      const uint32_t lou = asU32(members[loMember], lo);
      const uint32_t hiu = asU32(members[hiMember], hi);

      const uint32_t pair = newId(), address = newId();
      s.insert(s.end(), { (5u << 16) | OpCompositeConstruct, uvec2, pair, lou, hiu });
      s.insert(s.end(), { (4u << 16) | OpBitcast, u64, address, pair });

      const uint32_t di64 = newId(), command = newId(), offset = newId();
      s.insert(s.end(), { (4u << 16) | OpUConvert, u64, di64, di });
      s.insert(s.end(), { (5u << 16) | OpIMul, u64, command, di64, strideId });
      s.insert(s.end(), { (5u << 16) | OpIAdd, u64, offset, command,
                          constant64(uint64_t(range.command_offset) + (r.chain.offset - range.push_offset)) });

      uint32_t total = offset;

      if (r.chain.dynamicStride) {
        const uint32_t idx = newId(), scaled = newId(), sum = newId();
        s.insert(s.end(), { (4u << 16) | OpUConvert, u64, idx, r.chain.dynamicIndex });
        s.insert(s.end(), { (5u << 16) | OpIMul, u64, scaled, idx, constant64(r.chain.dynamicStride) });
        s.insert(s.end(), { (5u << 16) | OpIAdd, u64, sum, offset, scaled });
        total = sum;
      }

      const uint32_t where = newId(), pointer = newId();
      s.insert(s.end(), { (5u << 16) | OpIAdd, u64, where, address, total });
      s.insert(s.end(), { (4u << 16) | OpConvertUToPtr, pointerTo(kStoragePhysical, r.resultType), pointer, where });
      s.insert(s.end(), { (6u << 16) | OpLoad, r.resultType, r.result, pointer, kMemoryAccessAligned, 4u });
      return s;
    };

    // OpUConvert widens: a 32-bit dynamic index that is the u64 type already
    // would be invalid, but indices are 32-bit.
    const size_t decorationsAt = m.annotationEnd ? m.annotationEnd : m.firstDeclaration;

    if (!decorationsAt)
      return false;

    // Replacement code is built first: it may still add constants and types.
    std::unordered_map<size_t, std::vector<uint32_t>> replaced;

    for (const auto& r : rewrites)
      replaced[r.at] = replacement(r);

    patched.clear();
    patched.reserve(words + header.size() + declarations.size() + rewrites.size() * 48 + 16);

    size_t i = 0;

    while (i < words) {
      if (i == m.lastCapabilityEnd) patched.insert(patched.end(), header.begin(), header.end());
      if (i == decorationsAt)       patched.insert(patched.end(), decorations.begin(), decorations.end());
      if (i == firstFunction)       patched.insert(patched.end(), declarations.begin(), declarations.end());

      const uint32_t n = i >= kHeaderWords ? wordCount(code[i]) : 1u;

      if (i == memoryModelAt) {
        patched.insert(patched.end(), code + i, code + i + n);
        patched[patched.size() - n + 1] = kAddressingPhysical64;
        i += n;
        continue;
      }

      if (i == entryPointAt) {
        patched.push_back(((n + 1u) << 16) | OpEntryPoint);
        patched.insert(patched.end(), code + i + 1, code + i + n);
        patched.push_back(drawIndex);
        i += n;
        continue;
      }

      auto rep = replaced.find(i);

      if (rep != replaced.end()) {
        patched.insert(patched.end(), rep->second.begin(), rep->second.end());
        i += n;
        continue;
      }

      patched.insert(patched.end(), code + i, code + i + n);
      i += n;
    }

    patched[3] = bound;
    return true;
  }


  void D3D11VkBuildCapturePlan(D3D11VkPipeline& pipeline) {
    pipeline.captureSupported = false;

    const auto& stages = pipeline.stages;

    auto present = [&](remix_vkfe_stage s) { return !stages[s].code.empty(); };

    // Pixel shader analysis: texcoord, vertex colour and texture roles,
    // used for capture and for materials even when capture is impossible.
    const auto& ps = stages[REMIX_VKFE_STAGE_PIXEL];

    if (ps.format == REMIX_VKFE_SHADER_SPIRV && !ps.code.empty() && ps.code.size() % 4 == 0)
      pipeline.pixelAnalysis = D3D11VkAnalyzePixelShader(
        reinterpret_cast<const uint32_t*>(ps.code.data()), ps.code.size() / 4);

    // Mesh shaders have no transform feedback (the layer hides mesh shader
    // support so engines take their vertex-shader paths).
    if (present(REMIX_VKFE_STAGE_MESH) || present(REMIX_VKFE_STAGE_TASK))
      return;

    // The last stage before the rasterizer is the one streamed out. Behind
    // tessellation or a geometry shader the vertex count is decided on the
    // GPU, so those draws are counted like indirect ones.
    const remix_vkfe_stage last = present(REMIX_VKFE_STAGE_GEOMETRY) ? REMIX_VKFE_STAGE_GEOMETRY
                                : present(REMIX_VKFE_STAGE_DOMAIN)   ? REMIX_VKFE_STAGE_DOMAIN
                                :                                      REMIX_VKFE_STAGE_VERTEX;

    pipeline.captureCountedOnGpu = last != REMIX_VKFE_STAGE_VERTEX;

    switch (pipeline.state.topology) {
      case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST:
      case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP:
      case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN:
      // Dynamic topology: counted on the GPU per draw. Point and line draws
      // then write vertex counts that are not multiples of 3 and are
      // trimmed to whole triangles; their pipelines rarely use it.
      case VK_PRIMITIVE_TOPOLOGY_MAX_ENUM:
        break;
      // Patches (tessellation) and any input a geometry shader turns into
      // triangles.
      default:
        if (!pipeline.captureCountedOnGpu)
          return;
        break;
    }

    const auto& vs = stages[last];

    if (vs.format != REMIX_VKFE_SHADER_SPIRV || vs.code.size() % 4)
      return;

    D3D11VkCaptureLayout layout;

    if (!D3D11VkPatchSpirvForCapture(reinterpret_cast<const uint32_t*>(vs.code.data()), vs.code.size() / 4,
          pipeline.pixelAnalysis.texcoordLocation, pipeline.pixelAnalysis.vertexColorLocation,
          pipeline.captureSpirv, layout))
      return;

    pipeline.captureSupported   = true;
    pipeline.captureStage       = last;
    pipeline.captureStride      = layout.stride;
    pipeline.captureHasTexcoord = layout.texcoord;
    pipeline.captureLayout      = layout;
    pipeline.capturedTopology   = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  }

}
