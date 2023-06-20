---
includes:
- Shaders/Constants.glsl
- Shaders/Math.glsl

defines: 
- VERTICAL
- HORIZONTAL

glslCommon: |
  #version 460
  #extension GL_ARB_separate_shader_objects : enable
  #extension GL_EXT_control_flow_attributes : enable
  
glslVertex: |
  layout(location=DefaultPositionBinding) in vec3 inPosition;
  layout(location=DefaultTexcoordBinding) in vec2 inTexcoord;
  
  layout(location=0) out vec2 fragTexcoord;
  layout(set=1, binding=1) uniform sampler2D depthSampler;
  layout(set=1, binding=2) uniform sampler2D aoSampler;
  
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
    float sharpness;
    float distanceScale;
    float radius;
  } data;
  
  layout(set=1, binding=1) uniform sampler2D depthSampler;
  layout(set=1, binding=2) uniform sampler2D aoSampler;
  
  layout(location=0) in vec2 fragTexcoord;
  layout(location=0) out vec4 outColor;
 
  vec4 CalcBilateral(vec2 UV, float R, float CenterD, inout float TotalW)
  {
    vec4 C = texture(aoSampler, UV);
    float D = texture(depthSampler, UV).r;

    const float Sigma = float(data.radius) * data.sharpness;
    const float Falloff = 1.0 / (2.0*Sigma*Sigma);

    float DiffD = (D - CenterD) * data.distanceScale;
    float W = exp2(-R*R*Falloff - DiffD*DiffD);
    TotalW += W;

    return C*W;
  }

  void main()
  {
    vec2 aoPixelSize = rcp(textureSize(depthSampler, 0));
    
    #if defined(HORIZONTAL)
      aoPixelSize.y = 0;
    #elif defined(VERTICAL)
      aoPixelSize.x = 0;
    #endif
    
    vec4 CenterC = texture(aoSampler, fragTexcoord);
    float CenterD =  texture(depthSampler, fragTexcoord).r;

    vec4 TotalC = CenterC;
    float TotalW = 1.0;

    for (float i = 1; i <= data.radius; ++i)
    {
        vec2 SampleUV = fragTexcoord + aoPixelSize * i;
        TotalC += CalcBilateral(SampleUV, i, CenterD, TotalW);
    }

    for (float j = 1; j <= data.radius; ++j)
    {
        vec2 SampleUV =  fragTexcoord - aoPixelSize * j;
        TotalC += CalcBilateral(SampleUV, j, CenterD, TotalW);
    }

    outColor = TotalC/TotalW;
  }
