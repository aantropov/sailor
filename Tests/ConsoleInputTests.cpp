#include "Platform/Win32/ConsoleWindow.h"

#include <array>
#include <chrono>
#include <future>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

#if defined(_WIN32)
#include "Support/ScopeExit.h"
#include "Support/TempDirectory.h"
#include <fstream>
#include <windows.h>
#endif

namespace Sailor::Tests
{
	struct ConsoleWindowTestAccess
	{
		static Win32::ConsoleWindow Make() { return Win32::ConsoleWindow(false); }
		static void RequestExit(bool bWaitForShutdown) { Win32::ConsoleWindow::RequestExit(bWaitForShutdown); }

		static bool Feed(Win32::ConsoleWindow& console, std::wstring_view input)
		{
			bool bAccepted = true;
			for (wchar_t c : input) bAccepted = console.AppendInput(c) && bAccepted;
			return bAccepted;
		}
	};
}

namespace
{
	using Sailor::Tests::ConsoleWindowTestAccess;
	using Sailor::Win32::ConsoleWindow;

	void Require(bool value, std::string_view message)
	{
		if (!value) throw std::runtime_error(std::string(message));
	}

	void TestUnicodeLine()
	{
		auto console = ConsoleWindowTestAccess::Make();
		ConsoleWindowTestAccess::Feed(console, L"\u00e9\u8239\U0001f6a2\r");
		std::array<char, 64> output{};
		const auto length = console.Read(output.data(), static_cast<uint32_t>(output.size()));
		Require(std::string_view(output.data(), length) == "\xc3\xa9\xe8\x88\xb9\xf0\x9f\x9a\xa2",
			"console input must preserve complete UTF-8 characters");
	}

	void TestSequentialLines()
	{
		auto console = ConsoleWindowTestAccess::Make();
		ConsoleWindowTestAccess::Feed(console, L"one\rsecond line\r");
		std::array<char, 64> output{};
		Require(console.Read(output.data(), static_cast<uint32_t>(output.size())) == 3 && std::string_view(output.data()) == "one",
			"the first read must consume one complete line");
		Require(console.Read(output.data(), static_cast<uint32_t>(output.size())) == 11 && std::string_view(output.data()) == "second line",
			"the next line must survive consumption of its predecessor");
		Require(console.Read(output.data(), static_cast<uint32_t>(output.size())) == 0 && output[0] == '\0',
			"an empty read must not expose the previous command");
	}

	void TestBoundedOutput()
	{
		auto console = ConsoleWindowTestAccess::Make();
		std::array<char, 16> output;
		output.fill('!');
		Require(console.Read(output.data() + 1, 1) == 0 && output[1] == '\0' && output[0] == '!' && output[2] == '!',
			"empty input must write only the output terminator");
		Require(ConsoleWindowTestAccess::Feed(console, L"\u00e9\rnext\r"), "bounded-output fixture must queue both lines");
		Require(console.Read(nullptr, 64) == 0 && console.Read(output.data(), 0) == 0 && output[0] == '!',
			"an absent output must not consume queued input");
		Require(console.Read(output.data() + 1, 2) == 0 && output[1] == '\0' && output[0] == '!' && output[3] == '!',
			"a small output must not split UTF-8 or write beyond its capacity");
		Require(console.Read(output.data(), static_cast<uint32_t>(output.size())) == 2 &&
			std::string_view(output.data()) == "\xc3\xa9", "a larger output must receive the complete retained line");
		Require(console.Read(output.data(), static_cast<uint32_t>(output.size())) == 4 && std::string_view(output.data()) == "next",
			"retrying a small output must preserve subsequent lines");
	}

	void TestContinuousInput()
	{
		auto console = ConsoleWindowTestAccess::Make();
		std::array<char, ConsoleWindow::MaxCommandBytes> output{};
		for (uint32_t i = 0; i < 1000; ++i)
		{
			Require(ConsoleWindowTestAccess::Feed(console, L" scan\r"), "consumption must make room for the next command");
			Require(console.Read(output.data(), static_cast<uint32_t>(output.size())) == 4 && std::string_view(output.data()) == "scan",
				"more than 256 cumulative characters must not exhaust the input buffer");
		}
	}

	void TestFullAndOverflowedLines()
	{
		auto console = ConsoleWindowTestAccess::Make();
		std::array<char, ConsoleWindow::MaxCommandBytes> output{};
		std::wstring line(ConsoleWindow::LineBufferSize - 1, L'\u8239');
		Require(ConsoleWindowTestAccess::Feed(console, line) && ConsoleWindowTestAccess::Feed(console, L"\r"),
			"the input limit must reserve space for Enter");
		Require(console.Read(output.data(), static_cast<uint32_t>(output.size())) == 3 * line.size(),
			"the full wide line must fit in the engine's UTF-8 command buffer");
		for (size_t i = 0; i < line.size(); ++i)
			Require(std::string_view(output.data() + 3 * i, 3) == "\xe8\x88\xb9", "a full line must preserve every character");

		Require(ConsoleWindowTestAccess::Feed(console, L"kept\r"), "an earlier complete line must fit");
		Require(!ConsoleWindowTestAccess::Feed(console, std::wstring(ConsoleWindow::LineBufferSize + 20, L'x')),
			"overlong input must be rejected");
		ConsoleWindowTestAccess::Feed(console, L"\rnext\r");
		Require(console.Read(output.data(), static_cast<uint32_t>(output.size())) == 4 && std::string_view(output.data()) == "kept",
			"rejecting an overlong line must preserve previously completed commands");
		Require(console.Read(output.data(), static_cast<uint32_t>(output.size())) == 4 && std::string_view(output.data()) == "next",
			"an overflow must discard the whole command and recover at Enter");
		Require(console.Read(output.data(), static_cast<uint32_t>(output.size())) == 0,
			"a discarded command must not reappear from stale storage");
	}

	void TestEditingAndReset()
	{
		auto console = ConsoleWindowTestAccess::Make();
		std::array<char, ConsoleWindow::MaxCommandBytes> output{};
		const wchar_t edited[] = { L'A', 0xd83d, 0xdea2, L'\b', L'\r' };
		Require(ConsoleWindowTestAccess::Feed(console, std::wstring_view(edited, std::size(edited))), "surrogate-pair input must fit");
		Require(console.Read(output.data(), static_cast<uint32_t>(output.size())) == 1 && std::string_view(output.data()) == "A",
			"Backspace must remove a whole surrogate pair");
		ConsoleWindowTestAccess::Feed(console, L"one\r");
		Require(!ConsoleWindowTestAccess::Feed(console, L"\b"), "Backspace must not edit an already submitted line");
		ConsoleWindowTestAccess::Feed(console, L"two\r");
		Require(console.Read(output.data(), static_cast<uint32_t>(output.size())) == 3 && std::string_view(output.data()) == "one",
			"editing must preserve the earlier submitted line");
		Require(console.Read(output.data(), static_cast<uint32_t>(output.size())) == 3 && std::string_view(output.data()) == "two",
			"editing must preserve the next submitted line");
		ConsoleWindowTestAccess::Feed(console, L"\r\n pending");
		Require(console.Read(output.data(), static_cast<uint32_t>(output.size())) == 0,
			"an empty line must be consumed without a command");
		Require(console.Read(output.data(), static_cast<uint32_t>(output.size())) == 0,
			"an incomplete line must not be dispatched");
		console.CloseWindow();
		ConsoleWindowTestAccess::Feed(console, L"fresh\r");
		console.Update();
		Require(console.Read(output.data(), static_cast<uint32_t>(output.size())) == 5 && std::string_view(output.data()) == "fresh",
			"closing must clear pending input, and an unopened native console must not poll input");
	}

	void TestExitRequest()
	{
		for (uint32_t session = 0; session < 3; ++session)
		{
			ConsoleWindow::Initialize(false);
			const bool bStartedClean = !ConsoleWindow::IsExitRequested();
			auto request = std::async(std::launch::async, [] { ConsoleWindowTestAccess::RequestExit(false); });
			const auto status = request.wait_for(std::chrono::seconds(1));
			const bool bRequested = ConsoleWindow::IsExitRequested();
			ConsoleWindow::Shutdown();
			request.get();
			Require(bStartedClean && bRequested, "each console lifecycle must reset and receive its own stop request");
			Require(status == std::future_status::ready, "Ctrl-C and Ctrl-Break must request stop without waiting for shutdown");
		}
	}

	void TestShutdownHandoff()
	{
		ConsoleWindow::Initialize(false);
		auto request = std::async(std::launch::async, [] { ConsoleWindowTestAccess::RequestExit(true); });
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
		while (!ConsoleWindow::IsExitRequested() && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
		const bool bRequested = ConsoleWindow::IsExitRequested();
		const auto beforeShutdown = request.wait_for(std::chrono::milliseconds(20));
		{
			auto console = ConsoleWindowTestAccess::Make();
			console.CloseWindow();
		} // Destroying the console object is not completion of engine shutdown.
		const auto afterClose = request.wait_for(std::chrono::milliseconds(20));
		ConsoleWindow::Shutdown();
		request.get();
		Require(bRequested, "the main loop must observe a console-close request from another thread");
		Require(beforeShutdown == std::future_status::timeout && afterClose == std::future_status::timeout,
			"a close callback must wait through console closure and return only after engine shutdown completes");
		ConsoleWindowTestAccess::RequestExit(true);
	}

#if defined(_WIN32)
	int RunNativeConsole()
	{
		// OpenWindow redirects the CRT streams; retain the parent's report file separately.
		HANDLE report = nullptr;
		if (!DuplicateHandle(GetCurrentProcess(), GetStdHandle(STD_OUTPUT_HANDLE),
			GetCurrentProcess(), &report, 0, FALSE, DUPLICATE_SAME_ACCESS))
		{
			return 1;
		}
		Sailor::Tests::ScopeExit closeReport([&]() { CloseHandle(report); });
		try
		{
			for (UINT codePage : { 1251u, 1252u })
			{
				for (DWORD signal : { CTRL_C_EVENT, CTRL_BREAK_EVENT })
				{
					ConsoleWindow::Initialize(false);
					Sailor::Tests::ScopeExit shutdown([]() { ConsoleWindow::Shutdown(); });
					auto console = ConsoleWindowTestAccess::Make();
					console.OpenWindow(L"Sailor console test");
					Require(GetConsoleWindow() != nullptr, "the detached child must allocate its own console");
					ShowWindow(GetConsoleWindow(), SW_HIDE);
					Require(SetConsoleCP(codePage) && SetConsoleOutputCP(codePage), "the native code page must be set");
					Require(SetConsoleCtrlHandler(nullptr, FALSE), "Ctrl-C must not be inherited as ignored");

					// The fixture writes input and reads output, opposite to the engine's streams.
					const auto input = CreateFileW(L"CONIN$", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
						nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
					Require(input != INVALID_HANDLE_VALUE, "the native input queue must be writable by the fixture");
					Sailor::Tests::ScopeExit closeInput([&]() { CloseHandle(input); });
					const auto output = CreateFileW(L"CONOUT$", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
						nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
					Require(output != INVALID_HANDLE_VALUE, "the native echo must be readable by the fixture");
					Sailor::Tests::ScopeExit closeOutput([&]() { CloseHandle(output); });
					Require(FlushConsoleInputBuffer(input), "the native input queue must be empty before injection");
					Require(SetConsoleCursorPosition(output, { 0, 0 }), "the native output cursor must be ready");
					for (wchar_t character : std::wstring_view(L"\u041a\u00e9x\b\rnext\r"))
					{
						INPUT_RECORD event{};
						event.EventType = KEY_EVENT;
						event.Event.KeyEvent.bKeyDown = TRUE;
						event.Event.KeyEvent.wRepeatCount = 1;
						event.Event.KeyEvent.uChar.UnicodeChar = character;
						DWORD written = 0;
						Require(WriteConsoleInputW(input, &event, 1, &written) && written == 1,
							"each key must enter the actual Win32 input queue");
					}

					std::array<char, ConsoleWindow::MaxCommandBytes> line{};
					console.Update();
					const auto length = console.Read(line.data(), static_cast<uint32_t>(line.size()));
					Require(std::string_view(line.data(), length) == "\xd0\x9a\xc3\xa9",
						"native input must preserve Unicode and apply Backspace independently of the code page");
					std::array<wchar_t, 3> echo{};
					DWORD read = 0;
					Require(ReadConsoleOutputCharacterW(output, echo.data(), static_cast<DWORD>(echo.size()), { 0, 0 }, &read) &&
						read == echo.size() && std::wstring_view(echo.data(), echo.size()) == L"\u041a\u00e9 ",
						"native echo must preserve Unicode and erase the deleted character");
					console.Update();
					Require(console.Read(line.data(), static_cast<uint32_t>(line.size())) == 4 &&
						std::string_view(line.data()) == "next", "the second native line must survive the first Update");

					Require(!ConsoleWindow::IsExitRequested() && GenerateConsoleCtrlEvent(signal, 0),
						"each console session must receive a real control event");
					const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
					while (!ConsoleWindow::IsExitRequested() && std::chrono::steady_clock::now() < deadline)
					{
						std::this_thread::sleep_for(std::chrono::milliseconds(1));
					}
					Require(ConsoleWindow::IsExitRequested(), "the registered native control handler must request engine stop");
					closeOutput.Run();
					closeInput.Run();
					console.CloseWindow();
					shutdown.Run();
					Require(GetConsoleWindow() == nullptr, "shutdown must detach the console before the next session");
				}
			}
		}
		catch (const std::exception& error)
		{
			DWORD written = 0;
			const std::string message = std::string(error.what()) + '\n';
			WriteFile(report, message.data(), static_cast<DWORD>(message.size()), &written, nullptr);
			return 1;
		}
		return 0;
	}

	void TestNativeConsole()
	{
		Sailor::Tests::TempDirectory directory("native-console");
		const auto logPath = directory.Path("console.log");
		SECURITY_ATTRIBUTES security{ sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE };
		const auto report = CreateFileW(logPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &security,
			CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
		Require(report != INVALID_HANDLE_VALUE, "the native console report must be writable");
		Sailor::Tests::ScopeExit closeReport([&]() { CloseHandle(report); });
		const auto input = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &security,
			OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
		Require(input != INVALID_HANDLE_VALUE, "the native child must have an input handle");
		Sailor::Tests::ScopeExit closeInput([&]() { CloseHandle(input); });
		std::wstring executable(32768, L'\0');
		const DWORD length = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
		Require(length > 0 && length < executable.size(), "the test executable path must be complete");
		executable.resize(length);
		auto command = L"\"" + executable + L"\" --native-console";
		STARTUPINFOW startup{};
		startup.cb = sizeof(startup);
		startup.dwFlags = STARTF_USESTDHANDLES;
		startup.hStdInput = input;
		startup.hStdOutput = report;
		startup.hStdError = report;
		PROCESS_INFORMATION process{};
		// Never send control events to the CI runner's console or another process group.
		Require(CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, TRUE,
			DETACHED_PROCESS, nullptr, nullptr, &startup, &process), "the isolated native console child must start");
		CloseHandle(process.hThread);
		Sailor::Tests::ScopeExit closeProcess([&]() { CloseHandle(process.hProcess); });
		const auto wait = WaitForSingleObject(process.hProcess, 10000);
		if (wait != WAIT_OBJECT_0)
		{
			Require(TerminateProcess(process.hProcess, 1) &&
				WaitForSingleObject(process.hProcess, 5000) == WAIT_OBJECT_0,
				"the timed-out native console child must terminate before its report is removed");
		}
		DWORD exitCode = 1;
		const bool bHasExitCode = GetExitCodeProcess(process.hProcess, &exitCode) != FALSE;
		closeReport.Run();
		std::ifstream log(logPath);
		std::cout << std::string(std::istreambuf_iterator<char>(log), {});
		Require(wait == WAIT_OBJECT_0 && bHasExitCode && exitCode == 0,
			"native Windows console input, echo, control delivery and repeated shutdown must pass");
		std::cout << "Native Windows console passed: CP1251/CP1252, Ctrl-C/Ctrl-Break, four sessions\n";
	}
#endif
}

int main(int argc, char** argv)
{
#if defined(_WIN32)
	if (argc == 2 && std::string_view(argv[1]) == "--native-console")
	{
		return RunNativeConsole();
	}
#else
	(void)argc;
	(void)argv;
#endif
	uint32_t failures = 0;
	for (const auto test : { TestUnicodeLine, TestSequentialLines, TestBoundedOutput,
		TestContinuousInput, TestFullAndOverflowedLines, TestEditingAndReset, TestExitRequest, TestShutdownHandoff
#if defined(_WIN32)
		, TestNativeConsole
#endif
	})
	{
		try { test(); }
		catch (const std::exception& error)
		{
			std::cerr << "[FAIL] " << error.what() << '\n';
			++failures;
		}
	}
	if (failures != 0) return 1;
	std::cout << "Console input tests passed\n";
	return 0;
}
