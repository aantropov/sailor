#pragma once
#include "AssetRegistry/AssetInfo.h"
#include "AssetRegistry/AssetFactory.h"
#include "RHI/Types.h"
#include "Core/Singleton.hpp"

using namespace std;

namespace Sailor
{
	class ModelAssetInfo final : public AssetInfo
	{
	public:
		SAILOR_API virtual ~ModelAssetInfo() = default;

		SAILOR_API virtual YAML::Node Serialize() const override;
		SAILOR_API virtual void Deserialize(const YAML::Node& inData) override;

		SAILOR_API float GetUnitScale() const { return m_unitScale; }
		SAILOR_API bool ShouldGenerateMaterials() const { return m_bShouldGenerateMaterials; }
		SAILOR_API bool ShouldBatchByMaterial() const { return m_bShouldBatchByMaterial; }

SAILOR_API const TVector<FileId>& GetDefaultMaterials() const { return m_materials; }
SAILOR_API TVector<FileId>& GetDefaultMaterials() { return m_materials; }
SAILOR_API const TVector<FileId>& GetAnimations() const { return m_animations; }
SAILOR_API TVector<FileId>& GetAnimations() { return m_animations; }
		
	private:

TVector<FileId> m_materials;
TVector<FileId> m_animations;
		float m_unitScale = 1.0f;
		bool m_bShouldGenerateMaterials = true;
		bool m_bShouldBatchByMaterial = true;
	};

	using ModelAssetInfoPtr = ModelAssetInfo*;

	class SAILOR_API ModelAssetInfoHandler final : public TSubmodule<ModelAssetInfoHandler>, public IAssetInfoHandler
	{

	public:

		ModelAssetInfoHandler(AssetRegistry* assetRegistry);

		IAssetFactory* GetFactory() override;

		virtual void GetDefaultMeta(YAML::Node& outDefaultYaml) const override;
		AssetInfoPtr CreateAssetInfo() const override;

		virtual ~ModelAssetInfoHandler() = default;
	};
}
