#pragma once
#include "Framework.h"
#include "Defines.h"
#include "Platform/Win32/Input.h"
#include "Math.h"
#include "Utils.h"
#include "RHI/Renderer.h"
#include "RHI/Mesh.h"
#include "RHI/GfxDevice.h"
#include "AssetRegistry/ModelAssetInfo.h"
#include "AssetRegistry/ModelImporter.h"

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

void FrameState::PushCommandBuffer_ThreadSafe(RHI::CommandListPtr commandBuffer)
{
	SAILOR_PROFILE_FUNCTION();
	std::unique_lock<std::mutex> lk(m_commandBuffers);
	m_updateResourcesCommandBuffers.push_back(commandBuffer);
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
	static Utils::Timer timer;

	timer.Start();

	SAILOR_PROFILE_BLOCK("CPU Frame");
	CpuFrame();
	SAILOR_PROFILE_END_BLOCK();

	timer.Stop();

	totalFramesCount++;

	if (timer.ResultAccumulatedMs() > 1000)
	{
		m_pureFps = totalFramesCount;
		totalFramesCount = 0;
		timer.Clear();
	}
}

void Framework::CpuFrame()
{
	static bool bFirstFrame = true;

	if (bFirstFrame)
	{
		static std::vector<RHI::Vertex> g_testVertices =
		{
			{{-0.5f, 0.5f, -0.5f}, {1.0f, 1.0f}, {1.0f, 0.0f, 0.0f, 1.0f}},
			{{0.5f, 0.5f, -0.5f}, {0.0f, 1.0f}, {0.3f, 1.0f, 0.0f, 1.0f}},
			{{-0.5f, -0.5f, -0.5f}, {1.0f, 0.0f}, {0.0f, 0.0f, 0.5f, 1.0f}},
			{{0.5f, -0.5f, -0.5f}, {0.0f, 0.0f}, {0.0f, 1.0f, 1.0f, 1.0f}},

			{{-0.5f, 0.5f, 0.5f}, {1.0f, 1.0f}, {1.0f, 0.0f, 0.0f, 1.0f}},
			{{0.5f, 0.5f, 0.5f}, {0.0f, 1.0f}, {0.3f, 1.0f, 0.0f, 1.0f}},
			{{-0.5f, -0.5f, 0.5f}, {1.0f, 0.0f}, {0.0f, 0.0f, 0.5f, 1.0f}},
			{{0.5f, -0.5f, 0.5f}, {0.0f, 0.0f}, {0.0f, 1.0f, 1.0f, 1.0f}}
		};

		static std::vector<uint32_t> g_testIndices =
		{
		   0, 1, 2,    // side 1
			2, 1, 3,
			4, 0, 6,    // side 2
			6, 0, 2,
			7, 5, 6,    // side 3
			6, 5, 4,
			3, 1, 7,    // side 4
			7, 1, 5,
			4, 5, 0,    // side 5
			0, 5, 1,
			3, 7, 2,    // side 6
			2, 7, 6,
		};

		TSharedPtr<JobSystem::Job> jobLoadModel;

		if (auto modelUID = AssetRegistry::GetInstance()->GetAssetInfo<ModelAssetInfo>("Models\\Sponza\\sponza.obj"))
		{
			jobLoadModel = ModelImporter::GetInstance()->LoadModel(modelUID->GetUID(), g_testVertices, g_testIndices);
		}

		auto jobCreateBuffers = JobSystem::Scheduler::CreateJob("Create buffers",
			[this]()
		{
			m_testMesh = Sailor::RHI::Renderer::GetDriver()->CreateMesh(g_testVertices, g_testIndices);
		});

		jobCreateBuffers->Join(jobLoadModel);
		JobSystem::Scheduler::GetInstance()->Run(jobCreateBuffers);
		bFirstFrame = false;
	}
}