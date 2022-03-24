#pragma once
#include <array>
#include "Engine/World.h"
#include "Core/Submodule.h"
#include "Memory/UniquePtr.hpp"
#include "Memory/SharedPtr.hpp"
#include "Frame.h"

namespace Sailor
{
	class EngineLoop : public TSubmodule<EngineLoop>
	{
	public:

		SAILOR_API void ProcessCpuFrame(FrameState& currentInputState);
		SAILOR_API uint32_t GetSmoothFps() const { return m_pureFps.load(); }

		EngineLoop() = default;
		SAILOR_API ~EngineLoop() override;

		SAILOR_API TSharedPtr<World> CreateWorld(std::string name);

	protected:

		std::atomic<uint32_t> m_pureFps = 0u;
		TVector<TSharedPtr<World>> m_worlds;
	};
}