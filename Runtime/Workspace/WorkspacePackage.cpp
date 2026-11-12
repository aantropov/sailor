#include "Workspace/WorkspacePackage.h"
#include "Core/JsonUtils.h"
#include "Workspace/WorkspacePathEncoding.h"

#include <nlohmann/json.hpp>

using namespace Sailor;
using namespace Sailor::Workspace;

namespace
{
	bool ResolvePackagePath(const std::filesystem::path& root, const std::string& text,
		std::filesystem::path& result)
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
		result = std::filesystem::canonical(root / relative, error);
		return !error && IsPathWithin(root, result);
	}

	bool SamePath(const std::filesystem::path& first, const std::filesystem::path& second)
	{
		return IsPathWithin(first, second) && IsPathWithin(second, first);
	}
}

bool Workspace::MountPackageContent(const WorkspaceContext& context,
	TVector<FileSystem::ArchiveMountPtr>& outMounts, std::string& outDiagnostic)
{
	outDiagnostic.clear();
	outMounts.Clear();
	try
	{
		const auto manifest = context.GetRoot() / "GamePackage.json";
		std::error_code error;
		const auto status = std::filesystem::symlink_status(manifest, error);
		if (error == std::errc::no_such_file_or_directory || (!error && status.type() == std::filesystem::file_type::not_found))
		{
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
		const auto document = Utils::ParseJsonWithUniqueKeys(text);
		if (!document.is_object() || document.size() != 2 || !document.at("packageVersion").is_number_unsigned() ||
			document.at("packageVersion") != 1 || !document.at("archives").is_array() || document.at("archives").size() > 2)
		{
			outDiagnostic = "GamePackage.json must contain packageVersion 1 and a content archive list.";
			return false;
		}
		TVector<FileSystem::ArchiveMountPtr> staged;
		for (const auto& archive : document.at("archives"))
		{
			std::filesystem::path root;
			std::filesystem::path file;
			if (!archive.is_object() || archive.size() != 2 ||
				!ResolvePackagePath(context.GetRoot(), archive.at("root").get<std::string>(), root) ||
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
		outMounts = std::move(staged);
		return true;
	}
	catch (const std::exception& exception)
	{
		outDiagnostic = "Cannot load GamePackage.json: " + std::string(exception.what());
		return false;
	}
}
