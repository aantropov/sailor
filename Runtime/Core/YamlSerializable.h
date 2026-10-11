#pragma once
#include "Core/Defines.h"
#include "Core/FileRevision.h"
#include "Core/YamlUtils.h"
#include "Containers/Concepts.h"
#include "Math/Math.h"
#include "Containers/Containers.h"

#include <utility>
#include <string_view>
#include <refl.hpp>
#include <yaml-cpp/yaml.h>

#define SERIALIZE_PROPERTY(yamlNode, variable) Sailor::Serialize(yamlNode, &(#variable)[2], variable)
#define DESERIALIZE_PROPERTY(yamlNode, variable) Sailor::Deserialize(yamlNode, &(#variable)[2], variable)

namespace Sailor
{
	namespace Attributes
	{
		struct YamlName : refl::attr::usage::field
		{
			constexpr explicit YamlName(const char* name) : m_name(name) {}
			const char* m_name;
		};

		struct YamlOptional : refl::attr::usage::field {};
	}

	class SAILOR_SHARED_API IYamlSerializable
	{
	public:

		virtual YAML::Node Serialize() const { assert(0); return YAML::Node(); };
		virtual void Deserialize(const YAML::Node& inData) { assert(0); }
	};

	inline void Serialize(
		YAML::Node& node,
		std::string_view name,
		const FileRevision& revision)
	{
		node[name] = revision.Serialize();
	}

	inline bool Deserialize(
		const YAML::Node& node,
		std::string_view name,
		FileRevision& revision)
	{
		if (!node[name])
		{
			return false;
		}

		revision.Deserialize(node[name]);
		return revision.m_bIsValid;
	}

	// Cannot move enum to internal YAML convertions, since there is no way to use extra template arg
	template<typename T>
	YAML::Node SerializeEnum(typename std::enable_if< std::is_enum<T>::value, T >::type enumeration)
	{
		YAML::Node j;
		j = magic_enum::enum_name(enumeration);
		return j;
	}

	template<typename T>
	bool DeserializeEnum(const YAML::Node& j, typename std::enable_if< std::is_enum<T>::value, T >::type& outEnumeration)
	{
		auto value = magic_enum::enum_cast<T>(j.as<std::string_view>());
		if (!value)
		{
			return false;
		}
		outEnumeration = *value;
		return true;
	}

	template<typename T>
	__forceinline void Serialize(YAML::Node& node, std::string_view name, const T& variable)
	{
		if constexpr (IsEnum<T>)
		{
			node[name] = SerializeEnum<T>(variable);
		}
		else
		{
			node[name] = variable;
		}
	}

	template<typename T>
	__forceinline bool Deserialize(const YAML::Node& node, std::string_view name, T& variable)
	{
		const YAML::Node value = node[name];
		if (value)
		{
			if constexpr (IsEnum<T>)
			{
				return DeserializeEnum<T>(value, variable);
			}
			else
			{
				using TValue = typename std::decay<decltype(variable)>::type;
				TValue decoded{};
				if (!YAML::convert<TValue>::decode(value, decoded))
				{
					return false;
				}
				variable = std::move(decoded);
			}

			return true;
		}

		return false;
	}

	template<typename TMember>
	constexpr std::string_view GetYamlFieldName(TMember member)
	{
		if constexpr (refl::descriptor::has_attribute<Attributes::YamlName>(member))
		{
			return refl::descriptor::get_attribute<Attributes::YamlName>(member).m_name;
		}
		const std::string_view name = member.name.c_str();
		return name.starts_with("m_") ? name.substr(2) : name;
	}

	template<typename T>
	YAML::Node SerializeReflected(const T& value)
	{
		YAML::Node node(YAML::NodeType::Map);
		refl::util::for_each(refl::reflect<T>().members, [&](auto member)
			{
				if constexpr (refl::descriptor::is_field(member))
				{
					Sailor::Serialize(node, GetYamlFieldName(member), member(value));
				}
			});
		return node;
	}

	template<typename T>
	void DeserializeReflected(const YAML::Node& node, T& value)
	{
		const auto map = Utils::ValidateYamlMap(node);
		if (!map.IsValid())
		{
			throw YAML::RepresentationException(node.Mark(), "expected a map with unique field names: " + map.m_fieldName);
		}
		refl::util::for_each(refl::reflect<T>().members, [&](auto member)
			{
				if constexpr (refl::descriptor::is_field(member))
				{
					const std::string_view name = GetYamlFieldName(member);
					if constexpr (refl::descriptor::has_attribute<Attributes::YamlOptional>(member))
					{
						if (!node[name]) return;
					}
					try
					{
						if (!Sailor::Deserialize(node, name, member(value)))
						{
							throw YAML::BadConversion(node.Mark());
						}
					}
					catch (const YAML::Exception& error)
					{
						using TValue = std::remove_cvref_t<decltype(member(value))>;
						const char* separator = refl::trait::is_reflectable_v<TValue> ? "." : ": ";
						throw YAML::RepresentationException(error.mark, std::string(name) + separator + error.msg);
					}
				}
			});
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

	template<typename TKey, typename TValue>
	struct convert<Sailor::TConcurrentMap<TKey, TValue>>
	{
		static Node encode(const Sailor::TConcurrentMap<TKey, TValue>& rhs)
		{
			Node node;

			for (const auto& el : rhs)
			{
				node[el.m_first] = el.m_second;
			}

			return node;
		}

		static bool decode(const Node& node, Sailor::TConcurrentMap<TKey, TValue>& rhs)
		{
			rhs.Clear();

			if (node.size() == 0)
			{
				return true;
			}

			if (!node.IsMap())
				return false;

			for (const auto& el : node)
			{
				auto k = el.first.as<TKey>();
				auto v = el.second.as<TValue>();
				rhs.Insert(std::move(k), std::move(v));
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
			rhs.w = node[3].as<float>();
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
			rhs.w = node[3].as<float>();
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
				node = Sailor::SerializeEnum<T>(rhs);
			}
			else if constexpr (refl::trait::is_reflectable_v<T>)
			{
				node = Sailor::SerializeReflected(rhs);
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
				return Sailor::DeserializeEnum<T>(node, rhs);
			}
			else if constexpr (refl::trait::is_reflectable_v<T>)
			{
				Sailor::DeserializeReflected(node, rhs);
				return true;
			}
			return false;
		}
	};
}
