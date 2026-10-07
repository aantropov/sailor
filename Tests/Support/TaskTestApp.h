#pragma once

#include "Core/Defines.h"

namespace Sailor::Tasks { class Scheduler; }
namespace Sailor { class AnimationAssetInfoHandler; class AnimationImporter; class AudioSystem; }

namespace Sailor::Tests
{
	// Use the real App submodule lookup without starting a window or renderer.
	class SAILOR_SHARED_API TaskTestApp final
	{
	public:
		TaskTestApp();
		~TaskTestApp();

		TaskTestApp(const TaskTestApp&) = delete;
		TaskTestApp& operator=(const TaskTestApp&) = delete;

		Tasks::Scheduler& GetScheduler() const;
		AnimationImporter& AddAnimationImporter(AnimationAssetInfoHandler& handler);
		AudioSystem& AddAudioSystem();
	};
}
