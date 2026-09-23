colorAttachments: [R32G32B32A32_SFLOAT]
defines:
- VERTEX_COLOR
- OFFSET_COLOR
- MIXED_STAGES
glslCommon: |
  #version 450
glslVertex: |
  #ifdef VERTEX_COLOR
    layout(push_constant) uniform Constants { vec4 color; } constants;
    layout(location = 0) out vec4 vertexColor;
  #elif defined(MIXED_STAGES)
    layout(push_constant) uniform Constants { float scale; } constants;
  #endif

  void main()
  {
    const vec2 positions[3] = vec2[](vec2(-1, -1), vec2(3, -1), vec2(-1, 3));
    vec2 position = positions[gl_VertexIndex];
  #ifdef VERTEX_COLOR
    vertexColor = constants.color;
  #elif defined(MIXED_STAGES)
    position *= constants.scale;
  #endif
    gl_Position = vec4(position, 0, 1);
  }
glslFragment: |
  layout(location = 0) out vec4 outColor;
  #ifdef VERTEX_COLOR
    layout(location = 0) in vec4 vertexColor;
  #else
    layout(push_constant) uniform Constants
    {
    #if defined(OFFSET_COLOR) || defined(MIXED_STAGES)
      layout(offset = 16) vec4 color;
    #else
      vec4 color;
    #endif
    } constants;
  #endif

  void main()
  {
  #ifdef VERTEX_COLOR
    outColor = vertexColor;
  #else
    outColor = constants.color;
  #endif
  }
