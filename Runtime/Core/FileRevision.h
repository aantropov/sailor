#pragma once

#include "Core/Defines.h"

#include <cstdint>
#include <ctime>
#include <string>

namespace YAML
{
	class Node;
}

namespace Sailor
{
	struct FileRevision final
	{
		int64_t m_modificationTimeNanoseconds{};
		bool m_bIsValid = false;

		bool operator==(const FileRevision& rhs) const noexcept
		{
			return m_modificationTimeNanoseconds == rhs.m_modificationTimeNanoseconds &&
				m_bIsValid == rhs.m_bIsValid;
		}

		bool operator!=(const FileRevision& rhs) const noexcept
		{
			return !(*this == rhs);
		}

		SAILOR_API YAML::Node Serialize() const;
		SAILOR_API void Deserialize(const YAML::Node& inData);
	};

	namespace Utils
	{
		SAILOR_API std::time_t GetFileModificationTime(const std::string& filepath);
		SAILOR_API bool TryGetFileRevision(
			const std::string& filepath,
			FileRevision& outRevision) noexcept;
	}
}
