#pragma once
#include "Platform/Window.h"
#include <atomic>
#include <mutex>
#include <thread>
#include <functional>
#include <vector>

struct GLFWwindow;

namespace Sailor::Linux
{
	class Window final : public Platform::Window
	{
	public:
		SAILOR_API ~Window() override;
		SAILOR_API bool IsShown() const override;
		SAILOR_API bool IsResizing() const override;
		SAILOR_API bool IsActive() const override;
		SAILOR_API bool IsRunning() const override;
		SAILOR_API bool IsFullscreen() const override;
		SAILOR_API bool IsIconic() const override;

		SAILOR_API void SetActive(bool value) override;
		SAILOR_API void SetRunning(bool value) override;
		SAILOR_API void SetFullscreen(bool value) override;
		SAILOR_API void SetWindowTitle(const char* title) override;

		SAILOR_API void* GetNativeHandle() const override;

		SAILOR_API int32_t GetWidth() const override;
		SAILOR_API int32_t GetHeight() const override;
		SAILOR_API bool IsVsyncRequested() const override;

		SAILOR_API bool Create(const char* title = "Sailor", const char* className = "SailorViewport", int32_t width = 1920, int32_t height = 1080, bool bIsFullScreen = false, bool bRequestVsync = false, void* parentWindow = nullptr) override;
		SAILOR_API void Destroy() override;

		SAILOR_API glm::ivec2 GetRenderArea() const override;
		SAILOR_API void SetRenderArea(const glm::ivec2& renderArea) override;

		SAILOR_API bool IsParentWindowValid() const override;

		SAILOR_API void Show(bool bShowWindow) override;

		SAILOR_API glm::ivec2 GetCenterPointScreen() const override;
		SAILOR_API glm::ivec2 GetCenterPointClient() const override;
		SAILOR_API void RecalculateWindowSize() override;
		SAILOR_API void ChangeWindowSize(int32_t width, int32_t height, bool bIsFullScreen = false) override;
		SAILOR_API void ProcessSystemMessages() override;

		SAILOR_API void RequestMouseCapture(bool value) override;
		SAILOR_API bool IsMouseCaptured() const override;
		SAILOR_API glm::vec2 ConsumeMouseDelta() override;
		GLFWwindow* GetGlfwWindow() const { return m_window; }
		void AddMouseDelta(float x, float y);
	private:
		GLFWwindow* m_window = nullptr;
		std::atomic<int32_t> m_width = 1024;
		std::atomic<int32_t> m_height = 768;
		std::atomic<bool> m_running = false;
		std::atomic<bool> m_shown = false;
		std::atomic<bool> m_active = false;
		std::atomic<bool> m_fullscreen = false;
		std::atomic<bool> m_resizing = false;
		std::atomic<bool> m_iconic = false;
		std::atomic<bool> m_captureRequested = false;
		std::atomic<bool> m_captured = false;
		bool m_vsync = false;
		bool m_hasCursorPosition = false;
		double m_lastCursorX = 0;
		double m_lastCursorY = 0;
		std::atomic<int32_t> m_renderWidth = 1024;
		std::atomic<int32_t> m_renderHeight = 768;
		std::atomic<int32_t> m_positionX = 0;
		std::atomic<int32_t> m_positionY = 0;
		std::thread::id m_ownerThread;
		std::mutex m_commandsMutex;
		std::vector<std::function<void()>> m_pendingCommands;
		bool DeferToOwner(std::function<void()> command);
		std::mutex m_mouseMutex;
		glm::vec2 m_mouseDelta{};
	};
}
