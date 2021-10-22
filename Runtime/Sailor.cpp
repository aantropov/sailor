#include "Sailor.h"
#include "AssetRegistry/AssetRegistry.h"
#include "AssetRegistry/ShaderCompiler.h"
#include "AssetRegistry/TextureImporter.h"
#include "AssetRegistry/ModelImporter.h"
#include "Platform/Win32/ConsoleWindow.h"
#include "Platform/Win32/Input.h"
#include "GfxDevice/Vulkan/VulkanApi.h"
#include "JobSystem/JobSystem.h"
#include "RHI/Renderer.h"
#include "Framework/Framework.h"
#include "Memory/MemoryBlockAllocator.hpp"

using namespace Sailor;

EngineInstance* EngineInstance::m_pInstance = nullptr;
const std::string EngineInstance::ApplicationName = "SailorEngine";
const std::string EngineInstance::EngineName = "Sailor";

void EngineInstance::Initialize()
{
	SAILOR_PROFILE_FUNCTION();

	if (m_pInstance != nullptr)
	{
		return;
	}

#ifdef BUILD_WITH_EASY_PROFILER
	profiler::startListen();
	EASY_MAIN_THREAD;
#endif

	Win32::ConsoleWindow::Initialize(false);

	//#ifdef _DEBUG
	Win32::ConsoleWindow::GetInstance()->OpenWindow(L"Sailor Console");
	//#endif

	m_pInstance = new EngineInstance();
	m_pInstance->m_viewportWindow.Create(L"Sailor Viewport", L"SailorViewport", 1024, 768);

#ifdef _DEBUG
	const bool bIsEnabledVulkanValidationLayers = true;
#endif

#ifndef _DEBUG
	const bool bIsEnabledVulkanValidationLayers = false;
#endif 

	JobSystem::Scheduler::Initialize();

	AssetRegistry::Initialize();
	TextureImporter::Initialize();
	ShaderCompiler::Initialize();
	ModelImporter::Initialize();
	Renderer::Initialize(&m_pInstance->m_viewportWindow, RHI::EMsaaSamples::Samples_4, bIsEnabledVulkanValidationLayers);
	ShaderCompiler::GetInstance();
	Framework::Initialize();

	SAILOR_LOG("Sailor Engine initialized");
}

void EngineInstance::Start()
{
	m_pInstance->m_viewportWindow.SetActive(true);
	m_pInstance->m_viewportWindow.SetRunning(true);

	uint32_t frameCounter = 0U;
	Utils::Timer timer;
	FrameState currentFrame;
	FrameState lastFrame;
	bool bCanCreateNewFrame = true;


	while (m_pInstance->m_viewportWindow.IsRunning())
	{
		timer.Start();

		Win32::ConsoleWindow::GetInstance()->Update();
		Win32::Window::ProcessWin32Msgs();
		Renderer::GetInstance()->FixLostDevice();

		if (GlobalInput::GetInputState().IsKeyPressed(VK_ESCAPE))
		{
			Stop();
			break;
		}

		if (bCanCreateNewFrame)
		{
			if (GlobalInput::GetInputState().IsButtonDown(VK_LBUTTON))
			{
				ivec2 centerPosition = m_pInstance->m_viewportWindow.GetCenterPointScreen();
				GlobalInput::SetCursorPos(centerPosition.x, centerPosition.y);
			}

			FrameInputState inputState = (Sailor::FrameInputState)GlobalInput::GetInputState();
			currentFrame = FrameState(Utils::GetCurrentTimeMs(), inputState, m_pInstance->m_viewportWindow.GetCenterPointClient(), &lastFrame);
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

			m_pInstance->m_viewportWindow.SetWindowTitle(Buff);

			frameCounter = 0U;
			timer.Clear();

			SAILOR_PROFILE_END_BLOCK();
		}
	}

	m_pInstance->m_viewportWindow.SetActive(false);
	m_pInstance->m_viewportWindow.SetRunning(false);
}

void EngineInstance::Stop()
{
	m_pInstance->m_viewportWindow.SetActive(false);
}

void EngineInstance::Shutdown()
{
	SAILOR_LOG("Sailor Engine Releasing");

	JobSystem::Scheduler::Shutdown();
	Framework::Shutdown();
	AssetRegistry::Shutdown();
	Renderer::Shutdown();
	Win32::ConsoleWindow::Shutdown();
	ShaderCompiler::Shutdown();

	delete m_pInstance;
}

Win32::Window& EngineInstance::GetViewportWindow()
{
	return m_pInstance->m_viewportWindow;
}

std::mutex m_logMutex;
