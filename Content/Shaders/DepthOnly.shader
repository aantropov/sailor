---
defines:
- SKINNING
- MASKED

includes:
- Shaders/Constants.glsl

glslCommon: |
  #version 460
  #extension GL_ARB_separate_shader_objects : enable
  #extension GL_EXT_nonuniform_qualifier : require
   
glslVertex: |
  layout(location=DefaultPositionBinding) in vec3 inPosition;

  #ifdef SKINNING
  layout(location=DefaultBoneIdsBinding) in uvec4 inBoneIds;
  layout(location=DefaultBoneWeightsBinding) in vec4 inBoneWeights;
  #endif
  
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
      vec4 sphereBounds;
      uint materialInstance;
      uint skeletonOffset;
      uint isCulled;
      uint padding;
      vec4 bakedVolumeScale;
  };
  
  layout(std430, set = 1, binding = 0) readonly buffer PerInstanceDataSSBO
  {
      PerInstanceData instance[];
  } data;

  struct BoneData
  {
      mat4 matrix;
  };

  const uint INVALID_SKELETON_OFFSET = 0xFFFFFFFFu;

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

  #ifdef MASKED
  layout(location=DefaultTexcoordBinding) in vec2 inTexcoord;
  layout(location=DefaultColorBinding) in vec4 inColor;
  layout(location=0) out vec2 outTexcoord;
  layout(location=1) flat out uint outBaseColorSampler;
  layout(location=2) flat out float outAlphaCutoff;
  layout(location=3) out float outVertexAlpha;
  #endif
  
  void main() 
  {
      mat4 modelMatrix = data.instance[gl_InstanceIndex].model;
  #ifdef MASKED
      outTexcoord = inTexcoord;
      outBaseColorSampler = data.instance[gl_InstanceIndex].materialInstance;
      outAlphaCutoff = uintBitsToFloat(data.instance[gl_InstanceIndex].padding);
      outVertexAlpha = inColor.a;
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

      gl_Position = frame.projection * (frame.view * (modelMatrix * vec4(inPosition, 1.0)));
  }
  
glslFragment: |
  #ifdef MASKED
  layout(location=0) in vec2 inTexcoord;
  layout(location=1) flat in uint inBaseColorSampler;
  layout(location=2) flat in float inAlphaCutoff;
  layout(location=3) in float inVertexAlpha;

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

  void main() 
  {
  #ifdef MASKED
      float alpha = inVertexAlpha;
      if(inBaseColorSampler != 0u)
      {
        alpha *= texture(textureSamplers[nonuniformEXT(ResolveTextureSamplerIndex(inBaseColorSampler))], inTexcoord).a;
      }
      if(alpha < inAlphaCutoff)
      {
        discard;
      }
  #endif
  }
