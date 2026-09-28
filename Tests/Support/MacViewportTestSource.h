#pragma once

#include "Submodules/EditorRemote/RemoteViewportMacTransport.h"

namespace Sailor::Tests
{
	inline EditorRemote::MacRendererFrameSource MakeMacReadbackSource(uint32_t width, uint32_t height, uint8_t value)
	{
		auto frame = TSharedPtr<RHI::EditorReadbackFrame>::Make();
		frame->m_extent = { width, height };
		frame->m_frameIndex = 37;
		frame->m_bytesPerRow = width * 4;
		frame->m_bgraPixels.Resize(static_cast<size_t>(width) * height * 4u);
		for (auto& byte : frame->m_bgraPixels) byte = value;
		EditorRemote::MacRendererFrameSource source;
		source.m_kind = EditorRemote::MacRendererFrameSourceKind::RendererOwnedRenderTargetMetadata;
		source.m_width = width;
		source.m_height = height;
		source.m_bytesPerRow = frame->m_bytesPerRow;
		source.m_pixelFormat = EditorRemote::PixelFormat::B8G8R8A8_UNorm;
		source.m_sourceToken = frame->m_frameIndex;
		source.m_readback = std::move(frame);
		return source;
	}

	struct MacNativeBridgeProducerPattern
	{
		EditorRemote::ViewportId m_viewportId = 0;
		EditorRemote::ConnectionEpoch m_epoch = 0;
		EditorRemote::SurfaceGeneration m_generation = 0;
		EditorRemote::FrameIndex m_frameIndex = 0;
		uint32_t m_width = 0;
		uint32_t m_height = 0;
	};

	EditorRemote::Failure UploadMacRendererPatternToIntermediateTexture(uintptr_t textureObject, uint32_t width,
		uint32_t height, const MacNativeBridgeProducerPattern& pattern);

	class MacViewportTestSource final : public EditorRemote::IMacRendererFrameSourceProvider
	{
	public:
		EditorRemote::Failure AcquireFrameSource(const EditorRemote::MacViewportSurfaceState& state,
			EditorRemote::FrameIndex frame, EditorRemote::MacRendererFrameSource& out) override;
	};
}
