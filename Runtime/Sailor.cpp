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
#include "Core/Submodule.hpp"
#include "Framework/Framework.h"
#include "Memory/MemoryBlockAllocator.hpp"

using namespace Sailor;
using namespace Sailor::RHI;

EngineInstance* EngineInstance::m_pInstance = nullptr;
const char* EngineInstance::ApplicationName = "SailorEngine";
const char* EngineInstance::EngineName = "Sailor";

void EngineInstance::Initialize()
{
	SAILOR_PROFILE_FUNCTION();

	if (m_pInstance != nullptr)
	{
		return;
	}

	Win32::ConsoleWindow::Initialize(false);

	//#ifdef _DEBUG
	Win32::ConsoleWindow::GetInstance()->OpenWindow(L"Sailor Console");
	//#endif

	m_pInstance = new EngineInstance();
	m_pInstance->m_pViewportWindow = TUniquePtr<Win32::Window>::Make();
	m_pInstance->m_pViewportWindow->Create(L"Sailor Viewport", L"SailorViewport", 1024, 768);

#ifdef _DEBUG
	const bool bIsEnabledVulkanValidationLayers = true;
#endif

#ifndef _DEBUG
	const bool bIsEnabledVulkanValidationLayers = false;
#endif 

	JobSystem::Scheduler::Initialize();

#ifdef BUILD_WITH_EASY_PROFILER
	SAILOR_ENQUEUE_JOB("Initialize profiler", ([]() {profiler::startListen(); }));
	EASY_MAIN_THREAD;
#endif
	
	Renderer::Initialize(m_pInstance->m_pViewportWindow.GetRawPtr(), RHI::EMsaaSamples::Samples_2, bIsEnabledVulkanValidationLayers);
	
	m_pInstance->m_pAssetRegistry = Submodule::Make<AssetRegistry>();

	GetSubmodule<AssetRegistry>()->Initialize();

	TextureImporter::Initialize();
	ShaderCompiler::Initialize();
	ModelImporter::Initialize();
	MaterialImporter::Initialize();

	GetSubmodule<AssetRegistry>()->ScanContentFolder();

	ShaderCompiler::GetInstance();
	Framework::Initialize();

	SAILOR_LOG("Sailor Engine initialized");
}

void EngineInstance::Start()
{
	m_pInstance->m_pViewportWindow->SetActive(true);
	m_pInstance->m_pViewportWindow->SetRunning(true);

	uint32_t frameCounter = 0U;
	Utils::Timer timer;
	FrameState currentFrame;
	FrameState lastFrame;
	bool bCanCreateNewFrame = true;

	std::unordered_map<std::string, std::function<void()>> consoleVars;
	consoleVars["scan"] = std::bind(&AssetRegistry::ScanContentFolder, GetSubmodule<AssetRegistry>());
	consoleVars["memory.benchmark"] = &Memory::RunMemoryBenchmark;

	while (m_pInstance->m_pViewportWindow->IsRunning())
	{
		timer.Start();

		Win32::ConsoleWindow::GetInstance()->Update();

		char line[256];
		if (Win32::ConsoleWindow::GetInstance()->Read(line, 256) != 0)
		{
			std::string cmd = std::string(line);
			Utils::Trim(cmd);

			auto cmdIt = consoleVars.find(cmd);
			if (cmdIt != consoleVars.end())
			{
				cmdIt->second();
			}
		}

		Win32::Window::ProcessWin32Msgs();
		Renderer::GetInstance()->FixLostDevice();

		JobSystem::Scheduler::GetInstance()->ProcessJobsOnMainThread();

		if (GlobalInput::GetInputState().IsKeyPressed(VK_ESCAPE))
		{
			Stop();
			break;
		}

		if (bCanCreateNewFrame)
		{
			if (GlobalInput::GetInputState().IsButtonDown(VK_LBUTTON))
			{
				ivec2 centerPosition = m_pInstance->m_pViewportWindow->GetCenterPointScreen();
				GlobalInput::SetCursorPos(centerPosition.x, centerPosition.y);
			}

			FrameInputState inputState = (Sailor::FrameInputState)GlobalInput::GetInputState();
			currentFrame = FrameState(Utils::GetCurrentTimeMs(), inputState, m_pInstance->m_pViewportWindow->GetCenterPointClient(), &lastFrame);
			Framework::GetInstance()->ProcessCpuFrame(currentFrame);
		}

		if (bCanCreateNewFrame = Renderer::GetInstance()->PushFrame(currentFrame))
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
				Renderer::GetInstance()->GetSmoothFps(),
				(uint32_t)Framework::GetInstance()->GetSmoothFps());

			m_pInstance->m_pViewportWindow->SetWindowTitle(Buff);

			frameCounter = 0U;
			timer.Clear();

			SAILOR_PROFILE_END_BLOCK();
		}
	}

	m_pInstance->m_pViewportWindow->SetActive(false);
	m_pInstance->m_pViewportWindow->SetRunning(false);

	JobSystem::Scheduler::GetInstance()->WaitIdle(JobSystem::EThreadType::Worker);
	JobSystem::Scheduler::GetInstance()->WaitIdle(JobSystem::EThreadType::Rendering);
}

void EngineInstance::Stop()
{
	m_pInstance->m_pViewportWindow->SetActive(false);
}

void EngineInstance::Shutdown()
{
	SAILOR_LOG("Sailor Engine Releasing");

	Framework::Shutdown();

	// We need to finish all jobs before release
	JobSystem::Scheduler::GetInstance()->ProcessJobsOnMainThread();
	JobSystem::Scheduler::GetInstance()->WaitIdle(JobSystem::EThreadType::Worker);
	JobSystem::Scheduler::GetInstance()->WaitIdle(JobSystem::EThreadType::Rendering);

	Renderer::Shutdown();
	JobSystem::Scheduler::Shutdown();
	Win32::ConsoleWindow::Shutdown();
	ShaderCompiler::Shutdown();
	
	delete m_pInstance->m_pAssetRegistry;
	delete m_pInstance;
}

TUniquePtr<Win32::Window>& EngineInstance::GetViewportWindow()
{
	return m_pInstance->m_pViewportWindow;
}

std::mutex m_logMutex;
