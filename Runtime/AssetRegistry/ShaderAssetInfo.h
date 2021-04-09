#pragma once
#include "AssetInfo.h"
#include "Singleton.hpp"

using namespace std;

namespace Sailor { class ShaderAssetInfo; }

namespace ns
{
	void to_json(json& j, const class Sailor::ShaderAssetInfo& p);
	void from_json(const json& j, class Sailor::ShaderAssetInfo& p);
}

namespace Sailor
{
	class ShaderAssetInfo : public AssetInfo
	{

	public:

		virtual SAILOR_API ~ShaderAssetInfo() = default;

	protected:

		friend void ns::to_json(json& j, const ShaderAssetInfo& p);
		friend void ns::from_json(const json& j, ShaderAssetInfo& p);
	};
	
	class ShaderAssetInfoHandler : public Singleton<ShaderAssetInfoHandler>, public IAssetInfoHandler
	{

	public:

		static SAILOR_API void Initialize();

		virtual SAILOR_API void Serialize(const AssetInfo* inInfo, json& outData) const override;
		virtual SAILOR_API void Deserialize(const json& inData, AssetInfo* outInfo) const override;
				
		virtual SAILOR_API ShaderAssetInfo* ImportAssetInfo(const std::string& assetInfoPath) const override;
		virtual SAILOR_API ShaderAssetInfo* ImportFile(const std::string& filepath) const override;

		virtual SAILOR_API ~ShaderAssetInfoHandler() = default;
	};
}
