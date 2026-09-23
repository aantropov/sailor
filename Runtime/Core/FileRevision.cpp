#include "Core/FileRevision.h"

#include "Core/YamlSerializable.h"

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
