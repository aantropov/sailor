#include "Utils.h"
#include <sys/types.h>
#include <sys/stat.h>
#include <processthreadsapi.h>

#ifndef WIN32
#include <unistd.h>
#define stat _stat
#include <ntverp.h>
#endif

#include <algorithm> 
#include <functional> 
#include <cctype>

#include <windows.h>

using namespace Sailor::Utils;

std::string Sailor::Utils::wchar_to_UTF8(const wchar_t* in)
{
	std::string out;
	uint32_t codepoint = 0;
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
	uint32_t codepoint;
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

DWORD Sailor::Utils::GetRandomColorHex()
{
	COLORREF res = RGB(
		(BYTE)(rand() % 255), // red component of color
		(BYTE)(rand() % 255), // green component of color
		(BYTE)(rand() % 255) // blue component of color
	);

	return (DWORD)res;
}

void Sailor::Utils::SetThreadName(uint32_t dwThreadID, const std::string& threadName)
{
#if VER_PRODUCTBUILD > 9600
	// Windows 10+ SDK code goes here
	HRESULT r;
	r = SetThreadDescription(
		reinterpret_cast<HANDLE>(dwThreadID),
		UTF8_to_wchar(threadName.c_str()).c_str()
	);

#else
	// Windows 8.1- SDK code goes here
#endif

}

void Sailor::Utils::SetThreadName(const std::string& threadName)
{
#if VER_PRODUCTBUILD > 9600
	HRESULT r;
	r = SetThreadDescription(
		GetCurrentThread(),
		UTF8_to_wchar(threadName.c_str()).c_str()
	);
#else
	// Windows 8.1- SDK code goes here
#endif
}

void Sailor::Utils::SetThreadName(std::thread* thread, const std::string& threadName)
{
#if VER_PRODUCTBUILD > 9600

	HRESULT r;
	r = SetThreadDescription(
		(HANDLE)thread->native_handle(),
		UTF8_to_wchar(threadName.c_str()).c_str()
	);
#else
	// Windows 8.1- SDK code goes here
#endif
}

std::string Sailor::Utils::RemoveFileExtension(const std::string& filename)
{
	SAILOR_PROFILE_FUNCTION();
	size_t lastdot = filename.find_last_of(".");
	lastdot++;
	if (lastdot == std::string::npos)
		return filename;
	return filename.substr(0, lastdot);
}

std::string Sailor::Utils::GetFileFolder(const std::string& filepath)
{
	const size_t lastSlash = filepath.rfind('/');
	if (std::string::npos != lastSlash)
	{
		return filepath.substr(0, lastSlash + 1);
	}

	return std::string();
}

std::string Sailor::Utils::GetFileExtension(const std::string& filename)
{
	SAILOR_PROFILE_FUNCTION();
	size_t lastdot = filename.find_last_of(".");
	lastdot++;
	if (lastdot == std::string::npos)
		return std::string();
	return filename.substr(lastdot, filename.size() - lastdot);
}

std::vector<std::string> Sailor::Utils::SplitString(const std::string& str, const std::string& delimiter)
{
	SAILOR_PROFILE_FUNCTION();
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
	SAILOR_PROFILE_FUNCTION();
	while ((startPosition = str.find(from, startPosition)) < endLocation)
	{
		str.replace(startPosition, from.length(), to);

		// Handles case where 'to' is a substring of 'from'
		startPosition += to.length();

		size_t maxJump = std::string::npos - endLocation;
		endLocation += std::min(maxJump, to.length() - from.length());
	}
}

void Sailor::Utils::Erase(std::string& str, const std::string& substr, size_t startPosition, size_t endLocation)
{
	SAILOR_PROFILE_FUNCTION();
	while ((startPosition = str.find(substr, startPosition)) < endLocation)
	{
		str = str.erase(startPosition, substr.length());

		// Handles case where 'to' is a substring of 'from'
		startPosition += substr.length();

		size_t maxJump = std::string::npos - endLocation;
		endLocation += std::min(maxJump, substr.length());
	}
}

std::string Sailor::Utils::SanitizeFilepath(const std::string& filename)
{
	std::string res = filename;
	ReplaceAll(res, "\\", "/");
	ReplaceAll(res, "//", "/");
	return res;
}

std::time_t Sailor::Utils::GetFileModificationTime(const std::string& filepath)
{
	SAILOR_PROFILE_FUNCTION();
	struct stat result;
	if (stat(filepath.c_str(), &result) == 0)
	{
		return (std::time_t)result.st_mtime;
	}
	return 0;
}

void Sailor::Utils::FindAllOccurances(std::string& str, const std::string& substr, std::vector<size_t>& outLocations, size_t startPosition, size_t endLocation)
{
	SAILOR_PROFILE_FUNCTION();
	size_t pos = str.find(substr, startPosition);
	while (pos < endLocation)
	{
		outLocations.push_back(pos);
		pos = str.find(substr, pos + 1);
	}
}

void Sailor::Utils::Trim(std::string& s)
{
	SAILOR_PROFILE_FUNCTION();
	s.erase(s.begin(), std::find_if(s.begin(), s.end(), [](unsigned char ch) {
		return !std::isspace(ch);
		}));
}

int64_t Sailor::Utils::GetCurrentTimeMs()
{
	return (int64_t)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

int64_t Sailor::Utils::GetCurrentTimeMicro()
{
	return (int64_t)std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

int64_t Sailor::Utils::GetCurrentTimeNano()
{
	return (int64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

void Sailor::Utils::Timer::Start()
{
	LARGE_INTEGER li;
	if (!QueryPerformanceFrequency(&li))
	{
		SAILOR_LOG("QueryPerformanceFrequency failed!");
	}

	m_pcFrequence = double(li.QuadPart) / 1000.0;

	QueryPerformanceCounter(&li);
	m_counterStart = li.QuadPart;
}

void Sailor::Utils::Timer::Stop()
{
	LARGE_INTEGER li;
	QueryPerformanceCounter(&li);
	m_counterEnd = li.QuadPart;

	m_counterAcc += m_counterEnd - m_counterStart;
}

int64_t Sailor::Utils::Timer::ResultMs() const
{
	return int64_t(double(m_counterEnd - m_counterStart) / m_pcFrequence);
}

int64_t Sailor::Utils::Timer::ResultAccumulatedMs() const
{
	if (m_pcFrequence == 0.0)
	{
		return 0;
	}
	return int64_t((double)m_counterAcc / m_pcFrequence);
}

void Sailor::Utils::Timer::Clear()
{
	m_counterStart = 0;
	m_counterEnd = 0;
	m_counterAcc = 0;
	m_pcFrequence = 0.0;
}