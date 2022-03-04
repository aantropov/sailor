#pragma once
#include "Core/Defines.h"
#include <string>
#include "Containers/Vector.h"
#include "Containers/ConcurrentMap.h"
#include <nlohmann_json/include/nlohmann/json.hpp>
#include "Core/Submodule.h"
#include "Memory/SharedPtr.hpp"
#include "Memory/WeakPtr.hpp"
#include "AssetRegistry/AssetInfo.h"
#include "RHI/Renderer.h"
#include "ModelAssetInfo.h"
#include "JobSystem/JobSystem.h"
#include "ModelAssetInfo.h"
#include "Engine/Object.h"
#include "Memory/ObjectPtr.hpp"
#include "Memory/ObjectAllocator.hpp"
#include "RHI/Mesh.h"
#include "RHI/Material.h"
#include "AssetRegistry/Material/MaterialImporter.h"

namespace Sailor::RHI
{
	class Vertex;
}

namespace Sailor
{
	using ModelPtr = TObjectPtr<class Model>;

	class Model : public Object
	{
	public:

		SAILOR_API Model(UID uid, TVector<RHI::MeshPtr> meshes = {}, TVector<MaterialPtr> materials = {}) :
			Object(std::move(uid)),
			m_meshes(std::move(meshes)),
			m_materials(std::move(materials)) {}

		SAILOR_API const TVector<RHI::MeshPtr>& GetMeshes() const { return m_meshes; }
		SAILOR_API const TVector<MaterialPtr>& GetDefaultMaterials() const { return m_materials; }

		SAILOR_API TVector<RHI::MeshPtr>& GetMeshes() { return m_meshes; }
		SAILOR_API TVector<MaterialPtr>& GetDefaultMaterials() { return m_materials; }

		// Should be triggered after mesh/material changes
		SAILOR_API void Flush();

		SAILOR_API virtual bool IsReady() const override;
		SAILOR_API virtual ~Model() = default;

	protected:

		TVector<RHI::MeshPtr> m_meshes;
		std::atomic<bool> m_bIsReady;

		// TODO: Move materials to .asset
		TVector<MaterialPtr> m_materials;

		friend class ModelImporter;
	};

	class ModelImporter final : public TSubmodule<ModelImporter>, public IAssetInfoHandlerListener
	{
	public:

		SAILOR_API ModelImporter(ModelAssetInfoHandler* infoHandler);
		SAILOR_API virtual ~ModelImporter() override;

		SAILOR_API virtual void OnUpdateAssetInfo(AssetInfoPtr assetInfo, bool bWasExpired) override;
		SAILOR_API virtual void OnImportAsset(AssetInfoPtr assetInfo) override;

		SAILOR_API JobSystem::TaskPtr<bool> LoadModel(UID uid, ModelPtr& outModel);
		SAILOR_API bool LoadModel_Immediate(UID uid, ModelPtr& outModel);

		SAILOR_API JobSystem::TaskPtr<bool> LoadDefaultMaterials(UID uid, TVector<MaterialPtr>& outMaterials);

	protected:

		TConcurrentMap<UID, JobSystem::TaskPtr<bool>> m_promises;
		TConcurrentMap<UID, ModelPtr> m_loadedModels;

		SAILOR_API static bool ImportObjModel(ModelAssetInfoPtr assetInfo, TVector<RHI::MeshPtr>& outMeshes);
		SAILOR_API void GenerateMaterialAssets(ModelAssetInfoPtr assetInfo);

		ObjectAllocatorPtr m_allocator;
	};
}
