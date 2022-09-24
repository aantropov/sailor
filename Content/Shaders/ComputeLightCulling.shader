{
"includes": [] ,
"glslCommon" :
BEGIN_CODE
#version 460
#extension GL_ARB_separate_shader_objects : enable
END_CODE,

"glslCompute":
BEGIN_CODE

#define LIGHTS_PER_TILE 4
const int TILE_SIZE = 16;

struct LightData
{
	vec3 worldPosition;
	vec3 direction;
    vec3 intensity;
    vec3 attenuation;
    vec4 bounds;
    int type;
};

layout(std430)
struct CulledLights 
{	
	uint indices[LIGHTS_PER_TILE];
};

layout(push_constant) uniform Constants
{
	ivec2 viewportSize;
	ivec2 numTiles;
    int lightsNum;
} PushConstants;

layout(std140, set = 0, binding = 0) readonly buffer LightDataSSBO
{	
	LightData instance[];
} light;

layout(std430, set = 1, binding = 0) writeonly buffer CulledLightsSSBO
{
    CulledLights instance[];
} culledLights;

layout(set = 1, binding = 1) uniform sampler2D sceneDepth;

layout(set = 2, binding = 0) uniform FrameData
{
    mat4 view;
    mat4 projection;
    vec4 cameraPosition;
    float currentTime;
    float deltaTime;
} frame;

// vulkan ndc, minDepth = 0.0, maxDepth = 1.0
const vec2 ndcUpperLeft = vec2(-1.0, -1.0);
const float ndcNearPlane = 0.0;
const float ndcFarPlane = 1.0;

struct ViewFrustum
{
	vec4 planes[6];
	vec3 points[8]; // 0-3 near 4-7 far
};

layout(local_size_x = 32) in;

shared ViewFrustum frustum;
shared uint lightCountForTile;
shared uint minDepthInt;
shared uint maxDepthInt;
shared mat4 viewProjection;

// Construct view frustum
ViewFrustum createFrustum(ivec2 tileId)
{	
	ViewFrustum frustum;
	
	float minDepth = uintBitsToFloat(minDepthInt);
	float maxDepth = uintBitsToFloat(maxDepthInt);

	// Steps based on tile sale
	vec2 negativeStep = (2.0 * vec2(tileId)) / vec2(gl_NumWorkGroups.xy);
	vec2 positiveStep = (2.0 * vec2(tileId + ivec2(1, 1))) / vec2(gl_NumWorkGroups.xy);

	// Set up starting values for planes using steps and min and max z values
	frustum.planes[0] = vec4(1.0, 0.0, 0.0, 1.0 - negativeStep.x); // Left
	frustum.planes[1] = vec4(-1.0, 0.0, 0.0, -1.0 + positiveStep.x); // Right
	frustum.planes[2] = vec4(0.0, 1.0, 0.0, 1.0 - negativeStep.y); // Bottom
	frustum.planes[3] = vec4(0.0, -1.0, 0.0, -1.0 + positiveStep.y); // Top
	frustum.planes[4] = vec4(0.0, 0.0, -1.0, -minDepth); // Near
	frustum.planes[5] = vec4(0.0, 0.0, 1.0, maxDepth); // Far

	// Transform the first four planes
	for (uint i = 0; i < 4; i++) 
	{
		frustum.planes[i] *= viewProjection;
		frustum.planes[i] /= length(frustum.planes[i].xyz);
	}

	// Transform the depth planes
	frustum.planes[4] *= frame.view;
	frustum.planes[4] /= length(frustum.planes[4].xyz);
	frustum.planes[5] *= frame.view;
	frustum.planes[5] /= length(frustum.planes[5].xyz);
	
	return frustum;
}

void main()
{
	ivec2 location = ivec2(gl_GlobalInvocationID.xy);
	ivec2 itemID = ivec2(gl_LocalInvocationID.xy);
	ivec2 tileId = ivec2(gl_WorkGroupID.xy);
	ivec2 tileNumber = ivec2(gl_NumWorkGroups.xy);
	uint tileIndex = tileId.y * PushConstants.numTiles.x + tileId.x;

	if (gl_LocalInvocationIndex == 0)
	{
		maxDepthInt = 0;
		minDepthInt = 0xFFFFFFFF;
		lightCountForTile = 0;
		viewProjection = frame.projection * frame.view;
	}
	
	barrier();	
	
	vec2 uv = vec2(gl_GlobalInvocationID.xy) / PushConstants.viewportSize;
	float linearDepth = texture(sceneDepth, uv).x;
	
	// Convert depth to uint so we can do atomic min and max comparisons between the threads
	uint depthInt = floatBitsToUint(linearDepth);
	atomicMax(maxDepthInt, depthInt);
	atomicMin(minDepthInt, depthInt);

	barrier();
	
	if (gl_LocalInvocationIndex == 0)
	{
		frustum = createFrustum(tileId);
	}
	
	barrier();

	// Step 3: Cull lights.
	// Parallelize the threads against the lights now.
	// Can handle 256 simultaniously. Anymore lights than that and additional passes are performed
	uint threadCount = TILE_SIZE * TILE_SIZE;
	uint passCount = (PushConstants.lightsNum + threadCount - 1) / threadCount;
	for (uint i = 0; i < passCount; i++) 
	{
		// Get the lightIndex to test for this thread / pass. If the index is >= light count, then this thread can stop testing lights
		uint lightIndex = i * threadCount + gl_LocalInvocationIndex;
		if (lightIndex >= PushConstants.lightsNum || lightCountForTile >= LIGHTS_PER_TILE) 
		{
			break;
		}

		vec4 position = frame.view * vec4(light.instance[i].worldPosition, 1);
		position = vec4(position.xyz/position.w, 1);
		
		float radius = light.instance[i].bounds.x;

		// We check if the light exists in our frustum
		float distance = 0.0;
		for (uint j = 0; j < 6; j++) 
		{
			distance = dot(position, frustum.planes[j]) + radius;

			// If one of the tests fails, then there is no intersection
			if (distance <= 0.0) 
			{
				break;
			}
		}

		// If greater than zero, then it is a visible light
		if (distance > 0.0) 
		{
			// Add index to the shared array of visible indices
			uint offset = atomicAdd(lightCountForTile, 1);
			culledLights.instance[tileIndex].indices[offset] = int(lightIndex);
		}
	}

	barrier();

	if(lightCountForTile < LIGHTS_PER_TILE)
	{
		culledLights.instance[tileIndex].indices[lightCountForTile] = -1;
	}
}
END_CODE,

"defines":[]
}