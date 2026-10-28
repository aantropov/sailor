---
includes:
- Shaders/Constants.glsl

colorAttachments:
- R16G16B16A16_SFLOAT

glslCommon: |
  #version 460
  #extension GL_ARB_separate_shader_objects : enable

glslVertex: |
  layout(location=DefaultPositionBinding) in vec3 inPosition;
  void main()
  {
      gl_Position = vec4(inPosition, 1.0);
  }

glslFragment: |
  layout(set=0, binding=0) uniform FrameData
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
  layout(set=1, binding=0) uniform samplerCube environmentSampler;
  layout(location=0) out vec4 outColor;
  void main()
  {
      // Scene geometry uses a negative-height viewport.
      vec2 ndc = gl_FragCoord.xy / vec2(frame.viewportSize) * 2.0 - 1.0;
      ndc.y = -ndc.y;
      vec4 viewRay = frame.invProjection * vec4(ndc, 1.0, 1.0);
      vec3 direction = transpose(mat3(frame.view)) * normalize(viewRay.xyz / viewRay.w);
      outColor = vec4(textureLod(environmentSampler, direction, 0.0).rgb, 1.0);
  }
