#pragma once
#include "Sailor.h"
#include "Components/Component.h"

namespace Sailor
{
	class PerformanceTestSetupComponent : public Component
	{
		SAILOR_REFLECTABLE(PerformanceTestSetupComponent)

	public:
		SAILOR_API virtual void BeginPlay() override;
		SAILOR_API virtual void EndPlay() override;
		SAILOR_API virtual void Tick(float deltaTime) override;

		SAILOR_API uint32_t GetGridSize() const { return m_gridSize; }
		SAILOR_API void SetGridSize(uint32_t value) { m_gridSize = value; }
		SAILOR_API float GetSpacing() const { return m_spacing; }
		SAILOR_API void SetSpacing(float value) { m_spacing = value; }
		SAILOR_API glm::vec3 GetBaseColor() const { return m_baseColor; }
		SAILOR_API void SetBaseColor(const glm::vec3& value) { m_baseColor = value; }
		SAILOR_API glm::vec3 GetColorVariance() const { return m_colorVariance; }
		SAILOR_API void SetColorVariance(const glm::vec3& value) { m_colorVariance = value; }
		SAILOR_API float GetBoxScale() const { return m_boxScale; }
		SAILOR_API void SetBoxScale(float value) { m_boxScale = value; }
		SAILOR_API float GetRotationSpeedDeg() const { return m_rotationSpeedDeg; }
		SAILOR_API void SetRotationSpeedDeg(float value) { m_rotationSpeedDeg = value; }
		SAILOR_API float GetDirectionalLightIntensity() const { return m_directionalLightIntensity; }
		SAILOR_API void SetDirectionalLightIntensity(float value) { m_directionalLightIntensity = value; }
		SAILOR_API bool GetSpawnPointLights() const { return m_bSpawnPointLights; }
		SAILOR_API void SetSpawnPointLights(bool value) { m_bSpawnPointLights = value; }

	protected:
		void SpawnGrid();
		void SpawnLights();
		void EnsureCamera();
		bool ApplyRuntimeMaterialColors();

		TVector<glm::vec4> m_pendingColors;
		TVector<float> m_perObjectScale;
		TVector<float> m_perObjectRotationSpeedDeg;
		TVector<bool> m_bPerObjectRuntimeColorApplied;
		bool m_bAppliedRuntimeColors = false;

		uint32_t m_gridSize = 10;
		float m_spacing = 300.0f;
		glm::vec3 m_baseColor{ 0.3f, 0.3f, 0.3f };
		glm::vec3 m_colorVariance{ 0.5f, 0.5f, 0.5f };
		float m_boxScale = 60.0f;
		TVector<GameObjectPtr> m_spawnedObjects;
		float m_rotationSpeedDeg = 10.0f;
		float m_directionalLightIntensity = 320.0f;
		bool m_bSpawnPointLights = true;

		float m_minFps = FLT_MAX;
		float m_maxFps = 0.0f;
		double m_sumFps = 0.0;
		uint64_t m_numFpsSamples = 0;
	};
}

using namespace Sailor::Attributes;

REFL_AUTO(
	type(Sailor::PerformanceTestSetupComponent, bases<Sailor::Component>),

	func(GetGridSize, property("gridSize"), SkipCDO()),
	func(SetGridSize, property("gridSize"), SkipCDO()),
	func(GetSpacing, property("spacing"), SkipCDO()),
	func(SetSpacing, property("spacing"), SkipCDO()),
	func(GetBaseColor, property("baseColor"), SkipCDO()),
	func(SetBaseColor, property("baseColor"), SkipCDO()),
	func(GetColorVariance, property("colorVariance"), SkipCDO()),
	func(SetColorVariance, property("colorVariance"), SkipCDO()),
	func(GetBoxScale, property("boxScale"), SkipCDO()),
	func(SetBoxScale, property("boxScale"), SkipCDO()),
	func(GetRotationSpeedDeg, property("rotationSpeedDeg"), SkipCDO()),
	func(SetRotationSpeedDeg, property("rotationSpeedDeg"), SkipCDO()),
	func(GetDirectionalLightIntensity, property("directionalLightIntensity"), SkipCDO()),
	func(SetDirectionalLightIntensity, property("directionalLightIntensity"), SkipCDO()),
	func(GetSpawnPointLights, property("spawnPointLights"), SkipCDO()),
	func(SetSpawnPointLights, property("spawnPointLights"), SkipCDO())
)
