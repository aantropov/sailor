#include "Platform/Win32/ConsoleWindow.h"

#include <array>
#include <chrono>
#include <future>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

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
}

int main()
{
	uint32_t failures = 0;
	for (const auto test : { TestUnicodeLine, TestSequentialLines, TestBoundedOutput,
		TestContinuousInput, TestFullAndOverflowedLines, TestEditingAndReset, TestExitRequest, TestShutdownHandoff })
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
