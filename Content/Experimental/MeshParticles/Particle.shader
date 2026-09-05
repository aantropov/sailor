---
includes:
- Shaders/Constants.glsl
- Shaders/Math.glsl
- Shaders/Lighting.glsl

defines:
- SHADOW_CASTER
- PACKED_SHADOW_CASTER
- EVSM

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
  
  #ifdef PACKED_SHADOW_CASTER
  struct PerInstanceData
  {
      mat4 model;
      vec4 sphereBounds;
      uint materialInstance;
      uint skeletonOffset;
      uint isCulled;
      uint padding;
      vec4 bakedVolumeScale;
      float baseColorAlpha;
      uint baseColorSampler;
      float alphaCutoff;
      uint maskedPadding;
  };
  #else
  struct PerInstanceData
  {
      mat4 model;
      vec4 color;
      vec4 colorOld;
      uint materialInstance;
      uint isCulled;
  };
  #endif
  
  struct MaterialData
  {
    vec4 emission;
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
  
  layout(set=1, binding=8) uniform sampler2D shadowMaps[MAX_SHADOW_MAP_SAMPLERS];
  layout(set=1, binding=9) uniform sampler2D g_aoSampler;

  layout(std430, set = 2, binding = 0) readonly buffer PerInstanceDataSSBO
  {
      PerInstanceData instance[];
  } data;

  #ifdef PACKED_SHADOW_CASTER
  layout(std430, set = 2, binding = 1) readonly buffer InstanceIndicesSSBO
  {
      uint instance[];
  } instanceIndices;

  layout(std430, push_constant) uniform Constants
  {
      mat4 lightMatrix;
  } PushConstants;
  #endif
  
  layout(std430, set = 3, binding = 0) readonly buffer MaterialDataSSBO
  {
      MaterialData instance[];
  } material;
  
  layout(set=4, binding=0) uniform sampler2D shadowMapSampler;
  layout(set=5, binding=0) uniform sampler2D textureSamplers[MAX_TEXTURES_IN_SCENE];
    
  void main() 
  {
  #ifdef PACKED_SHADOW_CASTER
    uint instanceIndex = instanceIndices.instance[gl_InstanceIndex];
    gl_Position = PushConstants.lightMatrix * data.instance[instanceIndex].model * vec4(inPosition, 1.0);
  #else
    vec4 vertexPosition = data.instance[gl_InstanceIndex].model * vec4(inPosition, 1.0);
    vout.worldPosition = vertexPosition.xyz / vertexPosition.w;

    gl_Position = frame.projection * (frame.view * (data.instance[gl_InstanceIndex].model * vec4(inPosition, 1.0)));
    #if defined(SHADOW_CASTER)
      gl_Position = lightsMatrices.instance[0] * data.instance[gl_InstanceIndex].model * vec4(inPosition, 1.0);
    #endif
    
    vec4 worldNormal = data.instance[gl_InstanceIndex].model * vec4(inNormal, 0.0);

    vout.color = mix(data.instance[gl_InstanceIndex].color, data.instance[gl_InstanceIndex].colorOld, (inPosition.z + 1) / 2);

    vout.normal = normalize(worldNormal.xyz);
    vout.texcoord = inTexcoord;
    materialInstance = data.instance[gl_InstanceIndex].materialInstance;
    vout.tangentBasis = mat3(data.instance[gl_InstanceIndex].model) * mat3(inTangent, inBitangent, inNormal);
  #endif
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
  
  #if defined(SHADOW_CASTER) || defined(PACKED_SHADOW_CASTER)
    #if defined(EVSM)
      layout(location=0) out vec4 outDepth;
    #else
      layout(location=0) out float outDepth;
    #endif
  #else
    layout(location=0) out vec4 outColor;
  #endif
  
  struct MaterialData
  {
    vec4 emission;
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
  
  layout(set=1, binding=8) uniform sampler2D shadowMaps[MAX_SHADOW_MAP_SAMPLERS];
  layout(set=1, binding=9) uniform sampler2D g_aoSampler;
  
  layout(std430, set = 3, binding = 0) readonly buffer MaterialDataSSBO
  {
      MaterialData instance[];
  } material;
  
  layout(set=4, binding=0) uniform sampler2D shadowMapSampler;
  layout(set=5, binding=0) uniform sampler2D textureSamplers[MAX_TEXTURES_IN_SCENE];
  
  MaterialData GetMaterialData()
  {
      return material.instance[materialInstance];
  }
  
  void main() 
  {
    #if defined(SHADOW_CASTER) || defined(PACKED_SHADOW_CASTER)
      #if defined(EVSM)
        outDepth.x = exp(EVSM_C1 * gl_FragCoord.z);
        outDepth.y = outDepth.x * outDepth.x;
        outDepth.z = -exp(-EVSM_C2 * gl_FragCoord.z);
        outDepth.w = outDepth.z * outDepth.z;
      #else
        outDepth = gl_FragCoord.z;
      #endif
    #else   
        MaterialData material = GetMaterialData();
        outColor = vin.color;        
        
        vec3 normal = normalize(vin.tangentBasis * vec3(0,0,1));
        vec3 geometricNormal = NormalizeOrFallback(
          cross(dFdx(vin.worldPosition), dFdy(vin.worldPosition)), normal);
        float lighting = max(0, dot(-light.instance[0].direction, normal));
        float shadow = CalculateDirectionalShadow(SHADOW_TYPE_PCF,
          shadowMapSampler, lightsMatrices.instance[0], vin.worldPosition,
          geometricNormal, light.instance[0].shadowBias, 0);
        outColor.xyz *= max(0.05, min(lighting, shadow));
    #endif
  }
