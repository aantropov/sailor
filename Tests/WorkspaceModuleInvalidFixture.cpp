#include "Workspace/WorkspaceTypeRegistration.h"

namespace InvalidWorkspace
{
	enum class EMode { Default, Alternate };

	struct Settings
	{
		EMode m_mode = SAILOR_TEST_WORKSPACE_INVALID_CASE == 2 ? static_cast<EMode>(99) : EMode::Default;
		Sailor::TVector<EMode> m_modes{ SAILOR_TEST_WORKSPACE_INVALID_CASE == 3 ? static_cast<EMode>(99) : EMode::Default };
	};

	class BaseComponent : public Sailor::Component
	{
		SAILOR_WORKSPACE_REFLECTABLE(BaseComponent)
	public:
		float GetValue() const { return 1.0f; }
	};

	class FixtureComponent final : public BaseComponent
	{
		SAILOR_WORKSPACE_REFLECTABLE(FixtureComponent)
	public:
		EMode m_mode = SAILOR_TEST_WORKSPACE_INVALID_CASE == 1 ? static_cast<EMode>(99) : EMode::Default;
		Settings m_settings;
#if SAILOR_TEST_WORKSPACE_INVALID_CASE == 4
		std::string GetShadowedValue() const { return "shadow"; }
#endif
	};
}

REFL_AUTO(type(InvalidWorkspace::Settings), field(m_mode), field(m_modes))
REFL_AUTO(type(InvalidWorkspace::BaseComponent, bases<Sailor::Component>), func(GetValue, property("value")))
REFL_AUTO(
#if SAILOR_TEST_WORKSPACE_INVALID_CASE == 10
	type(InvalidWorkspace::FixtureComponent),
#else
	type(InvalidWorkspace::FixtureComponent, bases<InvalidWorkspace::BaseComponent>),
#endif
	field(m_mode), field(m_settings)
#if SAILOR_TEST_WORKSPACE_INVALID_CASE == 4
	, func(GetShadowedValue, property("value"))
#endif
)

namespace
{
	using namespace Sailor;
	using namespace Sailor::Workspace;

	uint32_t SAILOR_WORKSPACE_CALL RegisterTypes(const WorkspaceHostApiV1* host) noexcept
	{
		const auto baseResult = Sailor::Workspace::Internal::CollectWorkspaceTypeV1<InvalidWorkspace::BaseComponent>(*host);
		if (baseResult != static_cast<uint32_t>(EWorkspaceModuleResult::Success)) return baseResult;
		using Type = InvalidWorkspace::FixtureComponent;
		WorkspaceTypeDescriptorV1 descriptor{ sizeof(WorkspaceTypeDescriptorV1), &TypeInfo::Get<Type>(),
			sizeof(Type), alignof(Type), &Sailor::Workspace::Internal::ConstructWorkspaceTypeV1<Type> };
#if SAILOR_TEST_WORKSPACE_INVALID_CASE == 5
		++descriptor.typeSize;
#elif SAILOR_TEST_WORKSPACE_INVALID_CASE == 6
		descriptor.typeAlignment *= 2;
#elif SAILOR_TEST_WORKSPACE_INVALID_CASE == 7
		descriptor.typeInfo = nullptr;
#elif SAILOR_TEST_WORKSPACE_INVALID_CASE == 8
		descriptor.placementFactory = nullptr;
#elif SAILOR_TEST_WORKSPACE_INVALID_CASE == 9
		--descriptor.structSize;
#endif
		const auto result = host->collectType(host->context, &descriptor);
#if SAILOR_TEST_WORKSPACE_INVALID_CASE == 11
		if (result == static_cast<uint32_t>(EWorkspaceModuleResult::Success))
			return host->collectType(host->context, &descriptor);
#endif
		return result;
	}
}

extern "C" SAILOR_WORKSPACE_MODULE_EXPORT const Sailor::Workspace::WorkspaceModuleApiV1* SAILOR_WORKSPACE_CALL
	SailorGetWorkspaceModuleApiV1() noexcept
{
	using namespace Sailor::Workspace;
	static const WorkspaceModuleApiV1 api{ sizeof(WorkspaceModuleApiV1),
		SAILOR_TEST_WORKSPACE_INVALID_CASE == 12 ? 0u : WorkspaceModuleApiVersion,
		"InvalidFixture", sizeof("InvalidFixture") - 1,
		GetWorkspaceModuleAbiTagV1(), GetWorkspaceModuleAbiTagV1Length(), &RegisterTypes };
	return &api;
}
