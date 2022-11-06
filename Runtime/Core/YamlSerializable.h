#pragma once
#include "yaml-cpp/include/yaml-cpp/yaml.h"
#include "Core/Defines.h"
#include "Containers/Concepts.h"
#include "Math/Math.h"
#include "Containers/Containers.h"

namespace Sailor
{
	class SAILOR_API IYamlSerializable
	{
	public:

		virtual void Serialize(YAML::Node& outData) const = 0;
		virtual void Deserialize(const YAML::Node& inData) = 0;
	};

	// Cannot move enum to internal YAML convertions, since there is no way to use extra template arg
	template<typename T>
	void SerializeEnum(YAML::Node& j, typename std::enable_if< std::is_enum<T>::value, T >::type enumeration)
	{
		j = magic_enum::enum_name(enumeration);
	}

	template<typename T>
	void DeserializeEnum(const YAML::Node& j, typename std::enable_if< std::is_enum<T>::value, T >::type& outEnumeration)
	{
		auto value = magic_enum::enum_cast<T>(j.as<std::string>());
		assert(value.has_value());
		outEnumeration = value.value();
	}
}

namespace YAML
{
	template<>
	struct convert<Sailor::IYamlSerializable>
	{
		static Node encode(const Sailor::IYamlSerializable& rhs)
		{
			Node node;
			rhs.Serialize(node);
			return node;
		}

		static bool decode(const Node& node, Sailor::IYamlSerializable& rhs)
		{
			rhs.Deserialize(node);
			return true;
		}
	};

	template<typename T>
	struct convert<Sailor::TVector<T>>
	{
		static Node encode(const Sailor::TVector<T>& rhs)
		{
			Node node;
			for (const auto& el : rhs)
			{
				node.push_back(el);
			}
			return node;
		}
		static bool decode(const Node& node, Sailor::TVector<T>& rhs)
		{
			rhs.Clear();
			rhs.Reserve(node.size());

			if (!node.IsSequence())
			{
				return false;
			}

			for (std::size_t i = 0; i < node.size(); i++)
			{
				if constexpr (IsBaseOf<Sailor::IYamlSerializable, T>)
				{
					T value;
					value.Deserialize(node[i]);
					rhs.Emplace(std::move(value));
				}
				else
				{
					auto value = node[i].as<T>();
					rhs.Emplace(std::move(value));
				}
			}

			return true;
		}
	};

	template<typename TKeyType, typename TValueType>
	struct convert<Sailor::TPair<TKeyType, TValueType>>
	{
		static Node encode(const Sailor::TPair<TKeyType, TValueType>& rhs)
		{
			Node node;

			node.push_back(rhs.First());
			node.push_back(rhs.Second());

			return node;
		}

		static bool decode(const Node& node, Sailor::TPair<TKeyType, TValueType>& rhs)
		{
			rhs.Clear();
			rhs.m_first = std::move(node[0].as<TKeyType>());
			rhs.m_second = std::move(node[1].as<TValueType>());
			return true;
		}
	};

	template<typename T>
	struct convert<Sailor::TSet<T>>
	{
		static Node encode(const Sailor::TSet<T>& rhs)
		{
			Node node;

			for (const auto& el : rhs)
			{
				node.push_back(el);
			}

			return node;
		}

		static bool decode(const Node& node, Sailor::TSet<T>& rhs)
		{
			rhs.Clear();
			for (const auto& el : node)
			{
				rhs.Insert(el.as<T>());
			}

			return true;
		}
	};

	template<>
	struct convert<glm::vec2>
	{
		static Node encode(const glm::vec2& rhs)
		{
			Node node;
			node[0] = rhs.x;
			node[1] = rhs.y;
			return node;
		}

		static bool decode(const Node& node, glm::vec2& rhs)
		{
			rhs.x = node[0].as<float>();
			rhs.y = node[1].as<float>();
			return true;
		}
	};

	template<>
	struct convert<glm::vec3>
	{
		static Node encode(const glm::vec3& rhs)
		{
			Node node;
			node[0] = rhs.x;
			node[1] = rhs.y;
			node[2] = rhs.z;
			return node;
		}

		static bool decode(const Node& node, glm::vec3& rhs)
		{
			rhs.x = node[0].as<float>();
			rhs.y = node[1].as<float>();
			rhs.z = node[2].as<float>();
			return true;
		}
	};

	template<>
	struct convert<glm::vec4>
	{
		static Node encode(const glm::vec4& rhs)
		{
			Node node;
			node[0] = rhs.x;
			node[1] = rhs.y;
			node[2] = rhs.z;
			node[3] = rhs.w;
			return node;
		}

		static bool decode(const Node& node, glm::vec4& rhs)
		{
			rhs.x = node[0].as<float>();
			rhs.y = node[1].as<float>();
			rhs.z = node[2].as<float>();
			rhs.w = node[2].as<float>();
			return true;
		}
	};

	template<>
	struct convert<glm::quat>
	{
		static Node encode(const glm::quat& rhs)
		{
			Node node;
			node[0] = rhs.x;
			node[1] = rhs.y;
			node[2] = rhs.z;
			node[3] = rhs.w;
			return node;
		}

		static bool decode(const Node& node, glm::quat& rhs)
		{
			rhs.x = node[0].as<float>();
			rhs.y = node[1].as<float>();
			rhs.z = node[2].as<float>();
			rhs.w = node[2].as<float>();
			return true;
		}
	};
}
