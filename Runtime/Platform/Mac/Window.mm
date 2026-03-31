#include "Platform/Win32/Window.h"
#include "Platform/Win32/Input.h"
#include "Submodules/ImGuiApi.h"
#include "Sailor.h"

#if defined(__APPLE__)

#import <Cocoa/Cocoa.h>
#import <QuartzCore/CAMetalLayer.h>
#import <objc/runtime.h>

#include <algorithm>

using namespace Sailor;
using namespace Sailor::Win32;

static void SailorDispatchImGuiMacEvent(const ImGuiApi::MacEvent& event)
{
	if (auto* imGui = App::GetSubmodule<ImGuiApi>())
	{
		imGui->HandleMac(event);
	}
}

static uint32_t SailorMapMacKeyCode(unsigned short keyCode)
{
	switch (keyCode)
	{
	case 0x00: return 'A';
	case 0x01: return 'S';
	case 0x02: return 'D';
	case 0x03: return 'F';
	case 0x04: return 'H';
	case 0x05: return 'G';
	case 0x06: return 'Z';
	case 0x07: return 'X';
	case 0x08: return 'C';
	case 0x09: return 'V';
	case 0x0B: return 'B';
	case 0x0C: return 'Q';
	case 0x0D: return 'W';
	case 0x0E: return 'E';
	case 0x0F: return 'R';
	case 0x10: return 'Y';
	case 0x11: return 'T';
	case 0x20: return 'U';
	case 0x53: return VK_ESCAPE;
	case 0x60: return VK_F5;
	case 0x61: return VK_F6;
	case 0x38:
	case 0x3C:
		return VK_SHIFT;
	case 0x3B:
	case 0x3E:
		return VK_CONTROL;
	default:
		return 0;
	}
}

@interface SailorWindowDelegate : NSObject<NSWindowDelegate>
@property(nonatomic, assign) Sailor::Win32::Window* sailorWindow;
@end

@implementation SailorWindowDelegate

- (void)windowWillClose:(NSNotification*)notification
{
	(void)notification;
	if (self.sailorWindow)
	{
		self.sailorWindow->SetActive(false);
		self.sailorWindow->SetRunning(false);
	}

	// On macOS we want app process to exit when the main window is closed.
	// This avoids a background engine process that requires Force Quit.
	dispatch_async(dispatch_get_main_queue(), ^{
		[NSApp terminate:nil];
	});
}

- (void)windowDidBecomeKey:(NSNotification*)notification
{
	(void)notification;
	SailorDispatchImGuiMacEvent({ ImGuiApi::MacEvent::Type::Focus, 0.0f, 0.0f, 0, -1, true, nullptr });
}

- (void)windowDidResignKey:(NSNotification*)notification
{
	(void)notification;
	SailorDispatchImGuiMacEvent({ ImGuiApi::MacEvent::Type::Focus, 0.0f, 0.0f, 0, -1, false, nullptr });
}

- (void)windowDidResize:(NSNotification*)notification
{
	if (!self.sailorWindow)
	{
		return;
	}

	NSWindow* window = (NSWindow*)notification.object;
	NSRect contentRect = [window.contentView bounds];
	self.sailorWindow->ChangeWindowSize((int32_t)contentRect.size.width, (int32_t)contentRect.size.height, false);
}

@end

@interface SailorContentView : NSView
@property(nonatomic, assign) Sailor::Win32::Window* sailorWindow;
@end

@implementation SailorContentView

- (BOOL)acceptsFirstResponder
{
	return YES;
}

- (void)updateCursorFromEvent:(NSEvent*)event
{
	NSPoint point = [self convertPoint:event.locationInWindow fromView:nil];
	const float viewHeight = self.bounds.size.height;
	const float x = (float)point.x;
	const float y = (float)(viewHeight - point.y);
	GlobalInput::SetCursorPosition((int32_t)x, (int32_t)y);
	SailorDispatchImGuiMacEvent({ ImGuiApi::MacEvent::Type::MousePos, x, y, 0, -1, false, nullptr });
}

- (void)keyDown:(NSEvent*)event
{
	if (event.isARepeat)
	{
		return;
	}

	const uint32_t key = SailorMapMacKeyCode(event.keyCode);
	if (key != 0)
	{
		GlobalInput::SetKeyState(key, KeyState::Pressed);
		SailorDispatchImGuiMacEvent({ ImGuiApi::MacEvent::Type::Key, 0.0f, 0.0f, key, -1, true, nullptr });
	}

	SailorDispatchImGuiMacEvent({ ImGuiApi::MacEvent::Type::Text, 0.0f, 0.0f, 0, -1, false, [[event characters] UTF8String] });
}

- (void)keyUp:(NSEvent*)event
{
	const uint32_t key = SailorMapMacKeyCode(event.keyCode);
	if (key != 0)
	{
		GlobalInput::SetKeyState(key, KeyState::Up);
		SailorDispatchImGuiMacEvent({ ImGuiApi::MacEvent::Type::Key, 0.0f, 0.0f, key, -1, false, nullptr });
	}
}

- (void)flagsChanged:(NSEvent*)event
{
	const uint32_t key = SailorMapMacKeyCode(event.keyCode);
	if (key == VK_SHIFT || key == VK_CONTROL)
	{
		const bool isDown = (event.modifierFlags & (key == VK_SHIFT ? NSEventModifierFlagShift : NSEventModifierFlagControl)) != 0;
		GlobalInput::SetKeyState(key, isDown ? KeyState::Pressed : KeyState::Up);
		SailorDispatchImGuiMacEvent({ ImGuiApi::MacEvent::Type::Key, 0.0f, 0.0f, key, -1, isDown, nullptr });
	}
}

- (void)mouseMoved:(NSEvent*)event
{
	[self updateCursorFromEvent:event];
}

- (void)mouseDragged:(NSEvent*)event
{
	[self updateCursorFromEvent:event];
}

- (void)rightMouseDragged:(NSEvent*)event
{
	[self updateCursorFromEvent:event];
}

- (void)otherMouseDragged:(NSEvent*)event
{
	[self updateCursorFromEvent:event];
}

- (void)scrollWheel:(NSEvent*)event
{
	[self updateCursorFromEvent:event];
	SailorDispatchImGuiMacEvent({ ImGuiApi::MacEvent::Type::MouseWheel, (float)event.scrollingDeltaX, (float)event.scrollingDeltaY, 0, -1, false, nullptr });
}

- (void)mouseDown:(NSEvent*)event
{
	[self updateCursorFromEvent:event];
	GlobalInput::SetMouseButtonState(0, KeyState::Pressed);
	SailorDispatchImGuiMacEvent({ ImGuiApi::MacEvent::Type::MouseButton, 0.0f, 0.0f, 0, 0, true, nullptr });
}

- (void)mouseUp:(NSEvent*)event
{
	[self updateCursorFromEvent:event];
	GlobalInput::SetMouseButtonState(0, KeyState::Up);
	SailorDispatchImGuiMacEvent({ ImGuiApi::MacEvent::Type::MouseButton, 0.0f, 0.0f, 0, 0, false, nullptr });
}

- (void)rightMouseDown:(NSEvent*)event
{
	[self updateCursorFromEvent:event];
	GlobalInput::SetMouseButtonState(1, KeyState::Pressed);
	SailorDispatchImGuiMacEvent({ ImGuiApi::MacEvent::Type::MouseButton, 0.0f, 0.0f, 0, 1, true, nullptr });
}

- (void)rightMouseUp:(NSEvent*)event
{
	[self updateCursorFromEvent:event];
	GlobalInput::SetMouseButtonState(1, KeyState::Up);
	SailorDispatchImGuiMacEvent({ ImGuiApi::MacEvent::Type::MouseButton, 0.0f, 0.0f, 0, 1, false, nullptr });
}

- (void)otherMouseDown:(NSEvent*)event
{
	[self updateCursorFromEvent:event];
	GlobalInput::SetMouseButtonState(2, KeyState::Pressed);
	SailorDispatchImGuiMacEvent({ ImGuiApi::MacEvent::Type::MouseButton, 0.0f, 0.0f, 0, 2, true, nullptr });
}

- (void)otherMouseUp:(NSEvent*)event
{
	[self updateCursorFromEvent:event];
	GlobalInput::SetMouseButtonState(2, KeyState::Up);
	SailorDispatchImGuiMacEvent({ ImGuiApi::MacEvent::Type::MouseButton, 0.0f, 0.0f, 0, 2, false, nullptr });
}

@end

TVector<Window*> Window::g_windows;

bool Window::IsParentWindowValid() const
{
	if (m_parentHwnd == nullptr)
	{
		return true;
	}

	NSWindow* parent = (__bridge NSWindow*)m_parentHwnd;
	return parent != nil;
}

void Window::SetWindowPos(const RECT& rect)
{
	NSWindow* window = (__bridge NSWindow*)m_hWnd;
	if (!window)
	{
		return;
	}

	[window setFrame:NSMakeRect(rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top) display:YES];
}

void Window::Show(bool bShowWindow)
{
	NSWindow* window = (__bridge NSWindow*)m_hWnd;
	if (window)
	{
		if (bShowWindow)
		{
			[window makeKeyAndOrderFront:nil];
		}
		else
		{
			[window orderOut:nil];
		}
	}

	m_bIsShown = bShowWindow;
}

void Window::TrackParentWindowPosition(const RECT& viewport)
{
	(void)viewport;
}

bool Window::Create(LPCSTR title, LPCSTR className, int32_t inWidth, int32_t inHeight, bool inbIsFullScreen, bool bIsVsyncRequested, HWND parentHwnd)
{
	m_parentHwnd = parentHwnd;
	m_windowClassName = className;
	m_bIsVsyncRequested = bIsVsyncRequested;
	m_width = inWidth;
	m_height = inHeight;
	m_bIsFullscreen = inbIsFullScreen;

	@autoreleasepool
	{
		[NSApplication sharedApplication];
		[NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];

		const NSWindowStyleMask style = parentHwnd == nullptr ?
			(NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable) :
			NSWindowStyleMaskBorderless;

		NSRect frame = NSMakeRect(0, 0, inWidth, inHeight);
		NSWindow* window = [[NSWindow alloc] initWithContentRect:frame styleMask:style backing:NSBackingStoreBuffered defer:NO];
		if (!window)
		{
			return false;
		}

		SailorContentView* contentView = [[SailorContentView alloc] initWithFrame:frame];
		contentView.sailorWindow = this;
		contentView.wantsLayer = YES;
		contentView.layer = [CAMetalLayer layer];
		contentView.layer.contentsScale = [window backingScaleFactor];

		window.contentView = contentView;
		window.acceptsMouseMovedEvents = YES;
		window.title = [NSString stringWithUTF8String:title ? title : "Sailor"];
		[window makeFirstResponder:contentView];
		[window center];

		SailorWindowDelegate* delegate = [[SailorWindowDelegate alloc] init];
		delegate.sailorWindow = this;
		window.delegate = delegate;
		objc_setAssociatedObject(window, "sailor_delegate", delegate, OBJC_ASSOCIATION_RETAIN_NONATOMIC);

		[window makeKeyAndOrderFront:nil];
		[NSApp activateIgnoringOtherApps:YES];

		m_hWnd = (HWND)(__bridge void*)window;
		m_bIsShown = true;
	}

	g_windows.Add(this);
	printf("Window created\n");
	return true;
}

void Window::ChangeWindowSize(int32_t width, int32_t height, bool bInIsFullScreen)
{
	m_width = width;
	m_height = height;
	m_bIsFullscreen = bInIsFullScreen;

	NSWindow* window = (__bridge NSWindow*)m_hWnd;
	if (!window)
	{
		return;
	}

	[window setContentSize:NSMakeSize(width, height)];

	const bool bIsCurrentlyFullScreen = (([window styleMask] & NSWindowStyleMaskFullScreen) != 0);
	if (bInIsFullScreen != bIsCurrentlyFullScreen)
	{
		[window toggleFullScreen:nil];
	}
}

void Sailor::Win32::Window::ProcessMacMsgs()
{
	@autoreleasepool
	{
		for (;;)
		{
			NSEvent* event = [NSApp nextEventMatchingMask:NSEventMaskAny
				untilDate:[NSDate distantPast]
				inMode:NSDefaultRunLoopMode
				dequeue:YES];

			if (!event)
			{
				break;
			}

			[NSApp sendEvent:event];
		}

		[NSApp updateWindows];

		for (auto* pWindow : g_windows)
		{
			if (!pWindow || !pWindow->m_hWnd)
			{
				continue;
			}

			NSWindow* window = (__bridge NSWindow*)pWindow->m_hWnd;
			pWindow->SetIsIconic(window ? [window isMiniaturized] : false);
		}
	}
}

void Window::ProcessSystemMessages()
{
	ProcessMacMsgs();
}

glm::ivec2 Window::GetCenterPointScreen() const
{
	NSWindow* window = (__bridge NSWindow*)m_hWnd;
	if (!window)
	{
		return glm::ivec2(0);
	}

	const NSRect content = [window.contentView bounds];
	const NSPoint centerInWindow = NSMakePoint(content.size.width * 0.5, content.size.height * 0.5);
	const NSPoint centerScreen = [window convertPointToScreen:centerInWindow];
	return ivec2((int32_t)centerScreen.x, (int32_t)centerScreen.y);
}

glm::ivec2 Window::GetCenterPointClient() const
{
	return ivec2(m_width / 2, m_height / 2);
}

void Window::RecalculateWindowSize()
{
	NSWindow* window = (__bridge NSWindow*)m_hWnd;
	if (!window)
	{
		m_width = 0;
		m_height = 0;
		return;
	}

	if ([window isMiniaturized])
	{
		m_width = 0;
		m_height = 0;
		return;
	}

	NSRect rect = [window.contentView bounds];
	m_width = (int32_t)rect.size.width;
	m_height = (int32_t)rect.size.height;
}

void Window::Destroy()
{
	NSWindow* window = (__bridge NSWindow*)m_hWnd;
	if (window)
	{
		[window close];
	}

	m_hWnd = nullptr;
	g_windows.Remove(this);
}

bool Window::IsIconic() const
{
	return m_bIsIconic;
}

void* Window::GetMetalLayer() const
{
	NSWindow* window = (__bridge NSWindow*)m_hWnd;
	if (!window || !window.contentView)
	{
		return nullptr;
	}

	if (!window.contentView.wantsLayer)
	{
		window.contentView.wantsLayer = YES;
		window.contentView.layer = [CAMetalLayer layer];
	}

	return (__bridge void*)window.contentView.layer;
}

void* Window::GetNativeView() const
{
	NSWindow* window = (__bridge NSWindow*)m_hWnd;
	return window ? (__bridge void*)window.contentView : nullptr;
}

#endif
