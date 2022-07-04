#pragma once
#include "Sailor.h"
#include "Tasks/Scheduler.h"
#include "Engine/Types.h"
#include "Engine/Object.h"
#include "Components/Component.h"
#include "AssetRegistry/Model/ModelImporter.h"
#include "ECS/StaticMeshRendererECS.h"
#include "Containers/Octree.h"

namespace Sailor
{
	using TestComponentPtr = TObjectPtr<class TestComponent>;

	class TestComponent : public Component
	{
	public:

		SAILOR_API virtual void BeginPlay() override;
		SAILOR_API virtual void Tick(float deltaTime) override;
		SAILOR_API virtual void EndPlay() override;

		SAILOR_API RHI::RHIShaderBindingSetPtr& GetPerInstanceBinding() { return m_perInstanceData; }
		SAILOR_API RHI::RHIShaderBindingSetPtr& GetFrameBinding() { return m_frameDataBinding; }

		SAILOR_API const TVector<RHI::RHIMeshPtr>& GetCulledMeshes() const { return m_culledMeshes; }

	protected:

		float m_yaw = 0.0f;
		float m_pitch = 0.0f;

		RHI::RHIShaderBindingSetPtr m_perInstanceData;
		RHI::UboFrameData m_frameData;
		RHI::RHIShaderBindingSetPtr m_frameDataBinding;

		TexturePtr defaultTexture;
		glm::ivec2 m_lastCursorPos;

		TVector<RHI::RHIMeshPtr> m_culledMeshes;
		TOctree<Math::AABB> m_octree{};

		TVector<Math::AABB> m_culledBoxes{};
		TVector<Math::AABB> m_boxes{};

		glm::mat4 m_cachedFrustum{1};
	};
}
