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

		static constexpr const char* CacheRootFolder = "..//Cache//";
		static constexpr const char* ShaderCacheFilepath = "..//Cache//ShaderCache.json";
		static constexpr const char* PrecompiledShadersFolder = "..//Cache//PrecompiledShaders//";
		static constexpr const char* CompiledShadersFolder = "..//Cache//CompiledShaders//";
		static constexpr const char* CompiledShaderFileExtension = "spirv";
		static constexpr const char* PrecompiledShaderFileExtension = "glsl";

		SAILOR_API void Initialize();
		SAILOR_API void Shutdown();

		SAILOR_API void AddSpirv(const UID& uid, unsigned int permutation);
		SAILOR_API std::string GetSpirv(const UID& uid, unsigned int permutation);
		
		SAILOR_API void Remove(const UID& uid);

		SAILOR_API bool Contains(UID uid) const;
		SAILOR_API void LoadCache();
		SAILOR_API void ClearAll();
		SAILOR_API void ClearExpired();

	protected:

		class ShaderCacheEntry final : IJsonSerializable
		{
		public:

			UID m_UID;

			// Last time shader changed
			std::time_t m_timestamp;
			unsigned int m_permutation;

			std::string GetCompiledFilepath() const;
			std::string GetPrecompiledFilepath() const;

			virtual SAILOR_API void Serialize(nlohmann::json& outData) const;
			virtual SAILOR_API void Deserialize(const nlohmann::json& inData);
		};

		class ShaderCacheData final : IJsonSerializable
		{
		public:
			std::unordered_map<UID, std::vector<ShaderCache::ShaderCacheEntry*>> m_data;

			virtual SAILOR_API void Serialize(nlohmann::json& outData) const;
			virtual SAILOR_API void Deserialize(const nlohmann::json& inData);
		};

	private:

		ShaderCacheData m_cache;

		bool m_bSavePrecompiledShaders = true;
	};
}
