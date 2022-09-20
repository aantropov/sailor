{
"includes": [] ,


"glslCommon" :
BEGIN_CODE
#version 460
#extension GL_ARB_separate_shader_objects : enable

struct LightData
{
	vec3 worldPosition;
	vec3 direction;
    vec3 intensity;
    vec3 attenuation;
    vec4 bounds;
    int type;
};

const int CULLED_LIGHTS_TILE_SIZE = 16;

#define LIGHTS_PER_TILE 4
struct CulledLights 
{	
	uint indices[LIGHTS_PER_TILE];
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
    vec4 cameraPosition;
    ivec2 viewportSize;
    float currentTime;
    float deltaTime;
} frame;

layout(std140, set = 1, binding = 0) readonly buffer LightDataSSBO
{	
	LightData instance[];
} light;

layout(std140, set = 1, binding = 1) readonly buffer CulledLightsSSBO
{
    CulledLights instance[];
} culledLights;

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

MaterialData GetMaterialData()
{
    return material.instance[data.instance[gl_InstanceIndex].materialInstance];
}

layout(location=0) in vec3 inPosition;
layout(location=1) in vec3 inNormal;
layout(location=2) in vec2 inTexcoord;
layout(location=3) in vec4 inColor;

layout(location=0) out vec4 fragColor;
layout(location=1) out vec2 fragTexcoord;
layout(location=2) out vec3 fragNormal;
layout(location=3) out vec3 worldPosition;

void main() 
{
    worldPosition = vec3(data.instance[gl_InstanceIndex].model * vec4(inPosition, 1.0));
    
    gl_Position = frame.projection * frame.view * vec4(worldPosition, 1.0);
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

void main() 
{
    outColor = fragColor * texture(diffuseSampler, fragTexcoord);
	outColor.xyz *= max(0.2, dot(normalize(-vec3(-0.3, -0.5, 0.1)), fragNormal.xyz));	
    outColor *= 1 - vec4(length(light.instance[0].worldPosition.xyz - worldPosition) / 100);
    
    ivec2 numTiles = frame.viewportSize / CULLED_LIGHTS_TILE_SIZE;
    ivec2 tileId = ivec2(gl_FragCoord.xy / CULLED_LIGHTS_TILE_SIZE);
    uint tileIndex = tileId.y * numTiles.x + tileId.x;
    
    for(int i = 0; i < LIGHTS_PER_TILE; i++)
    {
        uint index = culledLights.instance[tileIndex].indices[i];
        
        if(index == -1)
        {
            break;
        }
            
        outColor = vec4(1,1,1,1);
        //light.instance[index]
    }    
}
END_CODE,

"defines":[]
}