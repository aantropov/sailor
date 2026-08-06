---
defines:
- EVSM
- SKINNING

includes:
- Shaders/Constants.glsl
- Shaders/Math.glsl
- Shaders/Lighting.glsl

glslCommon: |
  #version 460
  #extension GL_ARB_separate_shader_objects : enable
   
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
      uint skeletonOffset;
      uint padding0;
      uint padding1;
      uint padding2;
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
  layout(std430, set = 2, binding = 0) readonly buffer BoneMatricesSSBO
  {
      BoneData instance[];
  } bones;
  #endif
  
  layout(location=DefaultPositionBinding) in vec3 inPosition;

  #ifdef SKINNING
  layout(location=DefaultBoneIdsBinding) in uvec4 inBoneIds;
  layout(location=DefaultBoneWeightsBinding) in vec4 inBoneWeights;
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

      gl_Position = PushConstants.lightMatrix * modelMatrix * vec4(inPosition, 1.0);
  }
  
glslFragment: |

  #if defined(EVSM)
    layout(location=0) out vec4 outDepth;
  #else
    layout(location=0) out float outDepth;
  #endif
  
  void main() 
  {
    #if defined(EVSM)
      outDepth.x = exp(EVSM_C1 * gl_FragCoord.z);
      outDepth.y = outDepth.x * outDepth.x;
      outDepth.z = -exp(-EVSM_C2 * gl_FragCoord.z);
      outDepth.w = outDepth.z * outDepth.z;
    #else 
      outDepth = gl_FragCoord.z;
    #endif
  }
