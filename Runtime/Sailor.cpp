#include "Sailor.h"
#include "Foundation/Input.h"

using namespace Sailor;

GEngineInstance* GEngineInstance::Instance = nullptr;

void GEngineInstance::Initialize()
{
	if (Instance != nullptr)
	{
		return;
	}

	AConsoleWindow::Initialize(false);
	
#ifdef _DEBUG
	AConsoleWindow::GetInstance().OpenWindow(L"Sailor Console");
#endif
	
	Instance = new GEngineInstance();
	Instance->ViewportWindow.Create(L"Sailor Viewport", 1024, 768);	
}

void GEngineInstance::Start()
{
	MSG msg;

	Instance->ViewportWindow.SetActive(true);
	Instance->ViewportWindow.SetRunning(true);

	float DeltaTime = 0.0f;
	float BeginFrameTime = 0.0f;

	while (Instance->ViewportWindow.IsRunning())
	{
		AConsoleWindow::GetInstance().Update();
		
		while (PeekMessage(&msg, Instance->ViewportWindow.GetHWND(), 0, 0, PM_REMOVE))
		{
			if (msg.message == WM_QUIT)
			{
				Instance->ViewportWindow.SetRunning(false);
				break;
			}
			DispatchMessage(&msg);
		}

		BeginFrameTime = GetTickCount();

		if (GInput::IsKeyPressed(VK_ESCAPE))
		{
			break;
		}

		if (Instance->ViewportWindow.IsRunning() && Instance->ViewportWindow.IsActive())
		{
			SwapBuffers(Instance->ViewportWindow.GetHDC());

			DeltaTime += GetTickCount() - BeginFrameTime;

			Instance->ElapsedTime += (float)(GetTickCount() - BeginFrameTime) / 1000.0f;
			++Instance->FPS;

			if (Instance->ElapsedTime >= 1.0f)
			{
				WCHAR buff[50];
				wsprintf(buff, L"Sailor FPS: %u", Instance->FPS);

				Instance->ViewportWindow.SetWindowTitle(buff);
				Instance->FPS = 0;
				Instance->ElapsedTime = 0.0f;
			}
		}
	}
	
	Instance->ViewportWindow.SetActive(false);
	Instance->ViewportWindow.SetRunning(false);
}

void GEngineInstance::Stop()
{
	Instance->ViewportWindow.SetActive(false);
}

void GEngineInstance::Release()
{
	AConsoleWindow::GetInstance().CloseWindow();
	AConsoleWindow::GetInstance().Release();
	delete Instance;
}

AWindow& GEngineInstance::GetViewportWindow()
{
	return Instance->ViewportWindow;
}

