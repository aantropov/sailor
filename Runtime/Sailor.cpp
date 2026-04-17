#include "Sailor.h"
#include "Editor/EditorRuntimeBridge.h"
#include "AssetRegistry/AssetRegistry.h"
#include "AssetRegistry/Shader/ShaderCompiler.h"
#include "AssetRegistry/Texture/TextureImporter.h"
#include "AssetRegistry/Model/ModelImporter.h"
#include "AssetRegistry/Animation/AnimationImporter.h"
#include "AssetRegistry/Animation/AnimationAssetInfo.h"
#include "AssetRegistry/Material/MaterialImporter.h"
#include "AssetRegistry/FrameGraph/FrameGraphImporter.h"
#include "AssetRegistry/Prefab/PrefabImporter.h"
#include "AssetRegistry/World/WorldPrefabImporter.h"
#include "Core/Reflection.h"
#if defined(_WIN32)
#include "Platform/Win32/ConsoleWindow.h"
#endif
#include "Platform/Win32/Input.h"
#include "GraphicsDriver/Vulkan/VulkanApi.h"
#include "Tasks/Scheduler.h"
#include "RHI/Renderer.h"
#include "Core/Submodule.h"
#include "Containers/Vector.h"
#include "Containers/Set.h"
#include <sstream>
#include "Containers/Map.h"
#include "Containers/List.h"
#include "Containers/Octree.h"
#include "Engine/EngineLoop.h"
#include <algorithm>
#include <filesystem>
#include <cstring>
#include "Memory/MemoryBlockAllocator.hpp"
#include "ECS/ECS.h"
#include "FrameGraph/RHIFrameGraph.h"
#include "FrameGraph/FrameGraphNode.h"
#include "ECS/TransformECS.h"
#include "Submodules/RenderDocApi.h"
#include "Submodules/ImGuiApi.h"
#include "Raytracing/PathTracer.h"
#include "Submodules/Editor.h"
#include "Engine/InstanceId.h"

#if defined(_WIN32)
#include <timeapi.h>
#endif

using namespace Sailor;
using namespace Sailor::RHI;

App* App::s_pInstance = nullptr;
std::string App::s_workspace = "../";

App* App::GetInstance()
{
	return s_pInstance;
}

const std::string& App::GetWorkspace()
{
	return s_workspace;
}

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
		else if (arg == "--world")
		{
			params.m_world = Utils::GetArgValue(args, i, num);
		}
		else if (arg == "--pathtracer")
		{
			params.m_bRunPathTracer = true;
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

#if defined(_WIN32)
	timeBeginPeriod(1);
#endif
	const AppArgs params = ParseCommandLineArgs(commandLineArgs, num);
	if (params.m_bWaitForDebugger)
	{
		int32_t timeout = 5000;
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
	s_pInstance->m_args = params;

#if defined(_WIN32)
	Win32::ConsoleWindow::Initialize(false);

	if (params.m_bRunConsole)
	{
		Win32::ConsoleWindow::GetInstance()->OpenWindow(L"Sailor Console");
	}
#endif

	if (!params.m_workspace.empty())
	{
		s_pInstance->s_workspace = Utils::SanitizeFilepath(params.m_workspace);
		if (!s_pInstance->s_workspace.ends_with("/"))
		{
			s_pInstance->s_workspace += "/";
		}
	}
	else
	{
		const std::filesystem::path cwd = std::filesystem::current_path();
		const bool hasContentInCwd = std::filesystem::exists(cwd / "Content");
		const bool hasContentInParent = std::filesystem::exists(cwd / ".." / "Content");

		if (hasContentInCwd)
		{
			s_pInstance->s_workspace = "./";
		}
		else if (!hasContentInParent)
		{
			SAILOR_LOG_ERROR("Content folder was not found from current working directory. Use --workspace <path>.");
		}
	}

#if defined(SAILOR_BUILD_WITH_RENDER_DOC) && defined(_DEBUG)
	if (!params.m_bIsEditor)
	{
		s_pInstance->AddSubmodule(TSubmodule<RenderDocApi>::Make());
	}
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

	if (params.m_bIsEditor)
	{
		s_pInstance->AddSubmodule(TSubmodule<Editor>::Make(params.m_editorHwnd, params.m_editorPort, s_pInstance->m_pMainWindow.GetRawPtr()));
	}

	s_pInstance->AddSubmodule(TSubmodule<Tasks::Scheduler>::Make())->Initialize();
	auto renderer = s_pInstance->AddSubmodule(TSubmodule<Renderer>::Make(s_pInstance->m_pMainWindow.GetRawPtr(), RHI::EMsaaSamples::Samples_1, bEnableRenderValidationLayers));
	if (!renderer->IsInitialized())
	{
		SAILOR_LOG_ERROR("App initialization aborted: renderer backend failed to initialize.");
		return;
	}

	auto assetRegistry = s_pInstance->AddSubmodule(TSubmodule<AssetRegistry>::Make());
	s_pInstance->AddSubmodule(TSubmodule<DefaultAssetInfoHandler>::Make(assetRegistry));

	auto textureInfoHandler = s_pInstance->AddSubmodule(TSubmodule<TextureAssetInfoHandler>::Make(assetRegistry));
	auto shaderInfoHandler = s_pInstance->AddSubmodule(TSubmodule<ShaderAssetInfoHandler>::Make(assetRegistry));
	auto modelInfoHandler = s_pInstance->AddSubmodule(TSubmodule<ModelAssetInfoHandler>::Make(assetRegistry));
	auto animationInfoHandler = s_pInstance->AddSubmodule(TSubmodule<AnimationAssetInfoHandler>::Make(assetRegistry));
	auto materialInfoHandler = s_pInstance->AddSubmodule(TSubmodule<MaterialAssetInfoHandler>::Make(assetRegistry));
	auto frameGraphInfoHandler = s_pInstance->AddSubmodule(TSubmodule<FrameGraphAssetInfoHandler>::Make(assetRegistry));
	auto prefabInfoHandler = s_pInstance->AddSubmodule(TSubmodule<PrefabAssetInfoHandler>::Make(assetRegistry));
	auto worldPrefabInfoHandler = s_pInstance->AddSubmodule(TSubmodule<WorldPrefabAssetInfoHandler>::Make(assetRegistry));

	s_pInstance->AddSubmodule(TSubmodule<TextureImporter>::Make(textureInfoHandler));
	s_pInstance->AddSubmodule(TSubmodule<ShaderCompiler>::Make(shaderInfoHandler));
	s_pInstance->AddSubmodule(TSubmodule<ModelImporter>::Make(modelInfoHandler));
	s_pInstance->AddSubmodule(TSubmodule<AnimationImporter>::Make(animationInfoHandler));
	s_pInstance->AddSubmodule(TSubmodule<MaterialImporter>::Make(materialInfoHandler));
	s_pInstance->AddSubmodule(TSubmodule<FrameGraphImporter>::Make(frameGraphInfoHandler));
	s_pInstance->AddSubmodule(TSubmodule<ECS::ECSFactory>::Make());
	s_pInstance->AddSubmodule(TSubmodule<FrameGraphBuilder>::Make());
	s_pInstance->AddSubmodule(TSubmodule<PrefabImporter>::Make(prefabInfoHandler));
	s_pInstance->AddSubmodule(TSubmodule<WorldPrefabImporter>::Make(worldPrefabInfoHandler));

	assetRegistry->ScanContentFolder();

	if (params.m_bRunPathTracer)
	{
		Raytracing::PathTracer::Params pathTracerParams{};
		Raytracing::PathTracer::ParseCommandLineArgs(pathTracerParams, commandLineArgs, num);

		if (pathTracerParams.m_pathToModel.empty())
		{
			SAILOR_LOG_ERROR("PathTracer mode requires --in <modelPath> and --out <imagePath>.");
		}
		else
		{
			Raytracing::PathTracer tracer;
			tracer.Run(pathTracerParams);
		}

		s_pInstance->m_bSkipMainLoop = true;
		return;
	}

	s_pInstance->AddSubmodule(TSubmodule<ImGuiApi>::Make((void*)s_pInstance->m_pMainWindow->GetHWND()));
	auto engineLoop = s_pInstance->AddSubmodule(TSubmodule<EngineLoop>::Make());

#ifdef SAILOR_EDITOR
	AssetRegistry::WriteTextFile(AssetRegistry::GetCacheFolder() + "EngineTypes.yaml", Reflection::ExportEngineTypes());
#endif

	auto worldParams = params.m_bIsEditor ? EngineLoop::EditorWorldMask : EngineLoop::DefaultWorldMask;
	TWeakPtr<World> pWorld;

	if (!params.m_world.empty())
	{
		auto worldFileId = assetRegistry->GetOrLoadFile(params.m_world);
		auto worldPrefab = assetRegistry->LoadAssetFromFile<WorldPrefab>(worldFileId);
		pWorld = engineLoop->InstantiateWorld(worldPrefab, worldParams);
	}
	else
	{
		pWorld = engineLoop->CreateEmptyWorld("New World", worldParams);
	}

	if (auto editor = App::GetSubmodule<Editor>())
	{
		editor->SetWorld(pWorld.Lock().GetRawPtr());
	}

#if defined(__APPLE__)
	if (!App::HasEditor() && !renderer->EnsureFrameGraph())
#else
	if (!renderer->EnsureFrameGraph())
#endif
	{
		SAILOR_LOG_ERROR("App initialization warning: renderer frame graph is still unavailable after bootstrap.");
	}

	SAILOR_LOG("Sailor Engine initialized");
}

void App::Start()
{
	if (!s_pInstance)
	{
		return;
	}

	if (s_pInstance->m_bSkipMainLoop)
	{
		return;
	}

	auto scheduler = GetSubmodule<Tasks::Scheduler>();
	auto renderer = GetSubmodule<Renderer>();
	auto pEngineLoop = App::GetSubmodule<EngineLoop>();
	const bool bRunsInsideEditor = HasEditor();
	if (!scheduler || !renderer || !renderer->IsInitialized() || !pEngineLoop)
	{
		SAILOR_LOG_ERROR("App::Start skipped: engine subsystems are not fully initialized.");
		return;
	}

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

	FrameInputState systemInputState = (Sailor::FrameInputState)Win32::GlobalInput::GetInputState();

	while (pMainWindow->IsRunning())
	{
		timer.Start();
		trackEditor.Start();

#if defined(_WIN32)
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
#endif

#if defined(__APPLE__)
		if (!bRunsInsideEditor)
		{
			pMainWindow->ProcessSystemMessages();
		}
#else
		pMainWindow->ProcessSystemMessages();
#endif
		if (!bRunsInsideEditor)
		{
			renderer->FixLostDevice();
		}

		scheduler->ProcessTasksOnMainThread();
		if (bRunsInsideEditor)
		{
			EditorRuntime::ApplyPendingEditorViewportOnEngineThread();
		}

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

		const bool bEditorRenderAreaReady =
#if defined(__APPLE__)
			!bRunsInsideEditor || EditorRuntime::HasAppliedEditorRenderArea();
#else
			true;
#endif

		if (bCanCreateNewFrame && bEditorRenderAreaReady)
		{
			FrameInputState inputState = (Sailor::FrameInputState)Win32::GlobalInput::GetInputState();
			currentFrame = FrameState(pEngineLoop->GetWorld().GetRawPtr(),
				Utils::GetCurrentTimeMs(),
				inputState,
				pMainWindow->GetCenterPointClient(),
				bFirstFrame ? nullptr : &lastFrame);

			pEngineLoop->ProcessCpuFrame(currentFrame);
			bFirstFrame = false;
		}

		if (bEditorRenderAreaReady && (bCanCreateNewFrame = renderer->PushFrame(currentFrame)))
		{
			lastFrame = currentFrame;

			//Frame successfully pushed
			frameCounter++;
			if (bRunsInsideEditor)
			{
				EditorRuntime::PumpEditorRemoteViewportsOnEngineThread();
			}
		}

		pEngineLoop->ProcessPendingWorldExits();
		if (pEngineLoop->GetWorlds().IsEmpty())
		{
			Stop();
			break;
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

		if (trackEditor.ResultAccumulatedMs() > 1 && pMainWindow->IsShown())
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

			char Buff[256];
			SAILOR_SNPRINTF(Buff, sizeof(Buff), "Sailor FPS: %u, GPU FPS: %u, CPU FPS: %u, VRAM Usage: %.2f/%.2fmb, CmdLists: %u", frameCounter,
				stats.m_gpuFps,
				(uint32_t)pEngineLoop->GetCpuFps(),
				(float)stats.m_gpuHeapUsage / (1024.0f * 1024.0f),
				(float)stats.m_gpuHeapBudget / (1024.0f * 1024.0f),
				stats.m_numSubmittedCommandBuffers
			);

#if defined(__APPLE__)
			if (!bRunsInsideEditor)
			{
				pMainWindow->SetWindowTitle(Buff);
			}
#else
			pMainWindow->SetWindowTitle(Buff);
#endif

			frameCounter = 0U;
			timer.Clear();
		}

		auto oldInputState = systemInputState;
		systemInputState = Win32::GlobalInput::GetInputState();
		systemInputState.TrackForChanges(oldInputState);

		SAILOR_PROFILE_END_FRAME();
	}

	pMainWindow->SetActive(false);
	pMainWindow->SetRunning(false);
}

void App::Stop()
{
	s_pInstance->m_pMainWindow->SetActive(false);
	s_pInstance->m_pMainWindow->SetRunning(false);
}

void App::Shutdown()
{
	if (!s_pInstance)
	{
		return;
	}

	auto scheduler = GetSubmodule<Tasks::Scheduler>();
	auto renderer = GetSubmodule<Renderer>();

	if (scheduler)
	{
		scheduler->ProcessTasksOnMainThread();
	}

	if (renderer)
	{
		renderer->BeginConditionalDestroy();
	}

	if (scheduler)
	{
		scheduler->WaitIdle({ EThreadType::Main, EThreadType::Worker, EThreadType::RHI, EThreadType::Render });
	}

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

	if (scheduler)
	{
		scheduler->ProcessTasksOnMainThread();
	}

	RemoveSubmodule<FrameGraphImporter>();
	RemoveSubmodule<MaterialImporter>();
	RemoveSubmodule<ModelImporter>();
	RemoveSubmodule<AnimationImporter>();
	RemoveSubmodule<ShaderCompiler>();
	RemoveSubmodule<TextureImporter>();
	RemoveSubmodule<PrefabImporter>();
	RemoveSubmodule<WorldPrefabImporter>();

	RemoveSubmodule<AssetRegistry>();

	RemoveSubmodule<Tasks::Scheduler>();
	RemoveSubmodule<Renderer>();

#if defined(_WIN32)
	Win32::ConsoleWindow::Shutdown();
#endif

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

bool App::IsRendererInitialized()
{
	if (!s_pInstance)
	{
		return false;
	}

	if (auto renderer = s_pInstance->GetSubmodule<RHI::Renderer>())
	{
		return renderer->IsInitialized();
	}

	return false;
}

bool App::HasEditor()
{
	if (!s_pInstance)
	{
		return false;
	}

	return s_pInstance->GetSubmodule<Editor>() != nullptr;
}

bool App::IsEditorMode()
{
	return s_pInstance && s_pInstance->m_args.m_bIsEditor;
}

const std::string& App::GetLoadedWorldPath()
{
	static const std::string empty;
	return s_pInstance ? s_pInstance->m_args.m_world : empty;
}

TUniquePtr<Sailor::Win32::Window>& App::GetMainWindow()
{
	return s_pInstance->m_pMainWindow;
}

Sailor::Platform::Window* App::GetMainWindowPlatform()
{
	return s_pInstance ? static_cast<Sailor::Platform::Window*>(s_pInstance->m_pMainWindow.GetRawPtr()) : nullptr;
}

int32_t App::GetExitCode()
{
	return s_pInstance ? s_pInstance->m_exitCode : 0;
}

const char* App::GetBuildConfig()
{
#if defined(_DEBUG)
	return "Debug";
#elif defined(_SHIPPING)
	return "Release";
#else
	return "Unknown";
#endif
}

void App::SetExitCode(int32_t exitCode)
{
	if (s_pInstance)
	{
		s_pInstance->m_exitCode = (std::max)(s_pInstance->m_exitCode, exitCode);
	}
}
