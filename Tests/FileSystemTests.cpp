#include "Platform/FileSystem.h"
#include "AssetRegistry/AssetRegistry.h"
#include "Support/TempDirectory.h"
#include "Workspace/WorkspacePathEncoding.h"

#include <array>
#include <cstring>
#include <future>
#include <iostream>
#include <stdexcept>

using namespace Sailor;

namespace
{
	void Require(bool condition, const char* message)
	{
		if (!condition)
		{
			throw std::runtime_error(message);
		}
	}

	void TestNativeReaders()
	{
		Tests::TempDirectory directory("filesystem");
		const auto path = directory.Path(Workspace::PathFromUtf8("asset \xD0\xAF \xE8\x88\xB9.bin"));
		const std::string content("first\r\nsecond\0third", 19);
		Require(AssetRegistry::WriteTextFile(path, content), "native writes must report success");
		std::string restored;
		Require(FileSystem::ReadText(path, restored) && restored == content,
			"text reads must preserve CRLF and embedded zeros");
		const auto large = directory.Path("large.txt");
		for (size_t size : { 16383u, 16384u, 16385u, 65536u })
		{
			const std::string payload(size, 'x');
			Require(AssetRegistry::WriteTextFile(large, payload) && FileSystem::ReadText(large, restored) &&
				restored == payload, "chunk boundaries must not truncate or append text");
		}

		TVector<uint8_t> bytes;
		Require(FileSystem::ReadBinary(path, bytes) && bytes.Num() == content.size() &&
			std::memcmp(bytes.GetData(), content.data(), bytes.Num()) == 0,
			"binary reads must preserve the exact payload at a Unicode path");
		TVector<uint32_t> words;
		Require(FileSystem::ReadBinary(path, words) && words.Num() == 5 &&
			std::memcmp(words.GetData(), content.data(), content.size()) == 0 &&
			reinterpret_cast<const uint8_t*>(words.GetData())[19] == 0,
			"typed reads must round up and zero the partial final element");

		auto stream = FileSystem::OpenRead(path);
		Require(static_cast<bool>(stream), "native streams must open");
		stream->seekg(7);
		std::array<char, 6> part{};
		stream->read(part.data(), part.size());
		Require(*stream && std::string_view(part.data(), part.size()) == "second",
			"seekable readers must support codec and binary parser offsets");
		stream->seekg(-5, std::ios::end);
		Require(stream->tellg() == std::streampos(14), "end-relative seeks must use byte offsets");

		std::error_code error;
		Require(FileSystem::FileSize(path, error) == content.size() && !error,
			"filesystem sizes must agree with the stream contents");
		const auto timestamp = FileSystem::LastWriteTime(path, error);
		Require(!error && timestamp == std::filesystem::last_write_time(path),
			"filesystem revisions must preserve native timestamps");
		Require(std::filesystem::is_regular_file(FileSystem::Status(path, error)) && !error,
			"filesystem status must describe the addressed file");

		std::array<std::future<bool>, 8> readers;
		for (auto& reader : readers)
		{
			reader = std::async(std::launch::async, [&]()
			{
				std::string result;
				return FileSystem::ReadText(path, result) && result == content;
			});
		}
		for (auto& reader : readers)
		{
			Require(reader.get(), "concurrent reads must own independent cursor state");
		}
	}

	void TestEmptyAndFailedReads()
	{
		Tests::TempDirectory directory("filesystem-errors");
		const auto empty = directory.Path("empty");
		Require(AssetRegistry::WriteTextFile(empty, ""), "empty files must be writable");
		TVector<uint8_t> bytes;
		bytes.Add(42);
		std::string text = "previous";
		Require(FileSystem::ReadBinary(empty, bytes) && bytes.IsEmpty() &&
			FileSystem::ReadText(empty, text) && text.empty(), "empty files must load successfully");
		bytes.Add(42);
		text = "previous";
		const auto missing = directory.Path("missing");
		Require(!FileSystem::OpenRead(missing) && !FileSystem::ReadBinary(missing, bytes) &&
			bytes.IsEmpty() && !FileSystem::ReadText(missing, text) && text.empty(),
			"failed reads must not publish stale contents");
		Require(!AssetRegistry::WriteTextFile(missing / "file", "payload"),
			"failed writes must be observable by callers");

		std::error_code error = std::make_error_code(std::errc::permission_denied);
		TVector<std::filesystem::path> entries;
		Require(FileSystem::Enumerate(directory.Get(), entries, error) && !error &&
			entries.Num() == 1 && entries[0] == empty, "enumeration must clear prior error state");
		Require(!FileSystem::Enumerate(missing, entries, error) && error && entries.IsEmpty(),
			"failed enumeration must not expose entries from a different directory");
	}
}

int main()
{
	try
	{
		TestNativeReaders();
		TestEmptyAndFailedReads();
		std::cout << "Filesystem behavior passed\n";
		return 0;
	}
	catch (const std::exception& error)
	{
		std::cerr << error.what() << '\n';
		return 1;
	}
}
