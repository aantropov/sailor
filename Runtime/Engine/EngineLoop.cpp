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
#include "RHI/GpuFrameTimeQueryRing.h"
#include "RHI/Renderer.h"
#include "RHI/Texture.h"
#include "Settings/GraphicsSettings.h"

#include <imgui.h>
#include <chrono>
#include <cstdio>
#include <limits>
#include <string>
#include <thread>

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
		const RHI::RHIGlobalIlluminationRenderStats& globalIlluminationStats,
		const RHI::Stats& stats,
		const char* gpuQueryText)
	{
		const ImGuiIO& io = ImGui::GetIO();
		if (io.DisplaySize.x <= 0.0f || io.DisplaySize.y <= 0.0f)
		{
			return;
		}

		constexpr float BytesToMb = 1.0f / (1024.0f * 1024.0f);
		constexpr float BytesToKb = 1.0f / 1024.0f;
		const uint32_t globalIlluminationFlightSlot =
			globalIlluminationStats.m_flightSlot;
		char globalIlluminationFlight[16];
		if (globalIlluminationFlightSlot ==
			(std::numeric_limits<uint32_t>::max)())
		{
			std::snprintf(
				globalIlluminationFlight,
				sizeof(globalIlluminationFlight),
				"-");
		}
		else
		{
			std::snprintf(
				globalIlluminationFlight,
				sizeof(globalIlluminationFlight),
				"%u",
				globalIlluminationFlightSlot);
		}

		char text[2048];
		const char* globalIlluminationStatus =
			!globalIlluminationStats.m_bEnabled
				? "disabled"
				: globalIlluminationStats.m_mode ==
					EGlobalIlluminationMode::Realtime
					? "realtime"
					: globalIlluminationStats.m_bActive
						? "baked"
						: "fallback";
		const std::string globalIlluminationModeName(
			magic_enum::enum_name(globalIlluminationStats.m_mode));
		std::snprintf(
			text,
			sizeof(text),
			"CPU %u FPS\nGPU %u FPS\nBatches %u\nInstances %u\n"
			"Shadows %.1f / %.0f MB\n  CSM %.1f MB\n  Local %.1f MB\n"
			"GI %s (%s) rev %llu flight %s\n"
			"  States %u / %u, bricks %u / %u, probes %u\n"
			"  CPU payload %.2f MB, GPU/flight %.2f MB\n"
			"  Copy %.1f KB, upload %.1f KB\n"
			"GPU memory\n  Materials %.1f MB\n  Textures %.1f MB\n  Meshes %.1f MB\n  General %.1f MB%s%s",
			cpuFps,
			gpuFps,
			numBatches,
			numInstances,
			shadowMemoryMb,
			shadowMemoryBudgetMb,
			csmShadowMemoryMb,
			localShadowMemoryMb,
			globalIlluminationStatus,
			globalIlluminationModeName.c_str(),
			static_cast<unsigned long long>(
				globalIlluminationStats.m_activeRevision),
			globalIlluminationFlight,
			globalIlluminationStats.m_stateCount,
			globalIlluminationStats.m_qualityBudget,
			globalIlluminationStats.m_loadedBricks,
			globalIlluminationStats.m_totalBricks,
			globalIlluminationStats.m_probeCount,
			globalIlluminationStats.m_cpuPayloadBytes * BytesToMb,
			globalIlluminationStats.m_gpuAllocatedBytes * BytesToMb,
			globalIlluminationStats.m_copiedCpuBytes * BytesToKb,
			globalIlluminationStats.m_uploadedGpuBytes * BytesToKb,
			stats.m_materialsMemoryUsage.load(std::memory_order_relaxed) * BytesToMb,
			stats.m_texturesMemoryUsage.load(std::memory_order_relaxed) * BytesToMb,
			stats.m_meshesMemoryUsage.load(std::memory_order_relaxed) * BytesToMb,
			stats.m_generalMemoryUsage.load(std::memory_order_relaxed) * BytesToMb,
			gpuQueryText && gpuQueryText[0] != '\0' ? "\n" : "",
			gpuQueryText ? gpuQueryText : "");

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

	for (auto& world : m_worlds)
	{
		world->Tick(currentInputState);
	}

	const auto renderer = App::GetSubmodule<RHI::Renderer>();
	const Settings::ERenderStatsMode statsMode = App::GetRenderStatsMode();
	if (renderer && statsMode != Settings::ERenderStatsMode::None)
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
		const RHI::RHIGlobalIlluminationRenderStats globalIlluminationStats =
			renderer->GetGlobalIlluminationRenderStats();
		uint32_t displayedGpuFps =
			stats.m_gpuFps.load(std::memory_order_relaxed);
		std::string gpuQueryText;
		if (statsMode == Settings::ERenderStatsMode::RenderStatsAndQueries)
		{
			if (!renderer->GetDriver()->SupportsGpuFrameTimeQueries())
			{
				gpuQueryText = "GPU queries unavailable";
			}
			else
			{
				float gpuFrameTimeMs = 0.0f;
				if (renderer->GetDriver()->TryGetGpuFrameTimeMs(gpuFrameTimeMs))
				{
					char frameTimeText[64]{};
					const uint32_t measuredGpuFps =
						RHI::CalculateGpuFramesPerSecond(gpuFrameTimeMs);
					if (measuredGpuFps > 0u)
					{
						displayedGpuFps = measuredGpuFps;
					}
					std::snprintf(
						frameTimeText,
						sizeof(frameTimeText),
						"GPU frame %.2f ms",
						gpuFrameTimeMs);
					gpuQueryText = frameTimeText;

					const TVector<RHI::GpuTiming> topGpuTimings =
						renderer->GetSlowestGpuTimings();
					if (topGpuTimings.IsEmpty())
					{
						gpuQueryText += "\nGPU nodes/ops pending";
					}
					else
					{
						gpuQueryText += "\nSlowest GPU nodes (avg):";
						for (size_t i = 0u; i < topGpuTimings.Num(); ++i)
						{
							char timingText[128]{};
							std::snprintf(
								timingText,
								sizeof(timingText),
								"\n%zu. %.64s %.3f ms",
								i + 1u,
								topGpuTimings[i].m_name.c_str(),
								topGpuTimings[i].m_durationMilliseconds);
							gpuQueryText += timingText;
						}
					}
				}
				else
				{
					gpuQueryText = "GPU query pending";
				}
			}
		}
		DrawViewportStatsOverlay(
			m_cpuFps,
			displayedGpuFps,
			stats.m_numBatches.load(std::memory_order_relaxed),
			stats.m_numInstances.load(std::memory_order_relaxed),
			shadowMemoryMb,
			csmShadowMemoryMb,
			localShadowMemoryMb,
			shadowMemoryBudgetMb,
			globalIlluminationStats,
			stats,
			gpuQueryText.c_str());
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
