#pragma once
#include "Containers/Concepts.h"
#include "Core/Defines.h"
#include "refl-cpp/include/refl.hpp"
#include "Engine/Types.h"
#include "Memory/ObjectPtr.hpp"
#include "YamlSerializable.h"
#include "AssetRegistry/AssetRegistry.h"

using refl::reflect;
using refl::descriptor::type_descriptor;

namespace Sailor
{
	template<typename T>
	struct IsObjectPtr_type : std::false_type {};

	template<typename R>
	struct IsObjectPtr_type<TObjectPtr<R>> : std::true_type { using base_type = R; };

	template<typename T>
	inline constexpr bool IsObjectPtr = IsObjectPtr_type<T>::value;

	template<typename T>
	class TObjectPtr;

	template<typename T>
	struct TemplateParameter;

	template<typename T>
	struct TemplateParameter<TObjectPtr<T>>
	{
		using type = T;
	};

	template<typename T>
	using TemplateParameter_t = typename TemplateParameter<T>::type;
}

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
	virtual ReflectedData GetReflectedData() const override \
	{ \
		TypeInfo typeInfo = TypeInfo::Get<::refl::trait::remove_qualifiers_t<decltype(*this)>>(); \
		ReflectedData res = Reflection::ReflectStatic<::refl::trait::remove_qualifiers_t<decltype(*this)>>(this); \
		return res; \
	} \
	virtual void ApplyReflection(const ReflectedData& reflection) override \
	{ \
		__CLASSNAME__::ApplyReflection_Impl<__CLASSNAME__>(this, reflection); \
	} \
	virtual bool ResolveRefs(const ReflectedData& reflection, const TMap<InstanceId, ObjectPtr>& resolveContext, bool bImmediate = true) override \
	{ \
		return __CLASSNAME__::ResolveRefs_Impl<__CLASSNAME__>(this, reflection, resolveContext, bImmediate); \
	} \
	protected: \
	class SAILOR_API RegistrationFactoryMethod \
	{ \
	public: \
		RegistrationFactoryMethod(const TypeInfo& type) \
		{ \
			if (!s_bRegistered) \
			{ \
				if constexpr (IsDefaultConstructible<__CLASSNAME__>) \
				{ \
					auto placementNew = [](void* ptr) mutable \
					{ \
						return new (ptr) __CLASSNAME__(); \
					}; \
					Reflection::RegisterFactoryMethod(type, placementNew); \
					Reflection::RegisterType(type.Name(), &type); \
					Reflection::RegisterCDO<__CLASSNAME__>(type); \
				} \
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
	namespace Attributes
	{
		struct Transient : refl::attr::usage::field, refl::attr::usage::function { };
		struct SkipCDO : refl::attr::usage::field, refl::attr::usage::function { };
	}

	class TypeInfo : public IYamlSerializable
	{
	public:

		virtual YAML::Node Serialize() const;
		virtual void Deserialize(const YAML::Node& inData);

		// instances can be obtained only through calls to Get()
		template <typename T>
		static const TypeInfo& Get()
		{
			static const TypeInfo ti(refl::reflect<T>());
			return ti;
		}

		const std::string& Name() const { return m_name; }
		const TMap<std::string, std::string>& Properties() const { return m_props; }

		size_t Size() const { return m_size; }
		size_t GetHash() const { std::hash<std::string> h; return h(m_name); }

		// TODO: Should we optimize that?
		bool operator==(const TypeInfo& rhs) const { return m_name == rhs.Name(); }

	private:

		std::string m_name;
		size_t m_size;
		TMap<std::string, std::string> m_props;

		// given a type_descriptor, we construct a TypeInfo
		// with all the metadata we care about (currently only name)
		template <typename T, typename... Fields>
		TypeInfo(refl::type_descriptor<T> td)
			: m_name(td.name)
		{
			m_size = sizeof(T);

			T* empty = nullptr;// reinterpret_cast<T*>(_malloca(m_size));

			for_each(td.members, [&](auto member)
				{
					if constexpr (is_writable(member) /* && is_readable(member)*/)
					{
						const std::string displayName = get_display_name(member);

						if constexpr (is_field(member) || refl::descriptor::is_function(member))
						{
							using PropertyType = ::refl::trait::remove_qualifiers_t<decltype(get_reader(member)(*empty))>;

							m_props[displayName] = typeid(PropertyType).name();
						}
					}
				});

			//_freea(empty);
		}

		friend class ReflectedData;
	};

	class ReflectedData final : public IYamlSerializable
	{
	public:

		virtual YAML::Node Serialize() const;
		virtual void Deserialize(const YAML::Node& inData);

		const TypeInfo& GetTypeInfo() const { return *m_typeInfo; }
		const TMap<std::string, YAML::Node>& GetProperties() const { return m_properties; }
		TMap<std::string, YAML::Node> GetOverrideProperties() const;

		bool operator==(const ReflectedData& rhs) const;

		ReflectedData DiffTo(const ReflectedData& rhs) const;

	private:

		// TODO: Rethink the approach with raw pointer
		const TypeInfo* m_typeInfo{};

		// We store all properties already serialized to YAML to simplify the coding
		// Ideally we need to introduce Proxies, that store the properties, without 
		// binding to specific serialization format
		TMap<std::string, YAML::Node> m_properties{};

		friend class Reflection;
	};

	class SAILOR_API IReflectable
	{
	public:

		virtual const TypeInfo& GetTypeInfo() const = 0;
		virtual ReflectedData GetReflectedData() const = 0;
		virtual void ApplyReflection(const ReflectedData& reflection) = 0;
		virtual bool ResolveRefs(const ReflectedData& reflection, const TMap<InstanceId, ObjectPtr>& resolveContext, bool bImmediate = true) { return true; }

	protected:

		static ObjectPtr ResolveExternalDependency(const InstanceId& componentInstanceId, const TMap<InstanceId, ObjectPtr>& resolveContext);

		template<typename TPropertyType>
		__forceinline static TPropertyType ResolveObject(const YAML::Node& node, const TMap<InstanceId, ObjectPtr>& resolveContext, bool bImmediate)
		{
			using ElementType = TemplateParameter_t<TPropertyType>;

			TPropertyType v{};
			if (node["fileId"])
			{
				if (FileId fileId = node["fileId"].as<FileId>())
				{
					v = App::GetSubmodule<AssetRegistry>()->LoadAssetFromFile<ElementType>(fileId, bImmediate);
				}
			}

			if (node["instanceId"])
			{
				InstanceId instanceId = node["instanceId"].as<InstanceId>();

				if (instanceId)
				{
					// We expect only one of instanceId and fileId is filled
					check(!v);

					// Resolve internal dependencies
					if (resolveContext.ContainsKey(instanceId))
					{
						if (auto objPtr = resolveContext[instanceId])
						{
							v = objPtr.DynamicCast<ElementType>();
						}
					}
					// Resolve external dependencies
					else if (auto objPtr = ResolveExternalDependency(instanceId, resolveContext))
					{
						v = objPtr.DynamicCast<ElementType>();
					}
				}
			}

			return v;
		}

		template<typename T>
		static bool ResolveRefs_Impl(T* ptr, const ReflectedData& reflection, const TMap<InstanceId, ObjectPtr>& resolveContext, bool bImmediate = true)
		{
			bool bResolved = true;
			for_each(refl::reflect<T>().members, [&](auto member)
				{
					if constexpr (is_writable(member))
					{
						const std::string displayName = get_display_name(member);
						if (reflection.GetProperties().ContainsKey(displayName))
						{
							const YAML::Node& node = reflection.GetProperties()[displayName];
							if (node.IsDefined() && !node.IsNull())
							{
								if constexpr (is_field(member))
								{
									using PropertyType = ::refl::trait::remove_qualifiers_t<decltype(member(*ptr))>;
									if constexpr (Sailor::IsObjectPtr<PropertyType>)
									{
										if (!member(*ptr))
										{
											auto resolved = ResolveObject<PropertyType>(node, resolveContext, bImmediate);
											bResolved &= (bool)resolved;

											member(*ptr) = resolved;
										}
									}
								}
								else if constexpr (refl::descriptor::is_function(member))
								{
									using PropertyType = ::refl::trait::remove_qualifiers_t<decltype(get_reader(member)(*ptr))>;
									if constexpr (Sailor::IsObjectPtr<PropertyType>)
									{
										if (!get_reader(member)(*ptr))
										{
											auto resolved = ResolveObject<PropertyType>(node, resolveContext, bImmediate);
											bResolved &= (bool)resolved;

											member(*ptr, resolved);
										}
									}
								}
							}
						}
					}
				});

			return bResolved;
		}

		template<typename T>
		static void ApplyReflection_Impl(T* ptr, const ReflectedData& reflection)
		{
			for_each(refl::reflect<T>().members, [&](auto member)
				{
					if constexpr (is_writable(member))
					{
						const std::string displayName = get_display_name(member);
						if (reflection.GetProperties().ContainsKey(displayName))
						{
							const YAML::Node& node = reflection.GetProperties()[displayName];
							if (node.IsDefined())
							{
								if constexpr (is_field(member))
								{
									using PropertyType = ::refl::trait::remove_qualifiers_t<decltype(member(*ptr))>;
									if constexpr (!Sailor::IsObjectPtr<PropertyType>)
									{
										/* ObjectPtrs are resolved in ResolveObjects function */
										member(*ptr) = node.as<PropertyType>();
									}
								}
								else if constexpr (refl::descriptor::is_function(member))
								{
									using PropertyType = ::refl::trait::remove_qualifiers_t<decltype(get_reader(member)(*ptr))>;
									if constexpr (!Sailor::IsObjectPtr<PropertyType>)
									{
										/* ObjectPtrs are resolved in ResolveObjects function */
										member(*ptr, node.as<PropertyType>());
									}
								}
							}
						}
					}
				});
		}
	};

	namespace Internal
	{
		extern TUniquePtr<TConcurrentMap<std::string, ReflectedData, 32u, ERehashPolicy::Never>> g_pCdos;
		extern TUniquePtr<TConcurrentMap<std::string, std::function<IReflectable* (void*)>>> g_pPlacementFactoryMethods;
		extern TUniquePtr<TConcurrentMap<std::string, const TypeInfo*>> g_pReflectionTypes;
		extern Memory::ObjectAllocatorPtr g_cdoAllocator;
	}

	class SAILOR_API Reflection
	{
	public:

		using TPlacementFactoryMethod = std::function<IReflectable* (void*)>;

		static YAML::Node ExportEngineTypes();

		static void RegisterFactoryMethod(const TypeInfo& type, TPlacementFactoryMethod placementNew);
		static void RegisterType(const std::string& typeName, const TypeInfo* pType);

		template<typename T>
		static void RegisterCDO(const TypeInfo& pType)
		{
			std::string typeName = pType.Name();

			auto cdo = CreateCDO(pType);

			auto& cdoInfo = Internal::g_pCdos->At_Lock(typeName);
			cdoInfo = Reflection::ReflectCDO<T>(cdo.DynamicCast<T>().GetRawPtr());
			Internal::g_pCdos->Unlock(typeName);

			// We don't store CDOs
			cdo.ForcelyDestroyObject();
		}

		static const TypeInfo& GetTypeByName(const std::string& typeName);

		template<typename T = Object>
		static const ReflectedData& GetCDO(TObjectPtr<T> objPtr) requires IsBaseOf<IReflectable, T>&& IsBaseOf<Object, T> { return GetCDO(objPtr->GetTypeInfo().Name()); }

		static const ReflectedData& GetCDO(const std::string& typeName) { return (*Internal::g_pCdos)[typeName]; }

		template<typename T = Object>
		static TObjectPtr<T> CreateObject(const TypeInfo& type, Memory::ObjectAllocatorPtr pAllocator) requires IsBaseOf<IReflectable, T>&& IsBaseOf<Object, T>
		{
			check(Internal::g_pPlacementFactoryMethods->ContainsKey(type.Name()));

			auto ptr = pAllocator->Allocate(type.Size());
			(*Internal::g_pPlacementFactoryMethods)[type.Name()](ptr);
			TObjectPtr<T> pRes(reinterpret_cast<T*>(ptr), pAllocator);
			return pRes;
		}

		template<typename T>
		static ReflectedData ReflectStatic(const T* ptr) requires IsBaseOf<IReflectable, T>
		{
			ReflectedData reflection;
			reflection.m_typeInfo = &ptr->GetTypeInfo();

			for_each(refl::reflect(*ptr).members, [&](auto member)
				{
					if constexpr (is_readable(member) && !refl::descriptor::has_attribute<Attributes::Transient>(member))
					{
						//using PropertyType = decltype(member(*ptr));
						const std::string displayName = get_display_name(member);
						reflection.m_properties[displayName] = member(*ptr);
					}
				});

			return reflection;
		}

		static bool ApplyReflection(IReflectable* ptr, const ReflectedData& reflection)
		{
			if (ptr->GetTypeInfo() != reflection.GetTypeInfo())
			{
				return false;
			}

			ptr->ApplyReflection(reflection);
			return true;
		}

	private:

		template<typename TEnum>
		static YAML::Node ReflectEnumValues()
		{
			YAML::Node res;
			TVector<std::string> values;
			constexpr auto enumValues = magic_enum::enum_names<TEnum>();

			for (const auto& value : enumValues)
			{
				values.Add(std::string(value));
			}

			std::string enumName = typeid(TEnum).name();
			res[enumName] = values;

			return res;
		}

		static ComponentPtr CreateCDO(const TypeInfo& pType);

		template<typename T>
		static ReflectedData ReflectCDO(const T* ptr) requires IsBaseOf<IReflectable, T>
		{
			ReflectedData reflection;
			reflection.m_typeInfo = &ptr->GetTypeInfo();

			for_each(refl::reflect(*ptr).members, [&](auto member)
				{
					if constexpr (is_readable(member) &&
						!refl::descriptor::has_attribute<Attributes::Transient>(member) &&
						!refl::descriptor::has_attribute<Attributes::SkipCDO>(member))
					{
						//using PropertyType = decltype(member(*ptr));
						const std::string displayName = get_display_name(member);
						reflection.m_properties[displayName] = member(*ptr);
					}
				});

			return reflection;
		}
	};
}

REFL_AUTO(
	type(Sailor::IReflectable)
)