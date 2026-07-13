#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace Sailor::Workspace
{
	inline std::filesystem::path PathFromUtf8(std::string_view utf8Path)
	{
		if (utf8Path.empty())
		{
			return {};
		}

#if defined(__cpp_char8_t)
		const auto* begin = reinterpret_cast<const char8_t*>(utf8Path.data());
		return std::filesystem::path(std::u8string(begin, begin + utf8Path.size()));
#else
		return std::filesystem::u8path(utf8Path.begin(), utf8Path.end());
#endif
	}

	inline std::string PathToUtf8(const std::filesystem::path& path)
	{
		const auto utf8Path = path.generic_u8string();
		return std::string(
			reinterpret_cast<const char*>(utf8Path.data()),
			utf8Path.size());
	}
}
