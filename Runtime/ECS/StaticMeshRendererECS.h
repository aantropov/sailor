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

	class StaticMeshRendererData final : public ECS::TComponent
	{
	public:

		SAILOR_API __forceinline TVector<MaterialPtr>& GetMaterials() { return m_materials; }

		SAILOR_API __forceinline const ModelPtr& GetModel() const { return m_model; }
		SAILOR_API __forceinline void SetModel(const ModelPtr& model) { m_model = model; }

		SAILOR_API __forceinline bool ShouldCastShadow() const { return true; }

	protected:

		ModelPtr m_model;
		TVector<MaterialPtr> m_materials;

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

	template class ECS::TSystem<StaticMeshRendererECS, StaticMeshRendererData>;
}
