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
layout(local_size_x = TILE_SIZE, local_size_y = TILE_SIZE, local_size_z = 1) in;

layout(std430)
struct LightData
{
	vec3 worldPosition;
	vec3 direction;
    vec3 intensity;
    vec3 attenuation;
    int type;
	vec3 bounds;
};

layout(std430)
struct CulledLights 
{	
	uint indices[LIGHTS_PER_TILE];
};

layout(push_constant) uniform Constants
{
	mat4 invViewProjection;
	ivec2 viewportSize;
	ivec2 numTiles;	
    int lightsNum;
} PushConstants;

layout(std430, set = 0, binding = 0) readonly buffer LightDataSSBO
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

// Vulkan NDC, Reverse Z: minDepth = 1.0, maxDepth = 0.0
const vec2 ndcUpperLeft = vec2(-1.0, -1.0);
const float ndcNearPlane = 0.0;
const float ndcFarPlane = 1.0;

struct ViewFrustum
{
	vec4 planes[6];
	vec3 points[8]; // 0-3 near 4-7 far
};

shared ViewFrustum frustum;
shared int lightCountForTile;
shared uint minDepthInt;
shared uint maxDepthInt;
shared uint groupIndices[LIGHTS_PER_TILE];

// Construct view frustum
ViewFrustum CreateFrustum(ivec2 tileId)
{	
	ViewFrustum frustum;

	const float bias = 0.00001f;
		
	float minDepth = uintBitsToFloat(maxDepthInt) + bias;
	float maxDepth = uintBitsToFloat(minDepthInt) - bias;
	
	const vec2 ndcUpperLeft = vec2(-1.0, -1.0);
	vec2 ndcSizePerTile = 2.0 * vec2(TILE_SIZE, TILE_SIZE) / PushConstants.viewportSize;

	vec2 ndcCorners[4];
	// Top left
	ndcCorners[0] = ndcUpperLeft + tileId * ndcSizePerTile; 
	// Top right
	ndcCorners[1] = vec2(ndcCorners[0].x + ndcSizePerTile.x, ndcCorners[0].y); 
	// Bottom right
	ndcCorners[2] = ndcCorners[0] + ndcSizePerTile; 
	// Bottom left
	ndcCorners[3] = vec2(ndcCorners[0].x, ndcCorners[0].y + ndcSizePerTile.y); 

	vec4 temp;
	for (int i = 0; i < 4; i++)
	{
		temp = PushConstants.invViewProjection * vec4(ndcCorners[i], minDepth, 1.0);
		frustum.points[i] = temp.xyz / temp.w;		
		
		temp = PushConstants.invViewProjection * vec4(ndcCorners[i], maxDepth, 1.0);
		frustum.points[i+4] = temp.xyz / temp.w;		
	}

	vec3 tempNormal;
	for (int i = 0; i < 4; i++)
	{
		//Cax+Cby+Ccz+Cd = 0, planes[i] = (Ca, Cb, Cc, Cd)
		//tempNormal: normal without normalization
		tempNormal = cross(frustum.points[i] - frame.cameraPosition.xyz, frustum.points[i + 1] - frame.cameraPosition.xyz);
		tempNormal = normalize(tempNormal);
		frustum.planes[i] = vec4(tempNormal, - dot(tempNormal, frustum.points[i]));
	}
	
	// near plane
	{
		tempNormal = cross(frustum.points[1] - frustum.points[0], frustum.points[3] - frustum.points[0]);
		tempNormal = -normalize(tempNormal);
		frustum.planes[4] = vec4(tempNormal, - dot(tempNormal, frustum.points[0]));
	}
	
	// far plane
	{
		tempNormal = cross(frustum.points[7] - frustum.points[4], frustum.points[5] - frustum.points[4]);
		tempNormal = -normalize(tempNormal);
		frustum.planes[5] = vec4(tempNormal, - dot(tempNormal, frustum.points[4]));
	}
	
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
	}
	
	barrier();	
	
	vec2 uv = vec2(gl_GlobalInvocationID.xy) / PushConstants.viewportSize;	
	uv.y = 1 - uv.y;
	float depth = texture(sceneDepth, uv).x;
	
	// Convert depth to uint so we can do atomic min and max comparisons between the threads
	uint depthInt = floatBitsToUint(depth);
	atomicMax(maxDepthInt, depthInt);
	atomicMin(minDepthInt, depthInt);
	
	barrier();
	
	if (gl_LocalInvocationIndex == 0)
	{
		frustum = CreateFrustum(tileId);
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

		float radius = light.instance[lightIndex].bounds.x; 
		vec4 lightDirection = vec4(light.instance[lightIndex].worldPosition, 1);
		
		// We check if the light exists in our frustum
		float distance = 0.0;
		for (uint j = 0; j < 6; j++) 
		{
			distance = dot(lightDirection, frustum.planes[j]) + radius;

			// If one of the tests fails, then there is no intersection
			if (distance < 0.0)
			{
				break;
			}
		}

		// If greater than zero, then it is a visible light
		if (distance >= 0.0) 
		{
			// Add index to the shared array of visible indices
			uint offset = atomicAdd(lightCountForTile, 1);
			groupIndices[offset] = int(lightIndex);
		}
	}
	barrier();

	if(gl_LocalInvocationIndex == 0)
	{
		// Copy calculated data
		for(uint i = 0; i < LIGHTS_PER_TILE; i++)
		{
			culledLights.instance[tileIndex].indices[i] = groupIndices[i];
		}
		
		// Ending symbol
		if(lightCountForTile < LIGHTS_PER_TILE)
		{
			culledLights.instance[tileIndex].indices[lightCountForTile] = -1;
		}
	}
}
END_CODE,

"defines":[]
}