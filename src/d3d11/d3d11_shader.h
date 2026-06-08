#pragma once

#include <mutex>
#include <unordered_map>

#include "../dxbc/dxbc_module.h"
#include "../dxvk/dxvk_device.h"

#include "../util/sha1/sha1_util.h"

#include "../util/util_env.h"

#include "d3d11_device_child.h"
#include "d3d11_interfaces.h"

namespace dxvk {
  
  class D3D11Device;
  
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
    
  private:
    
    Rc<DxvkShader> m_shader;
    Rc<DxvkBuffer> m_buffer;
    
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
