#include "EngineLoop.h"
#include "Core/Defines.h"

#include "AssetRegistry/Model/ModelImporter.h"
#include "AssetRegistry/Texture/TextureImporter.h"
#include "AssetRegistry/Material/MaterialImporter.h"

#include "Engine/GameObject.h"
#include "Components/MeshRendererComponent.h"
#include "Components/CameraComponent.h"
#include "Components/EditorComponent.h"
#include "ECS/TransformECS.h"
#include "Submodules/ImGuiApi.h"
#include "RHI/Types.h"
#include "RHI/CommandList.h"

#include <thread>
#include <chrono>

using namespace Sailor;

TSharedPtr<World> EngineLoop::CreateEmptyWorld(std::string name, EWorldBehaviourMask mask)
{
	m_worlds.Emplace(TSharedPtr<World>::Make(std::move(name), mask));

	auto gameObject = m_worlds[0]->Instantiate();
	auto cameraComponent = gameObject->AddComponent<CameraComponent>();
	auto testComponent = gameObject->AddComponent<EditorComponent>();

	return m_worlds[m_worlds.Num() - 1];
}

TSharedPtr<World> EngineLoop::InstantiateWorld(WorldPrefabPtr worldPrefab, EWorldBehaviourMask mask)
{
	TSharedPtr<World> newWorld = TSharedPtr<World>::Make(worldPrefab->GetName(), mask);

	check(worldPrefab && worldPrefab->IsReady());

	for (const auto& prefab : worldPrefab->GetGameObjects())
	{
		newWorld->Instantiate(prefab);
	}

	newWorld->ResolveExternalDependencies();

	m_worlds.Emplace(newWorld);

	return newWorld;
}

void EngineLoop::ProcessCpuFrame(FrameState& currentInputState)
{
	SAILOR_PROFILE_FUNCTION();

	static uint32_t totalFramesCount = 0U;
	static Utils::Timer timer;

	timer.Start();

	App::GetSubmodule<ImGuiApi>()->NewFrame();

	for (auto& world : m_worlds)
	{
		world->Tick(currentInputState);
	}

	auto& task = currentInputState.GetDrawImGuiTask();

	{
		SAILOR_PROFILE_SCOPE("Record ImGui Update Command List");

		auto transferCmdList = currentInputState.CreateCommandBuffer(1);
		RHI::Renderer::GetDriver()->SetDebugName(transferCmdList, "ImGui Transfer CommandList");
		RHI::Renderer::GetDriverCommands()->BeginCommandList(transferCmdList, true);
		App::GetSubmodule<ImGuiApi>()->PrepareFrame(transferCmdList);
		RHI::Renderer::GetDriverCommands()->EndCommandList(transferCmdList);
	}

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
		}, EThreadType::RHI);

	task->Run();

	timer.Stop();

	totalFramesCount++;

	const float TargetCpuTime = (1000.0f / 130);

	if (timer.ResultMs() < TargetCpuTime)
	{
		SAILOR_PROFILE_SCOPE("Sleep Main Thread to cap FPS (~120fps)");

		std::this_thread::sleep_for(std::chrono::milliseconds(std::max(1ull, (uint64_t)(TargetCpuTime - timer.ResultMs()))));
	}

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