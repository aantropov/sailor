#include "Core/FileRevision.h"
#include "Platform/FileSystem.h"

#include "Core/YamlSerializable.h"
#include "Workspace/WorkspacePathEncoding.h"

#include <chrono>
#include <filesystem>

using namespace Sailor;

YAML::Node FileRevision::Serialize() const
{
	YAML::Node outData(YAML::NodeType::Map);
	SERIALIZE_PROPERTY(outData, m_modificationTimeNanoseconds);
	return outData;
}

void FileRevision::Deserialize(const YAML::Node& inData)
{
	*this = {};
	m_bIsValid = inData.IsMap() && inData.size() == 1 &&
		DESERIALIZE_PROPERTY(inData, m_modificationTimeNanoseconds);
}

std::time_t Utils::GetFileModificationTime(const std::string& filepath)
{
	SAILOR_PROFILE_FUNCTION();
	std::error_code error;
	const auto timestamp = FileSystem::LastWriteTime(Workspace::PathFromUtf8(filepath), error);
	if (!error)
	{
#if defined(_MSC_VER)
		const auto systemTime = std::chrono::clock_cast<std::chrono::system_clock>(timestamp);
#else
		const auto systemTime = std::chrono::file_clock::to_sys(timestamp);
#endif
		return static_cast<std::time_t>(std::chrono::floor<std::chrono::seconds>(
			systemTime).time_since_epoch().count());
	}
	return 0;
}

bool Utils::TryGetFileRevision(
	const std::string& filepath,
	FileRevision& outRevision) noexcept
{
	outRevision = {};
	const auto path = Workspace::PathFromUtf8(filepath);
	std::error_code error;
	if (!std::filesystem::is_regular_file(FileSystem::Status(path, error)) || error)
	{
		return false;
	}

	const auto modificationTime = FileSystem::LastWriteTime(path, error);
	if (error)
	{
		return false;
	}
	outRevision.m_modificationTimeNanoseconds = std::chrono::duration_cast<std::chrono::nanoseconds>(
		modificationTime.time_since_epoch()).count();
	outRevision.m_bIsValid = true;
	return true;
}
