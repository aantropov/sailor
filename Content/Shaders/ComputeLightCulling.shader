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

struct CulledLights 
{	
	uint indices[LIGHTS_PER_TILE];
};

layout(push_constant) uniform Constants
{
    int lightsNum;
	ivec2 viewportSize;
	ivec2 NumTiles;
} PushConstants;

layout(std140, set = 0, binding = 0) buffer readonly LightDataSSBO
{	
	LightData lightsData[];
};

layout(std140, set = 1, binding = 0) buffer writeonly CulledLightsSSBO
{
    CulledLights culledLights[];
};

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
shared float minDepth;
shared float maxDepth;

// Construct view frustum
ViewFrustum createFrustum(ivec2 tileId)
{
	mat4 invProjView = inverse(frame.projection * frame.view);

	vec2 ndcSizePerTile = 2.0 * vec2(TILE_SIZE, TILE_SIZE) / PushConstants.viewportSize;

	vec2 ndcCorners[4];
	ndcCorners[0] = ndcUpperLeft + tileId * ndcSizePerTile; // upper left
	ndcCorners[1] = vec2(ndcCorners[0].x + ndcSizePerTile.x, ndcCorners[0].y); // upper right
	ndcCorners[2] = ndcCorners[0] + ndcSizePerTile;
	ndcCorners[3] = vec2(ndcCorners[0].x, ndcCorners[0].y + ndcSizePerTile.y); // lower left

	ViewFrustum frustum;

	vec4 temp;
	for (int i = 0; i < 4; i++)
	{
		temp = invProjView * vec4(ndcCorners[i], minDepth, 1.0);
		frustum.points[i] = temp.xyz / temp.w;
		temp = invProjView * vec4(ndcCorners[i], maxDepth, 1.0);
		frustum.points[i + 4] = temp.xyz / temp.w;
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
		tempNormal = normalize(tempNormal);
		frustum.planes[4] = vec4(tempNormal, - dot(tempNormal, frustum.points[0]));
	}
	// far plane
	{
		tempNormal = cross(frustum.points[7] - frustum.points[4], frustum.points[5] - frustum.points[4]);
		tempNormal = normalize(tempNormal);
		frustum.planes[5] = vec4(tempNormal, - dot(tempNormal, frustum.points[4]));
	}

	return frustum;
}

bool Intersects(LightData light, ViewFrustum frustum)
{
	bool result = true;

    // Step1: sphere-plane test
	for (int i = 0; i < 6; i++)
	{
		if (dot(light.worldPosition, frustum.planes[i].xyz) + frustum.planes[i].w  < - light.bounds.x )
		{
			result = false;
			break;
		}
	}

    if (!result)
    {
        return false;
    }

    // Step2: bbox corner test (to reduce false positive)
    vec3 lightBboxMax = light.worldPosition + vec3(light.bounds.x);
    vec3 lightBboxMin = light.worldPosition - vec3(light.bounds.x);
    
    int probe;
    probe=0; for( int i=0; i<8; i++ ) probe += ((frustum.points[i].x > lightBboxMax.x)?1:0); if( probe==8 ) return false;
    probe=0; for( int i=0; i<8; i++ ) probe += ((frustum.points[i].x < lightBboxMin.x)?1:0); if( probe==8 ) return false;
    probe=0; for( int i=0; i<8; i++ ) probe += ((frustum.points[i].y > lightBboxMax.y)?1:0); if( probe==8 ) return false;
    probe=0; for( int i=0; i<8; i++ ) probe += ((frustum.points[i].y < lightBboxMin.y)?1:0); if( probe==8 ) return false;
    probe=0; for( int i=0; i<8; i++ ) probe += ((frustum.points[i].z > lightBboxMax.z)?1:0); if( probe==8 ) return false;
    probe=0; for( int i=0; i<8; i++ ) probe += ((frustum.points[i].z < lightBboxMin.z)?1:0); if( probe==8 ) return false;

	return true;
}

void main()
{
	ivec2 tileId = ivec2(gl_WorkGroupID.xy);
	uint tileIndex = tileId.y * PushConstants.NumTiles.x + tileId.x;

	if (gl_LocalInvocationIndex == 0)
	{
		minDepth = 0.0;
		maxDepth = 1.0;

		for (int y = 0; y < TILE_SIZE; y++)
		{
			for (int x = 0; x < TILE_SIZE; x++)
			{
				vec2 uv = (vec2(TILE_SIZE, TILE_SIZE) * tileId + vec2(x, y) ) / PushConstants.viewportSize;
				float depth = texture(sceneDepth, uv).x;
				minDepth = max(minDepth, depth);
				maxDepth = min(maxDepth, depth);
			}
		}

		if (minDepth <= maxDepth)
		{
			minDepth = maxDepth;
		}

		frustum = createFrustum(tileId);
		lightCountForTile = 0;
	}

	barrier();

	for (uint i = gl_LocalInvocationIndex; i < PushConstants.lightsNum && lightCountForTile < LIGHTS_PER_TILE; i += gl_WorkGroupSize.x)
	{
		if (Intersects(lightsData[i], frustum))
		{
			uint slot = atomicAdd(lightCountForTile, 1);
			if (slot >= LIGHTS_PER_TILE)
            {
                break;
            }
            
			culledLights[tileIndex].indices[slot] = i;
		}
	}

	barrier();

	if (gl_LocalInvocationIndex == 0)
	{
        if(lightCountForTile < LIGHTS_PER_TILE)
        {
            culledLights[tileIndex].indices[lightCountForTile] = -1;
        }
	}
}
END_CODE,

"defines":[]
}