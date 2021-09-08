#include "ConsoleWindow.h"
#include <windows.h>
#include <io.h>
#include <fcntl.h>
#include <conio.h>
#include <string>
#include "Utils.h"

using namespace Sailor::Win32;

namespace
{
	volatile bool ñonsoleExit = false; // This is set in case the console is signalled to close.
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

	m_pInstance = new ConsoleWindow(bInShouldAttach);
}

// Global handler for break and exit signals from the console.
BOOL WINAPI console_handler(DWORD signal) {
	if (signal == CTRL_C_EVENT || signal == CTRL_BREAK_EVENT || signal == CTRL_CLOSE_EVENT) {
#if defined(USE_FILE_LOG)
		// Add failsafe here as there is cases where the log would be cut/empty otherwise.
		if (FileLog::exists())
			FileLog::get().flush();
#endif
		ñonsoleExit = true;
		// Sleep here to give application time to close down nicely. After this time
		// ExitProcess will be called and force the application to stop immediately.
		Sleep(100);
	}
	return FALSE;
}

void ConsoleWindow::Attach()
{
	// This weird print statement is needed here, because otherwise, GetConsoleScreenBufferInfo() will
	// fail after AttachConsole. I don't know the reason for this weird Windows magic.
	printf("");

	if (AttachConsole(ATTACH_PARENT_PROCESS)) {
		freopen_s(&m_stdout_file, "CONOUT$", "wb", stdout);
		freopen_s(&m_stderr_file, "CONOUT$", "wb", stderr);
		freopen_s(&m_stdin_file, "CONIN$", "rb", stdin);
		SetConsoleCtrlHandler(console_handler, TRUE);
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
	SetConsoleCtrlHandler(console_handler, TRUE);

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
	DWORD num_events;
	BOOL result = GetNumberOfConsoleInputEvents(GetStdHandle(STD_INPUT_HANDLE), &num_events);
	if (!result)
		return;

	for (uint32_t i = 0; i < num_events; ++i) {
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

		// handle backspace
		if (c == 8) {
			if (m_bufferSize == 0)
				continue;

			// determine location to the left of the cursor
			CONSOLE_SCREEN_BUFFER_INFO info;
			BOOL result = GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &info);
			if (!result)
				continue;

			if (info.dwCursorPosition.X == 0) {
				// wrap to above row
				info.dwCursorPosition.X = info.dwSize.X - 1;
				if (info.dwCursorPosition.Y > 0)
					--info.dwCursorPosition.Y;
			}
			else {
				--info.dwCursorPosition.X;
			}

			// move to character before
			SetConsoleCursorPosition(GetStdHandle(STD_OUTPUT_HANDLE), info.dwCursorPosition);
			// overwrite with space
			Write(' ');
			// move to character before
			SetConsoleCursorPosition(GetStdHandle(STD_OUTPUT_HANDLE), info.dwCursorPosition);

			--m_bufferSize;
			continue;
		}
		// ignore escape
		if (c == 0x1b)
			continue;

		if (m_bufferSize == LINE_BUFFER_SIZE)
			continue;

		// echo
		if (c == 13) {
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
	for (uint32_t i = 0; i < BufferSize; ++i) {
		if (m_buffer[i] == 13) {
			eol_pos = i;
			break;
		}
	}
	if (eol_pos == NO_POS)
		return 0;

	char* out = OutBuffer;
	/*for (uint32_t i = 0; i < eol_pos; ++i) {
		// crop if output string does not fit
		if (buffer + buffer_size - out <= 4)
			break;
		out = encoding::utf8_encode(_buffer[i], out);
	}*/

	// adjust buffer
	memmove(m_buffer, m_buffer + eol_pos + 1, BufferSize - eol_pos - 1);

	BufferSize -= eol_pos + 1;
	return (unsigned)(out - OutBuffer);
}
