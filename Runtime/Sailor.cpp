#include "Sailor.h"
#include "AssetRegistry/AssetRegistry.h"
#include "AssetRegistry/Shader/ShaderCompiler.h"
#include "AssetRegistry/Texture/TextureImporter.h"
#include "AssetRegistry/Model/ModelImporter.h"
#include "AssetRegistry/Material/MaterialImporter.h"
#include "Platform/Win32/ConsoleWindow.h"
#include "Platform/Win32/Input.h"
#include "GfxDevice/Vulkan/VulkanApi.h"
#include "JobSystem/JobSystem.h"
#include "RHI/Renderer.h"
#include "Core/Submodule.h"
#include "Containers/Vector.h"
#include "Containers/Set.h"
#include "Containers/Map.h"
#include "Containers/List.h"
#include "Engine/EngineLoop.h"
#include "Memory/MemoryBlockAllocator.hpp"

using namespace Sailor;
using namespace Sailor::RHI;

App* App::s_pInstance = nullptr;
const char* App::ApplicationName = "SailorEngine";
const char* App::EngineName = "Sailor";

void App::Initialize()
{
	SAILOR_PROFILE_FUNCTION();

	if (s_pInstance != nullptr)
	{
		return;
	}

	Win32::ConsoleWindow::Initialize(false);

	//#ifdef _DEBUG
	Win32::ConsoleWindow::GetInstance()->OpenWindow(L"Sailor Console");
	//#endif

	s_pInstance = new App();
	s_pInstance->m_pViewportWindow = TUniquePtr<Win32::Window>::Make();
	s_pInstance->m_pViewportWindow->Create(L"Sailor Viewport", L"SailorViewport", 1024, 768);

#ifdef _DEBUG
	const bool bIsEnabledVulkanValidationLayers = true;
#endif

#ifndef _DEBUG
	const bool bIsEnabledVulkanValidationLayers = false;
#endif 

	s_pInstance->AddSubmodule(TSubmodule<JobSystem::Scheduler>::Make())->Initialize();

#ifdef BUILD_WITH_EASY_PROFILER
	SAILOR_ENQUEUE_JOB("Initialize profiler", ([]() {profiler::startListen(); }));
	EASY_MAIN_THREAD;
#endif

	s_pInstance->AddSubmodule(TSubmodule<Renderer>::Make(s_pInstance->m_pViewportWindow.GetRawPtr(), RHI::EMsaaSamples::Samples_2, bIsEnabledVulkanValidationLayers));
	auto assetRegistry = s_pInstance->AddSubmodule(TSubmodule<AssetRegistry>::Make());

	s_pInstance->AddSubmodule(TSubmodule<DefaultAssetInfoHandler>::Make(assetRegistry));

	auto textureInfoHandler = s_pInstance->AddSubmodule(TSubmodule<TextureAssetInfoHandler>::Make(assetRegistry));
	auto shaderInfoHandler = s_pInstance->AddSubmodule(TSubmodule<ShaderAssetInfoHandler>::Make(assetRegistry));
	auto modelInfoHandler = s_pInstance->AddSubmodule(TSubmodule<ModelAssetInfoHandler>::Make(assetRegistry));
	auto materialInfoHandler = s_pInstance->AddSubmodule(TSubmodule<MaterialAssetInfoHandler>::Make(assetRegistry));

	s_pInstance->AddSubmodule(TSubmodule<TextureImporter>::Make(textureInfoHandler));
	s_pInstance->AddSubmodule(TSubmodule<ShaderCompiler>::Make(shaderInfoHandler));
	s_pInstance->AddSubmodule(TSubmodule<ModelImporter>::Make(modelInfoHandler));
	s_pInstance->AddSubmodule(TSubmodule<MaterialImporter>::Make(materialInfoHandler));

	GetSubmodule<AssetRegistry>()->ScanContentFolder();

	s_pInstance->AddSubmodule(TSubmodule<EngineLoop>::Make());

	SAILOR_LOG("Sailor Engine initialized");
}

void App::Start()
{
	s_pInstance->m_pViewportWindow->SetActive(true);
	s_pInstance->m_pViewportWindow->SetRunning(true);

	uint32_t frameCounter = 0U;
	Utils::Timer timer;
	FrameState currentFrame;
	FrameState lastFrame;
	bool bCanCreateNewFrame = true;

	std::unordered_map<std::string, std::function<void()>> consoleVars;
	consoleVars["scan"] = std::bind(&AssetRegistry::ScanContentFolder, GetSubmodule<AssetRegistry>());
	consoleVars["memory.benchmark"] = &Memory::RunMemoryBenchmark;
	consoleVars["vector.benchmark"] = &Sailor::RunVectorBenchmark;
	consoleVars["set.benchmark"] = &Sailor::RunSetBenchmark;
	consoleVars["map.benchmark"] = &Sailor::RunMapBenchmark;
	consoleVars["list.benchmark"] = &Sailor::RunListBenchmark;

	while (s_pInstance->m_pViewportWindow->IsRunning())
	{
		timer.Start();

		Win32::ConsoleWindow::GetInstance()->Update();

		char line[256];
		if (Win32::ConsoleWindow::GetInstance()->Read(line, 256) != 0)
		{
			std::string cmd = std::string(line);
			Utils::Trim(cmd);

			auto it = consoleVars.find(cmd);
			if (it != consoleVars.end())
			{
				it->second();
			}
		}

		Win32::Window::ProcessWin32Msgs();
		GetSubmodule<Renderer>()->FixLostDevice();

		GetSubmodule<JobSystem::Scheduler>()->ProcessJobsOnMainThread();

		if (GlobalInput::GetInputState().IsKeyPressed(VK_ESCAPE))
		{
			Stop();
			break;
		}

		if (GlobalInput::GetInputState().IsKeyPressed(VK_F5))
		{
			GetSubmodule<AssetRegistry>()->ScanContentFolder();
		}

		if (bCanCreateNewFrame)
		{
			if (GlobalInput::GetInputState().IsButtonDown(VK_LBUTTON))
			{
				ivec2 centerPosition = s_pInstance->m_pViewportWindow->GetCenterPointScreen();
				GlobalInput::SetCursorPos(centerPosition.x, centerPosition.y);
			}

			FrameInputState inputState = (Sailor::FrameInputState)GlobalInput::GetInputState();
			currentFrame = FrameState(Utils::GetCurrentTimeMs(), inputState, s_pInstance->m_pViewportWindow->GetCenterPointClient(), &lastFrame);
			App::GetSubmodule<EngineLoop>()->ProcessCpuFrame(currentFrame);
		}

		if (bCanCreateNewFrame = GetSubmodule<Renderer>()->PushFrame(currentFrame))
		{
			lastFrame = currentFrame;

			//Frame succesfully pushed
			frameCounter++;
		}

		timer.Stop();

		if (timer.ResultAccumulatedMs() > 1000)
		{
			SAILOR_PROFILE_BLOCK("Track FPS");

			WCHAR Buff[50];
			wsprintf(Buff, L"Sailor FPS: %u, GPU FPS: %u, CPU FPS: %u", frameCounter,
				GetSubmodule<Renderer>()->GetSmoothFps(),
				(uint32_t)App::GetSubmodule<EngineLoop>()->GetSmoothFps());

			s_pInstance->m_pViewportWindow->SetWindowTitle(Buff);

			frameCounter = 0U;
			timer.Clear();

			SAILOR_PROFILE_END_BLOCK();
		}
	}

	s_pInstance->m_pViewportWindow->SetActive(false);
	s_pInstance->m_pViewportWindow->SetRunning(false);

	App::GetSubmodule<JobSystem::Scheduler>()->WaitIdle(JobSystem::EThreadType::Worker);
	App::GetSubmodule<JobSystem::Scheduler>()->WaitIdle(JobSystem::EThreadType::Rendering);
}

void App::Stop()
{
	s_pInstance->m_pViewportWindow->SetActive(false);
}

void App::Shutdown()
{
	SAILOR_LOG("Sailor Engine Releasing");

	App::RemoveSubmodule<EngineLoop>();

	// Release all resources before renderer
	RemoveSubmodule<DefaultAssetInfoHandler>();
	RemoveSubmodule<ShaderAssetInfoHandler>();
	RemoveSubmodule<TextureAssetInfoHandler>();
	RemoveSubmodule<ModelAssetInfoHandler>();

	RemoveSubmodule<MaterialImporter>();
	RemoveSubmodule<ModelImporter>();
	RemoveSubmodule<ShaderCompiler>();
	RemoveSubmodule<TextureImporter>();
	RemoveSubmodule<AssetRegistry>();

	// We need to finish all jobs before release
	App::GetSubmodule<JobSystem::Scheduler>()->ProcessJobsOnMainThread();
	App::GetSubmodule<JobSystem::Scheduler>()->WaitIdle(JobSystem::EThreadType::Worker);
	App::GetSubmodule<JobSystem::Scheduler>()->WaitIdle(JobSystem::EThreadType::Rendering);

	RemoveSubmodule<Renderer>();
	RemoveSubmodule<JobSystem::Scheduler>();

	Win32::ConsoleWindow::Shutdown();

	delete s_pInstance;
}

TUniquePtr<Win32::Window>& App::GetViewportWindow()
{
	return s_pInstance->m_pViewportWindow;
}

std::mutex m_logMutex;
