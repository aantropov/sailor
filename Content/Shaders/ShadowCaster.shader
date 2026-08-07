---
defines:
- EVSM
- SKINNING
- MASKED

includes:
- Shaders/Constants.glsl
- Shaders/Math.glsl
- Shaders/Lighting.glsl

glslCommon: |
  #version 460
  #extension GL_ARB_separate_shader_objects : enable
  #extension GL_EXT_nonuniform_qualifier : require
   
glslVertex: |
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
  
  struct PerInstanceData
  {
      mat4 model;
      vec4 baseColorFactor;
      uint skeletonOffset;
      uint baseColorSampler;
      float alphaCutoff;
      uint padding;
  };

  struct BoneData
  {
      mat4 matrix;
  };

  const uint INVALID_SKELETON_OFFSET = 0xFFFFFFFFu;
  
  layout(std430, push_constant) uniform Constants
  {
    mat4 lightMatrix;
  } PushConstants;
  
  layout(std140, set = 1, binding = 0) readonly buffer PerInstanceDataSSBO
  {
      PerInstanceData instance[];
  } data;

  #ifdef SKINNING
  #ifdef MASKED
  layout(std430, set = 3, binding = 0) readonly buffer BoneMatricesSSBO
  #else
  layout(std430, set = 2, binding = 0) readonly buffer BoneMatricesSSBO
  #endif
  {
      BoneData instance[];
  } bones;
  #endif
  
  layout(location=DefaultPositionBinding) in vec3 inPosition;

  #ifdef MASKED
  layout(location=DefaultTexcoordBinding) in vec2 inTexcoord;
  layout(location=0) out vec2 outTexcoord;
  layout(location=1) flat out vec4 outBaseColorFactor;
  layout(location=2) flat out uint outBaseColorSampler;
  layout(location=3) flat out float outAlphaCutoff;
  #endif

  #ifdef SKINNING
  layout(location=DefaultBoneIdsBinding) in uvec4 inBoneIds;
  layout(location=DefaultBoneWeightsBinding) in vec4 inBoneWeights;
  #endif
  
  void main() 
  {
      mat4 modelMatrix = data.instance[gl_InstanceIndex].model;
  #ifdef MASKED
      outTexcoord = inTexcoord;
      outBaseColorFactor = data.instance[gl_InstanceIndex].baseColorFactor;
      outBaseColorSampler = data.instance[gl_InstanceIndex].baseColorSampler;
      outAlphaCutoff = data.instance[gl_InstanceIndex].alphaCutoff;
  #endif
  #ifdef SKINNING
      uint offset = data.instance[gl_InstanceIndex].skeletonOffset;
      if (offset != INVALID_SKELETON_OFFSET)
      {
          mat4 skinMatrix = bones.instance[offset + inBoneIds.x].matrix * inBoneWeights.x +
                            bones.instance[offset + inBoneIds.y].matrix * inBoneWeights.y +
                            bones.instance[offset + inBoneIds.z].matrix * inBoneWeights.z +
                            bones.instance[offset + inBoneIds.w].matrix * inBoneWeights.w;
          modelMatrix *= skinMatrix;
      }
  #endif

      gl_Position = PushConstants.lightMatrix * modelMatrix * vec4(inPosition, 1.0);
  }
  
glslFragment: |

  #ifdef MASKED
  layout(location=0) in vec2 inTexcoord;
  layout(location=1) flat in vec4 inBaseColorFactor;
  layout(location=2) flat in uint inBaseColorSampler;
  layout(location=3) flat in float inAlphaCutoff;

  #if defined(SAILOR_TEXTURE_REMAP)
  layout(std430, set=2, binding=0) readonly buffer TextureSamplerRemapSSBO
  {
      uint indices[MAX_TEXTURES_IN_SCENE];
  } textureSamplerRemap;
  layout(set=2, binding=1) uniform sampler2D textureSamplers[];
  #else
  layout(set=2, binding=0) uniform sampler2D textureSamplers[];
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
  #endif

  #if defined(EVSM)
    layout(location=0) out vec4 outDepth;
  #else
    layout(location=0) out float outDepth;
  #endif
  
  void main() 
  {
    #ifdef MASKED
      float alpha = inBaseColorFactor.a;
      if(inBaseColorSampler != 0u)
      {
        alpha *= texture(textureSamplers[nonuniformEXT(ResolveTextureSamplerIndex(inBaseColorSampler))], inTexcoord).a;
      }
      if(alpha < inAlphaCutoff)
      {
        discard;
      }
    #endif

    #if defined(EVSM)
      outDepth.x = exp(EVSM_C1 * gl_FragCoord.z);
      outDepth.y = outDepth.x * outDepth.x;
      outDepth.z = -exp(-EVSM_C2 * gl_FragCoord.z);
      outDepth.w = outDepth.z * outDepth.z;
    #else 
      outDepth = gl_FragCoord.z;
    #endif
  }
