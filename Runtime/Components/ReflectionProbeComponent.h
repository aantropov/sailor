#pragma once

#include "Components/Component.h"
#include "ECS/ReflectionsECS.h"
#include "AssetRegistry/Texture/TextureImporter.h"

namespace Sailor
{
	class ReflectionProbeComponent final : public Component
	{
		SAILOR_REFLECTABLE(ReflectionProbeComponent)

	public:
		SAILOR_API void Initialize() override;
		SAILOR_API void EndPlay() override;
		SAILOR_API void OnGizmo() override;

		TexturePtr GetReflectionMap() const { return m_configuration.m_reflectionMap; }
		EReflectionProbeUpdateMode GetUpdateMode() const { return m_configuration.m_updateMode; }
		float GetUpdateInterval() const { return m_configuration.m_updateInterval; }
		uint32_t GetResolution() const { return m_configuration.m_resolution; }
		float GetRadius() const { return m_configuration.m_radius; }
		SAILOR_API void SetReflectionMap(const TexturePtr& value);
		SAILOR_API void SetUpdateMode(EReflectionProbeUpdateMode value);
		SAILOR_API void SetUpdateInterval(float value);
		SAILOR_API void SetResolution(uint32_t value);
		SAILOR_API void SetRadius(float value);
		SAILOR_API const ReflectionProbeData& GetData() const;

	private:
		void ApplyConfiguration();
		ReflectionProbeConfiguration m_configuration;
		size_t m_handle = ECS::InvalidIndex;
	};
}

using namespace Sailor::Attributes;

REFL_AUTO(
	type(Sailor::ReflectionProbeComponent, bases<Sailor::Component>),
	func(GetReflectionMap, property("reflectionMap")),
	func(SetReflectionMap, property("reflectionMap")),
	func(GetUpdateMode, property("updateMode")),
	func(SetUpdateMode, property("updateMode")),
	func(GetUpdateInterval, property("updateInterval"), Range(0.01, 3600.0)),
	func(SetUpdateInterval, property("updateInterval")),
	func(GetResolution, property("resolution"), Range(16.0, 512.0)),
	func(SetResolution, property("resolution")),
	func(GetRadius, property("radius"), Range(0.01, 100000.0)),
	func(SetRadius, property("radius"))
)
