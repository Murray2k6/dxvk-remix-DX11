#pragma once

#include <memory>
#include <mutex>
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

    // DX11_V281_FIXED_FUNCTION: true when this pixel shader's instruction
    // stream contains discard (HLSL clip()/discard) - DX10/11/12's actual
    // alpha-test mechanism, parsed once from the DXBC at creation.
    bool UsesDiscard() const {
      return m_usesDiscard;
    }

    // Lazily compiles and returns the xfb passthrough GS that captures the
    // VS's texcoord output (TEXCOORDn.xy, buffer 0, stride 8, rasterization
    // discarded). Returns nullptr if compilation failed or no candidate
    // exists. Thread-safe; compiles at most once per underlying shader.
    Rc<DxvkShader> GetTexcoordCaptureShader() const;

  private:

    Rc<DxvkShader> m_shader;
    Rc<DxvkBuffer> m_buffer;

    // DX11_V277_REAL_SHADER_MODEL (defaults match the old hardcoded report)
    uint32_t m_shaderModelMajor = 4;
    uint32_t m_shaderModelMinor = 0;

    // DX11_V281_FIXED_FUNCTION (parsed for pixel shaders only)
    bool m_usesDiscard = false;

    // DX11_V280_TEXCOORD_CAPTURE (null for non-VS or VS without texcoord output)
    std::shared_ptr<D3D11TexcoordCaptureState> m_texcoordCapture;

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
