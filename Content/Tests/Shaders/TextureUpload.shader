glslCommon: |
  #version 450
glslCompute: |
  layout(local_size_x = 6) in;
  layout(set = 0, binding = 0) uniform samplerCube source;
  layout(set = 1, binding = 0, rgba32f) uniform writeonly image2D outputValue;

  void main()
  {
    const vec3 directions[6] = vec3[](
      vec3(1, 0, 0), vec3(-1, 0, 0), vec3(0, 1, 0),
      vec3(0, -1, 0), vec3(0, 0, 1), vec3(0, 0, -1));
    uvec2 pixel = gl_GlobalInvocationID.xy;
    imageStore(outputValue, ivec2(pixel), textureLod(source, directions[pixel.x], float(pixel.y)));
  }
