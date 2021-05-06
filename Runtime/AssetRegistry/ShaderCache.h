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
	class ShaderCache
	{
	public:

		struct ShaderCacheEntry
		{
			UID uid;

			// Last time shader changed
			std::time_t m_timestamp;

			std::vector<std::string> m_compiledPermutations;
		};

		typedef std::unordered_map<UID, ShaderCache::ShaderCacheEntry*> ShaderCacheData;

		static constexpr const char* CacheRootFolder = "..//Cache//";
		static constexpr const char* ShaderCacheFilepath = "..//Cache//ShaderCache.json";
		static constexpr const char* PrecompiledShadersFolder = "..//Cache//PrecompiledShaders//";
		static constexpr const char* CompiledShadersFolder = "..//Cache//CompiledShaders//";
		static constexpr const char* CompiledShaderFileExtension = "spirv";

		SAILOR_API void Load();
		SAILOR_API void ClearAll();
		SAILOR_API void ClearExpired();

	private:

		ShaderCacheData m_cache;

		bool m_bSavePrecompiledShaders = true;
	};
}

namespace ns
{
	SAILOR_API void to_json(json& j, const Sailor::ShaderCache::ShaderCacheEntry& p);
	SAILOR_API void from_json(const json& j, Sailor::ShaderCache::ShaderCacheEntry& p);

	SAILOR_API void to_json(json& j, const Sailor::ShaderCache::ShaderCacheData& p);
	SAILOR_API void from_json(const json& j, Sailor::ShaderCache::ShaderCacheData& p);
}

