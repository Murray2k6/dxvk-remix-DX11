# Remix Vulkan layer. Kept free of the DXVK runtime: it only tracks the game's
# Vulkan state and forwards to Remix's d3d11.dll through remix_vk_frontend.h,
# so loading it into a process costs nothing until Remix is enabled there.
vklayer_src = files([
  'vklayer.cpp',
  'vklayer_dispatch.cpp',
  'vklayer_track.cpp',
  'vklayer_commands.cpp',
])

vklayer_dll = shared_library('remix_vk_layer', vklayer_src,
  name_prefix         : '',
  include_directories : [ dxvk_include_path, remix_api_include_path ],
  install             : true,
  vs_module_defs      : 'remix_vk_layer.def',
  override_options    : ['cpp_std='+dxvk_cpp_std])

# Next to the DLL in the build folder; the root meson.build stages both.
configure_file(
  input         : 'remix_vk_layer.json',
  output        : 'remix_vk_layer.json',
  copy          : true)
