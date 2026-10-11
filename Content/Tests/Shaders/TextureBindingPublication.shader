colorAttachments: [R8G8B8A8_UNORM]
glslCommon: |
  #version 450
glslVertex: |
  layout(location = 0) in vec3 inPosition;
  void main()
  {
    gl_Position = vec4(inPosition, 1);
  }
glslFragment: |
  struct MaterialData { vec4 color; };
  layout(std430, set = 3, binding = 0) readonly buffer MaterialBuffer
  {
    MaterialData data[1];
  } material;
  layout(location = 0) out vec4 outColor;
  void main()
  {
    outColor = material.data[0].color;
  }
