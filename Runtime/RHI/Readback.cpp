#include "Readback.h"
#include "RHI/Buffer.h"
#include "RHI/Fence.h"
#include <glm/gtc/packing.hpp>

using namespace Sailor::RHI;

EditorReadbackFrame::EditorReadbackFrame() = default;
EditorReadbackFrame::~EditorReadbackFrame() = default;

bool EditorReadbackFrame::PrepareBgraPixels()
{
	if (m_format == ETextureFormat::B8G8R8A8_UNORM || m_format == ETextureFormat::B8G8R8A8_SRGB)
	{
		m_bgraPixels.Clear(false);
		return true;
	}
	const bool halfFloat = m_format == ETextureFormat::R16G16B16A16_SFLOAT;
	if (!halfFloat && m_format != ETextureFormat::R8G8B8A8_UNORM && m_format != ETextureFormat::R8G8B8A8_SRGB)
	{
		return false;
	}

	const auto* pixels = static_cast<const uint8_t*>(m_buffer->GetPointer());
	const uint32_t destinationPitch = static_cast<uint32_t>(m_extent.x) * 4u;
	m_bgraPixels.Resize(static_cast<size_t>(destinationPitch) * m_extent.y);
	for (int y = 0; y < m_extent.y; ++y)
	{
		const uint8_t* src = pixels + static_cast<size_t>(y) * m_bytesPerRow;
		uint8_t* dst = m_bgraPixels.GetData() + static_cast<size_t>(y) * destinationPitch;
		for (int x = 0; x < m_extent.x; ++x, dst += 4)
		{
			if (halfFloat)
			{
				const auto* channels = reinterpret_cast<const uint16_t*>(src);
				const glm::vec4 values(glm::unpackHalf1x16(channels[2]), glm::unpackHalf1x16(channels[1]),
					glm::unpackHalf1x16(channels[0]), glm::unpackHalf1x16(channels[3]));
				const glm::u8vec4 bgra(glm::clamp(values, 0.0f, 1.0f) * 255.0f + 0.5f);
				dst[0] = bgra[0];
				dst[1] = bgra[1];
				dst[2] = bgra[2];
				dst[3] = bgra[3];
				src += 8;
			}
			else
			{
				dst[0] = src[2];
				dst[1] = src[1];
				dst[2] = src[0];
				dst[3] = src[3];
				src += 4;
			}
		}
	}
	return true;
}

const uint8_t* EditorReadbackFrame::GetBgraPixels() const
{
	return m_bgraPixels.IsEmpty() ? static_cast<const uint8_t*>(m_buffer->GetPointer()) : m_bgraPixels.GetData();
}

uint32_t EditorReadbackFrame::GetBgraBytesPerRow() const
{
	return m_bgraPixels.IsEmpty() ? m_bytesPerRow : static_cast<uint32_t>(m_extent.x) * 4u;
}
