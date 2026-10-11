defines:
- NONZERO_OFFSET
glslCommon: |
  #version 450
glslCompute: |
  layout(local_size_x = 4) in;
  layout(set = 0, binding = 0, r32f) uniform writeonly image2D outputValue;
  layout(push_constant) uniform Constants
  {
  #ifdef NONZERO_OFFSET
    layout(offset = 16) uint value;
  #else
    uint value;
  #endif
  } constants;

  void main()
  {
    uint index = gl_GlobalInvocationID.x;
    imageStore(outputValue, ivec2(index, 0), vec4(float(constants.value + index)));
  }
