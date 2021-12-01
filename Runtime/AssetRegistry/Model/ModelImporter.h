#pragma once
#include "Core/Defines.h"
#include <string>
#include <vector>
#include <nlohmann_json/include/nlohmann/json.hpp>
#include "Core/Submodule.h"
#include "Memory/SharedPtr.hpp"
#include "Memory/WeakPtr.hpp"
#include "AssetRegistry/AssetInfo.h"
#include "RHI/Renderer.h"
#include "ModelAssetInfo.h"
#include "JobSystem/JobSystem.h"
#include "ModelAssetInfo.h"
#include "Framework/Object.h"
#include "RHI/Mesh.h"
#include "RHI/Material.h"
#include "AssetRegistry/Material/MaterialImporter.h"

namespace Sailor::RHI
{
	class Vertex;
}

namespace Sailor
{
	class Model : public Object
	{
	public:

		Model(UID uid, std::vector<RHI::MeshPtr> meshes = {}, std::vector<MaterialPtr> materials = {}) :
			Object(std::move(uid)),
			m_meshes(std::move(meshes)),
			m_materials(std::move(materials)) {}

		const std::vector<RHI::MeshPtr>& GetMeshes() const { return m_meshes; }
		const std::vector<MaterialPtr>& GetMaterials() const { return m_materials; }

		std::vector<RHI::MeshPtr>& GetMeshes() { return m_meshes; }
		std::vector<MaterialPtr>& GetMaterials() { return m_materials; }

		// Should be triggered after mesh/material changes
		void Flush();

		virtual bool IsReady() const override;
		virtual ~Model() = default;

	protected:

		std::vector<RHI::MeshPtr> m_meshes;
		std::vector<MaterialPtr> m_materials;

		std::atomic<bool> m_bIsReady;

		friend class ModelImporter;
	};

	using ModelPtr = TWeakPtr<Model>;

	class ModelImporter final : public TSubmodule<ModelImporter>, public IAssetInfoHandlerListener
	{
	public:

		SAILOR_API ModelImporter(ModelAssetInfoHandler* infoHandler);
		virtual SAILOR_API ~ModelImporter() override;

		virtual SAILOR_API void OnUpdateAssetInfo(AssetInfoPtr assetInfo, bool bWasExpired) override;
		virtual SAILOR_API void OnImportAsset(AssetInfoPtr assetInfo) override;

		SAILOR_API bool LoadModel(UID uid, ModelPtr& outModel, JobSystem::TaskPtr& outLoadingTask);
		SAILOR_API bool LoadModel_Immediate(UID uid, ModelPtr& outModel);

	private:

		static SAILOR_API bool ImportObjModel(ModelAssetInfoPtr assetInfo,
			std::vector<RHI::MeshPtr>& outMeshes,
			std::vector<MaterialPtr>& outMaterials);

		SAILOR_API void GenerateMaterialAssets(ModelAssetInfoPtr assetInfo);

		std::unordered_map<UID, TSharedPtr<Model>> m_loadedModels;
	};
}
