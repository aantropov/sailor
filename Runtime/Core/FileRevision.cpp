#include "Core/FileRevision.h"

#include "Core/YamlSerializable.h"

#include <chrono>
#include <filesystem>
#include <sys/stat.h>

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
	struct stat result;
	if (stat(filepath.c_str(), &result) == 0)
	{
		return (std::time_t)result.st_mtime;
	}
	return 0;
}

bool Utils::TryGetFileRevision(
	const std::string& filepath,
	FileRevision& outRevision) noexcept
{
	outRevision = {};
	const std::filesystem::path path(filepath);
	std::error_code error;
	if (!std::filesystem::is_regular_file(path, error) || error)
	{
		return false;
	}

	const auto modificationTime = std::filesystem::last_write_time(path, error);
	if (error)
	{
		return false;
	}
	outRevision.m_modificationTimeNanoseconds = std::chrono::duration_cast<std::chrono::nanoseconds>(
		modificationTime.time_since_epoch()).count();
	outRevision.m_bIsValid = true;
	return true;
}
