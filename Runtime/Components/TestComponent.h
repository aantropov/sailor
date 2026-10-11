#pragma once
#include "Sailor.h"
#include "Tasks/Scheduler.h"
#include "Engine/Types.h"
#include "Engine/Object.h"
#include "Components/Component.h"
#include "AssetRegistry/Model/ModelImporter.h"
#include "ECS/StaticMeshRendererECS.h"
#include "Containers/Octree.h"
#include "Math/Math.h"
#include "Components/MeshRendererComponent.h"
#include "FrameGraph/CopyTextureToRamNode.h"
#include <string>

namespace Sailor
{
	using TestComponentPtr = TObjectPtr<class TestComponent>;

	class TestComponent : public Component
	{
		SAILOR_REFLECTABLE(TestComponent)

	public:

		SAILOR_API virtual void BeginPlay() override;
		SAILOR_API virtual void Tick(float deltaTime) override;
		SAILOR_API virtual void EndPlay() override;

		MeshRendererComponentPtr m_meshRenderer;
		glm::quat m_testQuat;

	protected:

		float m_yaw = 0.0f;
		float m_pitch = 0.0f;

		TexturePtr defaultTexture;
		glm::ivec2 m_lastCursorPos;

		TVector<glm::vec4> m_lightVelocities;
		TVector<GameObjectPtr> m_lights;
		TOctree<Math::AABB> m_octree{};
		TVector<GameObjectPtr> m_objects;

		TVector<Math::AABB> m_culledBoxes{};
		TVector<Math::AABB> m_boxes{};

		glm::mat4 m_cachedFrustum{ 1 };

		ModelPtr m_model{};
		GameObjectPtr m_mainModel;
		Framegraph::CopyTextureToRamNode::CaptureTask m_capture;
		Framegraph::CopyTextureToRamNode::CaptureTask m_maskCapture;
	};
}

REFL_AUTO(
	type(Sailor::TestComponent, bases<Sailor::Component>),
	field(m_testQuat),
	field(m_meshRenderer)
)
