#include "Components/Component.h"
#include "Workspace/WorkspaceTypeRegistration.h"

#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>

#ifndef SAILOR_TEST_METADATA_MODE
#error SAILOR_TEST_METADATA_MODE must select a malformed metadata fixture
#endif

#if SAILOR_TEST_METADATA_MODE == 1
#define SAILOR_TEST_FIXTURE_NAMESPACE PropertyMismatchWorkspaceFixture
#elif SAILOR_TEST_METADATA_MODE == 2
#define SAILOR_TEST_FIXTURE_NAMESPACE DuplicateCdoWorkspaceFixture
#elif SAILOR_TEST_METADATA_MODE == 3
#define SAILOR_TEST_FIXTURE_NAMESPACE UnknownDefaultWorkspaceFixture
#elif SAILOR_TEST_METADATA_MODE == 4
#define SAILOR_TEST_FIXTURE_NAMESPACE InvalidDefaultTypeWorkspaceFixture
#elif SAILOR_TEST_METADATA_MODE == 5
#define SAILOR_TEST_FIXTURE_NAMESPACE MissingDefaultWorkspaceFixture
#elif SAILOR_TEST_METADATA_MODE == 6
#define SAILOR_TEST_FIXTURE_NAMESPACE InvalidEnumDefaultWorkspaceFixture
#elif SAILOR_TEST_METADATA_MODE == 7
#define SAILOR_TEST_FIXTURE_NAMESPACE InvalidStructuredDefaultWorkspaceFixture
#elif SAILOR_TEST_METADATA_MODE == 8
#define SAILOR_TEST_FIXTURE_NAMESPACE MissingCdoWorkspaceFixture
#elif SAILOR_TEST_METADATA_MODE == 9
#define SAILOR_TEST_FIXTURE_NAMESPACE OversizedStructuredDefaultWorkspaceFixture
#elif SAILOR_TEST_METADATA_MODE == 10
#define SAILOR_TEST_FIXTURE_NAMESPACE ShadowedPropertyWorkspaceFixture
#elif SAILOR_TEST_METADATA_MODE == 11
#define SAILOR_TEST_FIXTURE_NAMESPACE MissingEmptyPropertySchemaWorkspaceFixture
#elif SAILOR_TEST_METADATA_MODE == 12
#define SAILOR_TEST_FIXTURE_NAMESPACE AliasExpansionWorkspaceFixture
#else
#error Unsupported SAILOR_TEST_METADATA_MODE
#endif

#if SAILOR_TEST_METADATA_MODE == 10
namespace SAILOR_TEST_FIXTURE_NAMESPACE
{
	class BaseComponent : public Sailor::Component
	{
		SAILOR_WORKSPACE_REFLECTABLE(BaseComponent)

	public:
		float GetShadowedValue() const { return m_shadowedValue; }
		void SetShadowedValue(float value) { m_shadowedValue = value; }

	private:
		float m_shadowedValue = 1.0f;
	};

	class FixtureComponent final : public BaseComponent
	{
		SAILOR_WORKSPACE_REFLECTABLE(FixtureComponent)

	public:
		std::string GetShadowedValue() const { return m_shadowedValue; }
		void SetShadowedValue(const std::string& value) { m_shadowedValue = value; }

	private:
		std::string m_shadowedValue = "derived";
	};

	using WorkspaceTypes = Sailor::Workspace::TWorkspaceTypeList<BaseComponent, FixtureComponent>;
}

REFL_AUTO(
	type(SAILOR_TEST_FIXTURE_NAMESPACE::BaseComponent, bases<Sailor::Component>),
	func(GetShadowedValue, property("shadowedValue")),
	func(SetShadowedValue, property("shadowedValue"))
)

REFL_AUTO(
	type(SAILOR_TEST_FIXTURE_NAMESPACE::FixtureComponent,
		bases<SAILOR_TEST_FIXTURE_NAMESPACE::BaseComponent>),
	func(GetShadowedValue, property("shadowedValue")),
	func(SetShadowedValue, property("shadowedValue"))
)
#elif SAILOR_TEST_METADATA_MODE == 11
namespace SAILOR_TEST_FIXTURE_NAMESPACE
{
	class FixtureComponent final : public Sailor::Component
	{
		SAILOR_WORKSPACE_REFLECTABLE(FixtureComponent)
	};

	using WorkspaceTypes = Sailor::Workspace::TWorkspaceTypeList<FixtureComponent>;
}

REFL_AUTO(
	type(SAILOR_TEST_FIXTURE_NAMESPACE::FixtureComponent, bases<Sailor::Component>)
)
#else
namespace SAILOR_TEST_FIXTURE_NAMESPACE
{
	enum class EFixtureMode
	{
		Default,
		Alternate
	};

	class FixtureComponent final : public Sailor::Component
	{
		SAILOR_WORKSPACE_REFLECTABLE(FixtureComponent)

	public:
		float GetMoveSpeed() const { return m_moveSpeed; }
		void SetMoveSpeed(float moveSpeed) { m_moveSpeed = moveSpeed; }
		EFixtureMode GetMode() const { return m_mode; }
		void SetMode(EFixtureMode mode) { m_mode = mode; }
		glm::vec3 GetOffset() const { return m_offset; }
		void SetOffset(const glm::vec3& offset) { m_offset = offset; }

	private:
		float m_moveSpeed = 5.0f;
		EFixtureMode m_mode = EFixtureMode::Default;
		glm::vec3 m_offset{ 1.0f, 2.0f, 3.0f };
	};

	using WorkspaceTypes = Sailor::Workspace::TWorkspaceTypeList<FixtureComponent>;
}

REFL_AUTO(
	type(SAILOR_TEST_FIXTURE_NAMESPACE::FixtureComponent, bases<Sailor::Component>),
	func(GetMoveSpeed, property("moveSpeed")),
	func(SetMoveSpeed, property("moveSpeed")),
	func(GetMode, property("mode")),
	func(SetMode, property("mode")),
	func(GetOffset, property("offset")),
	func(SetOffset, property("offset"))
)
#endif

namespace SelectedWorkspaceFixture = SAILOR_TEST_FIXTURE_NAMESPACE;

namespace
{
#if SAILOR_TEST_METADATA_MODE == 1
	constexpr char WorkspaceModuleName[] = "PropertyMismatchFixture";
#elif SAILOR_TEST_METADATA_MODE == 2
	constexpr char WorkspaceModuleName[] = "DuplicateCdoFixture";
#elif SAILOR_TEST_METADATA_MODE == 3
	constexpr char WorkspaceModuleName[] = "UnknownDefaultFixture";
#elif SAILOR_TEST_METADATA_MODE == 4
	constexpr char WorkspaceModuleName[] = "InvalidDefaultTypeFixture";
#elif SAILOR_TEST_METADATA_MODE == 5
	constexpr char WorkspaceModuleName[] = "MissingDefaultFixture";
#elif SAILOR_TEST_METADATA_MODE == 6
	constexpr char WorkspaceModuleName[] = "InvalidEnumDefaultFixture";
#elif SAILOR_TEST_METADATA_MODE == 7
	constexpr char WorkspaceModuleName[] = "InvalidStructuredDefaultFixture";
#elif SAILOR_TEST_METADATA_MODE == 8
	constexpr char WorkspaceModuleName[] = "MissingCdoFixture";
#elif SAILOR_TEST_METADATA_MODE == 9
	constexpr char WorkspaceModuleName[] = "OversizedStructuredDefaultFixture";
#elif SAILOR_TEST_METADATA_MODE == 10
	constexpr char WorkspaceModuleName[] = "ShadowedPropertyFixture";
#elif SAILOR_TEST_METADATA_MODE == 11
	constexpr char WorkspaceModuleName[] = "MissingEmptyPropertySchemaFixture";
#elif SAILOR_TEST_METADATA_MODE == 12
	constexpr char WorkspaceModuleName[] = "AliasExpansionFixture";
#else
#error Unsupported SAILOR_TEST_METADATA_MODE
#endif

	std::string BuildMalformedMetadata()
	{
		YAML::Node metadata = YAML::Load(
			Sailor::Workspace::Internal::TWorkspaceTypeMetadataSerializer<
				SelectedWorkspaceFixture::WorkspaceTypes>::Serialize(WorkspaceModuleName));

#if SAILOR_TEST_METADATA_MODE == 1
		metadata["engineTypes"][0]["properties"]["moveSpeed"] = "string";
#elif SAILOR_TEST_METADATA_MODE == 2
		metadata["cdos"].push_back(metadata["cdos"][0]);
#elif SAILOR_TEST_METADATA_MODE == 3
		metadata["cdos"][0]["defaultValues"]["unexpectedDefault"] = 7;
#elif SAILOR_TEST_METADATA_MODE == 4
		metadata["cdos"][0]["defaultValues"]["moveSpeed"] = "not-a-float";
#elif SAILOR_TEST_METADATA_MODE == 5
		metadata["cdos"][0]["defaultValues"].remove("moveSpeed");
#elif SAILOR_TEST_METADATA_MODE == 6
		metadata["cdos"][0]["defaultValues"]["mode"] = "NotARealMode";
#elif SAILOR_TEST_METADATA_MODE == 7
		metadata["cdos"][0]["defaultValues"]["offset"] = "not-a-vector";
#elif SAILOR_TEST_METADATA_MODE == 8
		metadata["cdos"] = YAML::Node(YAML::NodeType::Sequence);
#elif SAILOR_TEST_METADATA_MODE == 9
		metadata["cdos"][0]["defaultValues"]["offset"].push_back(4.0f);
#elif SAILOR_TEST_METADATA_MODE == 10
		// Canonical metadata is intentionally rejected because the derived property shadows its base.
#elif SAILOR_TEST_METADATA_MODE == 11
		metadata["engineTypes"][0].remove("properties");
#elif SAILOR_TEST_METADATA_MODE == 12
		// The alias expansion is injected into the serialized payload below.
#endif

		YAML::Emitter emitter;
		emitter << metadata;
		if (!emitter.good())
		{
			throw std::runtime_error(emitter.GetLastError());
		}

		std::string payload = emitter.c_str();
#if SAILOR_TEST_METADATA_MODE == 12
		const size_t markerPosition = payload.find("defaultValues:");
		const size_t lineStart = payload.rfind('\n', markerPosition);
		const size_t lineEnd = payload.find('\n', markerPosition);
		if (markerPosition == std::string::npos || lineEnd == std::string::npos)
		{
			throw std::runtime_error("Failed to locate defaultValues for alias expansion fixture");
		}

		const size_t indentation = markerPosition -
			(lineStart == std::string::npos ? 0 : lineStart + 1);
		const std::string propertyIndent(indentation + 2, ' ');
		const std::string itemIndent(indentation + 4, ' ');
		std::string aliasExpansion;
		aliasExpansion.reserve(600 * 1024);
		aliasExpansion += propertyIndent + "aliasExpansion:\n";
		aliasExpansion += itemIndent + "- &workspaceAlias ";
		aliasExpansion.append(512 * 1024, 'x');
		aliasExpansion.push_back('\n');
		for (size_t aliasIndex = 0; aliasIndex < 160; ++aliasIndex)
		{
			aliasExpansion += itemIndent + "- *workspaceAlias\n";
		}
		payload.insert(lineEnd + 1, aliasExpansion);
#endif

		return payload;
	}

	uint32_t ExportMalformedMetadata(
		char* destination,
		uint64_t destinationCapacity,
		uint64_t* outPayloadSize) noexcept
	{
		if (outPayloadSize == nullptr)
		{
			return static_cast<uint32_t>(Sailor::Workspace::EWorkspaceModuleResult::InvalidArgument);
		}

		*outPayloadSize = 0;
		try
		{
			static const std::string payload = BuildMalformedMetadata();
			if (payload.size() > std::numeric_limits<uint64_t>::max())
			{
				return static_cast<uint32_t>(Sailor::Workspace::EWorkspaceModuleResult::SerializationFailed);
			}

			const uint64_t payloadSize = static_cast<uint64_t>(payload.size());
			*outPayloadSize = payloadSize;
			if (destination == nullptr)
			{
				return static_cast<uint32_t>(destinationCapacity == 0
					? Sailor::Workspace::EWorkspaceModuleResult::BufferTooSmall
					: Sailor::Workspace::EWorkspaceModuleResult::InvalidArgument);
			}

			if (destinationCapacity < payloadSize)
			{
				return static_cast<uint32_t>(Sailor::Workspace::EWorkspaceModuleResult::BufferTooSmall);
			}

			std::memcpy(destination, payload.data(), payload.size());
			return static_cast<uint32_t>(Sailor::Workspace::EWorkspaceModuleResult::Success);
		}
		catch (...)
		{
			*outPayloadSize = 0;
			return static_cast<uint32_t>(Sailor::Workspace::EWorkspaceModuleResult::SerializationFailed);
		}
	}

	uint32_t SAILOR_WORKSPACE_CALL RegisterWorkspaceTypes(
		const Sailor::Workspace::WorkspaceHostApiV1* hostApi) noexcept
	{
		return Sailor::Workspace::RegisterWorkspaceTypesV1<
			SelectedWorkspaceFixture::WorkspaceTypes>(hostApi);
	}
}

extern "C" SAILOR_WORKSPACE_MODULE_EXPORT uint32_t SAILOR_WORKSPACE_CALL SailorGetWorkspaceTypeMetadataV1(
	char* destination,
	uint64_t destinationCapacity,
	uint64_t* outPayloadSize) noexcept
{
	return ExportMalformedMetadata(destination, destinationCapacity, outPayloadSize);
}

extern "C" SAILOR_WORKSPACE_MODULE_EXPORT const Sailor::Workspace::WorkspaceModuleApiV1* SAILOR_WORKSPACE_CALL
	SailorGetWorkspaceModuleApiV1() noexcept
{
	static const Sailor::Workspace::WorkspaceModuleApiV1 api
	{
		static_cast<uint32_t>(sizeof(Sailor::Workspace::WorkspaceModuleApiV1)),
		Sailor::Workspace::WorkspaceModuleApiVersion,
		WorkspaceModuleName,
		static_cast<uint64_t>(sizeof(WorkspaceModuleName) - 1),
		Sailor::Workspace::GetWorkspaceModuleAbiTagV1(),
		Sailor::Workspace::GetWorkspaceModuleAbiTagV1Length(),
		&SailorGetWorkspaceTypeMetadataV1,
		&RegisterWorkspaceTypes
	};

	return &api;
}

#undef SAILOR_TEST_FIXTURE_NAMESPACE
