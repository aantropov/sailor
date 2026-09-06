---
includes:
- Shaders/Constants.glsl
- Shaders/Math.glsl
defines: []
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
  layout(set=1, binding=1) uniform sampler2D depthSampler;
  layout(set=1, binding=2) uniform samplerCube environmentSampler;
  layout(set=1, binding=3) uniform samplerCube previousEnvironmentSampler;
  layout(set=1, binding=0) uniform FogData
  {
    vec4 fog;
    vec4 scattering;
    vec4 directionToSun;
    vec4 sunIlluminance;
    vec4 previousDirectionToSun;
    vec4 previousSunIlluminance;
  } data;
  layout(location=0) in vec2 fragTexcoord;
  layout(location=0) out vec4 outColor;

  vec3 AmbientRadiance(samplerCube environment, float lod)
  {
    // Environment's diffuse fallback already stores E / PI. Its full-sphere
    // average estimates isotropic incoming radiance; do not divide by PI again.
    // Six symmetric directions cancel the dominant directional SH bands.
    return (textureLod(environment, vec3(1, 0, 0), lod).rgb +
      textureLod(environment, vec3(-1, 0, 0), lod).rgb +
      textureLod(environment, vec3(0, 1, 0), lod).rgb +
      textureLod(environment, vec3(0, -1, 0), lod).rgb +
      textureLod(environment, vec3(0, 0, 1), lod).rgb +
      textureLod(environment, vec3(0, 0, -1), lod).rgb) / 6.0;
  }

  vec3 ScatteredRadiance(samplerCube environment, vec4 sunDirection, vec3 sunIlluminance, vec3 viewDirection)
  {
    float g = clamp(data.scattering.y, -0.95, 0.95);
    // Looking toward the sun produces the forward-scattering lobe.
    float cosine = clamp(dot(viewDirection, sunDirection.xyz), -1.0, 1.0);
    float denominator = max(1.0 + g * g - 2.0 * g * cosine, 0.0001);
    float phase = (1.0 - g * g) / (4.0 * PI * denominator * sqrt(denominator));
    return AmbientRadiance(environment, sunDirection.w) + sunIlluminance * phase;
  }

  void main()
  {
    outColor = vec4(0.0);
    float depth = textureLod(depthSampler, fragTexcoord, 0.0).r;
    // Reverse-Z clear depth is sky, which already includes atmospheric scattering.
    if(depth <= 0.0 || isnan(depth) || isinf(depth)) return;
    vec2 uv = FramebufferUvToSceneProjectionUv(fragTexcoord);
    vec4 viewPosition = frame.invProjection * vec4(uv * 2.0 - 1.0, depth, 1.0);
    if(abs(viewPosition.w) < 0.000001) return;
    vec3 displacement = transpose(mat3(frame.view)) * (viewPosition.xyz / viewPosition.w);
    float distance = length(displacement);
    float startDistance = max(data.fog.w, 0.0);
    if(isnan(distance) || isinf(distance) || distance <= startDistance) return;

    float startHeight = frame.cameraPosition.y + displacement.y * (startDistance / distance);
    float endHeight = frame.cameraPosition.y + displacement.y;
    float falloff = max(data.fog.y, 0.0);
    float heightDelta = falloff * abs(endHeight - startHeight);
    // Integrate from the denser endpoint: no growing exponential, and a
    // continuous horizontal-ray limit instead of division by a tiny slope.
    float integral = heightDelta < 0.001
      ? 1.0 - heightDelta * 0.5 + heightDelta * heightDelta / 6.0
      : (1.0 - exp(-heightDelta)) / heightDelta;
    float density = max(data.fog.x, 0.0) * exp(clamp(
      -falloff * (min(startHeight, endHeight) - data.fog.z), -80.0, 20.0));
    float opticalDepth = min(80.0, density * (distance - startDistance) * integral);
    float opacity = min(1.0 - exp(-opticalDepth), clamp(data.scattering.z, 0.0, 1.0));
    vec3 radiance = ScatteredRadiance(environmentSampler, data.directionToSun,
      data.sunIlluminance.rgb, displacement / distance);
    if(data.scattering.w < 1.0)
    {
      vec3 previous = ScatteredRadiance(previousEnvironmentSampler, data.previousDirectionToSun,
        data.previousSunIlluminance.rgb, displacement / distance);
      radiance = mix(previous, radiance, clamp(data.scattering.w, 0.0, 1.0));
    }
    // Fixed-function source-over preserves HDR alpha metadata and avoids a
    // framebuffer copy or simultaneous sampling/writing of the colour target.
    outColor = vec4(clamp(data.scattering.x, 0.0, 1.0) * clamp(radiance, vec3(0.0), vec3(65504.0)), opacity);
  }
