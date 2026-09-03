#include "Core/FileRevision.h"

#include "Core/YamlSerializable.h"

using namespace Sailor;

YAML::Node FileRevision::Serialize() const
{
	YAML::Node outData(YAML::NodeType::Map);
	SERIALIZE_PROPERTY(outData, m_modificationTimeNanoseconds);
	SERIALIZE_PROPERTY(outData, m_fileSize);
	SERIALIZE_PROPERTY(outData, m_contentHash);
	return outData;
}

void FileRevision::Deserialize(const YAML::Node& inData)
{
	m_bIsValid =
		DESERIALIZE_PROPERTY(inData, m_modificationTimeNanoseconds) &&
		DESERIALIZE_PROPERTY(inData, m_fileSize) &&
		DESERIALIZE_PROPERTY(inData, m_contentHash);
}
