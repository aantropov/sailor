#pragma once
#include "Components/Tests/TestCaseComponent.h"
#include "Tasks/Tasks.h"

namespace Sailor
{
	struct TextureBindingPublicationState;

	class TextureBindingPublicationTestComponent final : public TestCaseComponent
	{
		SAILOR_REFLECTABLE(TextureBindingPublicationTestComponent)

	public:
		SAILOR_API ~TextureBindingPublicationTestComponent() override;
		SAILOR_API void Tick(float deltaTime) override;

	private:
		TSharedPtr<TextureBindingPublicationState> m_state;
		Tasks::TaskPtr<std::string> m_validation;
	};
}

REFL_AUTO(
	type(Sailor::TextureBindingPublicationTestComponent, bases<Sailor::TestCaseComponent>)
)
