includes:
- Shaders/Constants.glsl
- Shaders/Math.glsl
- Shaders/Lighting.glsl
defines: []
glslCommon: |
  #version 450 core
  #extension GL_ARB_separate_shader_objects : enable
glslCompute: |
  // Pre-filters the environment for the Charlie sheen distribution.
  const float Epsilon = 0.00001;
  const uint NumSamples = 1024;
  const float InvNumSamples = 1.0 / float(NumSamples);
  const int NumMipLevels = 8;

  layout(set=0, binding=0) uniform samplerCube rawEnvMap;
  layout(set=0, binding=1, rgba16f) restrict writeonly uniform imageCube sheenEnvMap[NumMipLevels];

  layout(push_constant) uniform PushConstants
  {
    int level;
    float roughness;
  } pushConstants;

  vec2 SampleHammersley(uint index)
  {
    return vec2(index * InvNumSamples, RadicalInverse_VdC(index));
  }

  vec3 GetSamplingVector()
  {
    vec2 st =
      (vec2(gl_GlobalInvocationID.xy) + vec2(0.5)) /
      vec2(imageSize(sheenEnvMap[pushConstants.level]));
    vec2 uv = 2.0 * vec2(st.x, 1.0 - st.y) - vec2(1.0);

    vec3 direction;
    if(gl_GlobalInvocationID.z == 0) direction = vec3(1.0, uv.y, -uv.x);
    else if(gl_GlobalInvocationID.z == 1) direction = vec3(-1.0, uv.y, uv.x);
    else if(gl_GlobalInvocationID.z == 2) direction = vec3(uv.x, 1.0, -uv.y);
    else if(gl_GlobalInvocationID.z == 3) direction = vec3(uv.x, -1.0, uv.y);
    else if(gl_GlobalInvocationID.z == 4) direction = vec3(uv.x, uv.y, 1.0);
    else direction = vec3(-uv.x, uv.y, -1.0);
    return normalize(direction);
  }

  void ComputeBasisVectors(const vec3 normal, out vec3 tangent, out vec3 bitangent)
  {
    bitangent = cross(normal, vec3(0.0, 1.0, 0.0));
    bitangent = mix(
      cross(normal, vec3(1.0, 0.0, 0.0)),
      bitangent,
      step(Epsilon, dot(bitangent, bitangent)));
    bitangent = normalize(bitangent);
    tangent = normalize(cross(normal, bitangent));
  }

  vec3 TangentToWorld(
    const vec3 value,
    const vec3 normal,
    const vec3 tangent,
    const vec3 bitangent)
  {
    return tangent * value.x +
      bitangent * value.y +
      normal * value.z;
  }

  layout(local_size_x=16, local_size_y=16, local_size_z=1) in;
  void main(void)
  {
    ivec2 outputSize = imageSize(sheenEnvMap[pushConstants.level]);
    if(gl_GlobalInvocationID.x >= outputSize.x ||
      gl_GlobalInvocationID.y >= outputSize.y)
    {
      return;
    }

    vec2 inputSize = vec2(textureSize(rawEnvMap, 0));
    float texelSolidAngle =
      4.0 * PI / (6.0 * inputSize.x * inputSize.y);
    vec3 normal = GetSamplingVector();
    vec3 viewDirection = normal;
    vec3 tangent;
    vec3 bitangent;
    ComputeBasisVectors(normal, tangent, bitangent);

    vec3 color = vec3(0.0);
    float weight = 0.0;
    for(uint sampleIndex = 0; sampleIndex < NumSamples; ++sampleIndex)
    {
      vec2 samplePoint = SampleHammersley(sampleIndex);
      vec3 halfVector = TangentToWorld(
        SampleCharlie(
          samplePoint.x,
          samplePoint.y,
          pushConstants.roughness),
        normal,
        tangent,
        bitangent);
      vec3 incidentDirection =
        2.0 * dot(viewDirection, halfVector) * halfVector -
        viewDirection;
      float cosLi = dot(normal, incidentDirection);
      if(cosLi <= 0.0)
      {
        continue;
      }

      float cosLh = max(dot(normal, halfVector), 0.0);
      float pdf = max(
        NdfCharlie(cosLh, pushConstants.roughness) * 0.25,
        Epsilon);
      float sampleSolidAngle = 1.0 / (float(NumSamples) * pdf);
      float mipLevel = max(
        0.5 * log2(sampleSolidAngle / texelSolidAngle) + 1.0,
        0.0);
      color += textureLod(
        rawEnvMap,
        incidentDirection,
        mipLevel).rgb * cosLi;
      weight += cosLi;
    }

    color = weight > Epsilon
      ? color / weight
      : textureLod(rawEnvMap, normal, 0.0).rgb;
    imageStore(
      sheenEnvMap[pushConstants.level],
      ivec3(gl_GlobalInvocationID),
      vec4(color, 1.0));
  }
