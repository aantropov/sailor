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

		SAILOR_API __forceinline TVector<MaterialPtr>& GetMaterials() { return m_materials; }
		SAILOR_API __forceinline ModelPtr& GetModel() { return m_model; }

	protected:

		ModelPtr m_model;
		TVector<MaterialPtr> m_materials;

		bool m_bIsActive : 1 = false;
		ObjectPtr m_owner;

		friend class StaticMeshRendererECS;
	};

	class SAILOR_API StaticMeshRendererECS : public ECS::TSystem<StaticMeshRendererECS, StaticMeshRendererData>
	{
	public:

		virtual JobSystem::ITaskPtr Tick(float deltaTime) override;

	protected:

	};
}
