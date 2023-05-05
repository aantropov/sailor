--- 
colorAttachments :
- B8G8R8A8_SRGB

includes :
- "Shaders/Formats.glsl"

glslCommon: |
  #version 460
  #extension GL_ARB_separate_shader_objects : enable

glslVertex: |
  layout(location=0) in vec3 inPosition;
  layout(location=1) in vec3 inNormal;
  layout(location=2) in vec2 inTexcoord;
  layout(location=3) in vec4 inColor;
  
  layout(location=0) out vec2 fragTexcoord;
  layout(set=1, binding=1) uniform sampler2D colorSampler;  
  
  void main() 
  {
      gl_Position = vec4(inPosition, 1);
      fragTexcoord = inTexcoord;
  }
    
glslFragment: |
  layout(set = 0, binding = 0) uniform FrameData
  {
      mat4 view;
      mat4 projection;
      mat4 invProjection;
      vec4 cameraPosition;
      ivec2 viewportSize;
      vec2 cameraParams;
      float currentTime;
      float deltaTime;
  } frame;
  
  layout(set=1, binding=0) uniform PostProcessDataUBO
  {
    vec4 offset;
  } data;
  
  layout(set=1, binding=1) uniform sampler2D colorSampler;  
  
  layout(location = 0) in vec2 fragTexcoord;
  layout(location = 0) out vec4 outColor;
 
  void main() 
  {
     outColor = texture(colorSampler, fragTexcoord);
      
     float d = pow(abs(fragTexcoord.x - 0.5 )/ 0.5, 4);
     
     vec4 rValue = texture(colorSampler, fragTexcoord - data.offset.x * d);
     vec4 gValue = texture(colorSampler, fragTexcoord - data.offset.y * d);
     vec4 bValue = texture(colorSampler, fragTexcoord - data.offset.b * d);
      
     outColor = vec4(rValue.r, gValue.g, bValue.b, 1.0);
  }
