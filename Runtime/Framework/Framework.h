#pragma once
#include <array>
#include "Core/RefPtr.hpp"
#include "Core/Singleton.hpp"
#include "RHI/RHIResource.h"
#include "Platform/Win32/Input.h"

namespace Sailor::GfxDevice::Vulkan
{
	class VulkanCommandBuffer;
}

namespace Sailor
{
	using FrameInputState = Sailor::Win32::InputState;

	class FrameState
	{
	public:

		SAILOR_API FrameState();
		SAILOR_API FrameState(int64_t timeMs, const FrameInputState& currentInputState, const ivec2& centerPointViewport, const FrameState* previousFrame = nullptr);
		SAILOR_API virtual ~FrameState() = default;

		SAILOR_API glm::ivec2 GetMouseDelta() const { return m_mouseDelta; }
		SAILOR_API glm::ivec2 GetMouseDeltaToCenterViewport() const { return m_mouseDeltaToCenter; }

		SAILOR_API const FrameInputState& GetInputState() const { return m_inputState; }
		SAILOR_API int64_t GetTime() const { return m_currentTime; }
		SAILOR_API float GetDeltaTime() const { return m_deltaTimeSeconds; }
		SAILOR_API void AddCommandBuffer(TRefPtr<Sailor::GfxDevice::Vulkan::VulkanCommandBuffer> commandBuffer);

	protected:

		int64_t m_currentTime;
		float m_deltaTimeSeconds;
		glm::ivec2 m_mouseDelta;
		glm::ivec2 m_mouseDeltaToCenter;
		FrameInputState m_inputState;

		std::vector<TRefPtr<class GfxDevice::Vulkan::VulkanCommandBuffer>> m_updateResourcesCommandBuffers;
	};

	class Framework : public TSingleton<Framework>
	{
	public:

		static SAILOR_API void Initialize();

		SAILOR_API void ProcessCpuFrame(FrameState& currentInputState);
		SAILOR_API void CpuFrame();

		uint32_t SAILOR_API GetSmoothFps() const { return m_smoothFps.load(); }

		~Framework() override = default;

	protected:

		Framework() = default;
		std::atomic<uint32_t> m_smoothFps = 0u;
	};
}