#pragma once

#include "Submodules/EditorRemote/RemoteViewportMacTransport.h"

namespace Sailor::Tests
{
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
