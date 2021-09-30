#pragma once
#include <array>
#include "Core/RefPtr.hpp"
#include "Core/Singleton.hpp"
#include "RHI/RHIResource.h"
#include "Platform/Win32/Input.h"

using namespace Sailor;

namespace Sailor
{
	using FrameInputState = Sailor::Win32::InputState;

	class FrameState
	{
	public:

		SAILOR_API FrameState();
		SAILOR_API FrameState(float time, const FrameInputState& currentInputState, const FrameState* previousFrame = nullptr);
		SAILOR_API virtual ~FrameState() = default;

		SAILOR_API const FrameInputState& GetInputState() const { return m_inputState; }
		SAILOR_API float GetTime() const { return m_currentTime; }
		SAILOR_API float GetDelta() const { return m_deltaTime; }
		SAILOR_API void AddCommandBuffer(TRefPtr<RHI::Resource> commandBuffer);

	protected:

		float m_currentTime;
		float m_deltaTime;
		glm::ivec2 m_mouseDelta;
		FrameInputState m_inputState;

		std::vector<TRefPtr<RHI::Resource>> m_updateResourcesCommandBuffers;
	};

	class Framework : public TSingleton<Framework>
	{
	public:

		static SAILOR_API void Initialize();
		SAILOR_API void ProcessCpuFrame(FrameState& currentInputState);

		uint32_t SAILOR_API GetSmoothFps() const { return m_smoothFps.load(); }

		~Framework() override = default;

	protected:

		Framework() = default;

		std::atomic<uint32_t> m_smoothFps = 0u;
	};
}