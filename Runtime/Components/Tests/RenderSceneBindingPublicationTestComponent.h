#pragma once
#include "Components/Tests/TestCaseComponent.h"
#include "Tasks/Tasks.h"

namespace Sailor
{
	class RenderSceneBindingPublicationTestComponent final : public TestCaseComponent
	{
		SAILOR_REFLECTABLE(RenderSceneBindingPublicationTestComponent)

	public:
		SAILOR_API void Tick(float deltaTime) override;

	private:
		ShaderSetPtr m_shader;
		Tasks::TaskPtr<std::string> m_validation;
	};
}

REFL_AUTO(
	type(Sailor::RenderSceneBindingPublicationTestComponent, bases<Sailor::TestCaseComponent>)
)
