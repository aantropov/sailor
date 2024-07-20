---
includes:
- Shaders/Constants.glsl
- Shaders/Math.glsl
- Shaders/Lighting.glsl

defines:
- ALPHA_CUTOUT

glslCommon: |
  #version 460
  #extension GL_ARB_separate_shader_objects : enable

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
      uint isCulled;
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
  
  layout(set=1, binding=8) uniform sampler2D shadowMaps[MAX_SHADOWS_IN_VIEW];
  layout(set=1, binding=9) uniform sampler2D g_aoSampler;

  layout(std430, set = 2, binding = 0) readonly buffer PerInstanceDataSSBO
  {
      PerInstanceData instance[];
  } data;
  
  layout(std430, set = 3, binding = 0) readonly buffer MaterialDataSSBO
  {
      MaterialData instance[];
  } material;
  
  layout(set=4, binding=0) uniform sampler2D textureSamplers[MAX_TEXTURES_IN_SCENE];
  
  void main() 
  {
    vec4 vertexPosition = data.instance[gl_InstanceIndex].model * vec4(inPosition, 1.0);
    vout.worldPosition = vertexPosition.xyz / vertexPosition.w;

    gl_Position = frame.projection * (frame.view * (data.instance[gl_InstanceIndex].model * vec4(inPosition, 1.0)));
    vec4 worldNormal = data.instance[gl_InstanceIndex].model * vec4(inNormal, 0.0);

    vout.color = inColor;
    vout.normal = normalize(worldNormal.xyz);
    vout.texcoord = inTexcoord;
    materialInstance = data.instance[gl_InstanceIndex].materialInstance;
    vout.tangentBasis = mat3(data.instance[gl_InstanceIndex].model) * mat3(inTangent, inBitangent, inNormal);
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
      uint isCulled;
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
  
  layout(set=1, binding=8) uniform sampler2D shadowMaps[MAX_SHADOWS_IN_VIEW];
  layout(set=1, binding=9) uniform sampler2D g_aoSampler;
  
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
  
  layout(set=4, binding=0) uniform sampler2D textureSamplers[MAX_TEXTURES_IN_SCENE];
  
  MaterialData GetMaterialData()
  {
      return material.instance[materialInstance];
  }
  
  const float Epsilon = 0.00001;
  vec3 CalculateLighting(LightData light, MaterialData material, vec3 F0, vec3 Lo,float cosLo, vec3 normal, vec3 worldPos)
  {
    float falloff = 1.0f;
    float attenuation = 1.0;
    float shadow = 1.0f;
    
    // Directional light
    if(light.type == 0)
    {
        attenuation = 1.0;
      
        const int cascadeLayer = min(SelectCascade(frame.view, worldPos, frame.cameraZNearZFar), NUM_CSM_CASCADES - 1);
        
        // EVSM only for the first cascade
        if(light.shadowType == 2 && cascadeLayer == 0)
        {
          const float bias = (1.0 - dot(normal, light.direction)) * (1 + cascadeLayer);
          shadow = ShadowCalculation_Evsm(shadowMaps[cascadeLayer], lightsMatrices.instance[cascadeLayer] * vec4(worldPos, 1.0f), bias, cascadeLayer);
        }
        else
        {
          const float bias = max(0.000075 * (1.0 - dot(normal, light.direction)), 0.000005);   
          shadow = ShadowCalculation_Pcf(shadowMaps[cascadeLayer], lightsMatrices.instance[cascadeLayer] * vec4(worldPos, 1.0f), bias, cascadeLayer);    
        }
    }
    // Point light
    else if(light.type == 1)
    {
      // Attenuation
      const float distance    = length(light.worldPosition - worldPos);
      const float attenuation = 1.0 / (light.attenuation.x + light.attenuation.y * distance + light.attenuation.z * (distance * distance));
      falloff         = attenuation * (1 - pow(clamp(distance / light.bounds.x, 0,1), 2));
    }
    // Spot light
    else if(light.type == 2)
    {
      // Attenuation
      vec3 lightDir = normalize(light.worldPosition - worldPos);
      float epsilon   = light.cutOff.x - light.cutOff.y;
      float theta = dot(lightDir, normalize(-light.direction));
      const float distance    = length(light.worldPosition - worldPos);
      const float attenuation = 1.0 / (light.attenuation.x + light.attenuation.y * distance + light.attenuation.z * (distance * distance));
      falloff         = attenuation * clamp((theta - light.cutOff.y) / epsilon, 0.0, 1.0);      
      
      if(theta < light.cutOff.y)
      {
        falloff = 0.0f;
      }
    }
    
    vec3 Li = -light.direction;
    vec3 Lradiance = light.intensity;

    // Half-vector between Li and Lo.
    vec3 Lh = normalize(Li + Lo);

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

    // Lambert diffuse BRDF.
    // We don't scale by 1/PI for lighting & material units to be more convenient.
    // See: https://seblagarde.wordpress.com/2012/01/08/pi-or-not-to-pi-in-game-lighting-equation/
    vec3 diffuseBRDF = kd * material.baseColorFactor.xyz;

    // Cook-Torrance specular microfacet BRDF.
    vec3 specularBRDF = (F * D * G) / max(Epsilon, 4.0 * cosLi * cosLo);

    // Total contribution for this light.
    return shadow * ((diffuseBRDF + specularBRDF) * Lradiance * cosLi) * falloff;    
  }
  
  vec3 AmbientLighting(MaterialData material, vec3 F0, vec3 Lr, vec3 normal, float cosLo)
  {
    // Sample diffuse irradiance at normal direction.
    vec3 irradiance = texture(g_irradianceCubemap, normal).rgb;
    
    // Calculate Fresnel term for ambient lighting.
    // Since we use pre-filtered cubemap(s) and irradiance is coming from many directions
    // use cosLo instead of angle with light's half-vector (cosLh above).
    // See: https://seblagarde.wordpress.com/2011/08/17/hello-world/
    vec3 F = FresnelSchlick(F0, cosLo);
    
    // Get diffuse contribution factor (as with direct lighting).
    vec3 kd = mix(vec3(1.0) - F, vec3(0.0), material.metallicFactor);
    
    // Irradiance map contains exitant radiance assuming Lambertian BRDF, no need to scale by 1/PI here either.
    vec3 diffuseIBL = kd * material.baseColorFactor.xyz * irradiance;
    
    // Sample pre-filtered specular reflection environment at correct mipmap level.
    int specularTextureLevels = textureQueryLevels(g_envCubemap);
    vec3 specularIrradiance = textureLod(g_envCubemap, Lr, material.roughnessFactor * specularTextureLevels).rgb;
    
    // Split-sum approximation factors for Cook-Torrance specular BRDF.
    vec2 specularBRDF = texture(g_brdfSampler, vec2(cosLo, material.roughnessFactor)).rg;
    
    // Total specular IBL contribution.
    vec3 specularIBL = (F0 * specularBRDF.x + specularBRDF.y) * specularIrradiance;
    
    // Total ambient lighting contribution.  
    return material.occlusionStrength * (diffuseIBL + specularIBL);
  }
  
  // Constant normal incidence Fresnel factor for all dielectrics.
  const vec3 Fdielectric = vec3(0.04);
  
  void main() 
  {
    const vec3 viewDirection = normalize(vin.worldPosition - frame.cameraPosition.xyz);
    const vec2 viewportUv = gl_FragCoord.xy * rcp(frame.viewportSize);
    
    MaterialData material = GetMaterialData();
    material.baseColorFactor = material.baseColorFactor * texture(textureSamplers[material.baseColorSampler], vin.texcoord) * vin.color;
    material.metallicFactor = material.metallicFactor * texture(textureSamplers[material.ormSampler], vin.texcoord).b;
    material.roughnessFactor = material.roughnessFactor * texture(textureSamplers[material.ormSampler], vin.texcoord).g;
    material.occlusionStrength = min(texture(g_aoSampler, viewportUv).r, texture(textureSamplers[material.occlusionSampler], vin.texcoord).r);
    material.emissiveFactor = material.emissiveFactor * texture(textureSamplers[material.emissiveSampler], vin.texcoord);
    
    vec3 normal = normalize(2.0 * texture(textureSamplers[material.normalSampler], vin.texcoord).rgb - 1.0) * material.normalScale;
    normal = normalize(vin.tangentBasis * normal);
    
    //outColor.xyz = AmbientLighting(material, vin.normal, vin.worldPosition, viewDirection);
    outColor.xyz = vec3(material.emissiveFactor.xyz);
    
    // Angle between surface normal and outgoing light direction.
    float cosLo = max(0.0, dot(normal, -viewDirection));
    
    // Specular reflection vector.
    vec3 Lr = 2.0 * cosLo * normal + viewDirection;
    
    // Fresnel reflectance at normal incidence (for metals use albedo color).
    vec3 F0 = mix(Fdielectric, material.baseColorFactor.xyz, material.metallicFactor);
    
  #ifdef ALPHA_CUTOUT
    if(material.baseColorFactor.a < material.alphaCutoff)
    {
      discard;
    }
  #endif
  
    //outColor.xyz += vec3(texture(g_envCubemap, R).xyz);
    //outColor.xyz *= max(0.1, dot(normalize(-vec3(-0.3, -0.5, 0.1)), vin.normal.xyz)) * 0.5;
  
    vec2 numTiles = floor(frame.viewportSize / LIGHTS_CULLING_TILE_SIZE);
    vec2 screenUv = vec2(gl_FragCoord.x, frame.viewportSize.y - gl_FragCoord.y);
    ivec2 tileId = ivec2(screenUv) / LIGHTS_CULLING_TILE_SIZE;
    
    ivec2 mod = ivec2(frame.viewportSize.x % LIGHTS_CULLING_TILE_SIZE, frame.viewportSize.y % LIGHTS_CULLING_TILE_SIZE);
    ivec2 padding = ivec2(min(1, mod.x), min(1, mod.y));
    
    uint tileIndex = uint(tileId.y * (numTiles.x + padding.x) + tileId.x);
  
    const uint offset = lightsGrid.instance[tileIndex].offset;
    const uint numLights = lightsGrid.instance[tileIndex].num;
    
    outColor.xyz += AmbientLighting(material, F0, Lr, normal, cosLo);
    
    for(int i = 0; i < numLights; i++)
    {
        uint index = culledLights.indices[offset + i];
        if(index == uint(-1))
        {
            break;
        }
    
        outColor.xyz += CalculateLighting(light.instance[index], material, F0, -viewDirection, cosLo, normal, vin.worldPosition);
    }

    outColor.a = material.baseColorFactor.a;    
  }
