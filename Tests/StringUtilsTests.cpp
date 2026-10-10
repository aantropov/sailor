#include "Core/Utils.h"
#include "Core/StringHash.h"
#include "AssetRegistry/FileId.h"
#include "Engine/InstanceId.h"
#include "RHI/Material.h"
#include "RHI/Shader.h"

#include <array>
#include <barrier>
#include <cwchar>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

using namespace Sailor;

namespace
{
	void Require(bool condition, std::string_view message)
	{
		if (!condition) throw std::runtime_error(std::string(message));
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

	void TestTrimmedViews()
	{
		const char text[] = { ' ', '\t', 's', 'h', 'i', 'p', '\n', ' ', 'x' };
		const auto trimmed = Utils::TrimView(std::string_view(text, 8));
		Require(trimmed == "ship" && trimmed.data() == text + 2,
			"trimming a bounded view must borrow the original characters without reading its suffix");
		const char binary[] = { ' ', 'a', '\0', 'b', ' ' };
		Require(Utils::TrimView(std::string_view(binary, 5)) == std::string_view(binary + 1, 3),
			"trimming must preserve embedded zero bytes");
		Require(Utils::TrimView({}).empty() && Utils::TrimView(" \t\r\n\f\v").empty(),
			"empty and whitespace-only views must trim to empty views");
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

	void TestBorrowedInputAndOwnedResults()
	{
		const char path[] = { 's', 'h', 'i', 'p', '.', 'g', 'l', 'b', 'x' };
		const std::string_view filename(path, 8);
		Require(Utils::GetFileExtension(filename) == "glb" && Utils::RemoveFileExtension(filename) == "ship",
			"file helpers must respect a view's boundary without requiring a terminator");
		std::string source = "first::second::unused";
		auto split = Utils::SplitString(std::string_view(source).substr(0, 13), "::");
		source.assign(1024, 'x');
		Require(split.Num() == 2 && split[0] == "first" && split[1] == "second",
			"split results must own their text after the input is replaced");
		const auto lines = Utils::SplitStringByLines(std::string("first\n\nlast\n"));
		Require(lines.Num() == 3 && lines[0] == "first" && lines[1].empty() && lines[2] == "last",
			"line splitting must accept temporary input and retain owned results");
		const auto unsplit = Utils::SplitString(filename, {});
		Require(unsplit.Num() == 1 && unsplit[0] == filename,
			"an empty delimiter must leave the input as one owned field");
		std::string edited = "abcabc";
		const char pattern[] = { 'a', 'b', 'x' };
		Utils::ReplaceAll(edited, std::string_view(pattern, 2), filename.substr(0, 1));
		Require(edited == "scsc", "replacement must use bounded pattern and replacement views");
	}

	void TestDefaultIdentifierText()
	{
		const auto fileIdNode = FileId::Invalid.Serialize();
		const auto instanceIdNode = InstanceId::Invalid.Serialize();
		Require(fileIdNode.Scalar() == "NullFileId" && instanceIdNode.Scalar() == "NullInstanceId",
			"constant-initialized invalid identifiers must retain their canonical text on first serialization");
		FileId fileId;
		InstanceId instanceId;
		fileId.Deserialize(YAML::Load(YAML::Dump(fileIdNode)));
		instanceId.Deserialize(YAML::Load(YAML::Dump(instanceIdNode)));
		Require(fileId == FileId::Invalid && instanceId == InstanceId::Invalid && !fileId && !instanceId,
			"invalid identifiers must preserve identity and invalidity through a text round trip");
	}

	void TestUtf8Views()
	{
		const char bounded[] = { 'A', '\xc3', '\xa9', 'X' };
		Require(Utils::UTF8_to_wchar(std::string_view(bounded, 3)) == L"A\u00e9",
			"UTF-8 conversion must stop at the view boundary without a terminator");
		Require(Utils::UTF8_to_wchar({}).empty(), "an empty view must produce empty wide text");
		const char embedded[] = { 'A', '\0', 'B' };
		Require(Utils::UTF8_to_wchar(std::string_view(embedded, 3)) == std::wstring(L"A\0B", 3),
			"UTF-8 views must preserve embedded zero bytes");
		std::string source = "A\xc3\xa9\xe8\x88\xb9\xf0\x9f\x9a\xa2";
		const auto wide = Utils::UTF8_to_wchar(source);
		source.assign(1024, 'x');
		Require(wide == L"A\u00e9\u8239\U0001f6a2",
			"UTF-8 conversion must own the result and encode supplementary characters on both wchar widths");
		Require(Utils::wchar_to_UTF8(wide.c_str()) == "A\xc3\xa9\xe8\x88\xb9\xf0\x9f\x9a\xa2",
			"valid multibyte text must round-trip through the platform's wide-character encoding");
	}

	void TestWideViews()
	{
		const wchar_t bounded[] = { L'A', 0xd83d, 0xdea2, L'X' };
		Require(Utils::wchar_to_UTF8(std::wstring_view(bounded, 3)) == "A\xf0\x9f\x9a\xa2",
			"bounded wide conversion must preserve surrogate pairs without reading a following character");
		Require(Utils::wchar_to_UTF8(std::wstring_view(bounded, 2)) == "A\xef\xbf\xbd",
			"a surrogate outside the view must not complete a truncated pair");
		Require(Utils::wchar_to_UTF8(std::wstring_view{}).empty(), "empty wide input must remain empty");
		const wchar_t embedded[] = { L'A', 0, L'B' };
		Require(Utils::wchar_to_UTF8(std::wstring_view(embedded, 3)) == std::string("A\0B", 3),
			"bounded wide conversion must preserve embedded zeros");
		const wchar_t invalid[] = { 0xdc00, 0xd800, L'X' };
		Require(Utils::wchar_to_UTF8(std::wstring_view(invalid, 3)) == "\xef\xbf\xbd\xef\xbf\xbdX",
			"unpaired surrogates must produce replacement scalars without consuming the next character");
#if WCHAR_MAX > 0xffff
		const wchar_t scalars[] = { 0x1f6a2, 0x110000 };
		Require(Utils::wchar_to_UTF8(std::wstring_view(scalars, 2)) == "\xf0\x9f\x9a\xa2\xef\xbf\xbd",
			"UTF-32 wide text must preserve supplementary scalars and replace invalid ones");
#endif
	}

	void TestHashedStringNames()
	{
		static_assert(sizeof(StringHash) == sizeof(uint64_t));
		constexpr auto expected = "Upload mesh"_h;
		static_assert(expected.GetHash() == fnv1a("Upload mesh", 11));
		static_assert(HashString("Upload mesh") == expected.GetHash());
		const char queue[] = { 'M', 'a', 's', 'k', 'e', 'd', 'X' };
		Require(HashString(std::string_view(queue, 6)) == "Masked"_h.GetHash(),
			"numeric queue hashes and literal identifiers must agree without including bytes outside the view");
		const auto literal = "Upload mesh"_h;
		Require(literal == expected && literal.ToString() == "Upload mesh",
			"runtime literal registration must preserve the compile-time identifier and readable name");
		StringHash dynamic;
		{
			std::string source = "prefix:owned dynamic name:suffix";
			dynamic = StringHash::Runtime(std::string_view(source).substr(7, 18));
		}
		Require(dynamic.ToString() == "owned dynamic name",
			"dynamic identifiers must retain text after their source is destroyed");
		const auto embedded = "A\0B"_h;
		Require(embedded.ToString() == std::string("A\0B", 3),
			"hashed literals must use the literal length, including embedded zero bytes");
		std::array<std::string_view, 8> names;
		std::barrier start(static_cast<ptrdiff_t>(names.size()));
		std::array<std::jthread, 8> workers;
		for (size_t index = 0; index < workers.size(); ++index)
		{
			workers[index] = std::jthread([&, index]
			{
				start.arrive_and_wait();
				for (size_t repeat = 0; repeat < 1024; ++repeat)
					names[index] = "Concurrent literal registration"_h.ToString();
			});
		}
		for (auto& worker : workers) worker.join();
		for (const auto name : names)
			Require(name == "Concurrent literal registration" && name.data() == names[0].data(),
				"concurrent literal users must share one stable readable name");
	}

	void TestBindingIdentifiers()
	{
		auto bindings = RHI::RHIShaderBindingSetPtr::Make();
		RHI::ShaderLayoutBinding layout;
		{
			std::string source = "prefix:material.albedo:suffix";
			StringHash member;
			RHI::RHIShaderBindingSet::ParseParameter(
				StringHash::Runtime(std::string_view(source).substr(7, 15)), layout.m_name, member);
			layout.m_members.Add({ RHI::EShaderBindingMemberType::Float, member, 16, 16 });
		}
		bindings->SetLayoutShaderBindings({ layout });
		auto binding = bindings->GetOrAddShaderBinding("material"_h);
		binding->SetLayout(layout);
		Require(bindings->HasBinding("material"_h) && bindings->HasParameter("material.albedo"_h) &&
			!bindings->HasParameter("material.missing"_h) && !bindings->HasBinding("unknown"_h),
			"dynamic reflected names and literal lookups must address the same binding and member");
		Require(bindings->HasParameter("material"_h, "albedo"_h) &&
			!bindings->HasParameter("material"_h, "missing"_h) &&
			!bindings->HasParameter("unknown"_h, "albedo"_h),
			"split identifiers must match the same reflected parameters without constructing a dotted name");
		RHI::ShaderLayoutBindingMember member;
		Require(binding->FindVariableInUniformBuffer("albedo"_h, member) && member.m_absoluteOffset == 16 &&
			member.m_name.ToString() == "albedo" && layout.m_name.ToString() == "material",
			"reflected identifiers must preserve layout data and text after the parser input is destroyed");
		Require(bindings->GetOrAddShaderBinding(StringHash::Runtime(layout.m_name.ToString())) == binding &&
			bindings->GetShaderBindings().Num() == 1,
			"equivalent names must reuse the original RHIPtr without adding another binding");
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
		{ "TrimmedViews", TestTrimmedViews },
		{ "SplitStrings", TestSplitStrings },
		{ "BorrowedInputAndOwnedResults", TestBorrowedInputAndOwnedResults },
		{ "DefaultIdentifierText", TestDefaultIdentifierText },
		{ "Utf8Views", TestUtf8Views },
		{ "WideViews", TestWideViews },
		{ "HashedStringNames", TestHashedStringNames },
		{ "BindingIdentifiers", TestBindingIdentifiers }
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
