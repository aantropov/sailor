#include "Platform/Win32/Window.h"
#include "Platform/Win32/Input.h"
#include "Sailor.h"

#if defined(__APPLE__)

#import <Cocoa/Cocoa.h>
#import <CoreGraphics/CoreGraphics.h>
#import <IOKit/hidsystem/IOLLEvent.h>
#import <QuartzCore/CAMetalLayer.h>
#import <objc/runtime.h>

#include <algorithm>
#include <cmath>

using namespace Sailor;
using namespace Sailor::Win32;
using Sailor::Platform::InputEvent;

Utils::WindowSizeAndPosition Utils::GetWindowSizeAndPosition(HWND hwnd)
{
	return {};
}

namespace
{
	constexpr char sSailorWindowDelegateKey[] = "sailor_delegate";
	NSWindow* sReusableEditorRenderingWindow = nil;
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
	case 0x23: return 'P';
	case 0x25: return 'L';
	case 0x20: return 'U';
	case 0x22: return 'I';
	case 0x26: return 'J';
	case 0x28: return 'K';
	case 0x2E: return 'M';
	case 0x2D: return 'N';
	case 0x1F: return 'O';
	case 0x12: return '1';
	case 0x13: return '2';
	case 0x14: return '3';
	case 0x15: return '4';
	case 0x17: return '5';
	case 0x16: return '6';
	case 0x1A: return '7';
	case 0x1C: return '8';
	case 0x19: return '9';
	case 0x1D: return '0';
	case 0x30: return 0x09;
	case 0x31: return 0x20;
	case 0x24: return 0x0D;
	case 0x4C: return 0x0D;
	case 0x33: return 0x08;
	case 0x7B: return 0x25;
	case 0x7C: return 0x27;
	case 0x7E: return 0x26;
	case 0x7D: return 0x28;
	case 0x35: return VK_ESCAPE;
	case 0x60: return VK_F5;
	case 0x61: return VK_F6;
	case 0x38: return VK_LSHIFT;
	case 0x3C: return VK_RSHIFT;
	case 0x3B: return VK_LCONTROL;
	case 0x3E: return VK_RCONTROL;
	case 0x3A: return VK_LMENU;
	case 0x3D: return VK_RMENU;
	case 0x37: return VK_LWIN;
	case 0x36: return VK_RWIN;
	default:
		return 0;
	}
}

static void SailorConfigureMetalLayer(CAMetalLayer* metalLayer, bool bIsVsyncRequested)
{
	if (!metalLayer)
	{
		return;
	}

	metalLayer.displaySyncEnabled = bIsVsyncRequested ? YES : NO;
}

static void SailorUpdateMetalDrawableSize(NSWindow* window)
{
	NSView* contentView = window.contentView;
	CAMetalLayer* metalLayer = [contentView.layer isKindOfClass:[CAMetalLayer class]] ? (CAMetalLayer*)contentView.layer : nil;
	if (!metalLayer)
	{
		return;
	}

	const CGFloat backingScale = std::max<CGFloat>(window.backingScaleFactor, 1.0);
	metalLayer.contentsScale = backingScale;
	metalLayer.frame = contentView.bounds;
	// MoltenVK queries drawableSize for the surface extent. AppKit updating
	// layer bounds does not resize the pixel buffers after a native resize.
	metalLayer.drawableSize = CGSizeMake(
		std::max<CGFloat>(1.0, std::round(contentView.bounds.size.width * backingScale)),
		std::max<CGFloat>(1.0, std::round(contentView.bounds.size.height * backingScale)));
}

static void SailorApplyMacWindowSizeOnMainThread(NSWindow* window, int32_t width, int32_t height, bool bIsFullScreen, bool bRunsInsideEditor, bool bIsVsyncRequested)
{
	if (bRunsInsideEditor)
	{
		NSView* contentView = window.contentView;
		if (contentView)
		{
			const CGFloat backingScale = std::max<CGFloat>(window.backingScaleFactor, 1.0);
			const CGFloat logicalWidth = std::max<CGFloat>(1.0, (CGFloat)width / backingScale);
			const CGFloat logicalHeight = std::max<CGFloat>(1.0, (CGFloat)height / backingScale);
			contentView.frame = NSMakeRect(0.0, 0.0, logicalWidth, logicalHeight);
			[window setContentSize:NSMakeSize(logicalWidth, logicalHeight)];

			CAMetalLayer* metalLayer = [contentView.layer isKindOfClass:[CAMetalLayer class]] ? (CAMetalLayer*)contentView.layer : nil;
			if (!metalLayer)
			{
				contentView.wantsLayer = NO;
				metalLayer = [CAMetalLayer layer];
				contentView.layer = metalLayer;
				contentView.wantsLayer = YES;
			}

			SailorConfigureMetalLayer(metalLayer, bIsVsyncRequested);
			metalLayer.contentsScale = backingScale;
			metalLayer.frame = contentView.bounds;
			metalLayer.drawableSize = CGSizeMake((CGFloat)width, (CGFloat)height);
		}

		return;
	}

	[window setContentSize:NSMakeSize(width, height)];
	SailorUpdateMetalDrawableSize(window);

	const bool bIsCurrentlyFullScreen = (([window styleMask] & NSWindowStyleMaskFullScreen) != 0);
	if (bIsFullScreen != bIsCurrentlyFullScreen)
	{
		[window toggleFullScreen:nil];
	}
}

@interface SailorWindowDelegate : NSObject<NSWindowDelegate>
@property(nonatomic, assign) Sailor::Win32::Window* sailorWindow;
@property(nonatomic, assign) BOOL terminatesApplicationOnClose;
@end

@implementation SailorWindowDelegate

- (void)windowWillClose:(NSNotification*)notification
{
	SailorWindowDelegate* retainedSelf = [self retain];
	Sailor::Win32::Window* sailorWindow = self.sailorWindow;
	const BOOL bTerminatesApplicationOnClose = self.terminatesApplicationOnClose;
	self.sailorWindow = nullptr;

	if (sailorWindow && Sailor::Win32::Window::IsWindowAlive(sailorWindow))
	{
		NSWindow* window = (NSWindow*)notification.object;
		sailorWindow->HandleNativeWindowWillClose((HWND)(__bridge void*)window);
	}
	[retainedSelf release];

	if (bTerminatesApplicationOnClose)
	{
		// Standalone engine windows own the application lifetime. The hidden
		// editor rendering surface does not; closing it must leave MAUI alive.
		dispatch_async(dispatch_get_main_queue(), ^{
			[NSApp terminate:nil];
		});
	}
}

- (void)windowDidBecomeKey:(NSNotification*)notification
{
	(void)notification;
	if (self.sailorWindow && !App::IsEditorMode())
	{
		self.sailorWindow->SetActive(true);
		self.sailorWindow->UpdateMouseCapture();
	}

	GlobalInput::QueueNativeEvent({ InputEvent::Type::Focus, 0.0f, 0.0f, 0, -1, true, {} });
}

- (void)windowDidResignKey:(NSNotification*)notification
{
	(void)notification;
	if (self.sailorWindow && !App::IsEditorMode())
	{
		self.sailorWindow->SetActive(false);
		self.sailorWindow->UpdateMouseCapture();
	}

	GlobalInput::QueueNativeEvent({ InputEvent::Type::Focus, 0.0f, 0.0f, 0, -1, false, {} });
}

- (void)windowDidResize:(NSNotification*)notification
{
	if (!self.sailorWindow)
	{
		return;
	}

	if (App::IsEditorMode())
	{
		return;
	}

	NSWindow* window = (NSWindow*)notification.object;
	NSRect contentRect = [window.contentView bounds];
	self.sailorWindow->ChangeWindowSize((int32_t)contentRect.size.width, (int32_t)contentRect.size.height, false);
}

- (void)windowDidChangeBackingProperties:(NSNotification*)notification
{
	if (self.sailorWindow && !App::IsEditorMode())
	{
		SailorUpdateMetalDrawableSize((NSWindow*)notification.object);
	}
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

- (NSPoint)updateCursorFromEvent:(NSEvent*)event
{
	if (self.sailorWindow && (event.type == NSEventTypeMouseMoved ||
		event.type == NSEventTypeLeftMouseDragged || event.type == NSEventTypeRightMouseDragged ||
		event.type == NSEventTypeOtherMouseDragged))
	{
		self.sailorWindow->AddMouseDelta((float)event.deltaX, (float)event.deltaY);
	}

	NSPoint point = [self convertPoint:event.locationInWindow fromView:nil];
	const float viewHeight = self.bounds.size.height;
	const float x = (float)point.x;
	const float y = (float)(viewHeight - point.y);
	GlobalInput::QueueNativeEvent({ InputEvent::Type::MousePos, x, y, 0, -1, false, {} });
	return NSMakePoint(x, y);
}

- (void)keyDown:(NSEvent*)event
{
	const uint32_t key = SailorMapMacKeyCode(event.keyCode);
	if (key != 0 && !event.isARepeat)
	{
		GlobalInput::QueueNativeEvent({ InputEvent::Type::Key, 0.0f, 0.0f, key, -1, true, {}, event.keyCode == 0x4C });
	}

	if (const char* text = [[event characters] UTF8String])
		GlobalInput::QueueNativeEvent({ InputEvent::Type::Text, 0.0f, 0.0f, 0, -1, false, text });
}

- (void)keyUp:(NSEvent*)event
{
	const uint32_t key = SailorMapMacKeyCode(event.keyCode);
	if (key != 0)
	{
		GlobalInput::QueueNativeEvent({ InputEvent::Type::Key, 0.0f, 0.0f, key, -1, false, {}, event.keyCode == 0x4C });
	}
}

- (void)flagsChanged:(NSEvent*)event
{
	const uint32_t key = SailorMapMacKeyCode(event.keyCode);
	NSEventModifierFlags mask;
	switch (key)
	{
	case VK_LSHIFT: mask = NX_DEVICELSHIFTKEYMASK; break;
	case VK_RSHIFT: mask = NX_DEVICERSHIFTKEYMASK; break;
	case VK_LCONTROL: mask = NX_DEVICELCTLKEYMASK; break;
	case VK_RCONTROL: mask = NX_DEVICERCTLKEYMASK; break;
	case VK_LMENU: mask = NX_DEVICELALTKEYMASK; break;
	case VK_RMENU: mask = NX_DEVICERALTKEYMASK; break;
	case VK_LWIN: mask = NX_DEVICELCMDKEYMASK; break;
	case VK_RWIN: mask = NX_DEVICERCMDKEYMASK; break;
	default: return;
	}
	const bool bIsPressed = (event.modifierFlags & mask) != 0;
	GlobalInput::QueueNativeEvent({ InputEvent::Type::Key, 0.0f, 0.0f, key, -1, bIsPressed, {} });
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
	GlobalInput::QueueNativeEvent({ InputEvent::Type::MouseWheel, (float)event.scrollingDeltaX, (float)event.scrollingDeltaY, 0, -1, false, {} });
}

- (void)mouseDown:(NSEvent*)event
{
	NSPoint position = [self updateCursorFromEvent:event];
	GlobalInput::QueueNativeEvent({ InputEvent::Type::MouseButton, (float)position.x, (float)position.y, 0, 0, true, {} });
}

- (void)mouseUp:(NSEvent*)event
{
	NSPoint position = [self updateCursorFromEvent:event];
	GlobalInput::QueueNativeEvent({ InputEvent::Type::MouseButton, (float)position.x, (float)position.y, 0, 0, false, {} });
}

- (void)rightMouseDown:(NSEvent*)event
{
	NSPoint position = [self updateCursorFromEvent:event];
	GlobalInput::QueueNativeEvent({ InputEvent::Type::MouseButton, (float)position.x, (float)position.y, 0, 1, true, {} });
}

- (void)rightMouseUp:(NSEvent*)event
{
	NSPoint position = [self updateCursorFromEvent:event];
	GlobalInput::QueueNativeEvent({ InputEvent::Type::MouseButton, (float)position.x, (float)position.y, 0, 1, false, {} });
}

- (void)otherMouseDown:(NSEvent*)event
{
	NSPoint position = [self updateCursorFromEvent:event];
	GlobalInput::QueueNativeEvent({ InputEvent::Type::MouseButton, (float)position.x, (float)position.y, 0, 2, true, {} });
}

- (void)otherMouseUp:(NSEvent*)event
{
	NSPoint position = [self updateCursorFromEvent:event];
	GlobalInput::QueueNativeEvent({ InputEvent::Type::MouseButton, (float)position.x, (float)position.y, 0, 2, false, {} });
}

@end

TVector<Window*> Window::g_windows;

Window::~Window()
{
	Destroy();
}

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
	if (![NSThread isMainThread])
	{
		dispatch_sync(dispatch_get_main_queue(), ^
		{
			Show(bShowWindow);
		});
		return;
	}

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

void Window::SetWindowTitle(LPCSTR lString)
{
	NSWindow* window = (__bridge NSWindow*)m_hWnd;
	if (!window)
	{
		return;
	}

	@autoreleasepool
	{
		NSString* title = [NSString stringWithUTF8String:lString ? lString : ""];
		if (!title)
		{
			return;
		}

		if (![NSThread isMainThread])
		{
			dispatch_async(dispatch_get_main_queue(), ^
			{
				window.title = title;
			});
			return;
		}

		window.title = title;
	}
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
		const bool bRunsInsideEditor = App::IsEditorMode();
		if (!bRunsInsideEditor)
		{
			[NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
		}

		const NSWindowStyleMask style = parentHwnd == nullptr ?
			(NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable) :
			NSWindowStyleMaskBorderless;

		NSRect frame = NSMakeRect(0, 0, inWidth, inHeight);
		NSWindow* window = bRunsInsideEditor ? sReusableEditorRenderingWindow : nil;
		if (window)
		{
			sReusableEditorRenderingWindow = nil;
			[window setContentSize:frame.size];
		}
		else
		{
			window = [[NSWindow alloc] initWithContentRect:frame styleMask:style backing:NSBackingStoreBuffered defer:NO];
		}
		if (!window)
		{
			return false;
		}
		window.releasedWhenClosed = NO;

		SailorContentView* contentView = [[SailorContentView alloc] initWithFrame:frame];
		contentView.sailorWindow = this;
		CAMetalLayer* metalLayer = [CAMetalLayer layer];
		SailorConfigureMetalLayer(metalLayer, bIsVsyncRequested);
		// The renderer owns this layer's contents. Assign it before enabling
		// layer hosting so AppKit does not cache an empty NSView drawing over it.
		contentView.layer = metalLayer;
		contentView.wantsLayer = YES;
		metalLayer.contentsScale = [window backingScaleFactor];

		window.contentView = contentView;
		SailorUpdateMetalDrawableSize(window);
		window.acceptsMouseMovedEvents = YES;
		window.title = [NSString stringWithUTF8String:title ? title : "Sailor"];
		[window makeFirstResponder:contentView];
		[window center];
		[contentView release];

		SailorWindowDelegate* delegate = [[SailorWindowDelegate alloc] init];
		delegate.sailorWindow = this;
		delegate.terminatesApplicationOnClose = !bRunsInsideEditor;
		window.delegate = delegate;
		objc_setAssociatedObject(window, sSailorWindowDelegateKey, delegate, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
		[delegate release];

		if (!bRunsInsideEditor)
		{
			[window makeKeyAndOrderFront:nil];
			[NSApp activateIgnoringOtherApps:YES];
			m_bIsShown = true;
		}
		else
		{
			[window orderOut:nil];
			m_bIsShown = false;
		}

		m_hWnd = (HWND)(__bridge void*)window;
	}

	g_windows.Add(this);
	printf("%s\n", App::IsEditorMode() ? "Hidden editor rendering surface created" : "Window created");
	return true;
}

void Window::ChangeWindowSize(int32_t width, int32_t height, bool bInIsFullScreen)
{
	NSWindow* window = (__bridge NSWindow*)m_hWnd;
	if (!window)
	{
		m_width = width;
		m_height = height;
		m_bIsFullscreen = bInIsFullScreen;
		return;
	}

	const int32_t contentWidth = std::max<int32_t>(1, width);
	const int32_t contentHeight = std::max<int32_t>(1, height);
	m_width = contentWidth;
	m_height = contentHeight;
	m_bIsFullscreen = bInIsFullScreen;

	const bool bRunsInsideEditor = App::IsEditorMode();
	const bool bIsVsyncRequested = m_bIsVsyncRequested;
	if (![NSThread isMainThread])
	{
		dispatch_async(dispatch_get_main_queue(), ^
		{
			SailorApplyMacWindowSizeOnMainThread(window, contentWidth, contentHeight, bInIsFullScreen, bRunsInsideEditor, bIsVsyncRequested);
		});
		return;
	}

	SailorApplyMacWindowSizeOnMainThread(window, contentWidth, contentHeight, bInIsFullScreen, bRunsInsideEditor, bIsVsyncRequested);
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
			pWindow->SetIsIconic(window.isMiniaturized);
			if (!App::IsEditorMode())
			{
				pWindow->SetActive(window.isKeyWindow && NSApp.isActive && !window.isMiniaturized);
			}

			pWindow->UpdateMouseCapture();
		}
	}
}

void Window::ProcessSystemMessages()
{
	ProcessMacMsgs();
}

void Window::UpdateMouseCapture()
{
	check([NSThread isMainThread]);

	NSWindow* window = (__bridge NSWindow*)m_hWnd;
	const bool bShouldCapture = m_bMouseCaptureRequested && !App::IsEditorMode() && m_bIsActive &&
		window && window.isKeyWindow && NSApp.isActive && !window.isMiniaturized;
	if (bShouldCapture == m_bMouseCaptured)
	{
		return;
	}

	if (bShouldCapture)
	{
		if (CGAssociateMouseAndMouseCursorPosition(false) != kCGErrorSuccess)
		{
			return;
		}

		if (CGDisplayHideCursor(kCGDirectMainDisplay) != kCGErrorSuccess)
		{
			CGAssociateMouseAndMouseCursorPosition(true);
			return;
		}
	}
	else
	{
		CGAssociateMouseAndMouseCursorPosition(true);
		CGDisplayShowCursor(kCGDirectMainDisplay);
	}

	const std::lock_guard<std::mutex> lock(m_mouseDeltaMutex);
	m_mouseDelta = {};
	m_bMouseCaptured = bShouldCapture;
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
	if (![NSThread isMainThread])
	{
		dispatch_sync(dispatch_get_main_queue(), ^
		{
			Destroy();
		});
		return;
	}

	RequestMouseCapture(false);
	UpdateMouseCapture();
	NSWindow* window = (__bridge NSWindow*)m_hWnd;
	m_hWnd = nullptr;
	g_windows.Remove(this);
	m_bIsShown = false;
	m_bIsActive = false;
	m_bIsRunning = false;

	if (window)
	{
		SailorWindowDelegate* delegate = (SailorWindowDelegate*)objc_getAssociatedObject(window, sSailorWindowDelegateKey);
		const BOOL bTerminatesApplicationOnClose = delegate ? delegate.terminatesApplicationOnClose : NO;
		if (delegate)
		{
			delegate.sailorWindow = nullptr;
		}

		SailorContentView* contentView = [window.contentView isKindOfClass:[SailorContentView class]] ? (SailorContentView*)window.contentView : nil;
		if (contentView)
		{
			contentView.sailorWindow = nullptr;
		}

		window.delegate = nil;
		objc_setAssociatedObject(window, sSailorWindowDelegateKey, nil, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
		if (bTerminatesApplicationOnClose)
		{
			[window close];
			[window release];
		}
		else
		{
			// Closing or deallocating the last AppKit NSWindow terminates a Catalyst
			// host. Keep one process-owned hidden surface and reuse it when the
			// engine is initialized for the next workspace.
			[window orderOut:nil];
			sReusableEditorRenderingWindow = window;
		}
	}
}

void Window::HandleNativeWindowWillClose(HWND nativeWindow)
{
	NSWindow* window = (__bridge NSWindow*)nativeWindow;
	if (!window || (__bridge NSWindow*)m_hWnd != window)
	{
		return;
	}

	RequestMouseCapture(false);
	UpdateMouseCapture();
	m_hWnd = nullptr;
	g_windows.Remove(this);
	m_bIsShown = false;
	m_bIsActive = false;
	m_bIsRunning = false;

	SailorContentView* contentView = [window.contentView isKindOfClass:[SailorContentView class]] ? (SailorContentView*)window.contentView : nil;
	if (contentView)
	{
		contentView.sailorWindow = nullptr;
	}

	window.delegate = nil;
	objc_setAssociatedObject(window, sSailorWindowDelegateKey, nil, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
	dispatch_async(dispatch_get_main_queue(), ^
	{
		[window release];
	});
}

bool Window::IsWindowAlive(const Window* pWindow)
{
	for (auto* pWindowInList : g_windows)
	{
		if (pWindowInList == pWindow)
		{
			return true;
		}
	}

	return false;
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

	CAMetalLayer* metalLayer = [window.contentView.layer isKindOfClass:[CAMetalLayer class]] ? (CAMetalLayer*)window.contentView.layer : nil;
	if (!metalLayer)
	{
		window.contentView.wantsLayer = NO;
		metalLayer = [CAMetalLayer layer];
		window.contentView.layer = metalLayer;
		window.contentView.wantsLayer = YES;
		SailorUpdateMetalDrawableSize(window);
	}

	SailorConfigureMetalLayer(metalLayer, m_bIsVsyncRequested);
	return (__bridge void*)metalLayer;
}

void* Window::GetNativeView() const
{
	NSWindow* window = (__bridge NSWindow*)m_hWnd;
	return window ? (__bridge void*)window.contentView : nullptr;
}

#endif
