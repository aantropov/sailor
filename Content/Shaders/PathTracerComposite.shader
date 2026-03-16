--- 
includes:
- Shaders/Constants.glsl

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
      gl_Position = vec4(inPosition, 1.0);
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

  layout(set=1, binding=0) uniform sampler2D currentSampler;

  layout(std140, set=1, binding=1) uniform Data
  {
      vec4 fitScaleOffset;
      float blend;
  } data;
  
  layout(location=0) in vec2 fragTexcoord;
  layout(location=0) out vec4 outColor;
  
  void main() 
  {
      vec2 uv = fragTexcoord * data.fitScaleOffset.xy + data.fitScaleOffset.zw;
      vec4 current = texture(currentSampler, uv);
      outColor = vec4(current.rgb, clamp(current.a * data.blend, 0.0, 1.0));
  }
