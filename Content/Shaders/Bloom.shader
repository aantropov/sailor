--- 
includes :
- "Shaders/Lighting.glsl"

defines :
- FILTER
- APPLY

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
    vec4 border;
  } data;
  
  layout(set=1, binding=1) uniform sampler2D colorSampler;
  #ifdef APPLY
    layout(set=1, binding=2) uniform sampler2D destSampler;
  #endif
  
  layout(location = 0) in vec2 fragTexcoord;
  layout(location = 0) out vec4 outColor;
  
  void main() 
  {
    #if defined(FILTER)
      outColor = texture(colorSampler, fragTexcoord);      
      float bloomIntensity =  LuminanceCzm(outColor.xyz) - data.border.x;
      
      if(bloomIntensity > 0)
      {
        outColor.xyz -= max(0, bloomIntensity) * normalize(outColor.xyz);
      }
      else
      {
        outColor.xyz = vec3(0);
      }
    #elif defined(APPLY)

      vec4 sceneColor = texture(colorSampler, fragTexcoord);
      vec4 bloomColor = texture(destSampler, fragTexcoord);
      
      outColor = sceneColor;
      
      float bloomIntensity = LuminanceCzm(sceneColor.xyz) - data.border.x;
      
      if(bloomIntensity > 0)
      {
        outColor.xyz -= max(0, bloomIntensity) * normalize(sceneColor.xyz);
      }
      
      outColor.xyz += bloomColor.xyz;      
    #endif
  }
