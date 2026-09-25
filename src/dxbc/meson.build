dxbc_src = files([
  'dxbc_analysis.cpp',
  'dxbc_chunk_isgn.cpp',
  'dxbc_chunk_rdef.cpp',
  'dxbc_chunk_shex.cpp',
  'dxbc_common.cpp',
  'dxbc_compiler.cpp',
  'dxbc_defs.cpp',
  'dxbc_decoder.cpp',
  'dxbc_header.cpp',
  'dxbc_module.cpp',
  'dxbc_names.cpp',
  'dxbc_options.cpp',
  'dxbc_reader.cpp',
  'dxbc_util.cpp',
])

dxbc_lib = static_library('dxbc', dxbc_src,
  # Device-derived shader options use DXVK's enabled renderer dependencies.
  # Keep these implementation dependencies out of dxbc_options.h so decoding
  # and analysis do not transitively include the renderer and USD SDK.
  dependencies        : [ dxvk_deps ],
  include_directories : [ dxvk_include_path, dxvk_shader_include_path,
                          rtxdi_include_path, remix_api_include_path,
                          rtxcr_include_path, submodules_include_path ],
  override_options    : ['cpp_std='+dxvk_cpp_std])

dxbc_dep = declare_dependency(
  link_with           : [ dxbc_lib ],
  include_directories : [ dxvk_include_path ])
