#pragma once

#include "Memory/LockFreeHeapAllocator.h"
#include "Containers/Vector.h"
#include <glm/glm.hpp>
#include <cmath>

namespace Sailor::Framegraph
{
	// One finite, box-projected scene capture. This is outgoing scene radiance,
	// not an incident environment for the path tracer or diffuse GI.
	struct LocalReflectionParameters final
	{
		glm::vec4 m_positionBlend{};
		glm::vec4 m_minEnabled{};
		glm::vec4 m_max{};
	};
	static_assert(sizeof(LocalReflectionParameters) == 48u);

	struct LocalReflectionImage final
	{
		LocalReflectionParameters m_parameters{};
		glm::uvec2 m_extent{};
		uint32_t m_samplesPerPixel = 0u;
		TVector<glm::vec4> m_pixels{};

		bool IsValid() const
		{
			if (m_extent.x < 4u || m_extent.x > 1024u ||
				(m_extent.x & 1u) != 0u || m_extent.y != m_extent.x / 2u ||
				m_pixels.Num() != size_t(m_extent.x) * m_extent.y)
			{
				return false;
			}
			for (int axis = 0; axis < 4; ++axis)
			{
				if (!std::isfinite(m_parameters.m_positionBlend[axis]) ||
					!std::isfinite(m_parameters.m_minEnabled[axis]) ||
					!std::isfinite(m_parameters.m_max[axis])) return false;
			}
			if (m_parameters.m_positionBlend.w <= 0.0f) return false;
			for (int axis = 0; axis < 3; ++axis)
			{
				if (m_parameters.m_minEnabled[axis] >= m_parameters.m_max[axis] ||
					m_parameters.m_positionBlend[axis] <= m_parameters.m_minEnabled[axis] ||
					m_parameters.m_positionBlend[axis] >= m_parameters.m_max[axis]) return false;
			}
			for (const auto& pixel : m_pixels)
				for (int axis = 0; axis < 4; ++axis)
					if (!std::isfinite(pixel[axis]) || pixel[axis] < 0.0f || pixel[axis] > 65504.0f) return false;
			return true;
		}
	};
}
