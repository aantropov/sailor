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

		SAILOR_API virtual void Initialize() override;
		SAILOR_API virtual void BeginPlay() override;
		SAILOR_API virtual void EndPlay() override;
		SAILOR_API virtual void OnGizmo() override;

		SAILOR_API __forceinline const LightData& GetData() const;

		SAILOR_API __forceinline const glm::vec3& GetIntensity() const { return GetData().m_intensity; }
		SAILOR_API __forceinline const glm::vec3& GetAttenuation() const { return GetData().m_attenuation; }
		SAILOR_API __forceinline const glm::vec3& GetBounds() const { return GetData().m_bounds; }
                SAILOR_API __forceinline const glm::vec2& GetCutOff() const { return GetData().m_cutOff; }
                SAILOR_API __forceinline ELightType GetLightType() const { return (ELightType)GetData().m_type; }
                SAILOR_API __forceinline float GetEvsmBlurScale() const { return GetData().m_evsmBlurScale; }

		SAILOR_API void SetCutOff(const glm::vec2& innerOuterDegrees);
		SAILOR_API void SetIntensity(const glm::vec3& value);
		SAILOR_API void SetAttenuation(const glm::vec3& value);
		SAILOR_API void SetBounds(const glm::vec3& value);
                SAILOR_API void SetLightType(ELightType value);
                SAILOR_API void SetEvsmBlurScale(float value);

	protected:

		SAILOR_API __forceinline LightData& GetData();

		size_t m_handle = (size_t)(-1);
	};
}

using namespace Sailor::Attributes;

REFL_AUTO(
	type(Sailor::LightComponent, bases<Sailor::Component>),

	func(GetIntensity, property("intensity"), SkipCDO()),
	func(SetIntensity, property("intensity"), SkipCDO()),

	func(GetAttenuation, property("attenuation"), SkipCDO()),
	func(SetAttenuation, property("attenuation"), SkipCDO()),

	func(GetBounds, property("bounds"), SkipCDO()),
	func(SetBounds, property("bounds"), SkipCDO()),

	func(GetCutOff, property("cutOff"), SkipCDO()),
        func(SetCutOff, property("cutOff"), SkipCDO()),

        func(GetLightType, property("lightType"), SkipCDO()),
        func(SetLightType, property("lightType"), SkipCDO()),

        func(GetEvsmBlurScale, property("evsmBlurScale"), SkipCDO()),
        func(SetEvsmBlurScale, property("evsmBlurScale"), SkipCDO())
)