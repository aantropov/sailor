#include "Workspace/WorkspacePathEncoding.h"

#if defined(_WIN32)
#include <Windows.h>
#endif

bool Sailor::Workspace::IsPathWithin(const std::filesystem::path& root, const std::filesystem::path& path)
{
	auto part = path.begin();
	for (const auto& rootPart : root)
	{
		if (part == path.end()) return false;
		if (rootPart != *part)
		{
#if defined(_WIN32)
			const auto& left = rootPart.native();
			const auto& right = part->native();
			if (CompareStringOrdinal(left.data(), static_cast<int>(left.size()),
				right.data(), static_cast<int>(right.size()), TRUE) != CSTR_EQUAL) return false;
#else
			return false;
#endif
		}
		++part;
	}
	return true;
}
