#include "Utils.h"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <utility>

using namespace Sailor;
using namespace Sailor::Utils;

std::string Utils::wchar_to_UTF8(const wchar_t* in)
{
	return wchar_to_UTF8(std::wstring_view(in));
}

std::string Utils::wchar_to_UTF8(std::wstring_view in)
{
	std::string out;
	out.reserve(in.size());
	for (size_t i = 0; i < in.size(); ++i)
	{
		uint32_t codepoint = static_cast<uint32_t>(in[i]);
		if (codepoint >= 0xd800 && codepoint <= 0xdbff)
		{
			if (i + 1 < in.size() && in[i + 1] >= 0xdc00 && in[i + 1] <= 0xdfff)
				codepoint = 0x10000 + ((codepoint - 0xd800) << 10) + (in[++i] - 0xdc00);
			else codepoint = 0xfffd;
		}
		else if ((codepoint >= 0xdc00 && codepoint <= 0xdfff) || codepoint > 0x10ffff) codepoint = 0xfffd;

		if (codepoint <= 0x7f)
			out.append(1, static_cast<char>(codepoint));
		else if (codepoint <= 0x7ff)
		{
			out.append(1, static_cast<char>(0xc0 | (codepoint >> 6)));
			out.append(1, static_cast<char>(0x80 | (codepoint & 0x3f)));
		}
		else if (codepoint <= 0xffff)
		{
			out.append(1, static_cast<char>(0xe0 | (codepoint >> 12)));
			out.append(1, static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f)));
			out.append(1, static_cast<char>(0x80 | (codepoint & 0x3f)));
		}
		else
		{
			out.append(1, static_cast<char>(0xf0 | (codepoint >> 18)));
			out.append(1, static_cast<char>(0x80 | ((codepoint >> 12) & 0x3f)));
			out.append(1, static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f)));
			out.append(1, static_cast<char>(0x80 | (codepoint & 0x3f)));
		}
	}
	return out;
}

std::wstring Utils::UTF8_to_wchar(std::string_view in)
{
	std::wstring out;
	uint32_t codepoint = 0;
	for (size_t i = 0; i < in.size(); ++i)
	{
		const auto ch = static_cast<unsigned char>(in[i]);
		if (ch <= 0x7f)
			codepoint = ch;
		else if (ch <= 0xbf)
			codepoint = (codepoint << 6) | (ch & 0x3f);
		else if (ch <= 0xdf)
			codepoint = ch & 0x1f;
		else if (ch <= 0xef)
			codepoint = ch & 0x0f;
		else
			codepoint = ch & 0x07;
		if ((i + 1 == in.size() || (in[i + 1] & 0xc0) != 0x80) && codepoint <= 0x10ffff)
		{
			if (sizeof(wchar_t) > 2)
				out.append(1, static_cast<wchar_t>(codepoint));
			else if (codepoint > 0xffff)
			{
				codepoint -= 0x10000;
				out.append(1, static_cast<wchar_t>(0xd800 + (codepoint >> 10)));
				out.append(1, static_cast<wchar_t>(0xdc00 + (codepoint & 0x03ff)));
			}
			else if (codepoint < 0xd800 || codepoint >= 0xe000)
				out.append(1, static_cast<wchar_t>(codepoint));
		}
	}
	return out;
}

namespace
{
	size_t FindExtensionOffset(std::string_view filename)
	{
		const size_t separator = filename.find_last_of("/\\");
		const size_t nameStart = separator == std::string::npos ? 0 : separator + 1;
		const size_t dot = filename.find_last_of('.');
		if (dot == std::string::npos || dot <= nameStart ||
			filename.compare(nameStart, std::string::npos, "..") == 0)
		{
			return std::string::npos;
		}
		return dot;
	}
}

std::string Utils::RemoveFileExtension(std::string_view filename)
{
	SAILOR_PROFILE_FUNCTION();
	return std::string(filename.substr(0, FindExtensionOffset(filename)));
}

std::string Utils::GetFileFolder(std::string_view filepath)
{
	const size_t lastSlash = filepath.rfind('/');
	if (std::string::npos != lastSlash)
	{
		return std::string(filepath.substr(0, lastSlash + 1));
	}

	return std::string();
}

std::string Utils::GetFileExtension(std::string_view filename)
{
	SAILOR_PROFILE_FUNCTION();
	const size_t dot = FindExtensionOffset(filename);
	return dot == std::string::npos ? std::string() : std::string(filename.substr(dot + 1));
}

TVector<std::string> Utils::SplitStringByLines(std::string_view str)
{
	TVector<std::string> result;
	size_t start = 0;
	while (start < str.size())
	{
		const size_t end = str.find('\n', start);
		result.Emplace(str.substr(start, end == std::string_view::npos ? end : end - start));
		if (end == std::string_view::npos) break;
		start = end + 1;
	}

	return result;
}

TVector<std::string> Utils::SplitString(std::string_view str, std::string_view delimiter)
{
	SAILOR_PROFILE_FUNCTION();
	TVector<std::string> strings;
	if (delimiter.empty())
	{
		strings.Emplace(str);
		return strings;
	}

	std::string::size_type pos = 0;
	std::string::size_type prev = 0;
	while ((pos = str.find(delimiter, prev)) != std::string::npos)
	{
		strings.Emplace(str.substr(prev, pos - prev));
		prev = pos + delimiter.size();
	}

	// To get the last substring (or only, if delimiter is not found)
	strings.Emplace(str.substr(prev));

	return strings;
}

void Utils::ReplaceAll(std::string& str, std::string_view from, std::string_view to, size_t startPosition, size_t endLocation)
{
	SAILOR_PROFILE_FUNCTION();
	if (from.empty())
	{
		return;
	}

	endLocation = (std::min)(endLocation, str.size());
	while ((startPosition = str.find(from, startPosition)) < endLocation &&
		from.size() <= endLocation - startPosition)
	{
		str.replace(startPosition, from.size(), to);
		endLocation = endLocation - from.size() + to.size();
		startPosition += to.size();
	}
}

void Utils::Erase(std::string& str, std::string_view substr, size_t startPosition, size_t endLocation)
{
	SAILOR_PROFILE_FUNCTION();
	ReplaceAll(str, substr, {}, startPosition, endLocation);
}

std::string Utils::SanitizeFilepath(std::string_view filename)
{
	std::string res(filename);
	ReplaceAll(res, "\\", "/");
	ReplaceAll(res, "//", "/");
	return res;
}

void Utils::FindAllOccurances(std::string_view str, std::string_view substr, TVector<size_t>& outLocations, size_t startPosition, size_t endLocation)
{
	SAILOR_PROFILE_FUNCTION();
	size_t pos = str.find(substr, startPosition);
	while (pos < endLocation)
	{
		outLocations.Add(pos);
		pos = str.find(substr, pos + 1);
	}
}

void Utils::Trim(std::string& s)
{
	SAILOR_PROFILE_FUNCTION();
	s.assign(TrimView(s));
}

std::string_view Utils::TrimView(std::string_view str)
{
	size_t first = 0;
	size_t last = str.size();
	while (first < last && std::isspace(static_cast<unsigned char>(str[first]))) ++first;
	while (last > first && std::isspace(static_cast<unsigned char>(str[last - 1]))) --last;
	return str.substr(first, last - first);
}

std::string Utils::GetArgValue(const char** args, int32_t& i, int32_t num)
{
	// argv is already tokenized by the platform or the editor protocol.
	return i + 1 < num ? args[++i] : "";
}
