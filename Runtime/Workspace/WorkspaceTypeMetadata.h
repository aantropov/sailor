#pragma once

#include "Core/Reflection.h"
#include "Workspace/WorkspaceModuleApi.h"

#include <cstring>
#include <ctime>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>

namespace Sailor::Workspace
{
	template<typename... TTypes>
	struct TWorkspaceTypeList
	{
	};

	namespace Internal
	{
		template<typename TType>
		YAML::Node SerializeWorkspaceDefaultObject()
		{
			static_assert(IsBaseOf<IReflectable, TType>, "Workspace metadata types must implement IReflectable");
			static_assert(std::is_default_constructible_v<TType>, "Workspace metadata types must be default constructible");

			TType defaultObject{};
			YAML::Node defaultValues;
			for_each(refl::reflect(defaultObject).members, [&](auto member)
				{
					if constexpr (is_readable(member) &&
						!refl::descriptor::has_attribute<Attributes::Transient>(member) &&
						!refl::descriptor::has_attribute<Attributes::SkipCDO>(member))
					{
						defaultValues[get_display_name(member)] = member(defaultObject);
					}
				});

			YAML::Node node;
			node["typename"] = TypeInfo::Get<TType>().Name();
			node["defaultValues"] = defaultValues;
			return node;
		}

		template<typename TWorkspaceTypes>
		struct TWorkspaceTypeMetadataSerializer;

		template<typename... TTypes>
		struct TWorkspaceTypeMetadataSerializer<TWorkspaceTypeList<TTypes...>>
		{
			static std::string Serialize(std::string_view moduleName)
			{
				if (moduleName.empty())
				{
					throw std::invalid_argument("Workspace module name must not be empty");
				}

				YAML::Node types(YAML::NodeType::Sequence);
				(types.push_back(TypeInfo::Get<TTypes>().Serialize()), ...);

				YAML::Node defaultObjects(YAML::NodeType::Sequence);
				(defaultObjects.push_back(SerializeWorkspaceDefaultObject<TTypes>()), ...);

				YAML::Node metadata;
				metadata["metadataVersion"] = WorkspaceTypeMetadataVersion;
				metadata["moduleName"] = std::string(moduleName);
				metadata["timeStamp"] = std::time(nullptr);
				metadata["engineTypes"] = types;
				metadata["cdos"] = defaultObjects;
				metadata["enums"] = YAML::Node(YAML::NodeType::Sequence);
				metadata["assetTypes"] = YAML::Node(YAML::NodeType::Sequence);

				YAML::Emitter emitter;
				emitter << metadata;
				if (!emitter.good())
				{
					throw std::runtime_error(emitter.GetLastError());
				}

				return emitter.c_str();
			}
		};
	}

	template<typename TWorkspaceTypes>
	uint32_t ExportWorkspaceTypeMetadataV1(
		std::string_view moduleName,
		char* destination,
		uint64_t destinationCapacity,
		uint64_t* outPayloadSize) noexcept
	{
		if (outPayloadSize == nullptr)
		{
			return static_cast<uint32_t>(EWorkspaceModuleResult::InvalidArgument);
		}

		*outPayloadSize = 0;

		try
		{
			static const std::string payload = Internal::TWorkspaceTypeMetadataSerializer<TWorkspaceTypes>::Serialize(moduleName);
			if (payload.size() > std::numeric_limits<uint64_t>::max())
			{
				return static_cast<uint32_t>(EWorkspaceModuleResult::SerializationFailed);
			}

			const uint64_t payloadSize = static_cast<uint64_t>(payload.size());
			*outPayloadSize = payloadSize;

			if (destination == nullptr)
			{
				return static_cast<uint32_t>(destinationCapacity == 0
					? EWorkspaceModuleResult::BufferTooSmall
					: EWorkspaceModuleResult::InvalidArgument);
			}

			if (destinationCapacity < payloadSize)
			{
				return static_cast<uint32_t>(EWorkspaceModuleResult::BufferTooSmall);
			}

			std::memcpy(destination, payload.data(), payload.size());
			return static_cast<uint32_t>(EWorkspaceModuleResult::Success);
		}
		catch (...)
		{
			*outPayloadSize = 0;
			return static_cast<uint32_t>(EWorkspaceModuleResult::SerializationFailed);
		}
	}
}
