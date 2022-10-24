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

		SAILOR_API __forceinline GameObjectPtr& GetOwner() { return m_owner; }
		SAILOR_API __forceinline void SetOwner(const GameObjectPtr& owner) { m_owner = owner; }

		void SetDirty() { m_bIsDirty = true; }

		glm::vec3 m_intensity{ 100.0f, 100.0f, 100.0f };
		glm::vec3 m_attenuation{ 0.0f, 1.0f, 0.0f };
		glm::vec3 m_bounds{ 100.0f, 100.0f, 100.0f };
		ELightType m_type = ELightType::Point;

	protected:

		bool m_bIsActive : 1 = true;
		bool m_bIsDirty : 1 = true;

		GameObjectPtr m_owner;
		friend class LightingECS;
	};

	class LightingECS : public ECS::TSystem<LightingECS, LightData>
	{
	public:

		const uint32_t LightsMaxNum = 65535;

		// TODO: Tightly pack
		struct ShaderData
		{
			 alignas(16) glm::vec3 m_worldPosition;
			 alignas(16) glm::vec3 m_direction;
			 alignas(16) glm::vec3 m_intensity;
			 alignas(16) glm::vec3 m_attenuation;
			 int32_t m_type;
			 alignas(16) glm::vec3 m_bounds;
		};

		SAILOR_API virtual void BeginPlay() override;
		SAILOR_API virtual Tasks::ITaskPtr Tick(float deltaTime) override;
		SAILOR_API virtual void EndPlay() override;
		SAILOR_API virtual uint32_t GetOrder() const override { return 150; }

		void FillLightsData(RHI::RHISceneViewPtr& sceneView);

	protected:

		TVector<TPair<uint32_t, uint32_t>> m_skipList;
		RHI::RHIShaderBindingSetPtr m_lightsData;
	};

	template ECS::TSystem<LightingECS, LightData>;
}
