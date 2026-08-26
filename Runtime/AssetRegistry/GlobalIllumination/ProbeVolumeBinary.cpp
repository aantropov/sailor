#include "AssetRegistry/GlobalIllumination/ProbeVolumeBinary.h"

#include "Workspace/WorkspaceCacheContract.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <fstream>
#include <limits>

using namespace Sailor;

namespace
{
	constexpr std::array<uint8_t, 8u> Magic{
		'S', 'L', 'R', 'P', 'R', 'O', 'B', 'E'
	};
	constexpr uint32_t EndianMarker = 0x01020304u;
	constexpr uint32_t FixedHeaderSize = 40u;
	constexpr uint32_t OneBakedState = 1u;
	constexpr uint32_t MaxStringBytes = 1024u * 1024u;
	constexpr uint32_t MaxProbeCount = 16u * 1024u * 1024u;
	constexpr uint32_t MaxBrickCount = 1024u * 1024u;
	constexpr uint64_t MaxPayloadBytes = 8ull * 1024ull * 1024ull * 1024ull;

	uint64_t Checksum(const uint8_t* bytes, size_t size) noexcept
	{
		uint64_t hash = 1469598103934665603ull;
		for (size_t index = 0u; index < size; ++index)
		{
			hash ^= bytes[index];
			hash *= 1099511628211ull;
		}
		return hash;
	}

	class Writer final
	{
	public:
		void WriteU8(uint8_t value) { m_bytes.Add(value); }

		void WriteU32(uint32_t value)
		{
			for (uint32_t shift = 0u; shift < 32u; shift += 8u)
			{
				WriteU8(static_cast<uint8_t>((value >> shift) & 0xffu));
			}
		}

		void WriteU64(uint64_t value)
		{
			for (uint32_t shift = 0u; shift < 64u; shift += 8u)
			{
				WriteU8(static_cast<uint8_t>((value >> shift) & 0xffull));
			}
		}

		void WriteFloat(float value)
		{
			WriteU32(std::bit_cast<uint32_t>(value));
		}

		void WriteVec2(const glm::vec2& value)
		{
			WriteFloat(value.x);
			WriteFloat(value.y);
		}

		void WriteVec3(const glm::vec3& value)
		{
			WriteFloat(value.x);
			WriteFloat(value.y);
			WriteFloat(value.z);
		}

		bool WriteString(const std::string& value, std::string& outDiagnostic)
		{
			if (value.size() > MaxStringBytes ||
				value.size() > std::numeric_limits<uint32_t>::max())
			{
				outDiagnostic = "a probe-volume metadata string exceeds the format limit";
				return false;
			}
			WriteU32(static_cast<uint32_t>(value.size()));
			m_bytes.AddRange(
				reinterpret_cast<const uint8_t*>(value.data()),
				value.size());
			return true;
		}

		TVector<uint8_t> m_bytes{};
	};

	class Reader final
	{
	public:
		Reader(const uint8_t* bytes, size_t size) :
			m_bytes(bytes),
			m_size(size)
		{}

		bool ReadU8(uint8_t& value)
		{
			if (!CanRead(1u)) return false;
			value = m_bytes[m_offset++];
			return true;
		}

		bool ReadU32(uint32_t& value)
		{
			if (!CanRead(4u)) return false;
			value = 0u;
			for (uint32_t shift = 0u; shift < 32u; shift += 8u)
			{
				value |= static_cast<uint32_t>(m_bytes[m_offset++]) << shift;
			}
			return true;
		}

		bool ReadU64(uint64_t& value)
		{
			if (!CanRead(8u)) return false;
			value = 0u;
			for (uint32_t shift = 0u; shift < 64u; shift += 8u)
			{
				value |= static_cast<uint64_t>(m_bytes[m_offset++]) << shift;
			}
			return true;
		}

		bool ReadFloat(float& value)
		{
			uint32_t bits = 0u;
			if (!ReadU32(bits)) return false;
			value = std::bit_cast<float>(bits);
			return true;
		}

		bool ReadVec2(glm::vec2& value)
		{
			return ReadFloat(value.x) && ReadFloat(value.y);
		}

		bool ReadVec3(glm::vec3& value)
		{
			return ReadFloat(value.x) &&
				ReadFloat(value.y) &&
				ReadFloat(value.z);
		}

		bool ReadString(std::string& value)
		{
			uint32_t size = 0u;
			if (!ReadU32(size) || size > MaxStringBytes || !CanRead(size))
			{
				return false;
			}
			value.assign(
				reinterpret_cast<const char*>(m_bytes + m_offset),
				size);
			m_offset += size;
			return true;
		}

		bool CanRead(size_t size) const noexcept
		{
			return size <= m_size - (std::min)(m_offset, m_size);
		}

		size_t Remaining() const noexcept
		{
			return m_size - (std::min)(m_offset, m_size);
		}

	private:
		const uint8_t* m_bytes = nullptr;
		size_t m_size = 0u;
		size_t m_offset = 0u;
	};

	ProbeVolumeBinaryResult Fail(
		EProbeVolumeBinaryStatus status,
		std::string diagnostic)
	{
		ProbeVolumeBinaryResult result;
		result.m_status = status;
		result.m_diagnostic = std::move(diagnostic);
		return result;
	}

	bool ReadBakeSettings(Reader& reader, ProbeVolumeBakeSettings& settings)
	{
		uint32_t flags = 0u;
		if (!reader.ReadU32(settings.m_raysPerProbe) ||
			!reader.ReadU32(settings.m_bounceCount) ||
			!reader.ReadU32(settings.m_randomSeed) ||
			!reader.ReadU32(settings.m_maxSubdivisionLevel) ||
			!reader.ReadFloat(settings.m_minProbeSpacing) ||
			!reader.ReadFloat(settings.m_normalBias) ||
			!reader.ReadFloat(settings.m_viewBias) ||
			!reader.ReadFloat(settings.m_maxRayDistance) ||
			!reader.ReadFloat(settings.m_skyIndirectIntensity) ||
			!reader.ReadU32(flags))
		{
			return false;
		}
		settings.m_bIncludeSky = (flags & (1u << 0u)) != 0u;
		settings.m_bIncludeEmissive = (flags & (1u << 1u)) != 0u;
		settings.m_bIncludeDirectLighting = (flags & (1u << 2u)) != 0u;
		return (flags & ~0x7u) == 0u;
	}

	void WriteBakeSettings(Writer& writer, const ProbeVolumeBakeSettings& settings)
	{
		writer.WriteU32(settings.m_raysPerProbe);
		writer.WriteU32(settings.m_bounceCount);
		writer.WriteU32(settings.m_randomSeed);
		writer.WriteU32(settings.m_maxSubdivisionLevel);
		writer.WriteFloat(settings.m_minProbeSpacing);
		writer.WriteFloat(settings.m_normalBias);
		writer.WriteFloat(settings.m_viewBias);
		writer.WriteFloat(settings.m_maxRayDistance);
		writer.WriteFloat(settings.m_skyIndirectIntensity);
		uint32_t flags = 0u;
		flags |= settings.m_bIncludeSky ? 1u << 0u : 0u;
		flags |= settings.m_bIncludeEmissive ? 1u << 1u : 0u;
		flags |= settings.m_bIncludeDirectLighting ? 1u << 2u : 0u;
		writer.WriteU32(flags);
	}
}

bool ProbeVolumeBinary::Serialize(
	const ProbeVolumeData& source,
	TVector<uint8_t>& outBytes,
	std::string& outDiagnostic) noexcept
{
	outBytes.Clear();
	outDiagnostic.clear();
	try
	{
		ProbeVolumeData data = source;
		data.m_formatVersion = ProbeVolumeFormatVersion;
		data.m_layoutHash = ComputeProbeVolumeLayoutHash(data);
		data.m_representationHash = ComputeProbeVolumeRepresentationHash(
			data.m_formatVersion,
			data.m_shOrder,
			data.m_compression);
		if (!data.Validate(outDiagnostic))
		{
			return false;
		}
		if (data.m_transportHash == 0u || data.m_lightingHash == 0u)
		{
			outDiagnostic =
				"the probe volume is missing transport or lighting identity";
			return false;
		}
		if (data.m_bricks.Num() > MaxBrickCount ||
			data.m_probes.Num() > MaxProbeCount)
		{
			outDiagnostic = "the probe volume exceeds the binary format limits";
			return false;
		}

		Writer payload;
		payload.m_bytes.Reserve(
			256u + data.m_bricks.Num() * 48u + data.m_probes.Num() * 212u);
		payload.WriteU32(OneBakedState);
		payload.WriteU32(data.m_shOrder);
		payload.WriteU32(static_cast<uint32_t>(data.m_compression));
		payload.WriteU32(0u);
		payload.WriteU64(data.m_layoutHash);
		payload.WriteU64(data.m_representationHash);
		payload.WriteU64(data.m_transportHash);
		payload.WriteU64(data.m_lightingHash);
		payload.WriteU64(data.m_sourceWorldHash);
		payload.WriteVec3(data.m_volumeMin);
		payload.WriteVec3(data.m_volumeMax);
		WriteBakeSettings(payload, data.m_bakeSettings);
		payload.WriteU32(static_cast<uint32_t>(data.m_bricks.Num()));
		payload.WriteU32(static_cast<uint32_t>(data.m_probes.Num()));
		payload.WriteU32(data.m_diagnostics.m_invalidProbeCount);
		payload.WriteU32(data.m_diagnostics.m_relocatedProbeCount);
		payload.WriteFloat(data.m_diagnostics.m_averageValidity);
		payload.WriteFloat(data.m_diagnostics.m_bakeDurationSeconds);
		if (!payload.WriteString(data.m_stateName, outDiagnostic) ||
			!payload.WriteString(data.m_bakerVersion, outDiagnostic) ||
			!payload.WriteString(data.m_diagnostics.m_message, outDiagnostic))
		{
			return false;
		}

		for (const ProbeVolumeBrick& brick : data.m_bricks)
		{
			payload.WriteVec3(brick.m_min);
			payload.WriteVec3(brick.m_max);
			payload.WriteU32(brick.m_subdivisionLevel);
			payload.WriteU32(brick.m_firstProbeIndex);
			payload.WriteU32(brick.m_probeCount);
			payload.WriteU32(brick.m_probeCounts.x);
			payload.WriteU32(brick.m_probeCounts.y);
			payload.WriteU32(brick.m_probeCounts.z);
		}
		for (const ProbeVolumeSample& probe : data.m_probes)
		{
			payload.WriteVec3(probe.m_position);
			payload.WriteVec3(probe.m_relocationOffset);
			payload.WriteFloat(probe.m_validity);
			payload.WriteU32(probe.m_flags);
			for (const glm::vec3& coefficient : probe.m_irradiance)
			{
				payload.WriteVec3(coefficient);
			}
			for (const glm::vec2& visibility : probe.m_visibility)
			{
				payload.WriteVec2(visibility);
			}
			for (const float environmentVisibility :
				probe.m_environmentVisibility)
			{
				payload.WriteFloat(environmentVisibility);
			}
		}

		if (payload.m_bytes.Num() > MaxPayloadBytes)
		{
			outDiagnostic = "the serialized probe volume exceeds the maximum payload size";
			return false;
		}

		Writer file;
		file.m_bytes.Reserve(FixedHeaderSize + payload.m_bytes.Num());
		file.m_bytes.AddRange(Magic.data(), Magic.size());
		file.WriteU32(ProbeVolumeFormatVersion);
		file.WriteU32(EndianMarker);
		file.WriteU32(FixedHeaderSize);
		file.WriteU32(0u);
		file.WriteU64(payload.m_bytes.Num());
		file.WriteU64(Checksum(payload.m_bytes.GetData(), payload.m_bytes.Num()));
		file.m_bytes.AddRange(payload.m_bytes);
		outBytes = std::move(file.m_bytes);
		outDiagnostic = "serialized one baked probe-volume state";
		return true;
	}
	catch (const std::exception& exception)
	{
		outDiagnostic = std::string("cannot serialize probe volume: ") + exception.what();
		outBytes.Clear();
		return false;
	}
	catch (...)
	{
		outDiagnostic = "cannot serialize probe volume: unknown failure";
		outBytes.Clear();
		return false;
	}
}

ProbeVolumeBinaryResult ProbeVolumeBinary::Deserialize(
	const uint8_t* bytes,
	size_t size) noexcept
{
	try
	{
		if (!bytes || size < FixedHeaderSize)
		{
			return Fail(EProbeVolumeBinaryStatus::Truncated,
				"the .probes file is smaller than its fixed header");
		}
		if (!std::equal(Magic.begin(), Magic.end(), bytes))
		{
			return Fail(EProbeVolumeBinaryStatus::InvalidMagic,
				"the .probes file has an invalid magic value");
		}

		Reader header(bytes + Magic.size(), size - Magic.size());
		uint32_t version = 0u;
		uint32_t endian = 0u;
		uint32_t headerSize = 0u;
		uint32_t headerFlags = 0u;
		uint64_t payloadSize = 0u;
		uint64_t payloadChecksum = 0u;
		if (!header.ReadU32(version) ||
			!header.ReadU32(endian) ||
			!header.ReadU32(headerSize) ||
			!header.ReadU32(headerFlags) ||
			!header.ReadU64(payloadSize) ||
			!header.ReadU64(payloadChecksum))
		{
			return Fail(EProbeVolumeBinaryStatus::Truncated,
				"the .probes fixed header is truncated");
		}
		if (version != ProbeVolumeFormatVersion)
		{
			return Fail(EProbeVolumeBinaryStatus::UnsupportedVersion,
				"the .probes format version is unsupported");
		}
		if (endian != EndianMarker)
		{
			return Fail(EProbeVolumeBinaryStatus::UnsupportedEndianness,
				"the .probes file does not use the required little-endian encoding");
		}
		if (headerFlags != 0u)
		{
			return Fail(EProbeVolumeBinaryStatus::InvalidPayload,
				"the .probes fixed header contains unsupported flags");
		}
		if (headerSize != FixedHeaderSize ||
			payloadSize > MaxPayloadBytes ||
			payloadSize > size - headerSize ||
			payloadSize != size - headerSize)
		{
			return Fail(EProbeVolumeBinaryStatus::Truncated,
				"the .probes payload size does not match the file size");
		}

		const uint8_t* payloadBytes = bytes + headerSize;
		if (Checksum(payloadBytes, static_cast<size_t>(payloadSize)) != payloadChecksum)
		{
			return Fail(EProbeVolumeBinaryStatus::ChecksumMismatch,
				"the .probes payload checksum does not match");
		}

		Reader payload(payloadBytes, static_cast<size_t>(payloadSize));
		ProbeVolumeDataPtr data = ProbeVolumeDataPtr::Make();
		uint32_t bakedStateCount = 0u;
		uint32_t compression = 0u;
		uint32_t payloadFlags = 0u;
		uint32_t brickCount = 0u;
		uint32_t probeCount = 0u;
		if (!payload.ReadU32(bakedStateCount) ||
			!payload.ReadU32(data->m_shOrder) ||
			!payload.ReadU32(compression) ||
			!payload.ReadU32(payloadFlags) ||
			!payload.ReadU64(data->m_layoutHash) ||
			!payload.ReadU64(data->m_representationHash) ||
			!payload.ReadU64(data->m_transportHash) ||
			!payload.ReadU64(data->m_lightingHash) ||
			!payload.ReadU64(data->m_sourceWorldHash) ||
			!payload.ReadVec3(data->m_volumeMin) ||
			!payload.ReadVec3(data->m_volumeMax) ||
			!ReadBakeSettings(payload, data->m_bakeSettings) ||
			!payload.ReadU32(brickCount) ||
			!payload.ReadU32(probeCount) ||
			!payload.ReadU32(data->m_diagnostics.m_invalidProbeCount) ||
			!payload.ReadU32(data->m_diagnostics.m_relocatedProbeCount) ||
			!payload.ReadFloat(data->m_diagnostics.m_averageValidity) ||
			!payload.ReadFloat(data->m_diagnostics.m_bakeDurationSeconds) ||
			!payload.ReadString(data->m_stateName) ||
			!payload.ReadString(data->m_bakerVersion) ||
			!payload.ReadString(data->m_diagnostics.m_message))
		{
			return Fail(EProbeVolumeBinaryStatus::Truncated,
				"the .probes metadata payload is truncated or oversized");
		}
		data->m_formatVersion = version;
		data->m_compression = static_cast<EProbeVolumeCompression>(compression);
		if (payloadFlags != 0u)
		{
			return Fail(EProbeVolumeBinaryStatus::InvalidPayload,
				"the .probes payload contains unsupported flags");
		}
		if (bakedStateCount != OneBakedState)
		{
			return Fail(EProbeVolumeBinaryStatus::InvalidPayload,
				"a .probes file must contain exactly one baked lighting state");
		}
		if (data->m_layoutHash == 0u ||
			data->m_representationHash == 0u ||
			data->m_transportHash == 0u ||
			data->m_lightingHash == 0u)
		{
			return Fail(EProbeVolumeBinaryStatus::InvalidPayload,
				"the .probes payload is missing required identity hashes");
		}
		if (brickCount == 0u || brickCount > MaxBrickCount ||
			probeCount == 0u || probeCount > MaxProbeCount)
		{
			return Fail(EProbeVolumeBinaryStatus::InvalidPayload,
				"the .probes brick or probe count exceeds the supported limits");
		}

		constexpr size_t BrickBytes = 48u;
		constexpr size_t ProbeBytes = 212u;
		const uint64_t expectedRecordBytes =
			static_cast<uint64_t>(brickCount) * BrickBytes +
			static_cast<uint64_t>(probeCount) * ProbeBytes;
		if (expectedRecordBytes != payload.Remaining())
		{
			return Fail(EProbeVolumeBinaryStatus::InvalidPayload,
				"the .probes record counts do not match the payload size");
		}

		data->m_bricks.Resize(brickCount);
		for (ProbeVolumeBrick& brick : data->m_bricks)
		{
			if (!payload.ReadVec3(brick.m_min) ||
				!payload.ReadVec3(brick.m_max) ||
				!payload.ReadU32(brick.m_subdivisionLevel) ||
				!payload.ReadU32(brick.m_firstProbeIndex) ||
				!payload.ReadU32(brick.m_probeCount) ||
				!payload.ReadU32(brick.m_probeCounts.x) ||
				!payload.ReadU32(brick.m_probeCounts.y) ||
				!payload.ReadU32(brick.m_probeCounts.z))
			{
				return Fail(EProbeVolumeBinaryStatus::Truncated,
					"the .probes brick table is truncated");
			}
		}

		data->m_probes.Resize(probeCount);
		for (ProbeVolumeSample& probe : data->m_probes)
		{
			if (!payload.ReadVec3(probe.m_position) ||
				!payload.ReadVec3(probe.m_relocationOffset) ||
				!payload.ReadFloat(probe.m_validity) ||
				!payload.ReadU32(probe.m_flags))
			{
				return Fail(EProbeVolumeBinaryStatus::Truncated,
					"the .probes sample table is truncated");
			}
			for (glm::vec3& coefficient : probe.m_irradiance)
			{
				if (!payload.ReadVec3(coefficient))
				{
					return Fail(EProbeVolumeBinaryStatus::Truncated,
						"the .probes spherical-harmonics table is truncated");
				}
			}
			for (glm::vec2& visibility : probe.m_visibility)
			{
				if (!payload.ReadVec2(visibility))
				{
					return Fail(EProbeVolumeBinaryStatus::Truncated,
						"the .probes visibility table is truncated");
				}
			}
			for (float& environmentVisibility :
				probe.m_environmentVisibility)
			{
				if (!payload.ReadFloat(environmentVisibility))
				{
					return Fail(EProbeVolumeBinaryStatus::Truncated,
						"the .probes environment-visibility table is truncated");
				}
			}
		}
		if (payload.Remaining() != 0u)
		{
			return Fail(EProbeVolumeBinaryStatus::InvalidPayload,
				"the .probes payload has unexpected trailing records");
		}

		std::string validationDiagnostic;
		if (!data->Validate(validationDiagnostic))
		{
			return Fail(EProbeVolumeBinaryStatus::InvalidPayload,
				"invalid .probes payload: " + validationDiagnostic);
		}

		ProbeVolumeBinaryResult result;
		result.m_status = EProbeVolumeBinaryStatus::Success;
		result.m_diagnostic = "loaded one baked probe-volume state";
		result.m_data = std::move(data);
		return result;
	}
	catch (const std::exception& exception)
	{
		return Fail(EProbeVolumeBinaryStatus::InvalidPayload,
			std::string("cannot parse .probes payload: ") + exception.what());
	}
	catch (...)
	{
		return Fail(EProbeVolumeBinaryStatus::InvalidPayload,
			"cannot parse .probes payload: unknown failure");
	}
}

ProbeVolumeBinaryResult ProbeVolumeBinary::Load(
	const std::filesystem::path& path) noexcept
{
	try
	{
		std::ifstream stream(path, std::ios::binary | std::ios::ate);
		if (!stream.is_open())
		{
			return Fail(EProbeVolumeBinaryStatus::IoFailure,
				"cannot open .probes file '" + path.generic_string() + "'");
		}
		const std::streampos end = stream.tellg();
		if (end < 0 || static_cast<uint64_t>(end) > MaxPayloadBytes + FixedHeaderSize)
		{
			return Fail(EProbeVolumeBinaryStatus::IoFailure,
				"the .probes file size is invalid or exceeds the supported limit");
		}
		TVector<uint8_t> bytes;
		bytes.Resize(static_cast<size_t>(end));
		stream.seekg(0, std::ios::beg);
		if (!bytes.IsEmpty())
		{
			stream.read(
				reinterpret_cast<char*>(bytes.GetData()),
				static_cast<std::streamsize>(bytes.Num()));
		}
		if (!stream.good() && !stream.eof())
		{
			return Fail(EProbeVolumeBinaryStatus::IoFailure,
				"cannot read .probes file '" + path.generic_string() + "'");
		}
		return Deserialize(bytes.GetData(), bytes.Num());
	}
	catch (const std::exception& exception)
	{
		return Fail(EProbeVolumeBinaryStatus::IoFailure,
			std::string("cannot load .probes file: ") + exception.what());
	}
	catch (...)
	{
		return Fail(EProbeVolumeBinaryStatus::IoFailure,
			"cannot load .probes file: unknown failure");
	}
}

bool ProbeVolumeBinary::SaveAtomic(
	const std::filesystem::path& path,
	const ProbeVolumeData& data,
	std::string& outDiagnostic,
	bool bOverwrite) noexcept
{
	TVector<uint8_t> bytes;
	if (!Serialize(data, bytes, outDiagnostic))
	{
		return false;
	}
	return Workspace::AtomicReplaceWorkspaceCacheBinary(
		path,
		bytes.GetData(),
		bytes.Num(),
		outDiagnostic,
		Workspace::EWorkspaceCacheAtomicWriteFailurePoint::None,
		bOverwrite ?
			Workspace::EWorkspaceCacheAtomicWriteMode::ReplaceExisting :
			Workspace::EWorkspaceCacheAtomicWriteMode::FailIfExists);
}
