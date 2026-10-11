colorAttachments: [R32G32B32A32_SFLOAT]
defines: [HORIZONTAL, READ_FAILURE_INPUT]
glslCommon: |
  #version 450
glslVertex: |
  layout(location = 0) in vec3 position;
  void main()
  {
    gl_Position = vec4(position, 1);
  }
glslFragment: |
  layout(set = 1, binding = 1) uniform sampler2D colorSampler;
  #ifdef READ_FAILURE_INPUT
    layout(set = 0, binding = 7) uniform sampler2D failureInput;
  #endif
  layout(location = 0) out vec4 outColor;
  void main()
  {
    outColor = texelFetch(colorSampler, ivec2(gl_FragCoord.xy), 0);
  #ifdef HORIZONTAL
    outColor += vec4(0.25, 0, 0, 0);
  #else
    outColor += vec4(0, 0.5, 0, 0);
  #endif
  #ifdef READ_FAILURE_INPUT
    outColor += texelFetch(failureInput, ivec2(0), 0);
  #endif
  }
