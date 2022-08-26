#pragma once
#include "Sailor.h"
#include "Engine/Object.h"
#include "ECS/ECS.h"
#include "Tasks/Scheduler.h"
#include "Components/Component.h"
#include "Math/Transform.h"
#include "Memory/Memory.h"
#include "RHI/SceneView.h"
#include "Memory/UniquePtr.hpp"

namespace Sailor
{
	using WorldPtr = class World*;
	using GameObjectPtr = TObjectPtr<class GameObject>;
	
	enum class ERenderType : uint8_t
	{
		Static = 0,
		Stationary = 1,
		Dynamic = 2
	};

	class StaticMeshRendererData
	{
	public:

		SAILOR_API __forceinline ObjectPtr& GetOwner() { return m_owner; }
		SAILOR_API __forceinline void SetOwner(const ObjectPtr& owner) { m_owner = owner; }

		SAILOR_API __forceinline TVector<MaterialPtr>& GetMaterials() { return m_materials; }
		SAILOR_API __forceinline ModelPtr& GetModel() { return m_model; }
		SAILOR_API __forceinline const size_t& GetLastFrameChanged() const { return m_lastChanges; }
		SAILOR_API __forceinline ERenderType GetRenderType() const { return m_type; }
		SAILOR_API __forceinline void SetRenderType(ERenderType type) { m_type = type; }

	protected:

		ModelPtr m_model;
		TVector<MaterialPtr> m_materials;

		bool m_bIsActive : 1 = true;
		ERenderType m_type = ERenderType::Stationary;
		ObjectPtr m_owner;
		size_t m_lastChanges = 0;
		friend class StaticMeshRendererECS;
	};

	class StaticMeshRendererECS : public ECS::TSystem<StaticMeshRendererECS, StaticMeshRendererData>
	{
	public:

		virtual void BeginPlay() override;
		virtual void EndPlay() override;

		virtual Tasks::ITaskPtr Tick(float deltaTime) override;
		void CopySceneView(RHI::RHISceneViewPtr& outProxies);

		virtual uint32_t GetOrder() const override { return 1000; }

	protected:

		RHI::RHISceneViewPtr m_sceneViewProxiesCache;
	};

	template ECS::TSystem<StaticMeshRendererECS, StaticMeshRendererData>;
}
