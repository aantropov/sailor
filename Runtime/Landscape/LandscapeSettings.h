#pragma once

#include "Core/Reflection.h"

namespace Sailor
{
	enum class ELandscapeVegetationShadowMode : uint8_t
	{
		None = 0u,
		NearOnly,
		All
	};

	enum class ELandscapeVegetationResidency : uint8_t
	{
		Persistent = 0u,
		Grass
	};

	enum class ELandscapeSculptOperation : uint8_t
	{
		Raise,
		Lower,
		Flatten
	};

	struct LandscapeVegetationSettings
	{
		FileId m_modelFileId{};
		FileId m_materialFileId{};
		int32_t m_meshIndex = -1;
		uint32_t m_instancesPerChunk = 0u;
		ELandscapeVegetationResidency m_residency = ELandscapeVegetationResidency::Persistent;
		float m_priority = 1.0f;
		float m_minScale = 0.75f;
		float m_maxScale = 1.25f;
		float m_groundOffset = 0.0f;
		ELandscapeVegetationShadowMode m_shadowMode = ELandscapeVegetationShadowMode::NearOnly;
		float m_shadowDistance = 35.0f;
		uint32_t m_minLod = 0u;
		uint32_t m_maxLod = 2u;
		TVector<float> m_screenCoverageThresholds{ 0.25f, 0.05f };
		float m_cullDistance = 120.0f;
		float m_colliderRadius = 0.0f;
		float m_colliderHeight = 2.0f;
		float m_colliderOffsetY = 1.0f;

		SAILOR_API void Normalize();
		bool HasCollision() const { return m_residency == ELandscapeVegetationResidency::Persistent && m_colliderRadius > 0.0f; }
		bool operator==(const LandscapeVegetationSettings&) const = default;
	};

	struct LandscapeSculptStamp
	{
		float m_x = 0.0f;
		float m_z = 0.0f;
		float m_radius = 1.0f;
		float m_strength = 1.0f;
		ELandscapeSculptOperation m_operation = ELandscapeSculptOperation::Raise;
		bool operator==(const LandscapeSculptStamp&) const = default;
	};

	struct LandscapePaintStamp
	{
		float m_x = 0.0f;
		float m_z = 0.0f;
		float m_radius = 1.0f;
		float m_strength = 1.0f;
		uint32_t m_layer = 0u;
		bool operator==(const LandscapePaintStamp&) const = default;
	};
}

REFL_AUTO(type(Sailor::LandscapeVegetationSettings),
	field(m_modelFileId), field(m_materialFileId),
	field(m_meshIndex, Sailor::Attributes::Range(-1, 65535)),
	field(m_instancesPerChunk, Sailor::Attributes::Range(0, 2048)),
	field(m_residency),
	field(m_priority, Sailor::Attributes::Range(0, 100)),
	field(m_minScale), field(m_maxScale), field(m_groundOffset),
	field(m_shadowMode), field(m_shadowDistance),
	field(m_minLod, Sailor::Attributes::Range(0, 15)),
	field(m_maxLod, Sailor::Attributes::Range(0, 15)),
	field(m_screenCoverageThresholds), field(m_cullDistance),
	field(m_colliderRadius), field(m_colliderHeight), field(m_colliderOffsetY))

REFL_AUTO(type(Sailor::LandscapeSculptStamp),
	field(m_x), field(m_z), field(m_radius), field(m_strength), field(m_operation))

REFL_AUTO(type(Sailor::LandscapePaintStamp),
	field(m_x), field(m_z), field(m_radius), field(m_strength),
	field(m_layer, Sailor::Attributes::Range(0, 3)))
