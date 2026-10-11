#pragma once

#include "Platform/Window.h"
#include "Containers/Containers.h"

#include <atomic>
#include <mutex>

namespace Sailor::Mac
{
	SAILOR_SHARED_API void* GetNativeView(void* nativeWindow);
	SAILOR_SHARED_API void* GetMetalLayer(void* nativeWindow, bool bVsyncRequested);

	class Window : public Platform::Window
	{
	public:
		SAILOR_API Window() = default;
		SAILOR_API ~Window() override;

		SAILOR_API bool IsShown() const override { return m_bIsShown; }
		SAILOR_API bool IsResizing() const override { return m_bIsResizing; }
		SAILOR_API bool IsActive() const override { return m_bIsActive; }
		SAILOR_API bool IsRunning() const override { return m_bIsRunning; }
		SAILOR_API bool IsFullscreen() const override { return m_bIsFullscreen; }
		SAILOR_API bool IsIconic() const override;

		SAILOR_API void SetActive(bool value) override { m_bIsActive = value; }
		SAILOR_API void SetRunning(bool value) override { m_bIsRunning = value; }
		SAILOR_API void SetFullscreen(bool value) override { m_bIsFullscreen = value; }
		SAILOR_API void SetWindowTitle(const char* title) override;

		SAILOR_API void* GetNativeHandle() const override { return m_nativeWindow; }
		SAILOR_API void* GetMetalLayer() const;
		SAILOR_API void* GetNativeView() const;
		SAILOR_API void HandleNativeWindowWillClose(void* nativeWindow);

		SAILOR_API int32_t GetWidth() const override { return m_width; }
		SAILOR_API int32_t GetHeight() const override { return m_height; }
		SAILOR_API bool IsVsyncRequested() const override { return m_bIsVsyncRequested; }

		SAILOR_API bool Create(const char* title = "Sailor", const char* className = "SailorViewport", int32_t width = 1920, int32_t height = 1080, bool bIsFullScreen = false, bool bRequestVsync = false, void* parentWindow = nullptr) override;
		SAILOR_API void Destroy() override;

		SAILOR_API glm::ivec2 GetRenderArea() const override { return m_renderArea; }
		SAILOR_API void SetRenderArea(const glm::ivec2& renderArea) override { m_renderArea = renderArea; }
		SAILOR_API bool IsParentWindowValid() const override;
		SAILOR_API void Show(bool bShowWindow) override;

		SAILOR_API glm::ivec2 GetCenterPointScreen() const override;
		SAILOR_API glm::ivec2 GetCenterPointClient() const override;
		SAILOR_API void RecalculateWindowSize() override;
		SAILOR_API void ChangeWindowSize(int32_t width, int32_t height, bool bIsFullScreen = false) override;
		SAILOR_API void ProcessSystemMessages() override;

		SAILOR_API void RequestMouseCapture(bool value) override { m_bMouseCaptureRequested = value; }
		SAILOR_API bool IsMouseCaptured() const override { return m_bMouseCaptured; }
		SAILOR_API glm::vec2 ConsumeMouseDelta() override
		{
			const std::lock_guard<std::mutex> lock(m_mouseDeltaMutex);
			const glm::vec2 delta = m_mouseDelta;
			m_mouseDelta = {};
			return m_bMouseCaptured ? delta : glm::vec2(0.0f);
		}

		SAILOR_API void AddMouseDelta(float x, float y)
		{
			const std::lock_guard<std::mutex> lock(m_mouseDeltaMutex);
			if (m_bMouseCaptured)
			{
				m_mouseDelta += glm::vec2(x, y);
			}
		}

		SAILOR_API void UpdateMouseCapture();
		SAILOR_API static void ProcessMacMsgs();
		SAILOR_API static bool IsWindowAlive(const Window* pWindow);

	private:
		void* m_nativeWindow = nullptr;
		void* m_parentWindow = nullptr;
		std::atomic<int> m_width = 1024;
		std::atomic<int> m_height = 768;
		std::atomic<bool> m_bIsShown = true;
		std::atomic<bool> m_bIsFullscreen = false;
		std::atomic<bool> m_bIsActive = false;
		std::atomic<bool> m_bIsRunning = false;
		std::atomic<bool> m_bIsIconic = false;
		std::atomic<bool> m_bIsResizing = false;
		std::atomic<bool> m_bIsVsyncRequested = false;
		std::atomic<bool> m_bMouseCaptureRequested = false;
		std::atomic<bool> m_bMouseCaptured = false;
		glm::ivec2 m_renderArea{};
		std::mutex m_mouseDeltaMutex;
		glm::vec2 m_mouseDelta{};

		SAILOR_API void SetIsIconic(bool value) { m_bIsIconic = value; }
		static TVector<Window*> g_windows;
	};
}
