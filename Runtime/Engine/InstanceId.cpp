#include "Engine/InstanceId.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#ifdef _WIN32
#include <combaseapi.h>
#endif
#include <random>
#include <iomanip>
#include <sstream>
#include <string_view>

using namespace Sailor;

namespace
{
	constexpr size_t LegacyGameObjectIdLength = 16;
	constexpr size_t CanonicalGameObjectIdLength = 20;
	constexpr size_t LegacyComponentIdLength = 16;
	constexpr size_t CanonicalComponentIdLength = 32;

	bool IsHexString(std::string_view id)
	{
		return std::all_of(id.begin(), id.end(), [](unsigned char character)
			{
				return std::isxdigit(character) != 0;
			});
	}

	bool IsHexGameObjectId(std::string_view id)
	{
		return id.length() >= LegacyGameObjectIdLength &&
			id.length() <= CanonicalGameObjectIdLength &&
			IsHexString(id);
	}

	bool IsHexComponentId(std::string_view id)
	{
		return (id.length() == LegacyComponentIdLength || id.length() == CanonicalComponentIdLength) &&
			IsHexString(id);
	}

	bool SplitComponentInstanceId(
		std::string_view instanceId,
		std::string_view& outComponentId,
		std::string_view& outGameObjectId)
	{
		const size_t separator = instanceId.find('_');
		if (separator == std::string_view::npos ||
			instanceId.find('_', separator + 1) != std::string_view::npos)
		{
			return false;
		}

		outComponentId = instanceId.substr(0, separator);
		outGameObjectId = instanceId.substr(separator + 1);
		return IsHexComponentId(outComponentId) && IsHexGameObjectId(outGameObjectId);
	}
}

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
	const uint16_t randomSuffix = static_cast<uint16_t>(dis(gen));

	std::stringstream ss;
	ss << std::uppercase << std::hex << std::setfill('0')
		<< std::setw(16) << randomNumber
		<< std::setw(4) << randomSuffix;

	newInstanceId.m_instanceId = StringHash::Runtime(ss.str());

	return newInstanceId;
}

InstanceId InstanceId::GenerateNewComponentId(const InstanceId& gameObjectId)
{
	InstanceId newComponentInstanceId;

	char buffer[17];

#ifdef _WIN32
	::GUID win32;
	CoCreateGuid(&win32);
	sprintf_s(buffer, "%08lX%04hX%04hX", win32.Data1, win32.Data2, win32.Data3);
#else
	std::random_device rd;
	std::mt19937_64 gen(rd());
	std::uniform_int_distribution<uint64_t> dis;
	const uint64_t randomNumber = dis(gen);
	std::snprintf(buffer, sizeof(buffer), "%016llX", static_cast<unsigned long long>(randomNumber));
#endif

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
	return IsHexGameObjectId(m_instanceId.ToString());
}

InstanceId InstanceId::GameObjectId() const
{
	const std::string& idString = m_instanceId.ToString();
	std::string_view componentIdString;
	std::string_view gameObjectIdString;
	if (SplitComponentInstanceId(idString, componentIdString, gameObjectIdString))
	{
		InstanceId id{};
		id.m_instanceId = StringHash::Runtime(std::string(gameObjectIdString));
		return id;
	}
	else if (idString.find('_') == std::string::npos && IsGameObjectId())
	{
		return *this;
	}

	return InstanceId::Invalid;
}

InstanceId InstanceId::ComponentId() const
{
	const std::string& idString = m_instanceId.ToString();
	std::string_view componentIdString;
	std::string_view gameObjectIdString;
	if (SplitComponentInstanceId(idString, componentIdString, gameObjectIdString))
	{
		InstanceId id{};
		id.m_instanceId = StringHash::Runtime(std::string(componentIdString));
		return id;
	}

	return InstanceId::Invalid;
}
