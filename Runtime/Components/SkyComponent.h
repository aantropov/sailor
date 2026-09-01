#pragma once
#include "Components/Component.h"
#include "Components/LightComponent.h"
#include "FrameGraph/SkyParameters.h"

namespace Sailor
{
	class SkyComponent : public Component
	{
		SAILOR_REFLECTABLE(SkyComponent)

	public:

		SAILOR_API SkyComponent();

		SAILOR_API virtual void BeginPlay() override;
		SAILOR_API virtual void EndPlay() override;
		SAILOR_API virtual void Tick(float deltaTime) override;
		SAILOR_API virtual void EditorTick(float deltaTime) override;

		SAILOR_API float GetSunAngle() const { return m_sunAngleDegrees; }
		SAILOR_API void SetSunAngle(float value);

		SAILOR_API float GetCloudsDensity() const { return m_skyParams.m_cloudsDensity; }
		SAILOR_API void SetCloudsDensity(float value);

		SAILOR_API float GetCloudsCoverage() const { return m_skyParams.m_cloudsCoverage; }
		SAILOR_API void SetCloudsCoverage(float value);

		SAILOR_API float GetCloudsAttenuation1() const { return m_skyParams.m_cloudsAttenuation1; }
		SAILOR_API void SetCloudsAttenuation1(float value);

		SAILOR_API float GetCloudsAttenuation2() const { return m_skyParams.m_cloudsAttenuation2; }
		SAILOR_API void SetCloudsAttenuation2(float value);

		SAILOR_API float GetCloudsPhaseInfluence1() const { return m_skyParams.m_phaseInfluence1; }
		SAILOR_API void SetCloudsPhaseInfluence1(float value);

		SAILOR_API float GetCloudsPhaseEccentricity1() const { return m_skyParams.m_eccentrisy1; }
		SAILOR_API void SetCloudsPhaseEccentricity1(float value);

		SAILOR_API float GetCloudsPhaseInfluence2() const { return m_skyParams.m_phaseInfluence2; }
		SAILOR_API void SetCloudsPhaseInfluence2(float value);

		SAILOR_API float GetCloudsPhaseEccentricity2() const { return m_skyParams.m_eccentrisy2; }
		SAILOR_API void SetCloudsPhaseEccentricity2(float value);

		SAILOR_API float GetCloudsHorizonBlend() const { return m_skyParams.m_fog; }
		SAILOR_API void SetCloudsHorizonBlend(float value);

		SAILOR_API float GetCloudScatteringScale() const { return m_skyParams.m_cloudScatteringScale; }
		SAILOR_API void SetCloudScatteringScale(float value);

		SAILOR_API float GetAmbient() const { return m_skyParams.m_ambient; }
		SAILOR_API void SetAmbient(float value);

		SAILOR_API float GetGiIndirectIntensity() const { return m_giIndirectIntensity; }
		SAILOR_API void SetGiIndirectIntensity(float value);

		SAILOR_API int32_t GetScatteringSteps() const { return m_skyParams.m_scatteringSteps; }
		SAILOR_API void SetScatteringSteps(int32_t value);

		SAILOR_API float GetScatteringDensity() const { return m_skyParams.m_scatteringDensity; }
		SAILOR_API void SetScatteringDensity(float value);

		SAILOR_API float GetScatteringIntensity() const { return m_skyParams.m_scatteringIntensity; }
		SAILOR_API void SetScatteringIntensity(float value);

		SAILOR_API float GetScatteringPhase() const { return m_skyParams.m_scatteringPhase; }
		SAILOR_API void SetScatteringPhase(float value);

		SAILOR_API int32_t GetSunShaftsDistance() const { return m_skyParams.m_sunShaftsDistance; }
		SAILOR_API void SetSunShaftsDistance(int32_t value);

		SAILOR_API float GetSunShaftsIntensity() const { return m_skyParams.m_sunShaftsIntensity; }
		SAILOR_API void SetSunShaftsIntensity(float value);

		SAILOR_API const TObjectPtr<LightComponent>& GetDirectionalLight() const { return m_directionalLight; }
		SAILOR_API void SetDirectionalLight(const TObjectPtr<LightComponent>& value);

		SAILOR_API const glm::vec3& GetSunIlluminance() const { return m_sunIlluminance; }
		SAILOR_API void SetSunIlluminance(const glm::vec3& value);

		SAILOR_API const SkyParameters& GetSkyParameters() const { return m_skyParams; }

	protected:

		void Apply();
		void UpdateLightDirection();

		SkyParameters m_skyParams{};
		TObjectPtr<LightComponent> m_directionalLight{};
		glm::vec3 m_sunIlluminance = glm::vec3(120000.0f);
		float m_sunAngleDegrees = 60.0f;
		float m_giIndirectIntensity = 1.0f;
	};
}

using namespace Sailor::Attributes;

REFL_AUTO(
	type(Sailor::SkyComponent, bases<Sailor::Component>),

	func(GetSunAngle, property("sunAngle"), Range(-25.0, 89.0)),
	func(SetSunAngle, property("sunAngle")),

	func(GetCloudsDensity, property("cloudsDensity"), Range(0.0, 1.0)),
	func(SetCloudsDensity, property("cloudsDensity")),

	func(GetCloudsCoverage, property("cloudsCoverage"), Range(0.0, 2.0)),
	func(SetCloudsCoverage, property("cloudsCoverage")),

	func(GetCloudsAttenuation1, property("cloudsAttenuation1"), Range(0.1, 0.3)),
	func(SetCloudsAttenuation1, property("cloudsAttenuation1")),

	func(GetCloudsAttenuation2, property("cloudsAttenuation2"), Range(0.001, 0.1)),
	func(SetCloudsAttenuation2, property("cloudsAttenuation2")),

	func(GetCloudsPhaseInfluence1, property("cloudsPhaseInfluence1"), Range(0.0, 1.0)),
	func(SetCloudsPhaseInfluence1, property("cloudsPhaseInfluence1")),

	func(GetCloudsPhaseEccentricity1, property("cloudsPhaseEccentricity1"), Range(0.0, 1.0)),
	func(SetCloudsPhaseEccentricity1, property("cloudsPhaseEccentricity1")),

	func(GetCloudsPhaseInfluence2, property("cloudsPhaseInfluence2"), Range(0.0, 1.0)),
	func(SetCloudsPhaseInfluence2, property("cloudsPhaseInfluence2")),

	func(GetCloudsPhaseEccentricity2, property("cloudsPhaseEccentricity2"), Range(0.01, 1.0)),
	func(SetCloudsPhaseEccentricity2, property("cloudsPhaseEccentricity2")),

	func(GetCloudsHorizonBlend, property("cloudsHorizonBlend"), Range(0.0, 20.0)),
	func(SetCloudsHorizonBlend, property("cloudsHorizonBlend")),

	func(GetCloudScatteringScale, property("cloudScatteringScale"), Range(0.0, 8.0)),
	func(SetCloudScatteringScale, property("cloudScatteringScale")),

	func(GetAmbient, property("ambient"), Range(0.0, 10.0)),
	func(SetAmbient, property("ambient")),

	func(GetGiIndirectIntensity, property("giIndirectIntensity"), Range(0.0, 16.0)),
	func(SetGiIndirectIntensity, property("giIndirectIntensity")),

	func(GetScatteringSteps, property("scatteringSteps"), Range(1.0, 10.0)),
	func(SetScatteringSteps, property("scatteringSteps")),

	func(GetScatteringDensity, property("scatteringDensity"), Range(0.1, 1.0)),
	func(SetScatteringDensity, property("scatteringDensity")),

	func(GetScatteringIntensity, property("scatteringIntensity"), Range(0.01, 1.0)),
	func(SetScatteringIntensity, property("scatteringIntensity")),

	func(GetScatteringPhase, property("scatteringPhase"), Range(0.001, 1.0)),
	func(SetScatteringPhase, property("scatteringPhase")),

	func(GetSunShaftsDistance, property("sunShaftsDistance"), Range(1.0, 100.0)),
	func(SetSunShaftsDistance, property("sunShaftsDistance")),

	func(GetSunShaftsIntensity, property("sunShaftsIntensity"), Range(0.001, 1.0)),
	func(SetSunShaftsIntensity, property("sunShaftsIntensity")),

	func(GetDirectionalLight, property("m_directionalLight")),
	func(SetDirectionalLight, property("m_directionalLight")),

	func(GetSunIlluminance, property("sunIlluminance")),
	func(SetSunIlluminance, property("sunIlluminance"))
)
