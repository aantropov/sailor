#pragma once
#include "Sailor.h"
#include "Components/Component.h"

namespace Sailor
{
	class ShadowVisualTestSetupComponent : public Component
	{
		SAILOR_REFLECTABLE(ShadowVisualTestSetupComponent)

	public:
		SAILOR_API virtual void BeginPlay() override;
		SAILOR_API virtual void Tick(float deltaTime) override;

		SAILOR_API bool GetDirectional() const { return m_bDirectional; }
		SAILOR_API void SetDirectional(bool value) { m_bDirectional = value; }
		SAILOR_API bool GetPoint() const { return m_bPoint; }
		SAILOR_API void SetPoint(bool value) { m_bPoint = value; }
		SAILOR_API bool GetSpot() const { return m_bSpot; }
		SAILOR_API void SetSpot(bool value) { m_bSpot = value; }

	private:
		GameObjectPtr SpawnBox(
			const char* name,
			const glm::vec3& position,
			const glm::vec3& scale,
			EMobilityType mobility);
		void SpawnLights();
		void EnsureSky();
		void EnsureCamera();

		GameObjectPtr m_stationaryCaster{};
		GameObjectPtr m_dynamicCaster{};
		TVector<TObjectPtr<class LightComponent>> m_localShadowLights{};
		uint32_t m_numFrames = 0u;
		float m_elapsedTime = 0.0f;
		bool m_bDirectional = false;
		bool m_bPoint = false;
		bool m_bSpot = false;
	};
}

using namespace Sailor::Attributes;

REFL_AUTO(
	type(Sailor::ShadowVisualTestSetupComponent, bases<Sailor::Component>),

	func(GetDirectional, property("directional"), SkipCDO()),
	func(SetDirectional, property("directional"), SkipCDO()),
	func(GetPoint, property("point"), SkipCDO()),
	func(SetPoint, property("point"), SkipCDO()),
	func(GetSpot, property("spot"), SkipCDO()),
	func(SetSpot, property("spot"), SkipCDO())
)
