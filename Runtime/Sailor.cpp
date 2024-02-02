#include "Sailor.h"
#include "AssetRegistry/AssetRegistry.h"
#include "AssetRegistry/Shader/ShaderCompiler.h"
#include "AssetRegistry/Texture/TextureImporter.h"
#include "AssetRegistry/Model/ModelImporter.h"
#include "AssetRegistry/Material/MaterialImporter.h"
#include "AssetRegistry/FrameGraph/FrameGraphImporter.h"
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

using namespace Sailor;
using namespace Sailor::RHI;

App* App::s_pInstance = nullptr;

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

	Win32::ConsoleWindow::Initialize(false);

#ifdef SAILOR_WITH_CONSOLE
	if (params.m_bRunConsole)
	{
		Win32::ConsoleWindow::GetInstance()->OpenWindow(L"Sailor Console");
	}
#endif

	s_pInstance = new App();

#if defined(SAILOR_BUILD_WITH_RENDER_DOC) && defined(_DEBUG)
	s_pInstance->AddSubmodule(TSubmodule<RenderDocApi>::Make());
#endif

	s_pInstance->m_pMainWindow = TUniquePtr<Win32::Window>::Make();
	s_pInstance->m_pMainWindow->Create("Sailor Viewport", "SailorViewport", 1024, 768, false, false, params.m_editorHwnd);

#ifdef SAILOR_VULKAN_ENABLE_VALIDATION_LAYER
	const bool bIsEnabledVulkanValidationLayers = true;
#else
	const bool bIsEnabledVulkanValidationLayers = false;
#endif

	s_pInstance->AddSubmodule(TSubmodule<Tasks::Scheduler>::Make())->Initialize();

#ifdef BUILD_WITH_EASY_PROFILER
	SAILOR_ENQUEUE_TASK("Initialize profiler", ([]() {profiler::startListen(); }));
	EASY_MAIN_THREAD;
#endif

	s_pInstance->AddSubmodule(TSubmodule<Renderer>::Make(s_pInstance->m_pMainWindow.GetRawPtr(), RHI::EMsaaSamples::Samples_1, bIsEnabledVulkanValidationLayers));
	auto assetRegistry = s_pInstance->AddSubmodule(TSubmodule<AssetRegistry>::Make());

	s_pInstance->AddSubmodule(TSubmodule<DefaultAssetInfoHandler>::Make(assetRegistry));

	auto textureInfoHandler = s_pInstance->AddSubmodule(TSubmodule<TextureAssetInfoHandler>::Make(assetRegistry));
	auto shaderInfoHandler = s_pInstance->AddSubmodule(TSubmodule<ShaderAssetInfoHandler>::Make(assetRegistry));
	auto modelInfoHandler = s_pInstance->AddSubmodule(TSubmodule<ModelAssetInfoHandler>::Make(assetRegistry));
	auto materialInfoHandler = s_pInstance->AddSubmodule(TSubmodule<MaterialAssetInfoHandler>::Make(assetRegistry));
	auto frameGraphInfoHandler = s_pInstance->AddSubmodule(TSubmodule<FrameGraphAssetInfoHandler>::Make(assetRegistry));

	s_pInstance->AddSubmodule(TSubmodule<TextureImporter>::Make(textureInfoHandler));
	s_pInstance->AddSubmodule(TSubmodule<ShaderCompiler>::Make(shaderInfoHandler));
	s_pInstance->AddSubmodule(TSubmodule<ModelImporter>::Make(modelInfoHandler));
	s_pInstance->AddSubmodule(TSubmodule<MaterialImporter>::Make(materialInfoHandler));
	s_pInstance->AddSubmodule(TSubmodule<FrameGraphImporter>::Make(frameGraphInfoHandler));
	s_pInstance->AddSubmodule(TSubmodule<ECS::ECSFactory>::Make());
	s_pInstance->AddSubmodule(TSubmodule<FrameGraphBuilder>::Make());

	GetSubmodule<AssetRegistry>()->ScanContentFolder();

	s_pInstance->AddSubmodule(TSubmodule<ImGuiApi>::Make((void*)s_pInstance->m_pMainWindow->GetHWND()));
	s_pInstance->AddSubmodule(TSubmodule<EngineLoop>::Make());

	SAILOR_LOG("Sailor Engine initialized");
}

void App::Start()
{
	s_pInstance->m_pMainWindow->SetActive(true);
	s_pInstance->m_pMainWindow->SetRunning(true);

	uint32_t frameCounter = 0U;
	Utils::Timer timer;
	Utils::Timer trackEditor;
	FrameState currentFrame;
	FrameState lastFrame;
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
	TWeakPtr<World> pWorld = App::GetSubmodule<EngineLoop>()->CreateWorld("WorldEditor");
#endif

	FrameInputState systemInputState = (Sailor::FrameInputState)GlobalInput::GetInputState();

	while (s_pInstance->m_pMainWindow->IsRunning())
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
		GetSubmodule<Renderer>()->FixLostDevice();

		GetSubmodule<Tasks::Scheduler>()->ProcessJobsOnMainThread();

		if (systemInputState.IsKeyPressed(VK_ESCAPE))
		{
			Stop();
			break;
		}

		if (systemInputState.IsKeyPressed(VK_F5))
		{
			GetSubmodule<AssetRegistry>()->ScanContentFolder();
			GetSubmodule<Tasks::Scheduler>()->WaitIdle(Tasks::EThreadType::Render);
			GetSubmodule<Renderer>()->RefreshFrameGraph();
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
			currentFrame = FrameState(pWorld.Lock().GetRawPtr(), Utils::GetCurrentTimeMs(), inputState, s_pInstance->m_pMainWindow->GetCenterPointClient(), bFirstFrame ? nullptr : &lastFrame);
			App::GetSubmodule<EngineLoop>()->ProcessCpuFrame(currentFrame);
			bFirstFrame = false;
		}

		if (bCanCreateNewFrame = GetSubmodule<Renderer>()->PushFrame(currentFrame))
		{
			lastFrame = currentFrame;

			//Frame succesfully pushed
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

		if (trackEditor.ResultAccumulatedMs() > 33)
		{
			s_pInstance->m_pMainWindow->TrackParentWindowPosition();
			trackEditor.Clear();
		}

		if (timer.ResultAccumulatedMs() > 1000)
		{
			SAILOR_PROFILE_BLOCK("Track FPS");

			const Stats& stats = GetSubmodule<Renderer>()->GetStats();

			CHAR Buff[256];
			sprintf_s(Buff, "Sailor FPS: %u, GPU FPS: %u, CPU FPS: %u, VRAM Usage: %.2f/%.2fmb, CmdLists: %u", frameCounter,
				stats.m_gpuFps,
				(uint32_t)App::GetSubmodule<EngineLoop>()->GetCpuFps(),
				(float)stats.m_gpuHeapUsage / (1024.0f * 1024.0f),
				(float)stats.m_gpuHeapBudget / (1024.0f * 1024.0f),
				stats.m_numSubmittedCommandBuffers
			);

			s_pInstance->m_pMainWindow->SetWindowTitle(Buff);

			frameCounter = 0U;
			timer.Clear();

			SAILOR_PROFILE_END_BLOCK();
		}

		auto oldInputState = systemInputState;
		systemInputState = GlobalInput::GetInputState();
		systemInputState.TrackForChanges(oldInputState);
	}

	s_pInstance->m_pMainWindow->SetActive(false);
	s_pInstance->m_pMainWindow->SetRunning(false);

	App::GetSubmodule<Tasks::Scheduler>()->WaitIdle(Tasks::EThreadType::Worker);
	App::GetSubmodule<Tasks::Scheduler>()->WaitIdle(Tasks::EThreadType::Render);
}

void App::Stop()
{
	s_pInstance->m_pMainWindow->SetActive(false);
}

void App::Shutdown()
{
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

	// Release all resources before renderer
	RemoveSubmodule<DefaultAssetInfoHandler>();
	RemoveSubmodule<ShaderAssetInfoHandler>();
	RemoveSubmodule<TextureAssetInfoHandler>();
	RemoveSubmodule<ModelAssetInfoHandler>();
	RemoveSubmodule<FrameGraphAssetInfoHandler>();

	// We need to finish all jobs before release
	RemoveSubmodule<ImGuiApi>();
	GetSubmodule<Tasks::Scheduler>()->ProcessJobsOnMainThread();
	GetSubmodule<Tasks::Scheduler>()->WaitIdle(Tasks::EThreadType::Worker);
	GetSubmodule<Tasks::Scheduler>()->WaitIdle(Tasks::EThreadType::RHI);
	GetSubmodule<Tasks::Scheduler>()->WaitIdle(Tasks::EThreadType::Render);

	App::GetSubmodule<Renderer>()->BeginConditionalDestroy();

	RemoveSubmodule<FrameGraphImporter>();
	RemoveSubmodule<MaterialImporter>();
	RemoveSubmodule<ModelImporter>();
	RemoveSubmodule<ShaderCompiler>();
	RemoveSubmodule<TextureImporter>();

	RemoveSubmodule<AssetRegistry>();

	RemoveSubmodule<Renderer>();
	RemoveSubmodule<Tasks::Scheduler>();

	Win32::ConsoleWindow::Shutdown();

	delete s_pInstance;
}

TUniquePtr<Win32::Window>& App::GetMainWindow()
{
	return s_pInstance->m_pMainWindow;
}
