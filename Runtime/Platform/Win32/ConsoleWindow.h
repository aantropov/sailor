#pragma once
#include <cstdio>
#include "Sailor.h"
#include "Core/Singleton.hpp"

namespace Sailor::Win32
{
	class ConsoleWindow : public TSingleton<ConsoleWindow>
	{
	public:

		static SAILOR_API void Initialize(bool bInShouldAttach);

		SAILOR_API void OpenWindow(const wchar_t* pTitle);
		SAILOR_API void CloseWindow();

		uint32_t Read(char* pOutBuffer, uint32_t bufferSize);

		SAILOR_API void Update();

		SAILOR_API virtual ~ConsoleWindow() override;

	private:

		SAILOR_API ConsoleWindow(bool bInShouldAttach);

		SAILOR_API void Attach();
		SAILOR_API void Free();
		SAILOR_API void Write(wchar_t c);

		FILE* m_stdout_file;
		FILE* m_stderr_file;
		FILE* m_stdin_file;

		bool m_bShouldAttach;

		static const uint32_t LINE_BUFFER_SIZE = 256;

		uint32_t m_bufferSize;

		wchar_t m_buffer[LINE_BUFFER_SIZE];
	};
}