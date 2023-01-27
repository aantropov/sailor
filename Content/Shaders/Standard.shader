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
  layout(location=4) flat out uint materialInstance;
  
  struct PerInstanceData
  {
      mat4 model;
      uint materialInstance;
  };
  
  struct MaterialData
  {
  	vec4 albedo;
  	vec4 ambient;
  	vec4 emission;
  	vec4 specular;
    
    float metallic;
    float roughness;
    float ao;
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
  layout(set = 1, binding = 4) uniform sampler2D g_brdfSampler;

  layout(std430, set = 2, binding = 0) readonly buffer PerInstanceDataSSBO
  {
      PerInstanceData instance[];
  } data;
  
  layout(std430, set = 3, binding = 0) readonly buffer MaterialDataSSBO
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
    materialInstance = data.instance[gl_InstanceIndex].materialInstance;
  }

glslFragment: |
  layout(location=0) in vec4 fragColor;
  layout(location=1) in vec2 fragTexcoord;
  layout(location=2) in vec3 fragNormal;
  layout(location=3) in vec3 worldPosition;
  layout(location=4) flat in uint materialInstance;
  
  layout(location = 0) out vec4 outColor;
  
  struct PerInstanceData
  {
      mat4 model;
      uint materialInstance;
  };
  
  struct MaterialData
  {
  	vec4 albedo;
  	vec4 ambient;
  	vec4 emission;
  	vec4 specular;
    
    float metallic;
    float roughness;
    float ao;
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
  layout(set = 1, binding = 4) uniform sampler2D g_brdfSampler;

  layout(std430, set = 2, binding = 0) readonly buffer PerInstanceDataSSBO
  {
      PerInstanceData instance[];
  } data;
  
  layout(std430, set = 3, binding = 0) readonly buffer MaterialDataSSBO
  {
      MaterialData instance[];
  } material;
  
  layout(set=3, binding=1) uniform sampler2D diffuseSampler;
  layout(set=3, binding=2) uniform sampler2D ambientSampler;
  
  MaterialData GetMaterialData()
  {
      return material.instance[materialInstance];
  }
  
  vec3 CalculateLighting(LightData light, MaterialData material, vec3 normal, vec3 worldPos, vec3 viewDir)
  {
  	vec3 diffuse = material.albedo.xyz;
  	vec3 worldNormal = normalize(normal);
    vec3 toLight = normalize(-light.direction);
    vec3 halfView = normalize(viewDir + toLight);
  
    float falloff = 1.0f;
    float attenuation = 1.0;
    
  	// Directional light
  	if(light.type == 0)
  	{
        attenuation = 1.0;
  	}
  	// Point light
  	else if(light.type == 1)
  	{
  		// Attenuation
        const float distance    = length(light.worldPosition - worldPos);
  		const float attenuation = 1.0 / (light.attenuation.x + light.attenuation.y * distance + light.attenuation.z * (distance * distance));
  		falloff 				= attenuation * (1 - pow(clamp(distance / light.bounds.x, 0,1), 2));
  	}
  	// Spot light
  	else if(light.type == 2)
  	{
  		// Attenuation
        vec3 lightDir = normalize(light.worldPosition - worldPos);
        float epsilon   = light.cutOff.x - light.cutOff.y;
  		float theta = dot(lightDir, normalize(-light.direction));
        const float distance    = length(light.worldPosition - worldPos);
  		const float attenuation = 1.0 / (light.attenuation.x + light.attenuation.y * distance + light.attenuation.z * (distance * distance));
  		falloff 				= attenuation * clamp((theta - light.cutOff.y) / epsilon, 0.0, 1.0);			
  		
  		if(theta < light.cutOff.y)
  		{       
  			falloff = 0.0f;
  		}
  	}
  	
    vec3 radiance     = light.intensity * attenuation; 
    
    vec3 F0      = mix(vec3(0.04), material.albedo.xyz, material.metallic);
    vec3 F       = FresnelSchlick(max(dot(halfView, viewDir), 0.0), F0);
    
    float NDF = DistributionGGX(worldNormal, halfView, material.roughness);       
    float G   = GeometrySmith(worldNormal, viewDir, toLight, material.roughness);    

    vec3 numerator    = NDF * G * F;
    float denominator = 4.0 * max(dot(worldNormal, viewDir), 0.0) * max(dot(worldNormal, toLight), 0.0)  + 0.0001;
    vec3 specular     = numerator / denominator;
    
    vec3 kS = F;
    vec3 kD = vec3(1.0) - kS;
  
    kD *= 1.0 - material.metallic;
    
    float NdotL = max(dot(worldNormal, toLight), 0.0);
    return (kD * material.albedo.xyz / PI + specular) * radiance * NdotL * falloff;    
  }
  
  vec3 AmbientLighting(MaterialData material, vec3 normal, vec3 worldPos, vec3 viewDir)
  {
    vec3 worldNormal = normalize(normal);
    vec3 reflected = reflect(viewDir, worldNormal);
   
    vec3 F0 = mix(vec3(0.04), material.albedo.xyz, material.metallic);
    vec3 F  = FresnelSchlickRoughness(max(dot(worldNormal, viewDir), 0.0), F0, material.roughness);
      
    // Ambient lighting
    vec3 kS = FresnelSchlick(max(dot(worldNormal, viewDir), 0.0), F0);
    vec3 kD = 1.0 - kS;
    kD *= 1.0 - material.metallic;	  
    vec3 irradiance = texture(g_envCubemap, worldNormal).rgb;
    vec3 diffuse      = irradiance * material.albedo.xyz;
    
    // Sample both the pre-filter map and the BRDF lut and combine them together as per the Split-Sum approximation to get the IBL specular part.
    const float MAX_REFLECTION_LOD = 4.0;
    vec3 prefilteredColor = textureLod(g_envCubemap, reflected, material.roughness * MAX_REFLECTION_LOD).rgb;    
    vec2 brdf  = texture(g_brdfSampler, vec2(max(dot(worldNormal, viewDir), 0.0), material.roughness)).rg;
    vec3 specular = prefilteredColor * (F * brdf.x + brdf.y);

    vec3 ambient = (kD * diffuse + specular);// * material.ao;
    
    return ambient;
  }
  
  void main() 
  {
  	const vec3 viewDirection = normalize(worldPosition - frame.cameraPosition.xyz);
    
    MaterialData material = GetMaterialData();
  	material.albedo = material.albedo * texture(diffuseSampler, fragTexcoord) * fragColor;
  	//material.ambient *= texture(ambientSampler, fragTexcoord);
  	
    outColor.xyz = AmbientLighting(material, fragNormal, worldPosition, viewDirection);
    
  #ifdef ALPHA_CUTOUT
  	if(material.diffuse.a < 0.05)
  	{
  		discard;
  	}
  #endif
  
    //outColor.xyz += vec3(texture(g_envCubemap, R).xyz);
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
  
          outColor.xyz += CalculateLighting(light.instance[index], material, fragNormal, worldPosition, viewDirection);
  		//outColor.xyz += 0.01;
      }
  	
    outColor.a = 1.0f;  	
  }
