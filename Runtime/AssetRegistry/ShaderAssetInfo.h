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

		virtual ~ShaderAssetInfo() = default;

	protected:

		friend void ns::to_json(json& j, const ShaderAssetInfo& p);
		friend void ns::from_json(const json& j, ShaderAssetInfo& p);
		friend class IAssetInfoHandler;
	};
	
	class ShaderAssetInfoHandler : public Singleton<ShaderAssetInfoHandler>, public IAssetInfoHandler
	{

	public:

		static void Initialize();

		virtual void Serialize(const AssetInfo* inInfo, json& outData) const override;
		virtual void Deserialize(const json& inData, AssetInfo* outInfo) const override;

		virtual ShaderAssetInfo* ImportAssetInfo(const std::string& assetInfoPath) const override;
		virtual ShaderAssetInfo* ImportFile(const std::string& filePath) const override;

		virtual ~ShaderAssetInfoHandler() = default;
	};
}
