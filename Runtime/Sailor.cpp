#include "Sailor.h"
#include "Foundation/Input.h"

using namespace Sailor;

GEngineInstance* GEngineInstance::Instance = nullptr;
std::unique_ptr<AWindow> GEngineInstance::ViewportWindow;

void GEngineInstance::Initialize()
{
	if (Instance != nullptr)
	{
		return;
	}
	
	Instance = new GEngineInstance();
	Instance->ViewportWindow = std::make_unique<AWindow>();

	Instance->ViewportWindow->Create(L"Sailor Viewport", 1024, 768);
}

void GEngineInstance::Run()
{
	MSG msg;

	ViewportWindow->SetActive(true);
	ViewportWindow->SetRunning(true);

	float DeltaTime = 0.0f;
	float BeginFrameTime = 0.0f;

	while (ViewportWindow->IsRunning())
	{
		while (PeekMessage(&msg, ViewportWindow->GetHWND(), 0, 0, PM_REMOVE))
		{
			if (msg.message == WM_QUIT)
			{
				ViewportWindow->SetRunning(false);
				break;
			}
			DispatchMessage(&msg);
		}

		BeginFrameTime = (float)GetTickCount();

		if (GInput::IsKeyPressed(VK_ESCAPE))
		{
			break;
		}

		if (ViewportWindow->IsRunning() && ViewportWindow->IsActive())
		{

			SwapBuffers(ViewportWindow->GetHDC());

			DeltaTime += GetTickCount() - BeginFrameTime;

			Instance->ElapsedTime += (float)(GetTickCount() - BeginFrameTime) / 1000.0f;
			++Instance->FPS;

			if (Instance->ElapsedTime >= 1.0f)
			{
				WCHAR buff[50];
				wsprintf(buff, L"Sailor FPS: %u", Instance->FPS);

				ViewportWindow->SetWindowTitle(buff);
				Instance->FPS = 0;
				Instance->ElapsedTime = 0.0f;
			}
		}
	}

	ViewportWindow->SetActive(false);
	ViewportWindow->SetRunning(false);
}

void GEngineInstance::Stop()
{
	ViewportWindow->SetActive(false);
}

void GEngineInstance::Release()
{
	delete Instance;
}

AWindow& GEngineInstance::GetViewportWindow()
{
	return *ViewportWindow;
}

