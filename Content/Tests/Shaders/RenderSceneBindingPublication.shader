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
  layout(set = 1, binding = 7) uniform sampler2D lightingSampler;
  layout(set = 2, binding = 2) uniform sampler2D g_transmissionFramebufferSampler;
  layout(set = 2, binding = 3) uniform sampler2D g_sceneDepthSampler;
  layout(set = 2, binding = 4) uniform sampler2D g_globalIlluminationProbeCellIndicesSampler;
  layout(location = 0) out vec4 outColor;

  void main()
  {
    ivec2 pixel = ivec2(gl_FragCoord.xy);
    outColor = vec4(texelFetch(g_transmissionFramebufferSampler, pixel, 0).r,
      texelFetch(g_sceneDepthSampler, pixel, 0).g,
      texelFetch(g_globalIlluminationProbeCellIndicesSampler, pixel, 0).b,
      texelFetch(lightingSampler, pixel, 0).a);
  }
