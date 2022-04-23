#pragma once
#include "Sailor.h"
#include "Memory/SharedPtr.hpp"
#include "Memory/ObjectPtr.hpp"
#include "JobSystem/JobSystem.h"
#include "Engine/Object.h"
#include "ECS/ECS.h"
#include "Components/Component.h"
#include "Math/Transform.h"
#include "Memory/Memory.h"
#include "AssetRegistry/Model/ModelImporter.h"

namespace Sailor
{
	using WorldPtr = class World*;
	using GameObjectPtr = TObjectPtr<class GameObject>;

	class CameraData
	{
	public:

		SAILOR_API __forceinline ObjectPtr& GetOwner() { return m_owner; }
		SAILOR_API __forceinline void SetOwner(const ObjectPtr& owner) { m_owner = owner; }

		SAILOR_API __forceinline void SetProjectionMatrix(const glm::mat4& projection) { m_projectionMatrix = projection; }

		SAILOR_API __forceinline const glm::mat4& GetProjectionMatrix() const { return m_projectionMatrix; }
		SAILOR_API __forceinline const glm::mat4& GetViewMatrix() const { return m_viewMatrix; }

	protected:

		bool m_bIsActive : 1 = true;
		glm::mat4 m_projectionMatrix;
		glm::mat4 m_viewMatrix;

		ObjectPtr m_owner;

		friend class CameraECS;
	};

	class SAILOR_API CameraECS : public ECS::TSystem<CameraECS, CameraData>
	{
	public:

		virtual JobSystem::ITaskPtr Tick(float deltaTime) override;

	protected:

	};
}
