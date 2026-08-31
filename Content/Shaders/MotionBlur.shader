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
  
  layout(location=0) in vec2 fragTexcoord;
  layout(location=0) out vec4 outColor;
 
  void main() 
  {
      float depth = texture(depthSampler, fragTexcoord).r;
      vec2 ndc = fragTexcoord * 2.0 - 1.0;
      vec4 clipPos = vec4(ndc, depth, 1.0);
      
      vec4 viewPos = inverse(frame.projection) * clipPos;
      viewPos /= viewPos.w;

      const vec4 sourceColor = textureLod(
        colorSampler,
        fragTexcoord,
        0.0);
      vec3 color = sourceColor.xyz;
  
      vec4 worldPos = inverse(frame.view) * viewPos;

      vec4 previousClipPos = previousFrame.projection * previousFrame.view * worldPos;
      previousClipPos /= previousClipPos.w;
      
      vec2 velocity = (clipPos.xy - previousClipPos.xy) * 0.5 * data.intensity;
      vec2 velocityPixels = velocity * vec2(frame.viewportSize);
      float speedPixels = length(velocityPixels);
      if (speedPixels > data.maxSpeed && speedPixels > 0.0)
      {
          velocity *= data.maxSpeed / speedPixels;
      }
      
      if(length(velocity) <= 0.0001)
      {
          outColor = sourceColor;
          return;
      }
      
      int sampleCount = max(1, int(data.samples));
      for(int i = 1; i < sampleCount; ++i)
      {
          float t = float(i) / float(max(1, sampleCount - 1));
          vec2 texCoord = clamp(fragTexcoord - velocity * t, 0.0, 1.0);
          vec3 currentColor = textureLod(colorSampler, texCoord, 0.0).xyz;
          color += currentColor;
      }
      
      color /= float(sampleCount);
      // Preserve auxiliary HDR metadata from the current pixel. The motion
      // vector affects RGB reconstruction only; post effects such as bloom
      // consume the current pixel's classification after this pass.
      outColor = vec4(color, sourceColor.a);
  }
