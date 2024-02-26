#pragma once
#include "Core/Defines.h"
#include "Containers/Concepts.h"
#include "Math/Math.h"
#include "Containers/Containers.h"

#include "yaml-cpp/include/yaml-cpp/yaml.h"

namespace Sailor
{
	class SAILOR_API IYamlSerializable
	{
	public:

		virtual YAML::Node Serialize() const { assert(0); return YAML::Node(); };
		virtual void Deserialize(const YAML::Node& inData) { assert(0); }
	};

	// Cannot move enum to internal YAML convertions, since there is no way to use extra template arg
	template<typename T>
	YAML::Node SerializeEnum(typename std::enable_if< std::is_enum<T>::value, T >::type enumeration)
	{
		YAML::Node j;
		j = std::string(magic_enum::enum_name(enumeration));
		return j;
	}

	template<typename T>
	void DeserializeEnum(const YAML::Node& j, typename std::enable_if< std::is_enum<T>::value, T >::type& outEnumeration)
	{
		auto value = magic_enum::enum_cast<T>(j.as<std::string>());
		check(value.has_value());
		outEnumeration = value.value();
	}
}

namespace YAML
{
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

			if (node.size() == 0)
			{
				return true;
			}

			rhs.Reserve(node.size());

			if (!node.IsSequence())
			{
				return false;
			}

			for (std::size_t i = 0; i < node.size(); i++)
			{
				auto value = node[i].as<T>();
				rhs.Emplace(std::move(value));
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

			node[rhs.First()] = rhs.Second();

			return node;
		}

		static bool decode(const Node& node, Sailor::TPair<TKeyType, TValueType>& rhs)
		{
			rhs.m_first = node.begin()->first.as<TKeyType>();
			rhs.m_second = node.begin()->second.as<TValueType>();

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

	template<typename TKey, typename TValue>
	struct convert<Sailor::TMap<TKey, TValue>>
	{
		static Node encode(const Sailor::TMap<TKey, TValue>& rhs)
		{
			Node node;

			for (const auto& el : rhs)
			{
				node[el.m_first] = *el.m_second;
			}

			return node;
		}

		static bool decode(const Node& node, Sailor::TMap<TKey, TValue>& rhs)
		{
			rhs.Clear();

			if (node.size() == 0)
			{
				return true;
			}

			// TODO: Should we allow parse Vector of pairs as Map?
			/*if (node.IsSequence())
			{
				for (const auto& el : node)
				{
					auto k = el.begin()->first.as<TKey>();
					auto v = el.begin()->second.as<TValue>();
					rhs.Add(std::move(k), std::move(v));
				}
				return true;
			}*/

			if (!node.IsMap())
				return false;

			for (const auto& el : node)
			{
				auto k = el.first.as<TKey>();
				auto v = el.second.as<TValue>();
				rhs.Add(std::move(k), std::move(v));
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

	template<typename T>
	struct convert
	{
		static Node encode(const T& rhs)
		{
			Node node;

			if constexpr (Sailor::IsBaseOf<Sailor::IYamlSerializable, T>)
			{
				node = rhs.Serialize();
			}
			else if constexpr (Sailor::IsEnum<T>)
			{
				node = SerializeEnum(rhs);
			}
			else
			{
				check(0);
			}

			return node;
		}

		static bool decode(const Node& node, T& rhs)
		{
			if constexpr (Sailor::IsBaseOf<Sailor::IYamlSerializable, T>)
			{
				rhs.Deserialize(node);
				return true;
			}
			else if constexpr (Sailor::IsEnum<T>)
			{
				DeserializeEnum(node, rhs);
				return true;
			}
			return false;
		}
	};
}
