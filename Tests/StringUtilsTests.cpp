#include "Core/Utils.h"

#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>

using namespace Sailor;

namespace
{
	void Require(bool condition, const std::string& message)
	{
		if (!condition) throw std::runtime_error(message);
	}

	void TestFileExtensions()
	{
		struct Case { const char* path; const char* extension; const char* withoutExtension; };
		for (const auto& c : {
			Case{ "ship.glb", "glb", "ship" },
			Case{ "README", "", "README" },
			Case{ "", "", "" },
			Case{ ".", "", "." },
			Case{ "..", "", ".." },
			Case{ ".settings", "", ".settings" },
			Case{ "folder/.settings", "", "folder/.settings" },
			Case{ "folder.v1/ship", "", "folder.v1/ship" },
			Case{ "folder.v1/ship.glb", "glb", "folder.v1/ship" },
			Case{ "folder.v1/", "", "folder.v1/" },
			Case{ "folder.v1/..", "", "folder.v1/.." },
			Case{ R"(C:\assets.v1\ship)", "", R"(C:\assets.v1\ship)" },
			Case{ R"(C:\assets.v1\ship.DDS)", "DDS", R"(C:\assets.v1\ship)" },
			Case{ "ship.", "", "ship" },
			Case{ ".ship.gltf", "gltf", ".ship" },
			Case{ "archive.tar.gz", "gz", "archive.tar" } })
		{
			Require(Utils::GetFileExtension(c.path) == c.extension,
				"extension must come from the final filename: " + std::string(c.path));
			Require(Utils::RemoveFileExtension(c.path) == c.withoutExtension,
				"removing an extension must preserve directories and extensionless names: " + std::string(c.path));
		}
	}

	void TestAdjacentErase()
	{
		for (const auto& c : {
			std::pair{ "aaaab", "b" }, std::pair{ "aabaaa", "b" },
			std::pair{ "aaaa", "" }, std::pair{ "b", "b" }, std::pair{ "", "" } })
		{
			std::string value = c.first;
			Utils::Erase(value, "a");
			Require(value == c.second, "Erase must not skip a match shifted into the erased position");
		}
		std::string pairs = "ababab";
		Utils::Erase(pairs, "ab");
		Require(pairs.empty(), "adjacent multi-character matches must all be erased");
		std::string overlapping = "banana";
		Utils::Erase(overlapping, "ana");
		Require(overlapping == "bna", "Erase consumes non-overlapping matches from left to right");
	}

	void TestEraseRange()
	{
		std::string middle = "aa-aa-aa";
		Utils::Erase(middle, "aa", 3, 5);
		Require(middle == "aa--aa", "Erase must preserve text outside its original half-open range");
		std::string crossing = "abcabc";
		Utils::Erase(crossing, "abc", 0, 2);
		Require(crossing == "abcabc", "a match crossing the range end must remain intact");
		std::string beyond = "abc";
		Utils::Erase(beyond, "a", 4);
		Require(beyond == "abc", "a range beyond the string has no matches");
		Utils::Erase(beyond, "", 0, 2);
		Require(beyond == "abc", "an empty pattern must not erase or stall");
		Utils::Erase(beyond, "a", 2, 1);
		Require(beyond == "abc", "a reversed range has no matches");
	}

	void TestReplacementRange()
	{
		std::string shrinking = "aa-aa-aa";
		Utils::ReplaceAll(shrinking, "aa", "x", 0, 5);
		Require(shrinking == "x-x-aa", "shrinking replacements must not expand the original range");
		std::string growing = "aa-aa-aa";
		Utils::ReplaceAll(growing, "aa", "bbbb", 0, 5);
		Require(growing == "bbbb-bbbb-aa", "growing replacements must retain the original range end");
		std::string crossing = "abcabc";
		Utils::ReplaceAll(crossing, "abc", "x", 0, 2);
		Require(crossing == "abcabc", "replacement must not consume a partial boundary match");
		std::string inserted = "aa";
		Utils::ReplaceAll(inserted, "a", "aa");
		Require(inserted == "aaaa", "inserted text must not be searched again");
		std::string shader = "\tvec4\tcolor;\n";
		Utils::ReplaceAll(shader, "\t", " ");
		Require(shader == " vec4 color;\n", "shader tab replacement must preserve other whitespace");
		std::string unchanged = "abc";
		Utils::ReplaceAll(unchanged, "", "x");
		Require(unchanged == "abc", "an empty pattern must not insert or stall");
		Utils::ReplaceAll(unchanged, "a", "x", 2, 1);
		Require(unchanged == "abc", "a reversed replacement range has no matches");
		Utils::ReplaceAll(unchanged, "c", "xx", 0, 100);
		Require(unchanged == "abxx", "a range beyond the end must stop at the original string length");
	}

	void TestTwoSidedTrim()
	{
		for (const auto& c : {
			std::pair{ " \tstats.memory \r\n", "stats.memory" },
			std::pair{ "\r\n \f\v\t", "" }, std::pair{ "", "" },
			std::pair{ " two  words \t", "two  words" }, std::pair{ "already trimmed", "already trimmed" },
			std::pair{ " \t\xc3\xa9\t ", "\xc3\xa9" } })
		{
			std::string value = c.first;
			Utils::Trim(value);
			Require(value == c.second, "Trim must remove only surrounding whitespace");
		}
	}

	void TestSplitStrings()
	{
		const auto fields = Utils::SplitString("one::two::::", "::");
		Require(fields.Num() == 4 && fields[0] == "one" && fields[1] == "two" &&
			fields[2].empty() && fields[3].empty(), "splitting must preserve empty fields and return owned strings");
		const auto lines = Utils::SplitStringByLines("first\n\nlast\n");
		Require(lines.Num() == 3 && lines[0] == "first" && lines[1].empty() && lines[2] == "last",
			"line splitting must preserve interior empty lines");
	}
}

int main()
{
	const std::pair<const char*, void(*)()> tests[] = {
		{ "FileExtensions", TestFileExtensions },
		{ "AdjacentErase", TestAdjacentErase },
		{ "EraseRange", TestEraseRange },
		{ "ReplacementRange", TestReplacementRange },
		{ "TwoSidedTrim", TestTwoSidedTrim },
		{ "SplitStrings", TestSplitStrings }
	};
	bool passed = true;
	for (const auto& [name, test] : tests)
	{
		try
		{
			test();
			std::cout << "[PASS] " << name << std::endl;
		}
		catch (const std::exception& error)
		{
			std::cerr << "[FAIL] " << name << ": " << error.what() << std::endl;
			passed = false;
		}
	}
	return passed ? 0 : 1;
}
