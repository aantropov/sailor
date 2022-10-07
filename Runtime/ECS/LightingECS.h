#pragma once
#include "Sailor.h"
#include "Engine/Object.h"
#include "Tasks/Scheduler.h"
#include "ECS/ECS.h"
#include "Engine/Types.h"
#include "RHI/Types.h"
#include "Components/Component.h"
#include "Memory/Memory.h"

namespace Sailor
{
	using WorldPtr = class World*;
	using GameObjectPtr = TObjectPtr<class GameObject>;

	class LightData : public ECS::TComponent
	{

	public:

		SAILOR_API __forceinline ObjectPtr& GetOwner() { return m_owner; }
		SAILOR_API __forceinline void SetOwner(const ObjectPtr& owner) { m_owner = owner; }

		void SetDirty() { m_bIsDirty = true; }

		glm::vec3 m_intensity{ 1.0f, 1.0f, 1.0f };
		glm::vec3 m_attenuation{ 1.0f, 0.0f, 0.0f };
		glm::vec3 m_bounds{ 100.0f, 100.0f, 100.0f };
		ELightType m_type = ELightType::Point;

	protected:

		bool m_bIsActive : 1 = true;
		bool m_bIsDirty : 1 = true;

		ObjectPtr m_owner;
		friend class LightingECS;
	};

	class LightingECS : public ECS::TSystem<LightingECS, LightData>
	{
	public:

		const uint32_t LightsMaxNum = 65535;

		struct ShaderData
		{
			alignas(8) glm::vec3 m_worldPosition;
			alignas(8) glm::vec3 m_direction;
			alignas(8) glm::vec3 m_intensity;
			alignas(8) glm::vec3 m_attenuation;
			alignas(8) glm::vec3 m_bounds;
			alignas(8) int32_t m_type;
		};

		SAILOR_API virtual void BeginPlay() override;
		SAILOR_API virtual Tasks::ITaskPtr Tick(float deltaTime) override;
		SAILOR_API virtual void EndPlay() override;
		SAILOR_API virtual uint32_t GetOrder() const override { return 150; }

		void FillLightsData(RHI::RHISceneViewPtr& sceneView);

	protected:

		RHI::RHIShaderBindingSetPtr m_lightsData;
	};

	template ECS::TSystem<LightingECS, LightData>;
}
