---
includes:
- Shaders/Constants.glsl
- Shaders/Math.glsl
- Shaders/Lighting.glsl
- Shaders/GlobalIllumination.glsl

defines:
 - ALPHA_CUTOUT
 - SKINNING
 - CLEAR_COAT
 - SHEEN
 - TRANSMISSION
 - SUPPORT_LIGHTS_OVERFLOW

glslCommon: |
  #version 460
  #extension GL_ARB_separate_shader_objects : enable
  #extension GL_EXT_nonuniform_qualifier : require

glslVertex: |
  layout(location=DefaultPositionBinding) in vec3 inPosition;
  layout(location=DefaultNormalBinding) in vec3 inNormal;
  layout(location=DefaultTexcoordBinding) in vec2 inTexcoord;
  layout(location=DefaultColorBinding) in vec4 inColor;
  layout(location=DefaultTangentBinding) in vec3 inTangent;
  layout(location=DefaultBitangentBinding) in vec3 inBitangent;
  #ifdef SKINNING
    layout(location=DefaultBoneIdsBinding) in uvec4 inBoneIds;
    layout(location=DefaultBoneWeightsBinding) in vec4 inBoneWeights;
  #endif

  layout(location=0)
  flat out uint materialInstance;
  
  layout(location=1) out Vertex
  {
    vec2 texcoord;
    vec3 worldPosition;
    vec3 normal;
    vec4 color;
    mat3 tangentBasis;
  #ifdef TRANSMISSION
    flat vec3 modelScale;
  #endif
  } vout;
  
  struct PerInstanceData
  {
    mat4 model;
    vec4 sphereBounds;
    uint materialInstance;
    uint skeletonOffset;
    uint isCulled;
    uint padding;
    vec4 bakedVolumeScale;
  };
  
  struct MaterialData
  {
    vec4 baseColorFactor;
    vec4 emissiveFactor;

    float metallicFactor;
    float roughnessFactor;
    float occlusionStrength;
    float normalScale;
    float alphaCutoff;

    uint baseColorSampler;
    uint normalSampler;
    uint ormSampler;
    uint occlusionSampler;
    uint emissiveSampler;

    float clearcoatFactor;
    float clearcoatRoughnessFactor;
    float clearcoatNormalScale;

    float sheenRoughnessFactor;
    vec4 sheenColorFactor;

    uint clearcoatSampler;
    uint clearcoatRoughnessSampler;
    uint clearcoatNormalSampler;
    uint sheenColorSampler;
    uint sheenRoughnessSampler;

  #ifdef TRANSMISSION
    float transmissionFactor;
    uint transmissionSampler;
    float thicknessFactor;
    float attenuationDistance;
    float indexOfRefraction;
    uint thicknessSampler;
    vec4 attenuationColor;
  #endif
  };

  struct BoneData
  {
    mat4 matrix;
  };

  const uint INVALID_SKELETON_OFFSET = 0xFFFFFFFFu;
  
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
  
  layout(std430, set = 1, binding = 0) readonly buffer LightDataSSBO
  {  
    LightData instance[];
  } light;
  
  layout(std430, set = 1, binding = 1) readonly buffer CulledLightsSSBO
  {
      uint indices[MAX_TEXTURES_IN_SCENE];
  } culledLights;
  
  layout(std430, set = 1, binding = 2) readonly buffer LightsGridSSBO
  {
      LightsGrid instance[];
  } lightsGrid;
  
  layout(set=1, binding=3) uniform samplerCube g_irradianceCubemap;
  layout(set=1, binding=4) uniform sampler2D   g_brdfSampler;
  layout(set=1, binding=5) uniform samplerCube g_envCubemap;

  layout(std430, set = 1, binding = 6) readonly buffer LightsMatricesSSBO
  {
      mat4 instance[];
  } lightsMatrices;

  layout(std430, set = 1, binding = 7) readonly buffer ShadowIndicesSSBO
  {
      uint instance[];
  } shadowIndices;

  layout(set=1, binding=8) uniform sampler2D g_aoSampler;
  layout(set=1, binding=9) uniform sampler2D shadowMaps[MAX_SHADOW_MAP_SAMPLERS];

     layout(std430, set = 1, binding = 11) readonly buffer ShadowAtlasTilesSSBO
     {
         uint instance[];
     } shadowAtlasTiles;

  layout(std430, set = 2, binding = 0) readonly buffer PerInstanceDataSSBO
  {
      PerInstanceData instance[];
  } data;

  layout(std430, set = 2, binding = 1) readonly buffer InstanceIndicesSSBO
  {
      uint instance[];
  } instanceIndices;
  
  layout(std430, set = 3, binding = 0) readonly buffer MaterialDataSSBO
  {
      MaterialData instance[];
  } material;
  
  #if defined(SAILOR_TEXTURE_REMAP)
  layout(std430, set=4, binding=0) readonly buffer TextureSamplerRemapSSBO
  {
      uint indices[MAX_TEXTURES_IN_SCENE];
  } textureSamplerRemap;
  layout(set=4, binding=1) uniform sampler2D textureSamplers[];
  #else
  layout(set=4, binding=0) uniform sampler2D textureSamplers[];
  #endif

  #ifdef SKINNING
  layout(std430, set = 5, binding = 0) readonly buffer BoneMatricesSSBO
  {
      BoneData instance[];
  } bones;
  #endif
  
  void main()
  {
    uint instanceIndex = instanceIndices.instance[gl_InstanceIndex];
    mat4 modelMatrix = data.instance[instanceIndex].model;
  #ifdef TRANSMISSION
    mat3 instanceLinearMatrix = mat3(modelMatrix);
    vout.modelScale = vec3(
      length(instanceLinearMatrix[0]),
      length(instanceLinearMatrix[1]),
      length(instanceLinearMatrix[2])) *
      data.instance[instanceIndex].bakedVolumeScale.xyz;
  #endif
  #ifdef SKINNING
    uint offset = data.instance[instanceIndex].skeletonOffset;

    if (offset != INVALID_SKELETON_OFFSET)
    {
      mat4 skinMatrix = bones.instance[offset + inBoneIds.x].matrix * inBoneWeights.x +
                        bones.instance[offset + inBoneIds.y].matrix * inBoneWeights.y +
                        bones.instance[offset + inBoneIds.z].matrix * inBoneWeights.z +
                        bones.instance[offset + inBoneIds.w].matrix * inBoneWeights.w;

      modelMatrix *= skinMatrix;
    }
  #endif

    vec4 vertexPosition = modelMatrix * vec4(inPosition, 1.0);
    vout.worldPosition = vertexPosition.xyz / vertexPosition.w;

    gl_Position = frame.projection * (frame.view * vertexPosition);

    mat3 linearMatrix = mat3(modelMatrix);
    mat3 normalMatrix = transpose(inverse(linearMatrix));
    vec3 normal = normalize(normalMatrix * inNormal);

    vec3 tangent = linearMatrix * inTangent;
    tangent -= normal * dot(normal, tangent);
    float tangentLengthSquared = dot(tangent, tangent);
    if(tangentLengthSquared > 1e-8)
    {
      tangent *= inversesqrt(tangentLengthSquared);
    }
    else
    {
      vec3 fallbackAxis = abs(normal.y) < 0.999 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
      tangent = normalize(cross(fallbackAxis, normal));
    }

    vec3 transformedBitangent = linearMatrix * inBitangent;
    float handedness = dot(cross(normal, tangent), transformedBitangent) < 0.0 ? -1.0 : 1.0;
    vec3 bitangent = normalize(cross(normal, tangent)) * handedness;

    vout.normal = normal;
    vout.tangentBasis = mat3(tangent, bitangent, normal);

    vout.color = inColor;
    vout.texcoord = inTexcoord;
    materialInstance = data.instance[instanceIndex].materialInstance;
  }

glslFragment: |
  layout(location=0)
  flat in uint materialInstance;
  
  layout(location=1) in Vertex
  {   
    vec2 texcoord;
    vec3 worldPosition;
    vec3 normal;
    vec4 color;
    mat3 tangentBasis;
  #ifdef TRANSMISSION
    flat vec3 modelScale;
  #endif
  } vin;
  
  layout(location=0) out vec4 outColor;
  
  struct PerInstanceData
  {
      mat4 model;
      vec4 sphereBounds;
      uint materialInstance;
      uint skeletonOffset;
      uint isCulled;
      uint padding;
      vec4 bakedVolumeScale;
  };
  
  struct MaterialData
  {
    vec4 baseColorFactor;
    vec4 emissiveFactor;

    float metallicFactor;
    float roughnessFactor;
    float occlusionStrength;
    float normalScale;
    float alphaCutoff;

    uint baseColorSampler;
    uint normalSampler;
    uint ormSampler;
    uint occlusionSampler;
    uint emissiveSampler;

    float clearcoatFactor;
    float clearcoatRoughnessFactor;
    float clearcoatNormalScale;

    float sheenRoughnessFactor;
    vec4 sheenColorFactor;

    uint clearcoatSampler;
    uint clearcoatRoughnessSampler;
    uint clearcoatNormalSampler;
    uint sheenColorSampler;
    uint sheenRoughnessSampler;

  #ifdef TRANSMISSION
    float transmissionFactor;
    uint transmissionSampler;
    float thicknessFactor;
    float attenuationDistance;
    float indexOfRefraction;
    uint thicknessSampler;
    vec4 attenuationColor;
  #endif
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
  
  layout(set=1, binding=3) uniform samplerCube g_irradianceCubemap;
  layout(set=1, binding=4) uniform sampler2D   g_brdfSampler;
  layout(set=1, binding=5) uniform samplerCube g_envCubemap;

  layout(std430, set = 1, binding = 6) readonly buffer LightsMatricesSSBO
  {
      mat4 instance[];
  } lightsMatrices;
  
  layout(std430, set = 1, binding = 7) readonly buffer ShadowIndicesSSBO
  {
      uint instance[];
  } shadowIndices;

  layout(set=1, binding=8) uniform sampler2D g_aoSampler;
  layout(set=1, binding=9) uniform sampler2D shadowMaps[MAX_SHADOW_MAP_SAMPLERS];
  #ifdef TRANSMISSION
  layout(set=1, binding=10) uniform sampler2D g_transmissionFramebufferSampler;
  #endif

     layout(std430, set = 1, binding = 11) readonly buffer ShadowAtlasTilesSSBO
     {
         uint instance[];
     } shadowAtlasTiles;
  
  layout(std430, set = 2, binding = 0) readonly buffer PerInstanceDataSSBO
  {
      PerInstanceData instance[];
  } data;
  
  layout(std430, set = 3, binding = 0) readonly buffer MaterialDataSSBO
  {
      MaterialData instance[];
  } material;
  
  //layout(set=3, binding=1) uniform sampler2D albedoSampler;
  //layout(set=3, binding=2) uniform sampler2D metalnessSampler;
  //layout(set=3, binding=3) uniform sampler2D normalSampler;
  //layout(set=3, binding=4) uniform sampler2D roughnessSampler;
  
  #if defined(SAILOR_TEXTURE_REMAP)
  layout(std430, set=4, binding=0) readonly buffer TextureSamplerRemapSSBO
  {
      uint indices[];
  } textureSamplerRemap;
  layout(set=4, binding=1) uniform sampler2D textureSamplers[];
  #else
  layout(set=4, binding=0) uniform sampler2D textureSamplers[];
  #endif

  uint ResolveTextureSamplerIndex(uint globalTextureIndex)
  {
  #if defined(SAILOR_TEXTURE_REMAP)
      if(globalTextureIndex >= MAX_TEXTURES_IN_SCENE)
      {
        return 0;
      }
      return textureSamplerRemap.indices[globalTextureIndex];
  #else
      return globalTextureIndex;
  #endif
  }
  
  MaterialData GetMaterialData()
  {
      return material.instance[materialInstance];
  }

  float CalculateCascadedDirectionalShadow(
    LightData light,
    vec3 worldPosition,
    vec3 surfaceNormal,
    vec3 surfaceToLightDirection)
  {
    const uint activeCascadeCount = clamp(
      light.activeCascadeCount, 1u, uint(NUM_CSM_CASCADES));
    const int cascadeLayer = SelectCascade(
      frame.view, worldPosition, frame.cameraZNearZFar, activeCascadeCount);
    if(cascadeLayer >= int(activeCascadeCount))
    {
      return 1.0f;
    }

    const uint cascadeShadowType = GetDirectionalCascadeShadowType(
      light.shadowType,
      cascadeLayer);
    const vec3 shadowReceiverPosition = OffsetDirectionalShadowReceiver(
      worldPosition,
      surfaceNormal,
      surfaceToLightDirection,
      GetDirectionalShadowReceiverBiasScale(
        cascadeShadowType,
        light.shadowBias));
    const mat4 cascadeLightMatrix = lightsMatrices.instance[cascadeLayer];
    float shadow = CalculateDirectionalShadow(
      cascadeShadowType,
      shadowMaps[cascadeLayer],
      cascadeLightMatrix * vec4(shadowReceiverPosition, 1.0f),
      cascadeLayer);

    const float cascadeBlend = CalculateCascadeBlend(
      frame.view,
      worldPosition,
      frame.cameraZNearZFar,
      cascadeLayer,
      activeCascadeCount);
    if(cascadeBlend > 0.0f)
    {
      const int nextCascadeLayer = cascadeLayer + 1;
      const uint nextCascadeShadowType = GetDirectionalCascadeShadowType(
        light.shadowType,
        nextCascadeLayer);
      const vec3 nextShadowReceiverPosition = OffsetDirectionalShadowReceiver(
        worldPosition,
        surfaceNormal,
        surfaceToLightDirection,
        GetDirectionalShadowReceiverBiasScale(
          nextCascadeShadowType,
          light.shadowBias));
      const mat4 nextCascadeLightMatrix = lightsMatrices.instance[nextCascadeLayer];
      const float nextShadow = CalculateDirectionalShadow(
        nextCascadeShadowType,
        shadowMaps[nextCascadeLayer],
        nextCascadeLightMatrix * vec4(nextShadowReceiverPosition, 1.0f),
        nextCascadeLayer);
      shadow = mix(shadow, nextShadow, cascadeBlend);
    }

    const float shadowDistanceFade = CalculateShadowDistanceFade(
      frame.view,
      worldPosition,
      frame.cameraZNearZFar,
      cascadeLayer,
      activeCascadeCount);
    shadow = mix(shadow, 1.0f, shadowDistanceFade);

    return shadow;
  }

  float CalculateLocalLightShadow(
    LightData light,
    uint lightIndex,
    vec3 worldPosition,
    vec3 surfaceNormal,
    vec3 surfaceToLightDirection)
  {
    const uint packedShadowIndex = shadowIndices.instance[lightIndex];
    if(light.shadowType == SHADOW_TYPE_NONE ||
       packedShadowIndex == INVALID_SHADOW_MAP_INDEX)
    {
      return 1.0f;
    }

    uint shadowMapIndex = packedShadowIndex & SHADOW_MAP_INDEX_MASK;
    if(light.type == 1u)
    {
      shadowMapIndex += SelectPointShadowFace(worldPosition - light.worldPosition);
    }
    if(shadowMapIndex >= MAX_SHADOWS_IN_VIEW)
    {
      return 1.0f;
    }

    const uint packedAtlasTile = shadowAtlasTiles.instance[shadowMapIndex];
    const uint shadowSamplerIndex = NUM_CSM_CASCADES + DecodeShadowAtlasIndex(packedAtlasTile);
    if(shadowSamplerIndex >= MAX_SHADOW_MAP_SAMPLERS)
    {
      return 1.0f;
    }

    const vec4 atlasRect = DecodeShadowAtlasRect(packedAtlasTile);
    return CalculateLocalPcfShadow(
      shadowMaps[nonuniformEXT(shadowSamplerIndex)],
      lightsMatrices.instance[shadowMapIndex],
      atlasRect,
      worldPosition,
      surfaceNormal,
      surfaceToLightDirection,
      length(light.worldPosition - worldPosition),
      (packedShadowIndex & SOFT_SHADOW_MAP_BIT) != 0u,
      light.shadowBias);
  }
  
  const float Epsilon = 0.00001;

  #ifdef TRANSMISSION
  float ApplyIorToRoughness(float roughness, float ior)
  {
    return roughness * clamp(ior * 2.0 - 2.0, 0.0, 1.0);
  }

  float NdfGGXAlpha(float cosLh, float alphaRoughness)
  {
    float alphaSq = alphaRoughness * alphaRoughness;
    float denominator = cosLh * cosLh * (alphaSq - 1.0) + 1.0;
    return alphaSq / max(PI * denominator * denominator, Epsilon);
  }

  float VisibilityGGXAlpha(float cosLi, float cosLo, float alphaRoughness)
  {
    float alphaSq = alphaRoughness * alphaRoughness;
    float ggxV = cosLi * sqrt(cosLo * cosLo * (1.0 - alphaSq) + alphaSq);
    float ggxL = cosLo * sqrt(cosLi * cosLi * (1.0 - alphaSq) + alphaSq);
    float ggx = ggxV + ggxL;
    return ggx > Epsilon ? 0.5 / ggx : 0.0;
  }

  vec3 GetRefractionDirection(
    vec3 normal,
    vec3 view,
    float ior)
  {
    vec3 refractedDirection = refract(-view, normalize(normal), 1.0 / ior);
    float refractedLengthSquared = dot(refractedDirection, refractedDirection);
    if(refractedLengthSquared <= Epsilon)
    {
      return vec3(0.0);
    }

    return refractedDirection * inversesqrt(refractedLengthSquared);
  }

  vec3 GetVolumeTransmissionRay(
    vec3 refractedDirection,
    float thickness,
    vec3 modelScale)
  {
    return refractedDirection * thickness * modelScale;
  }

  vec3 GetVolumeAttenuation(
    float transmissionDistance,
    vec3 attenuationColor,
    float attenuationDistance)
  {
    if(transmissionDistance <= Epsilon ||
      attenuationDistance >= 3.402823466e+38)
    {
      return vec3(1.0);
    }

    float attenuationExponent = transmissionDistance /
      max(attenuationDistance, Epsilon);
    bvec3 fullyAbsorbed = lessThanEqual(attenuationColor, vec3(0.0));
    vec3 attenuation = pow(
      max(attenuationColor, vec3(Epsilon)),
      vec3(attenuationExponent));
    return mix(attenuation, vec3(0.0), fullyAbsorbed);
  }
  #endif

  vec3 CalculateLighting(LightData light, uint lightIndex, MaterialData material, vec3 F0, vec3 Lo,float cosLo, vec3 normal, vec3 worldPos)
  {
    float falloff = 1.0f;
    float shadow = 1.0f;
    vec3 pointToLight = light.type == 0 ?
      -light.direction :
      light.worldPosition - worldPos;
    float pointToLightLengthSquared = dot(pointToLight, pointToLight);
    vec3 Li = pointToLightLengthSquared > Epsilon ?
      pointToLight * inversesqrt(pointToLightLengthSquared) :
      vec3(0.0);
    
    // Directional light
    if(light.type == 0)
    {
        shadow = CalculateCascadedDirectionalShadow(
          light,
          worldPos,
          normal,
          Li);
    }
    // Point light
    else if(light.type == 1)
    {
      // Attenuation
      const float distance    = length(pointToLight);
      falloff = CalculateLocalLightRangeAttenuation(light, distance);
      shadow = CalculateLocalLightShadow(light, lightIndex, worldPos, normal, Li);
    }
    // Spot light
    else if(light.type == 2)
    {
      // Attenuation
      vec3 lightDir = Li;
      float epsilon   = light.cutOff.x - light.cutOff.y;
      float theta = dot(lightDir, normalize(-light.direction));
      const float distance    = length(pointToLight);
      falloff = CalculateLocalLightRangeAttenuation(light, distance) *
        clamp((theta - light.cutOff.y) / max(epsilon, Epsilon), 0.0, 1.0);
      
      if(theta < light.cutOff.y)
      {
        falloff = 0.0f;
      }

      shadow = CalculateLocalLightShadow(light, lightIndex, worldPos, normal, Li);
    }
    
    vec3 Lradiance = light.intensity;

    // Half-vector between Li and Lo.
    vec3 halfVector = Li + Lo;
    float halfVectorLengthSquared = dot(halfVector, halfVector);
    vec3 Lh = halfVectorLengthSquared > Epsilon ?
      halfVector * inversesqrt(halfVectorLengthSquared) :
      normal;

    // Calculate angles between surface normal and various light vectors.
    float cosLi = max(0.0, dot(normal, Li));
    float cosLh = max(0.0, dot(normal, Lh));

    // Calculate Fresnel term for direct lighting. 
    vec3 F  = FresnelSchlick(F0, max(0.0, dot(Lh, Lo)));
    // Calculate normal distribution for specular BRDF.
    float D = NdfGGX(cosLh, material.roughnessFactor);
    // Calculate geometric attenuation for specular BRDF.
    float G = GeometrySchlickGGX(cosLi, cosLo, material.roughnessFactor);

    // Diffuse scattering happens due to light being refracted multiple times by a dielectric medium.
    // Metals on the other hand either reflect or absorb energy, so diffuse contribution is always zero.
    // To be energy conserving we must scale diffuse BRDF contribution based on Fresnel factor & metalness.
    vec3 kd = mix(vec3(1.0) - F, vec3(0.0), material.metallicFactor);
  #ifdef TRANSMISSION
    kd *= 1.0 - material.transmissionFactor;
  #endif

    // Lambert diffuse BRDF.
    // We don't scale by 1/PI for lighting & material units to be more convenient.
    // See: https://seblagarde.wordpress.com/2012/01/08/pi-or-not-to-pi-in-game-lighting-equation/
    vec3 diffuseBRDF = kd * material.baseColorFactor.xyz;

    // Cook-Torrance specular microfacet BRDF.
    vec3 specularBRDF = (F * D * G) / max(Epsilon, 4.0 * cosLi * cosLo);

    vec3 directLighting =
      (diffuseBRDF + specularBRDF) * Lradiance * cosLi * falloff;

  #ifdef TRANSMISSION
    vec3 refractionNormal = gl_FrontFacing ? normal : -normal;
    vec3 refractionDirection = GetRefractionDirection(
      refractionNormal,
      Lo,
      material.indexOfRefraction);
    vec3 transmissionRay = GetVolumeTransmissionRay(
      refractionDirection,
      material.thicknessFactor,
      vin.modelScale);
    float transmissionRayLength = length(transmissionRay);
    float canTransmit = step(Epsilon, dot(
      refractionDirection,
      refractionDirection));
    float dielectricTransmission = material.transmissionFactor *
      (1.0 - clamp(material.metallicFactor, 0.0, 1.0));

    if(canTransmit > 0.0 &&
      dielectricTransmission > Epsilon)
    {
      vec3 exitPointToLight = light.type == 0 ?
        -light.direction :
        light.worldPosition - (worldPos + transmissionRay);
      float exitPointToLightLength = length(exitPointToLight);
      if(exitPointToLightLength > Epsilon)
      {
        vec3 transmissionLi = exitPointToLight / exitPointToLightLength;
        vec3 mirroredLiVector =
          transmissionLi +
          2.0 * refractionNormal *
            dot(-transmissionLi, refractionNormal);
        float mirroredLiLengthSquared = dot(
          mirroredLiVector,
          mirroredLiVector);
        if(mirroredLiLengthSquared > Epsilon)
        {
          vec3 mirroredLi = mirroredLiVector *
            inversesqrt(mirroredLiLengthSquared);
          vec3 transmissionHalfVector = mirroredLi + Lo;
          float transmissionHalfLengthSquared = dot(
            transmissionHalfVector,
            transmissionHalfVector);
          if(transmissionHalfLengthSquared > Epsilon)
          {
            vec3 transmissionH = transmissionHalfVector *
              inversesqrt(transmissionHalfLengthSquared);
            float alphaRoughness = ApplyIorToRoughness(
              material.roughnessFactor * material.roughnessFactor,
              material.indexOfRefraction);
            float transmissionDistribution = NdfGGXAlpha(
              clamp(dot(refractionNormal, transmissionH), 0.0, 1.0),
              max(alphaRoughness, Epsilon));
            float transmissionCosLi = clamp(
              dot(refractionNormal, mirroredLi),
              0.0,
              1.0);
            float transmissionCosLo = clamp(
              dot(refractionNormal, Lo),
              0.0,
              1.0);
            float transmissionVisibility = VisibilityGGXAlpha(
              transmissionCosLi,
              transmissionCosLo,
              max(alphaRoughness, Epsilon));
            float dielectricFresnel =
              (material.indexOfRefraction - 1.0) /
              (material.indexOfRefraction + 1.0);
            vec3 transmissionFresnel = FresnelSchlick(
              vec3(dielectricFresnel * dielectricFresnel),
              clamp(abs(dot(Lo, transmissionH)), 0.0, 1.0));

            float transmissionFalloff = falloff;
            if(light.type == 1)
            {
              transmissionFalloff = CalculateLocalLightRangeAttenuation(
                light,
                exitPointToLightLength);
            }
            else if(light.type == 2)
            {
              float coneRange = light.cutOff.x - light.cutOff.y;
              float coneCos = dot(
                transmissionLi,
                normalize(-light.direction));
              transmissionFalloff = CalculateLocalLightRangeAttenuation(
                light,
                exitPointToLightLength) * clamp(
                (coneCos - light.cutOff.y) / max(coneRange, Epsilon),
                0.0,
                1.0);
            }

            vec3 volumeAttenuation = GetVolumeAttenuation(
              transmissionRayLength,
              material.attenuationColor.rgb,
              material.attenuationDistance);
            directLighting += material.baseColorFactor.rgb *
              transmissionDistribution * transmissionVisibility *
              volumeAttenuation * (vec3(1.0) - transmissionFresnel) *
              dielectricTransmission * Lradiance * transmissionFalloff;
          }
        }
      }
    }
  #endif

    // Total contribution for this light.
    return shadow * directLighting;
  }
  
  vec3 AmbientLighting(
    MaterialData material,
    vec3 F0,
    vec3 Lr,
    vec3 normal,
    vec3 geometricNormal,
    vec3 worldPosition,
    float cosLo,
    vec2 screenUv,
    out float environmentVisibility)
  {
    // Baked probes replace diffuse environment irradiance only. Specular IBL
    // remains sourced from the pre-filtered environment cubemap.
    const vec3 environmentIrradiance =
      texture(g_irradianceCubemap, normal).rgb;
    GlobalIlluminationSampleDebug globalIlluminationDebug;
    environmentVisibility = 1.0;
    const vec3 irradiance = ResolveGlobalIlluminationDiffuseIrradiance(
      screenUv,
      worldPosition,
      normal,
      geometricNormal,
      normalize(frame.cameraPosition.xyz - worldPosition),
      Lr,
      environmentIrradiance,
      environmentVisibility,
      globalIlluminationDebug);
    if(GlobalIlluminationDebugUsesProbeData())
    {
      return irradiance;
    }
    
    // Calculate Fresnel term for ambient lighting.
    // Since we use pre-filtered cubemap(s) and irradiance is coming from many directions
    // use cosLo instead of angle with light's half-vector (cosLh above).
    // See: https://seblagarde.wordpress.com/2011/08/17/hello-world/
    vec3 F = FresnelSchlick(F0, cosLo);
    
    // Get diffuse contribution factor (as with direct lighting).
    vec3 kd = mix(vec3(1.0) - F, vec3(0.0), material.metallicFactor);
  #ifdef TRANSMISSION
    kd *= 1.0 - material.transmissionFactor;
  #endif
    
    // Irradiance map contains exitant radiance assuming Lambertian BRDF, no need to scale by 1/PI here either.
    vec3 diffuseIBL = kd * material.baseColorFactor.xyz * irradiance;
    
    // Sample pre-filtered specular reflection environment at correct mipmap level.
    int specularTextureLevels = textureQueryLevels(g_envCubemap);
    vec3 specularIrradiance = textureLod(g_envCubemap, Lr, material.roughnessFactor * specularTextureLevels).rgb;
    
    // Split-sum approximation factors for Cook-Torrance specular BRDF.
    vec2 specularBRDF = texture(g_brdfSampler, vec2(cosLo, material.roughnessFactor)).rg;
    
    // Total specular IBL contribution.
    vec3 specularIBL = (F0 * specularBRDF.x + specularBRDF.y) * specularIrradiance;

    // Apply visibility directly to diffuse IBL and use a view- and
    // roughness-aware approximation for specular IBL. The bounded analytic
    // contact term is composed separately after direct-light accumulation.
    float ambientOcclusion = clamp(material.occlusionStrength, 0.0, 1.0);
    float specularOcclusion = CalculateSpecularOcclusion(
      min(ambientOcclusion, environmentVisibility),
      cosLo,
      material.roughnessFactor);
    vec3 indirectLighting = diffuseIBL * ambientOcclusion +
      specularIBL * specularOcclusion;
    if(globalIlluminationHeader.stateAndDebug.z ==
      GLOBAL_ILLUMINATION_DEBUG_INDIRECT_ONLY)
    {
      indirectLighting = diffuseIBL * ambientOcclusion;
    }
    return ApplyGlobalIlluminationDebug(
      indirectLighting,
      globalIlluminationDebug);
  }

  #ifdef CLEAR_COAT
  vec3 ClearCoatLighting(LightData light, float roughness, vec3 F0, vec3 Lo, float cosLo, vec3 normal, vec3 worldPos)
  {
    float falloff = 1.0f;
    float shadow = 1.0f;
    if(light.type == 0)
    {
        shadow = CalculateCascadedDirectionalShadow(
          light,
          worldPos,
          normal,
          -light.direction);
    }
    else if(light.type == 1 || light.type == 2)
    {
        const float distance = length(light.worldPosition - worldPos);
        falloff = CalculateLocalLightRangeAttenuation(light, distance);
    }

    vec3 pointToLight = light.type == 0 ?
      -light.direction :
      light.worldPosition - worldPos;
    float pointToLightLengthSquared = dot(pointToLight, pointToLight);
    vec3 Li = pointToLightLengthSquared > Epsilon ?
      pointToLight * inversesqrt(pointToLightLengthSquared) :
      vec3(0.0);
    vec3 Lradiance = light.intensity;

    vec3 halfVector = Li + Lo;
    float halfVectorLengthSquared = dot(halfVector, halfVector);
    vec3 Lh = halfVectorLengthSquared > Epsilon ?
      halfVector * inversesqrt(halfVectorLengthSquared) :
      normal;
    float cosLi = max(0.0, dot(normal, Li));
    float cosLh = max(0.0, dot(normal, Lh));

    vec3 F  = FresnelSchlick(F0, max(0.0, dot(Lh, Lo)));
    float D = NdfGGX(cosLh, roughness);
    float G = GeometrySchlickGGX(cosLi, cosLo, roughness);

    vec3 specularBRDF = (F * D * G) / max(Epsilon, 4.0 * cosLi * cosLo);
    return shadow * (specularBRDF * Lradiance * cosLi) * falloff;
  }

  vec3 ClearCoatAmbientLighting(float roughness, vec3 F0, vec3 Lr, vec3 normal, float cosLo, float ambientOcclusion)
  {
    int specularTextureLevels = textureQueryLevels(g_envCubemap);
    vec3 specularIrradiance = textureLod(g_envCubemap, Lr, roughness * specularTextureLevels).rgb;
    vec2 specularBRDF = texture(g_brdfSampler, vec2(cosLo, roughness)).rg;
    float specularOcclusion = CalculateSpecularOcclusion(
      ambientOcclusion,
      cosLo,
      roughness);
    return (F0 * specularBRDF.x + specularBRDF.y) * specularIrradiance * specularOcclusion;
  }
  #endif

  #ifdef SHEEN
  vec3 SheenLighting(LightData light, float roughness, vec3 color, vec3 Lo, float cosLo, vec3 normal, vec3 worldPos)
  {
    float falloff = 1.0f;
    float shadow = 1.0f;
    if(light.type == 0)
    {
        shadow = CalculateCascadedDirectionalShadow(
          light,
          worldPos,
          normal,
          -light.direction);
    }
    else if(light.type == 1 || light.type == 2)
    {
        const float distance = length(light.worldPosition - worldPos);
        falloff = CalculateLocalLightRangeAttenuation(light, distance);
    }

    vec3 pointToLight = light.type == 0 ?
      -light.direction :
      light.worldPosition - worldPos;
    float pointToLightLengthSquared = dot(pointToLight, pointToLight);
    vec3 Li = pointToLightLengthSquared > Epsilon ?
      pointToLight * inversesqrt(pointToLightLengthSquared) :
      vec3(0.0);
    vec3 Lradiance = light.intensity;

    vec3 halfVector = Li + Lo;
    float halfVectorLengthSquared = dot(halfVector, halfVector);
    vec3 Lh = halfVectorLengthSquared > Epsilon ?
      halfVector * inversesqrt(halfVectorLengthSquared) :
      normal;
    float cosLi = max(0.0, dot(normal, Li));
    float cosLh = max(0.0, dot(normal, Lh));

    vec3 F  = FresnelSchlick(color, max(0.0, dot(Lh, Lo)));
    float D = NdfCharlie(cosLh, roughness);
    float V = GeometryNeubelt(cosLi, cosLo);

    vec3 specularBRDF = F * D * V;
    return shadow * (specularBRDF * Lradiance * cosLi) * falloff;
  }

  vec3 SheenAmbientLighting(float roughness, vec3 color, vec3 Lr, vec3 normal, float cosLo, float ambientOcclusion)
  {
    int specularTextureLevels = textureQueryLevels(g_envCubemap);
    vec3 specularIrradiance = textureLod(g_envCubemap, Lr, roughness * specularTextureLevels).rgb;
    vec2 specularBRDF = texture(g_brdfSampler, vec2(cosLo, roughness)).rg;
    float specularOcclusion = CalculateSpecularOcclusion(
      ambientOcclusion,
      cosLo,
      roughness);
    return (color * specularBRDF.x + specularBRDF.y) * specularIrradiance * specularOcclusion;
  }
  #endif
  
  // Constant normal incidence Fresnel factor for all dielectrics.
  const vec3 Fdielectric = vec3(0.04);
  
  void main() 
  {
    const vec3 viewDirection = normalize(vin.worldPosition - frame.cameraPosition.xyz);
    // gl_FragCoord is expressed in the full scene render extent. AO and the
    // resolved GI target are lower-resolution resources, so their texture size
    // must not be used to normalize the screen coordinate.
    const vec2 viewportUv = gl_FragCoord.xy *
      rcp(vec2(frame.viewportSize));
    
    MaterialData material = GetMaterialData();
    if(material.baseColorSampler != 0)
    {
      material.baseColorFactor = material.baseColorFactor * texture(textureSamplers[nonuniformEXT(ResolveTextureSamplerIndex(material.baseColorSampler))], vin.texcoord);
    }
    material.baseColorFactor *= vin.color;

    if(material.ormSampler != 0)
    {
      vec4 orm = texture(textureSamplers[nonuniformEXT(ResolveTextureSamplerIndex(material.ormSampler))], vin.texcoord);
      material.metallicFactor = material.metallicFactor * orm.b;
      material.roughnessFactor = material.roughnessFactor * orm.g;
    }

    float screenSpaceOcclusion = 1.0;
  #ifndef DISABLE_SCREEN_SPACE_AO
    screenSpaceOcclusion = texture(g_aoSampler, viewportUv).r;
  #endif
    float occlusion = screenSpaceOcclusion;
    if(material.occlusionSampler != 0)
    {
      float occlusionTex = texture(textureSamplers[nonuniformEXT(ResolveTextureSamplerIndex(material.occlusionSampler))], vin.texcoord).r;
      float mixed = mix(1.0, occlusionTex, material.occlusionStrength);
      occlusion = min(occlusion, mixed);
    }
    material.occlusionStrength = occlusion;

    if(material.emissiveSampler != 0)
    {
      material.emissiveFactor = material.emissiveFactor * texture(textureSamplers[nonuniformEXT(ResolveTextureSamplerIndex(material.emissiveSampler))], vin.texcoord);
    }

  #ifdef CLEAR_COAT
    if(material.clearcoatSampler != 0)
    {
      material.clearcoatFactor = material.clearcoatFactor * texture(textureSamplers[nonuniformEXT(ResolveTextureSamplerIndex(material.clearcoatSampler))], vin.texcoord).r;
    }
    if(material.clearcoatRoughnessSampler != 0)
    {
      material.clearcoatRoughnessFactor = material.clearcoatRoughnessFactor * texture(textureSamplers[nonuniformEXT(ResolveTextureSamplerIndex(material.clearcoatRoughnessSampler))], vin.texcoord).g;
    }
  #endif
  #ifdef SHEEN
    if(material.sheenColorSampler != 0)
    {
      material.sheenColorFactor.rgb = material.sheenColorFactor.rgb * texture(textureSamplers[nonuniformEXT(ResolveTextureSamplerIndex(material.sheenColorSampler))], vin.texcoord).rgb;
    }
    if(material.sheenRoughnessSampler != 0)
    {
      material.sheenRoughnessFactor = material.sheenRoughnessFactor * texture(textureSamplers[nonuniformEXT(ResolveTextureSamplerIndex(material.sheenRoughnessSampler))], vin.texcoord).g;
    }
  #endif
  #ifdef TRANSMISSION
    material.transmissionFactor = clamp(material.transmissionFactor, 0.0, 1.0);
    if(material.transmissionSampler != 0)
    {
      material.transmissionFactor *= texture(textureSamplers[nonuniformEXT(ResolveTextureSamplerIndex(material.transmissionSampler))], vin.texcoord).r;
    }
    material.thicknessFactor = max(material.thicknessFactor, 0.0);
    if(material.thicknessSampler != 0)
    {
      material.thicknessFactor *= texture(textureSamplers[nonuniformEXT(ResolveTextureSamplerIndex(material.thicknessSampler))], vin.texcoord).g;
    }
    material.indexOfRefraction = max(material.indexOfRefraction, 1.0);
    material.attenuationDistance = max(material.attenuationDistance, Epsilon);
    material.attenuationColor = clamp(material.attenuationColor, vec4(0.0), vec4(1.0));
  #endif

    const vec3 geometricNormal = normalize(vin.normal);
    vec3 normal;
    if(material.normalSampler != 0)
    {
      normal = 2.0 * texture(textureSamplers[nonuniformEXT(ResolveTextureSamplerIndex(material.normalSampler))], vin.texcoord).rgb - 1.0;
      normal.xy *= material.normalScale;
      normal = normalize(vin.tangentBasis * normal);
    }
    else
    {
      normal = geometricNormal;
    }

  #ifdef CLEAR_COAT
    vec3 clearcoatNormal = normal;
    if(material.clearcoatNormalSampler != 0)
    {
      clearcoatNormal = 2.0 * texture(textureSamplers[nonuniformEXT(ResolveTextureSamplerIndex(material.clearcoatNormalSampler))], vin.texcoord).rgb - 1.0;
      clearcoatNormal.xy *= material.clearcoatNormalScale;
      clearcoatNormal = normalize(vin.tangentBasis * clearcoatNormal);
    }
    float cosLoCC = max(0.0, dot(clearcoatNormal, -viewDirection));
    vec3 LrCC = 2.0 * cosLoCC * clearcoatNormal + viewDirection;
  #endif
    
    //outColor.xyz = AmbientLighting(material, vin.normal, vin.worldPosition, viewDirection);
    outColor.xyz = vec3(material.emissiveFactor.xyz);
    
    // Angle between surface normal and outgoing light direction.
    float cosLo = max(0.0, dot(normal, -viewDirection));
    
    // Specular reflection vector.
    vec3 Lr = 2.0 * cosLo * normal + viewDirection;
    
    // Fresnel reflectance at normal incidence (for metals use albedo color).
  #ifdef TRANSMISSION
    float iorFresnel = (material.indexOfRefraction - 1.0) / (material.indexOfRefraction + 1.0);
    vec3 F0 = mix(vec3(iorFresnel * iorFresnel), material.baseColorFactor.xyz, material.metallicFactor);
  #else
    vec3 F0 = mix(Fdielectric, material.baseColorFactor.xyz, material.metallicFactor);
  #endif
    
  #ifdef ALPHA_CUTOUT
    if(material.baseColorFactor.a < material.alphaCutoff)
    {
      discard;
    }
  #endif
  
    //outColor.xyz += vec3(texture(g_envCubemap, R).xyz);
    //outColor.xyz *= max(0.1, dot(normalize(-vec3(-0.3, -0.5, 0.1)), vin.normal.xyz)) * 0.5;
  
    const uint tileIndex = GetLightTileIndex(gl_FragCoord.xy, frame.viewportSize);
    const uint gridLength = uint(lightsGrid.instance.length());
    const bool hasLightTile = tileIndex < gridLength;
    const uint offset = hasLightTile ? lightsGrid.instance[tileIndex].offset : 0;
    const uint listLength = uint(culledLights.indices.length());
    const uint availableLights = offset < listLength ? listLength - offset : 0;
    const uint packedNumLights = hasLightTile ? lightsGrid.instance[tileIndex].num : 0u;
    const bool lightsOverflow = (packedNumLights & LIGHT_TILE_OVERFLOW_BIT) != 0u;
    const uint numLights = !hasLightTile ? 0u :
    #ifdef SUPPORT_LIGHTS_OVERFLOW
        (lightsOverflow ? min(packedNumLights & LIGHT_TILE_COUNT_MASK, uint(light.instance.length())) :
            min(packedNumLights & LIGHT_TILE_COUNT_MASK, availableLights));
    #else
        min(min(packedNumLights & LIGHT_TILE_COUNT_MASK, uint(LIGHTS_PER_TILE)), availableLights);
    #endif
    
    float environmentVisibility = 1.0;
    outColor.xyz += AmbientLighting(
      material,
      F0,
      Lr,
      normal,
      geometricNormal,
      vin.worldPosition,
      cosLo,
      viewportUv,
      environmentVisibility);
    if(!GlobalIlluminationDebugSuppressesDirectLighting())
    {
  #ifdef CLEAR_COAT
      outColor.xyz += material.clearcoatFactor * ClearCoatAmbientLighting(material.clearcoatRoughnessFactor, Fdielectric, LrCC, clearcoatNormal, cosLoCC, min(material.occlusionStrength, environmentVisibility));
  #endif
  #ifdef SHEEN
      outColor.xyz += SheenAmbientLighting(material.sheenRoughnessFactor, material.sheenColorFactor.rgb, Lr, normal, cosLo, min(material.occlusionStrength, environmentVisibility));
  #endif
    }

    const vec3 nonAnalyticLighting = outColor.xyz;
    
    if(!GlobalIlluminationDebugSuppressesDirectLighting())
    {
      for(int i = 0; i < numLights; i++)
      {
    #ifdef SUPPORT_LIGHTS_OVERFLOW
        uint index = lightsOverflow ? uint(i) : culledLights.indices[offset + i];
    #else
        uint index = culledLights.indices[offset + i];
    #endif
        if(index == uint(-1) ||
            index >= uint(light.instance.length()) ||
            light.instance[index].type == INVALID_LIGHT_TYPE)
        {
            continue;
        }

        outColor.xyz += CalculateLighting(light.instance[index], index, material, F0, -viewDirection, cosLo, normal, vin.worldPosition);
  #ifdef CLEAR_COAT
        outColor.xyz += material.clearcoatFactor * ClearCoatLighting(light.instance[index], material.clearcoatRoughnessFactor, Fdielectric, -viewDirection, cosLoCC, clearcoatNormal, vin.worldPosition);
  #endif
  #ifdef SHEEN
        outColor.xyz += SheenLighting(light.instance[index], material.sheenRoughnessFactor, material.sheenColorFactor.rgb, -viewDirection, cosLo, normal, vin.worldPosition);
  #endif
      }
    }

    outColor.xyz = nonAnalyticLighting +
      (outColor.xyz - nonAnalyticLighting) *
      CalculateDirectLightingOcclusion(screenSpaceOcclusion);

  #ifdef TRANSMISSION
    if(!GlobalIlluminationDebugSuppressesDirectLighting())
    {
      float dielectricFresnel =
      (material.indexOfRefraction - 1.0) /
      (material.indexOfRefraction + 1.0);
    vec3 dielectricF0 = vec3(dielectricFresnel * dielectricFresnel);
    vec2 transmissionBrdf = texture(
      g_brdfSampler,
      clamp(vec2(cosLo, material.roughnessFactor), vec2(0.0), vec2(1.0))).rg;
    vec3 transmissionFresnel = clamp(
      dielectricF0 * transmissionBrdf.x + transmissionBrdf.y,
      vec3(0.0),
      vec3(1.0));
    vec3 refractionNormal = gl_FrontFacing ? normal : -normal;
    vec3 refractionDirection = GetRefractionDirection(
      refractionNormal,
      -viewDirection,
      material.indexOfRefraction);
    vec3 transmissionRay = GetVolumeTransmissionRay(
      refractionDirection,
      material.thicknessFactor,
      vin.modelScale);
    float canTransmit = step(Epsilon, dot(
      refractionDirection,
      refractionDirection));
    vec4 exitClip = frame.projection * frame.view * vec4(vin.worldPosition + transmissionRay, 1.0);
    vec2 transmissionUv = viewportUv;
    float transmissionUvWeight = 0.0;
    if(exitClip.w > Epsilon)
    {
      vec2 exitNdc = exitClip.xy / exitClip.w;
      vec2 exitUv = vec2(
        exitNdc.x * 0.5 + 0.5,
        0.5 - exitNdc.y * 0.5);
      float exitEdgeDistance = min(
        min(exitUv.x, 1.0 - exitUv.x),
        min(exitUv.y, 1.0 - exitUv.y));
      transmissionUvWeight = smoothstep(0.0, 0.025, exitEdgeDistance);
      transmissionUv = clamp(exitUv, vec2(0.0), vec2(1.0));
    }

    float transmissionDistance = length(transmissionRay);
    vec3 volumeAttenuation = GetVolumeAttenuation(
      transmissionDistance,
      material.attenuationColor.rgb,
      material.attenuationDistance);

    float transmissionRoughness = ApplyIorToRoughness(
      material.roughnessFactor,
      material.indexOfRefraction);
    int transmissionMipCount = textureQueryLevels(g_transmissionFramebufferSampler);
    float transmissionFramebufferWidth = float(max(
      textureSize(g_transmissionFramebufferSampler, 0).x,
      1));
    float transmissionLod = clamp(
      log2(transmissionFramebufferWidth) * transmissionRoughness,
      0.0,
      float(max(transmissionMipCount - 1, 0)));
    vec3 framebufferRadiance = textureLod(
      g_transmissionFramebufferSampler,
      transmissionUv,
      transmissionLod).rgb;
    int transmissionEnvMipCount = textureQueryLevels(g_envCubemap);
    float transmissionEnvLod = transmissionRoughness *
      float(max(transmissionEnvMipCount - 1, 0));
    vec3 environmentDirection = mix(
      -viewDirection,
      refractionDirection,
      canTransmit);
    vec3 environmentRadiance = textureLod(
      g_envCubemap,
      environmentDirection,
      transmissionEnvLod).rgb;
    vec3 transmittedColor = mix(
      environmentRadiance,
      framebufferRadiance,
      transmissionUvWeight);
    transmittedColor *= material.baseColorFactor.rgb * volumeAttenuation;
    float dielectricTransmission = material.transmissionFactor *
      (1.0 - clamp(material.metallicFactor, 0.0, 1.0)) *
      canTransmit;
    outColor.xyz += transmittedColor *
      (vec3(1.0) - transmissionFresnel) *
      dielectricTransmission;
    }
  #endif

    outColor.a = material.baseColorFactor.a;    
  }
