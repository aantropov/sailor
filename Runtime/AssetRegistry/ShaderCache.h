#pragma once
#include <chrono>
#include <ctime>
#include <vector>
#include <unordered_map>
#include "UID.h"
#include "Singleton.hpp"
#include "nlohmann_json/include/nlohmann/json.hpp"

namespace Sailor
{
	class ShaderCache
	{
	public:

		class ShaderCacheEntry : IJsonSerializable
		{
		public:

			UID m_UID;

			// Last time shader changed
			std::time_t m_timestamp;

			std::vector<std::string> m_compiledPermutations;

			virtual SAILOR_API void Serialize(nlohmann::json& outData) const = 0;
			virtual SAILOR_API void Deserialize(const nlohmann::json& inData) = 0;
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
