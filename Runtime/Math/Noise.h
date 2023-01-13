#pragma once

#include <glm/glm/glm.hpp>
#include <glm/glm/gtc/quaternion.hpp>
#include <glm/glm/common.hpp>
#include <glm/glm/gtc/matrix_transform.hpp>

using namespace glm;

namespace Sailor::Math
{	
	float Cellular2D(vec2 P);
	float Perlin2D(vec2 P);
	
	float Cellular3D(vec3 P);
	float Perlin3D(vec3 P);

	float TiledPerlin3D(vec3 P, uint32_t period);
	vec3 TiledVoronoiNoise3D(vec3 P, uint32_t period);

	vec3  fBmTiledVoronoi(vec3 value, int octaves, int frequency, float gain = 0.5f, float lacunarity = 2.0f, float amplitude = 1.0f);
	float fBmTiledPerlin(vec3 value, int octaves, int frequency, float gain = 0.5f, float lacunarity = 2.0f, float amplitude = 1.0f);
	float fBmTiledWorley(vec3 value, int octaves, int frequency, float gain = 0.5f, float lacunarity = 2.0f, float amplitude = 1.0f);

	float fBmPerlin(vec3 st, uint32_t octaves);
	float fBmVoronoi(vec3 st, uint32_t octaves);

	vec3 Frac(vec3 v);
	float Frac(float num);

	float EaseInOut(float interpolator);
	float EaseIn(float interpolator);
	float EaseOut(float interpolator);
	vec3 Mod(vec3 divident, int divisor);

	vec3 Rand3dTo3d(vec3 value);
	float Rand3dTo1d(vec3 value, vec3 dotDir = vec3(12.9898, 78.233, 37.719));
}

