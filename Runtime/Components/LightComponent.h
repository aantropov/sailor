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
	public:

		SAILOR_API virtual void BeginPlay() override;
		SAILOR_API virtual void EndPlay() override;
		SAILOR_API virtual void OnGizmo() override;

		SAILOR_API __forceinline const LightData& GetData() const;

		SAILOR_API __forceinline const glm::vec3& GetIntensity() const { return GetData().m_intensity; }
		SAILOR_API __forceinline const glm::vec3& GetAttenuation() const { return GetData().m_attenuation; }
		SAILOR_API __forceinline const glm::vec3& GetBounds() const { return GetData().m_bounds; }
		SAILOR_API __forceinline ELightType GetLightType() const { return (ELightType)GetData().m_type; }

		SAILOR_API __forceinline void SetIntensity(const glm::vec3& value) { GetData().m_intensity = value; }
		SAILOR_API __forceinline void SetAttenuation(const glm::vec3& value) { GetData().m_attenuation = value; }
		SAILOR_API __forceinline void SetBounds(const glm::vec3& value) { GetData().m_bounds = value; }
		SAILOR_API __forceinline void SetLightType(ELightType value) { GetData().m_type = value; }

	protected:

		SAILOR_API __forceinline LightData& GetData();

		size_t m_handle = (size_t)(-1);

	};
}
