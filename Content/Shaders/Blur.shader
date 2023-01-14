--- 
includes :
- Shaders/Math.glsl
- Shaders/Lighting.glsl

defines :
- HORIZONTAL
- VERTICAL
- RADIAL

glslCommon: |
  #version 460
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
    vec4 blurRadius;
    vec4 blurCenter;
    vec4 blurSampleCount;
  } data;
  
  layout(set=1, binding=1) uniform sampler2D colorSampler;
  
  layout(location = 0) in vec2 fragTexcoord;
  layout(location = 0) out vec4 outColor;
  
  void main() 
  {
    vec2 texelSize = 1.0f / textureSize(colorSampler, 0);
    
  #ifdef VERTICAL
    texelSize.x = 0;
  #endif

  #ifdef HORIZONTAL
    texelSize.y = 0;
  #endif
  
  #ifdef RADIAL
    
    vec2 direction = (data.blurCenter.xy - fragTexcoord.xy) * texelSize.xy * data.blurRadius.x;
    outColor = vec4(0, 0, 0, 0);
    vec2 uv = fragTexcoord.xy;
    
    for (int index = 0; index < data.blurSampleCount.x; ++index)
    {
      outColor += texture(colorSampler, uv);
      uv += direction;
    }
    
    outColor /= data.blurSampleCount.x;
  #else
    outColor.xyz = GaussianBlur(colorSampler, fragTexcoord, texelSize, uint(data.blurRadius.x));
  #endif
  
  }
