#pragma once

namespace Sailor::EditorRemote { struct MacRendererFrameSource; }

namespace Sailor::Tests
{
	void CheckMacReadbackPresentation(const EditorRemote::MacRendererFrameSource& source);
}
