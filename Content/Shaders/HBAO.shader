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
  const float OcclusionOffset = 0.00001f;
  
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
    return ClipSpaceToViewSpace(vec4(uv.x, uv.y, depth, 1.0f), frame.invProjection).xyz;
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

    vec4 mid    =  ClipSpaceToViewSpace(vec4(uv.x, uv.y, depth, 1.0f), frame.invProjection);
    vec4 right  =  ClipSpaceToViewSpace(vec4(uvRight.x, uvRight.y, depth + depthDdx, 1.0f), frame.invProjection) - mid;
    vec4 up     =  ClipSpaceToViewSpace(vec4(uvUp.x, uvUp.y, depth + depthDdy, 1.0f), frame.invProjection) - mid;

    return normalize(cross(up.xyz, right.xyz));
  }
  
  vec2 SnapTexel(vec2 uv, vec2 depthTextureSize)
  {
     return round(uv * depthTextureSize) * rcp(depthTextureSize);
  }
  
  float SampleAO(inout float sinH, vec3 viewSpaceSamplePos, vec3 viewSpaceOriginPos, vec3 viewSpaceOriginNormal)
  {
    vec3 horizonVector = viewSpaceSamplePos - viewSpaceOriginPos;
    float horizonVectorLength = length(horizonVector);

    vec3 viewSpaceSampleTangent = horizonVector;

    float occlusion = 0.0f;

    float sinS = sin((PI / 2.0) - acos(dot(viewSpaceOriginNormal, normalize(viewSpaceSampleTangent))));

    if(horizonVectorLength < data.occlusionRadius * data.occlusionRadius && sinS > (sinH + data.occlusionBias * 3))
    {
        float falloffZ = 1 - saturate(abs(horizonVector.z) * 0.007);
        float distanceFactor = 1 - horizonVectorLength * rcp(data.occlusionRadius * data.occlusionRadius) * rcp(data.occlusionAttenuation);

        occlusion = (sinS - sinH) * distanceFactor * falloffZ;
        sinH = sinS;
    }

    return occlusion;
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
    vec2 stepUV = SnapTexel(direction * rcp(NumSamples + 1.0f), depthTextureSize);
    vec2 jitteredOffset = mix(singleTexelStep, stepUV, jitter);
    vec2 rayStart = SnapTexel(rayOrigin + jitteredOffset, depthTextureSize);
    vec2 rayEnd = rayStart + direction;

    // top occlusion keeps track of the occlusion contribution of the last found occluder.
    // set to OcclusionBias value to avoid near-occluders
    float occlusion = 0.0;

    float sinH = data.occlusionBias;

    [[unroll]]
    for (uint step = 0; step < NumSamples; ++step)
    {
        vec2 uv = SnapTexel(mix(rayStart, rayEnd, step / float(NumSamples)), depthTextureSize);
        vec3 viewSpaceSamplePos = GetViewSpacePos(uv);

        occlusion += SampleAO(sinH, viewSpaceSamplePos, viewSpaceOriginPos, viewSpaceOriginNormal);
    }

    return occlusion;
  }

  void main()
  {
    vec3 viewSpacePosition = GetViewSpacePos(fragTexcoord);
    
    // sky dome check
    if (viewSpacePosition.z > 49000)
    {
        outColor = vec4(1,0,0,1);
        return;
    }
    
    const vec2 depthTextureSize = textureSize(depthSampler, 0);
    const vec2 noiseTextureSize = textureSize(noiseSampler, 0);
    
    vec3 viewSpaceNormal = normalize(GetViewSpaceNormal(fragTexcoord, depthTextureSize));
    
    viewSpacePosition += viewSpaceNormal * OcclusionOffset * (1  + 0.1 * viewSpacePosition.z / frame.cameraZNearZFar.x);
    
    vec3 noise = texture(noiseSampler, fragTexcoord * data.noiseScale).xyz;
    vec2 noiseOffset = (noise.xy * 2.0 - 1.0) / 4.0;

    float sampleRadius = 0;
   
    if(true)
    {
      const float MaxAORadius_depthScalar = 2.3;
      float screenSpace1Meter = length(ViewSpaceToScreenSpace(vec4(0, 1, 0, 1), frame.projection));
      float maxAORadius = (viewSpacePosition.z - frame.cameraZNearZFar.x) * screenSpace1Meter * MaxAORadius_depthScalar;
      sampleRadius = min(data.occlusionRadius, maxAORadius);
    }
    else
    {
      vec3 viewSpace1Pixel = vec3(data.occlusionRadius / frame.viewportSize.x + 0.5, 0, -LinearizeDepth(viewSpacePosition.z, frame.cameraZNearZFar.yx));
      sampleRadius = length(ScreenSpaceToViewSpace(viewSpace1Pixel.xy, viewSpace1Pixel.z, frame.invProjection));
    }
    
    float projectionScale = 50;
    float resolutionRatio = (depthTextureSize.y / frame.viewportSize.y);
    float screenSpaceRadius = ((projectionScale * sampleRadius * resolutionRatio) / viewSpacePosition.z);

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

    outColor = vec4(1 - saturate((data.occlusionPower / NumDirections) * occlusionFactor));
    //outColor.xyz = viewSpaceNormal;
  }
