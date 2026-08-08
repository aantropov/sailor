#include "Components/BuoyancyComponent.h"
#include "Math/Math.h"
#include <algorithm>
#include <cmath>

using namespace Sailor;

namespace
{
	float SanitizeNonNegative(float value, float fallback)
	{
		return std::isfinite(value) ? std::max(0.0f, value) : fallback;
	}
}

void BuoyancyComponent::SetWaterHeight(float value)
{
	if (std::isfinite(value))
	{
		m_waterHeight = value;
	}
}

void BuoyancyComponent::SetHalfExtents(const glm::vec2& value)
{
	if (Math::AllFinite(value))
	{
		m_halfExtents = glm::max(glm::abs(value), glm::vec2(0.01f));
	}
}

void BuoyancyComponent::SetFloatationPlane(float value)
{
	if (std::isfinite(value))
	{
		m_floatationPlane = value;
	}
}

void BuoyancyComponent::SetEquilibriumDepth(float value)
{
	m_equilibriumDepth = std::max(0.01f,
		SanitizeNonNegative(value, m_equilibriumDepth));
}

void BuoyancyComponent::SetBuoyancyScale(float value)
{
	m_buoyancyScale = SanitizeNonNegative(value, m_buoyancyScale);
}

void BuoyancyComponent::SetVerticalDamping(float value)
{
	m_verticalDamping = SanitizeNonNegative(value, m_verticalDamping);
}

void BuoyancyComponent::SetWaterDrag(float value)
{
	m_waterDrag = SanitizeNonNegative(value, m_waterDrag);
}

void BuoyancyComponent::SetWaveAmplitude(float value)
{
	m_waveAmplitude = SanitizeNonNegative(value, m_waveAmplitude);
}

void BuoyancyComponent::SetWaveLength(float value)
{
	m_waveLength = std::max(0.01f,
		SanitizeNonNegative(value, m_waveLength));
}

void BuoyancyComponent::SetWaveSpeed(float value)
{
	m_waveSpeed = SanitizeNonNegative(value, m_waveSpeed);
}
