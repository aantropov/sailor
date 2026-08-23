#pragma once

#include "Components/Tests/VisualTestCaseComponent.h"
#include "RHI/RenderDebugView.h"

namespace Sailor
{
	class GlobalIlluminationLandscapeVisualTestComponent final :
		public VisualTestCaseComponent
	{
		SAILOR_REFLECTABLE(GlobalIlluminationLandscapeVisualTestComponent)

	public:
		SAILOR_API void BeginPlay() override;
		SAILOR_API void Tick(float deltaTime) override;
		SAILOR_API void EndPlay() override;

	private:
		bool ValidateLandscapeAndSecondaryLighting(
			std::string& outDiagnostic,
			float& outReceiverEnergy) const;

		RHI::ESceneViewRenderMode m_previousRenderMode =
			RHI::ESceneViewRenderMode::Lit;
		uint32_t m_totalFrames = 0u;
		bool m_bRenderModeChanged = false;
		bool m_bSceneValidated = false;
	};
}

using namespace Sailor::Attributes;

REFL_AUTO(
	type(Sailor::GlobalIlluminationLandscapeVisualTestComponent,
		bases<Sailor::VisualTestCaseComponent>)
)
