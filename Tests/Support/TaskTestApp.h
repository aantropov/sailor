#pragma once

#include "Core/Submodule.h"

namespace Sailor::Tasks { class Scheduler; }
namespace Sailor { class AnimationAssetInfoHandler; class AnimationImporter; class AudioSystem; }

namespace Sailor::Tests
{
	// Windows imports this submodule's type ID from the runtime DLL.
	class SAILOR_SHARED_API GarbageCollectionProbe : public TSubmodule<GarbageCollectionProbe>
	{
	public:
		void CollectGarbage() override;

		uint64_t m_numCollections = 0;
	};

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
