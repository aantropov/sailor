#include "ConsoleWindow.h"
#include <windows.h>
#include <io.h>
#include <fcntl.h>
#include <conio.h>
#include <string>

#include "Types.h"

ConsoleWindow* ConsoleWindow::instance = 0;

namespace
{
	volatile bool ConsoleExit = false; // This is set in case the console is signalled to close.
}

ConsoleWindow::ConsoleWindow(bool bInShouldAttach)
	: stdout_file(0)
	, stderr_file(0)
	, stdin_file(0)
	, bufferSize(0)
	, bShouldAttach(bInShouldAttach)
{
	if (bInShouldAttach)
	{
		Attach();
	}
}

ConsoleWindow::~ConsoleWindow()
{
	Free();
}

void ConsoleWindow::Initialize(bool bInShouldAttach)
{
	instance = new ConsoleWindow(bInShouldAttach);
}

void ConsoleWindow::Release()
{
	delete instance;
}

ConsoleWindow& ConsoleWindow::GetInstance()
{
	return *instance;
}

// Global handler for break and exit signals from the console.
BOOL WINAPI console_handler(DWORD signal) {
	if (signal == CTRL_C_EVENT || signal == CTRL_BREAK_EVENT || signal == CTRL_CLOSE_EVENT) {
#if defined(USE_FILE_LOG)
		// Add failsafe here as there is cases where the log would be cut/empty otherwise.
		if (FileLog::exists())
			FileLog::get().flush();
#endif
		ConsoleExit = true;
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
		freopen_s(&stdout_file, "CONOUT$", "wb", stdout);
		freopen_s(&stderr_file, "CONOUT$", "wb", stderr);
		freopen_s(&stdin_file, "CONIN$", "rb", stdin);
		SetConsoleCtrlHandler(console_handler, TRUE);
	}
}

void ConsoleWindow::Free()
{
	if (stdout_file != 0)
	{
		fclose(stdout_file);
	}
	
	if (stderr_file != 0)
	{
		fclose(stderr_file);
	}
	
	if (stdin_file != 0)
	{
		fclose(stdin_file);
	}

	stdout_file = 0;
	stderr_file = 0;
	stdin_file = 0;
	
	FreeConsole();
}

bool ConsoleWindow::IsExitRequested() {
	return _exit;
}

void ConsoleWindow::OpenWindow(const wchar_t* Title)
{
	Free();
	BOOL result = AllocConsole();
	SetConsoleCtrlHandler(console_handler, TRUE);

	SetConsoleTitleW(Title);

	freopen_s(&stdout_file, "CONOUT$", "wb", stdout);
	freopen_s(&stderr_file, "CONOUT$", "wb", stderr);
	freopen_s(&stdin_file, "CONIN$", "rb", stdin);
}

void ConsoleWindow::CloseWindow()
{
	Free();
	
	if (bShouldAttach)
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

	for (unsigned int i = 0; i < num_events; ++i) {
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
			if (bufferSize == 0)
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

			--bufferSize;
			continue;
		}
		// ignore escape
		if (c == 0x1b)
			continue;

		if (bufferSize == LINE_BUFFER_SIZE)
			continue;

		// echo
		if (c == 13) {
			Write(13);
			Write(10);
		}
		else
			Write(c);

		buffer[bufferSize++] = c;
	}
}

unsigned int ConsoleWindow::Read(char* OutBuffer, unsigned int BufferSize)
{
	// find EOL
	static const unsigned int NO_POS = 0xffffffff;
	unsigned eol_pos = NO_POS;
	for (unsigned int i = 0; i < BufferSize; ++i) {
		if (buffer[i] == 13) {
			eol_pos = i;
			break;
		}
	}
	if (eol_pos == NO_POS)
		return 0;

	char* out = OutBuffer;
	/*for (unsigned int i = 0; i < eol_pos; ++i) {
		// crop if output string does not fit
		if (buffer + buffer_size - out <= 4)
			break;
		out = encoding::utf8_encode(_buffer[i], out);
	}*/

	// adjust buffer
	memmove(buffer, buffer + eol_pos + 1, BufferSize - eol_pos - 1);

	BufferSize -= eol_pos + 1;
	return (unsigned)(out - OutBuffer);
}
