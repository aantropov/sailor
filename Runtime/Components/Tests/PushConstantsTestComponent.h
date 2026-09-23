#pragma once
#include "Components/Tests/TestCaseComponent.h"
#include "Tasks/Tasks.h"
#include <array>

namespace Sailor
{
	class PushConstantsTestComponent final : public TestCaseComponent
	{
		SAILOR_REFLECTABLE(PushConstantsTestComponent)

	public:
		SAILOR_API void Tick(float deltaTime) override;

	private:
		std::array<ShaderSetPtr, 2> m_computeShaders;
		std::array<ShaderSetPtr, 4> m_graphicsShaders;
		Tasks::TaskPtr<std::string> m_validation;
	};
}

REFL_AUTO(
	type(Sailor::PushConstantsTestComponent, bases<Sailor::TestCaseComponent>)
)
