#pragma once
#include "Defines.h"
#include <string>
#include <vector>
#include <nlohmann_json/include/nlohmann/json.hpp>
#include "Core/Singleton.hpp"
#include "Core/SharedPtr.hpp"
#include "Core/WeakPtr.hpp"
#include "AssetInfo.h"

namespace Sailor
{
	class MaterialImporter final : public TSingleton<MaterialImporter>, public IAssetInfoHandlerListener
	{
	public:

		static SAILOR_API void Initialize();
		virtual SAILOR_API ~MaterialImporter() override;

		virtual SAILOR_API void OnAssetInfoUpdated(AssetInfo* assetInfo) override;

		bool SAILOR_API LoadMaterial(UID uid);

	private:

	};
}
