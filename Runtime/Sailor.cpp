#include "Sailor.h"
#include "AssetRegistry/AssetRegistry.h"
#include "AssetRegistry/ShaderCompiler.h"
#include "Platform/Win/ConsoleWindow.h"
#include "Platform/Win/Input.h"
#include "GfxDevice/Vulkan/VulkanApi.h"
#include "JobSystem/JobSystem.h"
#include "RHI/Renderer.h"

using namespace Sailor;

EngineInstance* EngineInstance::m_pInstance = nullptr;
const std::string EngineInstance::ApplicationName = "SailorEngine";
const std::string EngineInstance::EngineName = "Sailor";

void EngineInstance::Initialize()
{
	SAILOR_PROFILE_FUNCTION()

		if (m_pInstance != nullptr)
		{
			return;
		}

#ifdef BUILD_WITH_EASY_PROFILER
	profiler::startListen();
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
	ShaderCompiler::Initialize();
	Renderer::Initialize(&m_pInstance->m_viewportWindow, bIsEnabledVulkanValidationLayers);
	ShaderCompiler::GetInstance();

	SAILOR_LOG("Sailor Engine initialized");
}

void EngineInstance::Start()
{
	m_pInstance->m_viewportWindow.SetActive(true);
	m_pInstance->m_viewportWindow.SetRunning(true);

	Renderer::GetInstance()->RunRenderLoop();

	float timeToUpdateFps = 0.0f;
	while (m_pInstance->m_viewportWindow.IsRunning())
	{
		Win32::ConsoleWindow::GetInstance()->Update();
		Win32::Window::ProcessWin32Msgs();

		if (Input::IsKeyPressed(VK_ESCAPE))
		{
			break;
		}

		static float totalFramesCount = 0.0f;
		static float totalTime = 0.0f;

		const float beginFrameTime = (float)GetTickCount();
		
		SAILOR_PROFILE_BLOCK("Frame");
		Sleep(100);
		SAILOR_PROFILE_END_BLOCK();

		totalTime += (float)GetTickCount() - beginFrameTime;
		totalFramesCount++;

		if (totalTime > 1000)
		{
			SAILOR_PROFILE_BLOCK("Track FPS");

			WCHAR Buff[50];
			wsprintf(Buff, L"Sailor GPU FPS: %u, CPU FPS: %u", Renderer::GetInstance()->GetSmoothFps(), (uint32_t)totalFramesCount);
			m_pInstance->m_viewportWindow.SetWindowTitle(Buff);
			m_pInstance->m_FPS = totalFramesCount;

			totalFramesCount = 0;
			totalTime = 0;

			SAILOR_PROFILE_END_BLOCK();
		}
	}

	Renderer::GetInstance()->StopRenderLoop();

	m_pInstance->m_viewportWindow.SetActive(false);
	m_pInstance->m_viewportWindow.SetRunning(false);
}

void EngineInstance::Stop()
{
	m_pInstance->m_viewportWindow.SetActive(false);
}

void EngineInstance::Shutdown()
{
	SAILOR_LOG("Sailor Engine Released");
	JobSystem::Scheduler::Shutdown();
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
