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

	class StaticMeshRendererData
	{
	public:

		SAILOR_API __forceinline ObjectPtr& GetOwner() { return m_owner; }
		SAILOR_API __forceinline void SetOwner(const ObjectPtr& owner) { m_owner = owner; }

		SAILOR_API __forceinline ModelPtr& GetModel() { return m_model; }
		SAILOR_API __forceinline void SetModel(const ModelPtr& model) { m_model = model; }

		SAILOR_API void SetTransform(size_t transformIndex) { m_transformIndex = transformIndex; }
		SAILOR_API size_t GetTransform(size_t transformIndex) { return m_transformIndex; }

	protected:

		bool m_bIsActive : 1 = false;
		size_t m_transformIndex = ECS::InvalidIndex;
		ObjectPtr m_owner;
		ModelPtr m_model;

		friend class StaticMeshRendererECS;
	};

	class SAILOR_API StaticMeshRendererECS : public ECS::TSystem<StaticMeshRendererECS, StaticMeshRendererData>
	{
	public:

		virtual JobSystem::ITaskPtr Tick(float deltaTime) override;

	protected:

	};
}
