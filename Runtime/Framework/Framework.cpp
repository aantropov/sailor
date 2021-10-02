#pragma once
#include "Framework.h"
#include "Defines.h"
#include "Platform/Win32/Input.h"
#include "Math.h"
#include "Utils.h"
#include "GfxDevice/Vulkan/VulkanCommandBuffer.h"

using namespace Sailor;

FrameState::FrameState() :
	m_currentTime(0),
	m_deltaTimeSeconds(0.0f),
	m_mouseDelta(0.0f, 0.0f)
{
}

FrameState::FrameState(int64_t timeMs, const FrameInputState& currentInputState, const ivec2& centerPointViewport, const FrameState* previousFrame)
{
	SAILOR_PROFILE_FUNCTION();

	m_inputState = currentInputState;
	m_currentTime = timeMs;
	m_mouseDelta = glm::ivec2(0, 0);
	m_deltaTimeSeconds = 0;
	m_mouseDeltaToCenter = m_inputState.GetCursorPos() - centerPointViewport;

	if (previousFrame != nullptr)
	{
		m_mouseDelta = currentInputState.GetCursorPos() - previousFrame->GetInputState().GetCursorPos();
		m_deltaTimeSeconds = (m_currentTime - previousFrame->GetTime()) / 1000.0f;
		m_inputState.TrackForChanges(previousFrame->GetInputState());
	}
}

void FrameState::AddCommandBuffer(TRefPtr<GfxDevice::Vulkan::VulkanCommandBuffer> commandBuffer)
{
	SAILOR_PROFILE_FUNCTION();
}

void Framework::Initialize()
{
	if (m_pInstance == nullptr)
	{
		m_pInstance = new Framework();
	}
}

void Framework::ProcessCpuFrame(FrameState& currentInputState)
{
	SAILOR_PROFILE_FUNCTION();

	static uint32_t totalFramesCount = 0U;
	static int64_t totalTime = 0;

	SAILOR_PROFILE_BLOCK("CPU Frame");
	CpuFrame();
	SAILOR_PROFILE_END_BLOCK();

	totalFramesCount++;

	if (Utils::GetCurrentTimeMicro() - totalTime > 1000000)
	{
		m_smoothFps = totalFramesCount;
		totalFramesCount = 0;
		totalTime = Utils::GetCurrentTimeMicro();
	}
}

void Framework::CpuFrame()
{
}