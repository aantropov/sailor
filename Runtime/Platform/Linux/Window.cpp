#if defined(__linux__)

#include "Platform/Linux/Window.h"
#include "Platform/Win32/Input.h"
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <algorithm>
#include <cmath>

namespace Sailor::Linux
{
	static int g_glfwInitCount = 0;

	static bool EnsureGlfwInit()
	{
		if (g_glfwInitCount == 0 && glfwInit() != GLFW_TRUE)
		{
			return false;
		}
		g_glfwInitCount++;
		return true;
	}

	static void ReleaseGlfwInit()
	{
		if (g_glfwInitCount > 0)
		{
			g_glfwInitCount--;
			if (g_glfwInitCount == 0)
			{
				glfwTerminate();
			}
		}
	}

	static uint32_t MapGlfwKeyToVK(int key)
	{
		if ((key >= GLFW_KEY_A && key <= GLFW_KEY_Z) || (key >= GLFW_KEY_0 && key <= GLFW_KEY_9))
		{
			return static_cast<uint32_t>(key);
		}
		if (key >= GLFW_KEY_F1 && key <= GLFW_KEY_F24)
		{
			return static_cast<uint32_t>(0x70 + key - GLFW_KEY_F1);
		}
		if (key >= GLFW_KEY_KP_0 && key <= GLFW_KEY_KP_9)
		{
			return static_cast<uint32_t>(0x60 + key - GLFW_KEY_KP_0);
		}
		switch (key)
		{
		case GLFW_KEY_SPACE: return 0x20;
		case GLFW_KEY_APOSTROPHE: return 0xDE;
		case GLFW_KEY_COMMA: return 0xBC;
		case GLFW_KEY_MINUS: return 0xBD;
		case GLFW_KEY_PERIOD: return 0xBE;
		case GLFW_KEY_SLASH: return 0xBF;
		case GLFW_KEY_SEMICOLON: return 0xBA;
		case GLFW_KEY_EQUAL: return 0xBB;
		case GLFW_KEY_LEFT_BRACKET: return 0xDB;
		case GLFW_KEY_BACKSLASH: return 0xDC;
		case GLFW_KEY_RIGHT_BRACKET: return 0xDD;
		case GLFW_KEY_GRAVE_ACCENT: return 0xC0;
		case GLFW_KEY_ESCAPE: return 0x1B;
		case GLFW_KEY_ENTER: return 0x0D;
		case GLFW_KEY_TAB: return 0x09;
		case GLFW_KEY_BACKSPACE: return 0x08;
		case GLFW_KEY_INSERT: return 0x2D;
		case GLFW_KEY_DELETE: return 0x2E;
		case GLFW_KEY_RIGHT: return 0x27;
		case GLFW_KEY_LEFT: return 0x25;
		case GLFW_KEY_DOWN: return 0x28;
		case GLFW_KEY_UP: return 0x26;
		case GLFW_KEY_PAGE_UP: return 0x21;
		case GLFW_KEY_PAGE_DOWN: return 0x22;
		case GLFW_KEY_HOME: return 0x24;
		case GLFW_KEY_END: return 0x23;
		case GLFW_KEY_CAPS_LOCK: return 0x14;
		case GLFW_KEY_SCROLL_LOCK: return 0x91;
		case GLFW_KEY_NUM_LOCK: return 0x90;
		case GLFW_KEY_PRINT_SCREEN: return 0x2C;
		case GLFW_KEY_PAUSE: return 0x13;
		case GLFW_KEY_KP_DECIMAL: return 0x6E;
		case GLFW_KEY_KP_DIVIDE: return 0x6F;
		case GLFW_KEY_KP_MULTIPLY: return 0x6A;
		case GLFW_KEY_KP_SUBTRACT: return 0x6D;
		case GLFW_KEY_KP_ADD: return 0x6B;
		case GLFW_KEY_KP_ENTER: return 0x0D;
		case GLFW_KEY_KP_EQUAL: return 0xBB;
		case GLFW_KEY_LEFT_SHIFT: return VK_LSHIFT;
		case GLFW_KEY_LEFT_CONTROL: return VK_LCONTROL;
		case GLFW_KEY_LEFT_ALT: return VK_LMENU;
		case GLFW_KEY_LEFT_SUPER: return 0x5B;
		case GLFW_KEY_RIGHT_SHIFT: return VK_RSHIFT;
		case GLFW_KEY_RIGHT_CONTROL: return VK_RCONTROL;
		case GLFW_KEY_RIGHT_ALT: return VK_RMENU;
		case GLFW_KEY_RIGHT_SUPER: return 0x5C;
		case GLFW_KEY_MENU: return 0x5D;
		default: return 0;
		}
	}

	static bool IsKeypadKey(int key)
	{
		return key >= GLFW_KEY_KP_0 && key <= GLFW_KEY_KP_EQUAL;
	}

	Window::~Window()
	{
		Destroy();
	}

	bool Window::IsShown() const
	{
		return m_shown.load();
	}

	bool Window::IsResizing() const
	{
		return m_resizing.load();
	}

	bool Window::IsActive() const
	{
		return m_active.load();
	}

	bool Window::IsRunning() const
	{
		return m_running.load();
	}

	bool Window::IsFullscreen() const
	{
		return m_fullscreen.load();
	}

	bool Window::IsIconic() const
	{
		return m_iconic.load();
	}

	void Window::SetActive(bool value)
	{
		m_active.store(value);
	}

	void Window::SetRunning(bool value)
	{
		m_running.store(value);
	}

	void Window::SetFullscreen(bool value)
	{
		if (m_fullscreen.load() == value)
		{
			return;
		}
		m_fullscreen.store(value);
		if (m_window)
		{
			int32_t w = m_width.load();
			int32_t h = m_height.load();
			ChangeWindowSize(w, h, value);
		}
	}

	bool Window::DeferToOwner(std::function<void()> command)
	{
		if (std::this_thread::get_id() == m_ownerThread)
		{
			return false;
		}
		std::lock_guard lock(m_commandsMutex);
		m_pendingCommands.emplace_back(std::move(command));
		return true;
	}

	void Window::SetWindowTitle(const char* title)
	{
		if (std::this_thread::get_id() != m_ownerThread)
		{
			const std::string ownedTitle = title ? title : "";
			DeferToOwner([this, ownedTitle]() { SetWindowTitle(ownedTitle.c_str()); });
			return;
		}
		if (m_window && title)
		{
			glfwSetWindowTitle(m_window, title);
		}
	}

	void* Window::GetNativeHandle() const
	{
		return static_cast<void*>(m_window);
	}

	int32_t Window::GetWidth() const
	{
		return m_width.load();
	}

	int32_t Window::GetHeight() const
	{
		return m_height.load();
	}

	bool Window::IsVsyncRequested() const
	{
		return m_vsync;
	}

	bool Window::Create(const char* title, const char* className, int32_t width, int32_t height, bool bIsFullScreen, bool bRequestVsync, void* parentWindow)
	{
		if (parentWindow != nullptr)
		{
			return false;
		}

		if (m_window || width <= 0 || height <= 0 || !EnsureGlfwInit())
		{
			return false;
		}
		m_ownerThread = std::this_thread::get_id();

		glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);
		glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
		glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);

		GLFWmonitor* monitor = nullptr;
		if (bIsFullScreen)
		{
			monitor = glfwGetPrimaryMonitor();
		}

		m_window = glfwCreateWindow(width, height, title ? title : "Sailor", monitor, nullptr);
		if (!m_window)
		{
			ReleaseGlfwInit();
			return false;
		}

		m_width.store(width);
		m_height.store(height);
		m_vsync = bRequestVsync;
		m_fullscreen.store(bIsFullScreen);
		m_running.store(true);
		m_shown.store(false);
		m_active.store(false);
		m_iconic.store(false);
		m_resizing.store(false);
		m_captureRequested.store(false);
		m_captured.store(false);
		m_hasCursorPosition = false;
		m_lastCursorX = 0.0;
		m_lastCursorY = 0.0;
		SetRenderArea({ width, height });

		glfwShowWindow(m_window);
		m_shown.store(true);
		m_active = glfwGetWindowAttrib(m_window, GLFW_FOCUSED) == GLFW_TRUE;
		RecalculateWindowSize();
		SetRenderArea({ GetWidth(), GetHeight() });

		glfwSetWindowUserPointer(m_window, this);

		glfwSetWindowCloseCallback(m_window, [](GLFWwindow* win)
		{
			Window* self = static_cast<Window*>(glfwGetWindowUserPointer(win));
			if (self)
			{
				self->m_running.store(false);
				Sailor::Platform::InputEvent evt{};
				evt.m_type = Sailor::Platform::InputEvent::Type::Reset;
				Sailor::Win32::GlobalInput::QueueNativeEvent(evt);
			}
		});

		glfwSetWindowFocusCallback(m_window, [](GLFWwindow* win, int focused)
		{
			Window* self = static_cast<Window*>(glfwGetWindowUserPointer(win));
			if (self)
			{
				bool active = (focused != 0);
				self->m_active.store(active);
				Sailor::Platform::InputEvent evt{};
				evt.m_type = Sailor::Platform::InputEvent::Type::Focus;
				evt.m_bIsPressed = active;
				Sailor::Win32::GlobalInput::QueueNativeEvent(evt);
			}
		});

		glfwSetWindowIconifyCallback(m_window, [](GLFWwindow* win, int iconic)
		{
			auto* self = static_cast<Window*>(glfwGetWindowUserPointer(win));
			self->m_iconic = iconic == GLFW_TRUE;
		});

		glfwSetFramebufferSizeCallback(m_window, [](GLFWwindow* win, int w, int h)
		{
			Window* self = static_cast<Window*>(glfwGetWindowUserPointer(win));
			if (self)
			{
				self->m_width.store(w);
				self->m_height.store(h);
				self->SetRenderArea({ w, h });
				self->m_resizing.store(true);
			}
		});

		glfwSetCursorPosCallback(m_window, [](GLFWwindow* win, double xpos, double ypos)
		{
			Window* self = static_cast<Window*>(glfwGetWindowUserPointer(win));
			if (self)
			{
				int fbWidth, fbHeight;
				glfwGetFramebufferSize(win, &fbWidth, &fbHeight);
				int winWidth, winHeight;
				glfwGetWindowSize(win, &winWidth, &winHeight);

				float scaleX = (winWidth > 0) ? static_cast<float>(fbWidth) / static_cast<float>(winWidth) : 1.0f;
				float scaleY = (winHeight > 0) ? static_cast<float>(fbHeight) / static_cast<float>(winHeight) : 1.0f;

				float clientX = static_cast<float>(xpos) * scaleX;
				float clientY = static_cast<float>(ypos) * scaleY;

				if (self->m_hasCursorPosition)
				{
					float dx = static_cast<float>(xpos - self->m_lastCursorX);
					float dy = static_cast<float>(ypos - self->m_lastCursorY);
					if (self->m_captured && (dx != 0.0f || dy != 0.0f))
					{
						std::lock_guard<std::mutex> lock(self->m_mouseMutex);
						self->m_mouseDelta.x += dx * scaleX;
						self->m_mouseDelta.y += dy * scaleY;
					}
				}
				self->m_lastCursorX = xpos;
				self->m_lastCursorY = ypos;
				self->m_hasCursorPosition = true;

				Sailor::Platform::InputEvent evt{};
				evt.m_type = Sailor::Platform::InputEvent::Type::MousePos;
				evt.m_x = clientX;
				evt.m_y = clientY;
				Sailor::Win32::GlobalInput::QueueNativeEvent(evt);
			}
		});

		glfwSetMouseButtonCallback(m_window, [](GLFWwindow* win, int button, int action, int mods)
		{
			Window* self = static_cast<Window*>(glfwGetWindowUserPointer(win));
			if (self)
			{
				int32_t btnIndex = -1;
				if (button == GLFW_MOUSE_BUTTON_LEFT)
				{
					btnIndex = 0;
				}
				else if (button == GLFW_MOUSE_BUTTON_RIGHT)
				{
					btnIndex = 1;
				}
				else if (button == GLFW_MOUSE_BUTTON_MIDDLE)
				{
					btnIndex = 2;
				}

				if (btnIndex >= 0)
				{
					Sailor::Platform::InputEvent evt{};
					evt.m_type = Sailor::Platform::InputEvent::Type::MouseButton;
					evt.m_button = btnIndex;
					evt.m_bIsPressed = (action == GLFW_PRESS);
					Sailor::Win32::GlobalInput::QueueNativeEvent(evt);
				}
			}
		});

		glfwSetScrollCallback(m_window, [](GLFWwindow* win, double xoffset, double yoffset)
		{
			Window* self = static_cast<Window*>(glfwGetWindowUserPointer(win));
			if (self)
			{
				Sailor::Platform::InputEvent evt{};
				evt.m_type = Sailor::Platform::InputEvent::Type::MouseWheel;
				evt.m_x = static_cast<float>(xoffset);
				evt.m_y = static_cast<float>(yoffset);
				Sailor::Win32::GlobalInput::QueueNativeEvent(evt);
			}
		});

		glfwSetKeyCallback(m_window, [](GLFWwindow* win, int key, int scancode, int action, int mods)
		{
			Window* self = static_cast<Window*>(glfwGetWindowUserPointer(win));
			if (self)
			{
				uint32_t vk = MapGlfwKeyToVK(key);
				if (vk != 0)
				{
					Sailor::Platform::InputEvent evt{};
					evt.m_type = Sailor::Platform::InputEvent::Type::Key;
					evt.m_key = vk;
					evt.m_bIsPressed = (action == GLFW_PRESS || action == GLFW_REPEAT);
					evt.m_bIsKeypad = IsKeypadKey(key);
					Sailor::Win32::GlobalInput::QueueNativeEvent(evt);
				}
			}
		});

		glfwSetCharCallback(m_window, [](GLFWwindow* win, unsigned int codepoint)
		{
			Window* self = static_cast<Window*>(glfwGetWindowUserPointer(win));
			if (self)
			{
				Sailor::Platform::InputEvent evt{};
				evt.m_type = Sailor::Platform::InputEvent::Type::Text;
				char utf8Buf[8] = {};
				int len = 0;
				if (codepoint < 0x80)
				{
					utf8Buf[len++] = static_cast<char>(codepoint);
				}
				else if (codepoint < 0x800)
				{
					utf8Buf[len++] = static_cast<char>(0xC0 | ((codepoint >> 6) & 0x1F));
					utf8Buf[len++] = static_cast<char>(0x80 | (codepoint & 0x3F));
				}
				else if (codepoint < 0x10000)
				{
					utf8Buf[len++] = static_cast<char>(0xE0 | ((codepoint >> 12) & 0x0F));
					utf8Buf[len++] = static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
					utf8Buf[len++] = static_cast<char>(0x80 | (codepoint & 0x3F));
				}
				else
				{
					utf8Buf[len++] = static_cast<char>(0xF0 | ((codepoint >> 18) & 0x07));
					utf8Buf[len++] = static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F));
					utf8Buf[len++] = static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
					utf8Buf[len++] = static_cast<char>(0x80 | (codepoint & 0x3F));
				}
				evt.m_text.assign(utf8Buf, len);
				Sailor::Win32::GlobalInput::QueueNativeEvent(evt);
			}
		});

		return true;
	}

	void Window::Destroy()
	{
		if (m_window)
		{
			glfwDestroyWindow(m_window);
			m_window = nullptr;
			ReleaseGlfwInit();
		}
		m_running.store(false);
		m_shown.store(false);
		m_active.store(false);
		std::lock_guard lock(m_commandsMutex);
		m_pendingCommands.clear();
	}

	glm::ivec2 Window::GetRenderArea() const
	{
		return { m_renderWidth.load(), m_renderHeight.load() };
	}

	void Window::SetRenderArea(const glm::ivec2& renderArea)
	{
		m_renderWidth = renderArea.x;
		m_renderHeight = renderArea.y;
	}

	bool Window::IsParentWindowValid() const
	{
		return true;
	}

	void Window::Show(bool bShowWindow)
	{
		if (DeferToOwner([this, bShowWindow]() { Show(bShowWindow); }))
		{
			return;
		}
		if (m_window)
		{
			if (bShowWindow)
			{
				glfwShowWindow(m_window);
			}
			else
			{
				glfwHideWindow(m_window);
			}
			m_shown.store(bShowWindow);
		}
	}

	glm::ivec2 Window::GetCenterPointScreen() const
	{
		return { m_positionX.load() + m_width.load() / 2, m_positionY.load() + m_height.load() / 2 };
	}

	glm::ivec2 Window::GetCenterPointClient() const
	{
		int w = m_width.load();
		int h = m_height.load();
		return glm::ivec2(w / 2, h / 2);
	}

	void Window::RecalculateWindowSize()
	{
		if (DeferToOwner([this]() { RecalculateWindowSize(); }))
		{
			return;
		}
		if (m_window)
		{
			int w, h;
			glfwGetFramebufferSize(m_window, &w, &h);
			m_width.store(w);
			m_height.store(h);
		}
	}

	void Window::ChangeWindowSize(int32_t width, int32_t height, bool bIsFullScreen)
	{
		if (DeferToOwner([this, width, height, bIsFullScreen]() { ChangeWindowSize(width, height, bIsFullScreen); }))
		{
			return;
		}
		if (!m_window)
		{
			return;
		}

		if (bIsFullScreen)
		{
			GLFWmonitor* monitor = glfwGetPrimaryMonitor();
			const GLFWvidmode* mode = glfwGetVideoMode(monitor);
			if (mode)
			{
				glfwSetWindowMonitor(m_window, monitor, 0, 0, mode->width, mode->height, mode->refreshRate);
				m_width.store(mode->width);
				m_height.store(mode->height);
			}
		}
		else
		{
			glfwSetWindowMonitor(m_window, nullptr, 0, 0, width, height, 0);
			m_width.store(width);
			m_height.store(height);
		}
		m_fullscreen.store(bIsFullScreen);
	}

	void Window::ProcessSystemMessages()
	{
		if (!m_window)
		{
			return;
		}
		std::vector<std::function<void()>> commands;
		{
			std::lock_guard lock(m_commandsMutex);
			commands.swap(m_pendingCommands);
		}
		for (auto& command : commands)
		{
			command();
		}
		glfwPollEvents();
		int x = 0, y = 0;
		glfwGetWindowPos(m_window, &x, &y);
		m_positionX = x;
		m_positionY = y;

		if (glfwWindowShouldClose(m_window) == GLFW_TRUE)
		{
			m_running = false;
		}
		bool captureReq = m_captureRequested.load();
		bool currentlyCaptured = m_captured.load();
		bool active = m_active.load();

		if (captureReq && !currentlyCaptured && active)
		{
			glfwSetInputMode(m_window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
			m_hasCursorPosition = false;
			m_captured.store(true);
		}
		else if ((!captureReq || !active) && currentlyCaptured)
		{
			glfwSetInputMode(m_window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
			m_hasCursorPosition = false;
			m_captured.store(false);
		}

		if (m_resizing.load())
		{
			m_resizing.store(false);
		}
	}

	void Window::RequestMouseCapture(bool value)
	{
		m_captureRequested.store(value);
	}

	bool Window::IsMouseCaptured() const
	{
		return m_captured.load();
	}

	glm::vec2 Window::ConsumeMouseDelta()
	{
		std::lock_guard<std::mutex> lock(m_mouseMutex);
		glm::vec2 delta = m_mouseDelta;
		m_mouseDelta = glm::vec2(0.0f);
		return m_captured ? delta : glm::vec2(0.0f);
	}

	void Window::AddMouseDelta(float x, float y)
	{
		std::lock_guard<std::mutex> lock(m_mouseMutex);
		m_mouseDelta.x += x;
		m_mouseDelta.y += y;
	}
}

#endif