#pragma once

#include "Components/Component.h"
#include "Workspace/WorkspaceTypeMetadata.h"

#include <new>
#include <string>
#include <type_traits>

namespace Sailor::Workspace
{
	namespace Internal
	{
		template<typename TType>
		void* SAILOR_WORKSPACE_CALL ConstructWorkspaceTypeV1(void* destination) noexcept
		{
			if (destination == nullptr)
			{
				return nullptr;
			}

			TType* object = new (destination) TType();
			Component* component = static_cast<Component*>(object);
			if (static_cast<void*>(component) != destination)
			{
				object->~TType();
				return nullptr;
			}

			return component;
		}

		template<typename TType>
		uint32_t CollectWorkspaceTypeV1(const WorkspaceHostApiV1& hostApi)
		{
			const TypeInfo& typeInfo = TypeInfo::Get<TType>();
			const std::string& typeName = typeInfo.Name();
			const std::string& baseTypeName = typeInfo.Base();
			const WorkspaceDefaultObjectSnapshot& defaultObject =
				GetWorkspaceDefaultObjectSnapshot<TType>();
			if (defaultObject.m_serializedDefaultValues.empty())
			{
				return static_cast<uint32_t>(EWorkspaceModuleResult::SerializationFailed);
			}
			const WorkspaceTypeDescriptorV1 descriptor
			{
				static_cast<uint32_t>(sizeof(WorkspaceTypeDescriptorV1)),
				typeName.data(),
				static_cast<uint64_t>(typeName.size()),
				baseTypeName.data(),
				static_cast<uint64_t>(baseTypeName.size()),
				&typeInfo,
				static_cast<uint64_t>(sizeof(TType)),
				static_cast<uint64_t>(alignof(TType)),
				defaultObject.m_serializedDefaultValues.data(),
				static_cast<uint64_t>(defaultObject.m_serializedDefaultValues.size()),
				GetWorkspaceTypeDescriptorFlags<TType>(),
				&ConstructWorkspaceTypeV1<TType>
			};

			return hostApi.collectType(hostApi.context, &descriptor);
		}

		template<typename TWorkspaceTypes>
		struct TWorkspaceTypeRegistration;

		template<typename... TTypes>
		struct TWorkspaceTypeRegistration<TWorkspaceTypeList<TTypes...>>
		{
			static uint32_t Register(const WorkspaceHostApiV1& hostApi)
			{
				static_assert((std::is_base_of_v<Component, TTypes> && ...),
					"Workspace registration types must derive from Sailor::Component");
				static_assert((std::is_default_constructible_v<TTypes> && ...),
					"Workspace registration types must be default constructible");

				uint32_t result = static_cast<uint32_t>(EWorkspaceModuleResult::Success);
				([&]()
					{
						if (result == static_cast<uint32_t>(EWorkspaceModuleResult::Success))
						{
							result = CollectWorkspaceTypeV1<TTypes>(hostApi);
						}
					}(), ...);

				return result;
			}
		};
	}

	template<typename TWorkspaceTypes>
	uint32_t RegisterWorkspaceTypesV1(const WorkspaceHostApiV1* hostApi) noexcept
	{
		if (hostApi == nullptr ||
			hostApi->structSize < sizeof(WorkspaceHostApiV1) ||
			hostApi->apiVersion != WorkspaceHostApiVersion ||
			hostApi->collectType == nullptr)
		{
			return static_cast<uint32_t>(EWorkspaceModuleResult::InvalidArgument);
		}

		return Internal::TWorkspaceTypeRegistration<TWorkspaceTypes>::Register(*hostApi);
	}
}
