#include "Sailor.h"
#include "AssetRegistry/AssetRegistry.h"
#include "AssetRegistry/Shader/ShaderCompiler.h"
#include "AssetRegistry/Texture/TextureImporter.h"
#include "AssetRegistry/Model/ModelImporter.h"
#include "AssetRegistry/Material/MaterialImporter.h"
#include "AssetRegistry/FrameGraph/FrameGraphImporter.h"
#include "AssetRegistry/Prefab/PrefabImporter.h"
#include "AssetRegistry/World/WorldPrefabImporter.h"
#include "Platform/Win32/ConsoleWindow.h"
#include "Platform/Win32/Input.h"
#include "GraphicsDriver/Vulkan/VulkanApi.h"
#include "Tasks/Scheduler.h"
#include "RHI/Renderer.h"
#include "Core/Submodule.h"
#include "Containers/Vector.h"
#include "Containers/Set.h"
#include "Containers/Map.h"
#include "Containers/List.h"
#include "Containers/Octree.h"
#include "Engine/EngineLoop.h"
#include "Memory/MemoryBlockAllocator.hpp"
#include "ECS/ECS.h"
#include "FrameGraph/RHIFrameGraph.h"
#include "FrameGraph/FrameGraphNode.h"
#include "ECS/TransformECS.h"
#include "Submodules/RenderDocApi.h"
#include "timeApi.h"
#include "Submodules/ImGuiApi.h"
#include "Raytracing/PathTracer.h"
#include "Submodules/Editor.h"

using namespace Sailor;
using namespace Sailor::RHI;

App* App::s_pInstance = nullptr;
std::string App::s_workspace = "../";

AppArgs ParseCommandLineArgs(const char** args, int32_t num)
{
	AppArgs params{};

	for (int32_t i = 1; i < num; i++)
	{
		std::string arg = args[i];

		if (arg == "--port")
		{
			params.m_editorPort = atoi(Utils::GetArgValue(args, i, num).c_str());
		}
		else if (arg == "--hwnd")
		{
			const unsigned long long hwndInt = std::stoull(Utils::GetArgValue(args, i, num));
			params.m_editorHwnd = reinterpret_cast<HWND>(hwndInt);
		}
		else if (arg == "--editor")
		{
			params.m_bIsEditor = true;
		}
		else if (arg == "--waitfordebugger")
		{
			params.m_bWaitForDebugger = true;
		}
		else if (arg == "--workspace")
		{
			params.m_workspace = Utils::GetArgValue(args, i, num);
		}
		else if (arg == "--noconsole")
		{
			params.m_bRunConsole = false;
		}
	}

	return params;
}

void App::Initialize(const char** commandLineArgs, int32_t num)
{
	SAILOR_PROFILE_FUNCTION();

	if (s_pInstance != nullptr)
	{
		return;
	}

	timeBeginPeriod(1);
	const AppArgs params = ParseCommandLineArgs(commandLineArgs, num);
	if (params.m_bWaitForDebugger)
	{
		uint32_t timeout = 5000;
		while (!::IsDebuggerPresent())
		{
			::Sleep(100);
			timeout -= 100;
			if (timeout < 0)
			{
				exit(0);
			}
		}
	}

	s_pInstance = new App();

	Win32::ConsoleWindow::Initialize(false);

	if (params.m_bRunConsole)
	{
		Win32::ConsoleWindow::GetInstance()->OpenWindow(L"Sailor Console");
	}

	if (!params.m_workspace.empty())
	{
		s_pInstance->s_workspace = params.m_workspace;
	}

	if (params.m_bIsEditor)
	{
		s_pInstance->AddSubmodule(TSubmodule<Editor>::Make(params.m_editorHwnd, params.m_editorPort));
	}

#if defined(SAILOR_BUILD_WITH_RENDER_DOC) && defined(_DEBUG)
	s_pInstance->AddSubmodule(TSubmodule<RenderDocApi>::Make());
#endif

	bool bEnableRenderValidationLayers = params.m_bEnableRenderValidationLayers;
#if defined(_SHIPPING)
	// We don't enable Debug validation layers for Shipping (Release) configuration
	bEnableRenderValidationLayers = false;
#endif

	s_pInstance->m_pMainWindow = TUniquePtr<Win32::Window>::Make();

	std::string className = "SailorEngine";
	if (params.m_editorHwnd != 0)
	{
		className = std::format("SailorEditor PID{}", ::GetCurrentThreadId());
	}

	s_pInstance->m_pMainWindow->Create(className.c_str(), className.c_str(), 1024, 768, false, false, params.m_editorHwnd);

	s_pInstance->AddSubmodule(TSubmodule<Tasks::Scheduler>::Make())->Initialize();
	s_pInstance->AddSubmodule(TSubmodule<Renderer>::Make(s_pInstance->m_pMainWindow.GetRawPtr(), RHI::EMsaaSamples::Samples_16, bEnableRenderValidationLayers));

	auto assetRegistry = s_pInstance->AddSubmodule(TSubmodule<AssetRegistry>::Make());
	s_pInstance->AddSubmodule(TSubmodule<DefaultAssetInfoHandler>::Make(assetRegistry));

	auto textureInfoHandler = s_pInstance->AddSubmodule(TSubmodule<TextureAssetInfoHandler>::Make(assetRegistry));
	auto shaderInfoHandler = s_pInstance->AddSubmodule(TSubmodule<ShaderAssetInfoHandler>::Make(assetRegistry));
	auto modelInfoHandler = s_pInstance->AddSubmodule(TSubmodule<ModelAssetInfoHandler>::Make(assetRegistry));
	auto materialInfoHandler = s_pInstance->AddSubmodule(TSubmodule<MaterialAssetInfoHandler>::Make(assetRegistry));
	auto frameGraphInfoHandler = s_pInstance->AddSubmodule(TSubmodule<FrameGraphAssetInfoHandler>::Make(assetRegistry));
	auto prefabInfoHandler = s_pInstance->AddSubmodule(TSubmodule<PrefabAssetInfoHandler>::Make(assetRegistry));
	auto worldPrefabInfoHandler = s_pInstance->AddSubmodule(TSubmodule<WorldPrefabAssetInfoHandler>::Make(assetRegistry));

	s_pInstance->AddSubmodule(TSubmodule<TextureImporter>::Make(textureInfoHandler));
	s_pInstance->AddSubmodule(TSubmodule<ShaderCompiler>::Make(shaderInfoHandler));
	s_pInstance->AddSubmodule(TSubmodule<ModelImporter>::Make(modelInfoHandler));
	s_pInstance->AddSubmodule(TSubmodule<MaterialImporter>::Make(materialInfoHandler));
	s_pInstance->AddSubmodule(TSubmodule<FrameGraphImporter>::Make(frameGraphInfoHandler));
	s_pInstance->AddSubmodule(TSubmodule<ECS::ECSFactory>::Make());
	s_pInstance->AddSubmodule(TSubmodule<FrameGraphBuilder>::Make());
	s_pInstance->AddSubmodule(TSubmodule<PrefabImporter>::Make(prefabInfoHandler));
	s_pInstance->AddSubmodule(TSubmodule<WorldPrefabImporter>::Make(worldPrefabInfoHandler));

	GetSubmodule<AssetRegistry>()->ScanContentFolder();

	s_pInstance->AddSubmodule(TSubmodule<ImGuiApi>::Make((void*)s_pInstance->m_pMainWindow->GetHWND()));
	s_pInstance->AddSubmodule(TSubmodule<EngineLoop>::Make());

#ifdef SAILOR_EDITOR
	AssetRegistry::WriteTextFile(AssetRegistry::GetCacheFolder() + "EngineTypes.yaml", Reflection::ExportEngineTypes());
#endif

	SAILOR_LOG("Sailor Engine initialized");
}

void App::Start()
{
	auto& pMainWindow = s_pInstance->m_pMainWindow;

	pMainWindow->SetActive(true);
	pMainWindow->SetRunning(true);

	uint32_t frameCounter = 0U;
	Utils::Timer timer{};
	Utils::Timer trackEditor{};
	FrameState currentFrame{};
	FrameState lastFrame{};
	bool bCanCreateNewFrame = true;
	bool bFirstFrame = true;

	TMap<std::string, std::function<void()>> consoleVars;
	consoleVars["scan"] = std::bind(&AssetRegistry::ScanContentFolder, GetSubmodule<AssetRegistry>());
	consoleVars["memory.benchmark"] = &Memory::RunMemoryBenchmark;
	consoleVars["vector.benchmark"] = &Sailor::RunVectorBenchmark;
	consoleVars["set.benchmark"] = &Sailor::RunSetBenchmark;
	consoleVars["map.benchmark"] = &Sailor::RunMapBenchmark;
	consoleVars["list.benchmark"] = &Sailor::RunListBenchmark;
	consoleVars["octree.benchmark"] = &Sailor::RunOctreeBenchmark;
	consoleVars["stats.memory"] = &Sailor::RHI::Renderer::MemoryStats;

#ifdef SAILOR_EDITOR
	auto worldAssetInfo = App::GetSubmodule<AssetRegistry>()->GetAssetInfoPtr("Editor.world");
	auto worldPrefab = App::GetSubmodule<AssetRegistry>()->LoadAssetFromFile<WorldPrefab>(worldAssetInfo->GetFileId());
	TWeakPtr<World> pWorld = GetSubmodule<EngineLoop>()->CreateWorld(worldPrefab);

	if (auto editor = App::GetSubmodule<Editor>())
	{
		editor->SetWorld(pWorld.Lock().GetRawPtr());
	}
#endif

	FrameInputState systemInputState = (Sailor::FrameInputState)GlobalInput::GetInputState();

	auto scheduler = GetSubmodule<Tasks::Scheduler>();
	auto renderer = GetSubmodule<Renderer>();

	while (pMainWindow->IsRunning())
	{
		timer.Start();
		trackEditor.Start();

		Win32::ConsoleWindow::GetInstance()->Update();

		char line[256];
		if (Win32::ConsoleWindow::GetInstance()->Read(line, 256) != 0)
		{
			std::string cmd = std::string(line);
			Utils::Trim(cmd);

			auto it = consoleVars.Find(cmd);
			if (it != consoleVars.end())
			{
				it.Value()();
			}
		}

		Win32::Window::ProcessWin32Msgs();
		renderer->FixLostDevice();

		scheduler->ProcessTasksOnMainThread();

		if (systemInputState.IsKeyPressed(VK_ESCAPE) || !pMainWindow->IsParentWindowValid())
		{
			Stop();
			break;
		}

		if (systemInputState.IsKeyPressed(VK_F5))
		{
			GetSubmodule<AssetRegistry>()->ScanContentFolder();
			scheduler->WaitIdle({ EThreadType::Render, EThreadType::RHI });
			renderer->RefreshFrameGraph();
		}

#ifdef SAILOR_BUILD_WITH_RENDER_DOC
		if (auto renderDoc = App::GetSubmodule<RenderDocApi>())
		{
			if (systemInputState.IsKeyPressed(VK_F6) && !renderDoc->IsConnected())
			{
				renderDoc->LaunchRenderDocApp();
			}
		}
#endif

		if (bCanCreateNewFrame)
		{
			FrameInputState inputState = (Sailor::FrameInputState)GlobalInput::GetInputState();
			currentFrame = FrameState(pWorld.Lock().GetRawPtr(), Utils::GetCurrentTimeMs(), inputState, pMainWindow->GetCenterPointClient(), bFirstFrame ? nullptr : &lastFrame);
			App::GetSubmodule<EngineLoop>()->ProcessCpuFrame(currentFrame);
			bFirstFrame = false;
		}

		if ((bCanCreateNewFrame = renderer->PushFrame(currentFrame)))
		{
			lastFrame = currentFrame;

			//Frame successfully pushed
			frameCounter++;
		}

		// Collect garbage
		uint32_t index = 0;
		while (auto submodule = GetSubmodule(index))
		{
			submodule->CollectGarbage();
			index++;
		}

		timer.Stop();
		trackEditor.Stop();

		if (trackEditor.ResultAccumulatedMs() > 1)
		{
#ifdef SAILOR_EDITOR
			if (auto editor = App::GetSubmodule<Editor>())
			{
				auto rect = editor->GetViewport();
				pMainWindow->TrackParentWindowPosition(rect);
			}
#endif	
			trackEditor.Clear();
		}

		if (timer.ResultAccumulatedMs() > 1000)
		{
			SAILOR_PROFILE_SCOPE("Track FPS");

			const Stats& stats = renderer->GetStats();

			CHAR Buff[256];
			sprintf_s(Buff, "Sailor FPS: %u, GPU FPS: %u, CPU FPS: %u, VRAM Usage: %.2f/%.2fmb, CmdLists: %u", frameCounter,
				stats.m_gpuFps,
				(uint32_t)App::GetSubmodule<EngineLoop>()->GetCpuFps(),
				(float)stats.m_gpuHeapUsage / (1024.0f * 1024.0f),
				(float)stats.m_gpuHeapBudget / (1024.0f * 1024.0f),
				stats.m_numSubmittedCommandBuffers
			);

			pMainWindow->SetWindowTitle(Buff);

			frameCounter = 0U;
			timer.Clear();
		}

		auto oldInputState = systemInputState;
		systemInputState = GlobalInput::GetInputState();
		systemInputState.TrackForChanges(oldInputState);

		SAILOR_PROFILE_END_FRAME();
	}

	pMainWindow->SetActive(false);
	pMainWindow->SetRunning(false);
}

void App::Stop()
{
	s_pInstance->m_pMainWindow->SetActive(false);
}

void App::Shutdown()
{
	auto scheduler = GetSubmodule<Tasks::Scheduler>();
	auto renderer = GetSubmodule<Renderer>();

	scheduler->WaitIdle({ EThreadType::Main, EThreadType::Worker, EThreadType::RHI, EThreadType::Render });

	// TODO: Redo. We have 2 frames in flight.
	scheduler->WaitIdle(EThreadType::Render);

	renderer->BeginConditionalDestroy();

	SAILOR_LOG("Sailor Engine Releasing");

#if defined(SAILOR_BUILD_WITH_RENDER_DOC)
	if (App::GetSubmodule<RenderDocApi>())
	{
		RemoveSubmodule<RenderDocApi>();
	}
#endif

	RemoveSubmodule<EngineLoop>();
	RemoveSubmodule<ECS::ECSFactory>();
	RemoveSubmodule<FrameGraphBuilder>();

	// We need to finish all tasks before release
	RemoveSubmodule<ImGuiApi>();

	scheduler->WaitIdle({ EThreadType::Main, EThreadType::Worker, EThreadType::RHI, EThreadType::Render });

	RemoveSubmodule<FrameGraphImporter>();
	RemoveSubmodule<MaterialImporter>();
	RemoveSubmodule<ModelImporter>();
	RemoveSubmodule<ShaderCompiler>();
	RemoveSubmodule<TextureImporter>();
	RemoveSubmodule<PrefabImporter>();
	RemoveSubmodule<WorldPrefabImporter>();

	RemoveSubmodule<AssetRegistry>();

	RemoveSubmodule<Renderer>();
	RemoveSubmodule<Tasks::Scheduler>();

	Win32::ConsoleWindow::Shutdown();

	// Remove all left submodules
	for (auto& pSubmodule : s_pInstance->m_submodules)
	{
		if (pSubmodule)
		{
			pSubmodule.Clear();
		}
	}

	delete s_pInstance;
	s_pInstance = nullptr;
}

TUniquePtr<Sailor::Win32::Window>& App::GetMainWindow()
{
	return s_pInstance->m_pMainWindow;
}
