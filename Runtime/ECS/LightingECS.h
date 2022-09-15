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

		glm::vec3 m_intensity{ 1.0f, 1.0f, 1.0f };
		glm::vec3 m_attenuation{ 1.0f, 0.0f, 0.0f };
		glm::vec3 m_bounds{ 10.0f, 10.0f,10.0f };
		ELightType m_type = ELightType::Point;

	protected:

		bool m_bIsActive : 1 = true;

		ObjectPtr m_owner;
		friend class LightingECS;
	};

	class SAILOR_API LightingECS : public ECS::TSystem<LightingECS, LightData>
	{
	public:

		const uint32_t LightsMaxNum = 65535;

		struct ShaderData
		{
			glm::vec3 m_worldPosition;
			glm::vec3 m_direction;
			glm::vec3 m_intensity;
			glm::vec3 m_attenuation;
			glm::vec3 m_bounds;
			int32_t m_type;
		};

		virtual void BeginPlay() override;
		virtual Tasks::ITaskPtr Tick(float deltaTime) override;
		virtual void EndPlay() override;
		virtual uint32_t GetOrder() const override { return 150; }

	protected:

		RHI::RHISceneViewPtr m_sceneViewProxiesCache;
		RHI::RHIShaderBindingSetPtr m_lightsData;
	};
}
