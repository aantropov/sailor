---
includes:
- Shaders/Constants.glsl
- Shaders/Math.glsl
- Shaders/Motions.glsl

defines:
- ALPHA_CUTOUT
- MOTIONS
glslCommon: |
  #version 460
  #extension GL_ARB_separate_shader_objects : enable

glslVertex: |
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
      vec4 color;
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
  
  layout(std430, set = 2, binding = 0) readonly buffer PerInstanceDataSSBO
  {
      PerInstanceData instance[];
  } data;

  layout(std430, set = 2, binding = 1) readonly buffer InstanceIndicesSSBO
  {
      uint instance[];
  } instanceIndices;
  
  #ifdef CUSTOM_DATA
  layout(std140, set = 3, binding = 0) readonly buffer MaterialDataSSBO
  {
      MaterialData instance[];
  } material;
  
  MaterialData GetMaterialData()
  {
    return material.instance[data.instance[instanceIndices.instance[gl_InstanceIndex]].materialInstance];
  }
  #endif
  
  layout(location=DefaultPositionBinding) in vec3 inPosition;
  layout(location=DefaultNormalBinding) in vec3 inNormal;
  layout(location=DefaultTexcoordBinding) in vec2 inTexcoord;
  layout(location=DefaultColorBinding) in vec4 inColor;
  
  layout(location=0) out vec4 fragColor;
  layout(location=1) out vec2 fragTexcoord;
  layout(location=2) out vec3 fragNormal;
  
  void main() 
  {
      uint instanceIndex = instanceIndices.instance[gl_InstanceIndex];
      gl_Position = frame.projection * frame.view * data.instance[instanceIndex].model * vec4(inPosition, 1.0);
    #ifdef MOTIONS
      ObjectMotionData motion = data.instance[instanceIndex].motion;
      WriteMotionVertex(gl_Position, previousFrame.projection *
        (previousFrame.view * (motion.previousModel * vec4(inPosition, 1.0))), motion.state.y != 0u, motion.state.z != 0u);
    #endif
      vec4 worldNormal = data.instance[instanceIndex].model * vec4(inNormal, 0.0);
  
      fragColor = 1 - inColor * gl_Position.z / 3000;
  
  #ifdef CUSTOM_DATA
      fragColor *= GetMaterialData().color;
  #endif
  
      fragNormal = worldNormal.xyz;
      fragTexcoord = inTexcoord;
  }

glslFragment: |  
  layout(location=0) in vec4 fragColor;
  layout(location=1) in vec2 fragTexcoord;
  layout(location=2) in vec3 fragNormal;
  
  #ifndef NO_DIFFUSE
  layout(set=3, binding=1) uniform sampler2D diffuseSampler;
  #endif
  
  layout(location=0) out vec4 outColor;
  
  void main() 
  {
  #ifndef NO_DIFFUSE
      outColor = fragColor * texture(diffuseSampler, fragTexcoord);
  #endif
  
      outColor.xyz *= max(0.2, dot(normalize(-vec3(-0.3, -0.5, 0.1)), fragNormal.xyz));
      outColor.xyz += 0.007;
    #ifdef MOTIONS
      WriteMotionFragment(outColor.a);
    #endif
  }
