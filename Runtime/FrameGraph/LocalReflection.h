#pragma once

#include "RHI/Cubemap.h"
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

		bool IsValid() const
		{
			for (int axis = 0; axis < 4; ++axis)
			{
				if (!std::isfinite(m_positionBlend[axis]) ||
					!std::isfinite(m_minEnabled[axis]) || !std::isfinite(m_max[axis]))
				{
					return false;
				}
			}
			if (m_positionBlend.w <= 0.0f)
			{
				return false;
			}
			for (int axis = 0; axis < 3; ++axis)
			{
				if (m_minEnabled[axis] >= m_max[axis] ||
					m_positionBlend[axis] <= m_minEnabled[axis] ||
					m_positionBlend[axis] >= m_max[axis])
				{
					return false;
				}
			}
			return true;
		}
	};
	static_assert(sizeof(LocalReflectionParameters) == 48u);

	struct LocalReflectionCaptureRequest final
	{
		LocalReflectionParameters m_parameters{};
		uint32_t m_faceSize = 128u;
		float m_nearPlane = 0.1f;
		float m_farPlane = 1000.0f;

		bool IsValid() const
		{
			return m_parameters.IsValid() &&
				m_faceSize >= 16u && m_faceSize <= 512u &&
				(m_faceSize & (m_faceSize - 1u)) == 0u &&
				std::isfinite(m_nearPlane) && std::isfinite(m_farPlane) &&
				m_nearPlane > 0.0f && m_farPlane > m_nearPlane;
		}
	};

	struct LocalReflectionCubemap final
	{
		LocalReflectionParameters m_parameters{};
		// The producer completes all six faces and the mip chain before handing this off.
		RHI::RHICubemapPtr m_cubemap;
		uint64_t m_generation = 0u;

		bool IsValid() const
		{
			if (!m_parameters.IsValid() || !m_cubemap || !m_cubemap->IsReady() || m_generation == 0u)
			{
				return false;
			}
			const auto extent = m_cubemap->GetExtent();
			if (extent.x < 16 || extent.x > 512 || extent.y != extent.x ||
				(extent.x & (extent.x - 1)) != 0 ||
				(m_cubemap->GetFormat() != RHI::EFormat::R16G16B16A16_SFLOAT &&
					m_cubemap->GetFormat() != RHI::EFormat::R32G32B32A32_SFLOAT))
			{
				return false;
			}
			uint32_t levels = 1u;
			for (uint32_t size = static_cast<uint32_t>(extent.x); size > 1u; size >>= 1u)
			{
				++levels;
			}
			return m_cubemap->GetMipLevels() == levels;
		}
	};
}
