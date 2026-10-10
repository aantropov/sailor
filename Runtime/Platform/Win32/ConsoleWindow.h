#pragma once
#include <cstdint>
#include <cstdio>
#include <string_view>
#include "Core/Singleton.hpp"
#if defined(_WIN32)
#include <wtypes.h>
#endif

namespace Sailor::Tests { struct ConsoleWindowTestAccess; }

namespace Sailor::Win32
{
	class ConsoleWindow : public TSingleton<ConsoleWindow>
	{
	public:
		static constexpr uint32_t LineBufferSize = 256;
		static constexpr uint32_t MaxCommandBytes = LineBufferSize * 4;

		static SAILOR_API void Initialize(bool bInShouldAttach);
		static SAILOR_API void Shutdown();
		static SAILOR_API bool IsExitRequested();

		SAILOR_API void OpenWindow(const wchar_t* pTitle);
		SAILOR_API void CloseWindow();

		// Returns UTF-8 bytes excluding the terminator. A small output retains the complete line for retry.
		SAILOR_API uint32_t Read(char* pOutBuffer, uint32_t bufferSize);

		SAILOR_API void Update();

		SAILOR_API virtual ~ConsoleWindow() override;

	private:
		friend struct Sailor::Tests::ConsoleWindowTestAccess;

		SAILOR_API ConsoleWindow(bool bInShouldAttach);

		SAILOR_API void Attach();
		SAILOR_API void Free();
		SAILOR_API void Write(std::wstring_view text);
		SAILOR_API bool AppendInput(wchar_t c);
		static SAILOR_API void RequestExit(bool bWaitForShutdown);
#if defined(_WIN32)
		static BOOL WINAPI HandleControl(DWORD signal);
#endif

		FILE* m_stdout_file;
		FILE* m_stderr_file;
		FILE* m_stdin_file;

		bool m_bShouldAttach;
		bool m_bIsOpen = false;
		bool m_bIsLineOverflowed = false;
		uint32_t m_bufferSize = 0;
		wchar_t m_buffer[LineBufferSize]{};
	};
}
