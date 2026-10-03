#version 450
#extension GL_EXT_samplerless_texture_functions : require

// DX12 / Vulkan front end UI composite (d3d11_vk_frontend.cpp): the game's
// HUD on top of Remix's frame. s_preUi is the game's frame right before its
// UI was drawn, s_final the frame it presented: where they differ, the UI
// covers the pixel. Blended UI moves a pixel by its opacity, so the weight
// follows the size of the change; untouched pixels show Remix's frame.

layout(location = 0) in vec2 i_texcoord;
layout(location = 0) out vec4 o_color;

layout(set = 0, binding = 0) uniform texture2D s_final;
layout(set = 0, binding = 1) uniform texture2D s_preUi;
layout(set = 0, binding = 2) uniform texture2D s_remix;

void main() {
  ivec2 p = ivec2(gl_FragCoord.xy);

  vec4 fin = texelFetch(s_final, p, 0);
  vec4 pre = texelFetch(s_preUi, p, 0);
  vec4 rtx = texelFetch(s_remix, p, 0);

  vec3 d = abs(fin.rgb - pre.rgb);
  float change = max(d.r, max(d.g, d.b));

  // Below 2/255 is quantisation noise between two encodings of one frame.
  float ui = smoothstep(2.0 / 255.0, 24.0 / 255.0, change);

  o_color = vec4(mix(rtx.rgb, fin.rgb, ui), 1.0);
}
