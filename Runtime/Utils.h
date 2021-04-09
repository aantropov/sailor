#pragma once
#include <string>
#include "Sailor.h"

namespace Sailor
{
	namespace Utils
	{
		SAILOR_API std::string wchar_to_UTF8(const wchar_t* in);
		SAILOR_API std::wstring UTF8_to_wchar(const char* in);

		SAILOR_API std::string RemoveExtension(const std::string& filename);
		SAILOR_API std::string GetExtension(const std::string& filename);
	}
}