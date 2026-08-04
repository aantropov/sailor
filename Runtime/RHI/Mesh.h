#pragma once
#include <limits>

#include "Memory/RefPtr.hpp"
#include "Types.h"
#include "Math/Math.h"
#include "Math/Bounds.h"

namespace Sailor::RHI
{
	class RHIMesh : public RHIResource, public IDelayedInitialization
	{
	public:

		SAILOR_API uint32_t GetIndexCount() const;
		SAILOR_API uint32_t GetFirstIndex() const;
		SAILOR_API uint32_t GetVertexOffset() const;
		SAILOR_API size_t ResolveMaterialIndex(
			size_t meshIndex,
			size_t numMaterials) const;

		RHIBufferPtr m_vertexBuffer{};
		RHIBufferPtr m_indexBuffer{};
		RHIVertexDescriptionPtr m_vertexDescription{};
		Math::AABB m_bounds{};
		uint32_t m_materialIndex = (std::numeric_limits<uint32_t>::max)();
		glm::vec3 m_bakedVolumeScale{ 1.0f };

		SAILOR_API virtual bool IsReady() const override;

	protected:

	};
};
