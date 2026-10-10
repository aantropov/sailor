#pragma once
#include "Containers/Vector.h"
#include "RHI/Types.h"

namespace Sailor::RHI
{
	SAILOR_API uint32_t GetReadbackPixelSize(ETextureFormat format);

	struct ReadbackFrame
	{
		SAILOR_API ReadbackFrame();
		SAILOR_API ~ReadbackFrame();
		ReadbackFrame(const ReadbackFrame&) = delete;
		ReadbackFrame& operator=(const ReadbackFrame&) = delete;

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
		// Decode the original pixels, honoring format/pitch; sRGB formats stay encoded.
		SAILOR_API bool CopySrgbPixels(TVector<glm::u8vec4>& outPixels) const;
	};

	using ReadbackFramePtr = TSharedPtr<const ReadbackFrame>;

	struct EditorReadbackStats
	{
		uint64_t m_bufferAllocatedBytes = 0;
		uint64_t m_recordedReadbackBytes = 0;
		uint64_t m_conversionAllocatedBytes = 0;
		uint64_t m_convertedBytes = 0;
	};
}
