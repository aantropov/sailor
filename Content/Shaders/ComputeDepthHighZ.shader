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

  layout(std430, push_constant) uniform Constants
  {
    vec2 outputSize;
    // Number of output mips and the actual input sample count.
    uvec2 mipCountAndSamples;
  } PushConstants;

  #ifdef MSAA_DEPTH_INPUT
    layout(set = 0, binding = 0) uniform sampler2DMS inputDepth;
    ivec2 InputSize() { return textureSize(inputDepth); }
    float ReadDepth(ivec2 pos)
    {
      // Reverse-Z: an uncovered sample has depth 0 and must keep the pixel open.
      float depth = texelFetch(inputDepth, pos, 0).x;
      int sampleCount = int(PushConstants.mipCountAndSamples.y);
      // Keep sample indices constant for the common 2x/4x/8x attachments.
      // Zero is the minimum possible depth, so further reads cannot change it.
      if (sampleCount >= 2 && depth != 0.0)
      {
        depth = min(depth, texelFetch(inputDepth, pos, 1).x);
      }
      if (sampleCount >= 4 && depth != 0.0)
      {
        depth = min(depth, texelFetch(inputDepth, pos, 2).x);
        depth = min(depth, texelFetch(inputDepth, pos, 3).x);
      }
      if (sampleCount >= 8 && depth != 0.0)
      {
        depth = min(depth, texelFetch(inputDepth, pos, 4).x);
        depth = min(depth, texelFetch(inputDepth, pos, 5).x);
        depth = min(depth, texelFetch(inputDepth, pos, 6).x);
        depth = min(depth, texelFetch(inputDepth, pos, 7).x);
      }
      for (int sampleIndex = 8; depth != 0.0 && sampleIndex < sampleCount; ++sampleIndex)
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
  layout(set = 0, binding = 2, r32f) uniform writeonly image2D outputDepth1;
  layout(set = 0, binding = 3, r32f) uniform writeonly image2D outputDepth2;
  
  #define GROUP_SIZE 8
  shared float depthTile[GROUP_SIZE * GROUP_SIZE];

  layout(local_size_x = GROUP_SIZE, local_size_y = GROUP_SIZE) in;
  float ReduceDepth(ivec2 pos, ivec2 outputSize)
  {
    ivec2 inputSize = InputSize();
    // Most texels belong to the 1:1 depth copy or a floor-halved mip.
    // Keep those paths free of integer division and dynamic loops.
    if (all(equal(inputSize, outputSize)))
    {
      return ReadDepth(pos);
    }

    ivec2 trailing = inputSize - outputSize * 2;
    if (all(greaterThanEqual(trailing, ivec2(0))) && all(lessThanEqual(trailing, ivec2(1))))
    {
      ivec2 source = pos * 2;
      float depth = ReadDepth(source);
      if (depth != 0.0)
      {
        depth = min(depth, ReadDepth(source + ivec2(1, 0)));
      }
      if (depth != 0.0)
      {
        depth = min(depth, ReadDepth(source + ivec2(0, 1)));
      }
      if (depth != 0.0)
      {
        depth = min(depth, ReadDepth(source + ivec2(1, 1)));
      }
      // A floor-halved odd extent overlaps the next source texel in every
      // footprint, not just at the edge. Its begin is still exactly pos * 2.
      if (trailing.x != 0 && depth != 0.0)
      {
        depth = min(depth, ReadDepth(source + ivec2(2, 0)));
        if (depth != 0.0)
        {
          depth = min(depth, ReadDepth(source + ivec2(2, 1)));
        }
      }
      if (trailing.y != 0 && depth != 0.0)
      {
        depth = min(depth, ReadDepth(source + ivec2(0, 2)));
        if (depth != 0.0)
        {
          depth = min(depth, ReadDepth(source + ivec2(1, 2)));
        }
      }
      if (all(equal(trailing, ivec2(1))) && depth != 0.0)
      {
        depth = min(depth, ReadDepth(source + ivec2(2, 2)));
      }
      return depth;
    }

    ivec2 sourceBegin = (pos * inputSize) / outputSize;
    ivec2 sourceEnd = (((pos + ivec2(1)) * inputSize) + outputSize - ivec2(1)) / outputSize;
    sourceEnd = min(sourceEnd, inputSize);

    // Explicitly reduce the complete source footprint. This keeps odd-sized
    // mips conservative instead of dropping the last source row or column.
    float depth = ReadDepth(sourceBegin);
    for (int y = sourceBegin.y; y < sourceEnd.y && depth != 0.0; ++y)
    {
      for (int x = sourceBegin.x; x < sourceEnd.x && depth != 0.0; ++x)
      {
        depth = min(depth, ReadDepth(ivec2(x, y)));
      }
    }

    return depth;
  }

  float ReduceTile(ivec2 pos, int width)
  {
    int first = pos.y * 2 * width + pos.x * 2;
    return min(min(depthTile[first], depthTile[first + 1]),
               min(depthTile[first + width], depthTile[first + width + 1]));
  }

  void main()
  {
    ivec2 pos = ivec2(gl_GlobalInvocationID.xy);
    ivec2 local = ivec2(gl_LocalInvocationID.xy);
    ivec2 outputSize = min(ivec2(PushConstants.outputSize), imageSize(outputDepth));
    bool valid = all(lessThan(pos, outputSize));
    // Out-of-bounds lanes must reach the barriers and contribute a neutral min.
    float depth = valid ? ReduceDepth(pos, outputSize) : 1.0;
    if (valid)
    {
      imageStore(outputDepth, pos, vec4(depth));
    }
    if (PushConstants.mipCountAndSamples.x < 2u)
    {
      return;
    }

    depthTile[gl_LocalInvocationIndex] = depth;
    barrier();
    bool second = all(lessThan(local, ivec2(GROUP_SIZE / 2)));
    float nextDepth = second ? ReduceTile(local, GROUP_SIZE) : 1.0;
    ivec2 nextPos = ivec2(gl_WorkGroupID.xy) * (GROUP_SIZE / 2) + local;
    if (second && all(lessThan(nextPos, imageSize(outputDepth1))))
    {
      imageStore(outputDepth1, nextPos, vec4(nextDepth));
    }
    if (PushConstants.mipCountAndSamples.x < 3u)
    {
      return;
    }

    // Finish reading the first tile before compacting the 4x4 intermediate.
    barrier();
    if (second)
    {
      depthTile[local.y * (GROUP_SIZE / 2) + local.x] = nextDepth;
    }
    barrier();
    ivec2 lastPos = ivec2(gl_WorkGroupID.xy) * (GROUP_SIZE / 4) + local;
    if (all(lessThan(local, ivec2(GROUP_SIZE / 4))) &&
        all(lessThan(lastPos, imageSize(outputDepth2))))
    {
      imageStore(outputDepth2, lastPos, vec4(ReduceTile(local, GROUP_SIZE / 2)));
    }
  }
