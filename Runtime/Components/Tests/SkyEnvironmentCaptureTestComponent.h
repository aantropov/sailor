#pragma once
#include "Components/Tests/TestCaseComponent.h"
#include "Tasks/Tasks.h"
#include <memory>

namespace Sailor
{
	class SkyEnvironmentCaptureTestComponent final : public TestCaseComponent
	{
		SAILOR_REFLECTABLE(SkyEnvironmentCaptureTestComponent)

	public:
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
		std::shared_ptr<CaptureState> m_capture;
		Tasks::TaskPtr<CheckResult> m_check;
	};
}

REFL_AUTO(
	type(Sailor::SkyEnvironmentCaptureTestComponent, bases<Sailor::TestCaseComponent>)
)
