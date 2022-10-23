{
"includes": [] ,
"glslCommon" :
BEGIN_CODE
#version 460
#extension GL_ARB_separate_shader_objects : enable
END_CODE,

"glslCompute":
BEGIN_CODE

#define CANDIDATES_PER_TILE 256
#define LIGHTS_PER_TILE 128
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
    mat4 invProjection;
    vec4 cameraPosition;
    ivec2 viewportSize;
    float currentTime;
    float deltaTime;
} frame;

// Vulkan NDC, Reverse Z: minDepth = 1.0, maxDepth = 0.0
const vec2 ndcUpperLeft = vec2(-1.0, -1.0);
const float ndcNearPlane = 0.0;
const float ndcFarPlane = 1.0;

struct ViewFrustum
{
	vec4 planes[4];	
	vec2 center;
};

shared ViewFrustum frustum;
shared int lightCountForTile;
shared uint minDepthInt;
shared uint maxDepthInt;
shared uint candidateIndices[CANDIDATES_PER_TILE];
shared float candidateImpact[CANDIDATES_PER_TILE];

// Convert clip space coordinates to view space
vec4 ClipToView(vec4 clip)
{
    // View space position
    vec4 view = frame.invProjection * clip;
    // Perspective projection
    view = view/view.w;

	// Reverse Z
	view.z *= -1;
	
    return view;
}

// Convert screen space coordinates to view space.
vec4 ScreenToView(vec4 screen)
{
    // Convert to normalized texture coordinates
    vec2 texCoord = screen.xy / frame.viewportSize;

    // Convert to clip space
    vec4 clip = vec4(vec2(texCoord.x, texCoord.y) * 2.0f - 1.0f, screen.z, screen.w);

    return ClipToView( clip );
}

vec4 ComputePlane(vec3 p0, vec3 p1, vec3 p2)
{
    vec4 plane;

    vec3 v0 = p1 - p0;
    vec3 v2 = p2 - p0;

    plane.xyz = normalize( cross( v0, v2 ) );

    // Compute the distance to the origin using p0.
    plane.w = dot(plane.xyz, p0);

    return plane;
}

// Construct view frustum
ViewFrustum CreateFrustum(ivec2 tileId)
{	
	vec3 eyePos = vec3(0,0,0);
	vec4 screenSpace[5];

    // Top left point
    screenSpace[0] = vec4(tileId.xy * TILE_SIZE, -1.0f, 1.0f );
    // Top right point
    screenSpace[1] = vec4(vec2(tileId.x + 1, tileId.y) * TILE_SIZE, -1.0f, 1.0f );
    // Bottom left point
    screenSpace[2] = vec4(vec2(tileId.x, tileId.y + 1) * TILE_SIZE, -1.0f, 1.0f );
    // Bottom right point
    screenSpace[3] = vec4(vec2(tileId.x + 1, tileId.y + 1) * TILE_SIZE, -1.0f, 1.0f );
	
	// Center point
    screenSpace[4] = (screenSpace[0] + screenSpace[3]) * 0.5f;
	
    vec3 viewSpace[5];
    // Now convert the screen space points to view space
    for ( int i = 0; i < 5; i++ ) 
	{
        viewSpace[i] = ScreenToView(screenSpace[i]).xyz;		
    }

	ViewFrustum frustum;

    // Left plane
    frustum.planes[0] = ComputePlane(eyePos, viewSpace[2], viewSpace[0]);
    // Right plane
    frustum.planes[1] = ComputePlane(eyePos, viewSpace[1], viewSpace[3]);
    // Top plane
    frustum.planes[2] = ComputePlane(eyePos, viewSpace[0], viewSpace[1]);
    // Bottom plane
    frustum.planes[3] = ComputePlane(eyePos, viewSpace[3], viewSpace[2]);
	
	frustum.center = viewSpace[4].xy;
	
	return frustum;
}

bool Overlaps(vec3 lightPos, float radius, ViewFrustum frustum, float zNear, float zFar)
{
	if (lightPos.z - radius > zNear || lightPos.z + radius < zFar) 
	{
		return false;
	}
	
	for(int i = 0; i < 4; i++)
	{
		if(dot(frustum.planes[i].xyz, lightPos) - frustum.planes[i].w < -radius)
		{
			return false;
		}
	}

	return true;
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
	
	vec2 uv = vec2(gl_GlobalInvocationID.xy + vec2(0.5, 0.5))/ PushConstants.viewportSize;
	uv.y = 1 - uv.y;
	float linearDepth = texture(sceneDepth, uv).x;
	
	// Convert depth to uint so we can do atomic min and max comparisons between the threads
	uint depthInt = floatBitsToUint(linearDepth);
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
		if (lightIndex >= PushConstants.lightsNum || lightCountForTile >= CANDIDATES_PER_TILE) 
		{
			break;
		}

		float radius = light.instance[lightIndex].bounds.x; 
		vec4 lightPosViewSpace = frame.view * vec4(light.instance[lightIndex].worldPosition, 1);
		lightPosViewSpace /= lightPosViewSpace.w;
		
		// Reverse Z
		lightPosViewSpace.z *= -1;
		
		float zFar = uintBitsToFloat(maxDepthInt);
		float zNear = uintBitsToFloat(minDepthInt);
		
		// Add extra bounds
		const float diff = zFar - zNear;
		zFar -= diff;
		zNear += diff;
		
		// If greater than zero, then it is a visible light
		if (Overlaps(lightPosViewSpace.xyz, radius, frustum, zNear, zFar)) 
		{
			// Add index to the shared array of visible indices
			uint offset = atomicAdd(lightCountForTile, 1);
			if(offset < CANDIDATES_PER_TILE)
			{
				candidateIndices[offset] = int(lightIndex);
				candidateImpact[offset] = length(lightPosViewSpace.xyz - vec3(frustum.center.xy, (zFar + zNear) * 0.5));
			}
		}
	}
	barrier();

	if(gl_LocalInvocationIndex == 0)
	{
		uint numCandidates = min(lightCountForTile, CANDIDATES_PER_TILE);
		
		// Sort by distance
		if(numCandidates > LIGHTS_PER_TILE)
		{
			uint numSorted = LIGHTS_PER_TILE;

			for(uint i = 0; i < numCandidates-1; i++)
			{
				for(uint j = 0; j < numCandidates - i - 1; j++)
				{
					if(candidateImpact[j] < candidateImpact[j+1])
					{
						float value = candidateImpact[j];
						candidateImpact[j] = candidateImpact[j+1];
						candidateImpact[j+1] = value;
						
						uint index = candidateIndices[j];
						candidateIndices[j] = candidateIndices[j+1];
						candidateIndices[j+1] = index;
					}
				}
				
				--numSorted;
				
				if(numSorted == 0)
				{
					break;
				}
			}
		}
		
		// Copy calculated data
		for(uint i = 0; i < min(numCandidates, LIGHTS_PER_TILE); i++)
		{
			culledLights.instance[tileIndex].indices[i] = candidateIndices[numCandidates - i - 1];
		}
		
		// Ending symbol
		if(numCandidates < LIGHTS_PER_TILE)
		{
			culledLights.instance[tileIndex].indices[numCandidates] = -1;
		}
	}
}
END_CODE,

"defines":[]
}