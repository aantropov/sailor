#pragma once
#include "RHI/Buffer.h"
#include "RHI/Fence.h"

namespace Sailor::RHI
{
	struct EditorReadbackFrame
	{
		RHIBufferPtr m_buffer{};
		RHIFencePtr m_completion{};
		glm::ivec2 m_extent{};
		ETextureFormat m_format = ETextureFormat::B8G8R8A8_UNORM;
		uint32_t m_bytesPerRow = 0;
		uint64_t m_frameIndex = 0;
		uint64_t m_generation = 0;
	};

	using EditorReadbackFramePtr = TSharedPtr<const EditorReadbackFrame>;
}
