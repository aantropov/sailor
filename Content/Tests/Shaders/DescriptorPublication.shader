defines:
- SAMPLED_INPUT
- STORAGE_INPUT
- SPARSE_INPUT
- UNIFORM_INPUT
glslCommon: |
  #version 450
glslCompute: |
  layout(local_size_x = 4) in;
  #ifndef SPARSE_INPUT
    layout(set = 0, binding = 0, std430) readonly buffer NeighborValue
    {
      uint values[4];
    } neighbor;
  #endif
  #if defined(SAMPLED_INPUT)
    layout(set = 0, binding = 1) uniform sampler2D source[2];
  #elif defined(STORAGE_INPUT)
    layout(set = 0, binding = 1, rgba8) uniform readonly image2D source[2];
  #elif defined(SPARSE_INPUT)
    layout(set = 0, binding = 1) uniform sampler2D source[4];
  #elif defined(UNIFORM_INPUT)
    layout(set = 0, binding = 1, std140) uniform SourceValue
    {
      uvec4 values;
    } source;
  #else
    struct SourceData
    {
      uvec4 values;
    };
    layout(set = 0, binding = 1, std430) readonly buffer SourceValue
    {
      SourceData data[1];
    } source;
  #endif
  layout(set = 1, binding = 0, std430) writeonly buffer OutputValue
  {
    uint values[];
  } outputValue;

  void main()
  {
    uint index = gl_GlobalInvocationID.x;
    ivec2 pixel = ivec2(index, 0);
  #if defined(SAMPLED_INPUT)
    outputValue.values[index] = packUnorm4x8(texelFetch(source[0], pixel, 0));
    outputValue.values[4u + index] = packUnorm4x8(texelFetch(source[1], pixel, 0));
    outputValue.values[8u + index] = neighbor.values[index];
  #elif defined(STORAGE_INPUT)
    outputValue.values[index] = packUnorm4x8(imageLoad(source[0], pixel));
    outputValue.values[4u + index] = packUnorm4x8(imageLoad(source[1], pixel));
    outputValue.values[8u + index] = neighbor.values[index];
  #elif defined(SPARSE_INPUT)
    outputValue.values[index] = packUnorm4x8(texelFetch(source[0], pixel, 0));
    outputValue.values[4u + index] = packUnorm4x8(texelFetch(source[3], pixel, 0));
  #else
  #ifdef UNIFORM_INPUT
    outputValue.values[index] = source.values[index];
  #else
    outputValue.values[index] = source.data[0].values[index];
  #endif
    outputValue.values[4u + index] = neighbor.values[index];
  #endif
  }
