#pragma once
#include <chrono>
#include <ctime>
#include "Core/Vector.h"
#include <unordered_map>
#include "AssetRegistry/UID.h"
#include "Core/Singleton.hpp"
#include <nlohmann_json/include/nlohmann/json.hpp>
#include <mutex>
#include <filesystem>

namespace Sailor
{
	class ShaderCache
	{
	public:

		static constexpr const char* CacheRootFolder = "../Cache/";
		static constexpr const char* ShaderCacheFilepath = "../Cache/ShaderCache.json";
		static constexpr const char* PrecompiledShadersFolder = "../Cache/PrecompiledShaders/";
		static constexpr const char* CompiledShadersFolder = "../Cache/CompiledShaders//";
		static constexpr const char* CompiledShadersWithDebugFolder = "../Cache/CompiledShadersWithDebug/";
		static constexpr const char* CompiledShaderFileExtension = "spirv";
		static constexpr const char* PrecompiledShaderFileExtension = "glsl";

		SAILOR_API void Initialize();
		SAILOR_API void Shutdown();

		SAILOR_API void CachePrecompiledGlsl(const UID& uid, uint32_t permutation, const std::string& vertexGlsl, const std::string& fragmentGlsl) const;
		SAILOR_API void CacheSpirvWithDebugInfo(const UID& uid, uint32_t permutation, const TVector<uint32_t>& vertexSpirv, const TVector<uint32_t>& fragmentSpirv) const;
		SAILOR_API void CacheSpirv_ThreadSafe(const UID& uid, uint32_t permutation, const TVector<uint32_t>& vertexSpirv, const TVector<uint32_t>& fragmentSpirv);
		
		SAILOR_API bool GetSpirvCode(const UID& uid, uint32_t permutation, TVector<uint32_t>& vertexSpirv, TVector<uint32_t>& fragmentSpirv, bool bIsDebug = false) const;

		SAILOR_API void Remove(const UID& uid);

		SAILOR_API bool Contains(const UID& uid) const;
		SAILOR_API bool IsExpired(const UID& uid, uint32_t permutation) const;

		SAILOR_API void LoadCache();
		SAILOR_API void SaveCache(bool bForcely = false);

		SAILOR_API void ClearAll();
		SAILOR_API void ClearExpired();

		static SAILOR_API std::filesystem::path GetPrecompiledShaderFilepath(const UID& uid, int32_t permutation, const std::string& shaderKind);
		static SAILOR_API std::filesystem::path GetCachedShaderFilepath(const UID& uid, int32_t permutation, const std::string& shaderKind);
		static SAILOR_API std::filesystem::path GetCachedShaderWithDebugFilepath(const UID& uid, int32_t permutation, const std::string& shaderKind);

	protected:

		class ShaderCacheEntry final : IJsonSerializable
		{
		public:

			UID m_UID;

			// Last time shader changed
			std::time_t m_timestamp;
			uint32_t m_permutation;

			virtual SAILOR_API void Serialize(nlohmann::json& outData) const;
			virtual SAILOR_API void Deserialize(const nlohmann::json& inData);
		};

		class ShaderCacheData final : IJsonSerializable
		{
		public:
			std::unordered_map<UID, TVector<ShaderCache::ShaderCacheEntry*>> m_data;

			virtual SAILOR_API void Serialize(nlohmann::json& outData) const;
			virtual SAILOR_API void Deserialize(const nlohmann::json& inData);
		};

		std::mutex m_saveToCacheMutex;

		SAILOR_API void Remove(const ShaderCache::ShaderCacheEntry* entry);

	private:

		ShaderCacheData m_cache;
		bool m_bIsDirty = false;
		const bool m_bSavePrecompiledGlsl = false;
	};
}
