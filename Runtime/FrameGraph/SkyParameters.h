#pragma once
#include "Containers/Hash.h"
#include "Core/Defines.h"
#include "Math/Math.h"
#include <cstdint>
#include <type_traits>

namespace Sailor
{
	struct SkyEnvironmentKey
	{
		glm::ivec3 m_lightDirection{};
		glm::vec3 m_sunIlluminance{};
		bool m_bUsesLightDirection{};

		bool operator==(const SkyEnvironmentKey& rhs) const
		{
			return m_sunIlluminance == rhs.m_sunIlluminance &&
				m_bUsesLightDirection == rhs.m_bUsesLightDirection &&
				(!m_bUsesLightDirection || m_lightDirection == rhs.m_lightDirection);
		}

		size_t GetHash() const
		{
			size_t hash = 0;
			HashCombine(hash,
				m_sunIlluminance.x,
				m_sunIlluminance.y,
				m_sunIlluminance.z);
			HashCombine(hash, m_bUsesLightDirection);
			if (m_bUsesLightDirection)
			{
				HashCombine(hash, m_lightDirection.x, m_lightDirection.y, m_lightDirection.z);
			}

			return hash;
		}
	};

	struct SkyParameters
	{
		glm::vec4 m_lightDirection = Math::SafeNormalize(
			glm::vec4(0.0f, -1.0f, 1.0f, 0.0f),
			glm::vec4(0.0f, -1.0f, 0.0f, 0.0f));
		// RGB illuminance from the sun on a surface normal to its rays, in lux.
		glm::vec4 m_sunIlluminance = glm::vec4(120000.0f, 120000.0f, 120000.0f, 0.0f);
		float m_cloudsAttenuation1 = 0.3f;
		float m_cloudsAttenuation2 = 0.06f;
		float m_cloudsDensity = 0.3f;
		float m_cloudsCoverage = 0.56f;
		float m_phaseInfluence1 = 0.025f;
		float m_phaseInfluence2 = 0.9f;
		float m_eccentrisy1 = 0.95f;
		float m_eccentrisy2 = 0.51f;
		float m_fog = 10.0f;
		float m_cloudScatteringScale = 1.0f;
		float m_ambient = 0.5f;
		int32_t m_scatteringSteps = 5;
		float m_scatteringDensity = 0.5f;
		float m_scatteringIntensity = 0.5f;
		float m_scatteringPhase = 0.5f;
		float m_sunShaftsIntensity = 0.45f;
		int32_t m_sunShaftsDistance = 60;

		bool operator==(const SkyParameters& rhs) const
		{
			return m_lightDirection == rhs.m_lightDirection &&
				m_sunIlluminance == rhs.m_sunIlluminance &&
				m_cloudsAttenuation1 == rhs.m_cloudsAttenuation1 &&
				m_cloudsAttenuation2 == rhs.m_cloudsAttenuation2 &&
				m_cloudsDensity == rhs.m_cloudsDensity &&
				m_cloudsCoverage == rhs.m_cloudsCoverage &&
				m_phaseInfluence1 == rhs.m_phaseInfluence1 &&
				m_phaseInfluence2 == rhs.m_phaseInfluence2 &&
				m_eccentrisy1 == rhs.m_eccentrisy1 &&
				m_eccentrisy2 == rhs.m_eccentrisy2 &&
				m_fog == rhs.m_fog &&
				m_cloudScatteringScale == rhs.m_cloudScatteringScale &&
				m_ambient == rhs.m_ambient &&
				m_scatteringSteps == rhs.m_scatteringSteps &&
				m_scatteringDensity == rhs.m_scatteringDensity &&
				m_scatteringIntensity == rhs.m_scatteringIntensity &&
				m_scatteringPhase == rhs.m_scatteringPhase &&
				m_sunShaftsIntensity == rhs.m_sunShaftsIntensity &&
				m_sunShaftsDistance == rhs.m_sunShaftsDistance;
		}

		SkyEnvironmentKey GetEnvironmentKey() const
		{
			SkyEnvironmentKey key;
			key.m_sunIlluminance = glm::vec3(m_sunIlluminance);
			key.m_bUsesLightDirection =
				glm::dot(Math::vec4_Down, m_lightDirection) > -0.85f;
			if (key.m_bUsesLightDirection)
			{
				key.m_lightDirection =
					glm::ivec3(m_lightDirection * 10.0f);
			}

			return key;
		}
	};

	static_assert(std::is_standard_layout_v<SkyParameters>);
	static_assert(sizeof(SkyParameters) == 100);
}
