#pragma once
#include <nlohmann_json/include/nlohmann/json.hpp>
#include "Core/Defines.h"
#include "Math/Math.h"
#include "Containers/Vector.h"
#include "Containers/Set.h"

using json = nlohmann::json;

namespace Sailor
{
	class IJsonSerializable
	{
	public:

		virtual SAILOR_API void Serialize(nlohmann::json& outData) const = 0;
		virtual SAILOR_API void Deserialize(const nlohmann::json& inData) = 0;
	};

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
	void to_json(json& j, const glm::vec2& P);
	void from_json(const json& j, glm::vec2& P);
	void to_json(json& j, const glm::vec3& P);
	void from_json(const json& j, glm::vec3& P);
	void to_json(json& j, const glm::vec4& P);
	void from_json(const json& j, glm::vec4& P);
	void to_json(json& j, const glm::quat& P);
	void from_json(const json& j, glm::quat& P);
}