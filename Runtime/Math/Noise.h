#pragma once

#include "Core/Defines.h"
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/common.hpp>
#include <glm/gtc/matrix_transform.hpp>

using namespace glm;

namespace Sailor::Math
{	
	SAILOR_API float Cellular2D(vec2 P);
	SAILOR_API float Perlin2D(vec2 P);
	
	SAILOR_API float Cellular3D(vec3 P);
	SAILOR_API float Perlin3D(vec3 P);

	// Tiled noise requires a positive period. Voronoi returns distance, cell value,
	// and an unused edge-distance channel retained at 10.
	SAILOR_API float TiledPerlin3D(vec3 P, uint32_t period);
	SAILOR_API vec3 TiledVoronoiNoise3D(vec3 P, uint32_t period);

	SAILOR_API vec3 fBmTiledVoronoi(vec3 value, int octaves, int frequency, float gain = 0.5f, float lacunarity = 2.0f, float amplitude = 1.0f);
	SAILOR_API float fBmTiledPerlin(vec3 value, int octaves, int frequency, float gain = 0.5f, float lacunarity = 2.0f, float amplitude = 1.0f);
	SAILOR_API float fBmTiledWorley(vec3 value, int octaves, int frequency, float gain = 0.5f, float lacunarity = 2.0f, float amplitude = 1.0f);

	SAILOR_API float fBmPerlin(vec3 st, uint32_t octaves);
	SAILOR_API float fBmVoronoi(vec3 st, uint32_t octaves);

	SAILOR_API vec3 Frac(vec3 v);
	SAILOR_API float Frac(float num);

	SAILOR_API float EaseInOut(float interpolator);
	SAILOR_API float EaseIn(float interpolator);
	SAILOR_API float EaseOut(float interpolator);
	// Floor-based remainder in [0, divisor), including negative coordinates.
	SAILOR_API vec3 Mod(vec3 dividend, int divisor);

	SAILOR_API vec3 Rand3dTo3d(vec3 value);
	SAILOR_API float Rand3dTo1d(vec3 value, vec3 dotDir = vec3(12.9898, 78.233, 37.719));
}
