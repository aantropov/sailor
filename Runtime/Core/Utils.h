#pragma once

#include "Core/Defines.h"
#include "Memory/LockFreeHeapAllocator.h"
#include "Memory/MallocAllocator.hpp"
#include "Containers/Vector.h"

#include <cstddef>
#include <cstdint>
#include <string>

namespace Sailor
{
	namespace Utils
	{
		SAILOR_API std::string wchar_to_UTF8(const wchar_t* in);
		SAILOR_API std::wstring UTF8_to_wchar(const char* in);

		SAILOR_API std::string RemoveFileExtension(const std::string& filename);
		SAILOR_API std::string SanitizeFilepath(const std::string& filename);
		SAILOR_API std::string GetFileExtension(const std::string& filename);
		SAILOR_API std::string GetFileFolder(const std::string& filepath);

		SAILOR_API TVector<std::string> SplitStringByLines(const std::string& str);
		SAILOR_API TVector<std::string> SplitString(const std::string& str, const std::string& delimiter);
		// Matches must fit in the original [startPos, endPos) range; an empty pattern is ignored.
		SAILOR_API void ReplaceAll(std::string& str, const std::string& from, const std::string& to, size_t startPos = 0, size_t endPos = std::string::npos);
		SAILOR_API void Erase(std::string& str, const std::string& substr, size_t startPos = 0, size_t endPos = std::string::npos);

		SAILOR_API void FindAllOccurances(const std::string& str, const std::string& substr, TVector<size_t>& outLocations, size_t startPos = 0, size_t endPos = std::string::npos);

		SAILOR_API void Trim(std::string& s);

		std::string GetArgValue(const char** args, int32_t& i, int32_t num);
	}

	template<typename Class, typename Member>
	constexpr size_t OffsetOf(Member Class::* member)
	{
		return (char*)&((Class*)nullptr->*member) - (char*)nullptr;
	}
}
