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

#include <sstream>
#include <string>
#include <format>
#include <chrono>

#include <windows.h>
#include "Containers/Vector.h"
#include "Tasks/Tasks.h"
#include "Tasks/Scheduler.h"

using namespace Sailor;
using namespace Sailor::Utils;

glm::vec4 Utils::LinearToSRGB(const glm::u8vec4& linearRGB)
{
	return vec4(LinearToSRGB(vec4(linearRGB)));
}

glm::vec4 Utils::SRGBToLinear(const glm::u8vec4& srgbIn)
{
	return vec4(SRGBToLinear(vec4(srgbIn)));
}

glm::vec4 Utils::LinearToSRGB(const glm::vec4& linearRGB)
{
	return vec4(LinearToSRGB(linearRGB.xyz), linearRGB.a);
}

glm::vec4 Utils::SRGBToLinear(const glm::vec4& srgbIn)
{
	return vec4(SRGBToLinear(srgbIn.xyz), srgbIn.a);
}

glm::vec3 Utils::LinearToSRGB(const glm::vec3& linearRGB)
{
	auto cutoff = glm::lessThan(linearRGB, glm::vec3(0.0031308f));
	glm::vec3 higher = glm::vec3(1.055f) * glm::pow(linearRGB, glm::vec3(1.f / 2.4f)) - glm::vec3(0.055f);
	glm::vec3 lower = linearRGB * glm::vec3(12.92f);

	return glm::mix(higher, lower, cutoff);

	//return vec3(powf(linearRGB.x, 1.0f / 2.2f), powf(linearRGB.y, 1.0f / 2.2f), powf(linearRGB.z, 1.0f / 2.2f));
}

glm::vec3 Utils::SRGBToLinear(const glm::vec3& srgbIn)
{
	glm::vec3 bLess = glm::step(glm::vec3(0.04045f), srgbIn);
	return glm::mix(srgbIn / glm::vec3(12.92f), glm::pow((srgbIn + glm::vec3(0.055f)) / glm::vec3(1.055f), glm::vec3(2.4f)), bLess);
	//return vec3(powf(srgbIn.x, 2.2f), powf(srgbIn.y, 2.2f), powf(srgbIn.z, 2.2f));
}

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

DWORD Utils::GetRandomColorHex()
{
	COLORREF res = RGB(
		(BYTE)(rand() % 255), // red component of color
		(BYTE)(rand() % 255), // green component of color
		(BYTE)(rand() % 255) // blue component of color
	);

	return (DWORD)res;
}

std::string Utils::GetCurrentThreadName()
{
	if (App::GetSubmodule<Tasks::Scheduler>()->IsMainThread())
	{
		return std::string("Thread Main");
	}
	else if (App::GetSubmodule<Tasks::Scheduler>()->IsRendererThread())
	{
		return std::string("Thread Render");
	}
	else
	{
		return std::format("Thread {}", GetCurrentThreadId());
	}
}

void Utils::SetThreadName(size_t dwThreadID, const std::string& threadName)
{
	SetThreadDescription(
		(HANDLE)(dwThreadID),
		UTF8_to_wchar(threadName.c_str()).c_str()
	);
}

void Utils::SetThreadName(const std::string& threadName)
{
	SetThreadDescription(
		GetCurrentThread(),
		UTF8_to_wchar(threadName.c_str()).c_str()
	);
}

void Utils::SetThreadName(std::thread* thread, const std::string& threadName)
{
	SetThreadDescription(
		(HANDLE)thread->native_handle(),
		UTF8_to_wchar(threadName.c_str()).c_str()
	);
}

std::string Utils::RemoveFileExtension(const std::string& filename)
{
	SAILOR_PROFILE_FUNCTION();
	size_t lastdot = filename.find_last_of('.');
	lastdot++;
	if (lastdot == std::string::npos)
		return filename;
	return filename.substr(0, lastdot);
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
	size_t lastdot = filename.find_last_of('.');
	lastdot++;
	if (lastdot == std::string::npos)
		return std::string();
	return filename.substr(lastdot, filename.size() - lastdot);
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
	while ((startPosition = str.find(from, startPosition)) < endLocation)
	{
		str.replace(startPosition, from.length(), to);

		// Handles case where 'to' is a substring of 'from'
		startPosition += to.length();

		size_t maxJump = std::string::npos - endLocation;
		endLocation += std::min(maxJump, to.length() - from.length());
	}
}

void Utils::Erase(std::string& str, const std::string& substr, size_t startPosition, size_t endLocation)
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

std::string Utils::SanitizeFilepath(const std::string& filename)
{
	std::string res = filename;
	ReplaceAll(res, "\\", "/");
	ReplaceAll(res, "//", "/");
	return res;
}

std::time_t Utils::GetFileModificationTime(const std::string& filepath)
{
	SAILOR_PROFILE_FUNCTION();
	struct stat result;
	if (stat(filepath.c_str(), &result) == 0)
	{
		return (std::time_t)result.st_mtime;
	}
	return 0;
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
	s.erase(s.begin(), std::find_if(s.begin(), s.end(), [](unsigned char ch) {
		return !std::isspace(ch);
		}));
}

int64_t Utils::GetCurrentTimeMs()
{
	return (int64_t)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

int64_t Utils::GetCurrentTimeMicro()
{
	return (int64_t)std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

int64_t Utils::GetCurrentTimeNano()
{
	return (int64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

void Utils::Timer::Start()
{
	LARGE_INTEGER li;
	if (!QueryPerformanceFrequency(&li))
	{
		SAILOR_LOG("QueryPerformanceFrequency failed!");
	}

	m_pcFrequence = double(li.QuadPart) / 1000.0;

	QueryPerformanceCounter(&li);
	m_counterStart = li.QuadPart;

	m_bIsStarted = true;
}

void Utils::Timer::Stop()
{
	LARGE_INTEGER li;
	QueryPerformanceCounter(&li);
	m_counterEnd = li.QuadPart;

	m_counterAcc += m_counterEnd - m_counterStart;

	m_bIsStarted = false;
}

int64_t Utils::Timer::ResultMs() const
{
	if (m_bIsStarted)
	{
		LARGE_INTEGER li;
		QueryPerformanceCounter(&li);
		return int64_t(double(li.QuadPart - m_counterStart) / m_pcFrequence);
	}
	return int64_t(double(m_counterEnd - m_counterStart) / m_pcFrequence);
}

int64_t Utils::Timer::ResultAccumulatedMs() const
{
	if (m_pcFrequence == 0.0)
	{
		return 0;
	}

	if (m_bIsStarted)
	{
		LARGE_INTEGER li;
		QueryPerformanceCounter(&li);
		return int64_t(double(li.QuadPart - m_counterStart + m_counterAcc) / m_pcFrequence);
	}

	return int64_t((double)m_counterAcc / m_pcFrequence);
}

void Utils::Timer::Clear()
{
	m_counterStart = 0;
	m_counterEnd = 0;
	m_counterAcc = 0;
	m_pcFrequence = 0.0;
}

// Julian Date
// Julian dates are in DAYS (and fractions)
// JulianCalendar calendar = new JulianCalendar();
// Saturday, A.D. 2017 Jan 28	00:00:00.0	2457781.5
//    DateTime today = DateTime.Today;
//    DateTime dateInJulian = calendar.ToDateTime(today.Year, today.Month, today.Day, 0, 0, 0, 0);
// String ddd = calendar.ToString();
// float JD = 2457781.5f;
// T is in Julian centuries
// float T = ( JD - 2451545.0f ) / 36525.0f;

// From https://en.wikipedia.org/wiki/Julian_day
// Gregorian Calendar Date to Julian Day Number conversion

// Julian Day Number calculations.
// https://en.wikipedia.org/wiki/Julian_day
// https://aa.quae.nl/en/reken/juliaansedag.html
// https://core2.gsfc.nasa.gov/time/julian.txt
// http://www.cs.utsa.edu/~cs1063/projects/Spring2011/Project1/jdn-explanation.html
int32_t Utils::CalculateJulianDayNumber(int32_t year, int32_t month, int32_t day)
{
	// Formula coming from Wikipedia.
	int32_t a = (month - 14) / 12;
	int32_t jdn = (1461 * (year + 4800 + a)) / 4 +
		(367 * (month - 2 - 12 * a)) / 12 -
		(3 * ((year + 4900 + a) / 100)) / 4 +
		day - 32075;

	// Other formula found online:
	/*int m, y, leap_days;
	a = ( ( 14 - month ) / 12 );
	m = ( month - 3 ) + ( 12 * a );
	y = year + 4800 - a;
	leap_days = ( y / 4 ) - ( y / 100 ) + ( y / 400 );
	int32_t jdn2 = day + ( ( ( 153 * m ) + 2 ) / 5 ) + ( 365 * y ) + leap_days - 32045;*/

	return jdn;
}

double Utils::CalculateJulianDate(int32_t year, int32_t month, int32_t day, int32_t hour, int32_t minute, int32_t second)
{
	int32_t jdn = CalculateJulianDayNumber(year, month, day);

	double jd = jdn + ((hour - 12.0) / 24.0) + (minute / 1440.0) + (second / 86400.0);
	return jd;
}

double Utils::CalculateJulianCenturyDate(int32_t year, int32_t month, int32_t day, int32_t hour, int32_t minute, int32_t second)
{
	double jd = CalculateJulianDate(year, month, day, hour, minute, second);
	return (jd - s_j2000) / 36525.0;
}

glm::vec3 Utils::ConvertToEuclidean(float rightAscension, float declination, float radialDistance)
{
	const float cosd = cosf(declination);
	glm::vec3 out{};

	out.x = radialDistance * sinf(rightAscension) * cosd;
	out.y = radialDistance * cosf(rightAscension) * cosd;
	out.z = radialDistance * sinf(declination);

	return out;
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

WindowSizeAndPosition Utils::GetWindowSizeAndPosition(HWND hwnd)
{
	WindowSizeAndPosition result = {};

	if (GetWindowRect(hwnd, &result.m_windowRect))
	{
		result.m_width = result.m_windowRect.right - result.m_windowRect.left;
		result.m_height = result.m_windowRect.bottom - result.m_windowRect.top;
		result.m_xPos = result.m_windowRect.left;
		result.m_yPos = result.m_windowRect.top;
	}

	if (GetClientRect(hwnd, &result.m_clientRect))
	{
		result.m_clientWidth = result.m_clientRect.right - result.m_clientRect.left;
		result.m_clientHeight = result.m_clientRect.bottom - result.m_clientRect.top;
	}

	return result;
}