#pragma once
#include <string>

namespace Sailor
{
	namespace Utils
	{
		std::string wchar_to_UTF8(const wchar_t* in);
		std::wstring UTF8_to_wchar(const char* in);

		std::string RemoveExtension(const std::string& filename);
		std::string GetExtension(const std::string& filename);
	}
}