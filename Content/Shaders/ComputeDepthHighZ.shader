includes:
- Shaders/Constants.glsl
defines:
- DEPTH_INPUT
glslCommon: |
  #version 450
  #extension GL_ARB_separate_shader_objects : enable
  #extension GL_EXT_shader_atomic_float : enable
glslCompute: |
  // Inspired by https://vkguide.dev/docs/gpudriven/compute_culling/

  #ifdef DEPTH_INPUT
    layout(set = 0, binding = 0) uniform sampler2D inputDepth;
    ivec2 InputSize() { return textureSize(inputDepth, 0); }
    float ReadDepth(ivec2 pos) { return texelFetch(inputDepth, pos, 0).x; }
  #else
    layout(set = 0, binding = 0, r32f) uniform readonly image2D inputDepth;
    ivec2 InputSize() { return imageSize(inputDepth); }
    float ReadDepth(ivec2 pos) { return imageLoad(inputDepth, pos).x; }
  #endif
  layout(set = 0, binding = 1, r32f) uniform writeonly image2D outputDepth;
  
  layout(std430, push_constant) uniform Constants
  {
  	vec2 outputSize;
  } PushConstants;
  
  #define GROUP_SIZE 8
  
  layout(local_size_x = GROUP_SIZE, local_size_y = GROUP_SIZE) in;
  void main()
  {
    ivec2 pos = ivec2(gl_GlobalInvocationID.xy);
    ivec2 outputSize = min(ivec2(PushConstants.outputSize), imageSize(outputDepth));

    if (any(greaterThanEqual(pos, outputSize)))
    {
      return;
    }

    ivec2 inputSize = InputSize();
    ivec2 sourceBegin = (pos * inputSize) / outputSize;
    ivec2 sourceEnd = (((pos + ivec2(1)) * inputSize) + outputSize - ivec2(1)) / outputSize;
    sourceEnd = min(sourceEnd, inputSize);

    // Explicitly reduce the complete source footprint. This keeps odd-sized
    // mips conservative instead of dropping the last source row or column.
    float depth = ReadDepth(sourceBegin);
    for (int y = sourceBegin.y; y < sourceEnd.y; ++y)
    {
      for (int x = sourceBegin.x; x < sourceEnd.x; ++x)
      {
        depth = min(depth, ReadDepth(ivec2(x, y)));
      }
    }

    imageStore(outputDepth, pos, vec4(depth));
  }
