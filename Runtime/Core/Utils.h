#pragma once
#include <string>
#include <thread>
#include "Sailor.h"
#include "Containers/Containers.h"
#include <ctime>

namespace Sailor
{
	namespace Utils
	{
		SAILOR_API std::string wchar_to_UTF8(const wchar_t* in);
		SAILOR_API std::wstring UTF8_to_wchar(const char* in);

		SAILOR_API std::string RemoveFileExtension(const std::string& filename);
		SAILOR_API std::string SanitizeFilepath(const std::string& filename);
		SAILOR_API std::string GetFileExtension(const std::string& filename);
		SAILOR_API std::time_t GetFileModificationTime(const std::string& filepath);
		SAILOR_API std::string GetFileFolder(const std::string& filepath);

		SAILOR_API TVector<std::string> SplitString(const std::string& str, const std::string& delimiter);
		SAILOR_API void ReplaceAll(std::string& str, const std::string& from, const std::string& to, size_t startPos = 0, size_t endPos = std::string::npos);
		SAILOR_API void Erase(std::string& str, const std::string& substr, size_t startPos = 0, size_t endPos = std::string::npos);

		SAILOR_API void FindAllOccurances(std::string& str, const std::string& substr, TVector<size_t>& outLocations, size_t startPos = 0, size_t endPos = std::string::npos);

		SAILOR_API void Trim(std::string& s);

		SAILOR_API void SetThreadName(uint32_t dwThreadID, const std::string& threadName);
		SAILOR_API void SetThreadName(const std::string& threadName);
		SAILOR_API void SetThreadName(std::thread* thread, const std::string& threadName);

		SAILOR_API DWORD GetRandomColorHex();

		SAILOR_API int64_t GetCurrentTimeMs();
		SAILOR_API int64_t GetCurrentTimeMicro();
		SAILOR_API int64_t GetCurrentTimeNano();

		struct SAILOR_API Timer
		{
			int64_t m_counterStart = 0;
			int64_t m_counterEnd = 0;
			int64_t m_counterAcc = 0;
			double m_pcFrequence = 0.0;

			void Start();
			void Stop();

			int64_t ResultMs() const;
			int64_t ResultAccumulatedMs() const;

			void Clear();
		};
	}

	template<typename Class, typename Member>
	constexpr size_t OffsetOf(Member Class::* member)
	{
		return (char*)&((Class*)nullptr->*member) - (char*)nullptr;
	}
}