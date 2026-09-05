---
includes:
- Shaders/Constants.glsl
- Shaders/Math.glsl
- Shaders/Lighting.glsl
- Shaders/Motions.glsl

defines:
- SHADOW
- MOTIONS

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
      uint skeletonOffset;
      uint isCulled;
      uint padding;
      vec4 bakedVolumeScale;
      ObjectMotionData motion;
  };
  
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
    vec4 vertexPosition = data.instance[instanceIndex].model * vec4(inPosition, 1.0);
    vout.worldPosition = vertexPosition.xyz / vertexPosition.w;

    gl_Position = frame.projection * (frame.view * (data.instance[instanceIndex].model * vec4(inPosition, 1.0)));
  #if defined(MOTIONS) && !defined(SHADOW)
    ObjectMotionData motion = data.instance[instanceIndex].motion;
    WriteMotionVertex(gl_Position, previousFrame.projection *
      (previousFrame.view * (motion.previousModel * vec4(inPosition, 1.0))), motion.state.y != 0u, motion.state.z != 0u);
  #endif
    
    #if defined(SHADOW)
        gl_Position = lightsMatrices.instance[0] * data.instance[instanceIndex].model * vec4(inPosition, 1.0);
    #endif

    vec4 worldNormal = data.instance[instanceIndex].model * vec4(inNormal, 0.0);

    vout.color = inColor;
    vout.normal = normalize(worldNormal.xyz);
    vout.texcoord = inTexcoord;
    materialInstance = data.instance[instanceIndex].materialInstance;
    vout.tangentBasis = mat3(data.instance[instanceIndex].model) * mat3(inTangent, inBitangent, inNormal);
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
  
  #if defined(SHADOW)
    layout(location=0) out float outDepth;
  #else
    layout(location=0) out vec4 outColor;
  #endif
  
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
  
  layout(std430, set = 2, binding = 0) readonly buffer PerInstanceDataSSBO
  {
      PerInstanceData instance[];
  } data;
  
  layout(std430, set = 3, binding = 0) readonly buffer MaterialDataSSBO
  {
      MaterialData instance[];
  } material;
  
  #if defined(SAILOR_TEXTURE_REMAP)
  layout(std430, set=4, binding=0) readonly buffer TextureSamplerRemapSSBO
  {
      uint indices[];
  } textureSamplerRemap;
  layout(set=4, binding=1) uniform sampler2D textureSamplers[];
  #else
  layout(set=4, binding=0) uniform sampler2D textureSamplers[];
  #endif
  
  MaterialData GetMaterialData()
  {
      return material.instance[materialInstance];
  }
  
  void main() 
  {
    #if defined(SHADOW)
        outDepth = gl_FragCoord.z;
    #else
        MaterialData material = GetMaterialData();
        outColor.xyz = material.emission.xyz;
        outColor.a = material.albedo.a;
      #ifdef MOTIONS
        WriteMotionFragment(outColor.a);
      #endif
    #endif
  }
