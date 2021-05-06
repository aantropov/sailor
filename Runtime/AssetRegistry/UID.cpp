#include "UID.h"
#include <combaseapi.h>
#include <corecrt_io.h>

using namespace Sailor;
using namespace nlohmann;

void ns::to_json(json& j, const UID& p)
{
	j = json{ {"uid", p.m_UID} };
}

void ns::from_json(const json& j, UID& p)
{
	j.at("uid").get_to<std::string>(p.m_UID);
}

const std::string& UID::ToString() const
{
	return m_UID;
}

void UID::operator=(const UID& inUID)
{
	m_UID = inUID.m_UID;
}

bool UID::operator==(const UID& rhs) const
{
	return m_UID == rhs.m_UID;
}

UID UID::CreateNewUID()
{
	UID newuid;

	::GUID win32;
	HRESULT hr = CoCreateGuid(&win32);

	char buffer[128];
	sprintf_s(buffer, "{%08lX-%04hX-%04hX-%02hhX%02hhX-%02hhX%02hhX%02hhX%02hhX%02hhX%02hhX}",
		win32.Data1, win32.Data2, win32.Data3,
		win32.Data4[0], win32.Data4[1], win32.Data4[2], win32.Data4[3],
		win32.Data4[4], win32.Data4[5], win32.Data4[6], win32.Data4[7]);

	newuid.m_UID = std::string(buffer);
	return newuid;
}
