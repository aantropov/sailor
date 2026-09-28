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
}
@end

@implementation QueueFailureLayer
- (id<MTLDevice>)device { return m_failQueue ? m_failureDevice : [super device]; }
- (void)dealloc
{
	[m_failureDevice release];
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
		id<MTLDevice> device = MTLCreateSystemDefaultDevice();
		Require(device != nil, "shared-event test requires a real Metal device");
		id<MTLSharedEvent> sharedEvent = [device newSharedEvent];
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

		uintptr_t producerDeviceObject = 0;
		uintptr_t producerTextureObject = 0;
		Require(CreateMacIOSurfaceProducerTexture(reinterpret_cast<uintptr_t>(surface), width, height, PixelFormat::B8G8R8A8_UNorm, 0, producerDeviceObject, producerTextureObject).IsOk(), "shared-event test should create producer texture");
		uintptr_t rendererTextureObject = 0;
		Require(CreateMacRendererIntermediateTexture(producerDeviceObject, width, height, PixelFormat::B8G8R8A8_UNorm, rendererTextureObject).IsOk(), "shared-event test should create renderer texture");

		MacNativeBridgeProducerPattern pattern{ 91, 1, 1, 1, width, height };
		Require(UploadMacRendererPatternToIntermediateTexture(rendererTextureObject, width, height, pattern).IsOk(), "shared-event test should fill renderer texture");

		id<MTLCommandQueue> signalQueue = [device newCommandQueue];
		Require(signalQueue != nil, "shared-event test requires a Metal command queue for signaling");
		id<MTLCommandBuffer> signalBuffer = [signalQueue commandBuffer];
		Require(signalBuffer != nil, "shared-event test requires a Metal command buffer for signaling");
		[signalBuffer encodeSignalEvent:sharedEvent value:9ull];
		[signalBuffer commit];

		MacNativeBridgeRendererFrameInfo frameInfo{};
		Require(CopyMacRendererIntermediateToProducerTexture(producerDeviceObject, rendererTextureObject, producerTextureObject, width, height, frameInfo, reinterpret_cast<uintptr_t>((__bridge void*)sharedEvent), 9ull).IsOk(), "shared-event test should copy through a Metal shared-event wait");
		Require(frameInfo.m_waitedOnCrossApiSharedEvent, "shared-event test should report that the producer copy waited on a Metal shared event");
		Require(frameInfo.m_crossApiWaitValue == 9ull, "shared-event test should preserve the waited Metal shared-event value");
		Require(ReadIOSurfaceBGRA8Pixel(surface, width * 4u, 0, 0) == ExpectedProducerPatternBGRA8(pattern, 0, 0), "shared-event test should still land the renderer pixel into the IOSurface after the wait");

		ReleaseMacRendererIntermediateTexture(rendererTextureObject);
		ReleaseMacIOSurfaceProducerTexture(producerDeviceObject, producerTextureObject);
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

		uintptr_t producerDeviceObject = 0;
		uintptr_t producerTextureObject = 0;
		auto producerTextureResult = CreateMacIOSurfaceProducerTexture(surfaceHandle.m_surfaceObject, frame.m_width, frame.m_height, PixelFormat::B8G8R8A8_UNorm, 0, producerDeviceObject, producerTextureObject);
		Require(producerTextureResult.IsOk(), "test should materialize a producer-side IOSurface-backed Metal texture");
		Require(producerDeviceObject != 0 && producerTextureObject != 0, "producer-side Metal texture creation should yield live opaque Metal objects");

		MacNativeBridgeProducerPattern pattern{};
		pattern.m_viewportId = frame.m_viewportId;
		pattern.m_epoch = frame.m_connectionEpoch;
		pattern.m_generation = frame.m_generation;
		pattern.m_frameIndex = frame.m_frameIndex;
		pattern.m_width = frame.m_width;
		pattern.m_height = frame.m_height;
		uintptr_t rendererTextureObject = 0;
		auto rendererTextureResult = CreateMacRendererIntermediateTexture(producerDeviceObject, frame.m_width, frame.m_height, PixelFormat::B8G8R8A8_UNorm, rendererTextureObject);
		Require(rendererTextureResult.IsOk(), "test should materialize a renderer-shaped intermediate Metal texture");
		Require(rendererTextureObject != 0, "renderer-shaped intermediate Metal texture should yield a live opaque Metal object");

		auto uploadResult = UploadMacRendererPatternToIntermediateTexture(rendererTextureObject, frame.m_width, frame.m_height, pattern);
		Require(uploadResult.IsOk(), "test should upload renderer-shaped content into the intermediate texture before the producer copy");

		MacNativeBridgeRendererFrameInfo rendererFrameInfo{};
		auto copyResult = CopyMacRendererIntermediateToProducerTexture(producerDeviceObject, rendererTextureObject, producerTextureObject, frame.m_width, frame.m_height, rendererFrameInfo);
		Require(copyResult.IsOk(), "test should copy renderer-shaped output into the IOSurface-backed producer texture");
		Require(rendererFrameInfo.m_usedRendererIntermediateTexture, "copy result should report renderer-intermediate usage");
		Require(rendererFrameInfo.m_usedGpuCopyIntoProducerTexture, "copy result should report GPU copy into the producer texture");
		Require(rendererFrameInfo.m_rendererTextureToken != 0 && rendererFrameInfo.m_producerCopyToken != 0, "copy result should stamp renderer/copy tokens");
		Require(ReadIOSurfaceBGRA8Pixel(surface, surfaceHandle.m_bytesPerRow, 0, 0) == ExpectedProducerPatternBGRA8(pattern, 0, 0), "renderer-intermediate GPU copy should populate the shared IOSurface with the deterministic BGRA pattern");
		Require(ReadIOSurfaceBGRA8Pixel(surface, surfaceHandle.m_bytesPerRow, 23, 11) == ExpectedProducerPatternBGRA8(pattern, 23, 11), "renderer-intermediate GPU copy should populate more than the first pixel in the shared IOSurface");

		auto present = PresentMacNativeLayerFrame(*binding, surfaceHandle, frame, presentResult);
		ReleaseMacRendererIntermediateTexture(rendererTextureObject);
		ReleaseMacIOSurfaceProducerTexture(producerDeviceObject, producerTextureObject);
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
		// Core Animation also owns the presented layer until its transaction drains.
		[CATransaction flush];
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
		while (layers->load() == 0 && std::chrono::steady_clock::now() < deadline)
		{
			@autoreleasepool
			{
				[[NSRunLoop mainRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.01]];
			}
		}
		Require(layers->load() == 1 && queues->load() == 1 && textures->load() == 1,
			"clearing the binding must release each owned native object exactly once: layers=" + std::to_string(layers->load()) +
			" queues=" + std::to_string(queues->load()) + " textures=" + std::to_string(textures->load()));
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
		Require(queues->load() == 1, "successful rebind must release the previous queue");
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
