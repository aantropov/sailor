#pragma once

#include "Core/Defines.h"

#include <cstdint>
#include <string>

namespace Sailor
{
	struct FileRevision final
	{
		int64_t m_modificationTimeNanoseconds{};
		uint64_t m_fileSize{};
		uint64_t m_contentHash{};
		bool m_bIsValid = false;

		bool operator==(const FileRevision& rhs) const noexcept
		{
			return m_modificationTimeNanoseconds == rhs.m_modificationTimeNanoseconds &&
				m_fileSize == rhs.m_fileSize &&
				m_contentHash == rhs.m_contentHash &&
				m_bIsValid == rhs.m_bIsValid;
		}

		bool operator!=(const FileRevision& rhs) const noexcept
		{
			return !(*this == rhs);
		}
	};

	namespace Utils
	{
		SAILOR_API bool TryGetFileRevision(
			const std::string& filepath,
			FileRevision& outRevision) noexcept;
	}
}
