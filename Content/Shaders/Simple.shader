---
includes:
- Shaders/Lighting.glsl
defines:
- ALPHA_CUTOUT
glslCommon: |
  #version 460
  #extension GL_ARB_separate_shader_objects : enable

glslVertex: |
  struct LightData
  {
  	vec3 worldPosition;
  	vec3 direction;
  	vec3 intensity;
  	vec3 attenuation;
  	vec4 bounds;
  	int type;
  };
  
  struct PerInstanceData
  {
  	mat4 model;
  	uint materialInstance;
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
  	float currentTime;
  	float deltaTime;
  } frame;
  
  layout(std140, set = 1, binding = 0) readonly buffer LightDataSSBO
  {	
  	LightData instance[];
  } light;
  
  layout(std140, set = 2, binding = 0) readonly buffer PerInstanceDataSSBO
  {
  	PerInstanceData instance[];
  } data;
  
  #ifdef CUSTOM_DATA
  layout(std140, set = 3, binding = 0) readonly buffer MaterialDataSSBO
  {
  	MaterialData instance[];
  } material;
  
  MaterialData GetMaterialData()
  {
  	return material.instance[data.instance[gl_InstanceIndex].materialInstance];
  }
  #endif
  
  layout(location=0) in vec3 inPosition;
  layout(location=1) in vec3 inNormal;
  layout(location=2) in vec2 inTexcoord;
  layout(location=3) in vec4 inColor;
  
  layout(location=0) out vec4 fragColor;
  layout(location=1) out vec2 fragTexcoord;
  layout(location=2) out vec3 fragNormal;
  
  void main() 
  {
  	gl_Position = frame.projection * frame.view * data.instance[gl_InstanceIndex].model * vec4(inPosition, 1.0);
  	vec4 worldNormal = data.instance[gl_InstanceIndex].model * vec4(inNormal, 0.0);
  
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
  
  layout(location = 0) out vec4 outColor;
  
  void main() 
  {
  #ifndef NO_DIFFUSE
  	outColor = fragColor * texture(diffuseSampler, fragTexcoord);
  #endif
  
  	outColor.xyz *= max(0.2, dot(normalize(-vec3(-0.3, -0.5, 0.1)), fragNormal.xyz));
  	outColor.xyz += 0.007;    
  }