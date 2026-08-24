#pragma once

#include "AssetRegistry/AssetInfo.h"
#include "Containers/Vector.h"
#include "Core/Singleton.hpp"

#include <glm/mat4x4.hpp>

#include <cstdint>
#include <filesystem>
#include <string>

namespace Sailor
{
	inline constexpr uint32_t LandscapeVegetationFormatVersion = 1u;
	inline constexpr uint32_t LandscapeVegetationHeaderSize = 64u;
	inline constexpr uint32_t LandscapeVegetationChunkRecordSize = 24u;
	inline constexpr uint32_t LandscapeVegetationInstanceRecordSize = 96u;
	inline constexpr uint32_t LandscapeVegetationEndianMarker = 0x01020304u;
	inline constexpr uint32_t LandscapeVegetationMaxProfiles = 1024u;
	inline constexpr uint64_t LandscapeVegetationMaxInstances =
		16ull * 1024ull * 1024ull;
	inline constexpr uint32_t LandscapeVegetationLocalSpaceMatrices = 1u << 0u;
	inline constexpr uint32_t LandscapeVegetationInstanceEnabled = 1u << 0u;

	struct LandscapeVegetationInstance final
	{
		uint64_t m_stableId = 0u;
		uint32_t m_profileIndex = 0u;
		uint32_t m_flags = LandscapeVegetationInstanceEnabled;
		glm::mat4 m_transform{ 1.0f };
		int32_t m_lodBias = 0;
		float m_cullDistanceScale = 1.0f;
		float m_shadowDistanceScale = 1.0f;

		bool IsEnabled() const
		{
			return (m_flags & LandscapeVegetationInstanceEnabled) != 0u;
		}
	};

	struct LandscapeVegetationChunkData final
	{
		uint32_t m_chunkX = 0u;
		uint32_t m_chunkZ = 0u;
		TVector<LandscapeVegetationInstance> m_instances{};
		TVector<uint32_t> m_enabledInstancesPerProfile{};
	};

	struct LandscapeVegetationAssetData final
	{
		uint32_t m_chunksX = 0u;
		uint32_t m_chunksZ = 0u;
		uint32_t m_profileCount = 0u;
		float m_chunkSize = 0.0f;
		TVector<LandscapeVegetationChunkData> m_chunks{};

		SAILOR_API bool Load(
			const std::filesystem::path& filepath,
			std::string& outDiagnostic);
		SAILOR_API bool Save(
			const std::filesystem::path& filepath,
			std::string& outDiagnostic) const;
		SAILOR_API bool Validate(std::string& outDiagnostic) const;
		SAILOR_API uint64_t GetInstanceCount() const;
		SAILOR_API void RebuildRuntimeIndices();
	};

	class LandscapeVegetationAssetInfo final : public AssetInfo
	{
		SAILOR_REFLECTABLE(LandscapeVegetationAssetInfo)

	public:
		SAILOR_API ~LandscapeVegetationAssetInfo() override = default;

		SAILOR_API YAML::Node Serialize() const override;
		SAILOR_API void Deserialize(const YAML::Node& inData) override;
		SAILOR_API IAssetInfoHandler* GetHandler() override;
	};

	using LandscapeVegetationAssetInfoPtr = LandscapeVegetationAssetInfo*;

	class LandscapeVegetationAssetInfoHandler final :
		public TSubmodule<LandscapeVegetationAssetInfoHandler>,
		public IAssetInfoHandler
	{
	public:
		SAILOR_API explicit LandscapeVegetationAssetInfoHandler(
			class AssetRegistry* assetRegistry);
		SAILOR_API void GetDefaultMeta(YAML::Node& outDefaultYaml) const override;
		SAILOR_API AssetInfoPtr CreateAssetInfo() const override;
	};
}

REFL_AUTO(
	type(Sailor::LandscapeVegetationAssetInfo, bases<Sailor::AssetInfo>),
	field(m_fileId),
	field(m_assetFilename)
)
