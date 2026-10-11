includes:
- Shaders/Constants.glsl
defines:
- DEPTH_INPUT
- MSAA_DEPTH_INPUT
glslCommon: |
  #version 450
  #extension GL_ARB_separate_shader_objects : enable
glslCompute: |
  // Inspired by https://vkguide.dev/docs/gpudriven/compute_culling/

  #ifdef MSAA_DEPTH_INPUT
    layout(set = 0, binding = 0) uniform sampler2DMS inputDepth;
    ivec2 InputSize() { return textureSize(inputDepth); }
    float ReadDepth(ivec2 pos)
    {
      // Reverse-Z: an uncovered sample has depth 0 and must keep the pixel open.
      float depth = texelFetch(inputDepth, pos, 0).x;
      for (int sampleIndex = 1; sampleIndex < textureSamples(inputDepth); ++sampleIndex)
      {
        depth = min(depth, texelFetch(inputDepth, pos, sampleIndex).x);
      }
      return depth;
    }
  #elif defined(DEPTH_INPUT)
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
    // Most texels belong to the 1:1 depth copy or an even 2x2 mip.
    // Keep those paths free of integer division and dynamic loops.
    if (all(equal(inputSize, outputSize)))
    {
      imageStore(outputDepth, pos, vec4(ReadDepth(pos)));
      return;
    }

    if (all(equal(inputSize, outputSize * 2)))
    {
      ivec2 source = pos * 2;
      float depth = min(ReadDepth(source), ReadDepth(source + ivec2(1, 0)));
      depth = min(depth, ReadDepth(source + ivec2(0, 1)));
      depth = min(depth, ReadDepth(source + ivec2(1, 1)));
      imageStore(outputDepth, pos, vec4(depth));
      return;
    }

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
