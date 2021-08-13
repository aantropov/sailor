#include "Sailor.h"
#include "AssetRegistry/AssetRegistry.h"
#include "AssetRegistry/ShaderCompiler.h"
#include "Foundation/ConsoleWindow.h"
#include "Foundation/Input.h"
#include "GfxDevice/GfxDeviceVulkan.h"
#include "JobSystem/JobSystem.h"

using namespace Sailor;

EngineInstance* EngineInstance::m_pInstance = nullptr;
const std::string EngineInstance::ApplicationName = "SailorEngine";
const std::string EngineInstance::EngineName = "Sailor";

void EngineInstance::Initialize()
{	
	EASY_FUNCTION()

	if (m_pInstance != nullptr)
	{
		return;
	}

	profiler::startListen();

	ConsoleWindow::Initialize(false);

	//#ifdef _DEBUG
	ConsoleWindow::GetInstance()->OpenWindow(L"Sailor Console");
	//#endif

	m_pInstance = new EngineInstance();
	m_pInstance->m_viewportWindow.Create(L"Sailor Viewport", 1024, 768);

#ifdef _DEBUG
	const bool bIsEnabledVulkanValidationLayers = true;
#endif

#ifndef _DEBUG
	const bool bIsEnabledVulkanValidationLayers = false;
#endif 

	JobSystem::Scheduler::Initialize();

	AssetRegistry::Initialize();
	ShaderCompiler::Initialize();
	GfxDeviceVulkan::Initialize(&m_pInstance->m_viewportWindow, bIsEnabledVulkanValidationLayers);

	ShaderCompiler::GetInstance();

	SAILOR_LOG("Sailor Engine initialized");
}

void EngineInstance::Start()
{
	MSG msg;

	m_pInstance->m_viewportWindow.SetActive(true);
	m_pInstance->m_viewportWindow.SetRunning(true);

	std::shared_ptr<Sailor::JobSystem::Job> renderingJob;

	while (m_pInstance->m_viewportWindow.IsRunning())
	{
		ConsoleWindow::GetInstance()->Update();

		EASY_BLOCK("Process window's messages");

		while (PeekMessage(&msg, m_pInstance->m_viewportWindow.GetHWND(), 0, 0, PM_REMOVE))
		{
			if (msg.message == WM_QUIT)
			{
				m_pInstance->m_viewportWindow.SetRunning(false);
				break;
			}
			DispatchMessage(&msg);
		}

		EASY_END_BLOCK;

		if (Input::IsKeyPressed(VK_ESCAPE))
		{
			break;
		}

		if (m_pInstance->m_viewportWindow.IsRunning())// && m_pInstance->m_viewportWindow.IsActive())
		{
			EASY_BLOCK("Draw frame");

			//renderingJob = JobSystem::Scheduler::CreateJob("Draw Frame",
			//	[]() {

				EASY_FUNCTION();

				float beginFrameTime = (float)GetTickCount();
				float deltaTime = GetTickCount() - beginFrameTime;

				GfxDeviceVulkan::GetInstance()->DrawFrame(&m_pInstance->m_viewportWindow);

				m_pInstance->m_elapsedTime += (float)(GetTickCount() - beginFrameTime) / 1000.0f;
				++m_pInstance->m_FPS;
				//}, Sailor::JobSystem::EThreadType::Rendering);
			//JobSystem::Scheduler::GetInstance()->Run(renderingJob);

			EASY_END_BLOCK;
		}

		EASY_BLOCK("Track FPS");

		if (m_pInstance->m_elapsedTime >= 1.0f)
		{
			WCHAR Buff[50];
			wsprintf(Buff, L"Sailor FPS: %u", m_pInstance->m_FPS);

			m_pInstance->m_viewportWindow.SetWindowTitle(Buff);
			m_pInstance->m_FPS = 0;
			m_pInstance->m_elapsedTime = 0.0f;
		}

		EASY_END_BLOCK;
	}

	GfxDeviceVulkan::WaitIdle();

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
	AssetRegistry::Shutdown();
	GfxDeviceVulkan::Shutdown();
	ConsoleWindow::Shutdown();
	ShaderCompiler::Shutdown();
	JobSystem::Scheduler::Shutdown();

	delete m_pInstance;
}

Window& EngineInstance::GetViewportWindow()
{
	return m_pInstance->m_viewportWindow;
}

std::mutex m_logMutex;
