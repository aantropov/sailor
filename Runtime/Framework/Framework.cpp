#pragma once
#include "Framework.h"
#include "Defines.h"
#include "Platform/Win32/Input.h"
#include "Math.h"

using namespace Sailor;

void FrameState::SetInputState(const FrameInputState& currentInputState, const FrameInputState& previousInputState)
{
	SAILOR_PROFILE_FUNCTION();
	m_inputState = currentInputState;
	m_mouseDelta.x = currentInputState.m_cursorPosition[0] - previousInputState.m_cursorPosition[0];
	m_mouseDelta.y = currentInputState.m_cursorPosition[1] - previousInputState.m_cursorPosition[1];
}

void FrameState::AddCommandBuffer(TRefPtr<RHI::RHIResource> commandBuffer)
{
	SAILOR_PROFILE_FUNCTION();
}

void FrameState::Clear()
{
	m_updateResourcesCommandBuffers.clear();
}

void Framework::Initialize()
{
	m_pInstance = new Framework;
}

void Framework::ProcessCpuFrame(const FrameInputState& currentInputState)
{
	SAILOR_PROFILE_FUNCTION();

	static float totalFramesCount = 0.0f;
	static float totalTime = 0.0f;

	const float beginFrameTime = (float)GetTickCount();

	SAILOR_PROFILE_BLOCK("CPU Frame");
	Sleep(1);
	SAILOR_PROFILE_END_BLOCK();

	totalTime += (float)GetTickCount() - beginFrameTime;
	totalFramesCount++;

	if (totalTime > 1000)
	{
		m_smoothFps = (uint32_t)totalFramesCount;
		totalFramesCount = 0;
		totalTime = 0;
	}
}
