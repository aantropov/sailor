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
  
  float TakeSmallerAbsDelta(float Left, float Mid, float Right)
  {
    float A = Mid - Left;
    float B = Right - Mid;
    return (abs(A) < abs(B)) ? A : B;
  }

  vec3 GetViewSpacePos(vec2 UV)
  {
    float Depth = texture(depthSampler, UV).r;
    return ClipSpaceToViewSpace(vec4(UV.x, UV.y, Depth, 1.0f), frame.invProjection).xyz;
  }
  
  vec3 GetViewSpaceNormal(vec2 UV, vec2 depthTextureSize)
  {   
    vec2 InvDepthPixelSize = rcp(depthTextureSize);
    
    vec2 UVLeft     = UV + vec2(-1.0, 0.0 ) * InvDepthPixelSize.xy;
    vec2 UVRight    = UV + vec2(1.0,  0.0 ) * InvDepthPixelSize.xy;
    vec2 UVDown     = UV + vec2(0.0,  -1.0) * InvDepthPixelSize.xy;
    vec2 UVUp       = UV + vec2(0.0,  1.0 ) * InvDepthPixelSize.xy;

    float Depth      = texture(depthSampler, UV).r;
    float DepthLeft  = texture(depthSampler, UVLeft).r;
    float DepthRight = texture(depthSampler, UVRight).r;
    float DepthDown  = texture(depthSampler, UVDown).r;
    float DepthUp    = texture(depthSampler, UVUp).r;

    float DepthDdx = TakeSmallerAbsDelta(DepthLeft, Depth, DepthRight);
    float DepthDdy = TakeSmallerAbsDelta(DepthDown, Depth, DepthUp);

    vec4 Mid    =  ClipSpaceToViewSpace(vec4(UV.x, UV.y, Depth, 1.0f), frame.invProjection);
    vec4 Right  =  ClipSpaceToViewSpace(vec4(UVRight.x, UVRight.y, Depth + DepthDdx, 1.0f), frame.invProjection) - Mid;
    vec4 Up     =  ClipSpaceToViewSpace(vec4(UVUp.x, UVUp.y, Depth + DepthDdy, 1.0f), frame.invProjection) - Mid;

    return normalize(cross(Up.xyz, Right.xyz));
  }
  
  vec2 SnapTexel(vec2 uv, vec2 depthTextureSize)
  {
     return round(uv * depthTextureSize) * rcp(depthTextureSize);
  }
  
  float SampleAO(inout float SinH, vec3 ViewSpaceSamplePos, vec3 ViewSpaceOriginPos, vec3 ViewSpaceOriginNormal)
  {
    vec3 HorizonVector = ViewSpaceSamplePos - ViewSpaceOriginPos;
    float HorizonVectorLength = length(HorizonVector);

    vec3 ViewSpaceSampleTangent = HorizonVector;

    float Occlusion = 0.0f;

    float SinS = sin((PI / 2.0) - acos(dot(ViewSpaceOriginNormal, normalize(ViewSpaceSampleTangent))));

    if(HorizonVectorLength < data.occlusionRadius * data.occlusionRadius && SinS > (SinH + data.occlusionBias * 3))
    {
        float FalloffZ = 1 - saturate(abs(HorizonVector.z) * 0.005);
        float DistanceFactor = 1 - HorizonVectorLength * rcp(data.occlusionRadius * data.occlusionRadius) * rcp(data.occlusionAttenuation);

        Occlusion = (SinS - SinH) * DistanceFactor * FalloffZ;
        SinH = SinS;
    }

    return Occlusion;
  }
  
  float SampleRayAO(
    vec2 RayOrigin,
    vec2 Direction,
    float Jitter,
    vec2 SampleRadius,
    vec3 ViewSpaceOriginPos,
    vec3 ViewSpaceOriginNormal,
    vec2 depthTextureSize
    )
  {
    // calculate the nearest neighbour sample along the direction vector
    vec2 SingleTexelStep = Direction * rcp(depthTextureSize);
    Direction *= SampleRadius;

    // jitter the starting position for ray marching between the nearest neighbour and the sample step size
    vec2 StepUV = SnapTexel(Direction * rcp(NumSamples + 1.0f), depthTextureSize);
    vec2 JitteredOffset = mix(SingleTexelStep, StepUV, Jitter);
    vec2 RayStart = SnapTexel(RayOrigin + JitteredOffset, depthTextureSize);
    vec2 RayEnd = RayStart + Direction;

    // top occlusion keeps track of the occlusion contribution of the last found occluder.
    // set to OcclusionBias value to avoid near-occluders
    float Occlusion = 0.0;

    float SinH = data.occlusionBias;

    [[unroll]]
    for (uint Step = 0; Step < NumSamples; ++Step)
    {
        vec2 UV = SnapTexel(mix(RayStart, RayEnd, Step / float(NumSamples)), depthTextureSize);
        vec3 ViewSpaceSamplePos = GetViewSpacePos(UV);

        Occlusion += SampleAO(SinH, ViewSpaceSamplePos, ViewSpaceOriginPos, ViewSpaceOriginNormal);
    }

    return Occlusion;
  }

  void main()
  {
    vec3 ViewSpacePosition = GetViewSpacePos(fragTexcoord);
    
    // sky dome check
    if (ViewSpacePosition.z > 49000)
    {
        outColor = vec4(1,0,0,1);
        return;
    }
    
    const vec2 depthTextureSize = textureSize(depthSampler, 0);
    const vec2 noiseTextureSize = textureSize(noiseSampler, 0);
    
    vec3 ViewSpaceNormal = normalize(GetViewSpaceNormal(fragTexcoord, depthTextureSize));
    
    ViewSpacePosition += ViewSpaceNormal * OcclusionOffset * (1  + 0.1 * ViewSpacePosition.z / frame.cameraZNearZFar.x);
    
    vec3 Noise = texture(noiseSampler, fragTexcoord * data.noiseScale).xyz;
    vec2 NoiseOffset = (Noise.xy * 2.0 - 1.0) / 4.0;

    float SampleRadius = 0;
   
    if(true)
    {
      const float MaxAORadius_DepthScalar = 2.3;
      float ScreenSpace1Meter = length(ViewSpaceToScreenSpace(vec4(0, 1, 0, 1), frame.projection));
      float MaxAORadius = (ViewSpacePosition.z - frame.cameraZNearZFar.x) * ScreenSpace1Meter * MaxAORadius_DepthScalar;
      SampleRadius = min(data.occlusionRadius, MaxAORadius);
    }
    else
    {
      vec3 ViewSpace1Pixel = vec3(data.occlusionRadius / frame.viewportSize.x + 0.5, 0, -LinearizeDepth(ViewSpacePosition.z, frame.cameraZNearZFar.yx));
      SampleRadius = length(ScreenSpaceToViewSpace(ViewSpace1Pixel.xy, ViewSpace1Pixel.z, frame.invProjection));
    }
    
    float ProjectionScale = 50;
    float ResolutionRatio = (depthTextureSize.y / frame.viewportSize.y);
    float ScreenSpaceRadius = ((ProjectionScale * SampleRadius * ResolutionRatio) / ViewSpacePosition.z);

    if(ScreenSpaceRadius < 1.0)
    {
        outColor = vec4(1.0f);
        return;
    }
    
    float OcclusionFactor = 0.0;
    
    [[unroll]]
    for (uint i = 0; i < NumDirections; ++i)
    {
        vec2 Direction = normalize(Directions[i] + NoiseOffset);
        
        OcclusionFactor += SampleRayAO(fragTexcoord,
            Direction,
            Noise.y,
            ScreenSpaceRadius * rcp(depthTextureSize),
            ViewSpacePosition,
            ViewSpaceNormal,
            depthTextureSize);
    }

    outColor = vec4(1 - saturate((data.occlusionPower / NumDirections) * OcclusionFactor));
    //outColor.xyz = ViewSpaceNormal;
  }
