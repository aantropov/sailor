#include "Math/Noise.h"

#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

using namespace Sailor;

namespace
{
	void Require(bool condition, const char* message)
	{
		if (!condition)
		{
			throw std::runtime_error(message);
		}
	}

	bool IsNear(float lhs, float rhs, float tolerance = 0.0001f)
	{
		return std::abs(lhs - rhs) <= tolerance;
	}

	void TestFractionAndModuloUseFloor()
	{
		const float values[] = { -1234.25f, -10.0f, -0.75f, -0.125f, 0.0f, 0.75f, 3.25f, 1024.5f };
		for (float value : values)
		{
			const float expected = value - std::floor(value);
			Require(IsNear(Math::Frac(value), expected), "scalar fraction must use floor for either sign");
			const glm::vec3 fraction = Math::Frac(glm::vec3(value));
			Require(fraction == glm::vec3(expected), "vector and scalar fraction must agree");
		}

		const glm::vec3 remainder = Math::Mod(glm::vec3(-7007.5f, -14.25f, 8.125f), 7);
		Require(remainder == glm::vec3(6.5f, 6.75f, 1.125f),
			"modulo must wrap fractions and coordinates beyond one hundred negative periods");
		Require(Math::Mod(glm::vec3(-7.0f, 0.0f, 14.0f), 7) == glm::vec3(0.0f),
			"exact period boundaries must map to zero");
	}

	void TestVoronoiMeasuresDistanceToFeaturePoints()
	{
		Require(IsNear(Math::TiledVoronoiNoise3D(glm::vec3(0.0f), 5).x, 0.0f),
			"the origin feature point must have zero distance, not vector component count");

		const glm::vec3 positions[] = {
			glm::vec3(0.125f, 0.25f, 0.5f),
			glm::vec3(-0.75f, 3.5f, 1.125f),
			glm::vec3(6.25f, -8.5f, 11.75f)
		};
		for (const glm::vec3& position : positions)
		{
			float expected = std::numeric_limits<float>::max();
			const glm::vec3 base = glm::floor(position);
			for (int x = -2; x <= 2; ++x)
			{
				for (int y = -2; y <= 2; ++y)
				{
					for (int z = -2; z <= 2; ++z)
					{
						const glm::vec3 cell = base + glm::vec3(x, y, z);
						const glm::vec3 feature = cell + Math::Rand3dTo3d(glm::mod(cell, glm::vec3(5.0f)));
						const glm::vec3 offset = feature - position;
						expected = glm::min(expected, std::sqrt(glm::dot(offset, offset)));
					}
				}
			}
			Require(IsNear(Math::TiledVoronoiNoise3D(position, 5).x, expected),
				"Voronoi distance must match the nearest generated feature point");
		}
	}

	void TestTiledNoiseRepeatsAcrossPositiveAndNegativePeriods()
	{
		const glm::vec3 positions[] = {
			glm::vec3(0.125f, 0.25f, 0.5f),
			glm::vec3(-1.75f, 3.5f, -2.125f),
			glm::vec3(4.875f, 0.0f, 2.75f)
		};
		for (const glm::vec3& position : positions)
		{
			const glm::vec3 voronoi = Math::TiledVoronoiNoise3D(position, 5);
			const float perlin = Math::TiledPerlin3D(position, 5);
			const float worley = Math::fBmTiledWorley(position, 4, 5);
			const float perlinFbm = Math::fBmTiledPerlin(position, 4, 5);
			for (int axis = 0; axis < 3; ++axis)
			{
				for (int periods : { -128, -1, 1, 128 })
				{
					glm::vec3 shifted = position;
					shifted[axis] += static_cast<float>(periods * 5);
					const glm::vec3 shiftedVoronoi = Math::TiledVoronoiNoise3D(shifted, 5);
					Require(IsNear(voronoi.x, shiftedVoronoi.x) && IsNear(voronoi.y, shiftedVoronoi.y),
						"Voronoi distance and feature identity must repeat on every axis");
					Require(IsNear(perlin, Math::TiledPerlin3D(shifted, 5)),
						"Perlin noise must repeat on either side of the origin");
					Require(IsNear(worley, Math::fBmTiledWorley(shifted, 4, 5)),
						"cloud Worley octaves must preserve the base period");
					Require(IsNear(perlinFbm, Math::fBmTiledPerlin(shifted, 4, 5)),
						"cloud Perlin octaves must preserve the base period");
				}
			}
		}
	}

	void TestCloudWorleyHasVariation()
	{
		float minimum = 1.0f;
		float maximum = 0.0f;
		for (int x = 0; x < 8; ++x)
		{
			for (int y = 0; y < 8; ++y)
			{
				for (int z = 0; z < 8; ++z)
				{
					const float value = Math::fBmTiledWorley(glm::vec3(x, y, z) * 0.625f, 4, 5);
					Require(std::isfinite(value) && value >= 0.0f && value <= 1.0f,
						"cloud Worley samples must stay finite and normalized");
					minimum = glm::min(minimum, value);
					maximum = glm::max(maximum, value);
				}
			}
		}
		Require(maximum - minimum > 0.25f, "cloud Worley must not collapse to a constant texture");
	}
}

int main()
{
	try
	{
		TestFractionAndModuloUseFloor();
		TestVoronoiMeasuresDistanceToFeaturePoints();
		TestTiledNoiseRepeatsAcrossPositiveAndNegativePeriods();
		TestCloudWorleyHasVariation();
		std::cout << "Math noise contracts passed" << std::endl;
	}
	catch (const std::exception& error)
	{
		std::cerr << error.what() << std::endl;
		return 1;
	}
	return 0;
}
