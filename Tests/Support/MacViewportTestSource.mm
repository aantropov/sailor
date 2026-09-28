#import <Metal/Metal.h>

#include "MacViewportTestSource.h"

namespace Sailor::Tests
{
	using namespace EditorRemote;

	Failure UploadMacRendererPatternToIntermediateTexture(uintptr_t textureObject, uint32_t width,
		uint32_t height, const MacNativeBridgeProducerPattern& pattern)
	{
		if (!textureObject || width == 0 || height == 0)
		{
			return Failure::FromDomain(ErrorDomain::Session, 1, "test pattern requires a native texture and extents");
		}
		const uint32_t bytesPerRow = width * 4;
		std::vector<uint8_t> pixels(static_cast<size_t>(bytesPerRow) * height);
		const uint8_t seedA = static_cast<uint8_t>((pattern.m_frameIndex * 17u + pattern.m_generation * 13u) & 0xffu);
		const uint8_t seedB = static_cast<uint8_t>((pattern.m_epoch * 29u + pattern.m_viewportId * 7u) & 0xffu);
		const uint8_t seedC = static_cast<uint8_t>((pattern.m_width + pattern.m_height + pattern.m_frameIndex * 3u) & 0xffu);
		for (uint32_t y = 0; y < height; ++y)
		{
			for (uint32_t x = 0; x < width; ++x)
			{
				const size_t index = static_cast<size_t>(y) * bytesPerRow + static_cast<size_t>(x) * 4u;
				pixels[index] = static_cast<uint8_t>((x + seedA) & 0xffu);
				pixels[index + 1] = static_cast<uint8_t>((y + seedB) & 0xffu);
				pixels[index + 2] = seedC;
				pixels[index + 3] = 255;
			}
		}
		id<MTLTexture> texture = (id<MTLTexture>)textureObject;
		[texture replaceRegion:MTLRegionMake2D(0, 0, width, height) mipmapLevel:0 withBytes:pixels.data() bytesPerRow:bytesPerRow];
		return Failure::Ok();
	}

	Failure MacViewportTestSource::AcquireFrameSource(const MacViewportSurfaceState& state, FrameIndex frame, MacRendererFrameSource& out)
	{
		out = {};
		out.m_kind = MacRendererFrameSourceKind::SyntheticIntermediate;
		out.m_width = state.m_viewport.m_width;
		out.m_height = state.m_viewport.m_height;
		out.m_pixelFormat = state.m_viewport.m_pixelFormat;
		out.m_sourceToken = frame;
		out.m_releaseTextureObjectAfterUse = true;
		out.m_debugName = "InjectedTestPattern";
		auto result = CreateMacRendererIntermediateTexture(state.m_nativeAllocation->m_producerDeviceObject,
			out.m_width, out.m_height, out.m_pixelFormat, out.m_textureObject);
		if (!result.IsOk()) return result;
		return UploadMacRendererPatternToIntermediateTexture(out.m_textureObject, out.m_width, out.m_height,
			{ state.m_key.m_viewportId, state.m_key.m_epoch, state.m_key.m_generation, frame, out.m_width, out.m_height });
	}
}
