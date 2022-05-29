#pragma once
#include "Frame.h"
#include "Core/Defines.h"
#include "Platform/Win32/Input.h"
#include "Math.h"
#include "Core/Utils.h"
#include "RHI/Renderer.h"
#include "RHI/Mesh.h"
#include "RHI/Material.h"
#include "RHI/GraphicsDriver.h"

#include "Engine/World.h"

using namespace Sailor;

FrameState::FrameState() noexcept
{
	m_pData = TUniquePtr<FrameData>::Make();
}

FrameState::FrameState(WorldPtr world, int64_t timeMs, const FrameInputState& currentInputState, const ivec2& centerPointViewport, const FrameState* previousFrame) noexcept
	: FrameState()
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
}

FrameState::FrameState(const FrameState& frameState) noexcept :
	FrameState()
{
	m_pData = TUniquePtr<FrameData>::Make(*frameState.m_pData);
}

FrameState::FrameState(FrameState&& frameState) noexcept
{
	std::swap(m_pData, frameState.m_pData);
}

FrameState& FrameState::operator=(FrameState frameState)
{
	std::swap(m_pData, frameState.m_pData);
	return *this;
}

RHI::RHICommandListPtr FrameState::CreateCommandBuffer(uint32_t index)
{
	return m_pData->m_updateResourcesCommandBuffers[index] = RHI::Renderer::GetDriver()->CreateCommandList(false, true);
}

WorldPtr FrameState::GetWorld() const
{
	return m_pData->m_world;
}