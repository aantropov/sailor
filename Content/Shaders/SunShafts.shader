--- 
includes :
- Shaders/Math.glsl
- Shaders/Lighting.glsl

defines : ~

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
    vec4 lightDirection;
    float cloudsAttenuation1;
    float cloudsAttenuation2;
    float cloudsDensity;
    float cloudsCoverage;
    float phaseInfluence1;
    float phaseInfluence2;
    float eccentrisy1;
    float eccentrisy2;
    float fog;
    float sunIntensity;
    float ambient;
    int   scatteringSteps;
    float scatteringDensity;
    float scatteringIntensity;
    float scatteringPhase;
    float sunShaftsIntensity;
    int   sunShaftsDistance;
  } data;
  
  layout(set=1, binding=6) uniform sampler2D cloudsSampler;
  
  layout(location = 0) in vec2 fragTexcoord;
  layout(location = 0) out vec4 outColor;
  
  vec3 CalculateSunColor(vec3 sunDirection)
  {
      const vec3 GroundIlluminance = vec3(0.0499, 0.004, 4.10 * 0.00001);
      const vec3 HalfIlluminance = vec3(0.6f, 0.4490196f, 0.1588f);
      const vec3 ZenithIlluminance =  vec3(0.925, 0.861, 0.755);
      
      const float angle = dot(-sunDirection, vec3(0, 1, 0));
      
      const float border = 0.1f;
      
      const float artisticTune1 = clamp(pow((angle - border) / (1.0f - border), 0.5), 0, 1);
      const float artisticTune2 = clamp(pow(angle / border, 3), 0, 1);
      
      vec3 color = angle > border ? mix(HalfIlluminance, ZenithIlluminance, artisticTune1) : 
      mix(GroundIlluminance, HalfIlluminance, artisticTune2);
      
      return color;
  }
  
  void main() 
  {
    const float blurSampleCount = data.sunShaftsDistance;
    const float blurRadius = 5.0f;
        
    const vec3 dirToSun = normalize(-data.lightDirection.xyz);
    
    vec4 uvView = ((frame.projection * frame.view * vec4(dirToSun, 0)) + 1.0f) * 0.5f;
    uvView /= uvView.w;
    
    outColor = vec4(0, 0, 0, 0);
    
    if(data.sunShaftsIntensity == 0)
    {
        return;
    }
    
    vec2 texelSize = 1.0f / textureSize(cloudsSampler, 0);
    vec2 blurDirection = (uvView.xy - fragTexcoord.xy) * texelSize.xy * blurRadius;
    vec2 uv = fragTexcoord.xy;
        
    const float border = 0.51f;
    float fade = max(0, max(uvView.x - 1.0f, uvView.y - 1.0f));
    
    if(uvView.x > 1 + border || uvView.y > 1 + border ||
       uvView.x < -border || uvView.y < -border)
    {   
        return; 
    }
    
    for (int index = 0; index < blurSampleCount; ++index)
    {
      outColor += texture(cloudsSampler, uv);
      uv += blurDirection;
    }
    
    outColor /= blurSampleCount;
    
    outColor.a = 1 - clamp(1 - outColor.a * data.sunShaftsIntensity,0,1);
    

    outColor.xyz = vec3(0.005);
    outColor = outColor.a * outColor * mix(0, 1.0f, 1 - fade / border) * clamp(1 - outColor.r, 0, 1);
    outColor.a *= clamp(pow(texture(cloudsSampler, fragTexcoord.xy).g, 3), 0,1);    
  }