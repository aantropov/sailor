#pragma once
#include "Sailor.h"
#include "Tasks/Scheduler.h"
#include "Engine/Types.h"
#include "Engine/Object.h"
#include "Components/Component.h"
#include "AssetRegistry/Model/ModelImporter.h"
#include "ECS/StaticMeshRendererECS.h"
#include "Containers/Octree.h"
#include "Framegraph/SkyNode.h"
#include "Math/Math.h"

namespace Sailor
{
	using TestComponentPtr = TObjectPtr<class TestComponent>;

	class TestComponent : public Component
	{
	public:

		SAILOR_API virtual void BeginPlay() override;
		SAILOR_API virtual void Tick(float deltaTime) override;
		SAILOR_API virtual void EndPlay() override;

	protected:

		float m_yaw = 0.0f;
		float m_pitch = 0.0f;

		TexturePtr defaultTexture;
		glm::ivec2 m_lastCursorPos;

		TVector<glm::vec4> m_lightVelocities;
		TVector<GameObjectPtr> m_lights;
		TOctree<Math::AABB> m_octree{};

		TVector<Math::AABB> m_culledBoxes{};
		TVector<Math::AABB> m_boxes{};

		GameObjectPtr m_dirLight;

		glm::mat4 m_cachedFrustum{ 1 };

		ModelPtr m_model{};
		GameObjectPtr m_mainModel;
		float m_sunAngleRad = glm::radians(60.0f);

		SkyNode::SkyParams m_skyParams{};
		float m_lastSunAngleRad = 0;
	};
}
