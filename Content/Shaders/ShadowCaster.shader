---
colorAttachments: 
- UNDEFINED

depthStencilAttachment: D16_UNORM

glslCommon: |
  #version 460
  #extension GL_ARB_separate_shader_objects : enable
   
glslVertex: |
  layout(set = 0, binding = 0) uniform FrameData
  {
      mat4 view;
      mat4 projection;
      vec4 cameraPosition;
      ivec2 viewportSize;
      vec2 cameraParams;
      float currentTime;
      float deltaTime;
  } frame;
  
  struct PerInstanceData
  {
      mat4 model;
  };
  
  layout(std430, push_constant) uniform Constants
  {
    mat4 lightMatrix;
  } PushConstants;
  
  layout(std140, set = 1, binding = 0) readonly buffer PerInstanceDataSSBO
  {
      PerInstanceData instance[];
  } data;
  
  layout(location=0) in vec3 inPosition;
  
  void main() 
  {
      gl_Position = PushConstants.lightMatrix * data.instance[gl_InstanceIndex].model * vec4(inPosition, 1.0);
  }
  
glslFragment: |
  void main() 
  {
  }
