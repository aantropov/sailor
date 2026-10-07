#pragma once

#include "Platform/Window.h"
#if defined(_WIN32)
#include <wtypes.h>
#endif
#include <atomic>
#include <mutex>
#include <optional>
#include <string>
#include "Containers/Containers.h"

namespace Sailor::Utils
{
	struct WindowSizeAndPosition
	{
		RECT m_windowRect; // Includes title bar, borders, etc.
		RECT m_clientRect; // Only the client area
		int32_t m_width;
		int32_t m_height;
		int32_t m_clientWidth;
		int32_t m_clientHeight;
		int32_t m_xPos;
		int32_t m_yPos;
	};

	SAILOR_API WindowSizeAndPosition GetWindowSizeAndPosition(HWND hwnd);
}

namespace Sailor::Win32
{
	LRESULT CALLBACK WindowProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
}

namespace Sailor::Win32
{
	class Window
		: public Platform::Window
	{
		HINSTANCE m_hInstance = nullptr;
		HWND      m_hWnd = nullptr;
		HDC       m_hDC = nullptr;

		HWND      m_parentHwnd = nullptr;

#if defined(_WIN32)
		ATOM m_windowClassAtom = 0;
		uint32_t m_nativeMouseButtons = 0;
		IUnknown* m_editorViewportDropTarget = nullptr;
		bool m_bEditorViewportDropOleInitialized = false;

		struct EditorViewportAssetDrop
		{
			std::string m_fileId{};
			float m_normalizedX = 0.0f;
			float m_normalizedY = 0.0f;
		};

		std::mutex m_editorViewportAssetDropMutex{};
		std::optional<EditorViewportAssetDrop>
			m_pendingEditorViewportAssetDrop{};
		std::mutex m_editorViewportToolShortcutMutex{};
		TVector<uint32_t> m_pendingEditorViewportToolShortcuts{};
#endif

		std::atomic<int> m_width = 1024;
		std::atomic<int> m_height = 768;
		std::string m_windowClassName;

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

	public:

		SAILOR_API Window() = default;
#if defined(_WIN32) || defined(__APPLE__)
		SAILOR_API ~Window() override;
#else
		SAILOR_API ~Window() override = default;
#endif

		SAILOR_API bool IsShown() const override { return m_bIsShown; }
		SAILOR_API bool IsResizing() const override { return m_bIsResizing; }
		SAILOR_API bool IsActive() const override { return m_bIsActive; }
		SAILOR_API bool IsRunning() const override { return m_bIsRunning; }
		SAILOR_API bool IsFullscreen() const override { return m_bIsFullscreen; }
		SAILOR_API bool IsIconic() const override;

		SAILOR_API void SetActive(bool value) override { m_bIsActive = value; }
		SAILOR_API void SetRunning(bool value) override { m_bIsRunning = value; }
		SAILOR_API void SetFullscreen(bool value) override { m_bIsFullscreen = value; }
#if defined(__APPLE__)
		SAILOR_API void SetWindowTitle(const char* title) override;
#else
		SAILOR_API void SetWindowTitle(const char* title) override {
#if defined(_WIN32)
			SetWindowText(m_hWnd, title);
#else
			(void)title;
#endif
		}
#endif

		SAILOR_API void* GetNativeHandle() const override { return m_hWnd; }
		SAILOR_API HWND GetHWND() const { return m_hWnd; }
		SAILOR_API HDC GetHDC() const { return m_hDC; }
		SAILOR_API HINSTANCE GetHINSTANCE() const { return m_hInstance; }
#if defined(__APPLE__)
		SAILOR_API void* GetMetalLayer() const;
		SAILOR_API void* GetNativeView() const;
		SAILOR_API void HandleNativeWindowWillClose(HWND nativeWindow);
#endif

		SAILOR_API int32_t GetWidth() const override { return m_width; }
		SAILOR_API int32_t GetHeight() const override { return m_height; }
		SAILOR_API bool IsVsyncRequested() const override { return m_bIsVsyncRequested; }

		SAILOR_API bool Create(const char* title = "Sailor", const char* className = "SailorViewport", int32_t width = 1920, int32_t height = 1080, bool bIsFullScreen = false, bool bRequestVsync = false, void* parentWindow = nullptr) override;
		SAILOR_API void Destroy() override;

		SAILOR_API glm::ivec2 GetRenderArea() const override { return m_renderArea; }
		SAILOR_API void SetRenderArea(const glm::ivec2& renderArea) override { m_renderArea = renderArea; }

		SAILOR_API bool IsParentWindowValid() const override;
		SAILOR_API void TrackParentWindowPosition(const RECT& viewport);

		SAILOR_API void Show(bool bShowWindow) override;

		// Window size
		SAILOR_API glm::ivec2 GetCenterPointScreen() const override;
		SAILOR_API glm::ivec2 GetCenterPointClient() const override;
		SAILOR_API void SetWindowPos(const RECT& rect);
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

		// Called on the window thread after processing focus and input events.
		SAILOR_API void UpdateMouseCapture();

		SAILOR_API static void ProcessWin32Msgs();
		SAILOR_API static bool IsWindowAlive(const Window* pWindow);
#if defined(_WIN32)
		void QueueEditorViewportAssetDrop(
			std::string fileId,
			float normalizedX,
			float normalizedY);
		bool PullEditorViewportAssetDrop(
			std::string& outFileId,
			float& outNormalizedX,
			float& outNormalizedY);
		void QueueEditorViewportToolShortcut(uint32_t keyCode);
		bool PullEditorViewportToolShortcut(uint32_t& outKeyCode);
#endif
#if defined(__APPLE__)
		SAILOR_API static void ProcessMacMsgs();
#endif

	private:

		RECT m_viewport{};

		SAILOR_API void SetIsIconic(bool value) { m_bIsIconic = value; }

		static TVector<Window*> g_windows;
		static std::mutex g_windowsMutex;

		friend LRESULT CALLBACK WindowProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
	};

}
