#define LIGHTS_CANDIDATES_PER_TILE 196
#define LIGHTS_PER_TILE 128

const int LIGHTS_CULLING_TILE_SIZE = 16;

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
struct LightsGrid
{
	uint offset;
	uint num;
}; 

// Vulkan NDC, Reverse Z: minDepth = 1.0, maxDepth = 0.0
const vec2 ndcUpperLeft = vec2(-1.0, -1.0);
const float ndcNearPlane = 0.0;
const float ndcFarPlane = 1.0;

struct ViewFrustum
{
	vec4 planes[4];	
	vec2 center;
};

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

// Convert clip space coordinates to view space
vec4 ClipToView(vec4 clip, mat4 invProjection)
{
    // View space position
    vec4 view = invProjection * clip;
    // Perspective projection
    view = view/view.w;

	// Reverse Z
	view.z *= -1;
	
    return view;
}

// Convert screen space coordinates to view space.
vec4 ScreenToView(vec4 screen, vec2 viewportSize, mat4 invProjection)
{
    // Convert to normalized texture coordinates
    vec2 texCoord = screen.xy / viewportSize;

    // Convert to clip space
    vec4 clip = vec4(vec2(texCoord.x, texCoord.y) * 2.0f - 1.0f, screen.z, screen.w);

    return ClipToView(clip, invProjection);
}