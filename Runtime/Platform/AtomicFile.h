#pragma once

#include "Core/Defines.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace Sailor::Platform
{
	enum class EAtomicWriteMode
	{
		ReplaceExisting,
		FailIfExists
	};

	enum class EAtomicWriteResult
	{
		NotPublished,
		Published,
		DirectorySyncUnsupported,
		Synced
	};

	inline bool IsAtomicWriteComplete(EAtomicWriteResult result) noexcept
	{
		return result == EAtomicWriteResult::Synced || result == EAtomicWriteResult::DirectorySyncUnsupported;
	}

	// Published means the target changed, but the requested OS sync did not complete.
	// DirectorySyncUnsupported preserves best-effort publication on filesystems without directory fsync.
	SAILOR_SHARED_API EAtomicWriteResult AtomicWriteFile(
		const std::filesystem::path& target,
		const void* data,
		uint64_t size,
		std::string& outDiagnostic,
		EAtomicWriteMode mode = EAtomicWriteMode::ReplaceExisting) noexcept;

	inline EAtomicWriteResult AtomicWriteFile(
		const std::filesystem::path& target,
		std::string_view text,
		std::string& outDiagnostic,
		EAtomicWriteMode mode = EAtomicWriteMode::ReplaceExisting) noexcept
	{
		return AtomicWriteFile(target, text.data(), text.size(), outDiagnostic, mode);
	}
}
