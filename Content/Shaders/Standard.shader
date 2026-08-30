---
includes:
- Shaders/Constants.glsl
- Shaders/Math.glsl
- Shaders/Lighting.glsl
- Shaders/GlobalIllumination.glsl
- Shaders/ForwardLighting.glsl

defines:
- ALPHA_CUTOUT
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
  
  void main() 
  {
    uint instanceIndex = instanceIndices.instance[gl_InstanceIndex];
    mat4 modelMatrix = data.instance[instanceIndex].model;
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
  } vin;
  
  layout(location=0) out vec4 outColor;
  
  struct MaterialData
  {
    vec4 albedo;
    vec4 ambient;
    vec4 emission;
    
    float metallic;
    float roughness;
    float ao;
    
    uint albedoSampler;
    uint metalnessSampler;
    uint normalSampler;
    uint roughnessSampler;
  };
  
  layout(std430, set = 3, binding = 0) readonly buffer MaterialDataSSBO
  {
      MaterialData instance[];
  } material;
  
  MaterialData GetMaterialData()
  {
      return material.instance[materialInstance];
  }

  void main() 
  {
    const vec3 viewDirection = normalize(vin.worldPosition - frame.cameraPosition.xyz);
    // gl_FragCoord is expressed in the full scene render extent. AO and the
    // resolved GI target are lower-resolution resources, so their texture size
    // must not be used to normalize the screen coordinate.
    const vec2 viewportUv = gl_FragCoord.xy *
      rcp(vec2(frame.viewportSize));
    
    MaterialData material = GetMaterialData();
    if(material.albedoSampler != 0u)
    {
      material.albedo *= texture(textureSamplers[nonuniformEXT(ResolveTextureSamplerIndex(material.albedoSampler))], vin.texcoord);
    }
    material.albedo *= vin.color;

  #ifdef ALPHA_CUTOUT
    if(material.albedo.a < 0.5)
    {
      discard;
    }
  #endif

    if(material.metalnessSampler != 0u)
    {
      material.metallic *= texture(textureSamplers[nonuniformEXT(ResolveTextureSamplerIndex(material.metalnessSampler))], vin.texcoord).r;
    }
    if(material.roughnessSampler != 0u)
    {
      material.roughness *= texture(textureSamplers[nonuniformEXT(ResolveTextureSamplerIndex(material.roughnessSampler))], vin.texcoord).r;
    }
    material.ao = texture(g_aoSampler, viewportUv).r;
    
    const vec3 geometricNormal = normalize(vin.normal);
    vec3 normal = geometricNormal;
    if(material.normalSampler != 0u)
    {
      const vec3 tangentNormal = normalize(2.0 * texture(textureSamplers[nonuniformEXT(ResolveTextureSamplerIndex(material.normalSampler))], vin.texcoord).rgb - 1.0);
      normal = normalize(vin.tangentBasis * tangentNormal);
    }

    const ForwardPbrMaterial forwardMaterial = ForwardPbrMaterial(
      material.albedo.xyz,
      material.metallic,
      material.roughness,
      material.ao,
      1.0);
    
    // Angle between surface normal and outgoing light direction.
    float cosLo = max(0.0, dot(normal, -viewDirection));
    
    // Specular reflection vector.
    vec3 Lr = 2.0 * cosLo * normal + viewDirection;
    
    // Fresnel reflectance at normal incidence (for metals use albedo color).
    vec3 F0 = mix(Fdielectric, material.albedo.xyz, material.metallic);
    
    const bool bEvaluateDirectLighting =
      !GlobalIlluminationDebugSuppressesDirectLighting();
    ForwardLightList lightList = ForwardLightList(0u, 0u, false);
    if(bEvaluateDirectLighting)
    {
      lightList = ResolveForwardLightList(gl_FragCoord.xy);
    }

    float environmentVisibility = 1.0;
    const vec3 indirectLighting = CalculateForwardAmbientLighting(
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
    outColor.xyz = indirectLighting;
    
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

        outColor.xyz += CalculateForwardPbrLighting(
          light.instance[index],
          index,
          forwardMaterial,
          F0,
          -viewDirection,
          cosLo,
          normal,
          vin.worldPosition);
      }
      outColor.xyz = indirectLighting +
        (outColor.xyz - indirectLighting) *
        CalculateDirectLightingOcclusion(material.ao);
    }

    outColor.a = material.albedo.a;
  }
