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

namespace Sailor::RHI
{
	class Vertex;
}

namespace Sailor
{
	class Model
	{
	public:

		Model(std::vector<RHI::MeshPtr> meshes, std::vector<RHI::MaterialPtr> materials) :
			m_meshes(std::move(meshes)),
			m_materials(std::move(materials)) {}

		Model(const Model&) = delete;
		Model(Model&&) = default;

		Model& operator=(const Model&) = delete;
		Model& operator=(Model&&) = default;

		virtual ~Model() = default;

	protected:
		std::vector<RHI::MeshPtr> m_meshes;
		std::vector <RHI::MaterialPtr> m_materials;
	};

	class ModelImporter final : public TSubmodule<ModelImporter>, public IAssetInfoHandlerListener
	{
	public:

		SAILOR_API ModelImporter(ModelAssetInfoHandler* infoHandler);
		virtual SAILOR_API ~ModelImporter() override;

		virtual SAILOR_API void OnUpdateAssetInfo(AssetInfoPtr assetInfo, bool bWasExpired) override;
		virtual SAILOR_API void OnImportAsset(AssetInfoPtr assetInfo) override;

		JobSystem::TaskPtr SAILOR_API LoadModel(UID uid, std::vector<RHI::MeshPtr>& outMeshes, std::vector<RHI::MaterialPtr>& outMaterials);

	private:

		SAILOR_API void GenerateMaterialAssets(ModelAssetInfoPtr assetInfo);
	};
}
