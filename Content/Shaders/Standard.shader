---
includes:
- Shaders/Math.glsl
- Shaders/Lighting.glsl

defines:
- ALPHA_CUTOUT

glslCommon: |
  #version 460
  #extension GL_ARB_separate_shader_objects : enable

glslVertex: |
  layout(location=0) in vec3 inPosition;
  layout(location=1) in vec3 inNormal;
  layout(location=2) in vec2 inTexcoord;
  layout(location=3) in vec4 inColor;
  
  layout(location=0) out vec4 fragColor;
  layout(location=1) out vec2 fragTexcoord;
  layout(location=2) out vec3 fragNormal;
  layout(location=3) out vec3 worldPosition;
  layout(location=4) flat out uint instanceIndex;
  
  struct PerInstanceData
  {
      mat4 model;
      uint materialInstance;
  };
  
  struct MaterialData
  {
  	vec4 diffuse;
  	vec4 ambient;
  	vec4 emission;
  	vec4 specular;
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
  
  layout(set = 1, binding = 3) uniform samplerCube g_envCubemap;
  
  layout(std140, set = 2, binding = 0) readonly buffer PerInstanceDataSSBO
  {
      PerInstanceData instance[];
  } data;
  
  layout(std140, set = 3, binding = 0) readonly buffer MaterialDataSSBO
  {
      MaterialData instance[];
  } material;
  
  layout(set=3, binding=1) uniform sampler2D diffuseSampler;
  layout(set=3, binding=2) uniform sampler2D ambientSampler;
  
  void main() 
  {
  	vec4 vertexPosition = data.instance[gl_InstanceIndex].model * vec4(inPosition, 1.0);
  	worldPosition = vertexPosition.xyz / vertexPosition.w;
  	
      gl_Position = frame.projection * frame.view * data.instance[gl_InstanceIndex].model * vec4(inPosition, 1.0);
      vec4 worldNormal = data.instance[gl_InstanceIndex].model * vec4(inNormal, 0.0);
      	
      fragColor = inColor;
  	fragNormal = worldNormal.xyz;
      fragTexcoord = inTexcoord;
  	instanceIndex = gl_InstanceIndex;
  }

glslFragment: |
  layout(location=0) in vec4 fragColor;
  layout(location=1) in vec2 fragTexcoord;
  layout(location=2) in vec3 fragNormal;
  layout(location=3) in vec3 worldPosition;
  layout(location=4) flat in uint instanceIndex;
  
  layout(location = 0) out vec4 outColor;
  
  struct PerInstanceData
  {
      mat4 model;
      uint materialInstance;
  };
  
  struct MaterialData
  {
  	vec4 diffuse;
  	vec4 ambient;
  	vec4 emission;
  	vec4 specular;
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
  
  layout(set = 1, binding = 3) uniform samplerCube g_envCubemap;

  layout(std140, set = 2, binding = 0) readonly buffer PerInstanceDataSSBO
  {
      PerInstanceData instance[];
  } data;
  
  layout(std140, set = 3, binding = 0) readonly buffer MaterialDataSSBO
  {
      MaterialData instance[];
  } material;
  
  layout(set=3, binding=1) uniform sampler2D diffuseSampler;
  layout(set=3, binding=2) uniform sampler2D ambientSampler;
  
  MaterialData GetMaterialData()
  {
      return material.instance[data.instance[instanceIndex].materialInstance];
  }
  
  vec3 CalculateLighting(LightData light, MaterialData material, vec3 normal, vec3 worldPos, vec3 viewDir)
  {
  	//if(length(light.worldPosition - worldPos) > light.bounds.x)
  	//{
  		//return vec3(0.1,0.1,0.1);
  	//}
  	float falloff = 1.0f;
  	
  	vec3 diffuse = material.diffuse.xyz;
  	
  	// Directional light
  	if(light.type == 0)
  	{
  		float diff = max(dot(normal, -light.direction), 0.0);
  		diffuse *= light.intensity * diff;
  	}
  	// Point light
  	else if(light.type == 1)
  	{
  		// Attenuation
  		const float distance    = length(light.worldPosition - worldPos);
  		const float attenuation = 1.0 / (light.attenuation.x + light.attenuation.y * distance + light.attenuation.z * (distance * distance));
  		falloff 				= attenuation * (1 - pow(clamp(distance / light.bounds.x, 0,1), 2));
  		
  		vec3 lightDir = normalize(light.worldPosition - worldPos);
  		float diff = max(dot(normal, lightDir), 0.0);
  		diffuse *= light.intensity * diff;
  	}
  	// Spot light
  	else if(light.type == 2)
  	{
  		vec3 lightDir = normalize(light.worldPosition - worldPos);
  		float theta = dot(lightDir, normalize(-light.direction));
  		float epsilon   = light.cutOff.x - light.cutOff.y;
  		
  		// Attenuation
  		const float distance    = length(light.worldPosition - worldPos);
  		const float attenuation = 1.0 / (light.attenuation.x + light.attenuation.y * distance + light.attenuation.z * (distance * distance));
  		falloff 				= attenuation * clamp((theta - light.cutOff.y) / epsilon, 0.0, 1.0);			
  		
  		if(theta > light.cutOff.y)
  		{       
  			float diff = max(dot(normal, lightDir), 0.0);
  			diffuse *= light.intensity * diff;
  		}
  	}
  	
      return diffuse * falloff;
  }
  
  void main() 
  {
  	MaterialData material = GetMaterialData();
  	material.diffuse *= texture(diffuseSampler, fragTexcoord) * fragColor;
  	material.ambient *= texture(ambientSampler, fragTexcoord);
  	outColor = material.ambient + vec4(0,0,0,0);
  	
  #ifdef ALPHA_CUTOUT
  	if(material.diffuse.a < 0.05)
  	{
  		discard;
  	}
  #endif
  
  	// Sky
    vec3 I = normalize(worldPosition - frame.cameraPosition.xyz);
    vec3 R = reflect(I, normalize(fragNormal));
    
    outColor.xyz += vec3(textureLod(g_envCubemap, R, 0).xyz);
  	//outColor.xyz *= max(0.1, dot(normalize(-vec3(-0.3, -0.5, 0.1)), fragNormal.xyz)) * 0.5;
  
    vec2 numTiles = floor(frame.viewportSize / LIGHTS_CULLING_TILE_SIZE);
  	vec2 screenUv = vec2(gl_FragCoord.x, frame.viewportSize.y - gl_FragCoord.y);
    ivec2 tileId = ivec2(screenUv) / LIGHTS_CULLING_TILE_SIZE;
  	
  	ivec2 mod = ivec2(frame.viewportSize.x % LIGHTS_CULLING_TILE_SIZE, frame.viewportSize.y % LIGHTS_CULLING_TILE_SIZE);
  	ivec2 padding = ivec2(min(1, mod.x), min(1, mod.y));
  	
    uint tileIndex = uint(tileId.y * (numTiles.x + padding.x) + tileId.x);
  
  	const uint offset = lightsGrid.instance[tileIndex].offset;
  	const uint numLights = lightsGrid.instance[tileIndex].num;
  	
  	for(int i = 0; i < numLights; i++)
      {
          uint index = culledLights.indices[offset + i];
          if(index == uint(-1))
          {
              break;
          }
  
          vec3 viewDirection = worldPosition - frame.cameraPosition.xyz;
  		  outColor.xyz += CalculateLighting(light.instance[index], material, fragNormal, worldPosition, viewDirection);		
  		//outColor.xyz += 0.01;
      }
  	
    outColor.a = 1.0f;  	
  }
