#pragma once

#include "Core/Defines.h"
#include "Memory/LockFreeHeapAllocator.h"
#include "Memory/MallocAllocator.hpp"
#include "Containers/Vector.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace Sailor
{
	namespace Utils
	{
		SAILOR_API std::string wchar_to_UTF8(const wchar_t* in);
		SAILOR_API std::string wchar_to_UTF8(std::wstring_view in);
		SAILOR_API std::wstring UTF8_to_wchar(std::string_view in);

		SAILOR_API std::string RemoveFileExtension(std::string_view filename);
		SAILOR_API std::string SanitizeFilepath(std::string_view filename);
		SAILOR_API std::string GetFileExtension(std::string_view filename);
		SAILOR_API std::string GetFileFolder(std::string_view filepath);

		SAILOR_API TVector<std::string> SplitStringByLines(std::string_view str);
		SAILOR_API TVector<std::string> SplitString(std::string_view str, std::string_view delimiter);
		// Matches must fit in the original [startPos, endPos) range; an empty pattern is ignored.
		// Pattern and replacement views must not refer into the string being edited.
		SAILOR_API void ReplaceAll(std::string& str, std::string_view from, std::string_view to, size_t startPos = 0, size_t endPos = std::string::npos);
		SAILOR_API void Erase(std::string& str, std::string_view substr, size_t startPos = 0, size_t endPos = std::string::npos);

		SAILOR_API void FindAllOccurances(std::string_view str, std::string_view substr, TVector<size_t>& outLocations, size_t startPos = 0, size_t endPos = std::string::npos);

		SAILOR_API void Trim(std::string& s);
		[[nodiscard]] SAILOR_API std::string_view TrimView(std::string_view str);

		std::string GetArgValue(const char** args, int32_t& i, int32_t num);
	}

	template<typename Class, typename Member>
	constexpr size_t OffsetOf(Member Class::* member)
	{
		return (char*)&((Class*)nullptr->*member) - (char*)nullptr;
	}
}
