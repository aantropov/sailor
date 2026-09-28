#pragma once
#include <cstdint>
#include <functional>

namespace Sailor::EditorRemote { struct MacRendererFrameSource; }

namespace Sailor::Tests
{
	void CheckMacReadbackPresentation(const EditorRemote::MacRendererFrameSource& source);
	void CheckMacVulkanTexturePresentation(uintptr_t texture, uint32_t width, uint32_t height, uint32_t expectedPixel);
	void CheckMacAppViewportPump(const EditorRemote::MacRendererFrameSource& initial,
		const std::function<EditorRemote::MacRendererFrameSource()>& captureNextFrame);
}
