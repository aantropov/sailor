#include "Components/Component.h"
#include "Workspace/WorkspaceTypeMetadata.h"

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

extern "C" SAILOR_WORKSPACE_MODULE_EXPORT uint32_t SAILOR_WORKSPACE_CALL SailorGetWorkspaceTypeMetadataV1(
	char* destination,
	uint64_t destinationCapacity,
	uint64_t* outPayloadSize) noexcept
{
	return Sailor::Workspace::ExportWorkspaceTypeMetadataV1<WorkspaceFixture::WorkspaceTypes>(
		"WorkspaceFixture",
		destination,
		destinationCapacity,
		outPayloadSize);
}
