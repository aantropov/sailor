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
		SAILOR_API bool LoadModel(const std::string& path);
		SAILOR_API int32_t GetMeshIndex() const { return m_meshIndex; }
		SAILOR_API void SetMeshIndex(int32_t meshIndex);
		SAILOR_API const TVector<FileId>& GetOverrideMaterials() const { return m_overrideMaterials; }
		SAILOR_API void SetOverrideMaterials(const TVector<FileId>& overrideMaterials);
		SAILOR_API uint32_t GetMinLod() const { return m_minLod; }
		SAILOR_API void SetMinLod(uint32_t minLod);
		SAILOR_API uint32_t GetMaxLod() const { return m_maxLod; }
		SAILOR_API void SetMaxLod(uint32_t maxLod);
		SAILOR_API const TVector<float>& GetScreenCoverageThresholds() const { return m_screenCoverageThresholds; }
		SAILOR_API void SetScreenCoverageThresholds(const TVector<float>& screenCoverageThresholds);

		SAILOR_API __forceinline TVector<MaterialPtr>& GetMaterials() { return GetData().GetMaterials(); }
		SAILOR_API __forceinline StaticMeshRendererData& GetData();
		SAILOR_API __forceinline const StaticMeshRendererData& GetData() const;
		SAILOR_API __forceinline size_t GetComponentIndex() const { return m_handle; }

	protected:

		void RebuildMaterials();

		size_t m_handle = (size_t)(-1);
		int32_t m_meshIndex = Model::AllMeshes;
		TVector<FileId> m_overrideMaterials;
		uint32_t m_minLod = 0u;
		uint32_t m_maxLod = 2u;
		TVector<float> m_screenCoverageThresholds{ 0.25f, 0.05f };
	};
}

using namespace Sailor::Attributes;

REFL_AUTO(
	type(Sailor::MeshRendererComponent, bases<Sailor::Component>),

	func(GetModel, property("model"), SkipCDO()),
	func(SetModel, property("model"), SkipCDO()),
	func(GetMeshIndex, property("meshIndex")),
	func(SetMeshIndex, property("meshIndex")),

	func(GetOverrideMaterials, property("overrideMaterials")),
	func(SetOverrideMaterials, property("overrideMaterials")),
	func(GetMinLod, property("minLod"), Range(0.0, 16.0)),
	func(SetMinLod, property("minLod")),
	func(GetMaxLod, property("maxLod"), Range(0.0, 16.0)),
	func(SetMaxLod, property("maxLod")),
	func(GetScreenCoverageThresholds, property("screenCoverageThresholds")),
	func(SetScreenCoverageThresholds, property("screenCoverageThresholds"))
)
