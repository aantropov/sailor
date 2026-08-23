#pragma once

#include "Core/Defines.h"
#include "Memory/LockFreeHeapAllocator.h"
#include "Containers/Vector.h"

namespace Sailor
{
	struct LandscapeGrassCandidate final
	{
		size_t m_chunkIndex = 0u;
		size_t m_profileIndex = 0u;
		uint32_t m_capacity = 0u;
		float m_priority = 1.0f;
		float m_distance = 0.0f;
		bool m_bResident = false;
		float m_residencyHysteresis = 0.0f;
	};

	struct LandscapeGrassSelection final
	{
		size_t m_chunkIndex = 0u;
		size_t m_profileIndex = 0u;
		uint32_t m_instanceCount = 0u;
	};

	SAILOR_API TVector<uint32_t> BuildLandscapeLodCoordinates(
		uint32_t resolution,
		uint32_t stride);
	SAILOR_API void AppendLandscapeLodIndices(
		uint32_t resolution,
		const TVector<uint32_t>& coordinates,
		bool bIncludeSkirts,
		TVector<uint32_t>& indices);

	SAILOR_API TVector<LandscapeGrassSelection> SelectLandscapeGrassResidency(
		TVector<LandscapeGrassCandidate> candidates,
		uint32_t instanceBudget);
}
