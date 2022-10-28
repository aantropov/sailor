#define CANDIDATES_PER_TILE 256
#define LIGHTS_PER_TILE 128

const int TILE_SIZE = 16;

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