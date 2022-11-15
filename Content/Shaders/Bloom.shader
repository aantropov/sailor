--- 
includes :
- "Shaders/Lighting.glsl"

glslCommon: |
  #version 450 core
  #extension GL_ARB_separate_shader_objects : enable

glslVertex: |
  layout(location=0) in vec3 inPosition;
  layout(location=1) in vec3 inNormal;
  layout(location=2) in vec2 inTexcoord;
  layout(location=3) in vec4 inColor;
  
  layout(location=0) out vec2 fragTexcoord;
  
  void main() 
  {
      gl_Position = vec4(inPosition, 1);
      fragTexcoord = inTexcoord;
      fragTexcoord.y = 1.0f - fragTexcoord.y;
  }
  
glslFragment: |
  layout(set = 0, binding = 0) uniform FrameData
  {
      mat4 view;
      mat4 projection;
      mat4 invProjection;
      vec4 cameraPosition;
      ivec2 viewportSize;
      float currentTime;
      float deltaTime;
  } frame;
  
  layout(set=1, binding=0) uniform PostProcessDataUBO
  {
    vec4 border;
  } data;
  
  layout(set=1, binding=1) uniform sampler2D colorSampler;
  
  layout(location = 0) in vec2 fragTexcoord;
  layout(location = 0) out vec4 outColor;
  
  void main() 
  {
    outColor = texture(colorSampler, fragTexcoord);
    if(length(outColor.xyz) < data.border.x)
    {
        outColor.xyz = vec3(0);
    }    
  }
