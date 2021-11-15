#pragma once
#include "AssetInfo.h"
#include "RHI/Types.h"
#include "Core/Singleton.hpp"

using namespace std;

namespace Sailor
{
	class ModelAssetInfo final : public AssetInfo
	{
	public:
		virtual SAILOR_API ~ModelAssetInfo() = default;

		virtual SAILOR_API void Serialize(nlohmann::json& outData) const override;
		virtual SAILOR_API void Deserialize(const nlohmann::json& inData) override;

		SAILOR_API bool ShouldGenerateMaterials() const { return m_bShouldGenerateMaterials; }

	private:

		bool m_bShouldGenerateMaterials = true;
	};

	using ModelAssetInfoPtr = ModelAssetInfo*;

	class ModelAssetInfoHandler final : public TSingleton<ModelAssetInfoHandler>, public IAssetInfoHandler
	{

	public:

		static SAILOR_API void Initialize();

		virtual SAILOR_API void GetDefaultMetaJson(nlohmann::json& outDefaultJson) const;
		virtual SAILOR_API AssetInfoPtr CreateAssetInfo() const;

		virtual SAILOR_API ~ModelAssetInfoHandler() = default;
	};
}
