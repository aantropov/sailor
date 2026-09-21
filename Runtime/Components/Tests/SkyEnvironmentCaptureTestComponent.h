#pragma once
#include "Components/Tests/TestCaseComponent.h"
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
		bool QueueLocalReflection(uint32_t pendingSamples, uint32_t finalSamples, uint32_t publishedSamples);

		TSharedPtr<CaptureState> m_capture;
		Tasks::TaskPtr<CheckResult> m_check;
		Tasks::TaskPtr<bool> m_localPublicationCheck;
		bool m_bHandoffComplete = false;
		bool m_bSkyCaptureComplete = false;
		uint32_t m_expectedLocalSamples = 0;
	};
}

REFL_AUTO(
	type(Sailor::SkyEnvironmentCaptureTestComponent, bases<Sailor::TestCaseComponent>)
)
