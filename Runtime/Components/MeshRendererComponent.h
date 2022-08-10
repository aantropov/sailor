#pragma once
#include "Sailor.h"
#include "Tasks/Scheduler.h"
#include "Engine/Types.h"
#include "Engine/Object.h"
#include "Components/Component.h"
#include "AssetRegistry/Model/ModelImporter.h"
#include "ECS/StaticMeshRendererECS.h"

namespace Sailor
{	
	class MeshRendererComponent : public Component
	{
	public:

		SAILOR_API virtual void BeginPlay() override;
		SAILOR_API virtual void EndPlay() override;

		SAILOR_API __forceinline ModelPtr& GetModel() { return GetData().GetModel(); }
		SAILOR_API __forceinline TVector<MaterialPtr>& GetMaterials() { return GetData().GetMaterials(); }
		SAILOR_API __forceinline StaticMeshRendererData& GetData();
		SAILOR_API __forceinline size_t GetComponentIndex() const { return m_handle; }
		SAILOR_API __forceinline void SetRenderType(ERenderType type) { m_renderType = type; }

	protected:

		size_t m_handle = (size_t)(-1);
		ERenderType m_renderType = ERenderType::Stationary;
	};
}
