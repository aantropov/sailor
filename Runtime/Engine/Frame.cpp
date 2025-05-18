#include "Frame.h"
#include "Core/Defines.h"
#include "Platform/Input.h"
#include "Core/Utils.h"
#include "RHI/Renderer.h"
#include "RHI/Mesh.h"
#include "RHI/Material.h"
#include "RHI/CommandList.h"
#include "RHI/GraphicsDriver.h"

#include "Engine/World.h"

using namespace Sailor;


Sailor::FrameState::FrameState() noexcept
{
	m_pData = TUniquePtr<FrameData>::Make();
}

Sailor::FrameState::FrameState(WorldPtr world, int64_t timeMs, const FrameInputState& currentInputState, const ivec2& centerPointViewport, const Sailor::FrameState* previousFrame) noexcept
	: Sailor::FrameState()
{
	SAILOR_PROFILE_FUNCTION();

	m_pData->m_inputState = currentInputState;
	m_pData->m_currentTime = timeMs;
	m_pData->m_mouseDelta = glm::ivec2(0, 0);
	m_pData->m_deltaTimeSeconds = 0;
	m_pData->m_mouseDeltaToCenter = m_pData->m_inputState.GetCursorPos() - centerPointViewport;
	m_pData->m_world = world;

	if (previousFrame != nullptr)
	{
		m_pData->m_mouseDelta = currentInputState.GetCursorPos() - previousFrame->GetInputState().GetCursorPos();
		m_pData->m_deltaTimeSeconds = (m_pData->m_currentTime - previousFrame->GetTime()) / 1000.0f;
		m_pData->m_inputState.TrackForChanges(previousFrame->GetInputState());
	}

	m_pData->m_drawImGui = nullptr;
}

Sailor::FrameState::FrameState(const Sailor::FrameState& frameState) noexcept :
	Sailor::FrameState()
{
	m_pData = TUniquePtr<FrameData>::Make(*frameState.m_pData);
}

Sailor::FrameState::FrameState(Sailor::FrameState&& frameState) noexcept
{
	std::swap(m_pData, frameState.m_pData);
}

Sailor::FrameState& Sailor::FrameState::operator=(Sailor::FrameState frameState)
{
	std::swap(m_pData, frameState.m_pData);
	return *this;
}

RHI::RHICommandListPtr Sailor::FrameState::CreateCommandBuffer(uint32_t index)
{
	auto cmdList = m_pData->m_updateResourcesCommandBuffers[index] = RHI::Renderer::GetDriver()->CreateCommandList(false, RHI::ECommandListQueue::Transfer);
	RHI::Renderer::GetDriver()->SetDebugName(cmdList, "World");

	return cmdList;
}

WorldPtr Sailor::FrameState::GetWorld() const
{
	return m_pData->m_world;
}