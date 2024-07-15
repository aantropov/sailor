#include "Math/Noise.h"

using namespace glm;
using namespace Sailor;

float Math::Cellular2D(vec2 P)
{
	vec2 Pi = floor(P);
	vec2 Pf = P - Pi;

	//  calculate the hash
	vec4 Pt = vec4(Pi.x, Pi.y, Pi.x + 1.0, Pi.y + 1.0);
	Pt = Pt - floor(Pt * (1.0f / 71.0f)) * 71.0f;

	vec2 c = vec2(26.0, 161.0);
	Pt += vec4(c.x, c.y, c.x, c.y);

	Pt *= Pt;
	Pt = vec4(Pt.x, Pt.z, Pt.x, Pt.z) * vec4(Pt.y, Pt.y, Pt.w, Pt.w);
	vec4 hash_x = fract(Pt * (1.0f / 951.135664f));
	vec4 hash_y = fract(Pt * (1.0f / 642.949883f));

	//  generate the 4 points
	hash_x = hash_x * 2.0f - 1.0f;
	hash_y = hash_y * 2.0f - 1.0f;
	const float JITTER_WINDOW = 0.25;   // 0.25 will guarentee no artifacts
	hash_x = ((hash_x * hash_x * hash_x) - sign(hash_x)) * JITTER_WINDOW + vec4(0.0, 1.0, 0.0, 1.0);
	hash_y = ((hash_y * hash_y * hash_y) - sign(hash_y)) * JITTER_WINDOW + vec4(0.0, 0.0, 1.0, 1.0);

	//  return the closest squared distance
	vec4 dx = vec4(Pf.x, Pf.x, Pf.x, Pf.x) - hash_x;
	vec4 dy = vec4(Pf.y, Pf.y, Pf.y, Pf.y) - hash_y;
	vec4 d = dx * dx + dy * dy;
	d.x = glm::min(d.x, d.z);
	d.y = glm::min(d.y, d.w);

	return glm::min(d.x, d.y) * (1.0f / 1.125f); // return a value scaled to 0.0->1.0
}

float Math::Cellular3D(vec3 P)
{
	//	establish our grid cell and unit position
	vec3 Pi = floor(P);
	vec3 Pf = P - Pi;

	// clamp the domain
	Pi.x = Pi.x - floor(Pi.x * (1.0f / 69.0f)) * 69.0f;
	Pi.y = Pi.y - floor(Pi.y * (1.0f / 69.0f)) * 69.0f;
	Pi.z = Pi.z - floor(Pi.z * (1.0f / 69.0f)) * 69.0f;

	vec3 Pi_inc1 = step(Pi, vec3(69.0f - 1.5f)) * (Pi + 1.0f);

	// calculate the hash ( over -1.0->1.0 range )
	vec4 Pt = vec4(Pi.x, Pi.y, Pi_inc1.x, Pi_inc1.y) + vec4(50.0f, 161.0f, 50.0f, 161.0f);
	Pt *= Pt;
	Pt = vec4(Pt.x, Pt.z, Pt.x, Pt.z) * vec4(Pt.y, Pt.y, Pt.w, Pt.w);
	const vec3 SOMELARGEFLOATS = vec3(635.298681f, 682.357502f, 668.926525f);
	const vec3 ZINC = vec3(48.500388, 65.294118, 63.934599);
	vec3 lowz_mod = vec3(1.0f / (SOMELARGEFLOATS + vec3(Pi.z, Pi.z, Pi.z) * ZINC));
	vec3 highz_mod = vec3(1.0f / (SOMELARGEFLOATS + vec3(Pi_inc1.z, Pi_inc1.z, Pi_inc1.z) * ZINC));
	vec4 hash_x0 = fract(Pt * lowz_mod.x) * 2.0f - 1.0f;
	vec4 hash_x1 = fract(Pt * highz_mod.x) * 2.0f - 1.0f;
	vec4 hash_y0 = fract(Pt * lowz_mod.y) * 2.0f - 1.0f;
	vec4 hash_y1 = fract(Pt * highz_mod.y) * 2.0f - 1.0f;
	vec4 hash_z0 = fract(Pt * lowz_mod.z) * 2.0f - 1.0f;
	vec4 hash_z1 = fract(Pt * highz_mod.z) * 2.0f - 1.0f;

	//  generate the 8 point positions
	const float JITTER_WINDOW = 0.166666666f;	// 0.166666666 will guarentee no artifacts.
	hash_x0 = ((hash_x0 * hash_x0 * hash_x0) - sign(hash_x0)) * JITTER_WINDOW + vec4(0.0f, 1.0f, 0.0f, 1.0f);
	hash_y0 = ((hash_y0 * hash_y0 * hash_y0) - sign(hash_y0)) * JITTER_WINDOW + vec4(0.0f, 0.0f, 1.0f, 1.0f);
	hash_x1 = ((hash_x1 * hash_x1 * hash_x1) - sign(hash_x1)) * JITTER_WINDOW + vec4(0.0f, 1.0f, 0.0f, 1.0f);
	hash_y1 = ((hash_y1 * hash_y1 * hash_y1) - sign(hash_y1)) * JITTER_WINDOW + vec4(0.0f, 0.0f, 1.0f, 1.0f);
	hash_z0 = ((hash_z0 * hash_z0 * hash_z0) - sign(hash_z0)) * JITTER_WINDOW + vec4(0.0f, 0.0f, 0.0f, 0.0f);
	hash_z1 = ((hash_z1 * hash_z1 * hash_z1) - sign(hash_z1)) * JITTER_WINDOW + vec4(1.0f, 1.0f, 1.0f, 1.0f);

	//	return the closest squared distance
	vec4 dx1 = vec4(Pf.x, Pf.x, Pf.x, Pf.x) - hash_x0;
	vec4 dy1 = vec4(Pf.y, Pf.y, Pf.y, Pf.y) - hash_y0;
	vec4 dz1 = vec4(Pf.z, Pf.z, Pf.z, Pf.z) - hash_z0;
	vec4 dx2 = vec4(Pf.x, Pf.x, Pf.x, Pf.x) - hash_x1;
	vec4 dy2 = vec4(Pf.y, Pf.y, Pf.y, Pf.y) - hash_y1;
	vec4 dz2 = vec4(Pf.z, Pf.z, Pf.z, Pf.z) - hash_z1;
	vec4 d1 = dx1 * dx1 + dy1 * dy1 + dz1 * dz1;
	vec4 d2 = dx2 * dx2 + dy2 * dy2 + dz2 * dz2;
	d1 = min(d1, d2);

	d1.x = min(d1.x, d1.w);
	d1.y = min(d1.y, d1.z);

	return min(d1.x, d1.y) * (9.0f / 12.0f); // return a value scaled to 0.0->1.0
}

float Math::Perlin2D(vec2 P)
{
	//  https://github.com/BrianSharpe/Wombat/blob/master/Perlin2D.glsl

	// establish our grid cell and unit position
	vec2 Pi = floor(P);
	vec4 Pf_Pfmin1 = vec4(P.x, P.y, P.x, P.y) - vec4(Pi.x, Pi.y, Pi.x + 1.0, Pi.y + 1.0);

	// calculate the hash
	vec4 Pt = vec4(Pi.x, Pi.y, Pi.x + 1.0f, Pi.y + 1.0f);
	Pt = Pt - floor(Pt * (1.0f / 71.0f)) * 71.0f;

	vec2 c = vec2(26.0, 161.0);
	Pt += vec4(c.x, c.y, c.x, c.y);
	Pt *= Pt;
	Pt = vec4(Pt.x, Pt.z, Pt.x, Pt.z) * vec4(Pt.y, Pt.y, Pt.w, Pt.w);
	vec4 hash_x = fract(Pt * (1.0f / 951.135664f));
	vec4 hash_y = fract(Pt * (1.0f / 642.949883f));

	// calculate the gradient results
	vec4 grad_x = hash_x - 0.49999f;
	vec4 grad_y = hash_y - 0.49999f;
	vec4 grad_results = inversesqrt(grad_x * grad_x + grad_y * grad_y) * (grad_x * vec4(Pf_Pfmin1.x, Pf_Pfmin1.z, Pf_Pfmin1.x, Pf_Pfmin1.z) + grad_y * vec4(Pf_Pfmin1.y, Pf_Pfmin1.y, Pf_Pfmin1.w, Pf_Pfmin1.w));

	// Classic Perlin Interpolation
	grad_results *= 1.4142135623730950488016887242097f;  // scale things to a strict -1.0->1.0 range  *= 1.0/sqrt(0.5)
	vec2 blend = vec2(Pf_Pfmin1.x, Pf_Pfmin1.y) * vec2(Pf_Pfmin1.x, Pf_Pfmin1.y) * vec2(Pf_Pfmin1.x, Pf_Pfmin1.y) *
		(vec2(Pf_Pfmin1.x, Pf_Pfmin1.y) * (vec2(Pf_Pfmin1.x, Pf_Pfmin1.y) * 6.0f - 15.0f) + 10.0f);
	vec4 blend2 = vec4(blend, vec2(1.0f - blend));
	return glm::dot(grad_results, vec4(blend2.z, blend2.x, blend2.z, blend2.x) * vec4(blend2.w, blend2.w, blend2.y, blend2.y));
}

float Math::Rand3dTo1d(vec3 value, vec3 dotDir)
{
	vec3 smallValue(sin(value.x), sin(value.y), sin(value.z));
	float random = glm::dot(smallValue, dotDir);
	random = Frac(sin(random) * 1023465);
	return random;
}

vec3 Math::Rand3dTo3d(vec3 value)
{
	return vec3(
		Rand3dTo1d(value, vec3(12.989, 78.233, 37.719)),
		Rand3dTo1d(value, vec3(39.346, 11.135, 83.155)),
		Rand3dTo1d(value, vec3(73.156, 52.235, 09.151))
	);
}

vec3 Math::Mod(vec3 divident, int divisor)
{
	int x = ((int)divident.x + divisor * 100) % (divisor);
	int y = ((int)divident.y + divisor * 100) % (divisor);
	int z = ((int)divident.z + divisor * 100) % (divisor);

	return vec3(x, y, z);
}

float Math::Frac(float num)
{
	float f = (num - (int)(num));
	if (f < 0) f *= -1;

	return f;
}

vec3 Math::Frac(vec3 v)
{
	return vec3((v.x - (int)(v.x)), (v.y - (int)(v.y)), (v.z - (int)(v.z)));
}

float Math::EaseInOut(float interpolator)
{
	float easeInValue = EaseIn(interpolator);
	float easeOutValue = EaseOut(interpolator);
	return glm::mix(easeInValue, easeOutValue, interpolator);
}

float Math::EaseIn(float interpolator)
{
	return interpolator * interpolator;
}

float Math::EaseOut(float interpolator)
{
	return 1 - EaseIn(1 - interpolator);
}

float Math::TiledPerlin3D(vec3 value, uint32_t period)
{
	vec3 fraction = Frac(value);

	float interpolatorX = EaseInOut(fraction.x);
	float interpolatorY = EaseInOut(fraction.y);
	float interpolatorZ = EaseInOut(fraction.z);

	float cellNoiseZ[2];
	//[unroll]
	for (int z = 0; z <= 1; z++) {
		float cellNoiseY[2];
		//[unroll]
		for (int y = 0; y <= 1; y++) {
			float cellNoiseX[2];
			//[unroll]
			for (int x = 0; x <= 1; x++) {
				vec3 cell = Mod(floor(value) + vec3(x, y, z), period);// % period;
				vec3 cellDirection = Rand3dTo3d(cell) * 2.0f - 1.0f;
				vec3 compareVector = fraction - vec3(x, y, z);
				cellNoiseX[x] = glm::dot(cellDirection, compareVector);
			}
			cellNoiseY[y] = mix(cellNoiseX[0], cellNoiseX[1], interpolatorX);
		}
		cellNoiseZ[z] = mix(cellNoiseY[0], cellNoiseY[1], interpolatorY);
	}
	float noise = mix(cellNoiseZ[0], cellNoiseZ[1], interpolatorZ);
	return noise;
}

vec3 Math::TiledVoronoiNoise3D(vec3 value, uint32_t period)
{
	vec3 baseCell = floor(value);

	// First pass to find the closest cell
	float minDistToCell = 10;
	vec3 toClosestCell;
	vec3 closestCell;

	for (int x1 = -1; x1 <= 1; x1++) {

		for (int y1 = -1; y1 <= 1; y1++) {

			for (int z1 = -1; z1 <= 1; z1++)
			{
				vec3 cell = baseCell + vec3(x1, y1, z1);
				vec3 tiledCell = Mod(cell, period);
				vec3 cellPosition = cell + Rand3dTo3d(tiledCell);
				vec3 toCell = cellPosition - value;
				float distToCell = (float)toCell.length();
				if (distToCell < minDistToCell)
				{
					minDistToCell = distToCell;
					closestCell = cell;
					toClosestCell = toCell;
				}
			}
		}
	}

	// Second pass to find the distance to the closest edge
	float minEdgeDistance = 10;

	/*
	for (int x2 = -1; x2 <= 1; x2++) {

		for (int y2 = -1; y2 <= 1; y2++) {

			for (int z2 = -1; z2 <= 1; z2++) {
				vec3 cell = baseCell + vec3(x2, y2, z2);
				vec3 tiledCell = Mod(cell, period);
				vec3 cellPosition = cell + Rand3dTo3d(tiledCell);
				vec3 toCell = cellPosition - value;

				vec3 diffToClosestCell = abs(closestCell - cell);
				bool isClosestCell = diffToClosestCell.x + diffToClosestCell.y + diffToClosestCell.z < 0.1;
				if (!isClosestCell) {
					vec3 toCenter = (toClosestCell + toCell) * 0.5f;
					vec3 cellDifference = (toCell - toClosestCell);
					cellDifference = normalize(cellDifference);
					float edgeDistance = glm::dot(toCenter, cellDifference);
					minEdgeDistance = min(minEdgeDistance, edgeDistance);
				}
			}
		}
	}*/

	float random = Rand3dTo1d(closestCell);
	return vec3(minDistToCell, random, minEdgeDistance);

}

float Math::Perlin3D(vec3 P)
{
	//  https://github.com/BrianSharpe/Wombat/blob/master/Perlin3D.glsl

	// establish our grid cell and unit position
	vec3 Pi = floor(P);
	vec3 Pf = P - Pi;
	vec3 Pf_min1 = Pf - 1.0f;

	// clamp the domain
	Pi.x = Pi.x - floor(Pi.x * (1.0f / 69.0f)) * 69.0f;
	Pi.y = Pi.y - floor(Pi.y * (1.0f / 69.0f)) * 69.0f;
	Pi.z = Pi.z - floor(Pi.z * (1.0f / 69.0f)) * 69.0f;

	vec3 Pi_inc1 = step(Pi, vec3(69.0f - 1.5f)) * (Pi + 1.0f);

	// calculate the hash
	vec4 Pt = vec4(Pi.x, Pi.y, Pi_inc1.x, Pi_inc1.y) + vec4(50.0f, 161.0f, 50.0f, 161.0f);
	Pt *= Pt;
	Pt = vec4(Pt.x, Pt.z, Pt.x, Pt.z) * vec4(Pt.y, Pt.y, Pt.w, Pt.w);

	const vec3 SOMELARGEFLOATS = vec3(635.298681, 682.357502, 668.926525);
	const vec3 ZINC = vec3(48.500388, 65.294118, 63.934599);
	vec3 lowz_mod = vec3(1.0f / (SOMELARGEFLOATS + vec3(Pi.z, Pi.z, Pi.z) * ZINC));
	vec3 highz_mod = vec3(1.0f / (SOMELARGEFLOATS + vec3(Pi_inc1.z, Pi_inc1.z, Pi_inc1.z) * ZINC));
	vec4 hashx0 = fract(Pt * lowz_mod.x);
	vec4 hashx1 = fract(Pt * highz_mod.x);
	vec4 hashy0 = fract(Pt * lowz_mod.y);
	vec4 hashy1 = fract(Pt * highz_mod.y);
	vec4 hashz0 = fract(Pt * lowz_mod.z);
	vec4 hashz1 = fract(Pt * highz_mod.z);

	// calculate the gradients
	vec4 grad_x0 = hashx0 - 0.49999f;
	vec4 grad_y0 = hashy0 - 0.49999f;
	vec4 grad_z0 = hashz0 - 0.49999f;
	vec4 grad_x1 = hashx1 - 0.49999f;
	vec4 grad_y1 = hashy1 - 0.49999f;
	vec4 grad_z1 = hashz1 - 0.49999f;
	vec4 grad_results_0 = inversesqrt(grad_x0 * grad_x0 + grad_y0 * grad_y0 + grad_z0 * grad_z0) *
		(vec4(Pf.x, Pf_min1.x, Pf.x, Pf_min1.x) * grad_x0 + vec4(Pf.y, Pf.y, Pf_min1.y, Pf_min1.y) * grad_y0 + vec4(Pf.z, Pf.z, Pf.z, Pf.z) * grad_z0);

	vec4 grad_results_1 = inversesqrt(grad_x1 * grad_x1 + grad_y1 * grad_y1 + grad_z1 * grad_z1) *
		(vec4(Pf.x, Pf_min1.x, Pf.x, Pf_min1.x) * grad_x1 + vec4(Pf.y, Pf.y, Pf_min1.y, Pf_min1.y) * grad_y1 + vec4(Pf_min1.z, Pf_min1.z, Pf_min1.z, Pf_min1.z) * grad_z1);

	// Classic Perlin Interpolation
	vec3 blend = Pf * Pf * Pf * (Pf * (Pf * 6.0f - 15.0f) + 10.0f);
	vec4 res0 = mix(grad_results_0, grad_results_1, blend.z);
	vec4 blend2 = vec4(blend.x, blend.y, vec2(1.0f - blend.x, 1.0f - blend.y));
	float final = glm::dot(res0, vec4(blend2.z, blend2.x, blend2.z, blend2.x) * vec4(blend2.w, blend2.w, blend2.y, blend2.y));
	return (final * 1.1547005383792515290182975610039f);  // scale things to a strict -1.0->1.0 range  *= 1.0/sqrt(0.75)
}

float Math::fBmPerlin(vec3 st, uint32_t octaves)
{
	float value = 0.0;
	float amplitude = .5;

	for (uint32_t i = 0; i < octaves; i++)
	{
		value += amplitude * Perlin3D(st);
		st *= 2.;
		amplitude *= .5;
	}

	return value;
}

float Math::fBmVoronoi(vec3 st, uint32_t octaves)
{
	float value = 0.0;
	float amplitude = .5;

	for (uint32_t i = 0; i < octaves; i++)
	{
		value += amplitude * Cellular3D(st);
		st *= 2.;
		amplitude *= .5;
	}

	return value;
}

vec3 Math::fBmTiledVoronoi(vec3 value, int octaves, int frequency, float gain, float lacunarity, float amplitude)
{
	vec3 result = {};

	for (int i = 0; i < octaves; i++)
	{
		result += TiledVoronoiNoise3D(value, frequency) * amplitude;
		frequency *= (uint32_t)glm::round(lacunarity);
		value = value * lacunarity;
		amplitude *= gain;
	}

	return result;
}

float Math::fBmTiledWorley(vec3 value, int octaves, int frequency, float gain, float lacunarity, float amplitude)
{
	return glm::clamp((1.0f - Sailor::Math::fBmTiledVoronoi(value, octaves, frequency, gain, lacunarity, amplitude).x) * 1.5f - 0.25f, 0.0f, 1.0f);
}

float Math::fBmTiledPerlin(vec3 value, int octaves, int frequency, float gain, float lacunarity, float amplitude)
{
	float result = {};

	for (int i = 0; i < octaves; i++)
	{
		result += TiledPerlin3D(value, frequency) * amplitude;
		frequency *= (uint32_t)glm::round(lacunarity);
		value = value * lacunarity;
		amplitude *= gain;
	}

	return result;
}