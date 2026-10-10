#include "Core/Defines.h"

#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace
{
	std::string g_captured;
	const char* g_source = nullptr;
	const bool* g_expectedOwner = nullptr;
	size_t g_calls = 0;

	void Require(bool condition, const char* message)
	{
		if (!condition) throw std::runtime_error(message);
	}

	void CaptureProfileText(const char* text, size_t size)
	{
		Require(!g_expectedOwner || *g_expectedOwner,
			"profiling must consume a temporary's view before destroying its owner");
		g_captured.assign(text, size);
		g_source = text;
		++g_calls;
	}

	struct TemporaryText
	{
		std::string m_text;
		bool& m_bIsAlive;

		TemporaryText(bool& bIsAlive) : m_text(256, 'x'), m_bIsAlive(bIsAlive) { m_bIsAlive = true; }
		~TemporaryText() { m_bIsAlive = false; }
		operator std::string_view() const { return m_text; }
	};

	void TestTemporaryText()
	{
		bool bIsAlive = false;
		g_expectedOwner = &bIsAlive;
		SAILOR_PROFILE_TEXT(TemporaryText{ bIsAlive });
		g_expectedOwner = nullptr;
		Require(!bIsAlive && g_captured == std::string(256, 'x'),
			"profiling must preserve temporary text and release its owner at the end of the call");
		size_t evaluations = 0;
		auto makeText = [&]() { ++evaluations; return std::string(256, 'y'); };
		SAILOR_PROFILE_TEXT(makeText());
		Require(evaluations == 1 && g_captured == std::string(256, 'y'),
			"profiling must evaluate an owned-text expression exactly once");
		SAILOR_PROFILE_TEXT(makeText().c_str());
		Require(evaluations == 2 && g_captured == std::string(256, 'y'),
			"temporary C-string owners must remain alive through the profiling call too");
	}

	void TestBorrowedText()
	{
		const char bytes[] = { 'a', '\0', 'b', 'c', 'x' };
		SAILOR_PROFILE_TEXT(std::string_view(bytes, 4));
		Require(g_captured == std::string(bytes, 4) && g_source == bytes,
			"profiling must respect a bounded view and preserve embedded zero bytes");
		std::string owned = "an existing string";
		SAILOR_PROFILE_TEXT(owned);
		Require(g_captured == owned && g_source == owned.data(), "profiling must borrow existing strings without copying");
		SAILOR_PROFILE_TEXT("literal text");
		Require(g_captured == "literal text", "profiling must accept C-string literals");
	}
}

int main()
{
	try
	{
		TestTemporaryText();
		TestBorrowedText();
		Require(g_calls == 6, "each profiling expression must reach the text consumer once");
		std::cout << "[PASS] Profiling text lifetime, bounded views and single evaluation\n";
		return 0;
	}
	catch (const std::exception& error)
	{
		std::cerr << "[FAIL] " << error.what() << '\n';
		return 1;
	}
}
