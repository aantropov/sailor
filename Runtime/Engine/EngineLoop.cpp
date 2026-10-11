#include "EngineLoop.h"
#include "RenderStats.h"
#include "Platform/Time.h"
#include "Core/Defines.h"
#include "Core/LogMacros.h"

#include "AssetRegistry/FrameGraph/FrameGraphImporter.h"

#include "Engine/GameObject.h"
#include "Components/CameraComponent.h"
#include "Components/EditorComponent.h"
#include "Submodules/ImGuiApi.h"
#include "RHI/Types.h"
#include "RHI/CommandList.h"
#include "RHI/Renderer.h"
#include "RHI/Texture.h"

#include <chrono>
#include <string>
#include <thread>

using namespace Sailor;

namespace
{
	void EnsureEditorWorldInfrastructure(const TSharedPtr<World>& world)
	{
		GameObjectPtr firstCamera;
		GameObjectPtr editorOwner;

		for (const auto& gameObject : world->GetGameObjects())
		{
			if (!gameObject.IsValid())
			{
				continue;
			}

			if (!firstCamera.IsValid() && gameObject->GetComponent<CameraComponent>().IsInited())
			{
				firstCamera = gameObject;
			}

			if (!editorOwner.IsValid() && gameObject->GetComponent<EditorComponent>().IsInited())
			{
				editorOwner = gameObject;
			}
		}

		if (editorOwner.IsValid())
		{
			if (!editorOwner->GetComponent<CameraComponent>().IsInited())
			{
				editorOwner->AddComponent<CameraComponent>();
			}
			return;
		}

		if (!firstCamera.IsValid())
		{
			firstCamera = world->Instantiate("Editor Camera");
			firstCamera->AddComponent<CameraComponent>();
		}

		firstCamera->AddComponent<EditorComponent>();
	}
}

TSharedPtr<World> EngineLoop::CreateEmptyWorld(std::string name, EWorldBehaviourMask mask)
{
	m_worlds.Emplace(TSharedPtr<World>::Make(std::move(name), mask));
	auto world = m_worlds[m_worlds.Num() - 1];

	auto gameObject = world->Instantiate();
	auto cameraComponent = gameObject->AddComponent<CameraComponent>();
	auto editorComponent = gameObject->AddComponent<EditorComponent>();

	return world;
}

TSharedPtr<World> EngineLoop::InstantiateWorld(WorldPrefabPtr worldPrefab, EWorldBehaviourMask mask)
{
	if (!worldPrefab || !worldPrefab->IsReady())
	{
		SAILOR_LOG_ERROR("Cannot instantiate an unavailable world prefab.");
		return {};
	}

	TSharedPtr<World> newWorld = TSharedPtr<World>::Make(
		worldPrefab->GetName(),
		mask);
	std::string globalIlluminationDiagnostic;
	if (!newWorld->SetGISettings(
			worldPrefab->GetGISettings(),
			globalIlluminationDiagnostic))
	{
		SAILOR_LOG_ERROR(
			"Failed to initialize Global Illumination ECS for world '%s': %s",
			worldPrefab->GetName().c_str(),
			globalIlluminationDiagnostic.c_str());
		return {};
	}

	for (const auto& prefab : worldPrefab->GetGameObjects())
	{
		if (!newWorld->Instantiate(prefab))
		{
			SAILOR_LOG_ERROR(
				"Failed to instantiate world '%s'; no partial world will be activated.",
				worldPrefab->GetName().c_str());
			newWorld->Clear();
			return {};
		}
	}

	if ((mask & EditorWorldMask) == EditorWorldMask)
	{
		EnsureEditorWorldInfrastructure(newWorld);
	}

	m_worlds.Emplace(newWorld);
	ProcessPendingDependencyResolution();

	return newWorld;
}

bool EngineLoop::ExitWorld(WorldPtr world)
{
	if (!world)
	{
		return false;
	}

	if (m_pendingWorldsToExit.Contains(world))
	{
		return true;
	}

	m_pendingWorldsToExit.Add(world);
	return true;
}

void EngineLoop::ProcessPendingWorldExits()
{
	auto renderer = App::GetSubmodule<RHI::Renderer>();

	for (auto* world : m_pendingWorldsToExit)
	{
		const size_t index = m_worlds.FindIf([&](const auto& it) { return it.GetRawPtr() == world; });
		if (index == -1)
		{
			continue;
		}

		if (renderer && renderer->IsInitialized())
		{
			renderer->WaitIdle();
			renderer->RemoveSceneView(world);
		}

		m_worlds[index]->Clear();
		m_worlds.RemoveAt(index);
	}

	m_pendingWorldsToExit.Clear();
}

void EngineLoop::ProcessPendingDependencyResolution()
{
	for (auto& world : m_worlds)
	{
		world->ResolveExternalDependencies();
	}
}

void EngineLoop::ProcessCpuFrame(FrameState& currentInputState)
{
	SAILOR_PROFILE_FUNCTION();

	static uint32_t totalFramesCount = 0U;
	static Utils::Timer timer;
	const auto cpuFrameStartedAt = std::chrono::steady_clock::now();

	timer.Start();

	App::GetSubmodule<ImGuiApi>()->NewFrame();
	ProcessPendingDependencyResolution();

	const auto world = GetWorld();
	check(currentInputState.GetWorld() == world.GetRawPtr());
	if (world) world->Tick(currentInputState);

	const auto renderer = App::GetSubmodule<RHI::Renderer>();
	if (renderer) DrawRenderStats(m_cpuFps, m_worlds, *renderer, App::GetRenderStatsMode());

	auto& task = currentInputState.GetDrawImGuiTask();
	RHI::EFormat imguiColorFormat = renderer ?
		renderer->GetColorFormat() :
		RHI::EFormat::B8G8R8A8_SRGB;
	if (renderer)
	{
		if (auto frameGraph = renderer->GetFrameGraph())
		{
			if (auto rhiFrameGraph = frameGraph->GetRHI())
			{
				if (const auto renderImGuiNode = rhiFrameGraph->GetGraphNode("RenderImGui"_h))
				{
					if (const auto colorAttachment = renderImGuiNode->GetResolvedAttachment("color"_h))
					{
						imguiColorFormat = colorAttachment->GetFormat();
					}
				}
			}
		}
	}

	ImGuiApi::PreparedFramePtr imguiFrame;
	{
		SAILOR_PROFILE_SCOPE("Record ImGui Update Command List");

		auto transferCmdList = currentInputState.CreateCommandBuffer(1);
		RHI::Renderer::GetDriver()->SetDebugName(transferCmdList, "ImGui Transfer CommandList"_h);
		RHI::Renderer::GetDriverCommands()->BeginCommandList(transferCmdList, true);
		imguiFrame = App::GetSubmodule<ImGuiApi>()->PrepareFrame(transferCmdList);
		RHI::Renderer::GetDriverCommands()->EndCommandList(transferCmdList);
	}

	task = Tasks::CreateTaskWithResult<RHI::RHICommandListPtr>("Record ImGui Draw Command List"_h,
		[=]()
		{
			auto cmdList = RHI::Renderer::GetDriver()->CreateCommandList(true, RHI::ECommandListQueue::Graphics);
			RHI::Renderer::GetDriver()->SetDebugName(cmdList, "Record ImGui Draw Command List"_h);
			RHI::Renderer::GetDriverCommands()->BeginSecondaryCommandList(cmdList, false, false, imguiColorFormat);
			ImGuiApi::RenderFrame(imguiFrame, cmdList);
			RHI::Renderer::GetDriverCommands()->EndCommandList(cmdList);

			return cmdList;
		}, EThreadType::RHI);

	task->Run();

	if (m_fpsCap > 0u)
	{
		const auto targetCpuFrameTime =
			std::chrono::duration_cast<std::chrono::steady_clock::duration>(
				std::chrono::duration<double>(1.0 / static_cast<double>(m_fpsCap)));
		const auto cpuFrameDeadline = cpuFrameStartedAt + targetCpuFrameTime;
		if (std::chrono::steady_clock::now() < cpuFrameDeadline)
		{
			SAILOR_PROFILE_SCOPE("Sleep Main Thread to cap CPU FPS");
			std::this_thread::sleep_until(cpuFrameDeadline);
		}
	}

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
