#include "Workspace/WorkspacePackage.h"
#include "Core/JsonUtils.h"
#include "Workspace/WorkspacePathEncoding.h"
#include "Workspace/WorkspaceCacheContract.h"
#include "AssetRegistry/Shader/ShaderCompiler.h"
#include "AssetRegistry/FileId.h"

#include <nlohmann/json.hpp>
#include <array>
#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <memory>

using namespace Sailor;
using namespace Sailor::Workspace;

namespace
{
	struct TemporaryDirectory
	{
		std::filesystem::path m_path;
		bool m_bOwned = false;
		~TemporaryDirectory()
		{
			if (m_bOwned)
			{
				std::error_code error;
				std::filesystem::remove_all(m_path, error);
			}
		}
	};

	WorkspaceCacheIdentity ShaderIdentity(const WorkspaceContext& context)
	{
		return MakeWorkspaceCacheIdentity("shader-cache",
			"shader-compiler-v" + std::to_string(ShaderCompiler::CacheProducerVersion), 1, context);
	}

	bool PublishShaderIdentity(const std::filesystem::path& manifest,
		const WorkspaceCacheIdentity& expected, const WorkspaceCacheIdentity& destination,
		std::string& outDiagnostic)
	{
		if (!std::filesystem::exists(manifest))
		{
			return true;
		}
		const auto result = LoadWorkspaceCacheEnvelope(manifest, expected);
		std::string envelope;
		if (!result.IsLoaded() ||
			!SerializeWorkspaceCacheEnvelope(destination, result.m_payload, envelope, outDiagnostic))
		{
			outDiagnostic = "Initial shader cache must match the exported runtime: " +
				result.m_diagnostic + " " + outDiagnostic;
			return false;
		}
		std::ofstream output(manifest, std::ios::binary | std::ios::trunc);
		output.write(envelope.data(), static_cast<std::streamsize>(envelope.size()));
		output.close();
		if (!output)
		{
			outDiagnostic = "Cannot write the initial shader cache envelope.";
			return false;
		}
		return true;
	}

	bool ReadPackage(const std::filesystem::path& root, nlohmann::json& document,
		std::string& outDiagnostic)
	{
		const auto manifest = root / "GamePackage.json";
		std::error_code error;
		const auto status = std::filesystem::symlink_status(manifest, error);
		if (error == std::errc::no_such_file_or_directory ||
			(!error && status.type() == std::filesystem::file_type::not_found))
		{
			document = nullptr;
			return true;
		}
		if (error || !std::filesystem::is_regular_file(status) ||
			std::filesystem::file_size(manifest, error) > 1024 * 1024 || error)
		{
			outDiagnostic = "Cannot inspect GamePackage.json, or the manifest exceeds its size limit.";
			return false;
		}
		std::string text;
		if (!FileSystem::ReadText(manifest, text))
		{
			outDiagnostic = "Cannot read GamePackage.json.";
			return false;
		}
		document = Utils::ParseJsonWithUniqueKeys(text);
		if (!document.is_object() || document.size() != 4 ||
			!document.at("packageVersion").is_number_unsigned() || document.at("packageVersion") != 1 ||
			!document.at("archives").is_array() || document.at("archives").size() > 2 ||
			!document.at("packageId").is_string() ||
			(!document.at("initialCache").is_null() && !document.at("initialCache").is_string()))
		{
			outDiagnostic = "GamePackage.json requires packageVersion 1, packageId, archives and initialCache.";
			return false;
		}
		const auto& id = document.at("packageId").get_ref<const std::string&>();
		if (id.size() != 32 || !std::all_of(id.begin(), id.end(), [](char value)
			{ return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f'); }))
		{
			outDiagnostic = "A packageId must contain 32 lowercase hexadecimal characters.";
			return false;
		}
		return true;
	}

	bool IsSeedEntry(const std::filesystem::path& relative, bool directory)
	{
		if (relative.empty() || relative.is_absolute())
		{
			return false;
		}
		const auto first = *relative.begin();
		return first == "CompiledShaders" || first == "CompiledShadersWithDebug" || first == "Lods" ||
			(!directory && relative == "ShaderCache.yaml");
	}

	bool CopySeedTree(const std::filesystem::path& source, const std::filesystem::path& destination,
		const std::filesystem::path& relative, std::string& outDiagnostic, bool bSkipUnowned = false)
	{
		TVector<std::filesystem::path> entries;
		std::error_code error;
		if (!FileSystem::Enumerate(source / relative, entries, error))
		{
			outDiagnostic = "Cannot enumerate initial cache: " + error.message();
			return false;
		}
		for (const auto& entry : entries)
		{
			const auto child = relative / entry.filename();
			const auto status = FileSystem::Status(entry, error);
			const bool directory = std::filesystem::is_directory(status);
			if (!error && relative.empty() && bSkipUnowned && !IsSeedEntry(child, directory))
			{
				continue;
			}
			if (error || !IsSeedEntry(child, directory) ||
				(!directory && !std::filesystem::is_regular_file(status)) ||
				(!FileSystem::IsReadOnly(entry) && std::filesystem::is_symlink(std::filesystem::symlink_status(entry))))
			{
				outDiagnostic = "Initial cache contains an unsupported entry: " + PathToUtf8(child);
				return false;
			}
			if (directory)
			{
				std::filesystem::create_directory(destination / child);
				if (!CopySeedTree(source, destination, child, outDiagnostic))
				{
					return false;
				}
				continue;
			}
			auto input = FileSystem::OpenRead(entry);
			std::ofstream output(destination / child, std::ios::binary);
			if (!input || !output)
			{
				outDiagnostic = "Cannot open initial cache entry: " + PathToUtf8(child);
				return false;
			}
			std::array<char, 64 * 1024> buffer;
			while (input->read(buffer.data(), buffer.size()) || input->gcount() != 0)
			{
				output.write(buffer.data(), input->gcount());
			}
			output.close();
			if (!input->eof() || input->bad() || !output)
			{
				outDiagnostic = "Cannot copy initial cache entry: " + PathToUtf8(child);
				return false;
			}
		}
		return true;
	}

	bool ResolvePackagePath(const std::filesystem::path& root, const std::string& text,
		std::filesystem::path& result, bool bDirectoryNamespace = false)
	{
		const auto relative = PathFromUtf8(text);
		if (relative.empty() || relative.is_absolute() || relative.has_root_name() ||
			text.find(':') != text.npos || text.find('\\') != text.npos || text.find('\0') != text.npos)
		{
			return false;
		}
		for (const auto& part : relative)
		{
			if (part == "..")
			{
				return false;
			}
		}
		std::error_code error;
		result = bDirectoryNamespace
			? std::filesystem::weakly_canonical(root / relative, error)
			: std::filesystem::canonical(root / relative, error);
		if (error || !IsPathWithin(root, result))
		{
			return false;
		}
		const auto status = std::filesystem::status(result, error);
		return !bDirectoryNamespace ||
			(error == std::errc::no_such_file_or_directory ||
				(!error && (status.type() == std::filesystem::file_type::not_found || std::filesystem::is_directory(status))));
	}

	bool SamePath(const std::filesystem::path& first, const std::filesystem::path& second)
	{
		return IsPathWithin(first, second) && IsPathWithin(second, first);
	}

	std::filesystem::path UserCacheRoot()
	{
#if defined(_WIN32)
		wchar_t* local = nullptr;
		size_t length = 0;
		if (_wdupenv_s(&local, &length, L"LOCALAPPDATA") != 0)
		{
			return {};
		}
		std::unique_ptr<wchar_t, decltype(&std::free)> owner(local, &std::free);
		return local && length > 1 ? std::filesystem::path(local) : std::filesystem::path{};
#else
#if !defined(__APPLE__)
		if (const char* xdg = std::getenv("XDG_CACHE_HOME"); xdg && *xdg)
		{
			const auto root = PathFromUtf8(xdg);
			if (root.is_absolute())
			{
				return root;
			}
		}
#endif
		const char* home = std::getenv("HOME");
		if (!home || !*home)
		{
			return {};
		}
#if defined(__APPLE__)
		return PathFromUtf8(home) / "Library" / "Caches";
#else
		return PathFromUtf8(home) / ".cache";
#endif
#endif
	}

	bool InitializeCache(const WorkspaceContext& context, const nlohmann::json& document,
		std::string& outDiagnostic)
	{
		std::filesystem::path cache;
		TVector<std::filesystem::path> contentRoots;
		if (!Workspace::ResolvePackagePaths(context.GetRoot(), cache, contentRoots, outDiagnostic) ||
			!SamePath(cache, context.GetCache()))
		{
			outDiagnostic = "Package cache changed after workspace resolution. " + outDiagnostic;
			return false;
		}
		const auto status = std::filesystem::symlink_status(cache);
		if (std::filesystem::exists(status))
		{
			if (!std::filesystem::is_directory(status))
			{
				outDiagnostic = "Package cache is not a regular directory: " + PathToUtf8(cache);
				return false;
			}
			return true;
		}
		std::filesystem::create_directories(cache.parent_path());
		if (document.at("initialCache").is_null())
		{
			std::filesystem::create_directory(cache);
			return true;
		}
		std::filesystem::path archive;
		std::filesystem::path source;
		if (!ResolvePackagePath(context.GetRoot(), document.at("initialCache").get<std::string>(), archive) ||
			!ResolvePackagePath(context.GetRoot(), "InitialCache", source, true))
		{
			outDiagnostic = "Initial cache archive and its InitialCache mount directory must be inside the package.";
			return false;
		}
		auto mount = FileSystem::MountZipArchive(source, archive, outDiagnostic);
		if (!mount)
		{
			return false;
		}
		TemporaryDirectory staging{ cache.parent_path() / (".seed-" + FileId::CreateNewFileId().ToString()) };
		staging.m_bOwned = std::filesystem::create_directory(staging.m_path);
		if (!staging.m_bOwned ||
			!CopySeedTree(source, staging.m_path, {}, outDiagnostic))
		{
			return false;
		}
		const auto destinationIdentity = ShaderIdentity(context);
		auto seedIdentity = destinationIdentity;
		seedIdentity.m_workspaceId = "package-cache-seed";
		if (!PublishShaderIdentity(staging.m_path / "ShaderCache.yaml", seedIdentity,
			destinationIdentity, outDiagnostic))
		{
			return false;
		}
		// Publish a complete seed once. An existing cache belongs to a prior run.
		std::error_code error;
		std::filesystem::rename(staging.m_path, cache, error);
		if (error && !std::filesystem::is_directory(std::filesystem::symlink_status(cache)))
		{
			outDiagnostic = "Cannot publish package cache: " + error.message();
			return false;
		}
		return true;
	}
}

bool Workspace::ResolvePackagePaths(const std::filesystem::path& root,
	std::filesystem::path& outCache, TVector<std::filesystem::path>& outContentRoots,
	std::string& outDiagnostic)
{
	outCache.clear();
	outContentRoots.Clear();
	outDiagnostic.clear();
	try
	{
		nlohmann::json document;
		if (!ReadPackage(root, document, outDiagnostic))
		{
			return false;
		}
		if (document.is_null())
		{
			return true;
		}
		for (const auto& archive : document.at("archives"))
		{
			std::filesystem::path contentRoot;
			std::filesystem::path file;
			if (!archive.is_object() || archive.size() != 2 ||
				!ResolvePackagePath(root, archive.at("root").get<std::string>(), contentRoot, true) ||
				!ResolvePackagePath(root, archive.at("file").get<std::string>(), file) ||
				file.extension() != ".bundle")
			{
				outDiagnostic = "Package content requires an owned virtual root and an existing .bundle file.";
				return false;
			}
			outContentRoots.Add(contentRoot);
		}
		if (document.at("initialCache").is_string() &&
			!document.at("initialCache").get_ref<const std::string&>().ends_with(".bundle"))
		{
			outDiagnostic = "Initial cache requires a .bundle file.";
			return false;
		}
		const auto userRoot = UserCacheRoot();
		if (!userRoot.is_absolute())
		{
			outDiagnostic = "Cannot resolve the user's writable package cache directory.";
			return false;
		}
		outCache = std::filesystem::weakly_canonical(userRoot / "Sailor" / "Packages" /
			PathFromUtf8(document.at("packageId").get<std::string>()));
		return true;
	}
	catch (const std::exception& exception)
	{
		outDiagnostic = "Cannot resolve package cache: " + std::string(exception.what());
		return false;
	}
}

bool Workspace::WriteInitialCacheArchive(const WorkspaceContext& context,
	const std::filesystem::path& destination, FileSystem::EArchiveCompression compression,
	std::string& outDiagnostic)
{
	outDiagnostic.clear();
	try
	{
		TemporaryDirectory staging{ std::filesystem::temp_directory_path() /
			("sailor-cache-" + FileId::CreateNewFileId().ToString()) };
		staging.m_bOwned = std::filesystem::create_directory(staging.m_path);
		if (!staging.m_bOwned)
		{
			outDiagnostic = "Cannot create initial cache staging directory.";
			return false;
		}
		if (!CopySeedTree(context.GetCache(), staging.m_path, {}, outDiagnostic, true))
		{
			return false;
		}
		const auto sourceIdentity = ShaderIdentity(context);
		auto seedIdentity = sourceIdentity;
		seedIdentity.m_workspaceId = "package-cache-seed";
		if (!PublishShaderIdentity(staging.m_path / "ShaderCache.yaml", sourceIdentity,
			seedIdentity, outDiagnostic))
		{
			return false;
		}
		return FileSystem::WriteZipArchive(staging.m_path, destination, compression, outDiagnostic);
	}
	catch (const std::exception& exception)
	{
		outDiagnostic = "Cannot export initial cache: " + std::string(exception.what());
		return false;
	}
}

bool Workspace::MountPackageContent(const WorkspaceContext& context,
	TVector<FileSystem::ArchiveMountPtr>& outMounts, std::string& outDiagnostic)
{
	outDiagnostic.clear();
	outMounts.Clear();
	try
	{
		nlohmann::json document;
		if (!ReadPackage(context.GetRoot(), document, outDiagnostic))
		{
			return false;
		}
		if (document.is_null())
		{
			return true;
		}
		TVector<FileSystem::ArchiveMountPtr> staged;
		for (const auto& archive : document.at("archives"))
		{
			std::filesystem::path root;
			std::filesystem::path file;
			if (!archive.is_object() || archive.size() != 2 ||
				!ResolvePackagePath(context.GetRoot(), archive.at("root").get<std::string>(), root, true) ||
				!ResolvePackagePath(context.GetRoot(), archive.at("file").get<std::string>(), file) ||
				(!SamePath(root, context.GetContent()) && !SamePath(root, context.GetEngineContent())))
			{
				outDiagnostic = "Package archives must target the workspace/engine content roots inside the package.";
				return false;
			}
			auto mount = FileSystem::MountZipArchive(root, file, outDiagnostic);
			if (!mount)
			{
				return false;
			}
			staged.Add(std::move(mount));
		}
		if (!InitializeCache(context, document, outDiagnostic))
		{
			return false;
		}
		outMounts = std::move(staged);
		return true;
	}
	catch (const std::exception& exception)
	{
		outDiagnostic = "Cannot load GamePackage.json: " + std::string(exception.what());
		return false;
	}
}
