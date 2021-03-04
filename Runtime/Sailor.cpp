#include "Sailor.h"
#include "Foundation/Input.h"
#include "GfxDevice/GfxDeviceVulkan.h"

using namespace Sailor;

EngineInstance* EngineInstance::instance = nullptr;

void EngineInstance::Initialize()
{
	if (instance != nullptr)
	{
		return;
	}

	ConsoleWindow::Initialize(false);
	
#ifdef _DEBUG
	ConsoleWindow::GetInstance().OpenWindow(L"Sailor Console");
#endif
	
	instance = new EngineInstance();
	instance->viewportWindow.Create(L"Sailor Viewport", 1024, 768);
	
	SAILOR_LOG("Sailor Engine initialized");
	SAILOR_LOG("Num supported Vulkan extensions: %d", GfxDeviceVulkan::GetNumSupportedExtensions());
}

void EngineInstance::Start()
{
	MSG msg;

	instance->viewportWindow.SetActive(true);
	instance->viewportWindow.SetRunning(true);

	float deltaTime = 0.0f;
	float beginFrameTime = 0.0f;

	while (instance->viewportWindow.IsRunning())
	{
		ConsoleWindow::GetInstance().Update();
		
		while (PeekMessage(&msg, instance->viewportWindow.GetHWND(), 0, 0, PM_REMOVE))
		{
			if (msg.message == WM_QUIT)
			{
				instance->viewportWindow.SetRunning(false);
				break;
			}
			DispatchMessage(&msg);
		}

		beginFrameTime = GetTickCount();

		if (Input::IsKeyPressed(VK_ESCAPE))
		{
			break;
		}

		if (instance->viewportWindow.IsRunning() && instance->viewportWindow.IsActive())
		{
			SwapBuffers(instance->viewportWindow.GetHDC());

			deltaTime += GetTickCount() - beginFrameTime;

			instance->elapsedTime += (float)(GetTickCount() - beginFrameTime) / 1000.0f;
			++instance->FPS;

			if (instance->elapsedTime >= 1.0f)
			{
				WCHAR Buff[50];
				wsprintf(Buff, L"Sailor FPS: %u", instance->FPS);

				instance->viewportWindow.SetWindowTitle(Buff);
				instance->FPS = 0;
				instance->elapsedTime = 0.0f;
			}
		}
	}
	
	instance->viewportWindow.SetActive(false);
	instance->viewportWindow.SetRunning(false);
}

void EngineInstance::Stop()
{
	instance->viewportWindow.SetActive(false);
}

void EngineInstance::Release()
{
	SAILOR_LOG("Sailor Engine Released");
	ConsoleWindow::GetInstance().CloseWindow();
	ConsoleWindow::GetInstance().Release();
	delete instance;
}

Window& EngineInstance::GetViewportWindow()
{
	return instance->viewportWindow;
}

