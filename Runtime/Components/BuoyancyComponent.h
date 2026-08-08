#pragma once
#include "Components/Component.h"

namespace Sailor
{
	class BuoyancyComponent final : public Component
	{
		SAILOR_REFLECTABLE(BuoyancyComponent)

	public:
		SAILOR_API float GetWaterHeight() const { return m_waterHeight; }
		SAILOR_API void SetWaterHeight(float value);
		SAILOR_API const glm::vec2& GetHalfExtents() const { return m_halfExtents; }
		SAILOR_API void SetHalfExtents(const glm::vec2& value);
		SAILOR_API float GetFloatationPlane() const { return m_floatationPlane; }
		SAILOR_API void SetFloatationPlane(float value);
		SAILOR_API float GetEquilibriumDepth() const { return m_equilibriumDepth; }
		SAILOR_API void SetEquilibriumDepth(float value);
		SAILOR_API float GetBuoyancyScale() const { return m_buoyancyScale; }
		SAILOR_API void SetBuoyancyScale(float value);
		SAILOR_API float GetVerticalDamping() const { return m_verticalDamping; }
		SAILOR_API void SetVerticalDamping(float value);
		SAILOR_API float GetWaterDrag() const { return m_waterDrag; }
		SAILOR_API void SetWaterDrag(float value);
		SAILOR_API float GetWaveAmplitude() const { return m_waveAmplitude; }
		SAILOR_API void SetWaveAmplitude(float value);
		SAILOR_API float GetWaveLength() const { return m_waveLength; }
		SAILOR_API void SetWaveLength(float value);
		SAILOR_API float GetWaveSpeed() const { return m_waveSpeed; }
		SAILOR_API void SetWaveSpeed(float value);

	private:
		float m_waterHeight = 0.0f;
		glm::vec2 m_halfExtents = glm::vec2(0.65f, 1.45f);
		float m_floatationPlane = 0.25f;
		float m_equilibriumDepth = 0.28f;
		float m_buoyancyScale = 1.05f;
		float m_verticalDamping = 5.5f;
		float m_waterDrag = 1.8f;
		float m_waveAmplitude = 0.36f;
		float m_waveLength = 13.0f;
		float m_waveSpeed = 0.62f;
	};
}

using namespace Sailor::Attributes;

REFL_AUTO(
	type(Sailor::BuoyancyComponent, bases<Sailor::Component>),
	func(GetWaterHeight, property("waterHeight")),
	func(SetWaterHeight, property("waterHeight")),
	func(GetHalfExtents, property("halfExtents")),
	func(SetHalfExtents, property("halfExtents")),
	func(GetFloatationPlane, property("floatationPlane")),
	func(SetFloatationPlane, property("floatationPlane")),
	func(GetEquilibriumDepth, property("equilibriumDepth")),
	func(SetEquilibriumDepth, property("equilibriumDepth")),
	func(GetBuoyancyScale, property("buoyancyScale")),
	func(SetBuoyancyScale, property("buoyancyScale")),
	func(GetVerticalDamping, property("verticalDamping")),
	func(SetVerticalDamping, property("verticalDamping")),
	func(GetWaterDrag, property("waterDrag")),
	func(SetWaterDrag, property("waterDrag")),
	func(GetWaveAmplitude, property("waveAmplitude")),
	func(SetWaveAmplitude, property("waveAmplitude")),
	func(GetWaveLength, property("waveLength")),
	func(SetWaveLength, property("waveLength")),
	func(GetWaveSpeed, property("waveSpeed")),
	func(SetWaveSpeed, property("waveSpeed"))
)
