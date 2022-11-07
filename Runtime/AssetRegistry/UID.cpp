#include "AssetRegistry/UID.h"
#include <corecrt_io.h>
#include <combaseapi.h>

using namespace Sailor;
using namespace nlohmann;

const UID UID::Invalid = UID();

YAML::Node UID::Serialize() const
{
	YAML::Node outData;
	outData = m_UID;
	return outData;
}

void UID::Deserialize(const YAML::Node& inData)
{
	m_UID = inData.as<std::string>();
}

void UID::Serialize(nlohmann::json& outData) const
{
	outData = json{ {"uid", m_UID} };
}

void UID::Deserialize(const nlohmann::json& inData)
{
	inData.at("uid").get_to<std::string>(m_UID);
}

const std::string& UID::ToString() const
{
	return m_UID;
}

bool UID::operator==(const UID& rhs) const
{
	return m_UID == rhs.m_UID;
}

UID UID::CreateNewUID()
{
	UID newuid;

	::GUID win32;
	CoCreateGuid(&win32);

	char buffer[128];
	sprintf_s(buffer, "{%08lX-%04hX-%04hX-%02hhX%02hhX-%02hhX%02hhX%02hhX%02hhX%02hhX%02hhX}",
		win32.Data1, win32.Data2, win32.Data3,
		win32.Data4[0], win32.Data4[1], win32.Data4[2], win32.Data4[3],
		win32.Data4[4], win32.Data4[5], win32.Data4[6], win32.Data4[7]);

	newuid.m_UID = std::string(buffer);
	return newuid;
}