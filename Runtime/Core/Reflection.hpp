#pragma once
#include "Core/Defines.h"
#include "refl-cpp/include/refl.hpp"
#include "Engine/Types.h"
#include "Memory/ObjectPtr.hpp"
#include "Components/Component.h"
#include "YamlSerializable.h"

namespace Sailor
{
	class TypeInfo
	{
	public:

		// Instances can be obtained only through calls to Get()
		template <typename T>
		static const TypeInfo& Get()
		{
			// here we create the singleton instance for this particular type
			static const TypeInfo ti(refl::reflect<T>());
			return ti;
		}

		const std::string& Name() const
		{
			return m_name;
		}

	private:

		std::string m_name;

		// Given a type_descriptor, we construct a TypeInfo
		// with all the metadata we care about
		template <typename T, typename... Fields>
		TypeInfo(refl::type_descriptor<T> td)
			: m_name(td.name)
		{
		}

	};

	class SAILOR_API IReflectable
	{
	public:
		virtual const TypeInfo& GetTypeInfo() const = 0;
	};

#define SAILOR_REFLECTABLE() \
    virtual const TypeInfo& GetTypeInfo() const override \
    { \
        return TypeInfo::Get<::refl::trait::remove_qualifiers_t<decltype(*this)>>(); \
    }

	class SAILOR_API Reflection
	{
	public:

		template<typename TComponent>
		static YAML::Node Serialize(TObjectPtr<TComponent> ptr) requires IsBaseOf<Component, TComponent>
		{
			YAML::Node res{};

			res["class"] = refl::reflect(*ptr).name.c_str();

			const TypeInfo& typeInfo = ptr->GetTypeInfo();

			for_each(refl::reflect(*ptr).members, [&](auto member)
				{
					if constexpr (is_readable(member))// && refl::descriptor::has_attribute<serializable>(member))
					{
						if constexpr (Sailor::IsEnum<decltype(member(*ptr))>)
						{
							//res[get_display_name(member)] = SerializeEnum(member(*ptr));
						}
						else
						{
							res[get_display_name(member)] = member(*ptr);
						}
					}
				});

			return res;
		}
	};
}
