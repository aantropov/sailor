#pragma once
#include <functional>

namespace Sailor::EditorRemote { struct MacRendererFrameSource; }

namespace Sailor::Tests
{
	void CheckMacReadbackPresentation(const EditorRemote::MacRendererFrameSource& source);
	void CheckMacAppViewportPump(const EditorRemote::MacRendererFrameSource& initial,
		const std::function<EditorRemote::MacRendererFrameSource()>& captureNextFrame);
}
