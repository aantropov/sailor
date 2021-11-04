#pragma once
#include "AssetInfo.h"
#include "RHI/Types.h"
#include "Core/Singleton.hpp"

using namespace std;

namespace Sailor
{
	class MaterialAssetInfo final : public AssetInfo
	{
	public:
		virtual SAILOR_API ~MaterialAssetInfo() = default;

		virtual SAILOR_API void Serialize(nlohmann::json& outData) const override;
		virtual SAILOR_API void Deserialize(const nlohmann::json& inData) override;

	private:

	};

	class MaterialAssetInfoHandler final : public TSingleton<MaterialAssetInfoHandler>, public IAssetInfoHandler
	{

	public:

		static SAILOR_API void Initialize();

		virtual SAILOR_API void GetDefaultMetaJson(nlohmann::json& outDefaultJson) const;
		virtual SAILOR_API AssetInfo* CreateAssetInfo() const;

		virtual SAILOR_API ~MaterialAssetInfoHandler() = default;
	};
}
