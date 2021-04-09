#pragma once
#include <chrono>
#include <ctime>
#include <vector>
#include <unordered_map>
#include "UID.h"
#include "Singleton.hpp"
#include "nlohmann_json/include/nlohmann/json.hpp"

namespace Sailor { class ShaderAssetInfo; }

using namespace std;
using namespace Sailor;

namespace Sailor
{
	struct ShaderCacheEntry
	{
		// Last time shader changed
		std::time_t timestamp;

		std::vector<std::string> compiledShaders;
	};

	typedef std::unordered_map<UID, ShaderCacheEntry*> ShaderCacheData;

	class ShaderCache : Singleton<ShaderCache>
	{

	public:

		static constexpr const char* CacheRootFolder = "..//Cache//";
		static constexpr const char* Cachefilepath = "..//Cache//ShaderCache.json";
		static constexpr const char* CachPrecompiledFolder = "..//Cache//";
		static constexpr const char* CompiledShaderFileExtension = "spirv";

		SAILOR_API void Initialize();

		SAILOR_API void Load();
		SAILOR_API void ClearAll();
		SAILOR_API void ClearExpired();

	private:

		ShaderCacheData cache;

		bool bSavePrecompiledShaders = true;
	};
}

namespace ns
{
	SAILOR_API void to_json(json& j, const Sailor::ShaderCacheEntry& p);
	SAILOR_API void from_json(const json& j, Sailor::ShaderCacheEntry& p);

	SAILOR_API void to_json(json& j, const Sailor::ShaderCacheData& p);
	SAILOR_API void from_json(const json& j, Sailor::ShaderCacheData& p);
}

