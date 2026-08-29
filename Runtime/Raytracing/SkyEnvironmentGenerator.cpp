#include "Raytracing/SkyEnvironmentGenerator.h"

#include "Math/Math.h"

#include <algorithm>
#include <cmath>
#include <glm/gtc/constants.hpp>

using namespace Sailor;
using namespace Sailor::Raytracing;

namespace
{
	constexpr float EarthRadius = 6371000.0f;
	constexpr float AtmosphereHeight = 160000.0f;
	constexpr uint32_t DensityIntegralSteps = 8u;
	constexpr uint32_t ScatteringIntegralSteps = 128u;
	constexpr float RayleighScaleHeight = 7994.0f;
	constexpr float MieScaleHeight = 1200.0f;
	constexpr float SkyLightIntensity = 7.0f;
	constexpr float IntersectionEpsilon = 0.000001f;

	float PlanetHeight(const glm::vec3& position)
	{
		const float verticalRadius = EarthRadius + position.y;
		const float horizontalDistanceSquared =
			position.x * position.x + position.z * position.z;
		const float radialDistance = std::sqrt(
			verticalRadius * verticalRadius + horizontalDistanceSquared);
		return position.y + horizontalDistanceSquared /
			(std::max)(radialDistance + verticalRadius, 1.0f);
	}

	glm::vec2 RaySphereAtAltitude(
		const glm::vec3& origin,
		const glm::vec3& direction,
		float altitude)
	{
		const float a = glm::dot(direction, direction);
		const float halfB = glm::dot(direction, origin) +
			EarthRadius * direction.y;
		const float heightDelta = origin.y - altitude;
		const float c = origin.x * origin.x + origin.z * origin.z +
			heightDelta * (2.0f * EarthRadius + origin.y + altitude);
		const float discriminant = halfB * halfB - a * c;
		if (discriminant < 0.0f)
		{
			return glm::vec2(-1.0f);
		}

		const float root = std::sqrt(discriminant);
		const float q = -halfB - (halfB >= 0.0f ? root : -root);
		if (std::abs(q) <= IntersectionEpsilon)
		{
			const float repeatedRoot = -halfB /
				(std::max)(a, IntersectionEpsilon);
			return glm::vec2(repeatedRoot);
		}

		const float firstRoot = q / a;
		const float secondRoot = c / q;
		return firstRoot < secondRoot
			? glm::vec2(firstRoot, secondRoot)
			: glm::vec2(secondRoot, firstRoot);
	}

	float NearestPositiveIntersection(const glm::vec2& intersections)
	{
		if (intersections.x > 0.0f)
		{
			return intersections.x;
		}
		return intersections.y > 0.0f ? intersections.y : -1.0f;
	}

	float RadialMotion(
		const glm::vec3& position,
		const glm::vec3& direction)
	{
		return glm::dot(
			direction,
			glm::vec3(position.x, EarthRadius + position.y, position.z));
	}

	bool RayEntersAltitudeSphere(
		const glm::vec3& origin,
		const glm::vec3& direction,
		float altitude,
		float& outDistance)
	{
		const float heightFromBoundary = PlanetHeight(origin) - altitude;
		constexpr float BoundaryTolerance = 0.001f;
		outDistance = -1.0f;

		if (heightFromBoundary < -BoundaryTolerance)
		{
			return false;
		}
		if (std::abs(heightFromBoundary) <= BoundaryTolerance)
		{
			if (RadialMotion(origin, direction) < 0.0f)
			{
				outDistance = 0.0f;
				return true;
			}
			return false;
		}

		outDistance = NearestPositiveIntersection(
			RaySphereAtAltitude(origin, direction, altitude));
		return outDistance > 0.0f;
	}

	bool ResolveAtmosphereRayOrigin(
		const glm::vec3& origin,
		const glm::vec3& direction,
		glm::vec3& outAtmosphereOrigin)
	{
		outAtmosphereOrigin = origin;
		if (PlanetHeight(origin) >= 0.0f)
		{
			return true;
		}
		if (RadialMotion(origin, direction) < 0.0f)
		{
			return false;
		}

		const glm::vec2 intersections =
			RaySphereAtAltitude(origin, direction, 0.0f);
		const float surfaceExit = (std::max)(
			intersections.x,
			intersections.y);
		if (surfaceExit < 0.0f)
		{
			return false;
		}

		outAtmosphereOrigin = origin + direction * surfaceExit;
		return true;
	}

	glm::vec3 IntersectAtmosphere(
		const glm::vec3& origin,
		const glm::vec3& direction)
	{
		const float outer = NearestPositiveIntersection(
			RaySphereAtAltitude(origin, direction, AtmosphereHeight));
		if (outer <= 0.0f)
		{
			return origin;
		}

		float shift = (std::min)(AtmosphereHeight * 10.0f, outer);
		float inner = -1.0f;
		if (RayEntersAltitudeSphere(
				origin,
				direction,
				0.0f,
				inner))
		{
			shift = (std::min)(shift, (std::max)(inner, 0.0f));
		}
		return origin + direction * shift;
	}

	float PhaseRayleigh(float cosAngle)
	{
		return (3.0f * glm::pi<float>() / 16.0f) *
			(1.0f + cosAngle * cosAngle);
	}

	float PhaseMie(float cosAngle)
	{
		const glm::vec3 c(0.256098f, 0.132268f, 0.010016f);
		const glm::vec3 d(-1.5f, -1.74f, -1.98f);
		const glm::vec3 e(1.5625f, 1.7569f, 1.9801f);
		const glm::vec3 denominator = glm::pow(
			d * cosAngle + e,
			glm::vec3(1.5f));
		return glm::dot(
			(cosAngle * cosAngle + 1.0f) * c / denominator,
			glm::vec3(1.0f / 3.0f));
	}

	glm::vec3 EvaluateClearSky(
		const glm::vec3& direction,
		const glm::vec3& lightDirection)
	{
		const glm::vec3 surfaceOrigin(0.0f);
		glm::vec3 origin;
		if (!ResolveAtmosphereRayOrigin(
				surfaceOrigin,
				direction,
				origin))
		{
			return glm::vec3(0.0f);
		}

		const glm::vec3 destination =
			IntersectAtmosphere(origin, direction);
		if (glm::length(destination - origin) < 0.01f)
		{
			return glm::vec3(0.0f);
		}

		const float angle = glm::dot(
			glm::normalize(destination - origin),
			-lightDirection);
		const glm::vec3 step = (destination - origin) /
			static_cast<float>(ScatteringIntegralSteps);
		const float stepLength = glm::length(step);
		const glm::vec3 rayleighCoefficient(
			3.8e-6f,
			13.5e-6f,
			33.1e-6f);
		const glm::vec3 mieCoefficient(22.0e-6f);

		glm::vec3 accumulatedRayleigh(0.0f);
		glm::vec3 accumulatedMie(0.0f);
		float viewDensityRayleigh = 0.0f;
		float viewDensityMie = 0.0f;
		const float phaseRayleigh = PhaseRayleigh(angle);
		const float phaseMie = PhaseMie(angle);

		for (uint32_t index = 0u;
			index < ScatteringIntegralSteps - 1u;
			++index)
		{
			const glm::vec3 point = origin + step *
				static_cast<float>(index + 1u);
			const float height = (std::max)(PlanetHeight(point), 0.0f);
			const float localRayleigh =
				std::exp(-height / RayleighScaleHeight) * stepLength;
			const float localMie =
				std::exp(-height / MieScaleHeight) * stepLength;
			viewDensityRayleigh += localRayleigh;
			viewDensityMie += localMie;

			const glm::vec3 toLight =
				IntersectAtmosphere(point, -lightDirection);
			const float lightHeight =
				(std::max)(PlanetHeight(toLight), 0.0f);
			const float lightHeightStep = (lightHeight - height) /
				static_cast<float>(DensityIntegralSteps);
			const float lightDistanceStep = glm::length(toLight - point) /
				static_cast<float>(DensityIntegralSteps);
			float lightDensityRayleigh = 0.0f;
			float lightDensityMie = 0.0f;
			float planetEntryDistance = -1.0f;
			bool bReachedSun = !RayEntersAltitudeSphere(
				point,
				-lightDirection,
				0.0f,
				planetEntryDistance);

			for (uint32_t lightIndex = 0u;
				lightIndex < DensityIntegralSteps;
				++lightIndex)
			{
				const float sampleHeight = height + lightHeightStep *
					static_cast<float>(lightIndex);
				if (sampleHeight < 0.0f)
				{
					bReachedSun = false;
					break;
				}
				lightDensityMie +=
					std::exp(-sampleHeight / MieScaleHeight) *
					lightDistanceStep;
				lightDensityRayleigh +=
					std::exp(-sampleHeight / RayleighScaleHeight) *
					lightDistanceStep;
			}

			if (bReachedSun)
			{
				const glm::vec3 attenuation = glm::exp(
					-rayleighCoefficient *
						(viewDensityRayleigh + lightDensityRayleigh) -
					mieCoefficient * 1.1f *
						(lightDensityMie + viewDensityMie));
				accumulatedRayleigh += attenuation * localRayleigh;
				accumulatedMie += attenuation * localMie;
			}
		}

		const glm::vec3 result = SkyLightIntensity *
			(rayleighCoefficient * accumulatedRayleigh * phaseRayleigh +
				mieCoefficient * accumulatedMie * phaseMie);
		return Math::AllFinite(result)
			? glm::max(result, glm::vec3(0.0f))
			: glm::vec3(0.0f);
	}
}

bool Sailor::Raytracing::GenerateSkyEnvironmentEquirectangular(
	const SkyParameters& parameters,
	const glm::uvec2& extent,
	TVector<glm::vec4>& outEnvironment,
	const SkyEnvironmentProgressCallback& progress)
{
	outEnvironment.Clear();
	if (extent.x == 0u || extent.y == 0u)
	{
		return false;
	}

	const glm::vec3 lightDirection = Math::SafeNormalize(
		glm::vec3(parameters.m_lightDirection),
		Math::vec3_Down);
	outEnvironment.Resize(
		static_cast<size_t>(extent.x) * static_cast<size_t>(extent.y));
	const float pi = glm::pi<float>();

	for (uint32_t y = 0u; y < extent.y; ++y)
	{
		if (progress && !progress(y, extent.y))
		{
			outEnvironment.Clear();
			return false;
		}

		const float theta = pi *
			(static_cast<float>(y) + 0.5f) /
			static_cast<float>(extent.y);
		const float sinTheta = std::sin(theta);
		const float cosTheta = std::cos(theta);
		for (uint32_t x = 0u; x < extent.x; ++x)
		{
			const float phi = 2.0f * pi *
				(static_cast<float>(x) + 0.5f) /
				static_cast<float>(extent.x) - pi;
			const glm::vec3 direction(
				std::cos(phi) * sinTheta,
				cosTheta,
				std::sin(phi) * sinTheta);
			outEnvironment[x + y * extent.x] = glm::vec4(
				EvaluateClearSky(direction, lightDirection),
				1.0f);
		}
	}

	if (progress && !progress(extent.y, extent.y))
	{
		outEnvironment.Clear();
		return false;
	}
	return true;
}
