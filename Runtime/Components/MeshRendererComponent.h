#pragma once
#include "Sailor.h"
#include "Memory/SharedPtr.hpp"
#include "Memory/ObjectPtr.hpp"
#include "JobSystem/JobSystem.h"
#include "Engine/Object.h"
#include "Components/Component.h"
#include "AssetRegistry/Model/ModelImporter.h"
#include "ECS/StaticMeshRendererECS.h"

namespace Sailor
{
	using WorldPtr = class World*;
	using GameObjectPtr = TObjectPtr<class GameObject>;
	using MeshRendererComponentPtr = TObjectPtr<class MeshRendererComponent>;

	class MeshRendererComponent : public Component
	{
	public:

		SAILOR_API virtual void BeginPlay() override;
		SAILOR_API virtual void EndPlay() override;

		SAILOR_API __forceinline ModelPtr& GetModel() { return GetData().GetModel(); }
		SAILOR_API __forceinline TVector<MaterialPtr>& GetMaterials() { return GetData().GetMaterials(); }
		SAILOR_API __forceinline StaticMeshRendererData& GetData();

	protected:

		size_t m_handle = (size_t)(-1);
	};
}
