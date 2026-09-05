--- 
includes:
- Shaders/Constants.glsl
- Shaders/Math.glsl
- Shaders/Lighting.glsl

defines : ~

glslCommon: |
  #version 460
  #extension GL_ARB_separate_shader_objects : enable

glslVertex: |
  layout(location=DefaultPositionBinding) in vec3 inPosition;
  layout(location=DefaultTexcoordBinding) in vec2 inTexcoord;
  
  layout(location=0) out vec2 fragTexcoord;
  
  void main() 
  {
      gl_Position = vec4(inPosition, 1);
      fragTexcoord = inTexcoord;
  }
  
glslFragment: |
  const float earthRadius = 6371000.0f;
  const float sunAngularRadius = radians(0.266f);

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
  
  layout(set = 0, binding = 1) uniform PreviousFrameData
  {
      mat4 view;
      mat4 projection;
      mat4 invProjection;
      vec4 cameraPosition;
      ivec2 viewportSize;
      vec2 cameraZNearZFar;
      float currentTime;
      float deltaTime;
  } previousFrame;
  
  layout(set=1, binding=0) uniform PostProcessDataUBO
  {
    vec4 lightDirection;
    vec4 sunIlluminance;
    vec4 groundRadiance;
    float cloudsAttenuation1;
    float cloudsAttenuation2;
    float cloudsDensity;
    float cloudsCoverage;
    float phaseInfluence1;
    float phaseInfluence2;
    float eccentrisy1;
    float eccentrisy2;
    float fog;
    float cloudScatteringScale;
    float ambient;
    int   scatteringSteps;
    float scatteringDensity;
    float scatteringIntensity;
    float scatteringPhase;
    float sunShaftsIntensity;
    int   sunShaftsDistance;
  } data;
  
  layout(set=1, binding=6) uniform sampler2D cloudsSampler;
  
  layout(location=0) in vec2 fragTexcoord;
  layout(location=0) out vec4 outColor;
  
  vec3 NormalizeSunColor()
  {
      const vec3 illuminance = max(data.sunIlluminance.xyz, vec3(0.0f));
      const float maximum = max(
        max(illuminance.x, illuminance.y),
        illuminance.z);
      return maximum > 0.000001f
        ? illuminance / maximum
        : vec3(0.0f);
  }

  float SunDiskVisibility(vec3 dirToSun)
  {
      const float observerHeight = max(frame.cameraPosition.y, 0.0f);
      const float horizonDip = acos(clamp(
        earthRadius / (earthRadius + observerHeight),
        0.0f,
        1.0f));
      const float sunElevation = asin(clamp(dirToSun.y, -1.0f, 1.0f));
      const float normalizedElevation = clamp(
        (sunElevation + horizonDip) / sunAngularRadius,
        -1.0f,
        1.0f);
      const float circleSegment = acos(-normalizedElevation) +
        normalizedElevation * sqrt(max(
          1.0f - normalizedElevation * normalizedElevation,
          0.0f));
      return circleSegment / PI;
  }
  
  void main() 
  {
    const float blurSampleCount = data.sunShaftsDistance;
    const float blurRadius = 5.0f;
        
    const vec3 dirToSun = normalize(-data.lightDirection.xyz);
    const float sunVisibility = SunDiskVisibility(dirToSun);
    
    outColor = vec4(0, 0, 0, 0);
    
    if(data.sunShaftsIntensity == 0 ||
       sunVisibility <= 0.0f ||
       max(max(data.sunIlluminance.x, data.sunIlluminance.y), data.sunIlluminance.z) <= 0.0f)
    {
        return;
    }

    const vec4 sunClip =
      frame.projection * frame.view * vec4(dirToSun, 0.0f);
    if(sunClip.w <= 0.000001f)
    {
        return;
    }

    const vec2 sunNdc = sunClip.xy / sunClip.w;
    const vec2 sunUv = vec2(
      sunNdc.x * 0.5f + 0.5f,
      0.5f - sunNdc.y * 0.5f);
    
    vec2 texelSize = 1.0f / textureSize(cloudsSampler, 0);
    vec2 blurDirection =
      (sunUv - fragTexcoord.xy) * texelSize.xy * blurRadius;
    vec2 uv = fragTexcoord.xy;
        
    const float border = 0.51f;
    float fade = max(0, max(sunUv.x - 1.0f, sunUv.y - 1.0f));
    
    if(sunUv.x > 1 + border || sunUv.y > 1 + border ||
       sunUv.x < -border || sunUv.y < -border)
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
    

    outColor.xyz = vec3(0.005) * NormalizeSunColor();
    outColor = sunVisibility * outColor.a * outColor * mix(0, 1.0f, 1 - fade / border) * clamp(1 - outColor.r, 0, 1);
    outColor.a *= clamp(pow(texture(cloudsSampler, fragTexcoord.xy).g, 3), 0,1);    
  }
