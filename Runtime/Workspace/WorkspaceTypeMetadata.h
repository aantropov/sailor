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
#include <unordered_map>
#include <utility>

namespace Sailor::Workspace
{
	template<typename... TTypes>
	struct TWorkspaceTypeList
	{
	};

	namespace Internal
	{
		struct WorkspaceDefaultObjectSnapshot
		{
			YAML::Node m_metadata;
			std::string m_serializedDefaultValues;
		};

		template<typename TType>
		const WorkspaceDefaultObjectSnapshot& GetWorkspaceDefaultObjectSnapshot()
		{
			static_assert(IsBaseOf<IReflectable, TType>, "Workspace metadata types must implement IReflectable");
			static_assert(std::is_default_constructible_v<TType>, "Workspace metadata types must be default constructible");

			static const WorkspaceDefaultObjectSnapshot snapshot = []()
				{
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

					YAML::Node metadata;
					metadata["typename"] = TypeInfo::Get<TType>().Name();
					metadata["defaultValues"] = defaultValues;

					YAML::Emitter emitter;
					emitter << defaultValues;
					if (!emitter.good())
					{
						throw std::runtime_error(emitter.GetLastError());
					}

					return WorkspaceDefaultObjectSnapshot
					{
						std::move(metadata),
						std::string(emitter.c_str())
					};
				}();

			return snapshot;
		}

		struct WorkspacePropertyDeclaration
		{
			std::string m_declarator;
			std::string m_typeName;
			bool m_bReadable = false;
			bool m_bWritable = false;
		};

		template<typename TType>
		uint32_t GetWorkspaceTypeDescriptorFlags()
		{
			std::unordered_map<std::string, WorkspacePropertyDeclaration> declarations;
			uint32_t flags = 0;
			TType* empty = nullptr;

			for_each(refl::reflect<TType>().members, [&](auto member)
				{
					if constexpr (is_field(member) || refl::descriptor::is_function(member))
					{
						constexpr bool bWritable = is_writable(member);
						constexpr bool bReadable = is_readable(member) &&
							!refl::descriptor::has_attribute<Attributes::Transient>(member) &&
							!refl::descriptor::has_attribute<Attributes::SkipCDO>(member);
						if constexpr (bWritable || bReadable)
						{
							auto recordDeclaration = [&](auto propertyType)
							{
								using PropertyType = typename decltype(propertyType)::type;
								const std::string displayName = get_display_name(member);
								const std::string declarator =
									refl::descriptor::get_declarator(member).name.c_str();
								const std::string typeName =
									TypeInfo::GetReflectedPropertyTypeName<PropertyType>();
								WorkspacePropertyDeclaration declaration
								{
									declarator,
									typeName,
									bReadable,
									bWritable
								};

								const auto [existing, bInserted] = declarations.try_emplace(
									displayName,
									std::move(declaration));
								if (!bInserted)
								{
									if (existing->second.m_declarator != declarator ||
										existing->second.m_typeName != typeName ||
										(existing->second.m_bReadable && bReadable) ||
										(existing->second.m_bWritable && bWritable))
									{
										flags |= WorkspaceTypeDescriptorFlagAmbiguousProperties;
									}

									existing->second.m_bReadable |= bReadable;
									existing->second.m_bWritable |= bWritable;
								}
							};

							if constexpr (bReadable)
							{
								using PropertyType = ::refl::trait::remove_qualifiers_t<decltype(member(*empty))>;
								recordDeclaration(std::type_identity<PropertyType>{});
							}
							else
							{
								using PropertyType = ::refl::trait::remove_qualifiers_t<decltype(get_reader(member)(*empty))>;
								recordDeclaration(std::type_identity<PropertyType>{});
							}
						}
					}
				});

			return flags;
		}

		template<typename TType>
		YAML::Node SerializeWorkspaceDefaultObject()
		{
			return GetWorkspaceDefaultObjectSnapshot<TType>().m_metadata;
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
