#pragma once
#include "Components/Tests/TestCaseComponent.h"
#include "Tasks/Tasks.h"

namespace Sailor
{
	class SkyDrawCompletionTestComponent final : public TestCaseComponent
	{
		SAILOR_REFLECTABLE(SkyDrawCompletionTestComponent)

	public:
		SAILOR_API ~SkyDrawCompletionTestComponent() override;
		SAILOR_API void Tick(float deltaTime) override;
		SAILOR_API void EndPlay() override;

	private:
		struct StepResult
		{
			bool m_bFinished = false;
			std::string m_error;
		};
		struct CaptureState;
		TSharedPtr<CaptureState> m_capture;
		Tasks::TaskPtr<StepResult> m_step;
	};
}

REFL_AUTO(
	type(Sailor::SkyDrawCompletionTestComponent, bases<Sailor::TestCaseComponent>)
)
