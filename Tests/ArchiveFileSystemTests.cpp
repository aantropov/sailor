#include "Platform/FileSystem.h"
#include "AssetRegistry/AssetRegistry.h"
#include "Core/FileRevision.h"
#include "Support/TempDirectory.h"
#include "Workspace/WorkspacePathEncoding.h"
#include "Workspace/WorkspacePackage.h"
#include "Workspace/WorkspaceCacheContract.h"
#include "Support/TaskTestApp.h"
#include "Tasks/Scheduler.h"

#include <miniz/miniz.h>
#include <nlohmann/json.hpp>
#include <array>
#include <cctype>
#include <fstream>
#include <future>
#include <iostream>
#include <stdexcept>
#include <vector>

using namespace Sailor;
using namespace Sailor::FileSystem;

namespace
{
	void Require(bool condition, const std::string& message)
	{
		if (!condition)
		{
			throw std::runtime_error(std::string(message));
		}
	}

	void Save(const std::filesystem::path& path, std::string_view contents)
	{
		std::ofstream output(path, std::ios::binary);
		output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
		output.close();
		Require(static_cast<bool>(output), "fixture write must succeed");
	}

	std::string NewPackageId()
	{
		std::string result;
		for (char character : FileId::CreateNewFileId().ToString())
		{
			if (character != '-')
			{
				result.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(character))));
			}
		}
		return result;
	}

	std::string RawZip(std::initializer_list<std::pair<std::string, std::string>> files)
	{
		mz_zip_archive zip{};
		Require(mz_zip_writer_init_heap(&zip, 0, 0), "ZIP fixture writer must initialize");
		for (const auto& [name, contents] : files)
		{
			std::string writableName = name;
			if (writableName.front() == '/')
			{
				writableName.front() = 'a';
			}
			const size_t contentSize = name.ends_with('/') ? 0 : contents.size();
			Require(mz_zip_writer_add_mem(&zip, writableName.c_str(), contents.data(), contentSize, 0),
				"ZIP fixture entry must be written: " + name);
		}
		void* data = nullptr;
		size_t size = 0;
		Require(mz_zip_writer_finalize_heap_archive(&zip, &data, &size), "ZIP fixture must finalize");
		std::string result(static_cast<char*>(data), size);
		mz_free(data);
		mz_zip_writer_end(&zip);
		// Miniz refuses to write absolute names. Patch both parsed header fields
		// so reader validation still receives an otherwise well-formed hostile ZIP.
		const auto word = [&](size_t offset, size_t bytes)
		{
			Require(offset + bytes <= result.size(), "ZIP fixture header must fit");
			uint32_t value = 0;
			for (size_t index = 0; index < bytes; ++index)
			{
				value |= uint32_t(static_cast<uint8_t>(result[offset + index])) << (index * 8);
			}
			return value;
		};
		size_t central = word(result.size() - 22 + 16, 4);
		for (const auto& [name, contents] : files)
		{
			const size_t local = word(central + 42, 4);
			Require(word(central + 28, 2) == name.size(), "ZIP fixture name lengths must agree");
			result.replace(central + 46, name.size(), name);
			result.replace(local + 30, name.size(), name);
			central += 46 + word(central + 28, 2) + word(central + 30, 2) + word(central + 32, 2);
		}
		return result;
	}

	void TestCompressionAndMount(EArchiveCompression compression)
	{
		Tests::TempDirectory fixture("zip-filesystem");
		const auto source = fixture.Path("source");
		const auto mountRoot = fixture.Path("mounted");
		std::filesystem::create_directories(source / "nested" / "empty");
		std::filesystem::create_directory(mountRoot);
		const auto name = Workspace::PathFromUtf8("nested/payload \xD0\xAF \xE8\x88\xB9.bin");
		std::string payload(512 * 1024, 'x');
		for (size_t byte = 0; byte < payload.size(); ++byte)
		{
			payload[byte] = static_cast<char>(byte % 251);
		}
		payload[9] = '\0';
		payload[51] = static_cast<char>(255);
		Save(source / name, payload);
		Save(source / "empty", "");
		Save(mountRoot / "only-native", "hidden");
		std::filesystem::create_directories((mountRoot / name).parent_path());
		Save(mountRoot / name, "native shadow");
		const auto timestamp = std::filesystem::last_write_time(source / name) +
			std::chrono::duration_cast<std::filesystem::file_time_type::duration>(std::chrono::nanoseconds(123456));
		std::filesystem::last_write_time(source / name, timestamp);
		FileRevision nativeRevision;
		Require(Utils::TryGetFileRevision(Workspace::PathToUtf8(source / name), nativeRevision), "native revision must resolve");
		const auto archive = fixture.Path("content.zip");
		std::string diagnostic;
		Require(WriteZipArchive(source, archive, compression, diagnostic), diagnostic);

		// Inspect the ZIP's parsed entry method, not a source-level implementation detail.
		std::string zipBytes;
		Require(ReadText(archive, zipBytes), "written archive must be readable");
		mz_zip_archive zip{};
		Require(mz_zip_reader_init_mem(&zip, zipBytes.data(), zipBytes.size(), 0), "written ZIP must parse");
		const std::string entryName = Workspace::PathToUtf8(name);
		const int index = mz_zip_reader_locate_file(&zip, entryName.c_str(), nullptr, 0);
		mz_zip_archive_file_stat info{};
		Require(index >= 0 && mz_zip_reader_file_stat(&zip, static_cast<mz_uint>(index), &info), "payload must be in the archive");
		Require(info.m_method == (compression == EArchiveCompression::Stored ? 0 : 8),
			"stored and compressed options must select the requested ZIP method");
		if (compression != EArchiveCompression::Stored)
		{
			Require(info.m_comp_size < info.m_uncomp_size / 10, "deflate must compress repetitive data");
		}
		const int indexEntry = mz_zip_reader_locate_file(&zip, ".sailor-files.json", nullptr, 0);
		Require(indexEntry >= 0 && mz_zip_reader_file_stat(&zip, static_cast<mz_uint>(indexEntry), &info) &&
			(compression != EArchiveCompression::Stored || info.m_method == 0),
			"raw ZIP mode must also store its index without compression");
		mz_zip_reader_end(&zip);

		auto mount = MountZipArchive(mountRoot, archive, diagnostic);
		Require(static_cast<bool>(mount), diagnostic);
		std::string restored;
		Require(ReadText(mountRoot / name, restored) && restored == payload,
			"archive bytes must shadow loose files without extracting assets");
#if defined(_WIN32)
		std::string otherCase = Workspace::PathToUtf8(mountRoot);
		for (char& character : otherCase)
		{
			const auto byte = static_cast<unsigned char>(character);
			if (byte < 128)
			{
				character = static_cast<char>(std::islower(byte) ? std::toupper(byte) : std::tolower(byte));
			}
		}
		Require(ReadText(Workspace::PathFromUtf8(otherCase) / name, restored) && restored == payload,
			"Windows mount lookup must preserve ordinal case-insensitive root semantics");
#endif
		Require(ReadText(mountRoot / "empty", restored) && restored.empty(), "zero-byte ZIP entries must load");
		Require(!ReadText(mountRoot / "only-native", restored), "a mounted namespace must not fall back to loose files");
		Require(!OpenWrite(mountRoot / name) && !OpenWrite(mountRoot / "new") && IsReadOnly(mountRoot / "new"),
			"ZIP content must be read-only, including absent files");
		Require(!IsReadOnly(source) && !std::filesystem::exists(mountRoot / "empty"),
			"mounting must leave native IO outside the mount usable and must not extract files");
		std::error_code error;
		Require(Canonical(mountRoot, error) == mount->GetRoot() && !error,
			"canonical mount roots must not gain a trailing component");
		Require(FileSize(mountRoot / name, error) == payload.size() && !error &&
			LastWriteTime(mountRoot / name, error) == std::filesystem::last_write_time(source / name),
			"archive file metadata must preserve exact source size and timestamp");
		FileRevision archiveRevision;
		Require(Utils::TryGetFileRevision(Workspace::PathToUtf8(mountRoot / name), archiveRevision) &&
			archiveRevision == nativeRevision && Utils::GetFileModificationTime(Workspace::PathToUtf8(mountRoot / name)) ==
			Utils::GetFileModificationTime(Workspace::PathToUtf8(source / name)),
			"mounting must preserve source revision and import-time identities");
		Require(Canonical(mountRoot / "nested" / ".." / "empty", error) == mount->GetRoot() / "empty" && !error,
			"logical canonicalization must resolve archive entries");
		TVector<std::filesystem::path> entries;
		Require(Enumerate(mountRoot, entries, error) && !error && entries.Num() == 2,
			"enumeration must expose entries and hide the internal index and loose files");
		Require(Enumerate(mountRoot / "nested" / "empty", entries, error) && entries.IsEmpty(),
			"empty directories must survive packaging");
		Require(!Enumerate(mountRoot / name, entries, error) && error == std::errc::not_a_directory,
			"enumerating a ZIP file as a directory must fail");
		Require(!Canonical(mountRoot / "missing", error).is_absolute() && error == std::errc::no_such_file_or_directory,
			"missing archive entries must not canonicalize as native files");
		const auto size = FileSize(mountRoot, error);
		Require(size == static_cast<uint64_t>(-1) && error == std::errc::is_a_directory,
			"archive directories must retain native size error semantics");
		Require(!MountZipArchive(mountRoot, archive, diagnostic) && !diagnostic.empty(), "duplicate mounts must fail");
		Require(!MountZipArchive(mountRoot / "nested", archive, diagnostic), "overlapping mounts must fail");

		std::array<std::future<bool>, 12> readers;
		for (auto& reader : readers)
		{
			reader = std::async(std::launch::async, [&]()
			{
				std::string text;
				return ReadText(mountRoot / name, text) && text == payload;
			});
		}
		for (auto& reader : readers)
		{
			Require(reader.get(), "concurrent archive reads must not share decoder or cursor state");
		}
		auto stream = OpenRead(mountRoot / name);
		Require(static_cast<bool>(stream), "an archive reader must open");
		stream->seekg(200000);
		std::array<char, 31> middle{};
		stream->read(middle.data(), middle.size());
		Require(*stream && std::string_view(middle.data(), middle.size()) == std::string_view(payload).substr(200000, middle.size()),
			"archive readers must seek across decoder buffers");
		stream->seekg(-17, std::ios::cur);
		Require(stream->tellg() == std::streampos(200014), "relative seeks must use the current consumer position");
		stream->seekg(9);
		Require(stream->get() == 0, "backward seeks must restart decompression when needed");
		mount.Clear();
		Require(!IsReadOnly(mountRoot) && ReadText(mountRoot / name, restored) && restored == "native shadow",
			"releasing a mount must restore native folder access");
		stream->seekg(8);
		std::array<char, 4> part{};
		stream->read(part.data(), part.size());
		Require(*stream && std::string_view(part.data(), part.size()) == std::string_view(payload).substr(8, 4),
			"retained streams must remain seekable after their mount is gone");
		Require(!WriteZipArchive(source, archive, compression, diagnostic) && ReadText(archive, restored) && restored == zipBytes,
			"existing archives must not be replaced by a repeated write");
		Require(!WriteZipArchive(source, source / "inside.zip", compression, diagnostic), "recursive archive output must be rejected");
	}

	void TestInvalidArchives()
	{
		Tests::TempDirectory fixture("invalid-zip");
		const auto root = fixture.Path("mount");
		std::filesystem::create_directory(root);
		const auto zip = fixture.Path("invalid.zip");
		std::string diagnostic;
		for (const std::string name : { "../escape", "/absolute", "C:/drive", "folder\\escape", "folder/../escape",
			"folder//escape", "folder/./escape", "trailing.", ".sailor-files.json/child", "folder//" })
		{
			Save(zip, RawZip({ { name, "payload" } }));
			Require(!MountZipArchive(root, zip, diagnostic) && !diagnostic.empty(), "unsafe ZIP entry must fail to mount");
		}
		for (const auto& bytes : { RawZip({ { "same", "first" }, { "same", "second" } }),
			RawZip({ { "file", "data" }, { "file/child", "data" } }),
			RawZip({ { "file/child", "data" }, { "file", "data" } }) })
		{
			Save(zip, bytes);
			Require(!MountZipArchive(root, zip, diagnostic), "duplicate/conflicting ZIP paths must fail");
		}
		for (const std::string index : { "broken JSON", "{\"schemaVersion\":2,\"files\":{}}",
			"{\"schemaVersion\":1,\"schemaVersion\":1,\"files\":{}}", "{\"schemaVersion\":1.0,\"files\":{}}",
			"{\"schemaVersion\":1,\"files\":{\"data\":{\"size\":10,\"modifiedUnixNs\":0}}}" })
		{
			Save(zip, RawZip({ { "data", "payload" }, { ".sailor-files.json", index } }));
			Require(!MountZipArchive(root, zip, diagnostic), "malformed Sailor file indexes must fail");
		}
		std::string symlink = RawZip({ { "link", "target" } });
		const auto central = symlink.find("PK\1\2");
		Require(central != symlink.npos, "ZIP fixture must have a central directory");
		const uint32_t attributes = 0120777u << 16;
		for (size_t byte = 0; byte < 4; ++byte)
		{
			symlink[central + 38 + byte] = static_cast<char>(attributes >> (byte * 8));
		}
		Save(zip, symlink);
		Require(!MountZipArchive(root, zip, diagnostic), "symbolic links in ZIP metadata must be rejected");
		std::string corrupted = RawZip({ { "data", "CRC payload" } });
		const auto content = corrupted.find("CRC payload");
		Require(content != corrupted.npos, "stored ZIP fixture must have a payload");
		corrupted[content] = 'X';
		Save(zip, corrupted);
		auto mount = MountZipArchive(root, zip, diagnostic);
		Require(static_cast<bool>(mount), diagnostic);
		std::string text = "stale";
		Require(!ReadText(root / "data", text) && text.empty(), "CRC failures must not publish corrupt bytes");
		mount.Clear();
		corrupted.resize(corrupted.size() - 15);
		Save(zip, corrupted);
		Require(!MountZipArchive(root, zip, diagnostic), "truncated ZIP directories must fail to mount");
	}

	void TestPackagedWorkspace()
	{
		Tests::TempDirectory fixture("packaged-workspace");
		const auto source = fixture.Path("assets");
		const auto game = fixture.Path("game");
		std::filesystem::create_directories(source / "nested");
		std::filesystem::create_directories(game / "Content");
		Save(source / "nested" / "payload.txt", "packaged resource");
		const FileId id = FileId::CreateNewFileId();
		Save(source / "nested" / "payload.txt.asset",
			YAML::Dump(CreateAssetInfoMetadata<AssetInfo>(id, "payload.txt")));
		std::string diagnostic;
		Require(WriteZipArchive(source, game / "Content.zip", EArchiveCompression::Balanced, diagnostic), diagnostic);
		auto resolved = Workspace::ResolveWorkspaceContext(game);
		Require(resolved.IsSuccess(), resolved.m_message);
		TVector<ArchiveMountPtr> mounts;
		Require(!resolved.m_context.IsPackaged(), "ordinary workspaces must retain authoring ownership");
		Require(Workspace::MountPackageContent(resolved.m_context, mounts, diagnostic) && mounts.IsEmpty(),
			"ordinary workspaces must not require a package manifest");
		const auto packageId = NewPackageId();
		Save(game / "GamePackage.json", nlohmann::json{{"packageVersion", 1}, {"packageId", packageId},
			{"initialCache", nullptr}, {"archives", {{{"root", "Content"}, {"file", "Content.zip"}}}}}.dump());
		resolved = Workspace::ResolveWorkspaceContext(game);
		Require(resolved.IsSuccess(), resolved.m_message);
		Require(resolved.m_context.IsPackaged(), "game package resolution must retain runtime ownership");
		// Isolate test cache storage without changing HOME or shared process state.
		const auto cache = resolved.m_context.GetCache();
		Require(!std::filesystem::exists(cache), "fixture package ID must not reuse an existing cache");
		struct CacheCleanup
		{
			std::filesystem::path m_path;
			~CacheCleanup()
			{
				std::error_code error;
				std::filesystem::remove_all(m_path, error);
			}
		} cleanup{cache};
		Require(Workspace::MountPackageContent(resolved.m_context, mounts, diagnostic) && mounts.Num() == 1, diagnostic);
		{
			Tests::TaskTestApp app;
			auto& scheduler = app.GetScheduler();
			scheduler.Initialize();
			AssetRegistry registry(resolved.m_context, &scheduler);
			App::AddSubmodule(TSubmodule<DefaultAssetInfoHandler>::Make(&registry));
			Require(registry.ScanContentFolder(), "the real asset registry must scan mounted content");
			std::string text;
			Require(registry.ReadContentText("nested/payload.txt", text) && text == "packaged resource",
				"registry content reads must resolve archived assets");
			Require(registry.GetOrLoadFile("nested/payload.txt") == id && registry.GetAssetInfoPtr(id) &&
				!registry.GetAssetInfoPtr(id)->IsWritable(), "archived metadata must retain IDs and read-only ownership");
			std::filesystem::path writable;
			Require(!registry.ResolveWorkspaceContentPathForWrite("nested/payload.txt", writable),
				"authoring APIs must refuse archived workspace content");
			Require(AssetRegistry::WriteTextFile(resolved.m_context.GetCache() / "runtime", "writable"),
				"runtime cache must stay writable outside the mounted namespace");
			App::RemoveSubmodule<DefaultAssetInfoHandler>();
		}
		mounts.Clear();
		Save(game / "GamePackage.json", nlohmann::json{{"packageVersion", 1}, {"packageId", packageId},
			{"initialCache", nullptr}, {"archives", nlohmann::json::array()}}.dump());
		std::filesystem::copy(source, game / "Content", std::filesystem::copy_options::recursive);
		resolved = Workspace::ResolveWorkspaceContext(game);
		Require(resolved.IsSuccess() && resolved.m_context.IsPackaged(),
			"loose game packages must retain runtime ownership");
		{
			Tests::TaskTestApp app;
			auto& scheduler = app.GetScheduler();
			scheduler.Initialize();
			AssetRegistry registry(resolved.m_context, &scheduler);
			App::AddSubmodule(TSubmodule<DefaultAssetInfoHandler>::Make(&registry));
			Require(registry.ScanContentFolder(), "the registry must scan loose packaged content");
			Require(registry.GetOrLoadFile("nested/payload.txt") == id && registry.GetAssetInfoPtr(id) &&
				!registry.GetAssetInfoPtr(id)->IsWritable(),
				"loose packaged assets must keep read-only ownership even on writable storage");
			std::filesystem::path writable;
			Require(!registry.ResolveWorkspaceContentPathForWrite("nested/new.asset", writable),
				"authoring APIs must refuse loose packaged content");
			App::RemoveSubmodule<DefaultAssetInfoHandler>();
		}
		for (const std::string manifest : {
			R"({"packageVersion":2,"archives":[]})",
			R"({"packageVersion":1,"packageVersion":1,"archives":[]})",
			R"({"packageVersion":1,"archives":[{"root":"Cache","file":"Content.zip"}]})",
			R"({"packageVersion":1,"archives":[{"root":"../assets","file":"Content.zip"}]})",
			R"({"packageVersion":1,"archives":[{"root":"Content","file":"missing.zip"}]})",
			R"({"packageVersion":1,"archives":[{"root":"Content","file":"Content.zip"},{"root":"Content","file":"Content.zip"}]})" })
		{
			auto document = nlohmann::json::parse(manifest);
			document["packageId"] = packageId;
			document["initialCache"] = nullptr;
			// Preserve the duplicate field fixture as raw text.
			Save(game / "GamePackage.json", manifest.find("packageVersion\":1,\"packageVersion") != manifest.npos
				? manifest : document.dump());
			Require(!Workspace::MountPackageContent(resolved.m_context, mounts, diagnostic) && mounts.IsEmpty() &&
				!IsReadOnly(game / "Content"), "invalid packages must roll back their staged mounts");
		}
	}

	void TestInitialCache()
	{
		Tests::TempDirectory fixture("package-cache");
		const auto source = fixture.Path("source");
		std::filesystem::create_directories(source / "Content");
		const auto authoring = Workspace::ResolveWorkspaceContext(source);
		Require(authoring.IsSuccess(), authoring.m_message);
		const auto sourceCache = authoring.m_context.GetCache();
		std::filesystem::create_directory(sourceCache / "CompiledShaders");
		std::filesystem::create_directory(sourceCache / "Lods");
		std::filesystem::create_directory(sourceCache / "Build");
		Save(sourceCache / "CompiledShaders" / "shader.spirv", "spirv fixture");
		Save(sourceCache / "Lods" / "mesh.bin", "lod fixture");
		Save(sourceCache / "Build" / "private", "do not ship");
		Save(sourceCache / "EditorSettings.yaml", "do not ship");
		Save(sourceCache / "AssetCache.yaml", "absolute source index: do not ship");
		const auto sourceIdentity = Workspace::MakeWorkspaceCacheIdentity("shader-cache", "shader-compiler-v1", 1,
			authoring.m_context);
		std::string envelope;
		std::string diagnostic;
		const std::string payload = "shaderCache: {entries: {}}";
		Require(Workspace::SerializeWorkspaceCacheEnvelope(sourceIdentity, payload, envelope, diagnostic), diagnostic);
		Save(sourceCache / "ShaderCache.yaml", envelope);
		const auto game = fixture.Path("relocated game");
		std::filesystem::create_directories(game / "Content");
		std::filesystem::create_directory(game / "InitialCache");
		auto incompatibleIdentity = sourceIdentity;
		incompatibleIdentity.m_buildIdentity += ";different-runtime";
		std::string incompatibleEnvelope;
		Require(Workspace::SerializeWorkspaceCacheEnvelope(incompatibleIdentity, payload,
			incompatibleEnvelope, diagnostic), diagnostic);
		Save(sourceCache / "ShaderCache.yaml", incompatibleEnvelope);
		Require(!Workspace::WriteInitialCacheArchive(authoring.m_context, game / "invalid.zip",
			EArchiveCompression::Stored, diagnostic) && !std::filesystem::exists(game / "invalid.zip"),
			"initial cache export must reject a different runtime/configuration identity without publishing output");
		Save(sourceCache / "ShaderCache.yaml", envelope);
		Require(Workspace::WriteInitialCacheArchive(authoring.m_context, game / "InitialCache.zip",
			EArchiveCompression::Balanced, diagnostic), diagnostic);
		nlohmann::json document = {{"packageVersion", 1}, {"packageId", NewPackageId()},
			{"archives", nlohmann::json::array()}, {"initialCache", "InitialCache.zip"}};
		Save(game / "GamePackage.json", document.dump());
		const auto resolved = Workspace::ResolveWorkspaceContext(game);
		Require(resolved.IsSuccess(), resolved.m_message);
		const auto cache = resolved.m_context.GetCache();
		Require(!Workspace::IsPathWithin(game, cache) && !std::filesystem::exists(game / "Cache") &&
			!std::filesystem::exists(cache), "resolution must not create storage in a read-only package");
		struct Cleanup
		{
			std::filesystem::path m_path;
			~Cleanup()
			{
				std::error_code error;
				std::filesystem::remove_all(m_path, error);
			}
		} cleanup{cache};
		// The source checkout is unavailable while the seed is installed.
		std::filesystem::rename(source, fixture.Path("hidden-source"));
		TVector<ArchiveMountPtr> mounts;
		Require(Workspace::MountPackageContent(resolved.m_context, mounts, diagnostic), diagnostic);
		std::string contents;
		Require(ReadText(cache / "CompiledShaders" / "shader.spirv", contents) && contents == "spirv fixture",
			"compiled artifacts must survive relocation without the source workspace");
		Require(ReadText(cache / "Lods" / "mesh.bin", contents) && contents == "lod fixture",
			"portable model caches must seed writable storage");
		Require(!std::filesystem::exists(cache / "AssetCache.yaml") &&
			!std::filesystem::exists(cache / "EditorSettings.yaml") && !std::filesystem::exists(cache / "Build"),
			"initial cache must exclude authoring state and absolute source indexes");
		const auto identity = Workspace::MakeWorkspaceCacheIdentity("shader-cache", "shader-compiler-v1", 1,
			resolved.m_context);
		const auto loaded = Workspace::LoadWorkspaceCacheEnvelope(cache / "ShaderCache.yaml", identity);
		Require(loaded.IsLoaded() && loaded.m_payload == payload,
			"seed must bind the shader envelope to the relocated workspace identity");
		Save(cache / "Lods" / "mesh.bin", "runtime update");
		std::filesystem::remove(game / "InitialCache.zip");
		Require(Workspace::MountPackageContent(resolved.m_context, mounts, diagnostic) &&
			ReadText(cache / "Lods" / "mesh.bin", contents) && contents == "runtime update",
			"later launches must preserve writable cache updates and skip reseeding");
		std::filesystem::remove_all(cache);
		Save(game / "InitialCache.zip", RawZip({{"EditorSettings.yaml", "unexpected authoring state"}}));
		Require(!Workspace::MountPackageContent(resolved.m_context, mounts, diagnostic) &&
			!std::filesystem::exists(cache), "failed seeding must leave no partial runtime cache");
	}
}

int main()
{
	try
	{
		for (auto compression : { EArchiveCompression::Stored, EArchiveCompression::Fastest,
			EArchiveCompression::Balanced, EArchiveCompression::Smallest })
		{
			TestCompressionAndMount(compression);
		}
		TestInvalidArchives();
		TestPackagedWorkspace();
		TestInitialCache();
		std::cout << "Archive filesystem behavior passed\n";
		return 0;
	}
	catch (const std::exception& exception)
	{
		std::cerr << exception.what() << '\n';
		return 1;
	}
}
