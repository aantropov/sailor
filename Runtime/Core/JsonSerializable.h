#pragma once
#include <nlohmann_json/include/nlohmann/json.hpp>
#include "Core/Defines.h"
#include "Math/Math.h"
#include "Containers/Containers.h"

using json = nlohmann::json;

namespace Sailor
{
	const uint32_t JsonDumpIndent = 4;

	class SAILOR_API IJsonSerializable
	{
	public:

		virtual void Serialize(nlohmann::json& outData) const = 0;
		virtual void Deserialize(const nlohmann::json& inData) = 0;
	};

	template<typename T>
	void SerializeEnum(json& j, typename std::enable_if< std::is_enum<T>::value, T >::type enumeration)
	{
		j = magic_enum::enum_name(enumeration);
	}

	template<typename T>
	void DeserializeEnum(const json& j, typename std::enable_if< std::is_enum<T>::value, T >::type& outEnumeration)
	{
		auto value = magic_enum::enum_cast<T>(j.get<std::string>());
		check(value.has_value());
		outEnumeration = value.value();
	}

	template<typename TEntryType>
	void SerializeArray(const TVector<TEntryType>& inArray, nlohmann::json& outJson)
	{
		auto jsonArray = nlohmann::json::array();
		for (const auto& entry : inArray)
		{
			nlohmann::json o;
			entry.Serialize(o);
			jsonArray.push_back(o);
		}

		outJson = std::move(jsonArray);
	}

	template<typename TEntryType>
	void DeserializeArray(TVector<TEntryType>& outArray, const nlohmann::json& inJson)
	{
		outArray.Clear();

		for (auto& elem : inJson)
		{
			TEntryType entry;
			entry.Deserialize(elem);
			outArray.Add(std::move(entry));
		}
	}

	template<typename TKeyType, typename TValueType>
	void to_json(json& j, const TPair<TKeyType, TValueType>& p)
	{
		j.push_back(p.First());
		j.push_back(p.Second());
	}

	template<typename TKeyType, typename TValueType>
	void from_json(const json& j, TPair<TKeyType, TValueType>& p)
	{
		p.Clear();
		p.m_first = std::move(j[0].get<TKeyType>());
		p.m_second = std::move(j[1].get<TValueType>());
	}

	template<typename T>
	void to_json(json& j, const TVector<T>& p)
	{
		for (const auto& el : p)
		{
			j.push_back(el);
		}
	}

	template<typename T>
	void from_json(const json& j, TVector<T>& p)
	{
		p.Clear();
		for (const auto& el : j)
		{
			p.Emplace(el.get<T>());
		}
	}

	template<typename T>
	void to_json(json& j, const TSet<T>& p)
	{
		for (const auto& el : p)
		{
			j.push_back(el);
		}
	}

	template<typename T>
	void from_json(const json& j, TSet<T>& p)
	{
		p.Clear();
		for (const auto& el : j)
		{
			p.Insert(el.get<T>());
		}
	}
}

namespace ns
{
	SAILOR_API void to_json(nlohmann::json& j, const class Sailor::IJsonSerializable& p);
	SAILOR_API void from_json(const nlohmann::json& j, class Sailor::IJsonSerializable& p);
}

namespace glm
{
	SAILOR_API void to_json(json& j, const glm::vec2& P);
	SAILOR_API void from_json(const json& j, glm::vec2& P);
	SAILOR_API void to_json(json& j, const glm::vec3& P);
	SAILOR_API void from_json(const json& j, glm::vec3& P);
	SAILOR_API void to_json(json& j, const glm::vec4& P);
	SAILOR_API void from_json(const json& j, glm::vec4& P);
	SAILOR_API void to_json(json& j, const glm::quat& P);
	SAILOR_API void from_json(const json& j, glm::quat& P);
}