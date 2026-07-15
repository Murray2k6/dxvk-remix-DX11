#pragma once

#include <memory>
#include <mutex>
#include <array>
#include <unordered_map>
#include <vector>

#include "../dxbc/dxbc_module.h"
#include "../dxvk/dxvk_device.h"

#include "../util/sha1/sha1_util.h"

#include "../util/util_env.h"

#include "d3d11_device_child.h"
#include "d3d11_interfaces.h"

namespace dxvk {

  class D3D11Device;

  // A common DXBC vertex-shader transform pattern writes SV_Position with
  // four dp4 instructions whose constant-buffer operands are the four rows or
  // columns of object-to-clip. Recording the exact registers avoids guessing
  // camera/world matrices from unrelated affine constants at draw time.
  struct D3D11PositionTransformMatrixBinding {
    uint32_t constantBufferSlot = 0;
    std::array<uint32_t, 4> constantRegisters = { 0, 0, 0, 0 };
  };

  struct D3D11PositionTransformBinding {
    bool valid = false;
    uint32_t matrixCount = 0;
    // Application order: matrices[0] consumes the original POSITION input;
    // matrices[1], when present, consumes matrices[0]'s homogeneous result.
    std::array<D3D11PositionTransformMatrixBinding, 2> matrices;
  };

  // Creation-time, shader-exact list of constant-buffer registers read by a
  // vertex shader. Optimized shaders often do not contain a recognizable
  // four-DP4 matrix chain, but their declared DXBC operands still tell us
  // precisely which constants can affect SV_Position. Hashing this profile at
  // draw time avoids replaying post-VS capture because unrelated bytes in a
  // large engine cbuffer changed. Dynamic indexing is represented by one
  // whole-buffer dependency for the affected slot.
  struct D3D11ConstantBufferDependency {
    uint32_t slot = 0;
    uint32_t constantRegister = 0;
    bool wholeBuffer = false;
  };

  struct D3D11ConstantBufferDependencyProfile {
    std::vector<D3D11ConstantBufferDependency> dependencies;
    bool complete = false;
  };

  // DX11_V280_TEXCOORD_CAPTURE: shared state for building a stream-output
  // "capture" variant of a game vertex shader on demand. Some engines carry no
  // TEXCOORD stream in the input layout - the UVs exist only as VERTEX SHADER
  // OUTPUTS (computed from other attributes, instance data, or constants). For
  // those draws the capture layer replays the vertex range through DXVK's
  // xfb passthrough pipeline (the same mechanism that backs D3D11
  // CreateGeometryShaderWithStreamOutput) and reads the VS's texcoord output
  // back as a real per-vertex stream. The bytecode and compile options are
  // retained at shader creation; the passthrough GS is compiled lazily on the
  // first draw that actually needs it and the bytecode is then released.
  // D3D11CommonShader objects are copied by value (module cache + COM wrapper),
  // so this state is shared via shared_ptr: one compile serves all copies.
  struct D3D11TexcoordCaptureState {
    dxvk::mutex        mutex;
    std::vector<char>  bytecode;
    DxbcOptions        options;
    std::string        semanticName;
    uint32_t           semanticIndex = 0;
    Rc<DxvkShader>     shader;
    bool               attempted = false;
  };

  struct D3D11TexcoordSemantic {
    std::string semanticName;
    uint32_t    semanticIndex = 0;
  };

  struct D3D11PositionCaptureVariant {
    Rc<DxvkShader> shader;
    bool           attempted = false;
  };

  struct D3D11SampledTexcoordSemantic {
    std::string semanticName;
    uint32_t    semanticIndex = 0;
    uint32_t    componentIndex = 0;
    bool        valid = false;
  };

  // DX11_V290_POST_VS_POSITION_CAPTURE: some D3D11 engines perform skinning,
  // morphing, instancing, and object/world/view transforms entirely inside the
  // vertex shader, then expose a resulting pre-projection world/view position
  // as a non-system POSITION output. The IA POSITION stream alone is insufficient
  // for ray tracing in those games. Retain the original VS long enough to build
  // a transform-feedback passthrough shader that captures POSITIONn.xyz.
  enum class D3D11CapturedPositionSpace : uint8_t {
    View,
    World,
  };

  struct D3D11PositionCaptureState {
    dxvk::mutex        mutex;
    std::vector<char>  bytecode;
    DxbcOptions        options;
    // Stable application shader identity used by the capture logger.  Keeping
    // it with the shared state also makes lazy GS compilation attributable
    // after the original D3D11 shader wrapper has been copied or released.
    std::string        shaderName;
    std::string        semanticName;
    uint32_t           semanticIndex = 0;
    D3D11CapturedPositionSpace positionSpace = D3D11CapturedPositionSpace::View;
    // SV_Position is the only vertex output guaranteed to contain the exact
    // geometry the rasterizer consumed. When true, stream output stores xyzw
    // clip coordinates and the RT interleaver unprojects them to view space.
    bool               homogeneousClipSpace = false;
    // When the vertex shader also exposes a TEXCOORD output, capture it in the
    // same interleaved transform-feedback record as SV_Position. This keeps UVs
    // in the exact post-index-expansion domain consumed by the rasterizer and
    // avoids a second full vertex-shader replay.
    std::string        texcoordSemanticName;
    uint32_t           texcoordSemanticIndex = 0;
    // All non-system float2-or-larger VS outputs. The bound pixel shader's
    // sample dataflow selects one at draw time; TEXCOORD0 is only the fallback
    // when a shader does not expose a traceable texture-coordinate input.
    std::vector<D3D11TexcoordSemantic> texcoordSemantics;
    // Exact constant-buffer matrix proven by DXBC dataflow to transform this
    // captured output into SV_Position. At draw time the DX11 layer factors it
    // against the active projection to recover captured-position-to-view,
    // avoiding semantic-name guesses about object/world/view space.
    D3D11PositionTransformBinding clipTransform;
    bool               loadedFromProfile = false;
    std::unordered_map<uint64_t, D3D11PositionCaptureVariant> variants;
  };

  /**
   * \brief Common shader object
   * 
   * Stores the compiled SPIR-V shader and the SHA-1
   * hash of the original DXBC shader, which can be
   * used to identify the shader.
   */
  class D3D11CommonShader {
    
  public:
    
    D3D11CommonShader();
    D3D11CommonShader(
            D3D11Device*    pDevice,
      const DxvkShaderKey*  pShaderKey,
      const DxbcModuleInfo* pDxbcModuleInfo,
      const void*           pShaderBytecode,
            size_t          BytecodeLength);
    ~D3D11CommonShader();

    Rc<DxvkShader> GetShader() const {
      return m_shader;
    }

    Rc<DxvkBuffer> GetIcb() const {
      return m_buffer;
    }

    std::string GetName() const {
      return m_shader->debugName();
    }

    // DX11_V277_REAL_SHADER_MODEL: the actual shader model parsed from the
    // DXBC version token at creation (e.g. 5.0 for vs_5_0/ps_5_0). Modern
    // D3D11 games ship SM 5.x shaders; the capture previously stamped every
    // draw as SM 4.0, misreporting the pipeline to Remix's shader heuristics.
    uint32_t GetShaderModelMajor() const {
      return m_shaderModelMajor;
    }

    uint32_t GetShaderModelMinor() const {
      return m_shaderModelMinor;
    }

    // DX11_V280_TEXCOORD_CAPTURE: true when this is a vertex shader whose
    // output signature declares a texcoord-like element, i.e. a stream-out
    // capture variant CAN be built for it (cheap check, no compilation).
    bool HasTexcoordCaptureCandidate() const {
      return m_texcoordCapture != nullptr;
    }

    bool HasPositionCaptureCandidate() const {
      return m_positionCapture != nullptr;
    }

    D3D11CapturedPositionSpace GetPositionCaptureSpace() const {
      return m_positionCapture != nullptr
        ? m_positionCapture->positionSpace
        : D3D11CapturedPositionSpace::View;
    }

    bool IsPositionCaptureHomogeneousClipSpace() const {
      return m_positionCapture != nullptr
          && m_positionCapture->homogeneousClipSpace;
    }

    bool PositionCaptureIncludesTexcoord() const {
      return m_positionCapture != nullptr
          && !m_positionCapture->texcoordSemantics.empty();
    }

    // Returns the PS input semantic proven to feed sampling from the given
    // resource slot. This metadata belongs to pixel shaders; callers may ask
    // any common shader and receive false when no mapping exists.
    bool GetSampledTexcoordSemantic(uint32_t resourceSlot,
                                    std::string& semanticName,
                                    uint32_t& semanticIndex,
                                    uint32_t& componentIndex) const;

    bool HasCompleteSampledResourceProfile() const {
      return m_sampledResourceProfileComplete;
    }

    bool SamplesResourceSlot(uint32_t resourceSlot) const {
      return resourceSlot < m_sampledResourceSlots.size()
          && m_sampledResourceSlots[resourceSlot];
    }

    // Resolves a requested PS input semantic and component pair against this
    // VS's actual output signature. An explicit resource-slot contract is
    // exact-or-fail; only the legacy no-contract path may use the shader's
    // generic texcoord-like fallback.
    bool ResolvePositionCaptureTexcoord(const std::string& requestedName,
                                        uint32_t requestedIndex,
                                        uint32_t requestedComponent,
                                        std::string& semanticName,
                                        uint32_t& semanticIndex,
                                        uint32_t& componentIndex) const;

    const D3D11PositionTransformBinding* GetPositionCaptureClipTransformBinding() const {
      return m_positionCapture != nullptr && m_positionCapture->clipTransform.valid
        ? &m_positionCapture->clipTransform
        : nullptr;
    }

    // DX11_V281_FIXED_FUNCTION: true when this pixel shader's instruction
    // stream contains discard (HLSL clip()/discard) - DX10/11/12's actual
    // alpha-test mechanism, parsed once from the DXBC at creation.
    bool UsesDiscard() const {
      return m_usesDiscard;
    }

    const D3D11PositionTransformBinding* GetPositionTransformBinding() const {
      return m_positionTransform.valid ? &m_positionTransform : nullptr;
    }

    const D3D11ConstantBufferDependencyProfile& GetConstantBufferDependencyProfile() const {
      return m_constantBufferDependencies;
    }

    // Lazily compiles and returns the xfb passthrough GS that captures the
    // VS's texcoord output (TEXCOORDn.xy, buffer 0, stride 8, rasterization
    // discarded). Returns nullptr if compilation failed or no candidate
    // exists. Thread-safe; compiles at most once per underlying shader.
    Rc<DxvkShader> GetTexcoordCaptureShader() const;

    // Lazily compiles the pure stream-output GS used to capture the game VS's
    // shader-computed pre-projection POSITIONn.xyz stream.
    Rc<DxvkShader> GetPositionCaptureShader(const std::string& texcoordSemanticName,
                                            uint32_t texcoordSemanticIndex,
                                            uint32_t texcoordComponentIndex) const;

  private:

    Rc<DxvkShader> m_shader;
    Rc<DxvkBuffer> m_buffer;

    // DX11_V277_REAL_SHADER_MODEL (defaults match the old hardcoded report)
    uint32_t m_shaderModelMajor = 4;
    uint32_t m_shaderModelMinor = 0;

    // DX11_V281_FIXED_FUNCTION (parsed for pixel shaders only)
    bool m_usesDiscard = false;

    std::array<D3D11SampledTexcoordSemantic,
      D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT> m_sampledTexcoordSemantics = {};
    std::array<bool,
      D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT> m_sampledResourceSlots = {};
    bool m_sampledResourceProfileComplete = false;

    D3D11PositionTransformBinding m_positionTransform;
    D3D11ConstantBufferDependencyProfile m_constantBufferDependencies;

    // DX11_V280_TEXCOORD_CAPTURE (null for non-VS or VS without texcoord output)
    std::shared_ptr<D3D11TexcoordCaptureState> m_texcoordCapture;

    // DX11_V290_POST_VS_POSITION_CAPTURE (null unless the output signature has
    // a conservative position candidate such as POSITION1/VIEWPOSITION).
    std::shared_ptr<D3D11PositionCaptureState> m_positionCapture;

  };
  
  
  /**
   * \brief Common shader interface
   * 
   * Implements methods for all D3D11*Shader
   * interfaces and stores the actual shader
   * module object.
   */
  template<typename D3D11Interface>
  class D3D11Shader : public D3D11DeviceChild<D3D11Interface> {

  public:
    
    D3D11Shader(D3D11Device* device, const D3D11CommonShader& shader)
    : D3D11DeviceChild<D3D11Interface>(device),
      m_shader(shader) { }
    
    ~D3D11Shader() { }
    
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) final {
      *ppvObject = nullptr;
      
      if (riid == __uuidof(IUnknown)
       || riid == __uuidof(ID3D11DeviceChild)
       || riid == __uuidof(D3D11Interface)) {
        *ppvObject = ref(this);
        return S_OK;
      }
      
      Logger::warn("D3D11Shader::QueryInterface: Unknown interface query");
      return E_NOINTERFACE;
    }
    
    const D3D11CommonShader* GetCommonShader() const {
      return &m_shader;
    }

  private:
    
    D3D11CommonShader m_shader;
    
  };
  
  using D3D11VertexShader   = D3D11Shader<ID3D11VertexShader>;
  using D3D11HullShader     = D3D11Shader<ID3D11HullShader>;
  using D3D11DomainShader   = D3D11Shader<ID3D11DomainShader>;
  using D3D11GeometryShader = D3D11Shader<ID3D11GeometryShader>;
  using D3D11PixelShader    = D3D11Shader<ID3D11PixelShader>;
  using D3D11ComputeShader  = D3D11Shader<ID3D11ComputeShader>;
  
  
  /**
   * \brief Shader module set
   * 
   * Some applications may compile the same shader multiple
   * times, so we should cache the resulting shader modules
   * and reuse them rather than creating new ones. This
   * class is thread-safe.
   */
  class D3D11ShaderModuleSet {
    
  public:
    
    D3D11ShaderModuleSet();
    ~D3D11ShaderModuleSet();
    
    HRESULT GetShaderModule(
            D3D11Device*        pDevice,
      const DxvkShaderKey*      pShaderKey,
      const DxbcModuleInfo*     pDxbcModuleInfo,
      const void*               pShaderBytecode,
            size_t              BytecodeLength,
            D3D11CommonShader*  pShader);
    
  private:
    
    dxvk::mutex m_mutex;
    
    std::unordered_map<
      DxvkShaderKey,
      D3D11CommonShader,
      DxvkHash, DxvkEq> m_modules;
    
  };
  
}
