#include "Platform/Linux/Window.h"
#include <chrono>
#include <iostream>
#include <thread>
#include <cstring>
#include <X11/Xlib.h>

namespace
{
	bool Expect(bool condition, const char* message)
	{
		if (!condition)
		{
			std::cerr << message << '\n';
		}
		return condition;
	}
	bool RequestNativeClose()
	{
		Display* display = XOpenDisplay(nullptr);
		if (!display)
		{
			return false;
		}
		::Window root = DefaultRootWindow(display), parent = 0;
		::Window* children = nullptr;
		unsigned count = 0;
		::Window target = 0;
		if (XQueryTree(display, root, &root, &parent, &children, &count))
		{
			for (unsigned i = 0; i < count; ++i)
			{
				char* title = nullptr;
				if (XFetchName(display, children[i], &title) && title)
				{
					if (std::strcmp(title, "Sailor close event test") == 0)
					{
						target = children[i];
					}
					XFree(title);
				}
			}
			XFree(children);
		}
		XEvent event{};
		event.xclient.type = ClientMessage;
		event.xclient.window = target;
		event.xclient.message_type = XInternAtom(display, "WM_PROTOCOLS", False);
		event.xclient.format = 32;
		event.xclient.data.l[0] = XInternAtom(display, "WM_DELETE_WINDOW", False);
		const bool sent = target && XSendEvent(display, target, False, NoEventMask, &event);
		XSync(display, False);
		XCloseDisplay(display);
		return sent;
	}
}

int main()
{
	Sailor::Linux::Window window;
	if (!Expect(!window.Create("Invalid parent", "Test", 320, 240, false, false, reinterpret_cast<void*>(1)),
		"Unsupported native parent must fail without creating a window."))
	{
		return 1;
	}
	for (int cycle = 0; cycle < 2; ++cycle)
	{
		if (!Expect(window.Create("Sailor Linux window test", "Test", 320, 240), "GLFW window creation failed."))
		{
			return 1;
		}
		window.ProcessSystemMessages();
		if (!Expect(window.IsRunning() && window.GetNativeHandle() && window.GetWidth() > 0 && window.GetHeight() > 0,
			"Created window must have a live handle and nonzero framebuffer."))
		{
			return 1;
		}
		std::thread producer([&window]()
		{
			window.SetWindowTitle("Linux UTF-8 — Привет");
			window.ChangeWindowSize(512, 288);
			window.Show(false);
		});
		producer.join();
		window.ProcessSystemMessages();
		window.RecalculateWindowSize();
		if (!Expect(!window.IsShown() && window.GetWidth() >= 512 && window.GetHeight() >= 288,
			"Queued title/resize/hide commands must apply on the owner thread."))
		{
			return 1;
		}
		window.SetRenderArea({ 256, 144 });
		if (!Expect(window.GetRenderArea() == glm::ivec2(256, 144), "Offscreen render area must remain independent of the native framebuffer."))
		{
			return 1;
		}
		window.Show(true);
		window.ProcessSystemMessages();
		window.SetRunning(false);
		window.ProcessSystemMessages();
		if (!Expect(!window.IsRunning(), "Polling must preserve an engine stop request."))
		{
			return 1;
		}
		window.SetRunning(true);
		window.SetWindowTitle("Sailor close event test");
		window.ProcessSystemMessages();
		if (!Expect(RequestNativeClose(), "Could not deliver the native close event."))
		{
			return 1;
		}
		for (int attempt = 0; attempt < 100 && window.IsRunning(); ++attempt)
		{
			window.ProcessSystemMessages();
			std::this_thread::sleep_for(std::chrono::milliseconds(2));
		}
		if (!Expect(!window.IsRunning(), "The native close event must stop the runtime loop."))
		{
			return 1;
		}
		window.Destroy();
		window.Destroy();
		if (!Expect(!window.IsRunning() && !window.GetNativeHandle(), "Window destruction must be idempotent."))
		{
			return 1;
		}
	}
	std::cout << "Linux window lifecycle and UI command delivery passed.\n";
	return 0;
}
