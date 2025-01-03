#pragma once
#include "Sailor.h"
#include "Tasks/Scheduler.h"
#include "Engine/Types.h"
#include "Engine/Object.h"
#include "Engine/Types.h"
#include "Components/Component.h"
#include "AssetRegistry/Model/ModelImporter.h"
#include "ECS/StaticMeshRendererECS.h"
#include "AssetRegistry/AssetRegistry.h"

namespace Sailor
{
	class MeshRendererComponent : public Component
	{
		SAILOR_REFLECTABLE(MeshRendererComponent)

	public:

		SAILOR_API virtual void Initialize() override;
		SAILOR_API virtual void BeginPlay() override;
		SAILOR_API virtual void EndPlay() override;

		SAILOR_API const ModelPtr& GetModel() const { return GetData().GetModel(); }
		SAILOR_API void SetModel(const ModelPtr& model);

		SAILOR_API __forceinline TVector<MaterialPtr>& GetMaterials() { return GetData().GetMaterials(); }
		SAILOR_API __forceinline StaticMeshRendererData& GetData();
		SAILOR_API __forceinline const StaticMeshRendererData& GetData() const;
		SAILOR_API __forceinline size_t GetComponentIndex() const { return m_handle; }

		void LoadModel(const std::string& path);

	protected:

		size_t m_handle = (size_t)(-1);
	};
}

using namespace Sailor::Attributes;

REFL_AUTO(
	type(Sailor::MeshRendererComponent, bases<Sailor::Component>),

	func(GetModel, property("model"), SkipCDO()),
	func(SetModel, property("model"), SkipCDO())
)