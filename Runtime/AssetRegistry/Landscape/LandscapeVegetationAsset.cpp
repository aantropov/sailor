#include "AssetRegistry/Landscape/LandscapeVegetationAsset.h"

#include "AssetRegistry/AssetRegistry.h"
#include "Containers/Set.h"
#include "Math/Math.h"
#include "Workspace/WorkspaceCacheContract.h"

#include <array>
#include <bit>
#include <cmath>
#include <fstream>
#include <limits>

using namespace Sailor;

namespace
{
	constexpr std::array<uint8_t, 8u> Magic = {
		'S', 'L', 'V', 'E', 'G', '0', '0', '1'
	};
	void AppendU32(TVector<uint8_t>& bytes, uint32_t value)
	{
		for (uint32_t byte = 0u; byte < 4u; ++byte)
		{
			bytes.Add(static_cast<uint8_t>(value >> (byte * 8u)));
		}
	}

	void AppendI32(TVector<uint8_t>& bytes, int32_t value)
	{
		AppendU32(bytes, std::bit_cast<uint32_t>(value));
	}

	void AppendU64(TVector<uint8_t>& bytes, uint64_t value)
	{
		for (uint32_t byte = 0u; byte < 8u; ++byte)
		{
			bytes.Add(static_cast<uint8_t>(value >> (byte * 8u)));
		}
	}

	void AppendFloat(TVector<uint8_t>& bytes, float value)
	{
		AppendU32(bytes, std::bit_cast<uint32_t>(value));
	}

	bool ReadU32(
		const TVector<uint8_t>& bytes,
		size_t offset,
		uint32_t& result)
	{
		if (offset > bytes.Num() || bytes.Num() - offset < 4u)
		{
			return false;
		}
		result = 0u;
		for (uint32_t byte = 0u; byte < 4u; ++byte)
		{
			result |= static_cast<uint32_t>(bytes[offset + byte]) << (byte * 8u);
		}
		return true;
	}

	bool ReadI32(
		const TVector<uint8_t>& bytes,
		size_t offset,
		int32_t& result)
	{
		uint32_t value = 0u;
		if (!ReadU32(bytes, offset, value))
		{
			return false;
		}
		result = std::bit_cast<int32_t>(value);
		return true;
	}

	bool ReadU64(
		const TVector<uint8_t>& bytes,
		size_t offset,
		uint64_t& result)
	{
		if (offset > bytes.Num() || bytes.Num() - offset < 8u)
		{
			return false;
		}
		result = 0u;
		for (uint32_t byte = 0u; byte < 8u; ++byte)
		{
			result |= static_cast<uint64_t>(bytes[offset + byte]) << (byte * 8u);
		}
		return true;
	}

	bool ReadFloat(
		const TVector<uint8_t>& bytes,
		size_t offset,
		float& result)
	{
		uint32_t value = 0u;
		if (!ReadU32(bytes, offset, value))
		{
			return false;
		}
		result = std::bit_cast<float>(value);
		return true;
	}

	bool TryCalculateFileSize(
		uint32_t chunkCount,
		uint64_t instanceCount,
		uint64_t& result)
	{
		if (instanceCount > LandscapeVegetationMaxInstances)
		{
			return false;
		}
		constexpr uint64_t header = LandscapeVegetationHeaderSize;
		const uint64_t chunks = static_cast<uint64_t>(chunkCount) *
			LandscapeVegetationChunkRecordSize;
		const uint64_t instances = instanceCount *
			LandscapeVegetationInstanceRecordSize;
		result = header + chunks + instances;
		return result >= header && result >= chunks && result >= instances &&
			result <= static_cast<uint64_t>((std::numeric_limits<size_t>::max)());
	}

	bool IsFiniteMatrix(const glm::mat4& matrix)
	{
		return Math::AllFinite(matrix[0]) &&
			Math::AllFinite(matrix[1]) &&
			Math::AllFinite(matrix[2]) &&
			Math::AllFinite(matrix[3]);
	}

	bool IsAffineMatrix(const glm::mat4& matrix)
	{
		constexpr float epsilon = 1.0e-5f;
		return std::abs(matrix[0].w) <= epsilon &&
			std::abs(matrix[1].w) <= epsilon &&
			std::abs(matrix[2].w) <= epsilon &&
			std::abs(matrix[3].w - 1.0f) <= epsilon;
	}

	bool Fail(std::string message, std::string& outDiagnostic)
	{
		outDiagnostic = std::move(message);
		return false;
	}
}

uint64_t LandscapeVegetationAssetData::GetInstanceCount() const
{
	uint64_t result = 0u;
	for (const auto& chunk : m_chunks)
	{
		result += static_cast<uint64_t>(chunk.m_instances.Num());
	}
	return result;
}

void LandscapeVegetationAssetData::RebuildRuntimeIndices()
{
	for (auto& chunk : m_chunks)
	{
		chunk.m_enabledInstancesPerProfile.Clear();
		chunk.m_enabledInstancesPerProfile.Resize(m_profileCount);
		for (const auto& instance : chunk.m_instances)
		{
			if (instance.IsEnabled() && instance.m_profileIndex < m_profileCount)
			{
				++chunk.m_enabledInstancesPerProfile[instance.m_profileIndex];
			}
		}
	}
}

bool LandscapeVegetationAssetData::Validate(std::string& outDiagnostic) const
{
	outDiagnostic.clear();
	if (m_chunksX == 0u || m_chunksZ == 0u ||
		m_chunksX > 64u || m_chunksZ > 64u)
	{
		return Fail("Landscape vegetation dimensions must be in the 1..64 range.", outDiagnostic);
	}
	if (!std::isfinite(m_chunkSize) || m_chunkSize <= 0.0f)
	{
		return Fail("Landscape vegetation chunk size must be finite and positive.", outDiagnostic);
	}
	if (m_profileCount > LandscapeVegetationMaxProfiles)
	{
		return Fail("Landscape vegetation profile count exceeds the supported limit.", outDiagnostic);
	}
	const uint64_t expectedChunkCount =
		static_cast<uint64_t>(m_chunksX) * m_chunksZ;
	if (m_chunks.Num() != expectedChunkCount)
	{
		return Fail("Landscape vegetation must contain exactly one record per landscape chunk.", outDiagnostic);
	}
	if (GetInstanceCount() > LandscapeVegetationMaxInstances)
	{
		return Fail("Landscape vegetation exceeds the supported instance count.", outDiagnostic);
	}

	TSet<uint64_t> stableIds;
	const float landscapeWidth = m_chunksX * m_chunkSize;
	const float landscapeDepth = m_chunksZ * m_chunkSize;
	const float placementEpsilon = (std::max)(0.001f, m_chunkSize * 0.0001f);
	for (size_t chunkIndex = 0u; chunkIndex < m_chunks.Num(); ++chunkIndex)
	{
		const auto& chunk = m_chunks[chunkIndex];
		const uint32_t expectedX = static_cast<uint32_t>(chunkIndex % m_chunksX);
		const uint32_t expectedZ = static_cast<uint32_t>(chunkIndex / m_chunksX);
		if (chunk.m_chunkX != expectedX || chunk.m_chunkZ != expectedZ)
		{
			return Fail("Landscape vegetation chunks must be stored in canonical Z-major order.", outDiagnostic);
		}
		for (const auto& instance : chunk.m_instances)
		{
			if (instance.m_stableId == 0u || stableIds.Contains(instance.m_stableId))
			{
				return Fail("Landscape vegetation stable IDs must be non-zero and unique.", outDiagnostic);
			}
			stableIds.Insert(instance.m_stableId);
			if (instance.m_profileIndex >= m_profileCount)
			{
				return Fail("Landscape vegetation contains an invalid profile index.", outDiagnostic);
			}
			if ((instance.m_flags & ~LandscapeVegetationInstanceEnabled) != 0u)
			{
				return Fail("Landscape vegetation contains unsupported instance flags.", outDiagnostic);
			}
			if (!IsFiniteMatrix(instance.m_transform))
			{
				return Fail("Landscape vegetation contains a non-finite instance matrix.", outDiagnostic);
			}
			if (!IsAffineMatrix(instance.m_transform))
			{
				return Fail("Landscape vegetation instance matrices must be affine transforms.", outDiagnostic);
			}
			const float chunkMinX = chunk.m_chunkX * m_chunkSize - landscapeWidth * 0.5f;
			const float chunkMinZ = chunk.m_chunkZ * m_chunkSize - landscapeDepth * 0.5f;
			const glm::vec3 position(instance.m_transform[3]);
			if (position.x < chunkMinX - placementEpsilon ||
				position.x > chunkMinX + m_chunkSize + placementEpsilon ||
				position.z < chunkMinZ - placementEpsilon ||
				position.z > chunkMinZ + m_chunkSize + placementEpsilon)
			{
				return Fail("Landscape vegetation instance origins must belong to their owning chunk.", outDiagnostic);
			}
			if (!std::isfinite(instance.m_cullDistanceScale) ||
				instance.m_cullDistanceScale <= 0.0f ||
				!std::isfinite(instance.m_shadowDistanceScale) ||
				instance.m_shadowDistanceScale <= 0.0f)
			{
				return Fail("Landscape vegetation distance scales must be finite and positive.", outDiagnostic);
			}
		}
	}
	return true;
}

bool LandscapeVegetationAssetData::Save(
	const std::filesystem::path& filepath,
	std::string& outDiagnostic) const
{
	if (!Validate(outDiagnostic))
	{
		return false;
	}

	const uint64_t instanceCount = GetInstanceCount();
	uint64_t fileSize = 0u;
	if (!TryCalculateFileSize(
		static_cast<uint32_t>(m_chunks.Num()),
		instanceCount,
		fileSize))
	{
		return Fail("Landscape vegetation file size is unsupported.", outDiagnostic);
	}

	TVector<uint8_t> bytes;
	bytes.Reserve(static_cast<size_t>(fileSize));
	for (uint8_t byte : Magic)
	{
		bytes.Add(byte);
	}
	AppendU32(bytes, LandscapeVegetationFormatVersion);
	AppendU32(bytes, LandscapeVegetationEndianMarker);
	AppendU32(bytes, LandscapeVegetationHeaderSize);
	AppendU32(bytes, LandscapeVegetationChunkRecordSize);
	AppendU32(bytes, LandscapeVegetationInstanceRecordSize);
	AppendU32(bytes, m_chunksX);
	AppendU32(bytes, m_chunksZ);
	AppendU32(bytes, m_profileCount);
	AppendU32(bytes, static_cast<uint32_t>(m_chunks.Num()));
	AppendU64(bytes, instanceCount);
	AppendFloat(bytes, m_chunkSize);
	AppendU32(bytes, LandscapeVegetationLocalSpaceMatrices);
	AppendU32(bytes, 0u);

	uint64_t firstInstance = 0u;
	for (const auto& chunk : m_chunks)
	{
		AppendU32(bytes, chunk.m_chunkX);
		AppendU32(bytes, chunk.m_chunkZ);
		AppendU64(bytes, firstInstance);
		AppendU32(bytes, static_cast<uint32_t>(chunk.m_instances.Num()));
		AppendU32(bytes, 0u);
		firstInstance += static_cast<uint64_t>(chunk.m_instances.Num());
	}
	for (const auto& chunk : m_chunks)
	{
		for (const auto& instance : chunk.m_instances)
		{
			AppendU64(bytes, instance.m_stableId);
			AppendU32(bytes, instance.m_profileIndex);
			AppendU32(bytes, instance.m_flags);
			for (glm::length_t column = 0; column < 4; ++column)
			{
				for (glm::length_t row = 0; row < 4; ++row)
				{
					AppendFloat(bytes, instance.m_transform[column][row]);
				}
			}
			AppendI32(bytes, instance.m_lodBias);
			AppendFloat(bytes, instance.m_cullDistanceScale);
			AppendFloat(bytes, instance.m_shadowDistanceScale);
			AppendU32(bytes, 0u);
		}
	}
	if (bytes.Num() != fileSize)
	{
		return Fail("Landscape vegetation writer produced an unexpected byte count.", outDiagnostic);
	}

	return Workspace::AtomicReplaceWorkspaceCacheBinary(
		filepath,
		bytes.GetData(),
		static_cast<uint64_t>(bytes.Num()),
		outDiagnostic);
}

bool LandscapeVegetationAssetData::Load(
	const std::filesystem::path& filepath,
	std::string& outDiagnostic)
{
	outDiagnostic.clear();
	std::error_code fileError;
	const uint64_t fileSize = std::filesystem::file_size(filepath, fileError);
	if (fileError)
	{
		return Fail("Cannot inspect landscape vegetation file: " + fileError.message() + ".", outDiagnostic);
	}
	if (fileSize < LandscapeVegetationHeaderSize ||
		fileSize > static_cast<uint64_t>((std::numeric_limits<size_t>::max)()))
	{
		return Fail("Landscape vegetation file size is invalid.", outDiagnostic);
	}

	TVector<uint8_t> bytes;
	bytes.Resize(static_cast<size_t>(fileSize));
	std::ifstream stream(filepath, std::ios::binary);
	if (!stream.is_open())
	{
		return Fail("Cannot open landscape vegetation file.", outDiagnostic);
	}
	stream.read(
		reinterpret_cast<char*>(bytes.GetData()),
		static_cast<std::streamsize>(bytes.Num()));
	if (!stream || stream.gcount() != static_cast<std::streamsize>(bytes.Num()))
	{
		return Fail("Cannot read the complete landscape vegetation file.", outDiagnostic);
	}
	for (size_t index = 0u; index < Magic.size(); ++index)
	{
		if (bytes[index] != Magic[index])
		{
			return Fail("Landscape vegetation magic is invalid.", outDiagnostic);
		}
	}

	uint32_t version = 0u;
	uint32_t endianMarker = 0u;
	uint32_t headerSize = 0u;
	uint32_t chunkRecordSize = 0u;
	uint32_t instanceRecordSize = 0u;
	uint32_t chunksX = 0u;
	uint32_t chunksZ = 0u;
	uint32_t profileCount = 0u;
	uint32_t chunkCount = 0u;
	uint64_t instanceCount = 0u;
	float chunkSize = 0.0f;
	uint32_t flags = 0u;
	uint32_t reserved = 0u;
	if (!ReadU32(bytes, 8u, version) ||
		!ReadU32(bytes, 12u, endianMarker) ||
		!ReadU32(bytes, 16u, headerSize) ||
		!ReadU32(bytes, 20u, chunkRecordSize) ||
		!ReadU32(bytes, 24u, instanceRecordSize) ||
		!ReadU32(bytes, 28u, chunksX) ||
		!ReadU32(bytes, 32u, chunksZ) ||
		!ReadU32(bytes, 36u, profileCount) ||
		!ReadU32(bytes, 40u, chunkCount) ||
		!ReadU64(bytes, 44u, instanceCount) ||
		!ReadFloat(bytes, 52u, chunkSize) ||
		!ReadU32(bytes, 56u, flags) ||
		!ReadU32(bytes, 60u, reserved))
	{
		return Fail("Landscape vegetation header is truncated.", outDiagnostic);
	}
	if (version != LandscapeVegetationFormatVersion)
	{
		return Fail("Unsupported landscape vegetation format version.", outDiagnostic);
	}
	if (endianMarker != LandscapeVegetationEndianMarker)
	{
		return Fail("Landscape vegetation endian marker is invalid.", outDiagnostic);
	}
	if (headerSize != LandscapeVegetationHeaderSize ||
		chunkRecordSize != LandscapeVegetationChunkRecordSize ||
		instanceRecordSize != LandscapeVegetationInstanceRecordSize)
	{
		return Fail("Landscape vegetation record sizes are incompatible with version 1.", outDiagnostic);
	}
	if (flags != LandscapeVegetationLocalSpaceMatrices || reserved != 0u)
	{
		return Fail("Landscape vegetation version 1 header flags or reserved bytes are invalid.", outDiagnostic);
	}
	if (chunksX == 0u || chunksZ == 0u ||
		chunksX > 64u || chunksZ > 64u ||
		static_cast<uint64_t>(chunksX) * chunksZ != chunkCount)
	{
		return Fail("Landscape vegetation chunk dimensions do not match its table.", outDiagnostic);
	}
	if (profileCount > LandscapeVegetationMaxProfiles)
	{
		return Fail("Landscape vegetation profile count exceeds the supported limit.", outDiagnostic);
	}
	uint64_t expectedFileSize = 0u;
	if (!TryCalculateFileSize(chunkCount, instanceCount, expectedFileSize) ||
		expectedFileSize != fileSize)
	{
		return Fail("Landscape vegetation file length does not match its header.", outDiagnostic);
	}

	LandscapeVegetationAssetData loaded;
	loaded.m_chunksX = chunksX;
	loaded.m_chunksZ = chunksZ;
	loaded.m_profileCount = profileCount;
	loaded.m_chunkSize = chunkSize;
	loaded.m_chunks.Resize(chunkCount);
	const uint64_t instancesOffset = LandscapeVegetationHeaderSize +
		static_cast<uint64_t>(chunkCount) * LandscapeVegetationChunkRecordSize;
	uint64_t expectedFirstInstance = 0u;
	for (uint32_t chunkIndex = 0u; chunkIndex < chunkCount; ++chunkIndex)
	{
		const size_t chunkOffset = LandscapeVegetationHeaderSize +
			static_cast<size_t>(chunkIndex) * LandscapeVegetationChunkRecordSize;
		uint32_t chunkX = 0u;
		uint32_t chunkZ = 0u;
		uint64_t firstInstance = 0u;
		uint32_t numInstances = 0u;
		uint32_t chunkReserved = 0u;
		if (!ReadU32(bytes, chunkOffset, chunkX) ||
			!ReadU32(bytes, chunkOffset + 4u, chunkZ) ||
			!ReadU64(bytes, chunkOffset + 8u, firstInstance) ||
			!ReadU32(bytes, chunkOffset + 16u, numInstances) ||
			!ReadU32(bytes, chunkOffset + 20u, chunkReserved))
		{
			return Fail("Landscape vegetation chunk table is truncated.", outDiagnostic);
		}
		if (chunkX != chunkIndex % chunksX ||
			chunkZ != chunkIndex / chunksX ||
			firstInstance != expectedFirstInstance ||
			firstInstance > instanceCount ||
			static_cast<uint64_t>(numInstances) > instanceCount - firstInstance ||
			chunkReserved != 0u)
		{
			return Fail("Landscape vegetation chunk table is not canonical.", outDiagnostic);
		}

		auto& chunk = loaded.m_chunks[chunkIndex];
		chunk.m_chunkX = chunkX;
		chunk.m_chunkZ = chunkZ;
		chunk.m_instances.Resize(numInstances);
		for (uint32_t localInstance = 0u; localInstance < numInstances; ++localInstance)
		{
			const uint64_t globalInstance = firstInstance + localInstance;
			const size_t instanceOffset = static_cast<size_t>(instancesOffset +
				globalInstance * LandscapeVegetationInstanceRecordSize);
			auto& instance = chunk.m_instances[localInstance];
			uint32_t instanceReserved = 0u;
			if (!ReadU64(bytes, instanceOffset, instance.m_stableId) ||
				!ReadU32(bytes, instanceOffset + 8u, instance.m_profileIndex) ||
				!ReadU32(bytes, instanceOffset + 12u, instance.m_flags) ||
				!ReadU32(bytes, instanceOffset + 92u, instanceReserved))
			{
				return Fail("Landscape vegetation instance record is truncated.", outDiagnostic);
			}
			if (instanceReserved != 0u)
			{
				return Fail("Landscape vegetation instance reserved bytes are invalid.", outDiagnostic);
			}
			size_t matrixOffset = instanceOffset + 16u;
			for (glm::length_t column = 0; column < 4; ++column)
			{
				for (glm::length_t row = 0; row < 4; ++row)
				{
					if (!ReadFloat(bytes, matrixOffset, instance.m_transform[column][row]))
					{
						return Fail("Landscape vegetation matrix is truncated.", outDiagnostic);
					}
					matrixOffset += sizeof(float);
				}
			}
			if (!ReadI32(bytes, instanceOffset + 80u, instance.m_lodBias) ||
				!ReadFloat(bytes, instanceOffset + 84u, instance.m_cullDistanceScale) ||
				!ReadFloat(bytes, instanceOffset + 88u, instance.m_shadowDistanceScale))
			{
				return Fail("Landscape vegetation per-instance settings are truncated.", outDiagnostic);
			}
		}
		expectedFirstInstance += numInstances;
	}
	if (expectedFirstInstance != instanceCount || !loaded.Validate(outDiagnostic))
	{
		return false;
	}
	loaded.RebuildRuntimeIndices();

	*this = std::move(loaded);
	outDiagnostic = "Landscape vegetation loaded.";
	return true;
}

YAML::Node LandscapeVegetationAssetInfo::Serialize() const
{
	return SerializeReflectedAssetInfo(*this);
}

void LandscapeVegetationAssetInfo::Deserialize(const YAML::Node& inData)
{
	DeserializeReflectedAssetInfo(*this, inData);
}

IAssetInfoHandler* LandscapeVegetationAssetInfo::GetHandler()
{
	return App::GetSubmodule<LandscapeVegetationAssetInfoHandler>();
}

LandscapeVegetationAssetInfoHandler::LandscapeVegetationAssetInfoHandler(
	AssetRegistry* assetRegistry)
{
	m_supportedExtensions.Emplace("vegetation");
	assetRegistry->RegisterAssetInfoHandler(m_supportedExtensions, this);
}

void LandscapeVegetationAssetInfoHandler::GetDefaultMeta(
	YAML::Node& outDefaultYaml) const
{
	LandscapeVegetationAssetInfo defaultObject;
	outDefaultYaml = defaultObject.Serialize();
}

AssetInfoPtr LandscapeVegetationAssetInfoHandler::CreateAssetInfo() const
{
	return new LandscapeVegetationAssetInfo();
}
