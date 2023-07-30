#pragma once
#include "Sailor.h"
#include "Memory/SharedPtr.hpp"
#include "Memory/ObjectPtr.hpp"
#include "Tasks/Scheduler.h"
#include "Engine/Types.h"
#include "Engine/Object.h"
#include "Components/Component.h"
#include "ECS/LightingECS.h"

namespace Sailor
{
	class LightComponent : public Component
	{
		SAILOR_REFLECTABLE(LightComponent)

	public:

		SAILOR_API virtual void BeginPlay() override;
		SAILOR_API virtual void EndPlay() override;
		SAILOR_API virtual void OnGizmo() override;

		SAILOR_API __forceinline const LightData& GetData() const;

		SAILOR_API __forceinline const glm::vec3& GetIntensity() const { return GetData().m_intensity; }
		SAILOR_API __forceinline const glm::vec3& GetAttenuation() const { return GetData().m_attenuation; }
		SAILOR_API __forceinline const glm::vec3& GetBounds() const { return GetData().m_bounds; }
		SAILOR_API __forceinline const glm::vec2& GetCutOff() const { return GetData().m_cutOff; }
		SAILOR_API __forceinline ELightType GetLightType() const { return (ELightType)GetData().m_type; }

		SAILOR_API __forceinline void SetCutOff(const glm::vec2& innerOuterDegrees);
		SAILOR_API __forceinline void SetIntensity(const glm::vec3& value);
		SAILOR_API __forceinline void SetAttenuation(const glm::vec3& value);
		SAILOR_API __forceinline void SetBounds(const glm::vec3& value);
		SAILOR_API __forceinline void SetLightType(ELightType value);

	protected:

		SAILOR_API __forceinline LightData& GetData();

		size_t m_handle = (size_t)(-1);
	};
}

REFL_AUTO(
	type(Sailor::LightComponent, bases<Sailor::Component>),

	func(GetIntensity, property("intensity")),
	func(SetIntensity, property("intensity")),

	func(GetAttenuation, property("attenuation")),
	func(SetAttenuation, property("attenuation")),

	func(GetBounds, property("bounds")),
	func(SetBounds, property("bounds")),

	func(GetCutOff, property("cutOff")),
	func(SetCutOff, property("cutOff")),

	func(GetLightType, property("lightType")),
	func(SetLightType, property("lightType"))
)