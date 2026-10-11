#include "Readback.h"
#include "RHI/Buffer.h"
#include "Math/Math.h"
#include <cstring>
#include <glm/gtc/packing.hpp>

using namespace Sailor;
using namespace Sailor::RHI;

namespace
{
	glm::u8vec4 ReadRgba8(const uint8_t* src, ETextureFormat format)
	{
		if (format == ETextureFormat::B8G8R8A8_UNORM || format == ETextureFormat::B8G8R8A8_SRGB)
			return { src[2], src[1], src[0], src[3] };
		return { src[0], src[1], src[2], src[3] };
	}

	glm::vec4 ReadRgba16f(const uint8_t* src)
	{
		uint16_t channels[4];
		std::memcpy(channels, src, sizeof(channels));
		return { glm::unpackHalf1x16(channels[0]), glm::unpackHalf1x16(channels[1]),
			glm::unpackHalf1x16(channels[2]), glm::unpackHalf1x16(channels[3]) };
	}
}

uint32_t Sailor::RHI::GetReadbackPixelSize(ETextureFormat format)
{
	switch (format)
	{
	case ETextureFormat::B8G8R8A8_UNORM:
	case ETextureFormat::B8G8R8A8_SRGB:
	case ETextureFormat::R8G8B8A8_UNORM:
	case ETextureFormat::R8G8B8A8_SRGB: return 4;
	case ETextureFormat::R16G16B16A16_SFLOAT: return 8;
	default: return 0;
	}
}

ReadbackFrame::ReadbackFrame() = default;
ReadbackFrame::~ReadbackFrame() = default;

bool ReadbackFrame::PrepareBgraPixels()
{
	if (m_format == ETextureFormat::B8G8R8A8_UNORM || m_format == ETextureFormat::B8G8R8A8_SRGB)
	{
		m_bgraPixels.Clear(false);
		return true;
	}
	const uint32_t pixelSize = GetReadbackPixelSize(m_format);
	if (!pixelSize) return false;
	const auto* pixels = static_cast<const uint8_t*>(m_buffer->GetPointer());
	const uint32_t destinationPitch = static_cast<uint32_t>(m_extent.x) * 4u;
	m_bgraPixels.Resize(static_cast<size_t>(destinationPitch) * m_extent.y);
	for (int y = 0; y < m_extent.y; ++y)
	{
		const uint8_t* src = pixels + static_cast<size_t>(y) * m_bytesPerRow;
		uint8_t* dst = m_bgraPixels.GetData() + static_cast<size_t>(y) * destinationPitch;
		for (int x = 0; x < m_extent.x; ++x, src += pixelSize, dst += 4)
		{
			const auto rgba = pixelSize == 4 ? ReadRgba8(src, m_format) :
				glm::u8vec4(glm::clamp(ReadRgba16f(src), 0.0f, 1.0f) * 255.0f + 0.5f);
			dst[0] = rgba.b;
			dst[1] = rgba.g;
			dst[2] = rgba.r;
			dst[3] = rgba.a;
		}
	}
	return true;
}

bool ReadbackFrame::CopySrgbPixels(TVector<glm::u8vec4>& outPixels) const
{
	const uint32_t pixelSize = GetReadbackPixelSize(m_format);
	if (!pixelSize || !m_buffer || m_extent.x <= 0 || m_extent.y <= 0) return false;
	const size_t rowSize = static_cast<size_t>(m_extent.x) * pixelSize;
	if (m_bytesPerRow < rowSize || m_buffer->GetSize() < static_cast<size_t>(m_extent.y - 1) * m_bytesPerRow + rowSize) return false;
	const auto* pixels = static_cast<const uint8_t*>(m_buffer->GetPointer());
	if (!pixels) return false;
	const bool bIsSrgb = m_format == ETextureFormat::B8G8R8A8_SRGB || m_format == ETextureFormat::R8G8B8A8_SRGB;
	outPixels.Resize(static_cast<size_t>(m_extent.x) * m_extent.y);
	for (int y = 0; y < m_extent.y; ++y)
	{
		const uint8_t* src = pixels + static_cast<size_t>(y) * m_bytesPerRow;
		for (int x = 0; x < m_extent.x; ++x, src += pixelSize)
		{
			auto& pixel = outPixels[static_cast<size_t>(y) * m_extent.x + x];
			if (bIsSrgb) pixel = ReadRgba8(src, m_format);
			else
			{
				const auto rgba = pixelSize == 4 ? glm::vec4(ReadRgba8(src, m_format)) / 255.0f : ReadRgba16f(src);
				pixel = Utils::LinearToSRGB8(glm::clamp(rgba, 0.0f, 1.0f));
			}
		}
	}
	return true;
}

const uint8_t* ReadbackFrame::GetBgraPixels() const
{
	return m_bgraPixels.IsEmpty() ? static_cast<const uint8_t*>(m_buffer->GetPointer()) : m_bgraPixels.GetData();
}

uint32_t ReadbackFrame::GetBgraBytesPerRow() const
{
	return m_bgraPixels.IsEmpty() ? m_bytesPerRow : static_cast<uint32_t>(m_extent.x) * 4u;
}
