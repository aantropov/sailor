#pragma once

#include "Platform/AtomicFile.h"

namespace Sailor::Platform
{
	enum class EAtomicWriteFailurePoint
	{
		None,
		BeforePublish,
		DirectorySync,
		DirectorySyncUnsupported,
		TemporaryCleanup
	};

#if defined(SAILOR_FILE_IO_TEST_HOOKS)
	SAILOR_SHARED_API EAtomicWriteResult AtomicWriteFileForTests(
		const std::filesystem::path& target,
		const void* data,
		uint64_t size,
		std::string& outDiagnostic,
		EAtomicWriteFailurePoint failurePoint,
		EAtomicWriteMode mode = EAtomicWriteMode::ReplaceExisting) noexcept;
#endif
}
