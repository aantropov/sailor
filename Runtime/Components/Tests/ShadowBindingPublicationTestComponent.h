#pragma once
#include "Components/Tests/TestCaseComponent.h"
#include "Tasks/Tasks.h"

namespace Sailor
{
	class ShadowBindingPublicationTestComponent final : public TestCaseComponent
	{
		SAILOR_REFLECTABLE(ShadowBindingPublicationTestComponent)

	public:
		SAILOR_API void Tick(float deltaTime) override;

	private:
		Tasks::TaskPtr<std::string> m_validation;
	};
}

REFL_AUTO(
	type(Sailor::ShadowBindingPublicationTestComponent, bases<Sailor::TestCaseComponent>)
)
