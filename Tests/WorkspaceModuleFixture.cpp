#include "Components/Component.h"
#include "Workspace/WorkspaceTypeRegistration.h"

namespace WorkspaceFixture
{
	class FixtureComponent final : public Sailor::Component
	{
		SAILOR_WORKSPACE_REFLECTABLE(FixtureComponent)

	public:
		float GetMoveSpeed() const { return m_moveSpeed; }
		void SetMoveSpeed(float moveSpeed) { m_moveSpeed = moveSpeed; }

	private:
		float m_moveSpeed = 5.0f;
	};

	using WorkspaceTypes = Sailor::Workspace::TWorkspaceTypeList<FixtureComponent>;
}

REFL_AUTO(
	type(WorkspaceFixture::FixtureComponent, bases<Sailor::Component>),
	func(GetMoveSpeed, property("moveSpeed")),
	func(SetMoveSpeed, property("moveSpeed"))
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
