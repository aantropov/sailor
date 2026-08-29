#pragma once

#include "Components/Tests/VisualTestCaseComponent.h"

namespace Sailor
{
	class GlobalIlluminationVisualTestComponent final :
		public VisualTestCaseComponent
	{
		SAILOR_REFLECTABLE(GlobalIlluminationVisualTestComponent)

	public:
		SAILOR_API void BeginPlay() override;
		SAILOR_API void Tick(float deltaTime) override;

	private:
		GameObjectPtr SpawnBox(
			const char* name,
			const glm::vec3& position,
			const glm::vec3& scale,
			EMobilityType mobility);
		void EnsureSkyAndCamera();
		bool AreProbeStatesResident() const;
		bool ValidateFinalSnapshot(std::string& outDiagnostic) const;

		GameObjectPtr m_dynamicReceiver{};
		uint64_t m_initialCompositionCount = 0u;
		uint32_t m_totalFrames = 0u;
		uint32_t m_transitionStartFrame = 0u;
		uint32_t m_transitionFrame = 0u;
		float m_elapsedTime = 0.0f;
		bool m_bTransitionStarted = false;
		bool m_bFinalSnapshotValidated = false;
	};
}

using namespace Sailor::Attributes;

REFL_AUTO(
	type(Sailor::GlobalIlluminationVisualTestComponent,
		bases<Sailor::VisualTestCaseComponent>)
)
