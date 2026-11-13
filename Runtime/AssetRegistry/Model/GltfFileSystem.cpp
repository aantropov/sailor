#include "AssetRegistry/Model/GltfImporterUtils.h"
#include "Platform/FileSystem.h"
#include "Workspace/WorkspacePathEncoding.h"

#include <tiny_gltf.h>

using namespace Sailor;

void GltfImporterUtils::ConfigureFileSystem(tinygltf::TinyGLTF& loader)
{
	tinygltf::FsCallbacks callbacks{};
	callbacks.FileExists = [](const std::string& path, void*)
	{
		std::error_code error;
		return std::filesystem::is_regular_file(FileSystem::Status(Workspace::PathFromUtf8(path), error)) && !error;
	};
	callbacks.ExpandFilePath = [](const std::string& path, void*)
	{
		return path;
	};
	callbacks.ReadWholeFile = [](std::vector<unsigned char>* output, std::string* error,
		const std::string& path, void*)
	{
		TVector<unsigned char> bytes;
		if (!FileSystem::ReadBinary(Workspace::PathFromUtf8(path), bytes))
		{
			output->clear();
			if (error)
			{
				*error = "Cannot read glTF resource: " + path;
			}
			return false;
		}
		output->assign(bytes.begin(), bytes.end());
		return true;
	};
	callbacks.WriteWholeFile = [](std::string* error, const std::string&,
		const std::vector<unsigned char>&, void*)
	{
		if (error)
		{
			*error = "The glTF importer filesystem is read-only.";
		}
		return false;
	};
	callbacks.GetFileSizeInBytes = [](size_t* output, std::string* diagnostic,
		const std::string& path, void*)
	{
		std::error_code error;
		const uint64_t size = FileSystem::FileSize(Workspace::PathFromUtf8(path), error);
		if (error || size > (std::numeric_limits<size_t>::max)())
		{
			if (diagnostic)
			{
				*diagnostic = "Cannot inspect glTF resource: " + path;
			}
			return false;
		}
		*output = static_cast<size_t>(size);
		return true;
	};
	loader.SetFsCallbacks(std::move(callbacks));
}
