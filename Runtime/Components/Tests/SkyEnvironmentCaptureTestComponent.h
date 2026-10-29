#pragma once
#include "Components/Tests/TestCaseComponent.h"
#include "Engine/Types.h"
#include "Tasks/Tasks.h"

namespace Sailor
{
	class SkyEnvironmentCaptureTestComponent final : public TestCaseComponent
	{
		SAILOR_REFLECTABLE(SkyEnvironmentCaptureTestComponent)

	public:
		SAILOR_API ~SkyEnvironmentCaptureTestComponent() override;
		SAILOR_API void BeginPlay() override;
		SAILOR_API void Tick(float deltaTime) override;
		SAILOR_API void EndPlay() override;

	private:
		struct CheckResult
		{
			bool m_bPassed = false;
			std::string m_message;
		};

		struct CaptureState;
		enum class LocalStage
		{
			None, FirstCapture, Replacement, ShadowsDisabled, CaptureRecovery, CaptureCancellation,
			Warm, AwaitReset, Reset
		};
		bool PrepareReflectionMarkers(std::string& error);

		TSharedPtr<CaptureState> m_capture;
		Tasks::TaskPtr<CheckResult> m_check;
		TVector<GameObjectPtr> m_markers;
		GameObjectPtr m_shadowReceiver;
		GameObjectPtr m_shadowBlocker;
		GameObjectPtr m_localLight;
		bool m_bSkyCaptureComplete = false;
		bool m_bMarkersInitialized = false;
		uint64_t m_markersReadyFrame = 0u;
		uint64_t m_expectedLocalGeneration = 0u;
		uint64_t m_expectedSubmissions = 0u;
		LocalStage m_localStage = LocalStage::None;
	};
}

REFL_AUTO(
	type(Sailor::SkyEnvironmentCaptureTestComponent, bases<Sailor::TestCaseComponent>)
)
