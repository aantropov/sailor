colorAttachments: [R32G32B32A32_SFLOAT]
glslCommon: |
  #version 450
glslVertex: |
  void main()
  {
    const vec2 positions[3] = vec2[](vec2(-1, -1), vec2(3, -1), vec2(-1, 3));
    gl_Position = vec4(positions[gl_VertexIndex], 0, 1);
  }
glslFragment: |
  layout(set = 1, binding = 10) uniform sampler2D g_transmissionFramebufferSampler;
  layout(location = 0) out vec4 outColor;

  void main()
  {
    outColor = texelFetch(g_transmissionFramebufferSampler, ivec2(gl_FragCoord.xy), 0);
  }
