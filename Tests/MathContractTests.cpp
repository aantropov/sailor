#include "Math/Noise.h"
#include "Math/Math.h"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <utility>

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

	void TestColorConversions()
	{
		const glm::vec3 linear(0.0f, 0.21404114f, 1.0f);
		const glm::vec3 srgb(0.0f, 0.5f, 1.0f);
		const auto encoded = Utils::LinearToSRGB(linear);
		const auto decoded = Utils::SRGBToLinear(srgb);
		for (int channel = 0; channel < 3; ++channel)
		{
			Require(IsNear(encoded[channel], srgb[channel]) && IsNear(decoded[channel], linear[channel]),
				"color conversions must retain the standard normalized endpoints and midtone");
		}
		Require(IsNear(Utils::LinearToSRGB(glm::vec3(0.001f)).x, 0.01292f) &&
			IsNear(Utils::SRGBToLinear(glm::vec3(0.01292f)).x, 0.001f),
			"dark color values must use the linear transfer segment");
		const glm::vec4 hdr(0.01f, 0.25f, 4.0f, 0.37f);
		const auto roundTrip = Utils::SRGBToLinear(Utils::LinearToSRGB(hdr));
		for (int channel = 0; channel < 4; ++channel)
		{
			Require(IsNear(roundTrip[channel], hdr[channel]), "float color conversion must retain HDR values and alpha");
		}
		const auto bytes = Utils::LinearToSRGB8(glm::vec4(0.0f, 0.5f, 2.0f, 0.5f));
		Require(bytes == glm::u8vec4(0, 187, 255, 128),
			"8-bit output must clamp RGB and round normalized alpha independently");
	}

	void TestAstronomicalCoordinates()
	{
		Require(Utils::CalculateJulianDayNumber(2000, 1, 1) == 2451545 &&
			Utils::CalculateJulianDate(2000, 1, 1, 12, 0, 0) == 2451545.0 &&
			Utils::CalculateJulianDate(2000, 1, 1, 0, 0, 0) == 2451544.5,
			"Julian dates must retain the noon epoch and fractional day");
		Require(Utils::CalculateJulianCenturyDate(2000, 1, 1, 12, 0, 0) == 0.0 &&
			Utils::CalculateJulianDayNumber(2000, 3, 1) - Utils::CalculateJulianDayNumber(2000, 2, 28) == 2,
			"Julian centuries and leap-day arithmetic must retain their existing epoch");
		const float halfPi = glm::radians(90.0f);
		for (const auto& [angles, expected] : {
			std::pair{ glm::vec2(0.0f), glm::vec3(0.0f, 2.0f, 0.0f) },
			std::pair{ glm::vec2(halfPi, 0.0f), glm::vec3(2.0f, 0.0f, 0.0f) },
			std::pair{ glm::vec2(0.0f, halfPi), glm::vec3(0.0f, 0.0f, 2.0f) } })
		{
			const auto position = Utils::ConvertToEuclidean(angles.x, angles.y, 2.0f);
			Require(glm::length(position - expected) < 0.0001f,
				"star coordinates must retain axis orientation and radial distance");
		}
	}

	void TestPackedRandomColor()
	{
		std::srand(7);
		const auto red = static_cast<uint32_t>(std::rand() % 255);
		const auto green = static_cast<uint32_t>(std::rand() % 255);
		const auto blue = static_cast<uint32_t>(std::rand() % 255);
		std::srand(7);
		Require(Utils::GetRandomColorHex() == (red | (green << 8) | (blue << 16)),
			"packed colors must retain red in the low byte and no alpha bits");
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
		TestColorConversions();
		TestAstronomicalCoordinates();
		TestPackedRandomColor();
		TestFractionAndModuloUseFloor();
		TestVoronoiMeasuresDistanceToFeaturePoints();
		TestTiledNoiseRepeatsAcrossPositiveAndNegativePeriods();
		TestCloudWorleyHasVariation();
		std::cout << "Math contracts passed" << std::endl;
	}
	catch (const std::exception& error)
	{
		std::cerr << error.what() << std::endl;
		return 1;
	}
	return 0;
}
