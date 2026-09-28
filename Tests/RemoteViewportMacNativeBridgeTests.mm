#if defined(__APPLE__)

#import <QuartzCore/CAMetalLayer.h>
#import <QuartzCore/CATransaction.h>
#import <Metal/Metal.h>
#import <IOSurface/IOSurface.h>
#import <Foundation/Foundation.h>
#import <AppKit/AppKit.h>
#import <objc/runtime.h>

#include <chrono>
#include <condition_variable>
#include <functional>
#include <future>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

#include "Submodules/EditorRemote/RemoteViewportMacNativeBridge.h"
#include "Submodules/EditorRemote/RemoteViewportMacTransport.h"
#include "Memory/SharedPtr.hpp"

using Sailor::TUniquePtr;
using namespace Sailor::EditorRemote;

static_assert(!std::is_copy_constructible_v<MacNativeLayerBinding>);
static_assert(!std::is_copy_assignable_v<MacNativeLayerBinding>);
static_assert(!std::is_copy_constructible_v<MacNativePresentationState>);
static_assert(std::is_move_assignable_v<MacNativePresentationState>);

@interface NativeReleaseObserver : NSObject
{
@public
	Sailor::TSharedPtr<std::atomic<uint32_t>> m_releases;
}
@end

@implementation NativeReleaseObserver
- (void)dealloc
{
	m_releases->fetch_add(1);
	[super dealloc];
}
@end

// Fault only queue creation; all other device operations retain native behavior.
@interface QueueFailureDevice : NSProxy
{
	id<MTLDevice> m_device;
}
- (id)initWithDevice:(id<MTLDevice>)device;
- (id<MTLCommandQueue>)newCommandQueue;
@end

@implementation QueueFailureDevice
- (id)initWithDevice:(id<MTLDevice>)device
{
	m_device = [device retain];
	return self;
}
- (id<MTLCommandQueue>)newCommandQueue { return nil; }
- (NSMethodSignature*)methodSignatureForSelector:(SEL)selector
{
	return [(NSObject*)m_device methodSignatureForSelector:selector];
}
- (void)forwardInvocation:(NSInvocation*)invocation { [invocation invokeWithTarget:m_device]; }
- (void)dealloc
{
	[m_device release];
	[super dealloc];
}
@end

@interface QueueFailureLayer : CAMetalLayer
{
@public
	id<MTLDevice> m_failureDevice;
	bool m_failQueue;
	bool m_refuseDrawable;
}
@end

@implementation QueueFailureLayer
- (id<MTLDevice>)device { return m_failQueue ? m_failureDevice : [super device]; }
- (id<CAMetalDrawable>)nextDrawable { return m_refuseDrawable ? nil : [super nextDrawable]; }
- (void)dealloc
{
	[m_failureDevice release];
	[super dealloc];
}
@end

@interface ProducerCommandBufferProbe : NSProxy
{
	id<MTLCommandBuffer> m_buffer;
}
- (id)initWithBuffer:(id<MTLCommandBuffer>)buffer;
@end

@implementation ProducerCommandBufferProbe
- (id)initWithBuffer:(id<MTLCommandBuffer>)buffer
{
	m_buffer = [buffer retain];
	return self;
}
- (MTLCommandBufferStatus)status
{
	const auto status = m_buffer.status;
	return status == MTLCommandBufferStatusCompleted ? MTLCommandBufferStatusError : status;
}
- (NSError*)error { return [NSError errorWithDomain:@"Sailor.ProducerCopyTest" code:71 userInfo:nil]; }
- (BOOL)respondsToSelector:(SEL)selector { return [m_buffer respondsToSelector:selector]; }
- (NSMethodSignature*)methodSignatureForSelector:(SEL)selector
{
	return [(NSObject*)m_buffer methodSignatureForSelector:selector];
}
- (void)forwardInvocation:(NSInvocation*)invocation { [invocation invokeWithTarget:m_buffer]; }
- (void)dealloc
{
	[m_buffer release];
	[super dealloc];
}
@end

@interface ProducerQueueProbe : NSProxy
{
	id<MTLCommandQueue> m_queue;
@public
	uint32_t m_commandBufferCount;
	bool m_refuseCommandBuffer;
	bool m_failCompletion;
}
- (id)initWithQueue:(id<MTLCommandQueue>)queue;
@end

@implementation ProducerQueueProbe
- (id)initWithQueue:(id<MTLCommandQueue>)queue
{
	m_queue = [queue retain];
	return self;
}
- (id<MTLCommandBuffer>)commandBuffer
{
	++m_commandBufferCount;
	if (m_refuseCommandBuffer) return nil;
	id<MTLCommandBuffer> buffer = [m_queue commandBuffer];
	return m_failCompletion ? (id<MTLCommandBuffer>)[[[ProducerCommandBufferProbe alloc] initWithBuffer:buffer] autorelease] : buffer;
}
- (NSMethodSignature*)methodSignatureForSelector:(SEL)selector
{
	return [(NSObject*)m_queue methodSignatureForSelector:selector];
}
- (void)forwardInvocation:(NSInvocation*)invocation { [invocation invokeWithTarget:m_queue]; }
- (void)dealloc
{
	[m_queue release];
	[super dealloc];
}
@end

@interface DelayedReadQueue : NSProxy
{
	id<MTLCommandQueue> m_queue;
	id<MTLTexture> m_source;
@public
	id<MTLSharedEvent> m_gate;
	id<MTLBuffer> m_pixel;
	id<MTLCommandBuffer> m_read;
	uint64_t m_waitValue;
	uint32_t m_commandCount;
}
- (id)initWithQueue:(id<MTLCommandQueue>)queue source:(id<MTLTexture>)source;
- (void)prepareNextReadWithSource:(id<MTLTexture>)source;
@end

@implementation DelayedReadQueue
- (id)initWithQueue:(id<MTLCommandQueue>)queue source:(id<MTLTexture>)source
{
	m_queue = [queue retain];
	m_source = [source retain];
	m_gate = [queue.device newSharedEvent];
	m_pixel = [queue.device newBufferWithLength:256 options:MTLResourceStorageModeShared];
	m_waitValue = 1;
	return self;
}
- (void)prepareNextReadWithSource:(id<MTLTexture>)source
{
	[m_read release];
	m_read = nil;
	[m_source release];
	m_source = [source retain];
	++m_waitValue;
}
- (id<MTLCommandBuffer>)commandBuffer
{
	++m_commandCount;
	id<MTLCommandBuffer> buffer = [m_queue commandBuffer];
	if (m_read == nil)
	{
		m_read = [buffer retain];
		[buffer encodeWaitForEvent:m_gate value:m_waitValue];
		id<MTLBlitCommandEncoder> encoder = [buffer blitCommandEncoder];
		[encoder copyFromTexture:m_source sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
			sourceSize:MTLSizeMake(1, 1, 1) toBuffer:m_pixel destinationOffset:0 destinationBytesPerRow:256 destinationBytesPerImage:256];
		[encoder endEncoding];
		[m_source release];
		m_source = nil;
	}
	return buffer;
}
- (NSMethodSignature*)methodSignatureForSelector:(SEL)selector
{
	return [(NSObject*)m_queue methodSignatureForSelector:selector];
}
- (void)forwardInvocation:(NSInvocation*)invocation { [invocation invokeWithTarget:m_queue]; }
- (void)dealloc
{
	m_gate.signaledValue = UINT64_MAX;
	[m_read waitUntilCompleted];
	[m_read release];
	[m_pixel release];
	[m_gate release];
	[m_source release];
	[m_queue release];
	[super dealloc];
}
@end

namespace
{
	void ObserveNativeRelease(id object, const Sailor::TSharedPtr<std::atomic<uint32_t>>& releases)
	{
		static char key;
		NativeReleaseObserver* observer = [[NativeReleaseObserver alloc] init];
		observer->m_releases = releases;
		objc_setAssociatedObject(object, &key, observer, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
		[observer release];
	}

	MacNativeHostHandle LayerHandle(CAMetalLayer* layer)
	{
		return { MacNativeHostHandleKind::CAMetalLayer, reinterpret_cast<uintptr_t>(layer) };
	}

	QueueFailureLayer* MakeQueueFailureLayer()
	{
		QueueFailureLayer* layer = [QueueFailureLayer layer];
		id<MTLDevice> device = [MTLCreateSystemDefaultDevice() autorelease];
		layer->m_failureDevice = (id<MTLDevice>)[[QueueFailureDevice alloc] initWithDevice:device];
		layer->m_failQueue = true;
		return layer;
	}

	struct NativeSurface
	{
		IOSurfaceRef m_surface;
		TransportDescriptor m_transport;

		NativeSurface(uint32_t width, uint32_t height)
		{
			NSDictionary* properties = @{
				(__bridge NSString*)kIOSurfaceWidth: @(width),
				(__bridge NSString*)kIOSurfaceHeight: @(height),
				(__bridge NSString*)kIOSurfaceBytesPerElement: @4,
				(__bridge NSString*)kIOSurfaceBytesPerRow: @(width * 4u),
				(__bridge NSString*)kIOSurfacePixelFormat: @('BGRA')
			};
			m_surface = IOSurfaceCreate((__bridge CFDictionaryRef)properties);
			if (!m_surface) throw std::runtime_error("native binding test requires an IOSurface");
			MacIOSurfaceHandle handle;
			handle.m_surfaceId = IOSurfaceGetID(m_surface);
			handle.m_registryId = [MTLCreateSystemDefaultDevice() autorelease].registryID;
			handle.m_surfaceObject = reinterpret_cast<uintptr_t>(m_surface);
			handle.m_planeCount = 1;
			handle.m_bytesPerRow = width * 4u;
			handle.m_bytesPerElement = 4;
			m_transport.m_transportType = TransportType::MacIOSurface;
			m_transport.m_width = width;
			m_transport.m_height = height;
			m_transport.m_pixelFormat = PixelFormat::B8G8R8A8_UNorm;
			m_transport.m_macSurfaces.push_back(handle);
		}

		~NativeSurface() { CFRelease(m_surface); }
		NativeSurface(const NativeSurface&) = delete;
		NativeSurface& operator=(const NativeSurface&) = delete;
	};

	uint32_t ReadIOSurfaceBGRA8Pixel(IOSurfaceRef surface, uint32_t bytesPerRow, uint32_t x, uint32_t y)
	{
		IOSurfaceLock(surface, kIOSurfaceLockReadOnly, nullptr);
		auto* baseAddress = static_cast<const uint8_t*>(IOSurfaceGetBaseAddress(surface));
		if (baseAddress == nullptr)
		{
			IOSurfaceUnlock(surface, kIOSurfaceLockReadOnly, nullptr);
			throw std::runtime_error("test expected IOSurface base address");
		}

		const size_t index = static_cast<size_t>(y) * bytesPerRow + static_cast<size_t>(x) * 4u;
		const uint32_t pixel = static_cast<uint32_t>(baseAddress[index + 0]) | (static_cast<uint32_t>(baseAddress[index + 1]) << 8u) | (static_cast<uint32_t>(baseAddress[index + 2]) << 16u) | (static_cast<uint32_t>(baseAddress[index + 3]) << 24u);
		IOSurfaceUnlock(surface, kIOSurfaceLockReadOnly, nullptr);
		return pixel;
	}

	uint32_t ExpectedProducerPatternBGRA8(const MacNativeBridgeProducerPattern& pattern, uint32_t x, uint32_t y)
	{
		const uint8_t b = static_cast<uint8_t>((x + ((pattern.m_frameIndex * 17u + pattern.m_generation * 13u) & 0xffu)) & 0xffu);
		const uint8_t g = static_cast<uint8_t>((y + ((pattern.m_epoch * 29u + pattern.m_viewportId * 7u) & 0xffu)) & 0xffu);
		const uint8_t r = static_cast<uint8_t>((pattern.m_width + pattern.m_height + pattern.m_frameIndex * 3u) & 0xffu);
		return static_cast<uint32_t>(b) | (static_cast<uint32_t>(g) << 8u) | (static_cast<uint32_t>(r) << 16u) | 0xff000000u;
	}

	void Require(bool condition, const std::string& message)
	{
		if (!condition)
		{
			throw std::runtime_error(message);
		}
	}

	void RequireNativeReleases(const Sailor::TSharedPtr<std::atomic<uint32_t>>& releases, uint32_t expected, const std::string& message)
	{
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
		while (releases->load() < expected && std::chrono::steady_clock::now() < deadline)
		{
			@autoreleasepool
			{
				[CATransaction flush];
				[[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.001]];
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		Require(releases->load() == expected, message + ": " + std::to_string(releases->load()));
	}

	Sailor::TSharedPtr<MacIOSurfaceAllocation> MakeNativeProducer(IOSurfaceRef surface, uint32_t width, uint32_t height)
	{
		auto allocation = Sailor::TSharedPtr<MacIOSurfaceAllocation>::Make();
		allocation->m_surfaceObject = reinterpret_cast<uintptr_t>(CFRetain(surface));
		allocation->m_pixelFormat = PixelFormat::B8G8R8A8_UNorm;
		allocation->m_plane = { 0, 1, width, height, static_cast<uint32_t>(IOSurfaceGetBytesPerRow(surface)), 4 };
		Require(CreateMacIOSurfaceProducerTexture(*allocation).IsOk(), "test must create a native producer owner");
		return allocation;
	}

	void TestGetMacRendererSourceSelectionPriorityPrefersSceneViewResolvedOutputs()
	{
		Require(GetMacRendererSourceSelectionPriority("Renderer.SceneView.Main.Resolved") < GetMacRendererSourceSelectionPriority("Renderer.Driver.BackBuffer"), "source-priority helper should prefer the final scene-view resolve over the driver backbuffer");
		Require(GetMacRendererSourceSelectionPriority("Renderer.SceneView.BackBuffer.Resolved") < GetMacRendererSourceSelectionPriority("Renderer.Driver.BackBuffer"), "source-priority helper should still prefer framegraph-owned resolves over the swapchain backbuffer");
		Require(GetMacRendererSourceSelectionPriority("Renderer.SceneView.Main.Target") < GetMacRendererSourceSelectionPriority("Renderer.Driver.BackBuffer"), "source-priority helper should prefer scene-view targets over the driver backbuffer when resolves are unavailable");
	}

	void TestSelectMacVulkanSemaphoreForMetalExportPrefersDedicatedMainResolvedSeam()
	{
		bool usedDedicatedSemaphore = false;
		const uintptr_t selectedHandle = SelectMacVulkanSemaphoreForMetalExport("Renderer.SceneView.Main.Resolved", 0x1111ull, 0x2222ull, usedDedicatedSemaphore);
		Require(selectedHandle == 0x1111ull, "selection helper should prefer the dedicated semaphore for Renderer.SceneView.Main.Resolved");
		Require(usedDedicatedSemaphore, "selection helper should report dedicated semaphore usage for Renderer.SceneView.Main.Resolved");

		usedDedicatedSemaphore = false;
		const uintptr_t fallbackHandle = SelectMacVulkanSemaphoreForMetalExport("Renderer.SceneView.Main.Resolved", 0ull, 0x2222ull, usedDedicatedSemaphore);
		Require(fallbackHandle == 0x2222ull, "selection helper should fall back to the generic render-finished semaphore when the dedicated seam is unavailable");
		Require(!usedDedicatedSemaphore, "selection helper should report fallback usage when the dedicated semaphore is unavailable");

		usedDedicatedSemaphore = false;
		const uintptr_t nonTargetHandle = SelectMacVulkanSemaphoreForMetalExport("Renderer.SceneView.Secondary", 0x1111ull, 0x2222ull, usedDedicatedSemaphore);
		Require(nonTargetHandle == 0x2222ull, "selection helper should keep non-target exports on the generic render-finished seam");
		Require(!usedDedicatedSemaphore, "selection helper should not report dedicated usage for non-target exports");
	}

	void TestSynchronizeMacVulkanRenderTargetPrefersMetalSharedEventWhenSemaphoreExportSeamExists()
	{
		SetMacVulkanMetalInteropTestMode(true);
		uintptr_t sharedEventObject = 0;
		uint64_t acquireValue = 0;
		CrossApiSyncKind syncKind = CrossApiSyncKind::None;
		bool cpuWaited = true;
		auto result = SynchronizeMacVulkanRenderTargetForMetalExport(0x1000ull, 0x2000ull, sharedEventObject, acquireValue, syncKind, cpuWaited);
		SetMacVulkanMetalInteropTestMode(false);
		Require(result.IsOk(), "sync selection test should succeed when the metal-object export seam is declared available");
		Require(syncKind == CrossApiSyncKind::MetalSharedEvent, "sync selection test should prefer Metal shared-event sync over CPU device-idle fallback");
		Require(acquireValue == 1ull, "sync selection test should materialize a concrete shared-event wait value");
		Require(sharedEventObject != 0, "sync selection test should materialize a concrete shared-event object token");
		Require(!cpuWaited, "sync selection test should not report CPU fallback when the Metal shared-event seam is available");
	}

	void TestBridgeWaitsOnMetalSharedEventBeforeProducerCopy()
	{
		id<MTLDevice> device = [MTLCreateSystemDefaultDevice() autorelease];
		Require(device != nil, "shared-event test requires a real Metal device");
		id<MTLSharedEvent> sharedEvent = [[device newSharedEvent] autorelease];
		Require(sharedEvent != nil, "shared-event test requires a real Metal shared event");

		const uint32_t width = 64;
		const uint32_t height = 32;
		NSMutableDictionary* properties = [NSMutableDictionary dictionary];
		properties[(__bridge NSString*)kIOSurfaceWidth] = @(width);
		properties[(__bridge NSString*)kIOSurfaceHeight] = @(height);
		properties[(__bridge NSString*)kIOSurfaceBytesPerElement] = @4;
		properties[(__bridge NSString*)kIOSurfaceBytesPerRow] = @(width * 4u);
		const uint32_t pixelFormat = 'BGRA';
		properties[(__bridge NSString*)kIOSurfacePixelFormat] = @(pixelFormat);
		IOSurfaceRef surface = IOSurfaceCreate((__bridge CFDictionaryRef)properties);
		Require(surface != nullptr, "shared-event test requires a real IOSurface");

		auto producer = MakeNativeProducer(surface, width, height);
		uintptr_t rendererTextureObject = 0;
		Require(CreateMacRendererIntermediateTexture(producer->m_producerDeviceObject, width, height, PixelFormat::B8G8R8A8_UNorm, rendererTextureObject).IsOk(), "shared-event test should create renderer texture");

		MacNativeBridgeProducerPattern pattern{ 91, 1, 1, 1, width, height };
		Require(UploadMacRendererPatternToIntermediateTexture(rendererTextureObject, width, height, pattern).IsOk(), "shared-event test should fill renderer texture");

		id<MTLCommandQueue> signalQueue = [[device newCommandQueue] autorelease];
		Require(signalQueue != nil, "shared-event test requires a Metal command queue for signaling");
		id<MTLCommandBuffer> signalBuffer = [signalQueue commandBuffer];
		Require(signalBuffer != nil, "shared-event test requires a Metal command buffer for signaling");
		[signalBuffer encodeSignalEvent:sharedEvent value:9ull];
		[signalBuffer commit];

		MacNativeBridgeRendererFrameInfo frameInfo{};
		Require(CopyMacRendererIntermediateToProducerTexture(*producer, rendererTextureObject, frameInfo, reinterpret_cast<uintptr_t>((__bridge void*)sharedEvent), 9ull).IsOk(), "shared-event test should copy through a Metal shared-event wait");
		Require(frameInfo.m_waitedOnCrossApiSharedEvent, "shared-event test should report that the producer copy waited on a Metal shared event");
		Require(frameInfo.m_crossApiWaitValue == 9ull, "shared-event test should preserve the waited Metal shared-event value");
		Require(ReadIOSurfaceBGRA8Pixel(surface, width * 4u, 0, 0) == ExpectedProducerPatternBGRA8(pattern, 0, 0), "shared-event test should still land the renderer pixel into the IOSurface after the wait");

		ReleaseMacRendererIntermediateTexture(rendererTextureObject);
		CFRelease(surface);
	}

	void TestBridgeBindsExistingCAMetalLayerAndPresentsDrawable()
	{
		CAMetalLayer* layer = [CAMetalLayer layer];
		MacNativeHostHandle hostHandle{ MacNativeHostHandleKind::CAMetalLayer, reinterpret_cast<uintptr_t>((__bridge void*)layer) };
		TUniquePtr<MacNativeLayerBinding> binding;
		auto bindResult = BindMacNativeLayer(hostHandle, 640, 360, PixelFormat::B8G8R8A8_UNorm, binding);
		Require(bindResult.IsOk(), "binding an existing CAMetalLayer should succeed");
		Require(binding && binding->IsValid(), "binding should materialize a valid CAMetalLayer state");
		Require(binding->m_layerObject == hostHandle.m_value, "bridge should retain the exact CAMetalLayer pointer");
		Require(binding->m_deviceObject != 0, "binding should materialize a real Metal device");
		Require(binding->m_commandQueueObject != 0, "binding should materialize a real Metal command queue");
		Require(layer.colorspace != nullptr &&
			CFEqual(CGColorSpaceGetName(layer.colorspace), kCGColorSpaceSRGB),
			"the editor viewport layer must present encoded pixels in the sRGB color space");

		FramePacket frame{};
		frame.m_viewportId = 1;
		frame.m_connectionEpoch = 1;
		frame.m_generation = 1;
		frame.m_frameIndex = 1;
		frame.m_width = 640;
		frame.m_height = 360;

		MacNativeBridgePresentResult presentResult{};
		NSMutableDictionary* properties = [NSMutableDictionary dictionary];
		properties[(__bridge NSString*)kIOSurfaceWidth] = @(frame.m_width);
		properties[(__bridge NSString*)kIOSurfaceHeight] = @(frame.m_height);
		properties[(__bridge NSString*)kIOSurfaceBytesPerElement] = @4;
		properties[(__bridge NSString*)kIOSurfaceBytesPerRow] = @(frame.m_width * 4u);
		const uint32_t pixelFormat = 'BGRA';
		properties[(__bridge NSString*)kIOSurfacePixelFormat] = @(pixelFormat);
		IOSurfaceRef surface = IOSurfaceCreate((__bridge CFDictionaryRef)properties);
		Require(surface != nullptr, "test should allocate a real IOSurface for import");

		MacIOSurfaceHandle surfaceHandle{};
		surfaceHandle.m_surfaceId = IOSurfaceGetID(surface);
		surfaceHandle.m_registryId = 0x7001ull;
		surfaceHandle.m_surfaceObject = reinterpret_cast<uintptr_t>(surface);
		surfaceHandle.m_planeIndex = 0;
		surfaceHandle.m_planeCount = 1;
		surfaceHandle.m_bytesPerRow = frame.m_width * 4u;
		surfaceHandle.m_bytesPerElement = 4u;
		surfaceHandle.m_framebufferOnly = false;

		auto producer = MakeNativeProducer(surface, frame.m_width, frame.m_height);
		Require(producer->m_producerDeviceObject != 0 && producer->m_producerTextureObject != 0, "producer-side Metal texture creation should yield live opaque Metal objects");

		MacNativeBridgeProducerPattern pattern{};
		pattern.m_viewportId = frame.m_viewportId;
		pattern.m_epoch = frame.m_connectionEpoch;
		pattern.m_generation = frame.m_generation;
		pattern.m_frameIndex = frame.m_frameIndex;
		pattern.m_width = frame.m_width;
		pattern.m_height = frame.m_height;
		uintptr_t rendererTextureObject = 0;
		auto rendererTextureResult = CreateMacRendererIntermediateTexture(producer->m_producerDeviceObject, frame.m_width, frame.m_height, PixelFormat::B8G8R8A8_UNorm, rendererTextureObject);
		Require(rendererTextureResult.IsOk(), "test should materialize a renderer-shaped intermediate Metal texture");
		Require(rendererTextureObject != 0, "renderer-shaped intermediate Metal texture should yield a live opaque Metal object");

		auto uploadResult = UploadMacRendererPatternToIntermediateTexture(rendererTextureObject, frame.m_width, frame.m_height, pattern);
		Require(uploadResult.IsOk(), "test should upload renderer-shaped content into the intermediate texture before the producer copy");

		MacNativeBridgeRendererFrameInfo rendererFrameInfo{};
		auto copyResult = CopyMacRendererIntermediateToProducerTexture(*producer, rendererTextureObject, rendererFrameInfo);
		Require(copyResult.IsOk(), "test should copy renderer-shaped output into the IOSurface-backed producer texture");
		Require(rendererFrameInfo.m_usedRendererIntermediateTexture, "copy result should report renderer-intermediate usage");
		Require(rendererFrameInfo.m_usedGpuCopyIntoProducerTexture, "copy result should report GPU copy into the producer texture");
		Require(rendererFrameInfo.m_rendererTextureToken != 0 && rendererFrameInfo.m_producerCopyToken != 0, "copy result should stamp renderer/copy tokens");
		Require(ReadIOSurfaceBGRA8Pixel(surface, surfaceHandle.m_bytesPerRow, 0, 0) == ExpectedProducerPatternBGRA8(pattern, 0, 0), "renderer-intermediate GPU copy should populate the shared IOSurface with the deterministic BGRA pattern");
		Require(ReadIOSurfaceBGRA8Pixel(surface, surfaceHandle.m_bytesPerRow, 23, 11) == ExpectedProducerPatternBGRA8(pattern, 23, 11), "renderer-intermediate GPU copy should populate more than the first pixel in the shared IOSurface");

		auto present = PresentMacNativeLayerFrame(*binding, surfaceHandle, frame, presentResult);
		ReleaseMacRendererIntermediateTexture(rendererTextureObject);
		CFRelease(surface);
		Require(present.IsOk(), "presenting through a bound CAMetalLayer should acquire a drawable");
		Require(presentResult.IsValid(), "present result should surface a real present token");
		Require(presentResult.m_usedRealCAMetalLayer, "present should report that a real CAMetalLayer path was exercised");
		Require(presentResult.m_usedMetalCommandQueue, "present should exercise a real Metal command queue");
		Require(presentResult.m_drawableObject != 0, "present should surface a real drawable handle");
		Require(presentResult.m_sourceTextureObject != 0, "present should surface the source Metal texture used for the copy path");
		Require(!presentResult.m_usedSyntheticSourceTexture, "bridge should import the real IOSurface into a Metal texture when a live IOSurface handle is supplied");
		Require(binding->m_importedIOSurfaceObject == surfaceHandle.m_surfaceObject, "binding should retain the imported IOSurface object used for the Metal texture import");
		Require(binding->m_lastSourceTextureObject == presentResult.m_sourceTextureObject, "binding should retain the last source Metal texture");
	}

	void TestBindingFailurePreservesCurrentLayer()
	{
		@autoreleasepool
		{
			CAMetalLayer* layer = [CAMetalLayer layer];
			const MacNativeHostHandle host{ MacNativeHostHandleKind::CAMetalLayer, reinterpret_cast<uintptr_t>(layer) };
			TUniquePtr<MacNativeLayerBinding> binding;
			Require(BindMacNativeLayer(host, 64, 48, PixelFormat::B8G8R8A8_UNorm, binding).IsOk(), "initial native binding must succeed");
			const auto oldLayer = binding->m_layerObject;
			const auto oldQueue = binding->m_commandQueueObject;
			const auto oldToken = binding->m_bindingToken;
			Require(!BindMacNativeLayer(host, 96, 72, PixelFormat::Unknown, binding).IsOk(), "unsupported pixel format must fail");
			Require(binding && binding->IsValid() && binding->m_layerObject == oldLayer && binding->m_commandQueueObject == oldQueue &&
				binding->m_bindingToken == oldToken && binding->m_width == 64u && binding->m_height == 48u,
				"failed rebind must preserve the original live native binding");
			Require(layer.drawableSize.width == 64 && layer.drawableSize.height == 48,
				"failed rebind must not mutate the original layer dimensions");
			Require(!BindMacNativeLayer(host, 0, 72, PixelFormat::B8G8R8A8_UNorm, binding).IsOk(), "zero width must fail");
			Require(!BindMacNativeLayer(host, 96, 0, PixelFormat::B8G8R8A8_UNorm, binding).IsOk(), "zero height must fail");
			Require(!BindMacNativeLayer({}, 96, 72, PixelFormat::B8G8R8A8_UNorm, binding).IsOk(), "missing host must fail");
			auto unsupportedHost = host;
			unsupportedHost.m_kind = static_cast<MacNativeHostHandleKind>(255);
			Require(!BindMacNativeLayer(unsupportedHost, 96, 72, PixelFormat::B8G8R8A8_UNorm, binding).IsOk(), "unsupported host kind must fail");
			Require(binding->m_commandQueueObject == oldQueue && binding->m_bindingToken == oldToken,
				"all rejected binds must preserve the working queue and token");
			FramePacket frame;
			MacNativeBridgePresentResult present;
			Require(PresentMacNativeLayerFrame(*binding, {}, frame, present).IsOk(), "prior binding must still present after rejected replacements");
		}
	}

	void TestNativeBindingReleasesOwnedObjects()
	{
		auto layers = Sailor::TSharedPtr<std::atomic<uint32_t>>::Make(0u);
		auto queues = Sailor::TSharedPtr<std::atomic<uint32_t>>::Make(0u);
		auto textures = Sailor::TSharedPtr<std::atomic<uint32_t>>::Make(0u);
		TUniquePtr<MacNativeLayerBinding> binding;
		@autoreleasepool
		{
			CAMetalLayer* layer = [CAMetalLayer layer];
			ObserveNativeRelease(layer, layers);
			Require(BindMacNativeLayer(LayerHandle(layer), 64, 48, PixelFormat::B8G8R8A8_UNorm, binding).IsOk(), "native owner must bind");
			ObserveNativeRelease((id)binding->m_commandQueueObject, queues);
			FramePacket frame;
			MacNativeBridgePresentResult present;
			Require(PresentMacNativeLayerFrame(*binding, {}, frame, present).IsOk(), "native owner must present a source texture");
			ObserveNativeRelease((id)present.m_sourceTextureObject, textures);
			id<MTLCommandBuffer> completion = [(id<MTLCommandQueue>)binding->m_commandQueueObject commandBuffer];
			[completion commit];
			[completion waitUntilCompleted];
		}
		Require(layers->load() == 0 && queues->load() == 0 && textures->load() == 0,
			"binding must own its layer, queue and last source texture beyond the native pool");
		binding.Clear();
		// Metal and Core Animation retire their references independently.
		RequireNativeReleases(layers, 1, "clearing the binding must release its layer exactly once");
		RequireNativeReleases(queues, 1, "clearing the binding must release its queue exactly once");
		RequireNativeReleases(textures, 1, "clearing the binding must release its source texture exactly once");
	}

	void TestProviderDestructionReleasesNativeTextures()
	{
		auto textures = Sailor::TSharedPtr<std::atomic<uint32_t>>::Make(0u);
		@autoreleasepool
		{
			MacLoopbackIOSurfaceProvider provider;
			MacViewportSurfaceState state;
			ViewportDescriptor viewport;
			viewport.m_viewportId = 100;
			viewport.m_width = 64;
			viewport.m_height = 48;
			viewport.m_pixelFormat = PixelFormat::B8G8R8A8_UNorm;
			Require(provider.CreateOrResizeSurface(viewport, 1, 1, state).IsOk(), "provider must allocate native textures");
			ObserveNativeRelease((id)state.m_nativeAllocation->m_producerTextureObject, textures);
			ObserveNativeRelease((id)state.m_nativeAllocation->m_rendererIntermediateTextureObject, textures);
		}
		Require(textures->load() == 2, "provider and state destruction must release both owned native textures without manual cleanup");
	}

	void TestLoopbackDefersWritesUntilPresentationCompletes()
	{
		class CpuSource : public IMacRendererFrameSourceProvider
		{
		public:
			uint32_t m_calls = 0;
			Failure AcquireFrameSource(const MacViewportSurfaceState& state, FrameIndex frameIndex, MacRendererFrameSource& out) override
			{
				++m_calls;
				out.m_kind = MacRendererFrameSourceKind::RendererOwnedRenderTargetMetadata;
				out.m_width = state.m_viewport.m_width;
				out.m_height = state.m_viewport.m_height;
				out.m_pixelFormat = state.m_viewport.m_pixelFormat;
				out.m_bytesPerRow = out.m_width * 4;
				out.m_cpuBytes = Sailor::TSharedPtr<std::vector<uint8_t>>::Make(out.m_bytesPerRow * out.m_height, static_cast<uint8_t>(frameIndex));
				return Failure::Ok();
			}
		} source;
		MacLoopbackIOSurfaceProvider provider(&source);
		MacLoopbackViewportPresenter presenter;
		ViewportDescriptor viewport;
		viewport.m_viewportId = 103;
		viewport.m_width = 64;
		viewport.m_height = 48;
		viewport.m_pixelFormat = PixelFormat::B8G8R8A8_UNorm;
		viewport.m_colorSpace = ColorSpace::Srgb;
		viewport.m_presentMode = PresentMode::Mailbox;
		MacViewportLoopbackBinding binding(viewport, provider, presenter);
		presenter.BindHostHandle(viewport.m_viewportId, LayerHandle([CAMetalLayer layer]));
		Require(binding.Create().IsOk(), "delayed read needs a real loopback surface and host");
		const auto surface = std::as_const(binding.GetTransportBackend()).FindSurface(viewport.m_viewportId, 1, 1);
		auto allocation = surface->m_nativeAllocation;
		const auto native = presenter.FindImportedState(viewport.m_viewportId)->m_layerBinding.GetRawPtr();
		id<MTLCommandQueue> queue = (id<MTLCommandQueue>)native->m_commandQueueObject;
		DelayedReadQueue* probe = [[[DelayedReadQueue alloc] initWithQueue:queue source:(id<MTLTexture>)allocation->m_producerTextureObject] autorelease];
		[queue release];
		native->m_commandQueueObject = reinterpret_cast<uintptr_t>([probe retain]);
		Require(probe->m_gate && probe->m_pixel, "delayed read needs native event and readback storage");

		std::promise<void> finished;
		auto finish = finished.get_future();
		std::thread watchdog([&]()
		{
			if (finish.wait_for(std::chrono::seconds(5)) == std::future_status::timeout)
			{
				probe->m_gate.signaledValue = UINT64_MAX;
			}
		});
		try
		{
			Require(binding.PumpFrame().IsOk(), "first upload and delayed native read must submit");
			Require(probe->m_read && probe->m_gate.signaledValue == 0 && probe->m_read.status != MTLCommandBufferStatusCompleted,
				"native presenter read must still be blocked on the test event");
			Require(binding.PumpFrame().IsOk(), "pending reader should defer the next pump without a failure");
			Require(binding.GetRuntimeSession().GetLastPublishedFrameIndex() == 1 && source.m_calls == 1,
				"pending native read must prevent a new upload and frame publication");
			Require(!binding.GetTransportBackend().BeginFrame(viewport, 1, 1).IsOk(), "direct producer begin must also reject early reuse");
			MacNativeBridgeRendererFrameInfo info;
			const auto& bytes = *allocation->m_lastRendererSource.m_cpuBytes;
			Require(!UploadMacRendererBytesToProducerTexture(*allocation, bytes.data(), viewport.m_width * 4, info).IsOk(),
				"direct CPU upload must also reject the pending native read");
			Require(!CopyMacRendererIntermediateToProducerTexture(*allocation, allocation->m_rendererIntermediateTextureObject, info).IsOk(),
				"direct GPU copy must also reject the pending native read");
			Require(!presenter.PresentFrame(viewport.m_viewportId, binding.GetRuntimeSession().GetLastFrame()).IsOk(),
				"duplicate presentation must not replace an unfinished read");
			for (uint32_t i = 0; i < 100; ++i)
			{
				Require(binding.PumpFrame().IsOk(), "pending pumps must remain nonblocking and successful");
			}
			Require(source.m_calls == 1 && probe->m_commandCount == 1, "pending pumps must not acquire frames or accumulate Metal work");
			Require(ReadIOSurfaceBGRA8Pixel((IOSurfaceRef)allocation->m_surfaceObject, allocation->m_plane.m_bytesPerRow, 0, 0) == 0x01010101u,
				"pending GPU reader must retain the original frame pixels");
			presenter.BindHostHandle(viewport.m_viewportId, LayerHandle([CAMetalLayer layer]));
			Require(binding.PumpFrame().IsOk() && source.m_calls == 1, "host rebind must not lose the surface's pending read");
			presenter.BindHostHandle(viewport.m_viewportId, {});
			Require(binding.PumpFrame().IsOk() && source.m_calls == 1, "host detach must not permit early surface reuse");
			probe->m_gate.signaledValue = 1;
			[probe->m_read waitUntilCompleted];
			Require(probe->m_read.status == MTLCommandBufferStatusCompleted && *(const uint32_t*)probe->m_pixel.contents == 0x01010101u,
				"the actual delayed GPU read must observe its original frame");
			Require(binding.PumpFrame().IsOk() && binding.GetRuntimeSession().GetLastPublishedFrameIndex() == 2 && source.m_calls == 2,
				"completed read must permit exactly the next frame");
			Require(ReadIOSurfaceBGRA8Pixel((IOSurfaceRef)allocation->m_surfaceObject, allocation->m_plane.m_bytesPerRow, 0, 0) == 0x02020202u,
				"reuse after native completion must write the new frame");

			presenter.BindHostHandle(viewport.m_viewportId, LayerHandle([CAMetalLayer layer]));
			const auto rebound = presenter.FindImportedState(viewport.m_viewportId)->m_layerBinding.GetRawPtr();
			[(id)rebound->m_commandQueueObject release];
			rebound->m_commandQueueObject = reinterpret_cast<uintptr_t>([probe retain]);
			[probe prepareNextReadWithSource:(id<MTLTexture>)allocation->m_producerTextureObject];
			auto sourceReleases = Sailor::TSharedPtr<std::atomic<uint32_t>>::Make(0u);
			Require(binding.PumpFrame().IsOk(), "another native read must submit before resizing");
			ObserveNativeRelease((id)rebound->m_lastSourceTextureObject, sourceReleases);
			Require(probe->m_gate.signaledValue == 1 && probe->m_read.status != MTLCommandBufferStatusCompleted,
				"resize test must keep the old generation's actual GPU read pending");
			Require(binding.Resize(80, 56).IsOk(), "resize must import a separate allocation while the old read is pending");
			Require(!allocation.IsShared(), "new generation must not keep the old C++ allocation registered or imported");
			allocation.Clear();
			Require(sourceReleases->load() == 0, "in-flight Metal command must retain its actual source texture after allocation and binding destruction");
			Require(binding.PumpFrame().IsOk() && binding.GetRuntimeSession().GetGeneration() == 2 &&
				binding.GetRuntimeSession().GetLastPublishedFrameIndex() == 1, "new allocation must progress independently of the retired reader");
			probe->m_gate.signaledValue = 2;
			[probe->m_read waitUntilCompleted];
			Require(probe->m_read.status == MTLCommandBufferStatusCompleted && *(const uint32_t*)probe->m_pixel.contents == 0x03030303u,
				"retired generation's delayed GPU read must preserve its own frame");
			[probe->m_read release];
			probe->m_read = nil;
			RequireNativeReleases(sourceReleases, 1, "completed retired command must release its native source texture once");
		}
		catch (...)
		{
			probe->m_gate.signaledValue = UINT64_MAX;
			finished.set_value();
			watchdog.join();
			throw;
		}
		finished.set_value();
		watchdog.join();
	}

	void TestPresentFailuresAndCompletedReadRetention()
	{
		MacLoopbackIOSurfaceProvider provider;
		MacLoopbackViewportPresenter presenter;
		ViewportDescriptor viewport;
		viewport.m_viewportId = 104;
		viewport.m_width = 64;
		viewport.m_height = 48;
		viewport.m_pixelFormat = PixelFormat::B8G8R8A8_UNorm;
		viewport.m_colorSpace = ColorSpace::Srgb;
		viewport.m_presentMode = PresentMode::Mailbox;
		MacViewportLoopbackBinding binding(viewport, provider, presenter);
		QueueFailureLayer* layer = [QueueFailureLayer layer];
		presenter.BindHostHandle(viewport.m_viewportId, LayerHandle(layer));
		Require(binding.Create().IsOk(), "present failure test requires a real loopback surface");
		auto allocation = std::as_const(binding.GetTransportBackend()).FindSurface(viewport.m_viewportId, 1, 1)->m_nativeAllocation;
		const auto native = presenter.FindImportedState(viewport.m_viewportId)->m_layerBinding.GetRawPtr();
		id<MTLCommandQueue> queue = (id<MTLCommandQueue>)native->m_commandQueueObject;
		ProducerQueueProbe* probe = [[[ProducerQueueProbe alloc] initWithQueue:queue] autorelease];
		[queue release];
		native->m_commandQueueObject = reinterpret_cast<uintptr_t>([probe retain]);
		layer->m_refuseDrawable = true;
		Require(!binding.PumpFrame().IsOk() && allocation->m_presentCommandBufferObject == 0, "drawable refusal must not reserve a surface reader");
		layer->m_refuseDrawable = false;
		probe->m_refuseCommandBuffer = true;
		Require(!binding.PumpFrame().IsOk() && allocation->m_presentCommandBufferObject == 0, "command refusal must leave the surface reusable");
		probe->m_refuseCommandBuffer = false;
		probe->m_failCompletion = true;
		Require(binding.PumpFrame().IsOk(), "native submission may be accepted before its terminal result");
		[(id<MTLCommandBuffer>)allocation->m_presentCommandBufferObject waitUntilCompleted];
		const auto priorFrame = binding.GetRuntimeSession().GetLastPublishedFrameIndex();
		const auto result = binding.PumpFrame();
		Require(!result.IsOk() && result.m_nativeCode == 1035 && allocation->m_presentCommandBufferObject == 0,
			"terminal presentation failure must surface and retire its native command");
		Require(binding.GetRuntimeSession().GetLastPublishedFrameIndex() == priorFrame && allocation->m_lastWrittenFrameIndex == priorFrame,
			"failed read completion must not upload or publish another frame");
		probe->m_failCompletion = false;
		auto commands = Sailor::TSharedPtr<std::atomic<uint32_t>>::Make(0u);
		for (uint32_t i = 0; i < 100; ++i)
		{
			@autoreleasepool
			{
				Require(binding.PumpFrame().IsOk(), "presentation must recover and continue after a terminal error");
				id<MTLCommandBuffer> command = (id<MTLCommandBuffer>)allocation->m_presentCommandBufferObject;
				Require(command && command.retainedReferences, "surface reader must own a resource-retaining native command");
				ObserveNativeRelease(command, commands);
				[command waitUntilCompleted];
			}
			RequireNativeReleases(commands, i, "allocation must retain only its latest native read command");
		}
		bool completed = false;
		Require(PollMacIOSurfaceReadCompletion(*allocation, completed).IsOk() && completed,
			"final completed read must release the last retained native command");
		RequireNativeReleases(commands, 100, "all completed native read commands must be released");
		Require(probe->m_commandBufferCount == 102, "refusal, terminal error and successful frames must not create extra native work");
	}

	void TestProducerAllocationSharedLifetimeAndReplacement()
	{
		auto textures = Sailor::TSharedPtr<std::atomic<uint32_t>>::Make(0u);
		auto queues = Sailor::TSharedPtr<std::atomic<uint32_t>>::Make(0u);
		MacViewportSurfaceState retainedState;
		@autoreleasepool
		{
			MacLoopbackIOSurfaceProvider provider;
			ViewportDescriptor viewport;
			viewport.m_viewportId = 101;
			viewport.m_width = 64;
			viewport.m_height = 48;
			viewport.m_pixelFormat = PixelFormat::B8G8R8A8_UNorm;
			Require(provider.CreateOrResizeSurface(viewport, 1, 1, retainedState).IsOk(), "initial native allocation must succeed");
			MacViewportSurfaceState oldState = retainedState;
			auto firstOwner = retainedState.m_nativeAllocation;
			ObserveNativeRelease((id)firstOwner->m_producerTextureObject, textures);
			ObserveNativeRelease((id)firstOwner->m_producerCommandQueueObject, queues);
			Require(provider.FindAllocation(retainedState.m_key) == firstOwner.GetRawPtr(), "provider and state must share the same native owner");
			auto invalidViewport = viewport;
			invalidViewport.m_pixelFormat = PixelFormat::Unknown;
			Require(!provider.CreateOrResizeSurface(invalidViewport, 1, 2, retainedState).IsOk(), "unsupported allocation format must fail");
			Require(retainedState.m_nativeAllocation == firstOwner && retainedState.m_key.m_generation == 1 &&
				provider.GetLiveAllocationCount() == 1, "failed creation must preserve the published allocation and generation");
			Require(provider.CreateOrResizeSurface(viewport, 1, 1, retainedState).IsOk(), "same-key replacement must create a new owner");
			Require(retainedState.m_nativeAllocation != firstOwner && provider.GetLiveAllocationCount() == 1,
				"same-key replacement must replace exactly one registered allocation");
			Require(provider.ReleaseSurface(oldState).IsOk(), "retiring the replaced state must succeed");
			Require(provider.FindAllocation(retainedState.m_key) == retainedState.m_nativeAllocation.GetRawPtr(),
				"retiring an old owner must not unregister the same-key replacement");
			oldState.m_nativeAllocation.Clear();
			Require(textures->load() == 0 && queues->load() == 0, "a retained old allocation must keep its native objects alive");
			firstOwner.Clear();
			Require(textures->load() == 1 && queues->load() == 1, "last old owner must release both native objects");
			ObserveNativeRelease((id)retainedState.m_nativeAllocation->m_producerTextureObject, textures);
			ObserveNativeRelease((id)retainedState.m_nativeAllocation->m_producerCommandQueueObject, queues);
			Require(provider.ReleaseSurface(retainedState).IsOk(), "provider must unregister the allocation");
			Require(provider.GetLiveAllocationCount() == 0 && retainedState.m_nativeAllocation->IsValid(), "unregister must not invalidate a retained state");
		}
		Require(textures->load() == 1 && queues->load() == 1, "state must retain its allocation after the provider is destroyed");
		retainedState.m_nativeAllocation.Clear();
		Require(textures->load() == 2 && queues->load() == 2, "last state must release the replacement allocation once");
	}

	void TestProducerCopiesReuseQueueAndPropagateFailure()
	{
		auto releases = Sailor::TSharedPtr<std::atomic<uint32_t>>::Make(0u);
		@autoreleasepool
		{
			MacLoopbackIOSurfaceProvider provider;
			MacViewportTransportBackend backend(provider);
			ViewportDescriptor viewport;
			viewport.m_viewportId = 102;
			viewport.m_width = 64;
			viewport.m_height = 48;
			viewport.m_pixelFormat = PixelFormat::B8G8R8A8_UNorm;
			TransportDescriptor transport;
			Require(backend.EnsureSurface(viewport, 1, 1, transport).IsOk(), "copy test needs a real native surface");
			const auto state = std::as_const(backend).FindSurface(viewport.m_viewportId, 1, 1);
			auto allocation = state->m_nativeAllocation;
			id<MTLCommandQueue> nativeQueue = (id<MTLCommandQueue>)allocation->m_producerCommandQueueObject;
			ObserveNativeRelease(nativeQueue, releases);
			ProducerQueueProbe* probe = [[ProducerQueueProbe alloc] initWithQueue:nativeQueue];
			[nativeQueue release];
			allocation->m_producerCommandQueueObject = reinterpret_cast<uintptr_t>(probe);
			for (uint32_t i = 1; i <= 100; ++i)
			{
				Require(backend.BeginFrame(viewport, 1, 1).IsOk(), "native producer copy must succeed");
				FramePacket frame;
				Require(backend.ExportFrame(viewport, 1, 1, frame).IsOk() && frame.m_frameIndex == i, "completed copy must publish the matching frame index");
				MacNativeBridgeProducerPattern pattern{ viewport.m_viewportId, 1, 1, i, viewport.m_width, viewport.m_height };
				Require(ReadIOSurfaceBGRA8Pixel((IOSurfaceRef)allocation->m_surfaceObject, allocation->m_plane.m_bytesPerRow, 23, 11) ==
					ExpectedProducerPatternBGRA8(pattern, 23, 11), "each native copy must write its own frame pixels");
				Require(probe->m_commandBufferCount == i, "every copy must use the allocation-owned queue");
			}
			probe->m_refuseCommandBuffer = true;
			Require(!backend.BeginFrame(viewport, 1, 1).IsOk(), "native command buffer refusal must propagate");
			probe->m_refuseCommandBuffer = false;
			probe->m_failCompletion = true;
			Require(!backend.BeginFrame(viewport, 1, 1).IsOk(), "failed native terminal status must propagate after the real copy");
			Require(allocation->m_lastWrittenFrameIndex == 100 && !state->m_frameBegun, "failed native work must not publish success metadata");
			FramePacket rejected;
			Require(!backend.ExportFrame(viewport, 1, 1, rejected).IsOk(), "failed copy must not export a frame");
			probe->m_failCompletion = false;
			Require(backend.BeginFrame(viewport, 1, 1).IsOk(), "retry must reuse the existing native queue");
			Require(backend.ExportFrame(viewport, 1, 1, rejected).IsOk() && rejected.m_frameIndex == 101, "retry must not skip failed frame indices");
			id<MTLSharedEvent> event = [[(id<MTLDevice>)allocation->m_producerDeviceObject newSharedEvent] autorelease];
			MacNativeBridgeRendererFrameInfo info;
			info.m_producerCopyToken = 99;
			Require(!CopyMacRendererIntermediateToProducerTexture(*allocation, allocation->m_rendererIntermediateTextureObject, info,
				reinterpret_cast<uintptr_t>(event), 0).IsOk(), "invalid native wait must fail without submission");
			Require(info.m_producerCopyToken == 0 && !info.m_usedGpuCopyIntoProducerTexture, "failed copy must clear previous caller provenance");
			Require(probe->m_commandBufferCount == 104, "all success and failure calls must use the same queue");
			Require(backend.ReleaseSurface(viewport.m_viewportId, 1, 1).IsOk(), "copy test must unregister its native allocation");
			Require(releases->load() == 0, "retained allocation must keep the producer queue alive after unregister");
		}
		RequireNativeReleases(releases, 1, "repeated copies and failures must release their one native queue");
	}

	void TestFailedQueueCreationPreservesBindingAndReleasesCandidate()
	{
		auto layers = Sailor::TSharedPtr<std::atomic<uint32_t>>::Make(0u);
		auto devices = Sailor::TSharedPtr<std::atomic<uint32_t>>::Make(0u);
		TUniquePtr<MacNativeLayerBinding> binding;
		CAMetalLayer* originalLayer = [CAMetalLayer layer];
		Require(BindMacNativeLayer(LayerHandle(originalLayer), 64, 48, PixelFormat::B8G8R8A8_UNorm, binding).IsOk(), "initial layer must bind");
		const auto original = binding.GetRawPtr();
		@autoreleasepool
		{
			QueueFailureLayer* failedLayer = MakeQueueFailureLayer();
			ObserveNativeRelease(failedLayer, layers);
			ObserveNativeRelease(failedLayer->m_failureDevice, devices);
			Require(!BindMacNativeLayer(LayerHandle(failedLayer), 96, 72, PixelFormat::B8G8R8A8_UNorm, binding).IsOk(), "queue creation must report the native failure");
			TUniquePtr<MacNativeLayerBinding> empty;
			Require(!BindMacNativeLayer(LayerHandle(failedLayer), 96, 72, PixelFormat::B8G8R8A8_UNorm, empty).IsOk() && !empty,
				"failed initial binding must not publish a partial native owner");
			Require(binding.GetRawPtr() == original && binding->IsValid(), "failed candidate must not replace the active native owner");
			Require(failedLayer.drawableSize.width != 96 && originalLayer.drawableSize.width == 64,
				"failed preparation must not resize either layer");
		}
		[CATransaction flush];
		Require(layers->load() == 1 && devices->load() == 1,
			"failed candidate must release its layer and device ownership: layers=" + std::to_string(layers->load()) +
			" devices=" + std::to_string(devices->load()));
		FramePacket frame;
		MacNativeBridgePresentResult present;
		Require(PresentMacNativeLayerFrame(*binding, {}, frame, present).IsOk(), "original layer must still present after queue creation failure");
	}

	void TestPresenterResizeResetAndDestructionReleaseQueues()
	{
		auto queues = Sailor::TSharedPtr<std::atomic<uint32_t>>::Make(0u);
		uint32_t created = 0;
		CAMetalLayer* layer = [CAMetalLayer layer];
		ViewportDescriptor viewport;
		viewport.m_viewportId = 98;
		const MacNativePresentationState* firstState = nullptr;
		{
			TUniquePtr<NativeSurface> currentSurface;
			MacLoopbackViewportPresenter presenter;
			presenter.BindHostHandle(viewport.m_viewportId, LayerHandle(layer));
			for (uint32_t i = 0; i < 101; ++i)
			{
				@autoreleasepool
				{
					viewport.m_width = 64 + (i % 8) * 8;
					viewport.m_height = 48 + (i % 5) * 8;
					auto surface = TUniquePtr<NativeSurface>::Make(viewport.m_width, viewport.m_height);
					Require(presenter.ImportSurface(viewport, surface->m_transport, 1, i + 1).IsOk(), "resize must import a real native surface");
					currentSurface = std::move(surface);
					const auto state = presenter.FindImportedState(viewport.m_viewportId);
					Require(state && state->m_layerBinding && state->m_layerBinding->IsValid(), "resize must publish a live native binding");
					if (!firstState) firstState = state;
					Require(state == firstState, "resize must preserve the address of the imported presentation state");
					Require(state->m_generation == i + 1 && layer.drawableSize.width == viewport.m_width &&
						layer.drawableSize.height == viewport.m_height, "resize must publish the matching generation and extent");
					ObserveNativeRelease((id)state->m_layerBinding->m_commandQueueObject, queues);
					++created;
				}
				Require(queues->load() == created - 1, "resize must release the previous native queue");
			}
			presenter.ResetViewport(viewport.m_viewportId);
			Require(!presenter.FindImportedState(viewport.m_viewportId) && queues->load() == created,
				"reset must remove the state and release its native queue");
			@autoreleasepool
			{
				currentSurface = TUniquePtr<NativeSurface>::Make(64, 48);
				Require(presenter.ImportSurface(viewport, currentSurface->m_transport, 2, 1).IsOk(), "viewport must import again after reset");
				ObserveNativeRelease((id)presenter.FindImportedState(viewport.m_viewportId)->m_layerBinding->m_commandQueueObject, queues);
				++created;
			}
		}
		Require(queues->load() == created, "presenter destruction must release a binding without an explicit reset");
	}

	void TestPresenterFailedHostRebindRetryAndDetach()
	{
		auto queues = Sailor::TSharedPtr<std::atomic<uint32_t>>::Make(0u);
		CAMetalLayer* layer = [CAMetalLayer layer];
		ViewportDescriptor viewport;
		viewport.m_viewportId = 99;
		NativeSurface surface(64, 48);
		MacLoopbackViewportPresenter presenter;
		presenter.BindHostHandle(viewport.m_viewportId, LayerHandle(layer));
		Require(presenter.ImportSurface(viewport, surface.m_transport, 1, 1).IsOk(), "initial surface import must succeed");
		const auto state = presenter.FindImportedState(viewport.m_viewportId);
		const auto original = state->m_layerBinding.GetRawPtr();
		ObserveNativeRelease((id)original->m_commandQueueObject, queues);
		QueueFailureLayer* failedLayer = MakeQueueFailureLayer();
		presenter.BindHostHandle(viewport.m_viewportId, LayerHandle(failedLayer));
		Require(!presenter.GetLastFailure().IsOk(), "host rebind must expose queue creation failure");
		Require(state->m_layerBinding.GetRawPtr() == original && state->m_hostHandle == LayerHandle(layer) && queues->load() == 0,
			"failed rebind must keep the actual host and its native owner");
		Require(!presenter.ImportSurface(viewport, surface.m_transport, 1, 2).IsOk(), "import against the failing requested host must fail");
		Require(state->m_generation == 1 && state->m_layerBinding.GetRawPtr() == original, "failed import must preserve the prior generation");
		@autoreleasepool
		{
			FramePacket frame;
			frame.m_viewportId = viewport.m_viewportId;
			frame.m_connectionEpoch = 1;
			frame.m_generation = 1;
			frame.m_frameIndex = 1;
			frame.m_width = 64;
			frame.m_height = 48;
			Require(presenter.PresentFrame(viewport.m_viewportId, frame).IsOk(), "prior imported surface must still present after failed host replacement");
			Require(!state->m_layerBinding->m_usesSyntheticSourceTexture, "recovered presentation must read the real imported IOSurface");
			id<MTLCommandBuffer> completion = [(id<MTLCommandQueue>)original->m_commandQueueObject commandBuffer];
			[completion commit];
			[completion waitUntilCompleted];
		}
		@autoreleasepool
		{
			failedLayer->m_failQueue = false;
			presenter.BindHostHandle(viewport.m_viewportId, LayerHandle(failedLayer));
		}
		Require(presenter.GetLastFailure().IsOk() && state->m_hostHandle == LayerHandle(failedLayer), "retrying the same host must install its recovered native binding");
		RequireNativeReleases(queues, 1, "successful rebind must release the previous queue");
		ObserveNativeRelease((id)state->m_layerBinding->m_commandQueueObject, queues);
		const auto rebound = state->m_layerBinding.GetRawPtr();
		presenter.BindHostHandle(viewport.m_viewportId, LayerHandle(failedLayer));
		Require(state->m_layerBinding.GetRawPtr() == rebound && queues->load() == 1, "binding the current host must not recreate its queue");
		presenter.BindHostHandle(viewport.m_viewportId, {});
		Require(!state->m_layerBinding && !state->m_usesRealCAMetalLayer && !state->m_hostHandle.IsValid() && queues->load() == 2,
			"host detach must clear native ownership and host state");
	}

	void TestNSViewBindingPublishesOnlyOnSuccess()
	{
		NSView* view = [[[NSView alloc] initWithFrame:NSMakeRect(0, 0, 64, 48)] autorelease];
		view.wantsLayer = YES;
		CALayer* originalLayer = view.layer;
		const MacNativeHostHandle host{ MacNativeHostHandleKind::NSView, reinterpret_cast<uintptr_t>(view) };
		TUniquePtr<MacNativeLayerBinding> binding;
		Require(!BindMacNativeLayer(host, 64, 48, PixelFormat::Unknown, binding).IsOk(), "unsupported format must fail before attaching an NSView layer");
		Require(!binding && view.layer == originalLayer, "failed initial bind must preserve the NSView layer");
		Require(BindMacNativeLayer(host, 64, 48, PixelFormat::B8G8R8A8_UNorm, binding).IsOk(), "NSView binding must create and attach a Metal layer");
		Require([view.layer isKindOfClass:[CAMetalLayer class]] && binding->m_hostObject == host.m_value &&
			binding->m_layerObject == reinterpret_cast<uintptr_t>(view.layer), "successful binding must publish the exact attached layer");
		CAMetalLayer* metalLayer = (CAMetalLayer*)view.layer;
		Require(BindMacNativeLayer(host, 96, 72, PixelFormat::B8G8R8A8_UNorm, binding).IsOk(), "NSView resize must reuse its Metal layer");
		Require(view.layer == metalLayer && metalLayer.drawableSize.width == 96 && metalLayer.drawableSize.height == 72,
			"NSView resize must retain its attachment and publish new dimensions");
		binding.Clear();
		Require(view.layer == metalLayer, "releasing binding ownership must not detach the view-owned layer");
	}

	void TestBridgeBindsCAMetalLayerOffMainThreadWithoutWaitingForMainQueue()
	{
		CAMetalLayer* layer = [CAMetalLayer layer];
		MacNativeHostHandle hostHandle{
			MacNativeHostHandleKind::CAMetalLayer,
			reinterpret_cast<uintptr_t>((__bridge void*)layer)
		};

		struct BackgroundBindState
		{
			TUniquePtr<MacNativeLayerBinding> m_binding;
			Failure m_failure{};
			std::mutex m_mutex;
			std::condition_variable m_completedCondition;
			bool m_completed = false;
		};

		auto state = Sailor::TSharedPtr<BackgroundBindState>::Make();
		std::thread binder([state, hostHandle]()
			{
				@autoreleasepool
				{
					state->m_failure = BindMacNativeLayer(
						hostHandle,
						64,
						64,
						PixelFormat::B8G8R8A8_UNorm,
						state->m_binding);
				}
				{
					std::lock_guard lock(state->m_mutex);
					state->m_completed = true;
				}
				state->m_completedCondition.notify_one();
			});

		bool completedWithoutMainQueuePump = false;
		{
			std::unique_lock lock(state->m_mutex);
			completedWithoutMainQueuePump =
				state->m_completedCondition.wait_for(
					lock,
					std::chrono::seconds(2),
					[state]() { return state->m_completed; });
		}

		// Drain a regressed dispatch_sync(main) before reporting the failure so
		// the worker cannot remain blocked after the test exits.
		if (!completedWithoutMainQueuePump)
		{
			const auto cleanupDeadline =
				std::chrono::steady_clock::now() + std::chrono::seconds(2);
			while (std::chrono::steady_clock::now() < cleanupDeadline)
			{
				{
					std::lock_guard lock(state->m_mutex);
					if (state->m_completed)
					{
						break;
					}
				}

				@autoreleasepool
				{
					[[NSRunLoop mainRunLoop]
						runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.01]];
				}
			}
		}

		bool completedEventually = false;
		{
			std::lock_guard lock(state->m_mutex);
			completedEventually = state->m_completed;
		}
		if (completedEventually)
		{
			binder.join();
			state->m_binding.Clear();
		}
		else
		{
			binder.detach();
		}

		Require(
			completedEventually,
			"background CAMetalLayer bind should eventually complete while unwinding the regression test");
		Require(
			completedWithoutMainQueuePump,
			"CAMetalLayer binding must not synchronously dispatch to the main queue");
		Require(
			state->m_failure.IsOk(),
			"background CAMetalLayer binding should succeed");
	}

	void TestBridgePresentsOffMainThreadWithoutWaitingForMainQueue()
	{
		CAMetalLayer* layer = [CAMetalLayer layer];
		MacNativeHostHandle hostHandle{ MacNativeHostHandleKind::CAMetalLayer, reinterpret_cast<uintptr_t>((__bridge void*)layer) };

		struct BackgroundPresentState
		{
			TUniquePtr<MacNativeLayerBinding> m_binding;
			MacIOSurfaceHandle m_surface{};
			FramePacket m_frame{};
			MacNativeBridgePresentResult m_presentResult{};
			Failure m_failure{};
			std::mutex m_mutex;
			std::condition_variable m_completedCondition;
			bool m_completed = false;
		};

		auto state = Sailor::TSharedPtr<BackgroundPresentState>::Make();
		Require(BindMacNativeLayer(hostHandle, 64, 64, PixelFormat::B8G8R8A8_UNorm, state->m_binding).IsOk(),
			"background-present test should bind a real CAMetalLayer on the main thread");
		state->m_frame.m_viewportId = 1;
		state->m_frame.m_connectionEpoch = 1;
		state->m_frame.m_generation = 1;
		state->m_frame.m_frameIndex = 1;
		state->m_frame.m_width = 64;
		state->m_frame.m_height = 64;

		std::thread presenter([state]()
			{
				state->m_failure = PresentMacNativeLayerFrame(
					*state->m_binding,
					state->m_surface,
					state->m_frame,
					state->m_presentResult);
				{
					std::lock_guard lock(state->m_mutex);
					state->m_completed = true;
				}
				state->m_completedCondition.notify_one();
			});

		bool completedWithoutMainQueuePump = false;
		{
			std::unique_lock lock(state->m_mutex);
			completedWithoutMainQueuePump = state->m_completedCondition.wait_for(
				lock,
				std::chrono::seconds(2),
				[state]() { return state->m_completed; });
		}

		// If this regresses to dispatch_sync(main), drain the queued block before
		// failing so the test does not leave a permanently blocked worker behind.
		if (!completedWithoutMainQueuePump)
		{
			const auto cleanupDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
			while (std::chrono::steady_clock::now() < cleanupDeadline)
			{
				{
					std::lock_guard lock(state->m_mutex);
					if (state->m_completed)
					{
						break;
					}
				}

				@autoreleasepool
				{
					[[NSRunLoop mainRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.01]];
				}
			}
		}

		bool completedEventually = false;
		{
			std::lock_guard lock(state->m_mutex);
			completedEventually = state->m_completed;
		}
		if (completedEventually)
		{
			presenter.join();
			state->m_binding.Clear();
		}
		else
		{
			presenter.detach();
		}

		Require(completedEventually, "background present should eventually complete while unwinding the regression test");
		Require(completedWithoutMainQueuePump,
			"per-frame CAMetalLayer present must not synchronously dispatch to the main queue");
		Require(state->m_failure.IsOk(), "background CAMetalLayer present should succeed");
		Require(state->m_presentResult.IsValid(), "background CAMetalLayer present should produce a real present token");
	}
}

int main()
{
	const std::pair<const char*, std::function<void()>> tests[] = {
		{ "LoopbackDefersWritesUntilPresentationCompletes", TestLoopbackDefersWritesUntilPresentationCompletes },
		{ "PresentFailuresAndCompletedReadRetention", TestPresentFailuresAndCompletedReadRetention },
		{ "ProviderDestructionReleasesNativeTextures", TestProviderDestructionReleasesNativeTextures },
		{ "ProducerAllocationSharedLifetimeAndReplacement", TestProducerAllocationSharedLifetimeAndReplacement },
		{ "ProducerCopiesReuseQueueAndPropagateFailure", TestProducerCopiesReuseQueueAndPropagateFailure },
		{ "BindingFailurePreservesCurrentLayer", TestBindingFailurePreservesCurrentLayer },
		{ "NativeBindingReleasesOwnedObjects", TestNativeBindingReleasesOwnedObjects },
		{ "FailedQueueCreationPreservesBindingAndReleasesCandidate", TestFailedQueueCreationPreservesBindingAndReleasesCandidate },
		{ "PresenterResizeResetAndDestructionReleaseQueues", TestPresenterResizeResetAndDestructionReleaseQueues },
		{ "PresenterFailedHostRebindRetryAndDetach", TestPresenterFailedHostRebindRetryAndDetach },
		{ "NSViewBindingPublishesOnlyOnSuccess", TestNSViewBindingPublishesOnlyOnSuccess },
		{ "GetMacRendererSourceSelectionPriorityPrefersSceneViewResolvedOutputs", TestGetMacRendererSourceSelectionPriorityPrefersSceneViewResolvedOutputs },
		{ "SelectMacVulkanSemaphoreForMetalExportPrefersDedicatedMainResolvedSeam", TestSelectMacVulkanSemaphoreForMetalExportPrefersDedicatedMainResolvedSeam },
		{ "SynchronizeMacVulkanRenderTargetPrefersMetalSharedEventWhenSemaphoreExportSeamExists", TestSynchronizeMacVulkanRenderTargetPrefersMetalSharedEventWhenSemaphoreExportSeamExists },
		{ "BridgeWaitsOnMetalSharedEventBeforeProducerCopy", TestBridgeWaitsOnMetalSharedEventBeforeProducerCopy },
		{ "BridgeBindsExistingCAMetalLayerAndPresentsDrawable", TestBridgeBindsExistingCAMetalLayerAndPresentsDrawable },
		{ "BridgeBindsCAMetalLayerOffMainThreadWithoutWaitingForMainQueue", TestBridgeBindsCAMetalLayerOffMainThreadWithoutWaitingForMainQueue },
		{ "BridgePresentsOffMainThreadWithoutWaitingForMainQueue", TestBridgePresentsOffMainThreadWithoutWaitingForMainQueue },
	};

	for (const auto& test : tests)
	{
		try
		{
			@autoreleasepool
			{
				test.second();
			}
			std::cout << "[PASS] " << test.first << std::endl;
		}
		catch (const std::exception& e)
		{
			std::cerr << "[FAIL] " << test.first << ": " << e.what() << std::endl;
			return 1;
		}
	}

	return 0;
}

#else
int main()
{
	return 0;
}
#endif
