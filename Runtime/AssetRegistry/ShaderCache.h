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

		SAILOR_API void CreatePrecompiledGlsl(const UID& uid, unsigned int permutation, const std::string& vertexGlsl, const std::string& fragmentGlsl);
		SAILOR_API void CacheSpirv(const UID& uid, unsigned int permutation, const std::vector<uint32_t>& vertexSpirv, const std::vector<uint32_t>& fragmentSpirv);
				
		SAILOR_API void Remove(const UID& uid);

		SAILOR_API bool Contains(const UID& uid) const;
		SAILOR_API bool IsExpired(const UID& uid) const;

		SAILOR_API void LoadCache();
		SAILOR_API void SaveCache(bool bForcely = false);

		SAILOR_API void ClearAll();
		SAILOR_API void ClearExpired();
		
		static SAILOR_API std::string GetCachedShaderFilepath(const UID& uid, int permutation, const std::string& shaderKind, bool bIsCompiledToSpirv);
		SAILOR_API std::string LoadSpirv(const UID& uid, unsigned int permutation, enum class EShaderKind shaderKind);

	protected:

		class ShaderCacheEntry final : IJsonSerializable
		{
		public:

			UID m_UID;

			// Last time shader changed
			std::time_t m_timestamp;
			unsigned int m_permutation;

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
		bool m_bIsDirty = false;
	};
}
