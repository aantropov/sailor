#pragma once
#include "Sailor.h"
#include "ECS/ECS.h"
#include "Engine/Types.h"
#include "Raytracing/PathTracer.h"
#include "Math/Bounds.h"
#include "AssetRegistry/FileId.h"
#include "RHI/SceneView.h"

namespace Sailor
{
	class PathTracerProxyData final : public ECS::TComponent
	{
	public:

		struct Options
		{
			bool m_bEnabled = false;
			bool m_bRebuildEveryFrame = false;
		};

		// Model and materials belong to MeshRendererComponent.
		SAILOR_API const Options& GetOptions() const { return m_options; }
		SAILOR_API Options& GetOptions() { return m_options; }

		SAILOR_API const Math::AABB& GetWorldBounds() const { return m_worldBounds; }
		SAILOR_API const glm::mat4& GetWorldMatrix() const { return m_worldMatrix; }
		SAILOR_API const glm::mat4& GetInverseWorldMatrix() const { return m_inverseWorldMatrix; }

		SAILOR_API virtual void Clear() override
		{
			m_owner = nullptr;
			m_options = Options();
			m_worldBounds = Math::AABB();
			m_worldMatrix = glm::mat4(1.0f);
			m_inverseWorldMatrix = glm::mat4(1.0f);
			m_modelFileId = FileId();
			m_meshIndex = -1;
			m_bNeedsRebuild = true;
			m_frameLastChange = 0;
			m_bIsDirty = false;
		}

	protected:

		Options m_options{};

		Math::AABB m_worldBounds{};
		glm::mat4 m_worldMatrix{ 1.0f };
		glm::mat4 m_inverseWorldMatrix{ 1.0f };
		FileId m_modelFileId{};
		int32_t m_meshIndex = -1;
		bool m_bNeedsRebuild = true;

		friend class PathTracerECS;
	};

	class PathTracerECS final : public ECS::TSystem<PathTracerECS, PathTracerProxyData>
	{
	public:

		SAILOR_API virtual void Tick(float) override {}
		SAILOR_API virtual void EndPlay() override;
		SAILOR_API void CopySceneView(RHI::RHISceneViewPtr& outSceneView);
		void SetPathTracingEnabled(bool bEnabled) { m_bPathTracingEnabled = bEnabled; }
		bool IsPathTracingEnabled() const { return m_bPathTracingEnabled; }
		virtual uint32_t GetOrder() const override { return 1100; }

	protected:

		void UpdateScene();
		bool m_bPathTracingEnabled = false;
		RHI::RHIPathTracerScenePtr m_scene;
		Raytracing::PathTracer::MaterialSnapshotCache m_materialSnapshots;
	};

	template class ECS::TSystem<PathTracerECS, PathTracerProxyData>;
}
