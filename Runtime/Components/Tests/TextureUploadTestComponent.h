#pragma once
#include "Components/Tests/TestCaseComponent.h"
#include "Tasks/Tasks.h"

namespace Sailor
{
	class TextureUploadTestComponent final : public TestCaseComponent
	{
		SAILOR_REFLECTABLE(TextureUploadTestComponent)

	public:
		SAILOR_API void Tick(float deltaTime) override;

	private:
		ShaderSetPtr m_shader;
		Tasks::TaskPtr<std::string> m_validation;
	};
}

REFL_AUTO(
	type(Sailor::TextureUploadTestComponent, bases<Sailor::TestCaseComponent>)
)
