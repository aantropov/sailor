---
includes:
- Shaders/Constants.glsl

glslCommon: |
  #version 460
  #extension GL_ARB_separate_shader_objects : enable
   
glslVertex: |
  layout(location=DefaultPositionBinding) in vec3 inPosition;
  
  layout(set = 0, binding = 0) uniform FrameData
  {
      mat4 view;
      mat4 projection;
      mat4 invProjection;
      vec4 cameraPosition;
      ivec2 viewportSize;
      vec2 cameraZNearZFar;
      float currentTime;
      float deltaTime;
  } frame;
  
  struct PerInstanceData
  {
      mat4 model;
      vec4 sphereBounds;
      uint materialInstance;
      uint isCulled;
  };
  
  layout(std430, set = 1, binding = 0) readonly buffer PerInstanceDataSSBO
  {
      PerInstanceData instance[];
  } data;
  
  void main() 
  {
      gl_Position = frame.projection * (frame.view * (data.instance[gl_InstanceIndex].model * vec4(inPosition, 1.0)));
  }
  
glslFragment: |
  void main() 
  {
  }
