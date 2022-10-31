{
"includes": ["Shaders/Lighting.glsl"],
"glslCommon" :
BEGIN_CODE
#version 460
#extension GL_ARB_separate_shader_objects : enable
END_CODE,

"glslCompute":
BEGIN_CODE

layout(local_size_x = LIGHTS_CULLING_TILE_SIZE, local_size_y = LIGHTS_CULLING_TILE_SIZE, local_size_z = 1) in;

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

layout(std430, set = 1, binding = 0) buffer CulledLightsSSBO
{
    uint indices[];
} culledLights;

layout(std430, set = 1, binding = 1) writeonly buffer LightsGridSSBO
{
    LightsGrid instance[];
} lightsGrid;

layout(set = 1, binding = 2) uniform sampler2D sceneDepth;

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

shared ViewFrustum frustum;
shared int lightCountForTile;
shared uint minDepthInt;
shared uint maxDepthInt;
shared uint candidateIndices[LIGHTS_CULLING_TILE_SIZE];
shared float candidateImpact[LIGHTS_CULLING_TILE_SIZE];

// Construct view frustum
ViewFrustum CreateFrustum(ivec2 tileId)
{	
	vec3 eyePos = vec3(0,0,0);
	vec4 screenSpace[5];

    // Top left point
    screenSpace[0] = vec4(tileId.xy * LIGHTS_CULLING_TILE_SIZE, -1.0f, 1.0f );
    // Top right point
    screenSpace[1] = vec4(vec2(tileId.x + 1, tileId.y) * LIGHTS_CULLING_TILE_SIZE, -1.0f, 1.0f );
    // Bottom left point
    screenSpace[2] = vec4(vec2(tileId.x, tileId.y + 1) * LIGHTS_CULLING_TILE_SIZE, -1.0f, 1.0f );
    // Bottom right point
    screenSpace[3] = vec4(vec2(tileId.x + 1, tileId.y + 1) * LIGHTS_CULLING_TILE_SIZE, -1.0f, 1.0f );
	
	// Center point
    screenSpace[4] = (screenSpace[0] + screenSpace[3]) * 0.5f;
	
    vec3 viewSpace[5];
    // Now convert the screen space points to view space
    for ( int i = 0; i < 5; i++ ) 
	{
        viewSpace[i] = ScreenToView(screenSpace[i], frame.viewportSize, frame.invProjection).xyz;		
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
	uint tileIndex = tileId.y * tileNumber.x + tileId.x;
	
	if(gl_GlobalInvocationID.xy == vec2(0,0))
	{
		culledLights.indices[0] = 0;
	}

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
	uint threadCount = LIGHTS_CULLING_TILE_SIZE * LIGHTS_CULLING_TILE_SIZE;
	uint passCount = (PushConstants.lightsNum + threadCount - 1) / threadCount;
	
	for (uint i = 0; i < passCount; i++)
	{
		// Get the lightIndex to test for this thread / pass. If the index is >= light count, then this thread can stop testing lights
		uint lightIndex = i * threadCount + gl_LocalInvocationIndex;
		if (lightIndex >= PushConstants.lightsNum || lightCountForTile >= LIGHTS_CULLING_TILE_SIZE) 
		{
			break;
		}

		// Directional light
		if(light.instance[lightIndex].type == 0)
		{
			uint offset = atomicAdd(lightCountForTile, 1);
			if(offset < LIGHTS_CULLING_TILE_SIZE)
			{
				candidateIndices[offset] = int(lightIndex);
				candidateImpact[offset] = 0.0f;
				continue;
			}
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
			if(offset < LIGHTS_CULLING_TILE_SIZE)
			{
				candidateIndices[offset] = int(lightIndex);
				candidateImpact[offset] = length(lightPosViewSpace.xyz - vec3(frustum.center.xy, (zFar + zNear) * 0.5));
			}
		}
	}
	barrier();

	if(gl_LocalInvocationIndex == 0)
	{
		uint numCandidates = min(lightCountForTile, LIGHTS_CULLING_TILE_SIZE);
		
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
		
		// Fill lightsGrid
		const uint numLights = min(numCandidates, LIGHTS_PER_TILE);		
		const uint offset = atomicAdd(culledLights.indices[0], numLights) + 1;
		
		lightsGrid.instance[tileIndex].num = numLights;
		lightsGrid.instance[tileIndex].offset = offset;
		
		// Copy calculated data
		for(uint i = 0; i < numLights; i++)
		{
			culledLights.indices[offset + i] = candidateIndices[numCandidates - i - 1];
		}
	}
}
END_CODE,

"defines":[]
}