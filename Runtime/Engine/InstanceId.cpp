#include "Engine/InstanceId.h"
#include "Containers/Hash.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#ifdef _WIN32
#include <combaseapi.h>
#endif
#include <random>
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

InstanceId::InstanceId(std::string_view value)
{
	Assign(value);
}

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
		Assign(combinedId);
	}
	else
	{
		*this = inComponentId;
	}
}

void InstanceId::Deserialize(const YAML::Node& inData)
{
	Assign(inData.as<std::string>());
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
	std::random_device rd;
	std::mt19937_64 gen(rd());
	std::uniform_int_distribution<uint64_t> dis;
	const uint64_t randomNumber = dis(gen);
	const uint16_t randomSuffix = static_cast<uint16_t>(dis(gen));
	return FromHash(randomNumber, randomSuffix);
}

InstanceId InstanceId::GenerateDeterministic(
	std::initializer_list<std::string_view> values,
	uint32_t variant)
{
	uint64_t hash = Fnv1aOffsetBasis;
	for (const std::string_view value : values)
	{
		HashString(hash, value);
		HashString(hash, "|");
	}
	HashString(hash, std::to_string(variant));

	uint64_t suffix = hash;
	HashString(suffix, "|suffix");
	return FromHash(hash, static_cast<uint16_t>(suffix));
}

InstanceId InstanceId::FromHash(uint64_t hash, uint16_t suffix)
{
	char value[21]{};
	std::snprintf(
		value,
		sizeof(value),
		"%016llX%04X",
		static_cast<unsigned long long>(hash),
		static_cast<unsigned int>(suffix));
	return InstanceId(value);
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

	newComponentInstanceId.Assign(buffer);

	if (gameObjectId)
	{
		std::string combinedId = newComponentInstanceId.ToString() + "_" + gameObjectId.ToString();
		newComponentInstanceId.Assign(combinedId);
	}

	return newComponentInstanceId;
}

bool InstanceId::IsGameObjectId() const
{
	return m_kind == EKind::GameObject;
}

InstanceId InstanceId::GameObjectId() const
{
	if (m_kind == EKind::GameObject)
	{
		return *this;
	}

	if (m_kind == EKind::Component)
	{
		InstanceId result;
		result.m_instanceId = m_gameObjectId;
		result.m_gameObjectId = m_gameObjectId;
		result.m_kind = EKind::GameObject;
		return result;
	}

	return InstanceId::Invalid;
}

InstanceId InstanceId::ComponentId() const
{
	if (m_kind == EKind::Component)
	{
		InstanceId result;
		result.m_instanceId = m_componentId;
		if (m_bStandaloneComponentIsGameObjectId)
		{
			result.m_gameObjectId = m_componentId;
			result.m_kind = EKind::GameObject;
		}
		return result;
	}

	return InstanceId::Invalid;
}

void InstanceId::Assign(std::string_view value)
{
	m_instanceId = StringHash::Runtime(value);
	m_gameObjectId = {};
	m_componentId = {};
	m_kind = EKind::Invalid;
	m_bStandaloneComponentIsGameObjectId = false;

	std::string_view componentId;
	std::string_view gameObjectId;
	if (SplitComponentInstanceId(value, componentId, gameObjectId))
	{
		m_componentId = StringHash::Runtime(componentId);
		m_gameObjectId = StringHash::Runtime(gameObjectId);
		m_kind = EKind::Component;
		m_bStandaloneComponentIsGameObjectId = IsHexGameObjectId(componentId);
	}
	else if (value.find('_') == std::string_view::npos &&
		IsHexGameObjectId(value))
	{
		m_gameObjectId = m_instanceId;
		m_kind = EKind::GameObject;
	}
}
