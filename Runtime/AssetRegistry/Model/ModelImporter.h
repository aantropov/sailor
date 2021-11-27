#pragma once
#include "Core/Defines.h"
#include <string>
#include <vector>
#include <nlohmann_json/include/nlohmann/json.hpp>
#include "Core/Singleton.hpp"
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

		RHI::MeshPtr m_meshes;
		RHI::MaterialPtr m_materials;
	};

	class ModelImporter final : public TSingleton<ModelImporter>, public IAssetInfoHandlerListener
	{
	public:

		static SAILOR_API void Initialize();
		virtual SAILOR_API ~ModelImporter() override;

		virtual SAILOR_API void OnUpdateAssetInfo(AssetInfoPtr assetInfo, bool bWasExpired) override;
		virtual SAILOR_API void OnImportAsset(AssetInfoPtr assetInfo) override;

		TSharedPtr<JobSystem::Job> SAILOR_API LoadModel(UID uid, std::vector<RHI::MeshPtr>& outMeshes, std::vector<RHI::MaterialPtr>& outMaterials);

	private:

		SAILOR_API void GenerateMaterialAssets(ModelAssetInfoPtr assetInfo);
	};
}
