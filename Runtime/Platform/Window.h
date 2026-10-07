#pragma once

#include "Core/Defines.h"
#include "Math/Math.h"

namespace Sailor::Platform
{
	class Window
	{
	public:
		SAILOR_API virtual ~Window() = default;

		SAILOR_API virtual bool IsShown() const = 0;
		SAILOR_API virtual bool IsResizing() const = 0;
		SAILOR_API virtual bool IsActive() const = 0;
		SAILOR_API virtual bool IsRunning() const = 0;
		SAILOR_API virtual bool IsFullscreen() const = 0;
		SAILOR_API virtual bool IsIconic() const = 0;

		SAILOR_API virtual void SetActive(bool value) = 0;
		SAILOR_API virtual void SetRunning(bool value) = 0;
		SAILOR_API virtual void SetFullscreen(bool value) = 0;
		SAILOR_API virtual void SetWindowTitle(const char* title) = 0;

		SAILOR_API virtual void* GetNativeHandle() const = 0;
#if defined(__APPLE__)
		SAILOR_API virtual void* GetMetalLayer() const = 0;
		SAILOR_API virtual void* GetNativeView() const = 0;
#endif

		SAILOR_API virtual int32_t GetWidth() const = 0;
		SAILOR_API virtual int32_t GetHeight() const = 0;
		SAILOR_API virtual bool IsVsyncRequested() const = 0;

		SAILOR_API virtual bool Create(const char* title = "Sailor", const char* className = "SailorViewport", int32_t width = 1920, int32_t height = 1080, bool bIsFullScreen = false, bool bRequestVsync = false, void* parentWindow = nullptr) = 0;
		SAILOR_API virtual void Destroy() = 0;

		SAILOR_API virtual glm::ivec2 GetRenderArea() const = 0;
		SAILOR_API virtual void SetRenderArea(const glm::ivec2& renderArea) = 0;

		SAILOR_API virtual bool IsParentWindowValid() const = 0;

		SAILOR_API virtual void Show(bool bShowWindow) = 0;

		SAILOR_API virtual glm::ivec2 GetCenterPointScreen() const = 0;
		SAILOR_API virtual glm::ivec2 GetCenterPointClient() const = 0;
		SAILOR_API virtual void RecalculateWindowSize() = 0;
		SAILOR_API virtual void ChangeWindowSize(int32_t width, int32_t height, bool bIsFullScreen = false) = 0;
		SAILOR_API virtual void ProcessSystemMessages() = 0;

		SAILOR_API virtual void RequestMouseCapture(bool value) = 0;
		SAILOR_API virtual bool IsMouseCaptured() const = 0;
		SAILOR_API virtual glm::vec2 ConsumeMouseDelta() = 0;
	};
}
