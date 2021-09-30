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

		SAILOR_API FrameState() = default;
		SAILOR_API virtual ~FrameState() = default;

		SAILOR_API const FrameInputState& GetInputState() const { return m_inputState; }
		SAILOR_API void SetInputState(const FrameInputState& currentInputState, const FrameInputState& previousInputState);
		SAILOR_API void AddCommandBuffer(TRefPtr<RHI::Resource> commandBuffer);

		SAILOR_API void Clear();

	protected:

		float m_deltaTime;
		glm::ivec2 m_mouseDelta;
		FrameInputState m_inputState;

		std::vector<TRefPtr<RHI::Resource>> m_updateResourcesCommandBuffers;
	};

	class Framework : public TSingleton<Framework>
	{
	public:

		static SAILOR_API void Initialize();
		SAILOR_API void ProcessCpuFrame(const FrameInputState& currentInputState);

		uint32_t SAILOR_API GetSmoothFps() const { return m_smoothFps.load(); }

	private:

		FrameState m_frame;
		FrameState m_previousFrame;
		std::atomic<uint32_t> m_smoothFps = 0u;
	};
}