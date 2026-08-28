---
includes:
- Shaders/Constants.glsl
- Shaders/Math.glsl
- Shaders/Lighting.glsl
- Shaders/GlobalIllumination.glsl

defines:
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

  layout(location=0)
  flat out uint materialInstance;

  layout(location=1) out Vertex
  {
    vec2 texcoord;
    vec3 worldPosition;
    vec3 normal;
    vec4 color;
    mat3 tangentBasis;
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
    vec4 albedo;
    vec4 ambient;
    vec4 emission;
    vec4 layerUvScale;

    float metallic;
    float roughness;
    float ao;

    uint layer0Sampler;
    uint layer1Sampler;
    uint layer2Sampler;
    uint layer3Sampler;
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

  void main()
  {
    uint instanceIndex = instanceIndices.instance[gl_InstanceIndex];
    mat4 modelMatrix = data.instance[instanceIndex].model;
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

    vout.color = inColor;
    vout.normal = normal;
    vout.texcoord = inTexcoord;
    materialInstance = data.instance[instanceIndex].materialInstance;
    vout.tangentBasis = mat3(tangent, bitangent, normal);
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
    vec4 albedo;
    vec4 ambient;
    vec4 emission;
    vec4 layerUvScale;

    float metallic;
    float roughness;
    float ao;

    uint layer0Sampler;
    uint layer1Sampler;
    uint layer2Sampler;
    uint layer3Sampler;
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

  //layout(set=3, binding=1) uniform sampler2D layer0Sampler;
  //layout(set=3, binding=2) uniform sampler2D layer1Sampler;
  //layout(set=3, binding=3) uniform sampler2D layer2Sampler;
  //layout(set=3, binding=4) uniform sampler2D layer3Sampler;

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

    return CalculateLocalPcfShadow(
      shadowMaps[nonuniformEXT(shadowSamplerIndex)],
      lightsMatrices.instance[shadowMapIndex],
      DecodeShadowAtlasRect(packedAtlasTile),
      worldPosition,
      surfaceNormal,
      surfaceToLightDirection,
      length(light.worldPosition - worldPosition),
      (packedShadowIndex & SOFT_SHADOW_MAP_BIT) != 0u,
      light.shadowBias);
  }

  const float Epsilon = 0.00001;
  vec3 CalculateLighting(LightData light, uint lightIndex, MaterialData material, vec3 F0, vec3 Lo,float cosLo, vec3 normal, vec3 worldPos)
  {
    float falloff = 1.0f;
    float attenuation = 1.0;
    float shadow = 1.0f;
    vec3 Li = -light.direction;

    // Directional light
    if(light.type == 0)
    {
        attenuation = 1.0;

        shadow = CalculateCascadedDirectionalShadow(
          light,
          worldPos,
          normal,
          -light.direction);
    }
    // Point light
    else if(light.type == 1)
    {
      // Attenuation
      const float distance    = length(light.worldPosition - worldPos);
      falloff = CalculateLocalLightRangeAttenuation(light, distance);
      Li = normalize(light.worldPosition - worldPos);
      shadow = CalculateLocalLightShadow(light, lightIndex, worldPos, normal, Li);
    }
    // Spot light
    else if(light.type == 2)
    {
      // Attenuation
      vec3 lightDir = normalize(light.worldPosition - worldPos);
      Li = lightDir;
      float epsilon   = light.cutOff.x - light.cutOff.y;
      float theta = dot(lightDir, normalize(-light.direction));
      const float distance    = length(light.worldPosition - worldPos);
      falloff = CalculateLocalLightRangeAttenuation(light, distance) *
        clamp((theta - light.cutOff.y) / epsilon, 0.0, 1.0);

      if(theta < light.cutOff.y)
      {
        falloff = 0.0f;
      }

      shadow = CalculateLocalLightShadow(light, lightIndex, worldPos, normal, Li);
    }
    vec3 Lradiance = light.intensity;

    // Half-vector between Li and Lo.
    vec3 Lh = normalize(Li + Lo);

    // Calculate angles between surface normal and various light vectors.
    float cosLi = max(0.0, dot(normal, Li));
    float cosLh = max(0.0, dot(normal, Lh));

    // Calculate Fresnel term for direct lighting.
    vec3 F  = FresnelSchlick(F0, max(0.0, dot(Lh, Lo)));
    // Calculate normal distribution for specular BRDF.
    float D = NdfGGX(cosLh, material.roughness);
    // Calculate geometric attenuation for specular BRDF.
    float G = GeometrySchlickGGX(cosLi, cosLo, material.roughness);

    // Diffuse scattering happens due to light being refracted multiple times by a dielectric medium.
    // Metals on the other hand either reflect or absorb energy, so diffuse contribution is always zero.
    // To be energy conserving we must scale diffuse BRDF contribution based on Fresnel factor & metalness.
    vec3 kd = mix(vec3(1.0) - F, vec3(0.0), material.metallic);

    // Lambert diffuse BRDF.
    // We don't scale by 1/PI for lighting & material units to be more convenient.
    // See: https://seblagarde.wordpress.com/2012/01/08/pi-or-not-to-pi-in-game-lighting-equation/
    vec3 diffuseBRDF = kd * material.albedo.xyz;

    // Cook-Torrance specular microfacet BRDF.
    vec3 specularBRDF = (F * D * G) / max(Epsilon, 4.0 * cosLi * cosLo);

    // Total contribution for this light.
    return shadow * ((diffuseBRDF + specularBRDF) * Lradiance * cosLi) * falloff;
  }

  vec3 AmbientLighting(
    MaterialData material,
    vec3 F0,
    vec3 Lr,
    vec3 normal,
    vec3 geometricNormal,
    vec3 worldPosition,
    vec3 surfaceToCamera,
    float cosLo,
    vec2 screenUv)
  {
    // Baked probes replace diffuse environment irradiance only. Specular IBL
    // remains sourced from the pre-filtered environment cubemap.
    GlobalIlluminationSampleDebug globalIlluminationDebug;
    float environmentVisibility = 1.0;
    vec3 irradiance = vec3(0.0);
    if(!TryResolveGlobalIlluminationDiffuseIrradiance(
      screenUv,
      worldPosition,
      normal,
      geometricNormal,
      surfaceToCamera,
      Lr,
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

    // Calculate Fresnel term for ambient lighting.
    // Since we use pre-filtered cubemap(s) and irradiance is coming from many directions
    // use cosLo instead of angle with light's half-vector (cosLh above).
    // See: https://seblagarde.wordpress.com/2011/08/17/hello-world/
    vec3 F = FresnelSchlick(F0, cosLo);

    // Get diffuse contribution factor (as with direct lighting).
    vec3 kd = mix(vec3(1.0) - F, vec3(0.0), material.metallic);

    // Irradiance map contains exitant radiance assuming Lambertian BRDF, no need to scale by 1/PI here either.
    vec3 diffuseIBL = kd * material.albedo.xyz * irradiance;

    float ambientOcclusion = clamp(material.ao, 0.0, 1.0);
    if(globalIlluminationHeader.stateAndDebug.z ==
      GLOBAL_ILLUMINATION_DEBUG_INDIRECT_ONLY)
    {
      return diffuseIBL * ambientOcclusion;
    }

    // Sample pre-filtered specular reflection environment at correct mipmap level.
    int specularTextureLevels = textureQueryLevels(g_envCubemap);
    vec3 specularIrradiance = textureLod(g_envCubemap, Lr, material.roughness * specularTextureLevels).rgb;

    // Split-sum approximation factors for Cook-Torrance specular BRDF.
    vec2 specularBRDF = texture(g_brdfSampler, vec2(cosLo, material.roughness)).rg;

    // Total specular IBL contribution.
    vec3 specularIBL = (F0 * specularBRDF.x + specularBRDF.y) * specularIrradiance;

    // Ambient occlusion applies to all indirect lighting. Use a view- and
    // roughness-dependent approximation for specular IBL so smooth surfaces
    // retain plausible reflections without leaking them into occluded areas.
    float specularOcclusion = CalculateSpecularOcclusion(
      min(ambientOcclusion, environmentVisibility),
      cosLo,
      material.roughness);
    vec3 indirectLighting = diffuseIBL * ambientOcclusion +
      specularIBL * specularOcclusion;
    return ApplyGlobalIlluminationDebug(
      indirectLighting,
      globalIlluminationDebug);
  }

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
    vec4 layerWeights = max(vin.color, vec4(0.0));
    layerWeights /= max(dot(layerWeights, vec4(1.0)), Epsilon);
    vec4 landscapeAlbedo =
      texture(textureSamplers[nonuniformEXT(ResolveTextureSamplerIndex(material.layer0Sampler))], vin.texcoord * material.layerUvScale.x) * layerWeights.x +
      texture(textureSamplers[nonuniformEXT(ResolveTextureSamplerIndex(material.layer1Sampler))], vin.texcoord * material.layerUvScale.y) * layerWeights.y +
      texture(textureSamplers[nonuniformEXT(ResolveTextureSamplerIndex(material.layer2Sampler))], vin.texcoord * material.layerUvScale.z) * layerWeights.z +
      texture(textureSamplers[nonuniformEXT(ResolveTextureSamplerIndex(material.layer3Sampler))], vin.texcoord * material.layerUvScale.w) * layerWeights.w;
    material.albedo *= landscapeAlbedo;
    material.metallic = 0.0;
    material.ao = texture(g_aoSampler, viewportUv).r;

    const vec3 geometricNormal = normalize(vin.normal);
    vec3 normal = geometricNormal;

    //outColor.xyz = AmbientLighting(material, vin.normal, vin.worldPosition, viewDirection);
    outColor.xyz = vec3(0);

    // Angle between surface normal and outgoing light direction.
    float cosLo = max(0.0, dot(normal, -viewDirection));

    // Specular reflection vector.
    vec3 Lr = 2.0 * cosLo * normal + viewDirection;

    // Fresnel reflectance at normal incidence (for metals use albedo color).
    vec3 F0 = mix(Fdielectric, material.albedo.xyz, material.metallic);

    //outColor.xyz += vec3(texture(g_envCubemap, R).xyz);
    //outColor.xyz *= max(0.1, dot(normalize(-vec3(-0.3, -0.5, 0.1)), vin.normal.xyz)) * 0.5;

    const bool bEvaluateDirectLighting =
      !GlobalIlluminationDebugSuppressesDirectLighting();
    uint offset = 0u;
    bool lightsOverflow = false;
    uint numLights = 0u;
    if(bEvaluateDirectLighting)
    {
      const uint tileIndex = GetLightTileIndex(
        gl_FragCoord.xy,
        frame.viewportSize);
      const uint gridLength = uint(lightsGrid.instance.length());
      const bool hasLightTile = tileIndex < gridLength;
      offset = hasLightTile ? lightsGrid.instance[tileIndex].offset : 0u;
      const uint listLength = uint(culledLights.indices.length());
      const uint availableLights = offset < listLength
        ? listLength - offset
        : 0u;
      const uint packedNumLights = hasLightTile
        ? lightsGrid.instance[tileIndex].num
        : 0u;
      lightsOverflow =
        (packedNumLights & LIGHT_TILE_OVERFLOW_BIT) != 0u;
      numLights = !hasLightTile ? 0u :
    #ifdef SUPPORT_LIGHTS_OVERFLOW
        (lightsOverflow ? min(packedNumLights & LIGHT_TILE_COUNT_MASK, uint(light.instance.length())) :
            min(packedNumLights & LIGHT_TILE_COUNT_MASK, availableLights));
    #else
        min(min(packedNumLights & LIGHT_TILE_COUNT_MASK, uint(LIGHTS_PER_TILE)), availableLights);
    #endif
    }

    const vec3 indirectLighting = AmbientLighting(
      material,
      F0,
      Lr,
      normal,
      geometricNormal,
      vin.worldPosition,
      -viewDirection,
      cosLo,
      viewportUv);
    outColor.xyz = indirectLighting;

    if(bEvaluateDirectLighting)
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
      }
      outColor.xyz = indirectLighting +
        (outColor.xyz - indirectLighting) *
        CalculateDirectLightingOcclusion(material.ao);
    }

    outColor.a = material.albedo.a;
  }
