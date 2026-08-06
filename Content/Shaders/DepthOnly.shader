---
defines:
- SKINNING

includes:
- Shaders/Constants.glsl

glslCommon: |
  #version 460
  #extension GL_ARB_separate_shader_objects : enable
   
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
  layout(std430, set = 2, binding = 0) readonly buffer BoneMatricesSSBO
  {
      BoneData instance[];
  } bones;
  #endif
  
  void main() 
  {
      mat4 modelMatrix = data.instance[gl_InstanceIndex].model;
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
  void main() 
  {
  }
