colorAttachments: [R32G32B32A32_SFLOAT]
depthStencilAttachment: D32_SFLOAT_S8_UINT
defines: [NO_DESCRIPTORS]
glslCommon: |
  #version 450
glslVertex: |
  layout(location = 0) in vec2 position;
  void main()
  {
    gl_Position = vec4(position, 0, 1);
  }
glslFragment: |
  layout(location = 0) out vec4 outColor;
  #ifndef NO_DESCRIPTORS
    layout(set = 4, binding = 0) uniform sampler2D source;
  #endif
  void main()
  {
  #ifdef NO_DESCRIPTORS
    outColor = vec4(0.25, 0.75, 0.125, 1);
  #else
    outColor = texelFetch(source, ivec2(0), 0);
  #endif
  }
