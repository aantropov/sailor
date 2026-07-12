#include "Components/Component.h"
#include "Workspace/WorkspaceTypeRegistration.h"

namespace WorkspaceFixture
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
		FixtureComponent()
			: m_registryLookupSucceeded(Sailor::Reflection::TryGetTypeByName(
				"WorkspaceFixture::FixtureComponent") != nullptr)
		{
		}

		float GetMoveSpeed() const { return m_moveSpeed; }
		void SetMoveSpeed(float moveSpeed) { m_moveSpeed = moveSpeed; }
		bool GetRegistryLookupSucceeded() const { return m_registryLookupSucceeded; }
		void SetRegistryLookupSucceeded(bool succeeded) { m_registryLookupSucceeded = succeeded; }
		int32_t GetReadOnlyValue() const { return m_readOnlyValue; }
		int32_t GetSkippedReadOnlyValue() const { return m_skippedReadOnlyValue; }
		float GetSkippedDefault() const { return m_skippedDefault; }
		void SetSkippedDefault(float value) { m_skippedDefault = value; }
		EFixtureMode GetMode() const { return m_mode; }
		void SetMode(EFixtureMode mode) { m_mode = mode; }
		Sailor::EMobilityType GetMobility() const { return m_mobility; }
		void SetMobility(Sailor::EMobilityType mobility) { m_mobility = mobility; }
		glm::vec3 GetOffset() const { return m_offset; }
		void SetOffset(const glm::vec3& offset) { m_offset = offset; }
		Sailor::ComponentPtr GetNullableComponent() const { return m_nullableComponent; }
		void SetNullableComponent(const Sailor::ComponentPtr& component) { m_nullableComponent = component; }

	private:
		float m_moveSpeed = 5.0f;
		bool m_registryLookupSucceeded = false;
		int32_t m_readOnlyValue = 17;
		int32_t m_skippedReadOnlyValue = 23;
		float m_skippedDefault = 9.0f;
		EFixtureMode m_mode = EFixtureMode::Default;
		Sailor::EMobilityType m_mobility = Sailor::EMobilityType::Stationary;
		glm::vec3 m_offset{ 1.0f, 2.0f, 3.0f };
		Sailor::ComponentPtr m_nullableComponent;
	};

	using WorkspaceTypes = Sailor::Workspace::TWorkspaceTypeList<FixtureComponent>;
}

REFL_AUTO(
	type(WorkspaceFixture::FixtureComponent, bases<Sailor::Component>),
	func(GetMoveSpeed, property("moveSpeed")),
	func(SetMoveSpeed, property("moveSpeed")),
	func(GetRegistryLookupSucceeded, property("registryLookupSucceeded")),
	func(SetRegistryLookupSucceeded, property("registryLookupSucceeded")),
	func(GetReadOnlyValue, property("readOnlyValue")),
	func(GetSkippedReadOnlyValue, property("skippedReadOnlyValue"), Sailor::Attributes::SkipCDO()),
	func(GetSkippedDefault, property("skippedDefault"), Sailor::Attributes::SkipCDO()),
	func(SetSkippedDefault, property("skippedDefault"), Sailor::Attributes::SkipCDO()),
	func(GetMode, property("mode")),
	func(SetMode, property("mode")),
	func(GetMobility, property("mobility")),
	func(SetMobility, property("mobility")),
	func(GetOffset, property("offset")),
	func(SetOffset, property("offset")),
	func(GetNullableComponent, property("nullableComponent")),
	func(SetNullableComponent, property("nullableComponent"))
)

namespace
{
	constexpr char WorkspaceModuleName[] = "WorkspaceFixture";

	uint32_t SAILOR_WORKSPACE_CALL RegisterWorkspaceTypes(
		const Sailor::Workspace::WorkspaceHostApiV1* hostApi) noexcept
	{
		return Sailor::Workspace::RegisterWorkspaceTypesV1<WorkspaceFixture::WorkspaceTypes>(hostApi);
	}
}

extern "C" SAILOR_WORKSPACE_MODULE_EXPORT uint32_t SAILOR_WORKSPACE_CALL SailorGetWorkspaceTypeMetadataV1(
	char* destination,
	uint64_t destinationCapacity,
	uint64_t* outPayloadSize) noexcept
{
	return Sailor::Workspace::ExportWorkspaceTypeMetadataV1<WorkspaceFixture::WorkspaceTypes>(
		WorkspaceModuleName,
		destination,
		destinationCapacity,
		outPayloadSize);
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
