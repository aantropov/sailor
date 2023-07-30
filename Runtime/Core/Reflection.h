#pragma once
#include "Core/Defines.h"
#include "refl-cpp/include/refl.hpp"
#include "Engine/Types.h"
#include "Memory/ObjectPtr.hpp"
#include "YamlSerializable.h"

#define SAILOR_REFLECTABLE(__CLASSNAME__) \
	public: \
	static const TypeInfo& GetStaticTypeInfo() \
	{ \
		return TypeInfo::Get<__CLASSNAME__>(); \
	} \
	virtual const TypeInfo& GetTypeInfo() const override \
	{ \
		return TypeInfo::Get<::refl::trait::remove_qualifiers_t<decltype(*this)>>(); \
	} \
	virtual ReflectionInfo GetReflectionInfo() const override \
	{ \
		TypeInfo typeInfo = TypeInfo::Get<::refl::trait::remove_qualifiers_t<decltype(*this)>>(); \
		ReflectionInfo res = Reflection::ReflectStatic<::refl::trait::remove_qualifiers_t<decltype(*this)>>(this); \
		return res; \
	} \
	virtual void ApplyReflection(const ReflectionInfo& reflection) override \
	{ \
		__CLASSNAME__::ApplyReflection_Impl<__CLASSNAME__>(this, reflection); \
	} \
	protected: \
	template<typename T> \
	static void ApplyReflection_Impl(T* ptr, const ReflectionInfo& reflection) \
	{ \
		for_each(refl::reflect<T>().members, [&](auto member) \
		{ \
			if constexpr (is_writable(member)) \
			{ \
				const std::string prop = get_display_name(member); \
				size_t index = reflection.GetMembers().FindIf([&](const auto& lhs) { return lhs[prop].IsDefined(); }); \
				if (index != -1) \
				{ \
					const YAML::Node& node = reflection.GetMembers()[index][prop]; \
					if constexpr (is_field(member)) \
					{ \
						using FieldType = ::refl::trait::remove_qualifiers_t<decltype(member(*ptr))>; \
						if constexpr (Sailor::IsEnum<FieldType>) \
						{ \
							DeserializeEnum<FieldType>(node, member(*ptr)); \
						} \
						else \
						{ \
							member(*ptr) = node.as<FieldType>(); \
						} \
					} \
					else if constexpr (refl::descriptor::is_function(member)) \
					{ \
						using FieldType = ::refl::trait::remove_qualifiers_t<decltype(get_reader(member)(*ptr))>; \
						FieldType v{}; \
						if constexpr (Sailor::IsEnum<FieldType>) \
						{ \
							DeserializeEnum<FieldType>(node, v); \
						} \
						else \
						{ \
							v = node.as<FieldType>(); \
						} \
						member(*ptr, v); \
					} \
				} \
			} \
		}); \
	} \
	class SAILOR_API RegistrationFactoryMethod \
	{ \
	public: \
		RegistrationFactoryMethod(const TypeInfo& type) \
		{ \
			if (!s_bRegistered) \
			{ \
				auto placementNew = [](void* ptr) mutable \
				{ \
					return new (ptr) __CLASSNAME__(); \
				}; \
				Reflection::RegisterFactoryMethod(type, placementNew); \
				s_bRegistered = true; \
			} \
		} \
	protected: \
		static inline bool s_bRegistered = false; \
	}; \
	static inline volatile RegistrationFactoryMethod s_registrationFactoryMethod{ GetStaticTypeInfo() }; \
	private: \

namespace Sailor
{
	class TypeInfo
	{
	public:

		// instances can be obtained only through calls to Get()
		template <typename T>
		static const TypeInfo& Get()
		{
			static const TypeInfo ti(refl::reflect<T>());
			return ti;
		}

		const std::string& Name() const
		{
			return m_name;
		}

		size_t Size() const
		{
			return m_size;
		}

		size_t GetHash() const
		{
			std::hash<std::string> h;
			return h(m_name);
		}

		bool operator==(const TypeInfo& rhs) const
		{
			// TODO: Should we optimize that?
			return m_name == rhs.Name();
		}

	private:

		std::string m_name;
		size_t m_size;

		// given a type_descriptor, we construct a TypeInfo
		// with all the metadata we care about (currently only name)
		template <typename T, typename... Fields>
		TypeInfo(refl::type_descriptor<T> td)
			: m_name(td.name)
		{
			m_size = sizeof(T);
		}
	};

	class ReflectionInfo : public IYamlSerializable
	{
	public:

		virtual YAML::Node Serialize() const
		{
			assert(m_typeInfo);

			YAML::Node res{};
			res["typename"] = m_typeInfo->Name();
			res["members"] = m_members;

			return res;
		};

		virtual void Deserialize(const YAML::Node& inData) { assert(0); }

		const TypeInfo& GetTypeInfo() const { return *m_typeInfo; }
		const TVector<YAML::Node>& GetMembers() const { return m_members; }

	private:

		// TODO: Rethink the approach with raw pointer
		const TypeInfo* m_typeInfo{};
		TVector<YAML::Node> m_members{};

		friend class Reflection;
	};

	class SAILOR_API IReflectable
	{
	public:
		virtual const TypeInfo& GetTypeInfo() const = 0;
		virtual ReflectionInfo GetReflectionInfo() const = 0;
		virtual void ApplyReflection(const ReflectionInfo& reflection) = 0;
	};

	namespace Internal
	{
		extern TUniquePtr<TConcurrentMap<std::string, std::function<IReflectable* (void*)>>> g_pPlacementFactoryMethods;
	}

	class SAILOR_API Reflection
	{
	public:
		using TPlacementFactoryMethod = std::function<IReflectable* (void*)>;

		static void RegisterFactoryMethod(const TypeInfo& type, TPlacementFactoryMethod placementNew);

		template<typename T = Object>
		static TObjectPtr<T> CreateObject(const TypeInfo& type, Memory::ObjectAllocatorPtr pAllocator) requires IsBaseOf<IReflectable, T>&& IsBaseOf<Object, T>
		{
			check(Internal::g_pPlacementFactoryMethods->ContainsKey(type.Name()));

			auto ptr = pAllocator->Allocate(type.Size());
			IReflectable* pRawPtr = (*Internal::g_pPlacementFactoryMethods)[type.Name()](ptr);
			TObjectPtr<T> pRes(reinterpret_cast<T*>(ptr), pAllocator);
			return pRes;
		}

		template<typename T>
		static ReflectionInfo ReflectStatic(const T* ptr) requires IsBaseOf<IReflectable, T>
		{
			ReflectionInfo reflection;
			reflection.m_typeInfo = &ptr->GetTypeInfo();

			for_each(refl::reflect(*ptr).members, [&](auto member)
				{
					if constexpr (is_readable(member))// && refl::descriptor::has_attribute<serializable>(member))
					{
						YAML::Node node{};

						if constexpr (Sailor::IsEnum<decltype(member(*ptr))>)
						{
							node[get_display_name(member)] = SerializeEnum<decltype(member(*ptr))>(member(*ptr));
						}
						else
						{
							node[get_display_name(member)] = member(*ptr);
						}
						reflection.m_members.Add(node);
					}
				});

			return reflection;
		}

		static bool ApplyReflection(IReflectable* ptr, const ReflectionInfo& reflection)
		{
			if (ptr->GetTypeInfo() != reflection.GetTypeInfo())
			{
				return false;
			}

			ptr->ApplyReflection(reflection);
			return true;
		}

	private:
	};
}

REFL_AUTO(
	type(Sailor::IReflectable)
)