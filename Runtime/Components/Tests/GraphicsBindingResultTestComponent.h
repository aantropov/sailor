#pragma once
#include "Components/Tests/TestCaseComponent.h"
#include "Submodules/ImGuiApi.h"
#include "Tasks/Tasks.h"
#include <array>
#include <atomic>

namespace Sailor
{
	class GraphicsBindingResultTestComponent final : public TestCaseComponent
	{
		SAILOR_REFLECTABLE(GraphicsBindingResultTestComponent)

	public:
		SAILOR_API ~GraphicsBindingResultTestComponent() override;
		SAILOR_API void Tick(float deltaTime) override;

	private:
		std::array<ShaderSetPtr, 3> m_shaders;
		TSharedPtr<ImGuiApi::PreparedFrame> m_imguiFrame;
		std::atomic<uint32_t> m_callbacks{ 0u };
		Tasks::TaskPtr<std::string> m_validation;
	};
}

REFL_AUTO(
	type(Sailor::GraphicsBindingResultTestComponent, bases<Sailor::TestCaseComponent>)
)
