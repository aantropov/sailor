#pragma once

#include "Containers/Vector.h"
#include "Core/Defines.h"
#include "Math/Bounds.h"

#include <cstdint>

namespace Sailor::GlobalIlluminationLandscapeTestScene
{
	enum class EMaterial : uint8_t
	{
		Landscape = 0u,
		ShadowRidge,
		BounceCliff,
		ShadowReceiver,
		Count
	};

	struct Box final
	{
		const char* m_name = nullptr;
		glm::vec3 m_position{};
		// Content/Models/Box/Box.gltf is normalized to [-1, 1], so this is
		// both the GameObject scale and the world-space half extent.
		glm::vec3 m_scale{ 1.0f };
		EMaterial m_material = EMaterial::ShadowRidge;
		const char* m_materialPath = nullptr;
	};

	inline constexpr uint32_t LandscapeChunksX = 2u;
	inline constexpr uint32_t LandscapeChunksZ = 2u;
	inline constexpr float LandscapeChunkSize = 32.0f;
	inline constexpr uint32_t LandscapeChunkResolution = 16u;
	inline constexpr float LandscapeHeightScale = 0.0f;
	inline constexpr float LandscapeNoiseScale = 0.035f;
	inline constexpr uint32_t LandscapeSeed = 155u;
	inline constexpr float LandscapeTextureTiling = 0.09f;
	inline constexpr float LandscapeWorldY = -4.0f;

	SAILOR_SHARED_API const TVector<Box>& GetBoxes();
	SAILOR_SHARED_API TVector<float> GetLandscapeSculptStamps();
	SAILOR_SHARED_API float SampleLandscapeHeight(float x, float z);
	SAILOR_SHARED_API void BuildBakeTriangles(
		TVector<Math::Triangle>& outTriangles,
		Math::AABB& outBounds);

	SAILOR_SHARED_API glm::vec3 GetEveningLightDirection();
	SAILOR_SHARED_API glm::vec3 GetEveningLightIntensity();
	SAILOR_SHARED_API glm::vec3 GetReceiverEvidencePoint();
	SAILOR_SHARED_API glm::vec3 GetBounceCliffEvidencePoint();
	SAILOR_SHARED_API glm::vec3 GetCameraPosition();
	SAILOR_SHARED_API glm::vec3 GetCameraTarget();
}
