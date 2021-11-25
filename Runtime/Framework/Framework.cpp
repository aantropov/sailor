#pragma once
#include "Framework.h"
#include "Defines.h"
#include "Platform/Win32/Input.h"
#include "Math.h"
#include "Utils.h"
#include "RHI/Renderer.h"
#include "RHI/Mesh.h"
#include "RHI/Material.h"
#include "RHI/GfxDevice.h"
#include "AssetRegistry/ModelAssetInfo.h"
#include "AssetRegistry/ModelImporter.h"
#include "AssetRegistry/TextureImporter.h"
#include "AssetRegistry/MaterialImporter.h"

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
	std::swap(m_pData, frameState.m_pData);
}

FrameState& FrameState::operator=(FrameState frameState)
{
	std::swap(m_pData, frameState.m_pData);
	return *this;
}

RHI::CommandListPtr FrameState::CreateCommandBuffer(uint32_t index)
{
	return m_pData->m_updateResourcesCommandBuffers[index] = Renderer::GetDriver()->CreateCommandList(false, true);
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
	CpuFrame(currentInputState);
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

void Framework::CpuFrame(FrameState& state)
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

		if (auto modelUID = AssetRegistry::GetInstance()->GetAssetInfoPtr<ModelAssetInfoPtr>("Models/Sponza/sponza.obj"))
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

		if (auto materialUID = AssetRegistry::GetInstance()->GetAssetInfoPtr<AssetInfoPtr>("Models/Sponza/sponza.mat"))
		{
			m_testMaterial = MaterialImporter::LoadMaterial(materialUID->GetUID());
		}

		m_testBinding = Sailor::RHI::Renderer::GetDriver()->CreateShaderBindings();
		Sailor::RHI::Renderer::GetDriver()->AddBufferToShaderBindings(m_testBinding, "data", sizeof(glm::mat4x4), 0, m_testMaterial->GetBindings()->NeedsStorageBuffer() ? EShaderBindingType::StorageBuffer : EShaderBindingType::UniformBuffer);

		m_frameDataBinding = Sailor::RHI::Renderer::GetDriver()->CreateShaderBindings();
		Sailor::RHI::Renderer::GetDriver()->AddBufferToShaderBindings(m_frameDataBinding, "frameData", sizeof(RHI::UboFrameData), 0, EShaderBindingType::UniformBuffer);

		if (auto textureUID = AssetRegistry::GetInstance()->GetAssetInfoPtr<AssetInfoPtr>("Textures/VulkanLogo.png"))
		{
			Sailor::RHI::Renderer::GetDriver()->AddSamplerToShaderBindings(m_frameDataBinding, "g_defaultSampler", TextureImporter::GetInstance()->LoadTexture(textureUID->GetUID()), 1);
		}

		bFirstFrame = false;
	}

	static glm::vec3 cameraPosition = Math::vec3_Forward * -10.0f;
	static glm::vec3 cameraViewDir = Math::vec3_Forward;

	const float sensitivity = 500;

	glm::vec3 delta = glm::vec3(0.0f, 0.0f, 0.0f);
	if (state.GetInputState().IsKeyDown('A'))
		delta += -cross(cameraViewDir, Math::vec3_Up);

	if (state.GetInputState().IsKeyDown('D'))
		delta += cross(cameraViewDir, Math::vec3_Up);

	if (state.GetInputState().IsKeyDown('W'))
		delta += cameraViewDir;

	if (state.GetInputState().IsKeyDown('S'))
		delta += -cameraViewDir;

	if (glm::length(delta) > 0)
		cameraPosition += glm::normalize(delta) * sensitivity * state.GetDeltaTime();

	const float speed = 50.0f;

	vec2 shift{};
	shift.x += (state.GetMouseDeltaToCenterViewport().x) * state.GetDeltaTime() * speed;
	shift.y += (state.GetMouseDeltaToCenterViewport().y) * state.GetDeltaTime() * speed;

	if (glm::length(shift) > 0.0f && state.GetInputState().IsKeyDown(VK_LBUTTON))
	{
		glm::quat hRotation = angleAxis(-glm::radians(shift.x), Math::vec3_Up);
		glm::quat vRotation = angleAxis(glm::radians(shift.y), cross(Math::vec3_Up, cameraViewDir));

		cameraViewDir = vRotation * cameraViewDir;
		cameraViewDir = hRotation * cameraViewDir;
	}

	auto width = Sailor::EngineInstance::GetViewportWindow().GetWidth();
	auto height = Sailor::EngineInstance::GetViewportWindow().GetHeight();

	float aspect = (height + width) > 0 ? width / (float)height : 1.0f;
	m_frameData.m_projection = glm::perspective(glm::radians(90.0f), aspect, 0.1f, 10000.0f);
	m_frameData.m_projection[1][1] *= -1;
	m_frameData.m_currentTime = (float)state.GetTime();
	m_frameData.m_deltaTime = state.GetDeltaTime();
	m_frameData.m_view = glm::lookAt(cameraPosition, cameraPosition + cameraViewDir, Math::vec3_Up);

	glm::mat4x4 model = glm::rotate(glm::mat4(1.0f), glm::radians(90.0f), Math::vec3_Up);

	state.PushFrameBinding(m_frameDataBinding);

	if (state.GetInputState().IsKeyDown(VK_SPACE))
	{
		if (auto materialUID = AssetRegistry::GetInstance()->GetAssetInfoPtr<AssetInfoPtr>("Models/Sponza/sponza.mat"))
		{
			m_testMaterial = MaterialImporter::LoadMaterial(materialUID->GetUID());
		}
	}

	SAILOR_PROFILE_BLOCK("Test Performance");

	auto pJob = JobSystem::Scheduler::GetInstance()->CreateJob("Update command list",
		[&state, this, model]()
		{
			auto pCommandList = state.CreateCommandBuffer(0);
			Renderer::GetDriverCommands()->BeginCommandList(pCommandList);
			RHI::Renderer::GetDriverCommands()->UpdateShaderBinding(pCommandList, m_frameDataBinding->GetOrCreateShaderBinding("frameData"), &m_frameData, sizeof(m_frameData));
			RHI::Renderer::GetDriverCommands()->UpdateShaderBinding(pCommandList, m_testBinding->GetOrCreateShaderBinding("data"), &model, sizeof(model));
			RHI::Renderer::GetDriverCommands()->SetMaterialParameter(pCommandList, m_testMaterial, "material.color", std::max(0.5f, float(sin(0.001 * (double)state.GetTime()))) * glm::vec4(1.0, 1.0, 1.0, 1.0f));
			Renderer::GetDriverCommands()->EndCommandList(pCommandList);
		});

	JobSystem::Scheduler::GetInstance()->Run(pJob);

	pJob->Wait();

	SAILOR_PROFILE_END_BLOCK();
}