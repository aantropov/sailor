#pragma once
#include "Containers/Concepts.h"
#include "Containers/Containers.h"
#include "AssetRegistry/FileId.h"
#include "Core/Defines.h"
#include "Core/YamlSerializable.h"
#include "Engine/InstanceId.h"
#include "Engine/Types.h"
#include "Memory/ObjectPtr.hpp"
#include <refl.hpp>

namespace Sailor
{
	class TypeInfo;
	class ReflectedData;

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

	class SAILOR_API IReflectable
	{
	public:

		virtual const TypeInfo& GetTypeInfo() const = 0;
		virtual ReflectedData GetReflectedData() const = 0;
		virtual void ApplyReflection(const ReflectedData& reflection) = 0;
		virtual bool ResolveRefs(const ReflectedData& reflection, const TMap<InstanceId, ObjectPtr>& resolveContext, bool bImmediate = true) { return true; }

	protected:

		static ObjectPtr ResolveAssetDependency(const FileId& fileId, bool bImmediate);
		static ObjectPtr ResolveExternalDependency(const InstanceId& componentInstanceId, const TMap<InstanceId, ObjectPtr>& resolveContext);

		template<typename TPropertyType>
		__forceinline static TPropertyType ResolveObject(const YAML::Node& node, const TMap<InstanceId, ObjectPtr>& resolveContext, bool bImmediate);

		template<typename T>
		static bool ResolveRefs_Impl(T* ptr, const ReflectedData& reflection, const TMap<InstanceId, ObjectPtr>& resolveContext, bool bImmediate = true);

		template<typename T>
		static void ApplyReflection_Impl(T* ptr, const ReflectedData& reflection);
	};
}
