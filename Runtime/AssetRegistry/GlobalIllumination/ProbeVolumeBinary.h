#pragma once

#include "AssetRegistry/GlobalIllumination/ProbeVolumeData.h"
#include "Core/Defines.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

namespace Sailor
{
	enum class EProbeVolumeBinaryStatus : uint8_t
	{
		Success = 0,
		IoFailure,
		InvalidMagic,
		UnsupportedVersion,
		UnsupportedEndianness,
		Truncated,
		ChecksumMismatch,
		InvalidPayload
	};

	struct SAILOR_SHARED_API ProbeVolumeBinaryResult final
	{
		EProbeVolumeBinaryStatus m_status = EProbeVolumeBinaryStatus::InvalidPayload;
		std::string m_diagnostic{};
		ProbeVolumeDataPtr m_data{};

		bool IsSuccess() const noexcept
		{
			return m_status == EProbeVolumeBinaryStatus::Success && m_data;
		}
	};

	class SAILOR_SHARED_API ProbeVolumeBinary final
	{
	public:
		static bool Serialize(
			const ProbeVolumeData& data,
			TVector<uint8_t>& outBytes,
			std::string& outDiagnostic) noexcept;
		static ProbeVolumeBinaryResult Deserialize(
			const uint8_t* bytes,
			size_t size) noexcept;
		static ProbeVolumeBinaryResult Load(
			const std::filesystem::path& path) noexcept;
		static bool SaveAtomic(
			const std::filesystem::path& path,
			const ProbeVolumeData& data,
			std::string& outDiagnostic,
			bool bOverwrite = true) noexcept;
	};
}
