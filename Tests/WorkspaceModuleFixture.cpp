#include "AssetRegistry/AssetCache.h"
#include "Components/Component.h"
#include "ECS/TransformECS.h"
#include "ECS/LandscapeSettings.h"
#include "Workspace/WorkspaceTypeRegistration.h"

#include <atomic>

namespace WorkspaceFixture
{
	static std::atomic<uint32_t> g_numConstructions = 0;

	enum class EFixtureMode
	{
		Default,
		Alternate
	};

	struct FixtureTuning
	{
		float m_gain = 0.25f;
	};

	struct FixtureSettings
	{
		Sailor::TVector<FixtureTuning> m_layers{ FixtureTuning{} };
		Sailor::TVector<EFixtureMode> m_modes{ EFixtureMode::Alternate };
	};

	struct EmptySettings {};

	class FixtureComponent final : public Sailor::Component
	{
		SAILOR_WORKSPACE_REFLECTABLE(FixtureComponent)

	public:
		FixtureComponent()
			: m_registryLookupSucceeded(Sailor::Reflection::TryGetTypeByName(
				"WorkspaceFixture::FixtureComponent") != nullptr)
		{
			++g_numConstructions;
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
		const FixtureSettings& GetSettings() const { return m_settings; }
		void SetSettings(const FixtureSettings& settings) { m_settings = settings; }
		Sailor::LandscapeVegetationSettings m_vegetation;
		EmptySettings m_empty;

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
		FixtureSettings m_settings;
	};

	class ReloadedComponent final : public Sailor::Component
	{
		SAILOR_WORKSPACE_REFLECTABLE(ReloadedComponent)

	public:
		uint32_t m_capacity = 12;
	};

#if defined(SAILOR_TEST_RELOADED_WORKSPACE)
	using WorkspaceTypes = Sailor::Workspace::TWorkspaceTypeList<ReloadedComponent>;
#else
	using WorkspaceTypes = Sailor::Workspace::TWorkspaceTypeList<FixtureComponent>;
#endif
}

REFL_AUTO(type(WorkspaceFixture::ReloadedComponent, bases<Sailor::Component>), field(m_capacity))
REFL_AUTO(type(WorkspaceFixture::FixtureTuning), field(m_gain, Sailor::Attributes::Range(0.0, 1.0)))
REFL_AUTO(type(WorkspaceFixture::FixtureSettings), field(m_layers), field(m_modes))
REFL_AUTO(type(WorkspaceFixture::EmptySettings))

REFL_AUTO(
	type(WorkspaceFixture::FixtureComponent, bases<Sailor::Component>),
	func(GetMoveSpeed, property("moveSpeed"), Sailor::Attributes::Range(0.0, 10.0)),
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
	func(SetNullableComponent, property("nullableComponent")),
	func(GetSettings, property("settings")),
	func(SetSettings, property("settings")),
	field(m_vegetation),
	field(m_empty)
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

extern "C" SAILOR_WORKSPACE_MODULE_EXPORT uint64_t SAILOR_WORKSPACE_CALL
	SailorWorkspaceFixtureAssetCacheSize() noexcept
{
	return sizeof(Sailor::AssetCache);
}

extern "C" SAILOR_WORKSPACE_MODULE_EXPORT uint32_t SAILOR_WORKSPACE_CALL
	SailorWorkspaceFixtureConstructionCount() noexcept
{
	return WorkspaceFixture::g_numConstructions;
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
		&RegisterWorkspaceTypes
	};

	return &api;
}
