#pragma once
#include "Containers/Vector.h"
#include "RHI/Types.h"

namespace Sailor::RHI
{
	struct EditorReadbackFrame
	{
		SAILOR_API EditorReadbackFrame();
		SAILOR_API ~EditorReadbackFrame();
		EditorReadbackFrame(const EditorReadbackFrame&) = delete;
		EditorReadbackFrame& operator=(const EditorReadbackFrame&) = delete;

		RHIBufferPtr m_buffer{};
		RHIFencePtr m_completion{};
		glm::ivec2 m_extent{};
		ETextureFormat m_format = ETextureFormat::B8G8R8A8_UNORM;
		uint32_t m_bytesPerRow = 0;
		uint64_t m_frameIndex = 0;
		uint64_t m_generation = 0;
		TVector<uint8_t> m_bgraPixels;

		// Prepare once before publication; readers retain this frame and its ring slot.
		SAILOR_API bool PrepareBgraPixels();
		SAILOR_API const uint8_t* GetBgraPixels() const;
		SAILOR_API uint32_t GetBgraBytesPerRow() const;
	};

	using EditorReadbackFramePtr = TSharedPtr<const EditorReadbackFrame>;

	struct EditorReadbackStats
	{
		uint64_t m_bufferAllocatedBytes = 0;
		uint64_t m_recordedReadbackBytes = 0;
		uint64_t m_conversionAllocatedBytes = 0;
		uint64_t m_convertedBytes = 0;
	};
}
