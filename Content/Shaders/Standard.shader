{
"includes": [] ,


"glslCommon" :
BEGIN_CODE
#version 460
#extension GL_ARB_separate_shader_objects : enable

layout(std430)
struct LightData
{
	vec3 worldPosition;
	vec3 direction;
    vec3 intensity;
    vec3 attenuation; // vec3(quadratic, linear, constant)
    vec3 bounds;
    int type;
};

const int CULLED_LIGHTS_TILE_SIZE = 16;

layout(std430)
struct LightsGrid
{
	uint offset;
	uint num;
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

layout(std140, set = 2, binding = 0) readonly buffer PerInstanceDataSSBO
{
    PerInstanceData instance[];
} data;

layout(std140, set = 3, binding = 0) readonly buffer MaterialDataSSBO
{
    MaterialData instance[];
} material;

END_CODE,

"glslVertex":
BEGIN_CODE

layout(location=0) in vec3 inPosition;
layout(location=1) in vec3 inNormal;
layout(location=2) in vec2 inTexcoord;
layout(location=3) in vec4 inColor;

layout(location=0) out vec4 fragColor;
layout(location=1) out vec2 fragTexcoord;
layout(location=2) out vec3 fragNormal;
layout(location=3) out vec3 worldPosition;

MaterialData GetMaterialData()
{
    return material.instance[data.instance[gl_InstanceIndex].materialInstance];
}

void main() 
{
	vec4 vertexPosition = data.instance[gl_InstanceIndex].model * vec4(inPosition, 1.0);
	worldPosition = vertexPosition.xyz / vertexPosition.w;
	
    gl_Position = frame.projection * frame.view * data.instance[gl_InstanceIndex].model * vec4(inPosition, 1.0);
    vec4 worldNormal = data.instance[gl_InstanceIndex].model * vec4(inNormal, 0.0);
    	
    fragColor = inColor;
	fragNormal = worldNormal.xyz;
    fragTexcoord = inTexcoord;
}
END_CODE,

"glslFragment":
BEGIN_CODE
layout(location=0) in vec4 fragColor;
layout(location=1) in vec2 fragTexcoord;
layout(location=2) in vec3 fragNormal;
layout(location=3) in vec3 worldPosition;

layout(set=3, binding=1) uniform sampler2D diffuseSampler;
layout(location = 0) out vec4 outColor;

vec3 CalculateLighting(LightData light, vec3 normal, vec3 worldPos, vec3 viewDir)
{
	vec3 lightDir = normalize(light.worldPosition - worldPos);
	
	//if(length(light.worldPosition - worldPos) > light.bounds.x)
	//{
		//return vec3(0.1,0.1,0.1);
	//}
	
    // diffuse shading
    float diff = max(dot(normal, lightDir), 0.0);
    
	// attenuation
    float distance    = length(light.worldPosition - worldPos);
    float attenuation = 1.0 / (light.attenuation.z + light.attenuation.y * distance + light.attenuation.x * (distance * distance));
	
	// combine results
    vec3 diffuse = light.intensity * diff * texture(diffuseSampler, fragTexcoord).xyz;
    diffuse  *= attenuation;
    
	float fallof = 1 - clamp(distance / light.bounds.x, 0,1);
	
    return diffuse * fallof;
}

void main() 
{
	// Sky
    outColor = fragColor * texture(diffuseSampler, fragTexcoord);
	outColor.xyz *= max(0.1, dot(normalize(-vec3(-0.3, -0.5, 0.1)), fragNormal.xyz)) * 0.5;
     
    vec2 numTiles = floor(frame.viewportSize / CULLED_LIGHTS_TILE_SIZE);
	vec2 screenUv = vec2(gl_FragCoord.x, frame.viewportSize.y - gl_FragCoord.y);
    ivec2 tileId = ivec2(screenUv) / CULLED_LIGHTS_TILE_SIZE;
	
	ivec2 mod = ivec2(frame.viewportSize.x % CULLED_LIGHTS_TILE_SIZE, frame.viewportSize.y % CULLED_LIGHTS_TILE_SIZE);
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
		//outColor.xyz += CalculateLighting(light.instance[index], fragNormal, worldPosition, viewDirection);		
		outColor.xyz += 0.01;
    }
}
END_CODE,

"defines":[]
}