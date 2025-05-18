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
      float stepSize;
      float maxSteps;
      float thickness;
      float intensity;
  } data;

  layout(set=1, binding=1) uniform sampler2D colorSampler;
  layout(set=1, binding=2) uniform sampler2D depthSampler;

  layout(location=0) in vec2 fragTexcoord;
  layout(location=0) out vec4 outColor;

  vec3 ReconstructViewPos(vec2 uv, float depth)
  {
      return ScreenSpaceToViewSpace(uv, depth, frame.invProjection).xyz;
  }

  vec3 GetNormal(vec2 uv, vec2 texSize)
  {
      float depth = texture(depthSampler, uv).r;
      float depthRight = texture(depthSampler, uv + vec2(1.0 / texSize.x, 0)).r;
      float depthUp = texture(depthSampler, uv + vec2(0, 1.0 / texSize.y)).r;

      vec3 pos = ReconstructViewPos(uv, depth);
      vec3 posRight = ReconstructViewPos(uv + vec2(1.0 / texSize.x, 0), depthRight);
      vec3 posUp = ReconstructViewPos(uv + vec2(0, 1.0 / texSize.y), depthUp);

      return normalize(cross(posUp - pos, posRight - pos));
  }

  void main()
  {
      vec3 baseColor = texture(colorSampler, fragTexcoord).rgb;
      float depth = texture(depthSampler, fragTexcoord).r;

      vec2 texSize = vec2(textureSize(depthSampler, 0));
      vec3 viewPos = ReconstructViewPos(fragTexcoord, depth);
      vec3 normal = GetNormal(fragTexcoord, texSize);
      vec3 viewDir = normalize(-viewPos);
      vec3 rayDir = normalize(reflect(viewDir, normal));

      vec3 pos = viewPos + rayDir * data.thickness;
      vec3 hitColor = vec3(0.0);
      bool hit = false;

      for(int i = 0; i < int(data.maxSteps); ++i)
      {
          pos += rayDir * data.stepSize;

          vec4 proj = ViewSpaceToScreenSpace(vec4(pos, 1.0), frame.projection);
          vec2 uv = proj.xy / proj.w * 0.5 + 0.5;

          if(uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0)
              break;

          float sceneDepth = texture(depthSampler, uv).r;
          vec3 scenePos = ReconstructViewPos(uv, sceneDepth);

          if(abs(scenePos.z - pos.z) <= data.thickness)
          {
              hitColor = texture(colorSampler, uv).rgb;
              hit = true;
              break;
          }
      }

      vec3 finalColor = mix(baseColor, hitColor, hit ? data.intensity : 0.0);
      outColor = vec4(finalColor, 1.0);
  }
