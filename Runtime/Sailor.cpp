#include "Sailor.h"
#include "AssetRegistry/AssetRegistry.h"
#include "AssetRegistry/ShaderCompiler.h"
#include "Foundation/ConsoleWindow.h"
#include "Foundation/Input.h"
#include "GfxDevice/GfxDeviceVulkan.h"

using namespace Sailor;

EngineInstance* EngineInstance::m_pInstance = nullptr;
const std::string EngineInstance::ApplicationName = "SailorEngine";
const std::string EngineInstance::EngineName = "Sailor";

void EngineInstance::Initialize()
{
	if (m_pInstance != nullptr)
	{
		return;
	}

	ConsoleWindow::Initialize(false);

#ifdef _DEBUG
	ConsoleWindow::GetInstance()->OpenWindow(L"Sailor Console");
#endif

	m_pInstance = new EngineInstance();
	m_pInstance->m_viewportWindow.Create(L"Sailor Viewport", 1024, 768);

#ifdef _DEBUG
	const bool bIsEnabledVulkanValidationLayers = true;
#endif

#ifndef _DEBUG
	const bool bIsEnabledVulkanValidationLayers = false;
#endif 

	GfxDeviceVulkan::Initialize(&m_pInstance->m_viewportWindow, bIsEnabledVulkanValidationLayers);
	AssetRegistry::Initialize();	
	ShaderCompiler::Initialize();

	ShaderCompiler::GetInstance();
	
	SAILOR_LOG("Sailor Engine initialized");
}

void EngineInstance::Start()
{
	MSG msg;

	m_pInstance->m_viewportWindow.SetActive(true);
	m_pInstance->m_viewportWindow.SetRunning(true);

	float deltaTime = 0.0f;
	float beginFrameTime = 0.0f;

	while (m_pInstance->m_viewportWindow.IsRunning())
	{
		ConsoleWindow::GetInstance()->Update();

		while (PeekMessage(&msg, m_pInstance->m_viewportWindow.GetHWND(), 0, 0, PM_REMOVE))
		{
			if (msg.message == WM_QUIT)
			{
				m_pInstance->m_viewportWindow.SetRunning(false);
				break;
			}
			DispatchMessage(&msg);
		}

		beginFrameTime = GetTickCount();

		if (Input::IsKeyPressed(VK_ESCAPE))
		{
			break;
		}

		if (m_pInstance->m_viewportWindow.IsRunning() && m_pInstance->m_viewportWindow.IsActive())
		{
			SwapBuffers(m_pInstance->m_viewportWindow.GetHDC());

			deltaTime += GetTickCount() - beginFrameTime;

			m_pInstance->m_elapsedTime += (float)(GetTickCount() - beginFrameTime) / 1000.0f;
			++m_pInstance->m_FPS;

			if (m_pInstance->m_elapsedTime >= 1.0f)
			{
				WCHAR Buff[50];
				wsprintf(Buff, L"Sailor FPS: %u", m_pInstance->m_FPS);

				m_pInstance->m_viewportWindow.SetWindowTitle(Buff);
				m_pInstance->m_FPS = 0;
				m_pInstance->m_elapsedTime = 0.0f;
			}
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
	SAILOR_LOG("Sailor Engine Released");
	AssetRegistry::Shutdown();
	GfxDeviceVulkan::Shutdown();
	ConsoleWindow::Shutdown();
	ShaderCompiler::Shutdown();

	delete m_pInstance;
}

Window& EngineInstance::GetViewportWindow()
{
	return m_pInstance->m_viewportWindow;
}

