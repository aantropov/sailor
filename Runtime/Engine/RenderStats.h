#pragma once

#include "Containers/Vector.h"
#include "Memory/SharedPtr.hpp"
#include <cstdint>

namespace Sailor
{
	class World;
	namespace RHI { class Renderer; }
	namespace Settings { enum class ERenderStatsMode : uint8_t; }

	void DrawRenderStats(uint32_t cpuFps, const TVector<TSharedPtr<World>>& worlds,
		const RHI::Renderer& renderer, Settings::ERenderStatsMode mode);
}
