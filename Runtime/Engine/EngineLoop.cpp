#include "EngineLoop.h"
#include "Core/Defines.h"
#include "Core/LogMacros.h"

#include "AssetRegistry/Model/ModelImporter.h"
#include "AssetRegistry/Texture/TextureImporter.h"
#include "AssetRegistry/Material/MaterialImporter.h"
#include "AssetRegistry/FrameGraph/FrameGraphImporter.h"

#include "Engine/GameObject.h"
#include "Components/MeshRendererComponent.h"
#include "Components/CameraComponent.h"
#include "Components/EditorComponent.h"
#include "ECS/LightingECS.h"
#include "ECS/TransformECS.h"
#include "Submodules/ImGuiApi.h"
#include "RHI/Types.h"
#include "RHI/CommandList.h"
#include "RHI/Renderer.h"
#include "RHI/Texture.h"

#include <imgui.h>
#include <cstdio>
#include <thread>
#include <chrono>

using namespace Sailor;

namespace
{
	void DrawViewportStatsOverlay(
		uint32_t cpuFps,
		uint32_t gpuFps,
		uint32_t numBatches,
		uint32_t numInstances,
		float shadowMemoryMb,
		float csmShadowMemoryMb,
		float localShadowMemoryMb,
		float shadowMemoryBudgetMb,
		const RHI::Stats& stats)
	{
		const ImGuiIO& io = ImGui::GetIO();
		if (io.DisplaySize.x <= 0.0f || io.DisplaySize.y <= 0.0f)
		{
			return;
		}

		constexpr float BytesToMb = 1.0f / (1024.0f * 1024.0f);
		char text[512];
		std::snprintf(
			text,
			sizeof(text),
			"CPU %u FPS\nGPU %u FPS\nBatches %u\nInstances %u\n"
			"Shadows %.1f / %.0f MB\n  CSM %.1f MB\n  Local %.1f MB\n"
			"GPU memory\n  Materials %.1f MB\n  Textures %.1f MB\n  Meshes %.1f MB\n  General %.1f MB",
			cpuFps,
			gpuFps,
			numBatches,
			numInstances,
			shadowMemoryMb,
			shadowMemoryBudgetMb,
			csmShadowMemoryMb,
			localShadowMemoryMb,
			stats.m_materialsMemoryUsage.load(std::memory_order_relaxed) * BytesToMb,
			stats.m_texturesMemoryUsage.load(std::memory_order_relaxed) * BytesToMb,
			stats.m_meshesMemoryUsage.load(std::memory_order_relaxed) * BytesToMb,
			stats.m_generalMemoryUsage.load(std::memory_order_relaxed) * BytesToMb);

		constexpr float Margin = 10.0f;
		const ImVec2 textSize = ImGui::CalcTextSize(text);
		const ImVec2 position(
			std::max(Margin, io.DisplaySize.x - textSize.x - Margin),
			Margin);

		ImGui::GetForegroundDrawList()->AddText(position, IM_COL32(160, 160, 160, 255), text);
	}

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

	TSharedPtr<World> newWorld = TSharedPtr<World>::Make(worldPrefab->GetName(), mask);

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

	timer.Start();

	App::GetSubmodule<ImGuiApi>()->NewFrame();
	ProcessPendingDependencyResolution();

	for (auto& world : m_worlds)
	{
		world->Tick(currentInputState);
	}

	const auto renderer = App::GetSubmodule<RHI::Renderer>();
	if (renderer)
	{
		float shadowMemoryMb = 0.0f;
		float csmShadowMemoryMb = 0.0f;
		float localShadowMemoryMb = 0.0f;
		float shadowMemoryBudgetMb = 0.0f;
		for (const auto& world : m_worlds)
		{
			if (auto lighting = world->GetECS<LightingECS>())
			{
				shadowMemoryMb += lighting->GetShadowsOccupiedMemoryMb();
				csmShadowMemoryMb += lighting->GetCsmShadowsOccupiedMemoryMb();
				localShadowMemoryMb += lighting->GetLocalShadowsOccupiedMemoryMb();
				shadowMemoryBudgetMb += lighting->GetShadowsMemoryBudgetMb();
			}
		}

		const auto& stats = renderer->GetStats();
		DrawViewportStatsOverlay(
			m_cpuFps,
			stats.m_gpuFps.load(std::memory_order_relaxed),
			stats.m_numBatches.load(std::memory_order_relaxed),
			stats.m_numInstances.load(std::memory_order_relaxed),
			shadowMemoryMb,
			csmShadowMemoryMb,
			localShadowMemoryMb,
			shadowMemoryBudgetMb,
			stats);
	}

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
				if (const auto renderImGuiNode = rhiFrameGraph->GetGraphNode("RenderImGui"))
				{
					if (const auto colorAttachment = renderImGuiNode->GetResolvedAttachment("color"))
					{
						imguiColorFormat = colorAttachment->GetFormat();
					}
				}
			}
		}
	}

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
			RHI::Renderer::GetDriverCommands()->BeginSecondaryCommandList(cmdList, false, false, imguiColorFormat);
			App::GetSubmodule<ImGuiApi>()->RenderFrame(cmdList);
			RHI::Renderer::GetDriverCommands()->EndCommandList(cmdList);

			return cmdList;
		}, EThreadType::RHI);

	task->Run();

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
