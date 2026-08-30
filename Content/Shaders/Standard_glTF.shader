---
includes:
- Shaders/Constants.glsl
- Shaders/Math.glsl
- Shaders/Lighting.glsl
- Shaders/GlobalIllumination.glsl
- Shaders/ForwardLighting.glsl

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
  
  struct BoneData
  {
    mat4 matrix;
  };

  const uint INVALID_SKELETON_OFFSET = 0xFFFFFFFFu;
  
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

    BuildForwardVertexFrame(
      modelMatrix,
      inPosition,
      inNormal,
      inTangent,
      inBitangent,
      vout.worldPosition,
      vout.normal,
      vout.tangentBasis);

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
  
  layout(std430, set = 3, binding = 0) readonly buffer MaterialDataSSBO
  {
      MaterialData instance[];
  } material;
  
  MaterialData GetMaterialData()
  {
      return material.instance[materialInstance];
  }

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

  vec3 CalculateLighting(
    LightData lightData,
    uint lightIndex,
    MaterialData materialData,
    ForwardPbrMaterial forwardMaterial,
    vec3 F0,
    vec3 surfaceToCamera,
    float cosLo,
    vec3 normal,
    vec3 worldPosition)
  {
    const ForwardLightSample lightSample = ResolveForwardLightSample(
      lightData,
      lightIndex,
      worldPosition,
      normal);
    vec3 directLighting = EvaluateForwardPbrDirectLighting(
      lightSample,
      forwardMaterial,
      F0,
      surfaceToCamera,
      cosLo,
      normal);
    const float falloff = lightSample.falloff;
    const float shadow = lightSample.shadow;
    const vec3 lightRadiance = lightSample.radiance;

  #ifdef TRANSMISSION
    vec3 refractionNormal = gl_FrontFacing ? normal : -normal;
    vec3 refractionDirection = GetRefractionDirection(
      refractionNormal,
      surfaceToCamera,
      materialData.indexOfRefraction);
    vec3 transmissionRay = GetVolumeTransmissionRay(
      refractionDirection,
      materialData.thicknessFactor,
      vin.modelScale);
    float transmissionRayLength = length(transmissionRay);
    float canTransmit = step(Epsilon, dot(
      refractionDirection,
      refractionDirection));
    float dielectricTransmission = materialData.transmissionFactor *
      (1.0 - clamp(materialData.metallicFactor, 0.0, 1.0));

    if(canTransmit > 0.0 &&
      dielectricTransmission > Epsilon)
    {
      vec3 exitPointToLight = lightData.type == 0 ?
        -lightData.direction :
        lightData.worldPosition - (worldPosition + transmissionRay);
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
          vec3 transmissionHalfVector = mirroredLi + surfaceToCamera;
          float transmissionHalfLengthSquared = dot(
            transmissionHalfVector,
            transmissionHalfVector);
          if(transmissionHalfLengthSquared > Epsilon)
          {
            vec3 transmissionH = transmissionHalfVector *
              inversesqrt(transmissionHalfLengthSquared);
            float alphaRoughness = ApplyIorToRoughness(
              materialData.roughnessFactor * materialData.roughnessFactor,
              materialData.indexOfRefraction);
            float transmissionDistribution = NdfGGXAlpha(
              clamp(dot(refractionNormal, transmissionH), 0.0, 1.0),
              max(alphaRoughness, Epsilon));
            float transmissionCosLi = clamp(
              dot(refractionNormal, mirroredLi),
              0.0,
              1.0);
            float transmissionCosLo = clamp(
              dot(refractionNormal, surfaceToCamera),
              0.0,
              1.0);
            float transmissionVisibility = VisibilityGGXAlpha(
              transmissionCosLi,
              transmissionCosLo,
              max(alphaRoughness, Epsilon));
            float dielectricFresnel =
              (materialData.indexOfRefraction - 1.0) /
              (materialData.indexOfRefraction + 1.0);
            vec3 transmissionFresnel = FresnelSchlick(
              vec3(dielectricFresnel * dielectricFresnel),
              clamp(abs(dot(surfaceToCamera, transmissionH)), 0.0, 1.0));

            float transmissionFalloff = falloff;
            if(lightData.type == 1)
            {
              transmissionFalloff = CalculateLocalLightRangeAttenuation(
                lightData,
                exitPointToLightLength);
            }
            else if(lightData.type == 2)
            {
              float coneRange = lightData.cutOff.x - lightData.cutOff.y;
              float coneCos = dot(
                transmissionLi,
                normalize(-lightData.direction));
              transmissionFalloff = CalculateLocalLightRangeAttenuation(
                lightData,
                exitPointToLightLength) * clamp(
                (coneCos - lightData.cutOff.y) / max(coneRange, Epsilon),
                0.0,
                1.0);
            }

            vec3 volumeAttenuation = GetVolumeAttenuation(
              transmissionRayLength,
              materialData.attenuationColor.rgb,
              materialData.attenuationDistance);
            directLighting += materialData.baseColorFactor.rgb *
              transmissionDistribution * transmissionVisibility *
              volumeAttenuation * (vec3(1.0) - transmissionFresnel) *
              dielectricTransmission * lightRadiance * transmissionFalloff;
          }
        }
      }
    }
  #endif

    // Total contribution for this light.
    return shadow * directLighting;
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

  #ifdef ALPHA_CUTOUT
    if(material.baseColorFactor.a < material.alphaCutoff)
    {
      discard;
    }
  #endif

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

    float diffuseWeight = 1.0;
  #ifdef TRANSMISSION
    diffuseWeight -= material.transmissionFactor;
  #endif
    const ForwardPbrMaterial forwardMaterial = ForwardPbrMaterial(
      material.baseColorFactor.xyz,
      material.metallicFactor,
      material.roughnessFactor,
      material.occlusionStrength,
      diffuseWeight);

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
    
    const bool bEvaluateDirectLighting =
      !GlobalIlluminationDebugSuppressesDirectLighting();
    ForwardLightList lightList = ForwardLightList(0u, 0u, false);
    if(bEvaluateDirectLighting)
    {
      lightList = ResolveForwardLightList(gl_FragCoord.xy);
    }
    
    float environmentVisibility = 1.0;
    outColor.xyz += CalculateForwardAmbientLighting(
      forwardMaterial,
      F0,
      Lr,
      normal,
      geometricNormal,
      vin.worldPosition,
      -viewDirection,
      cosLo,
      viewportUv,
      environmentVisibility);
    if(bEvaluateDirectLighting)
    {
  #ifdef CLEAR_COAT
      outColor.xyz += material.clearcoatFactor * ClearCoatAmbientLighting(material.clearcoatRoughnessFactor, Fdielectric, LrCC, clearcoatNormal, cosLoCC, min(material.occlusionStrength, environmentVisibility));
  #endif
  #ifdef SHEEN
      outColor.xyz += SheenAmbientLighting(material.sheenRoughnessFactor, material.sheenColorFactor.rgb, Lr, normal, cosLo, min(material.occlusionStrength, environmentVisibility));
  #endif
    }

    const vec3 nonAnalyticLighting = outColor.xyz;
    
    if(bEvaluateDirectLighting)
    {
      for(uint i = 0u; i < lightList.count; ++i)
      {
        const uint index = ResolveForwardLightIndex(lightList, i);
        if(index == uint(-1) ||
            index >= uint(light.instance.length()) ||
            light.instance[index].type == INVALID_LIGHT_TYPE)
        {
            continue;
        }

        outColor.xyz += CalculateLighting(
          light.instance[index],
          index,
          material,
          forwardMaterial,
          F0,
          -viewDirection,
          cosLo,
          normal,
          vin.worldPosition);
  #ifdef CLEAR_COAT
        outColor.xyz += material.clearcoatFactor * ClearCoatLighting(light.instance[index], material.clearcoatRoughnessFactor, Fdielectric, -viewDirection, cosLoCC, clearcoatNormal, vin.worldPosition);
  #endif
  #ifdef SHEEN
        outColor.xyz += SheenLighting(light.instance[index], material.sheenRoughnessFactor, material.sheenColorFactor.rgb, -viewDirection, cosLo, normal, vin.worldPosition);
  #endif
      }
    }

    if(bEvaluateDirectLighting)
    {
      outColor.xyz = nonAnalyticLighting +
        (outColor.xyz - nonAnalyticLighting) *
        CalculateDirectLightingOcclusion(screenSpaceOcclusion);
    }

  #ifdef TRANSMISSION
    if(bEvaluateDirectLighting)
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
