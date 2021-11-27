#pragma once
#include "AssetRegistry/AssetInfo.h"
#include "Core/Singleton.hpp"

using namespace std;

namespace Sailor
{
	class ShaderAssetInfo final : public AssetInfo
	{
	public:
		virtual SAILOR_API ~ShaderAssetInfo() = default;
	};

	using ShaderAssetInfoPtr = ShaderAssetInfo*;

	class ShaderAssetInfoHandler final : public TSingleton<ShaderAssetInfoHandler>, public IAssetInfoHandler
	{

	public:

		static SAILOR_API void Initialize();

		virtual SAILOR_API void GetDefaultMetaJson(nlohmann::json& outDefaultJson) const;
		virtual SAILOR_API AssetInfoPtr CreateAssetInfo() const;

		virtual SAILOR_API ~ShaderAssetInfoHandler() = default;
	};
}
