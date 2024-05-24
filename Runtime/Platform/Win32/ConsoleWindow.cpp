#include "ConsoleWindow.h"
#include <windows.h>
#include <io.h>
#include <fcntl.h>
#include <conio.h>
#include <string>
#include "Core/Utils.h"

using namespace Sailor::Win32;

namespace
{
	volatile bool consoleExit = false;
}

ConsoleWindow::ConsoleWindow(bool bInShouldAttach)
	: m_stdout_file(0)
	, m_stderr_file(0)
	, m_stdin_file(0)
	, m_bufferSize(0)
	, m_bShouldAttach(bInShouldAttach)
{
	if (bInShouldAttach)
	{
		Attach();
	}
}

void ConsoleWindow::Initialize(bool bInShouldAttach)
{
	SAILOR_PROFILE_FUNCTION();

	s_pInstance = new ConsoleWindow(bInShouldAttach);
}

BOOL WINAPI ConsoleHandler(DWORD signal)
{
	if (signal == CTRL_C_EVENT || signal == CTRL_BREAK_EVENT || signal == CTRL_CLOSE_EVENT)
	{
		consoleExit = true;
		Sleep(100);
	}
	return FALSE;
}

void ConsoleWindow::Attach()
{
	printf("");

	if (AttachConsole(ATTACH_PARENT_PROCESS)) {
		freopen_s(&m_stdout_file, "CONOUT$", "wb", stdout);
		freopen_s(&m_stderr_file, "CONOUT$", "wb", stderr);
		freopen_s(&m_stdin_file, "CONIN$", "rb", stdin);
		SetConsoleCtrlHandler(ConsoleHandler, TRUE);
	}
}

ConsoleWindow::~ConsoleWindow()
{
	Free();
}

void ConsoleWindow::Free()
{
	if (m_stdout_file != 0)
	{
		fclose(m_stdout_file);
	}

	if (m_stderr_file != 0)
	{
		fclose(m_stderr_file);
	}

	if (m_stdin_file != 0)
	{
		fclose(m_stdin_file);
	}

	m_stdout_file = 0;
	m_stderr_file = 0;
	m_stdin_file = 0;

	FreeConsole();
}

bool ConsoleWindow::IsExitRequested()
{
	return _exit != nullptr;
}

void ConsoleWindow::OpenWindow(const wchar_t* Title)
{
	Free();
	BOOL result = AllocConsole();
	SetConsoleCtrlHandler(ConsoleHandler, TRUE);

	SetConsoleTitleW(Title);

	freopen_s(&m_stdout_file, "CONOUT$", "wb", stdout);
	freopen_s(&m_stderr_file, "CONOUT$", "wb", stderr);
	freopen_s(&m_stdin_file, "CONIN$", "rb", stdin);
}

void ConsoleWindow::CloseWindow()
{
	Free();

	if (m_bShouldAttach)
	{
		Attach();
	}
}

void ConsoleWindow::Write(wchar_t c)
{
	DWORD written;
	WriteConsoleW(GetStdHandle(STD_OUTPUT_HANDLE), &c, 1, &written, 0);
}

void ConsoleWindow::Update()
{
	SAILOR_PROFILE_FUNCTION();

	DWORD num_events;
	BOOL result = GetNumberOfConsoleInputEvents(GetStdHandle(STD_INPUT_HANDLE), &num_events);
	if (!result)
		return;

	for (uint32_t i = 0; i < num_events; ++i)
	{
		INPUT_RECORD ir;
		DWORD was_read;
		result = ReadConsoleInputW(GetStdHandle(STD_INPUT_HANDLE), &ir, 1, &was_read);
		if (!result)
			break;

		if (ir.EventType != KEY_EVENT)
			continue;
		if (!ir.Event.KeyEvent.bKeyDown)
			continue;

		WCHAR c = ir.Event.KeyEvent.uChar.UnicodeChar;
		if (c == 0)
			continue;

		if (c == 8)
		{
			if (m_bufferSize == 0)
				continue;

			CONSOLE_SCREEN_BUFFER_INFO info;
			BOOL result = GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &info);
			if (!result)
				continue;

			if (info.dwCursorPosition.X == 0) 
			{
				info.dwCursorPosition.X = info.dwSize.X - 1;
				if (info.dwCursorPosition.Y > 0)
					--info.dwCursorPosition.Y;
			}
			else 
			{
				--info.dwCursorPosition.X;
			}

			SetConsoleCursorPosition(GetStdHandle(STD_OUTPUT_HANDLE), info.dwCursorPosition);
			Write(' ');
			SetConsoleCursorPosition(GetStdHandle(STD_OUTPUT_HANDLE), info.dwCursorPosition);

			--m_bufferSize;
			continue;
		}

		if (c == 0x1b)
			continue;

		if (m_bufferSize == LINE_BUFFER_SIZE)
			continue;

		if (c == 13) 
		{
			Write(13);
			Write(10);
		}
		else
			Write(c);

		m_buffer[m_bufferSize++] = c;
	}
}

uint32_t ConsoleWindow::Read(char* OutBuffer, uint32_t BufferSize)
{
	// find EOL
	static const uint32_t NO_POS = 0xffffffff;
	unsigned eol_pos = NO_POS;
	for (uint32_t i = 0; i < BufferSize; ++i) 
	{
		if (m_buffer[i] == 13) {
			eol_pos = i;
			break;
		}
	}

	if (eol_pos == NO_POS)
		return 0;

	char* out = OutBuffer;
	bool bSkipFirstSpaces = true;
	uint32_t j = 0;
	for (uint32_t i = 0; i < eol_pos; ++i)
	{
		char c = Utils::wchar_to_UTF8(&m_buffer[i]).c_str()[0];
		if (bSkipFirstSpaces)
		{
			if (c == '\0' || c == ' ')
			{
				continue;
			}
			bSkipFirstSpaces = false;
		}

		out[j++] = c;
	}
	out[j] = '\0';

	memmove(m_buffer, m_buffer + eol_pos + 1, BufferSize - eol_pos - 1);

	return (uint32_t)strlen(out);
}
