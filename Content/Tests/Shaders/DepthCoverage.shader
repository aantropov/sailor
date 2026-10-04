---
depthStencilAttachment: D32_SFLOAT_S8_UINT

glslCommon: |
  #version 450
  layout(push_constant) uniform Coverage
  {
    float depth;
    uint sampleMask;
    uvec2 edge;
  } coverage;

glslVertex: |
  void main()
  {
    const vec2 positions[3] = vec2[](vec2(-1, -1), vec2(3, -1), vec2(-1, 3));
    gl_Position = vec4(positions[gl_VertexIndex], coverage.depth, 1.0);
  }

glslFragment: |
  void main()
  {
    if (any(greaterThanEqual(uvec2(gl_FragCoord.xy), coverage.edge))) discard;
    gl_SampleMask[0] = int(coverage.sampleMask);
  }
