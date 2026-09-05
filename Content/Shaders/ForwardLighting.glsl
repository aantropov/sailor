#if defined(VERTEX) || defined(FRAGMENT)

struct PerInstanceData
{
  mat4 model;
  vec4 sphereBounds;
  uint materialInstance;
  uint skeletonOffset;
  uint isCulled;
  uint padding;
  vec4 bakedVolumeScale;
  ObjectMotionData motion;
};

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

layout(std430, set = 2, binding = 0) readonly buffer PerInstanceDataSSBO
{
  PerInstanceData instance[];
} data;

#endif

#ifdef VERTEX

layout(std430, set = 2, binding = 1) readonly buffer InstanceIndicesSSBO
{
  uint instance[];
} instanceIndices;

void BuildForwardVertexFrame(
  mat4 modelMatrix,
  vec3 position,
  vec3 sourceNormal,
  vec3 sourceTangent,
  vec3 sourceBitangent,
  out vec3 worldPosition,
  out vec3 normal,
  out mat3 tangentBasis)
{
  vec4 vertexPosition = modelMatrix * vec4(position, 1.0);
  worldPosition = vertexPosition.xyz / vertexPosition.w;
  gl_Position = frame.projection * (frame.view * vertexPosition);

  mat3 linearMatrix = mat3(modelMatrix);
  mat3 normalMatrix = transpose(inverse(linearMatrix));
  normal = normalize(normalMatrix * sourceNormal);

  vec3 tangent = linearMatrix * sourceTangent;
  tangent -= normal * dot(normal, tangent);
  float tangentLengthSquared = dot(tangent, tangent);
  if(tangentLengthSquared > 1e-8)
  {
    tangent *= inversesqrt(tangentLengthSquared);
  }
  else
  {
    vec3 fallbackAxis = abs(normal.y) < 0.999
      ? vec3(0.0, 1.0, 0.0)
      : vec3(1.0, 0.0, 0.0);
    tangent = normalize(cross(fallbackAxis, normal));
  }

  vec3 transformedBitangent = linearMatrix * sourceBitangent;
  float handedness = dot(cross(normal, tangent), transformedBitangent) < 0.0
    ? -1.0
    : 1.0;
  vec3 bitangent = normalize(cross(normal, tangent)) * handedness;
  tangentBasis = mat3(tangent, bitangent, normal);
}

#endif

#ifdef FRAGMENT

layout(std430, set = 1, binding = 0) readonly buffer LightDataSSBO
{
  LightData instance[];
} light;

layout(std430, set = 1, binding = 1) readonly buffer CulledLightsSSBO
{
  uint indices[];
} culledLights;

layout(std430, set = 1, binding = 2) readonly buffer LightsGridSSBO
{
  LightsGrid instance[];
} lightsGrid;

layout(set = 1, binding = 3) uniform samplerCube g_irradianceCubemap;
layout(set = 1, binding = 4) uniform sampler2D g_brdfSampler;
layout(set = 1, binding = 5) uniform samplerCube g_envCubemap;
layout(set = 1, binding = 20) uniform LocalReflectionData
{
  vec4 positionBlend;
  vec4 minEnabled;
  vec4 boxMax;
} localReflection;
layout(set = 1, binding = 21) uniform samplerCube g_localEnvCubemap;
layout(set = 1, binding = 22) uniform samplerCube g_localSheenEnvCubemap;

layout(std430, set = 1, binding = 6) readonly buffer LightsMatricesSSBO
{
  mat4 instance[];
} lightsMatrices;

layout(std430, set = 1, binding = 7) readonly buffer ShadowIndicesSSBO
{
  uint instance[];
} shadowIndices;

layout(set = 1, binding = 8) uniform sampler2D g_aoSampler;
layout(set = 1, binding = 9) uniform sampler2D shadowMaps[MAX_SHADOW_MAP_SAMPLERS];

#ifdef TRANSMISSION
layout(set = 1, binding = 10) uniform sampler2D g_transmissionFramebufferSampler;
#endif

layout(std430, set = 1, binding = 11) readonly buffer ShadowAtlasTilesSSBO
{
  uint instance[];
} shadowAtlasTiles;

#if defined(SAILOR_TEXTURE_REMAP)
layout(std430, set = 4, binding = 0) readonly buffer TextureSamplerRemapSSBO
{
  uint indices[];
} textureSamplerRemap;
layout(set = 4, binding = 1) uniform sampler2D textureSamplers[];
#else
layout(set = 4, binding = 0) uniform sampler2D textureSamplers[];
#endif

const float Epsilon = 0.00001;
const vec3 Fdielectric = vec3(0.04);

struct ForwardPbrMaterial
{
  vec3 baseColor;
  float metallic;
  float roughness;
  float ambientOcclusion;
  float diffuseWeight;
  float indexOfRefraction;
};

struct ForwardLightSample
{
  vec3 direction;
  vec3 radiance;
  float falloff;
  float shadow;
};

struct ForwardLightList
{
  uint offset;
  uint count;
  bool overflow;
};

uint ResolveTextureSamplerIndex(uint globalTextureIndex)
{
#if defined(SAILOR_TEXTURE_REMAP)
  return textureSamplerRemap.indices[globalTextureIndex];
#else
  return globalTextureIndex;
#endif
}

float CalculateCascadedDirectionalShadow(
  LightData lightData,
  vec3 worldPosition,
  vec3 surfaceNormal,
  vec3 surfaceToLightDirection)
{
  const uint activeCascadeCount = clamp(
    lightData.activeCascadeCount,
    1u,
    uint(NUM_CSM_CASCADES));
  const int cascadeLayer = SelectCascade(
    frame.view,
    worldPosition,
    frame.cameraZNearZFar,
    activeCascadeCount);
  if(cascadeLayer >= int(activeCascadeCount))
  {
    return 1.0;
  }

  const uint cascadeShadowType = GetDirectionalCascadeShadowType(
    lightData.shadowType,
    cascadeLayer);
  const vec3 shadowReceiverPosition = OffsetDirectionalShadowReceiver(
    worldPosition,
    surfaceNormal,
    surfaceToLightDirection);
  const mat4 cascadeLightMatrix = lightsMatrices.instance[cascadeLayer];
  float shadow = CalculateDirectionalShadow(
    cascadeShadowType,
    shadowMaps[cascadeLayer],
    cascadeLightMatrix * vec4(shadowReceiverPosition, 1.0),
    cascadeLayer);

  const float cascadeBlend = CalculateCascadeBlend(
    frame.view,
    worldPosition,
    frame.cameraZNearZFar,
    cascadeLayer,
    activeCascadeCount);
  if(cascadeBlend > 0.0)
  {
    const int nextCascadeLayer = cascadeLayer + 1;
    const uint nextCascadeShadowType = GetDirectionalCascadeShadowType(
      lightData.shadowType,
      nextCascadeLayer);
    const vec3 nextShadowReceiverPosition = OffsetDirectionalShadowReceiver(
      worldPosition,
      surfaceNormal,
      surfaceToLightDirection);
    const mat4 nextCascadeLightMatrix =
      lightsMatrices.instance[nextCascadeLayer];
    const float nextShadow = CalculateDirectionalShadow(
      nextCascadeShadowType,
      shadowMaps[nextCascadeLayer],
      nextCascadeLightMatrix * vec4(nextShadowReceiverPosition, 1.0),
      nextCascadeLayer);
    shadow = mix(shadow, nextShadow, cascadeBlend);
  }

  const float shadowDistanceFade = CalculateShadowDistanceFade(
    frame.view,
    worldPosition,
    frame.cameraZNearZFar,
    cascadeLayer,
    activeCascadeCount);
  return mix(shadow, 1.0, shadowDistanceFade);
}

float CalculateLocalLightShadow(
  LightData lightData,
  uint lightIndex,
  vec3 worldPosition,
  vec3 surfaceNormal)
{
  const uint packedShadowIndex = shadowIndices.instance[lightIndex];
  if(lightData.shadowType == SHADOW_TYPE_NONE ||
    packedShadowIndex == INVALID_SHADOW_MAP_INDEX)
  {
    return 1.0;
  }

  uint shadowMapIndex = packedShadowIndex & SHADOW_MAP_INDEX_MASK;
  if(lightData.type == 1u)
  {
    shadowMapIndex += SelectPointShadowFace(
      worldPosition - lightData.worldPosition);
  }
  if(shadowMapIndex >= MAX_SHADOWS_IN_VIEW)
  {
    return 1.0;
  }

  const uint packedAtlasTile = shadowAtlasTiles.instance[shadowMapIndex];
  const uint shadowSamplerIndex = NUM_CSM_CASCADES +
    DecodeShadowAtlasIndex(packedAtlasTile);
  if(shadowSamplerIndex >= MAX_SHADOW_MAP_SAMPLERS)
  {
    return 1.0;
  }

  return CalculateLocalPcfShadow(
    shadowMaps[nonuniformEXT(shadowSamplerIndex)],
    lightsMatrices.instance[shadowMapIndex],
    DecodeShadowAtlasRect(packedAtlasTile),
    worldPosition,
    surfaceNormal,
    (packedShadowIndex & SOFT_SHADOW_MAP_BIT) != 0u,
    lightData.shadowBias);
}

ForwardLightSample ResolveForwardLightSample(
  LightData lightData,
  uint lightIndex,
  vec3 worldPosition,
  vec3 geometricNormal)
{
  ForwardLightSample result;
  result.radiance = lightData.intensity;
  result.falloff = 1.0;
  result.shadow = 1.0;
  result.direction = vec3(0.0);

  if(lightData.type == 0u)
  {
    result.direction = -lightData.direction;
    result.shadow = CalculateCascadedDirectionalShadow(
      lightData,
      worldPosition,
      geometricNormal,
      result.direction);
  }
  else if(lightData.type == 1u || lightData.type == 2u)
  {
    vec3 pointToLight = lightData.worldPosition - worldPosition;
    float pointToLightLengthSquared = dot(pointToLight, pointToLight);
    result.direction = pointToLightLengthSquared > Epsilon
      ? pointToLight * inversesqrt(pointToLightLengthSquared)
      : vec3(0.0);
    const float distanceToLight = length(pointToLight);
    result.falloff = CalculateLocalLightRangeAttenuation(
      lightData,
      distanceToLight);
    if(lightData.type == 2u)
    {
      result.falloff *= CalculateSpotLightAngularAttenuation(
        lightData,
        result.direction);
    }
    result.shadow = CalculateLocalLightShadow(
      lightData,
      lightIndex,
      worldPosition,
      geometricNormal);
  }

  return result;
}

vec3 EvaluateForwardPbrDirectLighting(
  ForwardLightSample lightSample,
  ForwardPbrMaterial materialData,
  vec3 surfaceToCamera,
  float cosLo,
  vec3 normal)
{
  vec3 halfVector = lightSample.direction + surfaceToCamera;
  float halfVectorLengthSquared = dot(halfVector, halfVector);
  vec3 Lh = halfVectorLengthSquared > Epsilon
    ? halfVector * inversesqrt(halfVectorLengthSquared)
    : normal;

  float cosLi = max(0.0, dot(normal, lightSample.direction));
  float cosLh = max(0.0, dot(normal, Lh));
  const float halfViewCosine = clamp(
    abs(dot(Lh, surfaceToCamera)),
    0.0,
    1.0);
  vec3 dielectricFresnel = vec3(FresnelDielectric(
    halfViewCosine,
    1.0,
    materialData.indexOfRefraction));
  vec3 metalFresnel = FresnelSchlick(
    materialData.baseColor,
    halfViewCosine);
  vec3 F = mix(
    dielectricFresnel,
    metalFresnel,
    clamp(materialData.metallic, 0.0, 1.0));
  float D = NdfGGX(cosLh, materialData.roughness);
  float G = GeometrySmithGGXCorrelated(
    cosLi,
    cosLo,
    materialData.roughness);
  vec3 kd = (vec3(1.0) - dielectricFresnel) *
    (1.0 - clamp(materialData.metallic, 0.0, 1.0)) *
    materialData.diffuseWeight;
  vec3 diffuseBrdf = CalculateLambertDiffuseBRDF(
    kd,
    materialData.baseColor);
  vec3 specularBrdf = (F * D * G) /
    max(Epsilon, 4.0 * cosLi * cosLo);
  return (diffuseBrdf + specularBrdf) *
    lightSample.radiance * cosLi * lightSample.falloff;
}

vec3 CalculateForwardPbrLighting(
  LightData lightData,
  uint lightIndex,
  ForwardPbrMaterial materialData,
  vec3 surfaceToCamera,
  float cosLo,
  vec3 normal,
  vec3 geometricNormal,
  vec3 worldPosition)
{
  const ForwardLightSample lightSample = ResolveForwardLightSample(
    lightData,
    lightIndex,
    worldPosition,
    geometricNormal);
  return lightSample.shadow * EvaluateForwardPbrDirectLighting(
    lightSample,
    materialData,
    surfaceToCamera,
    cosLo,
    normal);
}

float LocalReflectionWeight(vec3 worldPosition)
{
  if(localReflection.minEnabled.w < 0.5) return 0.0;
  vec3 edge = min(worldPosition - localReflection.minEnabled.xyz,
    localReflection.boxMax.xyz - worldPosition);
  return smoothstep(0.0, max(localReflection.positionBlend.w, 0.0001),
    min(edge.x, min(edge.y, edge.z)));
}

vec3 LocalReflectionDirection(vec3 worldPosition, vec3 direction)
{
  // Called only inside the finite influence box. Parallel axes cannot win
  // the nearest exit, including at the exact box center or on an axis ray.
  vec3 exitDistance = vec3(1e20);
  for(int axis = 0; axis < 3; ++axis)
  {
    if(abs(direction[axis]) > 1e-6)
    {
      float plane = direction[axis] > 0.0
        ? localReflection.boxMax[axis] : localReflection.minEnabled[axis];
      exitDistance[axis] = (plane - worldPosition[axis]) / direction[axis];
    }
  }
  float distance = min(exitDistance.x, min(exitDistance.y, exitDistance.z));
  return worldPosition + direction * distance - localReflection.positionBlend.xyz;
}

vec3 SampleForwardEnvironment(
  samplerCube distantMap, samplerCube localMap,
  vec3 worldPosition, vec3 direction, float roughness,
  float cosLo, float ambientOcclusion, float environmentVisibility)
{
  vec3 distant = textureLod(distantMap, direction,
    CalculateSpecularEnvironmentLod(textureQueryLevels(distantMap), roughness)).rgb;
  distant *= CalculateSpecularOcclusion(min(ambientOcclusion, environmentVisibility), cosLo, roughness);
  float weight = LocalReflectionWeight(worldPosition);
  if(weight <= 0.0) return distant;
  vec3 captured = textureLod(localMap, LocalReflectionDirection(worldPosition, direction),
    CalculateSpecularEnvironmentLod(textureQueryLevels(localMap), roughness)).rgb;
  // The captured ray already encountered walls/floor. Sky-escape visibility
  // must not remove their outgoing radiance; material AO still applies.
  captured *= CalculateSpecularOcclusion(ambientOcclusion, cosLo, roughness);
  return mix(distant, captured, weight);
}

vec3 CalculateForwardAmbientLighting(
  ForwardPbrMaterial materialData,
  vec3 F0,
  vec3 reflectionDirection,
  vec3 normal,
  vec3 geometricNormal,
  vec3 worldPosition,
  vec3 surfaceToCamera,
  float cosLo,
  vec2 screenUv,
  out float environmentVisibility)
{
  GlobalIlluminationSampleDebug globalIlluminationDebug;
  environmentVisibility = 1.0;
  vec3 irradiance = vec3(0.0);
  if(!TryResolveGlobalIlluminationDiffuseIrradiance(
    screenUv,
    worldPosition,
    normal,
    geometricNormal,
    surfaceToCamera,
    reflectionDirection,
    irradiance,
    environmentVisibility,
    globalIlluminationDebug))
  {
    irradiance = texture(g_irradianceCubemap, normal).rgb;
  }
  if(GlobalIlluminationDebugUsesProbeData())
  {
    return irradiance;
  }

  const float ior = max(materialData.indexOfRefraction, 1.0);
  const float dielectricF0Value = (ior - 1.0) / (ior + 1.0);
  vec3 dielectricFresnel = FresnelSchlickRoughness(
    cosLo,
    vec3(dielectricF0Value * dielectricF0Value),
    materialData.roughness);
  vec3 kd = (vec3(1.0) - dielectricFresnel) *
    (1.0 - clamp(materialData.metallic, 0.0, 1.0)) *
    materialData.diffuseWeight;
  vec3 diffuseIbl = kd * materialData.baseColor * irradiance;
  float ambientOcclusion = clamp(
    materialData.ambientOcclusion,
    0.0,
    1.0);
  vec3 specularIrradiance = SampleForwardEnvironment(g_envCubemap, g_localEnvCubemap,
    worldPosition, reflectionDirection, materialData.roughness,
    cosLo, ambientOcclusion, environmentVisibility);
  vec2 specularBrdf = texture(
    g_brdfSampler,
    vec2(cosLo, materialData.roughness)).rg;
  vec3 specularIbl =
    (F0 * specularBrdf.x + specularBrdf.y) * specularIrradiance;
  vec3 indirectLighting = diffuseIbl * ambientOcclusion +
    specularIbl;
  return ApplyGlobalIlluminationDebug(
    indirectLighting,
    globalIlluminationDebug);
}

ForwardLightList ResolveForwardLightList(vec2 fragmentPosition)
{
  ForwardLightList result;
  const uint tileIndex = GetLightTileIndex(
    fragmentPosition,
    frame.viewportSize);
  const uint gridLength = uint(lightsGrid.instance.length());
  const bool hasLightTile = tileIndex < gridLength;
  result.offset = hasLightTile
    ? lightsGrid.instance[tileIndex].offset
    : 0u;
  const uint listLength = uint(culledLights.indices.length());
  const uint availableLights = result.offset < listLength
    ? listLength - result.offset
    : 0u;
  const uint packedLightCount = hasLightTile
    ? lightsGrid.instance[tileIndex].num
    : 0u;
  result.overflow =
    (packedLightCount & LIGHT_TILE_OVERFLOW_BIT) != 0u;
  result.count = 0u;
  if(hasLightTile)
  {
#ifdef SUPPORT_LIGHTS_OVERFLOW
    result.count = result.overflow
      ? min(
          packedLightCount & LIGHT_TILE_COUNT_MASK,
          uint(light.instance.length()))
      : min(
          packedLightCount & LIGHT_TILE_COUNT_MASK,
          availableLights);
#else
    result.count = min(
      min(
        packedLightCount & LIGHT_TILE_COUNT_MASK,
        uint(LIGHTS_PER_TILE)),
      availableLights);
#endif
  }
  return result;
}

uint ResolveForwardLightIndex(
  ForwardLightList lightList,
  uint localLightIndex)
{
#ifdef SUPPORT_LIGHTS_OVERFLOW
  return lightList.overflow
    ? localLightIndex
    : culledLights.indices[lightList.offset + localLightIndex];
#else
  return culledLights.indices[lightList.offset + localLightIndex];
#endif
}

#endif
