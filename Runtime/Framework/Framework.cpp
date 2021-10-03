#pragma once
#include "Framework.h"
#include "Defines.h"
#include "Platform/Win32/Input.h"
#include "Math.h"
#include "Utils.h"
#include "GfxDevice/Vulkan/VulkanCommandBuffer.h"

using namespace Sailor;

FrameState::FrameState() noexcept
{
	m_pData = TUniquePtr<FrameData>::Make();
}

FrameState::FrameState(int64_t timeMs, const FrameInputState& currentInputState, const ivec2& centerPointViewport, const FrameState* previousFrame) noexcept
	: FrameState()
{
	SAILOR_PROFILE_FUNCTION();

	m_pData->m_inputState = currentInputState;
	m_pData->m_currentTime = timeMs;
	m_pData->m_mouseDelta = glm::ivec2(0, 0);
	m_pData->m_deltaTimeSeconds = 0;
	m_pData->m_mouseDeltaToCenter = m_pData->m_inputState.GetCursorPos() - centerPointViewport;

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
	m_pData = std::move(frameState.m_pData);
}

FrameState& FrameState::operator=(FrameState frameState)
{
	m_pData = std::move(frameState.m_pData);
	return *this;
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