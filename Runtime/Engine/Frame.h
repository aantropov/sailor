#pragma once
#include <array>
#include "Memory/RefPtr.hpp"
#include "Core/Submodule.h"
#include "Engine/Types.h"
#include "Memory/UniquePtr.hpp"
#include "Memory/SharedPtr.hpp"
#include "RHI/Renderer.h"
#include "RHI/DebugContext.h"
#include "Platform/Win32/Input.h"

namespace Sailor
{
	using FrameInputState = Sailor::Win32::InputState;

	class FrameState
	{
	public:
		static constexpr uint32_t NumCommandLists = 4u;

		SAILOR_API FrameState() noexcept;

		SAILOR_API FrameState(WorldPtr world,
			int64_t timeMs,
			const FrameInputState& currentInputState,
			const ivec2& centerPointViewport,
			const FrameState* previousFrame = nullptr)  noexcept;

		SAILOR_API FrameState(const FrameState& frameState) noexcept;
		SAILOR_API FrameState(FrameState&& frameState) noexcept;

		SAILOR_API FrameState& operator=(FrameState frameState);

		SAILOR_API virtual ~FrameState() = default;

		SAILOR_API glm::ivec2 GetMouseDelta() const { return m_pData->m_mouseDelta; }
		SAILOR_API glm::ivec2 GetMouseDeltaToCenterViewport() const { return m_pData->m_mouseDeltaToCenter; }

		SAILOR_API const FrameInputState& GetInputState() const { return m_pData->m_inputState; }
		SAILOR_API int64_t GetTime() const { return m_pData->m_currentTime; }
		SAILOR_API float GetDeltaTime() const { return m_pData->m_deltaTimeSeconds; }

		SAILOR_API RHI::RHICommandListPtr CreateCommandBuffer(uint32_t index);
		SAILOR_API RHI::RHICommandListPtr GetCommandBuffer(uint32_t index) { return m_pData->m_updateResourcesCommandBuffers[index]; }

		SAILOR_API size_t GetNumCommandLists() const { return NumCommandLists; }
		SAILOR_API WorldPtr GetWorld() const;

		SAILOR_API Tasks::TaskPtr<RHI::RHICommandListPtr, void>& GetDrawImGuiTask() { return m_pData->m_drawImGui; }
		SAILOR_API Tasks::TaskPtr<RHI::RHICommandListPtr, void> GetDrawImGuiTask() const { return m_pData->m_drawImGui; }

	protected:

		struct FrameData
		{
			int64_t m_currentTime = 0;
			float m_deltaTimeSeconds = 0.0f;
			glm::ivec2 m_mouseDelta{ 0.0f, 0.0f };
			glm::ivec2 m_mouseDeltaToCenter{ 0.0f,0.0f };
			FrameInputState m_inputState{};
			std::array<RHI::RHICommandListPtr, NumCommandLists> m_updateResourcesCommandBuffers{};
			Tasks::TaskPtr<RHI::RHICommandListPtr, void> m_drawImGui{};
			WorldPtr m_world;
		};

		TUniquePtr<FrameData> m_pData;
	};
}