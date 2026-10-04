#include "Utils.h"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <utility>

using namespace Sailor;
using namespace Sailor::Utils;

std::string Utils::wchar_to_UTF8(const wchar_t* in)
{
	std::string out;
	uint32_t codepoint = 0;
	for (; *in != 0; ++in)
	{
		if (*in >= 0xd800 && *in <= 0xdbff)
			codepoint = ((*in - 0xd800) << 10) + 0x10000;
		else
		{
			if (*in >= 0xdc00 && *in <= 0xdfff)
				codepoint |= *in - 0xdc00;
			else
				codepoint = *in;

			if (codepoint <= 0x7f)
				out.append(1, static_cast<char>(codepoint));
			else if (codepoint <= 0x7ff)
			{
				out.append(1, static_cast<char>(0xc0 | ((codepoint >> 6) & 0x1f)));
				out.append(1, static_cast<char>(0x80 | (codepoint & 0x3f)));
			}
			else if (codepoint <= 0xffff)
			{
				out.append(1, static_cast<char>(0xe0 | ((codepoint >> 12) & 0x0f)));
				out.append(1, static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f)));
				out.append(1, static_cast<char>(0x80 | (codepoint & 0x3f)));
			}
			else
			{
				out.append(1, static_cast<char>(0xf0 | ((codepoint >> 18) & 0x07)));
				out.append(1, static_cast<char>(0x80 | ((codepoint >> 12) & 0x3f)));
				out.append(1, static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f)));
				out.append(1, static_cast<char>(0x80 | (codepoint & 0x3f)));
			}
			codepoint = 0;
		}
	}
	return out;
}

std::wstring Utils::UTF8_to_wchar(const char* in)
{
	std::wstring out;
	uint32_t codepoint = 0;
	while (*in != 0)
	{
		unsigned char ch = static_cast<unsigned char>(*in);
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
		++in;
		if (((*in & 0xc0) != 0x80) && (codepoint <= 0x10ffff))
		{
			if (sizeof(wchar_t) > 2)
				out.append(1, static_cast<wchar_t>(codepoint));
			else if (codepoint > 0xffff)
			{
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
	size_t FindExtensionOffset(const std::string& filename)
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

std::string Utils::RemoveFileExtension(const std::string& filename)
{
	SAILOR_PROFILE_FUNCTION();
	return filename.substr(0, FindExtensionOffset(filename));
}

std::string Utils::GetFileFolder(const std::string& filepath)
{
	const size_t lastSlash = filepath.rfind('/');
	if (std::string::npos != lastSlash)
	{
		return filepath.substr(0, lastSlash + 1);
	}

	return std::string();
}

std::string Utils::GetFileExtension(const std::string& filename)
{
	SAILOR_PROFILE_FUNCTION();
	const size_t dot = FindExtensionOffset(filename);
	return dot == std::string::npos ? std::string() : filename.substr(dot + 1);
}

TVector<std::string> Utils::SplitStringByLines(const std::string& str)
{
	TVector<std::string> result;
	auto ss = std::stringstream{ str };

	for (std::string line; std::getline(ss, line, '\n');)
	{
		result.Emplace(std::move(line));
	}

	return result;
}

TVector<std::string> Utils::SplitString(const std::string& str, const std::string& delimiter)
{
	SAILOR_PROFILE_FUNCTION();
	TVector<std::string> strings;

	std::string::size_type pos = 0;
	std::string::size_type prev = 0;
	while ((pos = str.find(delimiter, prev)) != std::string::npos)
	{
		strings.Add(str.substr(prev, pos - prev));
		prev = pos + delimiter.size();
	}

	// To get the last substring (or only, if delimiter is not found)
	strings.Add(str.substr(prev));

	return strings;
}

void Utils::ReplaceAll(std::string& str, const std::string& from, const std::string& to, size_t startPosition, size_t endLocation)
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

void Utils::Erase(std::string& str, const std::string& substr, size_t startPosition, size_t endLocation)
{
	SAILOR_PROFILE_FUNCTION();
	ReplaceAll(str, substr, {}, startPosition, endLocation);
}

std::string Utils::SanitizeFilepath(const std::string& filename)
{
	std::string res = filename;
	ReplaceAll(res, "\\", "/");
	ReplaceAll(res, "//", "/");
	return res;
}

void Utils::FindAllOccurances(const std::string& str, const std::string& substr, TVector<size_t>& outLocations, size_t startPosition, size_t endLocation)
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
	const auto isNotSpace = [](unsigned char ch) {
		return !std::isspace(ch);
	};
	s.erase(s.begin(), std::find_if(s.begin(), s.end(), isNotSpace));
	s.erase(std::find_if(s.rbegin(), s.rend(), isNotSpace).base(), s.end());
}

std::string Utils::GetArgValue(const char** args, int32_t& i, int32_t num)
{
	if (i + 1 >= num)
	{
		return "";
	}

	i++;
	std::string value = args[i];

	if (value[0] == '\"')
	{
		while (i < num && value[value.length() - 1] != '\"')
		{
			i++;
			value += " " + std::string(args[i]);
		}
		value = value.substr(1, value.length() - 2);
	}

	return value;
}
