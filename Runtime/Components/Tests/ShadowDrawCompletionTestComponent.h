#pragma once
#include "Components/Tests/TestCaseComponent.h"
#include "Tasks/Tasks.h"

namespace Sailor
{
	struct ShadowDrawCompletionState;

	class ShadowDrawCompletionTestComponent final : public TestCaseComponent
	{
		SAILOR_REFLECTABLE(ShadowDrawCompletionTestComponent)

	public:
		SAILOR_API ~ShadowDrawCompletionTestComponent() override;
		SAILOR_API void Tick(float deltaTime) override;

	private:
		TSharedPtr<ShadowDrawCompletionState> m_state;
		Tasks::TaskPtr<std::string> m_validation;
	};
}

REFL_AUTO(
	type(Sailor::ShadowDrawCompletionTestComponent, bases<Sailor::TestCaseComponent>)
)
