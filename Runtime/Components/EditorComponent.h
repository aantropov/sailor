#pragma once
#include "Sailor.h"
#include "Tasks/Scheduler.h"
#include "Engine/Types.h"
#include "Engine/Object.h"
#include "Components/Component.h"
#include "AssetRegistry/Model/ModelImporter.h"
#include "ECS/StaticMeshRendererECS.h"
#include "Containers/Octree.h"
#include "FrameGraph/SkyNode.h"
#include "Math/Math.h"
#include "Components/MeshRendererComponent.h"

namespace Sailor
{
	using EditorComponentPtr = TObjectPtr<class EditorComponent>;

	class EditorComponent : public Component
	{
		SAILOR_REFLECTABLE(EditorComponent)

	public:

		SAILOR_API virtual void EditorTick(float deltaTime) override;

	protected:

		bool m_bInited = false;
		float m_yaw = 0.0f;
		float m_pitch = 0.0f;

		TexturePtr defaultTexture;
		glm::ivec2 m_lastCursorPos;

		GameObjectPtr m_mainLight;
		float m_sunAngleRad = glm::radians(60.0f);
		size_t m_skyHash = 0;
	};
}

REFL_AUTO(
	type(Sailor::EditorComponent, bases<Sailor::Component>)
)