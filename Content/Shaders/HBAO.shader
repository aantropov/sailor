---
includes:
- Shaders/Constants.glsl
- Shaders/Math.glsl

glslCommon: |
  #version 460
  #extension GL_ARB_separate_shader_objects : enable
  #extension GL_EXT_control_flow_attributes : enable
  
glslVertex: |
  layout(location=DefaultPositionBinding) in vec3 inPosition;
  layout(location=DefaultTexcoordBinding) in vec2 inTexcoord;
  
  layout(location=0) out vec2 fragTexcoord;
  layout(set=1, binding=1) uniform sampler2D depthSampler;
  layout(set=1, binding=2) uniform sampler2D noiseSampler;
  
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
    float occlusionRadius;
    float occlusionPower;
    float occlusionAttenuation;
    float occlusionBias;
    float noiseScale;
  } data;
  
  layout(set=1, binding=1) uniform sampler2D depthSampler;
  layout(set=1, binding=2) uniform sampler2D noiseSampler;
  
  layout(location=0) in vec2 fragTexcoord;
  layout(location=0) out vec4 outColor;
 
  const uint NumDirections = 8;
  const uint NumSamples = 8;
  const float OcclusionOffset = 0.002f;
  
  const vec2 Directions[8] = 
  {
    {0.0, 1.0},
    {1.0, 0.0},
    {0.0, -1.0},
    {-1.0, 0.0},
    {-0.7071069, 0.7071068},
    {0.7071068, 0.7071069},
    {0.7071069, -0.7071068},
    {-0.7071068, -0.7071069},
  };
  
  float TakeSmallerAbsDelta(float left, float mid, float right)
  {
    float a = mid - left;
    float b = right - mid;
    return (abs(a) < abs(b)) ? a : b;
  }

  vec3 GetViewSpacePos(vec2 uv)
  {
    float depth = texture(depthSampler, uv).r;
    vec2 projectionUv = FramebufferUvToSceneProjectionUv(uv);
    return ScreenSpaceToViewSpace(projectionUv, depth, frame.invProjection).xyz;
  }
  
  vec3 GetViewSpaceNormal(vec2 uv, vec2 depthTextureSize)
  {   
    vec2 invDepthPixelSize = rcp(depthTextureSize);
    
    vec2 uvLeft     = uv + vec2(-1.0, 0.0 ) * invDepthPixelSize.xy;
    vec2 uvRight    = uv + vec2(1.0,  0.0 ) * invDepthPixelSize.xy;
    vec2 uvDown     = uv + vec2(0.0,  -1.0) * invDepthPixelSize.xy;
    vec2 uvUp       = uv + vec2(0.0,  1.0 ) * invDepthPixelSize.xy;

    float depth      = texture(depthSampler, uv).r;
    float depthLeft  = texture(depthSampler, uvLeft).r;
    float depthRight = texture(depthSampler, uvRight).r;
    float depthDown  = texture(depthSampler, uvDown).r;
    float depthUp    = texture(depthSampler, uvUp).r;

    float depthDdx = TakeSmallerAbsDelta(depthLeft, depth, depthRight);
    float depthDdy = TakeSmallerAbsDelta(depthDown, depth, depthUp);

    vec4 mid = ScreenSpaceToViewSpace(
      FramebufferUvToSceneProjectionUv(uv),
      depth,
      frame.invProjection);
    vec4 right = ScreenSpaceToViewSpace(
      FramebufferUvToSceneProjectionUv(uvRight),
      depth + depthDdx,
      frame.invProjection) - mid;
    vec4 framebufferDown = ScreenSpaceToViewSpace(
      FramebufferUvToSceneProjectionUv(uvUp),
      depth + depthDdy,
      frame.invProjection) - mid;

    return normalize(cross(right.xyz, framebufferDown.xyz));
  }
  
  vec2 SnapTexelOffset(vec2 uvOffset, vec2 depthTextureSize)
  {
    return round(uvOffset * depthTextureSize) * rcp(depthTextureSize);
  }

  vec2 SnapTexelCenter(vec2 uv, vec2 depthTextureSize)
  {
    vec2 pixel = clamp(
      floor(uv * depthTextureSize),
      vec2(0.0f),
      depthTextureSize - vec2(1.0f));
    return (pixel + vec2(0.5f)) * rcp(depthTextureSize);
  }
  
  float SampleAO(inout float sinH, vec3 viewSpaceSamplePos, vec3 viewSpaceOriginPos, vec3 viewSpaceOriginNormal)
  {
    vec3 horizonVector = viewSpaceSamplePos - viewSpaceOriginPos;
    float horizonVectorLengthSquared = dot(horizonVector, horizonVector);
    float occlusionRadiusSquared = data.occlusionRadius * data.occlusionRadius;
    if(horizonVectorLengthSquared <= 0.000001f || horizonVectorLengthSquared >= occlusionRadiusSquared)
    {
      return 0.0f;
    }

    float sinS = clamp(dot(viewSpaceOriginNormal, normalize(horizonVector)), -1.0f, 1.0f);
    if(sinS > sinH + data.occlusionBias)
    {
        float falloffZ = 1.0f - saturate(abs(horizonVector.z) / data.occlusionRadius);
        float distanceFactor = pow(
          saturate(1.0f - horizonVectorLengthSquared / occlusionRadiusSquared),
          max(data.occlusionAttenuation, 0.0001f));

        float occlusion = (sinS - sinH) * distanceFactor * falloffZ;
        sinH = sinS;
        return occlusion;
    }

    return 0.0f;
  }
  
  float SampleRayAO(
    vec2 rayOrigin,
    vec2 direction,
    float jitter,
    vec2 sampleRadius,
    vec3 viewSpaceOriginPos,
    vec3 viewSpaceOriginNormal,
    vec2 depthTextureSize
    )
  {
    // calculate the nearest neighbour sample along the direction vector
    vec2 singleTexelStep = direction * rcp(depthTextureSize);
    direction *= sampleRadius;

    // jitter the starting position for ray marching between the nearest neighbour and the sample step size
    vec2 stepUV = SnapTexelOffset(direction * rcp(NumSamples + 1.0f), depthTextureSize);
    vec2 jitteredOffset = mix(singleTexelStep, stepUV, jitter);
    vec2 rayStart = SnapTexelCenter(rayOrigin + jitteredOffset, depthTextureSize);
    vec2 rayEnd = rayStart + direction;

    // top occlusion keeps track of the occlusion contribution of the last found occluder.
    // set to OcclusionBias value to avoid near-occluders
    float occlusion = 0.0;

    float sinH = data.occlusionBias;

    [[unroll]]
    for (uint step = 0; step < NumSamples; ++step)
    {
        vec2 uv = SnapTexelCenter(
          mix(rayStart, rayEnd, step / float(NumSamples)),
          depthTextureSize);
        vec3 viewSpaceSamplePos = GetViewSpacePos(uv);

        occlusion += SampleAO(sinH, viewSpaceSamplePos, viewSpaceOriginPos, viewSpaceOriginNormal);
    }

    return occlusion;
  }

  void main()
  {
    const float depth = texture(depthSampler, fragTexcoord).r;
    if(depth <= 0.000001f)
    {
        outColor = vec4(1.0f);
        return;
    }

    vec3 viewSpacePosition = GetViewSpacePos(fragTexcoord);
    
    const vec2 depthTextureSize = textureSize(depthSampler, 0);
    const vec2 noiseTextureSize = textureSize(noiseSampler, 0);
    
    vec3 viewSpaceNormal = normalize(GetViewSpaceNormal(fragTexcoord, depthTextureSize));
    
    viewSpacePosition += viewSpaceNormal * OcclusionOffset;
    
    vec2 noiseUv = fragTexcoord * depthTextureSize / max(noiseTextureSize, vec2(1.0f));
    vec3 noise = texture(noiseSampler, noiseUv).xyz;
    vec2 noiseOffset = (noise.xy * 2.0 - 1.0) / 4.0;

    float viewDepth = max(abs(viewSpacePosition.z), frame.cameraZNearZFar.x);
    float screenSpaceRadius =
      0.5f * depthTextureSize.y * abs(frame.projection[1][1]) * data.occlusionRadius / viewDepth;
    screenSpaceRadius = min(screenSpaceRadius, 128.0f);

    if(screenSpaceRadius < 1.0)
    {
        outColor = vec4(1.0f);
        return;
    }
    
    float occlusionFactor = 0.0;
    
    [[unroll]]
    for (uint i = 0; i < NumDirections; ++i)
    {
        vec2 direction = normalize(Directions[i] + noiseOffset);
        
        occlusionFactor += SampleRayAO(fragTexcoord,
            direction,
            noise.y,
            screenSpaceRadius * rcp(depthTextureSize),
            viewSpacePosition,
            viewSpaceNormal,
            depthTextureSize);
    }

    float averageOcclusion = saturate(occlusionFactor / float(NumDirections));
    float visibility = saturate(1.0f - averageOcclusion);
    // Treat power as the final visibility exponent so it changes contrast
    // without changing the directional normalization of the estimator.
    outColor = vec4(pow(visibility, max(data.occlusionPower, 0.0001f)));
    //outColor.xyz = viewSpaceNormal;
  }
