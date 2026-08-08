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
		static constexpr uint32_t InvalidSkeletonOffset = (std::numeric_limits<uint32_t>::max)();

		SAILOR_API __forceinline TVector<MaterialPtr>& GetMaterials() { return m_materials; }

		SAILOR_API __forceinline const ModelPtr& GetModel() const { return m_model; }
		SAILOR_API __forceinline void SetModel(const ModelPtr& model)
		{
			m_model = model;
			m_bInvalidMeshIndexReported = false;
			if (!m_model)
			{
				m_materials.Clear();
			}
		}
		SAILOR_API __forceinline int32_t GetMeshIndex() const { return m_meshIndex; }
		SAILOR_API __forceinline void SetMeshIndex(int32_t meshIndex)
		{
			if (m_meshIndex != meshIndex)
			{
				m_meshIndex = meshIndex;
				m_bInvalidMeshIndexReported = false;
				MarkDirty();
			}
		}

		SAILOR_API __forceinline bool ShouldCastShadow() const { return true; }
		SAILOR_API __forceinline uint32_t GetSkeletonOffset() const { return m_skeletonOffset; }
		SAILOR_API uint32_t ResolveLod(float screenCoverage, uint32_t numAvailableLods) const;
		SAILOR_API void SetLodSettings(
			uint32_t minLod,
			uint32_t maxLod,
			const TVector<float>& screenCoverageThresholds);
	protected:

		ModelPtr m_model;
		TVector<MaterialPtr> m_materials;
		TVector<uint64_t> m_materialContentRevisions;
		RHI::RHIShadowCasterProxyPtr m_shadowCaster;
		uint32_t m_skeletonOffset = InvalidSkeletonOffset;
		int32_t m_meshIndex = -1;
		uint32_t m_minLod = 0u;
		uint32_t m_maxLod = 2u;
		TVector<float> m_screenCoverageThresholds{ 0.25f, 0.05f };
		bool m_bInvalidMeshIndexReported = false;

		friend class StaticMeshRendererECS;
	};

	class StaticMeshRendererECS : public ECS::TSystem<StaticMeshRendererECS, StaticMeshRendererData>
	{
	public:

		SAILOR_API virtual void BeginPlay() override;
		SAILOR_API virtual void EndPlay() override;
		SAILOR_API virtual Tasks::ITaskPtr Tick(float deltaTime) override;
		void CopySceneView(RHI::RHISceneViewPtr& outProxies);
		void MarkDirty(GameObjectPtr owner);

		virtual uint32_t GetOrder() const override { return 1000; }

	protected:

		SAILOR_API virtual void OnComponentUnregistered(size_t index, StaticMeshRendererData& component) override;

		RHI::RHISceneViewPtr m_sceneViewProxiesCache;
		uint64_t m_lastMaterialContentRevision = 0;
	};

	template class ECS::TSystem<StaticMeshRendererECS, StaticMeshRendererData>;
}
