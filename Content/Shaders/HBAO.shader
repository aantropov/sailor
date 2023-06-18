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
    float occlusionRadius;
    float occlusionPower;
    float occlusionAttenuation;
    float occlusionBias;
  } data;
  
  layout(set=1, binding=1) uniform sampler2D depthSampler;  
  
  layout(location=0) in vec2 fragTexcoord;
  layout(location=0) out vec4 outColor;
 
  const uint NumDirections = 8;
  const uint NumSamples = 8;
  
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
  
  vec3 GetViewSpacePos(vec2 UV)
  {
    float Depth = texture(depthSampler, UV).r;
    return ClipSpaceToViewSpace(vec4(UV.x, UV.y, Depth, 1.0f), frame.invProjection).xyz;
  }
  
  vec2 SnapTexel(vec2 uv, float depthPixelSize)
  {
     return round(uv * depthPixelSize) * (1.0f / depthPixelSize);
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
    float depthPixelSize
    )
  {
    // calculate the nearest neighbour sample along the direction vector
    vec2 SingleTexelStep = Direction * rcp(depthPixelSize);
    Direction *= SampleRadius;

    // jitter the starting position for ray marching between the nearest neighbour and the sample step size
    vec2 StepUV = SnapTexel(Direction * rcp(NumSamples + 1.0f), depthPixelSize);
    vec2 JitteredOffset = mix(SingleTexelStep, StepUV, Jitter);
    vec2 RayStart = SnapTexel(RayOrigin + JitteredOffset, depthPixelSize);
    vec2 RayEnd = RayStart + Direction;

    // top occlusion keeps track of the occlusion contribution of the last found occluder.
    // set to OcclusionBias value to avoid near-occluders
    float Occlusion = 0.0;

    float SinH = data.occlusionBias;

    [[unroll]]
    for (uint Step = 0; Step < NumSamples; ++Step)
    {
        vec2 UV = SnapTexel(mix(RayStart, RayEnd, Step / float(NumSamples)), depthPixelSize);
        vec3 ViewSpaceSamplePos = GetViewSpacePos(UV);

        Occlusion += SampleAO(SinH, ViewSpaceSamplePos, ViewSpaceOriginPos, ViewSpaceOriginNormal);
    }

    return Occlusion;
  }


  void main()
  {
     outColor = texture(depthSampler, fragTexcoord);
  }
