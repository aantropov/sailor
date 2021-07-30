#pragma once
#include "AssetInfo.h"
#include "Singleton.hpp"

using namespace std;

namespace Sailor
{
	class ShaderAssetInfo final : public AssetInfo
	{

	public:
		virtual SAILOR_API ~ShaderAssetInfo() = default;
	};
	
	class ShaderAssetInfoHandler final : public Singleton<ShaderAssetInfoHandler>, public IAssetInfoHandler
	{

	public:

		static SAILOR_API void Initialize();

		virtual SAILOR_API void GetDefaultMetaJson(nlohmann::json& outDefaultJson) const;
		virtual SAILOR_API AssetInfo* CreateAssetInfo() const;

		virtual SAILOR_API ~ShaderAssetInfoHandler() = default;
	};
}
