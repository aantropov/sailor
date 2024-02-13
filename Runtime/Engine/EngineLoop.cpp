#pragma once
#include "EngineLoop.h"
#include "Core/Defines.h"

#include "AssetRegistry/Model/ModelImporter.h"
#include "AssetRegistry/Texture/TextureImporter.h"
#include "AssetRegistry/Material/MaterialImporter.h"

#include "Engine/GameObject.h"
#include "Components/MeshRendererComponent.h"
#include "Components/CameraComponent.h"
#include "Components/TestComponent.h"
#include "ECS/TransformECS.h"
#include "Submodules/ImGuiApi.h"
#include "RHI/Types.h"
#include "RHI/CommandList.h"

using namespace Sailor;

TSharedPtr<World> EngineLoop::CreateWorld(std::string name)
{
	m_worlds.Emplace(TSharedPtr<World>::Make(std::move(name)));

	auto gameObject = m_worlds[0]->Instantiate();
	auto cameraComponent = gameObject->AddComponent<CameraComponent>();
	auto testComponent = gameObject->AddComponent<TestComponent>();

	return m_worlds[m_worlds.Num() - 1];
}

void EngineLoop::ProcessCpuFrame(FrameState& currentInputState)
{
	SAILOR_PROFILE_FUNCTION();

	static uint32_t totalFramesCount = 0U;
	static Utils::Timer timer;

	timer.Start();
	SAILOR_PROFILE_BLOCK("CPU Frame");
	App::GetSubmodule<ImGuiApi>()->NewFrame();

	for (auto& world : m_worlds)
	{
		world->Tick(currentInputState);
	}

	auto& task = currentInputState.GetDrawImGuiTask();

	SAILOR_PROFILE_BLOCK("Record ImGui Update Command List");

	auto transferCmdList = currentInputState.CreateCommandBuffer(1);
	RHI::Renderer::GetDriver()->SetDebugName(transferCmdList, "ImGui Transfer CommandList");
	RHI::Renderer::GetDriverCommands()->BeginCommandList(transferCmdList, true);
	App::GetSubmodule<ImGuiApi>()->PrepareFrame(transferCmdList);
	RHI::Renderer::GetDriverCommands()->EndCommandList(transferCmdList);

	SAILOR_PROFILE_END_BLOCK();

	task = Tasks::CreateTaskWithResult<RHI::RHICommandListPtr>("Record ImGui Draw Command List",
		[=]()
		{
			auto cmdList = RHI::Renderer::GetDriver()->CreateCommandList(true, RHI::ECommandListQueue::Graphics);			
			RHI::Renderer::GetDriver()->SetDebugName(cmdList, "Record ImGui Draw Command List");
			// Default swapchain format
			RHI::Renderer::GetDriverCommands()->BeginSecondaryCommandList(cmdList, false, false, RHI::EFormat::B8G8R8A8_SRGB);
			App::GetSubmodule<ImGuiApi>()->RenderFrame(cmdList);
			RHI::Renderer::GetDriverCommands()->EndCommandList(cmdList);

			return cmdList;
		}, Tasks::EThreadType::RHI);

	task->Run();

	SAILOR_PROFILE_END_BLOCK();

	timer.Stop();

	totalFramesCount++;

	if (timer.ResultAccumulatedMs() > 1000)
	{
		m_cpuFps = totalFramesCount;
		totalFramesCount = 0;
		timer.Clear();
	}
}

EngineLoop::~EngineLoop()
{
	for (auto& world : m_worlds)
	{
		world->Clear();
	}
}