glslCommon: |
  #version 450
glslCompute: |
  layout(local_size_x = 4) in;
  layout(set = 0, binding = 0, std430) readonly buffer InputValue
  {
    uint values[];
  } inputs[2];
  layout(set = 1, binding = 0, std430) writeonly buffer OutputValue
  {
    uint values[];
  } outputValue;

  void main()
  {
    uint index = gl_GlobalInvocationID.x;
    outputValue.values[index] = inputs[0].values[index];
    outputValue.values[4u + index] = inputs[1].values[index];
  }
