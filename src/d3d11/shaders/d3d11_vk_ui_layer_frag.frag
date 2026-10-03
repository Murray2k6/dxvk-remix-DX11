#version 450
#extension GL_EXT_samplerless_texture_functions : require

// DX12 / Vulkan front end UI layer composite (d3d11_vk_frontend.cpp): the
// game's UI draws were replayed into s_layer, cleared to zero, with their
// colour blending kept and their coverage accumulated in alpha
// (src alpha ONE, dst alpha ONE_MINUS_SRC_ALPHA; additive UI leaves alpha
// alone). The layer is premultiplied: it goes over Remix's frame.

layout(location = 0) in vec2 i_texcoord;
layout(location = 0) out vec4 o_color;

layout(set = 0, binding = 0) uniform texture2D s_layer;
layout(set = 0, binding = 1) uniform texture2D s_remix;

void main() {
  ivec2 p = ivec2(gl_FragCoord.xy);

  vec4 ui  = texelFetch(s_layer, p, 0);
  vec4 rtx = texelFetch(s_remix, p, 0);

  o_color = vec4(rtx.rgb * (1.0 - clamp(ui.a, 0.0, 1.0)) + ui.rgb, 1.0);
}
