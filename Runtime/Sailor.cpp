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

	Renderer::GetInstance()->RunRenderLoop();

	float timeToUpdateFps = 0.0f;
	while (m_pInstance->m_viewportWindow.IsRunning())
	{
		Win32::ConsoleWindow::GetInstance()->Update();
		Win32::Window::ProcessWin32Msgs();
		Renderer::GetInstance()->FixLostDevice();

		if (GlobalInput::GetInputState().IsKeyPressed(VK_ESCAPE))
		{
			Stop();
			break;
		}

		FrameInputState inputState = (Sailor::FrameInputState)GlobalInput::GetInputState();
		FrameState currentFrame(Utils::GetCurrentTimeMs() / 1000.0f, inputState);

		Framework::GetInstance()->ProcessCpuFrame(currentFrame);
		Renderer::GetInstance()->PushFrame(currentFrame);

		static int64_t updateInterval = 0;
		if (Utils::GetCurrentTimeMicro() - updateInterval > 1000000)
		{
			SAILOR_PROFILE_BLOCK("Track FPS");

			WCHAR Buff[50];
			wsprintf(Buff, L"Sailor GPU FPS: %u, CPU FPS: %u", Renderer::GetInstance()->GetSmoothFps(), (uint32_t)Framework::GetInstance()->GetSmoothFps());
			m_pInstance->m_viewportWindow.SetWindowTitle(Buff);

			updateInterval = Utils::GetCurrentTimeMicro();

			SAILOR_PROFILE_END_BLOCK();
		}
	}

	m_pInstance->m_viewportWindow.SetActive(false);
	m_pInstance->m_viewportWindow.SetRunning(false);
}

void EngineInstance::Stop()
{
	m_pInstance->m_viewportWindow.SetActive(false);

	Renderer::GetInstance()->StopRenderLoop();
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
