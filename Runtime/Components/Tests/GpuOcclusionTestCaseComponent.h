#pragma once
#include "Components/Tests/TestCaseComponent.h"
#include "Tasks/Tasks.h"

namespace Sailor
{
	class GpuOcclusionTestCaseComponent final : public TestCaseComponent
	{
		SAILOR_REFLECTABLE(GpuOcclusionTestCaseComponent)

	public:
		SAILOR_API void Tick(float deltaTime) override;

	private:
		ShaderSetPtr m_cullingShader;
		ShaderSetPtr m_depthInputShader;
		ShaderSetPtr m_depthMipShader;
		Tasks::TaskPtr<std::string> m_validation;
		int64_t m_gpuStartTimeMs = 0;
	};
}

REFL_AUTO(
	type(Sailor::GpuOcclusionTestCaseComponent, bases<Sailor::TestCaseComponent>)
)
