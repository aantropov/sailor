#pragma once

#include "GlobalIllumination/GIProbesData.h"
#include "Core/Defines.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

namespace Sailor
{
	enum class EGIProbesBinaryStatus : uint8_t
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

	struct SAILOR_SHARED_API GIProbesBinaryResult final
	{
		EGIProbesBinaryStatus m_status = EGIProbesBinaryStatus::InvalidPayload;
		std::string m_diagnostic{};
		GIProbesDataPtr m_data{};

		bool IsSuccess() const noexcept
		{
			return m_status == EGIProbesBinaryStatus::Success && m_data;
		}
	};

	class SAILOR_SHARED_API GIProbesBinary final
	{
	public:
		static bool Serialize(
			const GIProbesData& data,
			TVector<uint8_t>& outBytes,
			std::string& outDiagnostic) noexcept;
		static GIProbesBinaryResult Deserialize(
			const uint8_t* bytes,
			size_t size) noexcept;
		static GIProbesBinaryResult Load(
			const std::filesystem::path& path) noexcept;
		static bool SaveAtomic(
			const std::filesystem::path& path,
			const GIProbesData& data,
			std::string& outDiagnostic,
			bool bOverwrite = true) noexcept;
	};
}
