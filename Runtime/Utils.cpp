#include "Utils.h"

std::string Sailor::Utils::wchar_to_UTF8(const wchar_t* in)
{
	std::string out;
	unsigned int codepoint = 0;
	for (in; *in != 0; ++in)
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

std::wstring Sailor::Utils::UTF8_to_wchar(const char* in)
{
	std::wstring out;
	unsigned int codepoint;
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

std::string Sailor::Utils::RemoveExtension(const std::string& filename)
{
	size_t lastdot = filename.find_last_of(".");
	lastdot++;
	if (lastdot == std::string::npos)
		return filename;
	return filename.substr(0, lastdot);
}

std::string Sailor::Utils::GetExtension(const std::string& filename)
{
	size_t lastdot = filename.find_last_of(".");
	lastdot++;
	if (lastdot == std::string::npos)
		return std::string();
	return filename.substr(lastdot, filename.size() - lastdot);
}

std::vector<std::string> Sailor::Utils::SplitString(const std::string& str, const std::string& delimiter)
{
	std::vector<std::string> strings;

	std::string::size_type pos = 0;
	std::string::size_type prev = 0;
	while ((pos = str.find(delimiter, prev)) != std::string::npos)
	{
		strings.push_back(str.substr(prev, pos - prev));
		prev = pos + delimiter.size();
	}

	// To get the last substring (or only, if delimiter is not found)
	strings.push_back(str.substr(prev));

	return strings;
}

void Sailor::Utils::ReplaceAll(std::string& str, const std::string& from, const std::string& to, size_t startPosition, size_t endLocation)
{
	while ((startPosition = str.find(from, startPosition)) < endLocation)
	{
		str.replace(startPosition, from.length(), to);

		// Handles case where 'to' is a substring of 'from'
		startPosition += to.length();

		size_t maxJump = std::string::npos - endLocation;
		endLocation += min(maxJump, to.length() - from.length());
	}
}

void Sailor::Utils::FindAllOccurances(std::string& str, const std::string& substr, std::vector<size_t>& outLocations, size_t startPosition, size_t endLocation)
{
	size_t pos = str.find(substr, startPosition);
	while (pos < endLocation)
	{
		outLocations.push_back(pos);
		pos = str.find(substr, pos + 1);
	}
}