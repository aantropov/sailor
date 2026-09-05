---
defines:
- DEBUG_MOTIONS
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
  layout(set=1, binding=2) uniform sampler2D colorSampler;
  
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
    float intensity;    
    float samples;
    float maxSpeed;
  } data;
  
  layout(set=1, binding=1) uniform sampler2D depthSampler;
  layout(set=1, binding=2) uniform sampler2D colorSampler;
  layout(set=1, binding=3) uniform sampler2D motionSampler;
  
  layout(location=0) in vec2 fragTexcoord;
  layout(location=0) out vec4 outColor;
 
  float ViewDepth(vec2 uv, vec4 motion)
  {
    if(motion.a > 0.0001) return motion.z / motion.a;
    float depth = textureLod(depthSampler, uv, 0.0).r;
    vec4 viewPosition = frame.invProjection * vec4(uv * vec2(2.0, -2.0) + vec2(-1.0, 1.0), depth, 1.0);
    return abs(viewPosition.w) > 0.000001 ? abs(viewPosition.z / viewPosition.w) : 65000.0;
  }

  vec2 CameraVelocity(vec2 uv)
  {
    float depth = textureLod(depthSampler, uv, 0.0).r;
    vec2 ndc = uv * vec2(2.0, -2.0) + vec2(-1.0, 1.0);
    vec4 viewPosition = frame.invProjection * vec4(ndc, depth, 1.0);
    // Homogeneous reconstruction also handles sky at infinity without a
    // division by zero. Camera translation then cannot drag the sky.
    vec4 oldClip = previousFrame.projection * previousFrame.view * inverse(frame.view) * viewPosition;
    if(oldClip.w <= 0.00001) return vec2(0.0);
    return (ndc - oldClip.xy / oldClip.w) * vec2(0.5, -0.5);
  }

  void main()
  {
    vec4 source = textureLod(colorSampler, fragTexcoord, 0.0);
    vec4 motion = textureLod(motionSampler, fragTexcoord, 0.0);
    vec2 velocity = motion.xy + CameraVelocity(fragTexcoord) * (1.0 - clamp(motion.a, 0.0, 1.0));
    if(frame.deltaTime <= 0.0 || any(isnan(velocity)) || any(isinf(velocity))) velocity = vec2(0.0);
  #ifdef DEBUG_MOTIONS
    outColor = vec4(vec2(0.5) + velocity * 10.0, motion.a, 1.0);
    return;
  #endif
    velocity *= max(data.intensity, 0.0);
    float speed = length(velocity * vec2(frame.viewportSize));
    float limit = max(data.maxSpeed, 0.0);
    if(speed > limit && speed > 0.0) velocity *= limit / speed;
    speed = min(speed, limit);
    if(speed < 0.5)
    {
      outColor = source;
      return;
    }
    float centerDepth = ViewDepth(fragTexcoord, motion);
    vec3 color = source.rgb;
    float weight = 1.0;
    int sampleCount = clamp(int(data.samples), 1, 32);
    for(int i = 0; i < sampleCount; ++i)
    {
      float t = (float(i) + 0.5) / float(sampleCount) - 0.5;
      vec2 uv = fragTexcoord + velocity * t;
      if(any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) continue;
      vec4 sampleMotion = textureLod(motionSampler, uv, 0.0);
      float sampleDepth = ViewDepth(uv, sampleMotion);
      float relativeDepth = abs(sampleDepth - centerDepth) / max(min(centerDepth, sampleDepth), 0.1);
      float sampleWeight = (1.0 - smoothstep(0.02, 0.10, relativeDepth)) * (1.0 - abs(t));
      color += textureLod(colorSampler, uv, 0.0).rgb * sampleWeight;
      weight += sampleWeight;
    }
    // Bloom and other HDR consumers keep the current pixel's metadata.
    outColor = vec4(color / weight, source.a);
  }
