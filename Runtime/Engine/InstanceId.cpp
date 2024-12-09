#include "Engine/InstanceId.h"
#include <corecrt_io.h>
#include <combaseapi.h>
#include <random>
#include <iomanip>

using namespace Sailor;

const InstanceId InstanceId::Invalid = InstanceId();

YAML::Node InstanceId::Serialize() const
{
	YAML::Node outData;
	outData = m_instanceId.ToString();
	return outData;
}

InstanceId::InstanceId(const InstanceId& inComponentId, const InstanceId& inGameObjectId)
{
	if (inGameObjectId)
	{
		std::string combinedId = inComponentId.ToString() + "_" + inGameObjectId.ToString();
		m_instanceId = StringHash::Runtime(combinedId);
	}
	else
	{
		*this = inComponentId;
	}
}

void InstanceId::Deserialize(const YAML::Node& inData)
{
	m_instanceId = StringHash::Runtime(inData.as<std::string>());
}

const std::string& InstanceId::ToString() const
{
	return m_instanceId.ToString();
}

bool InstanceId::operator==(const InstanceId& rhs) const
{
	return m_instanceId == rhs.m_instanceId;
}

InstanceId InstanceId::GenerateNewInstanceId()
{
	InstanceId newInstanceId;

	std::random_device rd;
	std::mt19937_64 gen(rd());
	std::uniform_int_distribution<uint64_t> dis;

	uint64_t randomNumber = dis(gen);

	std::stringstream ss;
	ss << std::setw(16) << std::setfill('0') << randomNumber;

	newInstanceId.m_instanceId = StringHash::Runtime(ss.str());

	return newInstanceId;
}

InstanceId InstanceId::GenerateNewComponentId(const InstanceId& gameObjectId)
{
	InstanceId newComponentInstanceId;

	::GUID win32;
	CoCreateGuid(&win32);

	char buffer[17];
	sprintf_s(buffer, "%08lX%04hX%04hX", win32.Data1, win32.Data2, win32.Data3);

	newComponentInstanceId.m_instanceId = StringHash::Runtime(buffer);

	if (gameObjectId)
	{
		std::string combinedId = newComponentInstanceId.ToString() + "_" + gameObjectId.ToString();
		newComponentInstanceId.m_instanceId = StringHash::Runtime(combinedId);
	}

	return newComponentInstanceId;
}

bool InstanceId::IsGameObjectId() const
{
	std::string idString = m_instanceId.ToString();
	return idString.length() == 20 && std::all_of(idString.begin(), idString.end(), ::isxdigit);
}

InstanceId InstanceId::GameObjectId() const
{
	std::string idString = m_instanceId.ToString();
	size_t pos = idString.find('_');
	if (pos != std::string::npos)
	{
		std::string gameObjectIdString = idString.substr(pos + 1);

		InstanceId id{};
		id.m_instanceId = StringHash::Runtime(gameObjectIdString);
		return id;
	}
	else if (IsGameObjectId())
	{
		return *this;
	}

	return InstanceId::Invalid;
}

InstanceId InstanceId::ComponentId() const
{
	std::string idString = m_instanceId.ToString();
	size_t pos = idString.find('_');
	if (pos != std::string::npos)
	{
		std::string componentIdString = idString.substr(0, pos);

		InstanceId id{};
		id.m_instanceId = StringHash::Runtime(componentIdString);
		return id;
	}

	return InstanceId::Invalid;
}