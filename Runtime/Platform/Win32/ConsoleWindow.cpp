#include "ConsoleWindow.h"
#include "Core/Utils.h"
#include <algorithm>
#include <atomic>
#include <cstring>
using namespace Sailor::Win32;

namespace
{
	// Only the standalone host opens a console; its runtime stays loaded until process exit.
	// Control callbacks may outlive the console object and only access these signals.
	std::atomic<bool> g_bIsExitRequested{ false };
	std::atomic<bool> g_bIsShutdownCompleted{ false };
}

ConsoleWindow::ConsoleWindow(bool bInShouldAttach)
	: m_stdout_file(nullptr)
	, m_stderr_file(nullptr)
	, m_stdin_file(nullptr)
	, m_bShouldAttach(bInShouldAttach)
{
	if (m_bShouldAttach) Attach();
}

void ConsoleWindow::Initialize(bool bInShouldAttach)
{
	g_bIsExitRequested.store(false, std::memory_order_relaxed);
	g_bIsShutdownCompleted.store(false, std::memory_order_relaxed);
	s_pInstance = new ConsoleWindow(bInShouldAttach);
}

void ConsoleWindow::Shutdown()
{
	TSingleton<ConsoleWindow>::Shutdown();
	g_bIsShutdownCompleted.store(true, std::memory_order_release);
	g_bIsShutdownCompleted.notify_all();
}

bool ConsoleWindow::IsExitRequested()
{
	return g_bIsExitRequested.load(std::memory_order_acquire);
}

void ConsoleWindow::RequestExit(bool bWaitForShutdown)
{
	g_bIsExitRequested.store(true, std::memory_order_release);
	// Returning from CTRL_CLOSE_EVENT terminates the process, even when handled.
	if (bWaitForShutdown) g_bIsShutdownCompleted.wait(false, std::memory_order_acquire);
}

ConsoleWindow::~ConsoleWindow()
{
	Free();
}

void ConsoleWindow::CloseWindow()
{
	Free();
	if (m_bShouldAttach) Attach();
}

#ifdef _WIN32
#include <windows.h>

BOOL WINAPI ConsoleWindow::HandleControl(DWORD signal)
{
	if (signal != CTRL_C_EVENT && signal != CTRL_BREAK_EVENT && signal != CTRL_CLOSE_EVENT) return FALSE;
	RequestExit(signal == CTRL_CLOSE_EVENT);
	return TRUE;
}

void ConsoleWindow::Attach()
{
	if (AttachConsole(ATTACH_PARENT_PROCESS))
	{
		m_bIsOpen = true;
		freopen_s(&m_stdout_file, "CONOUT$", "wb", stdout);
		freopen_s(&m_stderr_file, "CONOUT$", "wb", stderr);
		freopen_s(&m_stdin_file, "CONIN$", "rb", stdin);
		SetConsoleCtrlHandler(HandleControl, TRUE);
	}
}

void ConsoleWindow::Free()
{
	m_bufferSize = 0;
	m_bIsLineOverflowed = false;
	if (!m_bIsOpen) return;
	SetConsoleCtrlHandler(HandleControl, FALSE);
	m_bIsOpen = false;
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

void ConsoleWindow::OpenWindow(const wchar_t* Title)
{
	Free();
	BOOL result = AllocConsole();
	if (result)
	{
		m_bIsOpen = true;
		SetConsoleCtrlHandler(HandleControl, TRUE);

		SetConsoleTitleW(Title);

		freopen_s(&m_stdout_file, "CONOUT$", "wb", stdout);
		freopen_s(&m_stderr_file, "CONOUT$", "wb", stderr);
		freopen_s(&m_stdin_file, "CONIN$", "rb", stdin);
	}
}

void ConsoleWindow::Write(std::wstring_view text)
{
	DWORD written;
	WriteConsoleW(GetStdHandle(STD_OUTPUT_HANDLE), text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
}

void ConsoleWindow::Update()
{
	if (!m_bIsOpen) return;
	SAILOR_PROFILE_FUNCTION();
	if (std::find(m_buffer, m_buffer + m_bufferSize, L'\r') != m_buffer + m_bufferSize) return;

	DWORD numEvents;
	const auto input = GetStdHandle(STD_INPUT_HANDLE);
	if (!GetNumberOfConsoleInputEvents(input, &numEvents)) return;

	for (uint32_t i = 0; i < numEvents; ++i)
	{
		INPUT_RECORD event;
		DWORD read;
		if (!ReadConsoleInputW(input, &event, 1, &read) || read == 0) break;
		if (event.EventType != KEY_EVENT || !event.Event.KeyEvent.bKeyDown) continue;

		const wchar_t c = event.Event.KeyEvent.uChar.UnicodeChar;
		for (uint32_t repeat = 0; repeat < event.Event.KeyEvent.wRepeatCount; ++repeat)
		{
			if (!AppendInput(c)) continue;
			if (c == L'\b')
			{
				CONSOLE_SCREEN_BUFFER_INFO info;
				const auto output = GetStdHandle(STD_OUTPUT_HANDLE);
				if (!GetConsoleScreenBufferInfo(output, &info)) continue;
				if (info.dwCursorPosition.X == 0)
				{
					info.dwCursorPosition.X = info.dwSize.X - 1;
					if (info.dwCursorPosition.Y > 0) --info.dwCursorPosition.Y;
				}
				else --info.dwCursorPosition.X;
				SetConsoleCursorPosition(output, info.dwCursorPosition);
				Write(L" ");
				SetConsoleCursorPosition(output, info.dwCursorPosition);
			}
			else if (c == L'\r')
			{
				Write(L"\r\n");
				return; // Leave the next line in the native input queue until Read consumes this one.
			}
			else if (c >= 0xdc00 && c <= 0xdfff && m_bufferSize >= 2 &&
				m_buffer[m_bufferSize - 2] >= 0xd800 && m_buffer[m_bufferSize - 2] <= 0xdbff)
			{
				Write(std::wstring_view(m_buffer + m_bufferSize - 2, 2));
			}
			else if (c < 0xd800 || c > 0xdbff) Write(std::wstring_view(&c, 1));
		}
	}
}

#else

void ConsoleWindow::Attach()
{
}

void ConsoleWindow::Free()
{
	m_bufferSize = 0;
	m_bIsLineOverflowed = false;
}

void ConsoleWindow::OpenWindow(const wchar_t* Title)
{
	(void)Title;
	Free();
}

void ConsoleWindow::Write(std::wstring_view text)
{
	(void)text;
}

void ConsoleWindow::Update()
{
}

#endif

bool ConsoleWindow::AppendInput(wchar_t c)
{
	if (c == 0 || c == L'\x1b' || c == L'\n') return false;
	if (c == L'\b')
	{
		if (m_bIsLineOverflowed || m_bufferSize == 0 || m_buffer[m_bufferSize - 1] == L'\r') return false;
		const auto last = m_buffer[--m_bufferSize];
		if (last >= 0xdc00 && last <= 0xdfff && m_bufferSize > 0 &&
			m_buffer[m_bufferSize - 1] >= 0xd800 && m_buffer[m_bufferSize - 1] <= 0xdbff) --m_bufferSize;
		return true;
	}
	if (c == L'\r')
	{
		if (m_bIsLineOverflowed)
		{
			// Discard the overlong command, never execute its truncated prefix.
			while (m_bufferSize > 0 && m_buffer[m_bufferSize - 1] != L'\r') --m_bufferSize;
			m_bIsLineOverflowed = false;
			return true;
		}
		if (m_bufferSize == LineBufferSize) return false;
	}
	else if (m_bIsLineOverflowed || m_bufferSize >= LineBufferSize - 1)
	{
		m_bIsLineOverflowed = true;
		return false;
	}
	m_buffer[m_bufferSize++] = c;
	return true;
}

uint32_t ConsoleWindow::Read(char* outBuffer, uint32_t bufferSize)
{
	if (!outBuffer || bufferSize == 0) return 0;
	outBuffer[0] = '\0';
	const auto end = std::find(m_buffer, m_buffer + m_bufferSize, L'\r');
	if (end == m_buffer + m_bufferSize) return 0;

	std::wstring_view line(m_buffer, static_cast<size_t>(end - m_buffer));
	while (!line.empty() && line.front() == L' ') line.remove_prefix(1);
	const std::string text = Sailor::Utils::wchar_to_UTF8(line);
	// A smaller caller may retry; do not consume or split a UTF-8 command.
	if (text.size() >= bufferSize) return 0;
	std::memcpy(outBuffer, text.data(), text.size());
	outBuffer[text.size()] = '\0';

	const auto consumed = static_cast<uint32_t>(end - m_buffer) + 1;
	m_bufferSize -= consumed;
	std::memmove(m_buffer, m_buffer + consumed, m_bufferSize * sizeof(wchar_t));
	return static_cast<uint32_t>(text.size());
}
