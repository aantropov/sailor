---
glslCommon: |
  BEGIN_CODE
  #version 460
  #extension GL_ARB_separate_shader_objects : enable
   
glslVertex: |
  layout(push_constant) uniform Constants
  {
  	mat4 viewProjection;
  } PushConstants; 
  
  layout(location=0) in vec3 inPosition;
  layout(location=3) in vec4 inColor;
  layout(location=0) out vec4 fragColor;
  
  void main() 
  {
      gl_Position = PushConstants.viewProjection * vec4(inPosition, 1.0);
      fragColor = inColor;
  }

glslFragment: |
  layout(location=0) in vec4 fragColor;
  layout(location = 0) out vec4 outColor;
  
  layout(set=0, binding=1) uniform sampler2D g_defaultSampler;
  
  void main() 
  {
      outColor = fragColor;
  }
