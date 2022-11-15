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
	vec2 cutOff;
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

vec3 GaussianBlur(sampler2D textureSampler, vec2 uv, vec2 texelSize, uint radius)
{
	const int stepCount = 12;
	
	const float weights[stepCount][stepCount] = {
		{ 0.5, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
		{ 0.281088, 0.218912, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
		{ 0.197159, 0.176426, 0.126415, 0, 0, 0, 0, 0, 0, 0, 0, 0},
		{ 0.152068, 0.142855, 0.118431, 0.0866459, 0, 0, 0, 0, 0, 0, 0, 0},
		{ 0.123827, 0.118971, 0.105518, 0.0863909, 0.0652929, 0, 0, 0, 0, 0, 0, 0},
		{ 0.104454, 0.101593, 0.0934699, 0.0813492, 0.0669741, 0.0521595, 0, 0, 0, 0, 0, 0},
		{ 0.0903332, 0.0885083, 0.083252, 0.0751759, 0.0651684, 0.0542336, 0.0433285, 0, 0, 0, 0, 0},
		{ 0.07958, 0.0783462, 0.0747585, 0.0691403, 0.061977, 0.0538465, 0.0453433, 0.0370081, 0, 0, 0, 0},
		{ 0.0711171, 0.0702445, 0.0676904, 0.0636383, 0.0583697, 0.0522315, 0.0455989, 0.0388376, 0.0322721, 0, 0, 0},
		{ 0.0642825, 0.0636429, 0.0617619, 0.0587498, 0.0547779, 0.0500633, 0.0448484, 0.0393811, 0.0338957, 0.0285966, 0, 0},
		{ 0.0586472, 0.0581645, 0.0567402, 0.0544433, 0.0513831, 0.0476999, 0.0435548, 0.039118, 0.0345572, 0.0300277, 0.0256641, 0},
		{ 0.0539209, 0.0535478, 0.0524437, 0.050654, 0.0482506, 0.0453272, 0.0419936, 0.0383686, 0.034573, 0.0307232, 0.0269255, 0.0232718}};

    const uint blurRadius = min(radius, stepCount);
	
	vec3 pixelSum = vec3(0.0f);
	
    for(int i = 0; i < blurRadius; i++)
    {  
        vec2 texCoordOffset = i * texelSize;
        vec3 color = texture(textureSampler, uv + texCoordOffset).xyz + texture(textureSampler, uv - texCoordOffset).xyz;
		pixelSum += color * weights[blurRadius-1][i];
    }

    return pixelSum;
}

float LuminanceCzm(vec3 rgb)
{
    // Algorithm from Chapter 10 of Graphics Shaders.
    const vec3 w = vec3(0.2125, 0.7154, 0.0721);
    return dot(rgb, w);
}