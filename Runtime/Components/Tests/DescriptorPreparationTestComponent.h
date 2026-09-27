#pragma once
#include "Components/Tests/TestCaseComponent.h"
#include "Tasks/Tasks.h"

namespace Sailor
{
	class DescriptorPreparationTestComponent final : public TestCaseComponent
	{
		SAILOR_REFLECTABLE(DescriptorPreparationTestComponent)

	public:
		SAILOR_API void Tick(float deltaTime) override;

	private:
		struct ValidationResult
		{
			std::string m_error;
			bool m_bVariableDescriptorsTested = false;
		};

		ShaderSetPtr m_shader;
		Tasks::TaskPtr<ValidationResult> m_validation;
	};
}

REFL_AUTO(
	type(Sailor::DescriptorPreparationTestComponent, bases<Sailor::TestCaseComponent>)
)
