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
		std::time_t timestamp;
		std::vector<std::string> compiledShaders;
	};

	typedef std::unordered_map<UID, ShaderCacheEntry*> ShaderCacheData;

	class ShaderCache : Singleton<ShaderCache>
	{

	public:

		static constexpr const char* CacheRootFolder = "..//Cache//";
		static constexpr const char* CacheFilePath = "..//Cache//ShaderCache.json";
		static constexpr const char* CompiledShaderFileExtension = "spirv";

		void Initialize();

		void Load();
		void ClearAll();
		void ClearOutdated();

	private:

		ShaderCacheData cache;
	};
}

namespace ns
{
	void to_json(json& j, const Sailor::ShaderCacheEntry& p);
	void from_json(const json& j, Sailor::ShaderCacheEntry& p);

	void to_json(json& j, const Sailor::ShaderCacheData& p);
	void from_json(const json& j, Sailor::ShaderCacheData& p);
}

