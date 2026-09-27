#pragma once
#include "Components/Tests/TestCaseComponent.h"
#include "Tasks/Tasks.h"
#include <array>

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
			bool m_bSparsePublicationTested = false;
		};

		ShaderSetPtr m_shader;
		std::array<ShaderSetPtr, 5> m_publicationShaders;
		Tasks::TaskPtr<ValidationResult> m_validation;
	};
}

REFL_AUTO(
	type(Sailor::DescriptorPreparationTestComponent, bases<Sailor::TestCaseComponent>)
)
