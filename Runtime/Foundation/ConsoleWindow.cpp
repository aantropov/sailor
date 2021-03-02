#include "ConsoleWindow.h"
#include <windows.h>
#include <io.h>
#include <fcntl.h>
#include <conio.h>
#include <string>

#include "Types.h"

ConsoleWindow* ConsoleWindow::__instance = 0;

namespace {
	volatile bool ConsoleExit = false; // This is set in case the console is signalled to close.
}

ConsoleWindow::ConsoleWindow(bool to_attach)
	: _stdout_file(0)
	, _stderr_file(0)
	, _stdin_file(0)
	, _buffer_size(0)
	, _attach(to_attach)
{
	if (_attach)
		attach();
}

ConsoleWindow::~ConsoleWindow()
{
	free();
}

void ConsoleWindow::setup(bool to_attach)
{	
	__instance = new ConsoleWindow(to_attach);
}

void ConsoleWindow::shutdown()
{
	if(__instance != nullptr)
	{
		delete __instance;
	}
}

ConsoleWindow& ConsoleWindow::get()
{
	return *__instance;
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
		Sleep(10000);
	}
	return FALSE;
}

void ConsoleWindow::attach()
{
	// This weird print statement is needed here, because otherwise, GetConsoleScreenBufferInfo() will
	// fail after AttachConsole. I don't know the reason for this weird Windows magic.
	printf("");

	if (AttachConsole(ATTACH_PARENT_PROCESS)) {
		freopen_s(&_stdout_file, "CONOUT$", "wb", stdout);
		freopen_s(&_stderr_file, "CONOUT$", "wb", stderr);
		freopen_s(&_stdin_file, "CONIN$", "rb", stdin);
		SetConsoleCtrlHandler(console_handler, TRUE);
	}
}

void ConsoleWindow::free()
{
	if (_stdout_file != 0)
		fclose(_stdout_file);
	if (_stderr_file != 0)
		fclose(_stderr_file);
	if (_stdin_file != 0)
		fclose(_stdin_file);

	_stdout_file = 0;
	_stderr_file = 0;
	_stdin_file = 0;
	FreeConsole();
}

bool ConsoleWindow::exit_requested() {
	return _exit;
}

void ConsoleWindow::open_window(const char* title)
{
	free();
	BOOL result = AllocConsole();
	SetConsoleCtrlHandler(console_handler, TRUE);

	const wchar_t* wstr = UTF8_to_wchar(title).c_str();
	SetConsoleTitleW(wstr);

	freopen_s(&_stdout_file, "CONOUT$", "wb", stdout);
	freopen_s(&_stderr_file, "CONOUT$", "wb", stderr);
	freopen_s(&_stdin_file, "CONIN$", "rb", stdin);
}

void ConsoleWindow::close_window()
{
	free();
	if (_attach)
		attach();
}

void ConsoleWindow::write(wchar_t c)
{
	DWORD written;
	WriteConsoleW(GetStdHandle(STD_OUTPUT_HANDLE), &c, 1, &written, 0);
}

void ConsoleWindow::update()
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
			if (_buffer_size == 0)
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
			write(' ');
			// move to character before
			SetConsoleCursorPosition(GetStdHandle(STD_OUTPUT_HANDLE), info.dwCursorPosition);

			--_buffer_size;
			continue;
		}
		// ignore escape
		if (c == 0x1b)
			continue;

		if (_buffer_size == LINE_BUFFER_SIZE)
			continue;

		// echo
		if (c == 13) {
			write(13);
			write(10);
		}
		else
			write(c);

		_buffer[_buffer_size++] = c;
	}
}

unsigned int ConsoleWindow::read(char* buffer, unsigned int buffer_size)
{
	// find EOL
	static const unsigned int NO_POS = 0xffffffff;
	unsigned eol_pos = NO_POS;
	for (unsigned int i = 0; i < _buffer_size; ++i) {
		if (_buffer[i] == 13) {
			eol_pos = i;
			break;
		}
	}
	if (eol_pos == NO_POS)
		return 0;

	char* out = buffer;
	/*for (unsigned int i = 0; i < eol_pos; ++i) {
		// crop if output string does not fit
		if (buffer + buffer_size - out <= 4)
			break;
		out = encoding::utf8_encode(_buffer[i], out);
	}*/

	// adjust buffer
	memmove(_buffer, _buffer + eol_pos + 1, _buffer_size - eol_pos - 1);

	_buffer_size -= eol_pos + 1;
	return (unsigned)(out - buffer);
}
