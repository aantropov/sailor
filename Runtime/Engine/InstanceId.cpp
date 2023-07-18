#include "Engine/InstanceId.h"
#include <corecrt_io.h>
#include <combaseapi.h>

using namespace Sailor;
using namespace nlohmann;

const InstanceId InstanceId::Invalid = InstanceId();

YAML::Node InstanceId::Serialize() const
{
	YAML::Node outData;
	outData = m_InstanceId;
	return outData;
}

void InstanceId::Deserialize(const YAML::Node& inData)
{
	m_InstanceId = inData.as<std::string>();
}

void InstanceId::Serialize(nlohmann::json& outData) const
{
	outData = json{ {"InstanceId", m_InstanceId} };
}

void InstanceId::Deserialize(const nlohmann::json& inData)
{
	inData.at("InstanceId").get_to<std::string>(m_InstanceId);
}

const std::string& InstanceId::ToString() const
{
	return m_InstanceId;
}

bool InstanceId::operator==(const InstanceId& rhs) const
{
	return m_InstanceId == rhs.m_InstanceId;
}

InstanceId InstanceId::CreateNewInstanceId()
{
	InstanceId newInstanceId;

	::GUID win32;
	CoCreateGuid(&win32);

	char buffer[128];
	sprintf_s(buffer, "%08lX%04hX%04hX%02hhX%02hhX%02hhX%02hhX%02hhX%02hhX%02hhX%02hhX",
		win32.Data1, win32.Data2, win32.Data3,
		win32.Data4[0], win32.Data4[1], win32.Data4[2], win32.Data4[3],
		win32.Data4[4], win32.Data4[5], win32.Data4[6], win32.Data4[7]);

	newInstanceId.m_InstanceId = std::string(buffer);
	return newInstanceId;
}