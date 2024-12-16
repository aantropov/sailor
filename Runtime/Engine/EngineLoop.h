#pragma once
#include <array>
#include "Engine/World.h"
#include "Engine/Types.h"
#include "Core/Submodule.h"
#include "Memory/UniquePtr.hpp"
#include "Memory/SharedPtr.hpp"
#include "Frame.h"
#include "AssetRegistry/World/WorldPrefabImporter.h"

namespace Sailor
{
	class EngineLoop : public TSubmodule<EngineLoop>
	{
	public:
		static constexpr EWorldBehaviourMask DefaultWorldMask = (uint8_t)EWorldBehaviourBit::Tickable |
			(uint8_t)EWorldBehaviourBit::CallBeginPlay |
			(uint8_t)EWorldBehaviourBit::EcsTickable;

		static constexpr EWorldBehaviourMask EditorWorldMask = (uint8_t)EWorldBehaviourBit::EcsTickable |
			(uint8_t)EWorldBehaviourBit::EditorTick;

		const float MaxCpuFrames = 120.0f;

		SAILOR_API void ProcessCpuFrame(FrameState& currentInputState);
		SAILOR_API uint32_t GetCpuFps() const { return m_cpuFps; }

		EngineLoop() = default;
		SAILOR_API ~EngineLoop() override;

		SAILOR_API TSharedPtr<World> CreateEmptyWorld(std::string name, EWorldBehaviourMask mask);
		SAILOR_API TSharedPtr<World> InstantiateWorld(WorldPrefabPtr worldPrefab, EWorldBehaviourMask mask);

		SAILOR_API const TVector<TSharedPtr<World>>& GetWorlds() const { return m_worlds; }
		SAILOR_API TSharedPtr<World> GetWorld() const { return m_worlds[0]; }

	protected:

		uint32_t m_cpuFps = 0u;
		TVector<TSharedPtr<World>> m_worlds;
	};
}