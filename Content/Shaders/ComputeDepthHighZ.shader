includes:
- Shaders/Constants.glsl
defines: []
glslCommon: |
  #version 450
  #extension GL_ARB_separate_shader_objects : enable
  #extension GL_EXT_shader_atomic_float : enable
glslCompute: |
  // Inspired by https://vkguide.dev/docs/gpudriven/compute_culling/

  layout(set = 0, binding = 0) uniform sampler2D inputDepth;
  layout(set = 0, binding = 1, rgba16f) uniform writeonly image2D outputDepth;
  
  layout(std430, push_constant) uniform Constants
  {
  	vec2 outputSize;
  } PushConstants;
  
  #define GROUP_SIZE 8
  
  layout(local_size_x = GROUP_SIZE, local_size_y = GROUP_SIZE) in;
  void main()
  {
    uvec2 pos = gl_GlobalInvocationID.xy;
    // Sampler is set up to do min reduction, so this computes the minimum depth of a 2x2 texel quad
    float depth = texture(inputDepth, (vec2(pos) + vec2(0.5)) / PushConstants.outputSize).x;
    imageStore(outputDepth, ivec2(pos), vec4(depth));
  }
