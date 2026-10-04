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
#include "RHI/Fence.h"
#include "RHI/Texture.h"
#include "Memory/SharedPtr.hpp"
#include "Support/MacViewportTestSource.h"

using Sailor::TUniquePtr;
using namespace Sailor::EditorRemote;
using namespace Sailor::Tests;

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

	void CompleteNativeCopy(MacIOSurfaceAllocation& allocation)
	{
		[(id<MTLCommandBuffer>)allocation.m_copyCommandBufferObject waitUntilCompleted];
		bool completed = false;
		Require(PollMacIOSurfaceCopyCompletion(allocation, completed).IsOk() && completed, "native producer copy must complete successfully");
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

	void TestUnfinishedRendererSubmissionDoesNotExportMetalTexture()
	{
		using namespace Sailor::RHI;
		auto texture = RHITexturePtr::Make(ETextureFiltration::Linear, ETextureClamping::Clamp, false);
		auto completion = RHIFencePtr::Make();
		uintptr_t exported = 0;
		for (uint32_t i = 0; i < 100; ++i)
			Require(ExportMacMetalTextureFromVulkanRenderTarget(*texture, *completion, exported).IsOk() && exported == 0,
				"an unsubmitted frame must defer native export without requiring a Vulkan device");
		completion->MarkSubmissionFailed();
		Require(ExportMacMetalTextureFromVulkanRenderTarget(*texture, *completion, exported).m_nativeCode == 1028 && exported == 0,
			"a failed renderer submission must never export a texture as completed");
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
		CompleteNativeCopy(*producer);
		Require(frameInfo.m_waitedOnCrossApiSharedEvent, "shared-event test should report that the producer copy waited on a Metal shared event");
		Require(frameInfo.m_crossApiWaitValue == 9ull, "shared-event test should preserve the waited Metal shared-event value");
		Require(ReadIOSurfaceBGRA8Pixel(surface, width * 4u, 0, 0) == ExpectedProducerPatternBGRA8(pattern, 0, 0), "shared-event test should still land the renderer pixel into the IOSurface after the wait");

		ReleaseMacRendererIntermediateTexture(rendererTextureObject);
		CFRelease(surface);
	}

	void TestPresentationDoesNotCapturePixelsAutomatically()
	{
		NativeSurface surface(64, 48);
		MacLoopbackViewportPresenter presenter;
		ViewportDescriptor viewport;
		viewport.m_viewportId = 108;
		viewport.m_width = 64;
		viewport.m_height = 48;
		viewport.m_pixelFormat = PixelFormat::B8G8R8A8_UNorm;
		presenter.BindHostHandle(viewport.m_viewportId, LayerHandle([CAMetalLayer layer]));
		Require(presenter.ImportSurface(viewport, surface.m_transport, 1, 1).IsOk(), "sampling test needs a real IOSurface");
		FramePacket frame;
		frame.m_viewportId = viewport.m_viewportId;
		frame.m_connectionEpoch = 1;
		frame.m_generation = 1;
		frame.m_width = 64;
		frame.m_height = 48;
		for (uint32_t i = 1; i <= 120; ++i)
		{
			frame.m_frameIndex = i;
			Require(presenter.PresentFrame(viewport.m_viewportId, frame).IsOk(), "ordinary presentation must succeed");
			presenter.BuildViewportSummary(viewport.m_viewportId);
			Require(!presenter.FindImportedState(viewport.m_viewportId)->m_hasFrameEvidence,
				"presentation and status polling must not sample pixels automatically");
			Require(presenter.FindImportedState(viewport.m_viewportId)->m_evidenceCaptureCount == 0,
				"ordinary frames must never call the native pixel capture helper");
		}
	}

	void TestMissingRendererFramesPreserveLastPresentation()
	{
		class Source : public IMacRendererFrameSourceProvider
		{
		public:
			MacRendererFrameSource m_next;
			uint32_t m_calls = 0;
			Failure m_failure = Failure::Ok();
			Failure AcquireFrameSource(const MacViewportSurfaceState&, FrameIndex, MacRendererFrameSource& out) override
			{
				++m_calls;
				out = m_next;
				return m_failure;
			}
		} source;
		MacLoopbackIOSurfaceProvider provider(&source);
		MacLoopbackViewportPresenter presenter;
		ViewportDescriptor viewport;
		viewport.m_viewportId = 107;
		viewport.m_width = 64;
		viewport.m_height = 48;
		viewport.m_pixelFormat = PixelFormat::B8G8R8A8_UNorm;
		viewport.m_colorSpace = ColorSpace::Srgb;
		viewport.m_presentMode = PresentMode::Mailbox;
		MacViewportLoopbackBinding binding(viewport, provider, presenter);
		presenter.BindHostHandle(viewport.m_viewportId, LayerHandle([CAMetalLayer layer]));
		Require(binding.Create().IsOk(), "missing-frame test needs a real transport");
		auto allocation = std::as_const(binding.GetTransportBackend()).FindSurface(viewport.m_viewportId, 1, 1)->m_nativeAllocation;
		for (uint32_t i = 0; i < 100; ++i)
		{
			Require(binding.PumpFrame().IsOk(), "cold start without a source is not an error");
			Require(allocation->m_copyCommandBufferObject == 0 && binding.GetRuntimeSession().GetLastPublishedFrameIndex() == 0,
				"cold start must not submit or publish a synthetic frame");
		}
		Require(source.m_calls == 100 && allocation->m_cpuUploadedBytes == 0 &&
			presenter.FindImportedState(viewport.m_viewportId)->m_presentedFrameCount == 0,
			"missing source must remain retryable without presentation");
		MacRendererFrameSource frame;
		frame.m_kind = MacRendererFrameSourceKind::RendererOwnedRenderTargetMetadata;
		frame.m_width = 64;
		frame.m_height = 48;
		frame.m_pixelFormat = PixelFormat::B8G8R8A8_UNorm;
		frame.m_bytesPerRow = 64 * 4;
		frame.m_sourceToken = 42;
		frame.m_cpuBytes = Sailor::TSharedPtr<std::vector<uint8_t>>::Make(64 * 48 * 4, 0x30);
		source.m_next = frame;
		Require(binding.PumpFrame().IsOk() && binding.GetRuntimeSession().GetLastPublishedFrameIndex() == 1,
			"first real frame must publish without a skipped index");
		[(id<MTLCommandBuffer>)allocation->m_presentCommandBufferObject waitUntilCompleted];
		const auto token = allocation->m_lastProducerCopyToken;
		const uint64_t payload = 64u * 48u * 4u;
		Require(allocation->m_cpuUploadedBytes == payload, "first real source must account for one native upload");
		for (uint32_t mode = 0; mode < 4; ++mode)
		{
			source.m_next = frame;
			if (mode == 0) source.m_next = {};
			if (mode == 1) source.m_next.m_cpuBytes.Clear();
			if (mode == 2) source.m_next.m_width = 32;
			if (mode == 3)
			{
				source.m_next.m_kind = MacRendererFrameSourceKind::RendererOwnedMetalTexture;
				source.m_next.m_cpuBytes.Clear();
			}
			for (uint32_t i = 0; i < 20; ++i) Require(binding.PumpFrame().IsOk(), "temporarily unavailable source must defer");
			Require(binding.GetRuntimeSession().GetLastPublishedFrameIndex() == 1 && allocation->m_lastWrittenFrameIndex == 1 &&
				allocation->m_lastProducerCopyToken == token && allocation->m_lastRendererSource.m_sourceToken == 42 &&
				allocation->m_cpuUploadedBytes == payload,
				"missing, metadata-only, stale and texture-less sources must preserve completed provenance");
			Require(presenter.FindImportedState(viewport.m_viewportId)->m_presentedFrameCount == 1,
				"no-frame-yet must not re-present the old frame");
			Require(ReadIOSurfaceBGRA8Pixel((IOSurfaceRef)allocation->m_surfaceObject, allocation->m_plane.m_bytesPerRow, 23, 11) == 0x30303030u,
				"no-frame-yet must preserve the last real pixels");
		}
		frame.m_cpuBytes = Sailor::TSharedPtr<std::vector<uint8_t>>::Make(64 * 48 * 4, 0x55);
		frame.m_sourceToken = 43;
		source.m_next = frame;
		Require(binding.PumpFrame().IsOk() && binding.GetRuntimeSession().GetLastPublishedFrameIndex() == 2,
			"source recovery must publish the next real frame");
		Require(allocation->m_cpuUploadedBytes == 2u * payload, "source recovery must account for only the new upload");
		Require(allocation->m_lastRendererSource.m_sourceToken == 43 &&
			ReadIOSurfaceBGRA8Pixel((IOSurfaceRef)allocation->m_surfaceObject, allocation->m_plane.m_bytesPerRow, 23, 11) == 0x55555555u,
			"recovery must publish the new source's pixels and provenance");
		[(id<MTLCommandBuffer>)allocation->m_presentCommandBufferObject waitUntilCompleted];
		source.m_failure = Failure::FromDomain(ErrorDomain::Session, 77, "test source refused");
		Require(binding.PumpFrame().m_nativeCode == 77 && binding.GetRuntimeSession().GetLastPublishedFrameIndex() == 2,
			"source failure must propagate without publishing a substitute frame");
		source.m_failure = Failure::Ok();
		Require(binding.PumpFrame().IsOk() && binding.GetRuntimeSession().GetLastPublishedFrameIndex() == 3,
			"source failure must remain retryable without skipped frame indices");
	}

	void TestInvalidIOSurfaceDoesNotPresentSyntheticPixels()
	{
		Sailor::TUniquePtr<MacNativeLayerBinding> binding;
		Require(BindMacNativeLayer(LayerHandle([CAMetalLayer layer]), 64, 48, PixelFormat::B8G8R8A8_UNorm, binding).IsOk(),
			"invalid-import test needs a native layer");
		FramePacket frame;
		frame.m_width = 64;
		frame.m_height = 48;
		frame.m_frameIndex = 1;
		MacNativeBridgePresentResult result;
		Require(PresentMacNativeLayerFrame(*binding, {}, frame, result).m_nativeCode == 2116,
			"invalid IOSurface must fail instead of creating synthetic pixels");
		Require(!result.IsValid() && binding->m_presentToken == 0 && binding->m_lastSourceTextureObject == 0,
			"failed import must not publish a successful native frame");
		NativeSurface surface(64, 48);
		Require(PresentMacNativeLayerFrame(*binding, surface.m_transport.m_macSurfaces.front(), frame, result).IsOk() && result.IsValid(),
			"valid IOSurface must recover on the same binding");
		const auto presentedToken = binding->m_presentToken;
		const auto presentedSource = binding->m_lastSourceTextureObject;
		Require(PresentMacNativeLayerFrame(*binding, {}, frame, result).m_nativeCode == 2116 && !result.IsValid(),
			"invalid import after a good frame must still fail");
		Require(binding->m_presentToken == presentedToken && binding->m_lastSourceTextureObject == presentedSource,
			"failed import must preserve the prior native frame");
		Require(PresentMacNativeLayerFrame(*binding, surface.m_transport.m_macSurfaces.front(), frame, result).IsOk() &&
			binding->m_presentToken == presentedToken + 1, "valid retry must use the next present token");
	}

	void TestProducerCopyReturnsBeforeSourceCompletion()
	{
		NativeSurface surface(64, 48);
		auto producer = MakeNativeProducer(surface.m_surface, 64, 48);
		uintptr_t source = 0;
		Require(CreateMacRendererIntermediateTexture(producer->m_producerDeviceObject, 64, 48, PixelFormat::B8G8R8A8_UNorm, source).IsOk(),
			"nonblocking copy test needs a native source");
		const MacNativeBridgeProducerPattern pattern{ 105, 1, 1, 1, 64, 48 };
		Require(UploadMacRendererPatternToIntermediateTexture(source, 64, 48, pattern).IsOk(), "source pixels must be prepared");
		id<MTLSharedEvent> gate = [[(id<MTLDevice>)producer->m_producerDeviceObject newSharedEvent] autorelease];
		Require(gate != nil, "copy test needs a native event");
		std::promise<void> finished;
		auto finish = finished.get_future();
		std::thread watchdog([&]()
		{
			if (finish.wait_for(std::chrono::milliseconds(250)) == std::future_status::timeout) gate.signaledValue = 9;
		});
		MacNativeBridgeRendererFrameInfo info;
		const auto result = CopyMacRendererIntermediateToProducerTexture(*producer, source, info, reinterpret_cast<uintptr_t>(gate), 9);
		const bool returnedBeforeSignal = gate.signaledValue == 0;
		gate.signaledValue = 9;
		finished.set_value();
		watchdog.join();
		id<MTLCommandBuffer> completion = [(id<MTLCommandQueue>)producer->m_producerCommandQueueObject commandBuffer];
		[completion commit];
		[completion waitUntilCompleted];
		ReleaseMacRendererIntermediateTexture(source);
		Require(returnedBeforeSignal, "producer copy must return while the actual GPU source event is still unsignaled");
		Require(result.IsOk(), "copy submission must succeed: " + result.m_message);
		CompleteNativeCopy(*producer);
		Require(ReadIOSurfaceBGRA8Pixel(surface.m_surface, producer->m_plane.m_bytesPerRow, 23, 11) ==
			ExpectedProducerPatternBGRA8(pattern, 23, 11), "submitted copy must preserve pixels after source completion");
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
		CompleteNativeCopy(*producer);
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
			NativeSurface surface(64, 48);
			Require(PresentMacNativeLayerFrame(*binding, surface.m_transport.m_macSurfaces.front(), frame, present).IsOk(), "prior binding must still present after rejected replacements");
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
			NativeSurface surface(64, 48);
			Require(PresentMacNativeLayerFrame(*binding, surface.m_transport.m_macSurfaces.front(), frame, present).IsOk(), "native owner must present a source texture");
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
		}
		Require(textures->load() == 1, "provider and state destruction must release the owned producer texture without manual cleanup");
	}

	class CpuSource : public IMacRendererFrameSourceProvider
	{
	public:
		uint32_t m_calls = 0;
		MacRendererFrameSource m_immutableSource;
		Failure AcquireFrameSource(const MacViewportSurfaceState& state, FrameIndex frameIndex, MacRendererFrameSource& out) override
		{
			++m_calls;
			if (m_immutableSource.IsValid())
			{
				out = m_immutableSource;
				return Failure::Ok();
			}
			out.m_kind = MacRendererFrameSourceKind::RendererOwnedRenderTargetMetadata;
			out.m_width = state.m_viewport.m_width;
			out.m_height = state.m_viewport.m_height;
			out.m_pixelFormat = state.m_viewport.m_pixelFormat;
			out.m_bytesPerRow = out.m_width * 4;
			out.m_cpuBytes = Sailor::TSharedPtr<std::vector<uint8_t>>::Make(out.m_bytesPerRow * out.m_height, static_cast<uint8_t>(frameIndex));
			return Failure::Ok();
		}
	};

	void TestNativeSourceTextureReuse()
	{
		bool reimported = false;
		for (const auto [width, height] : { std::pair{1280u, 720u}, std::pair{1920u, 1080u}, std::pair{3840u, 2160u} })
		{
			@autoreleasepool
			{
				CpuSource source;
				MacLoopbackIOSurfaceProvider provider(&source);
				MacLoopbackViewportPresenter presenter;
				ViewportDescriptor viewport;
				viewport.m_viewportId = 114;
				viewport.m_width = width;
				viewport.m_height = height;
				viewport.m_pixelFormat = PixelFormat::B8G8R8A8_UNorm;
				MacViewportLoopbackBinding binding(viewport, provider, presenter);
				presenter.BindHostHandle(viewport.m_viewportId, LayerHandle([CAMetalLayer layer]));
				Require(binding.Create().IsOk(), "texture reuse must exercise a real native binding");
				auto allocation = std::as_const(binding.GetTransportBackend()).FindSurface(viewport.m_viewportId, 1, 1)->m_nativeAllocation;
				const auto native = presenter.FindImportedState(viewport.m_viewportId)->m_layerBinding.GetRawPtr();
				id<MTLTexture> first = nil;
				double pumpUs = 0;
				for (uint32_t i = 1; i <= 16; ++i)
				{
					const auto start = std::chrono::steady_clock::now();
					const auto result = binding.PumpFrame();
					pumpUs += std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count();
					Require(result.IsOk(), "new CPU pixels must reach native presentation");
					if (i == 1) first = [[(id<MTLTexture>)native->m_lastSourceTextureObject retain] autorelease];
					reimported |= native->m_lastSourceTextureObject != reinterpret_cast<uintptr_t>(first);
					id<MTLCommandBuffer> command = (id<MTLCommandBuffer>)allocation->m_presentCommandBufferObject;
					[command waitUntilCompleted];
					Require(command.status == MTLCommandBufferStatusCompleted, "presentation must finish reading the uploaded pixels");
					uint32_t pixel = 0;
					[(id<MTLTexture>)native->m_lastSourceTextureObject getBytes:&pixel bytesPerRow:4
						fromRegion:MTLRegionMake2D(width / 2, height / 2, 1, 1) mipmapLevel:0];
					Require(pixel == i * 0x01010101u && native->m_presentToken == i,
						"retained IOSurface texture must expose each new frame's pixels and normal present token");
				}
				std::cout << "Native source import " << width << 'x' << height << ": 16 new-frame pumps " << pumpUs << " us\n";
				bool completed = false;
				Require(PollMacIOSurfaceReadCompletion(*allocation, completed).IsOk() && completed,
					"native reuse test must retire its final read");
			}
		}
		Require(!reimported, "new pixels in an existing IOSurface must not create another Metal texture import");
	}

	void TestNativeSourceImportReplacement()
	{
		NativeSurface first(64, 48), second(64, 48), resized(96, 72);
		TUniquePtr<MacNativeLayerBinding> binding;
		CAMetalLayer* layer = [CAMetalLayer layer];
		Require(BindMacNativeLayer(LayerHandle(layer), 64, 48, PixelFormat::B8G8R8A8_UNorm, binding).IsOk(),
			"replacement test requires a real native binding");
		auto present = [&](const NativeSurface& surface)
			{
				FramePacket frame;
				MacNativeBridgePresentResult result;
				Require(PresentMacNativeLayerFrame(*binding, surface.m_transport.m_macSurfaces.front(), frame, result).IsOk(),
					"replacement IOSurface must present through the current binding");
				id<MTLCommandBuffer> completion = [(id<MTLCommandQueue>)binding->m_commandQueueObject commandBuffer];
				[completion commit];
				[completion waitUntilCompleted];
				return [[(id<MTLTexture>)result.m_sourceTextureObject retain] autorelease];
			};
		id<MTLTexture> a = present(first);
		Require(present(first) == a, "the same source and binding must reuse the native import");
		id<MTLTexture> b = present(second);
		Require(b != a && b.iosurface == second.m_surface && present(second) == b,
			"another IOSurface at the same size must replace, then reuse, its native import");
		Require(BindMacNativeLayer(LayerHandle(layer), 96, 72, PixelFormat::B8G8R8A8_UNorm, binding).IsOk() &&
			binding->m_lastSourceTextureObject == 0, "resizing must create a binding without a stale texture import");
		id<MTLTexture> c = present(resized);
		Require(c != b && c.width == 96 && c.height == 72 && c.iosurface == resized.m_surface,
			"resized import must belong to the new extent and source");
		Require(BindMacNativeLayer(LayerHandle([CAMetalLayer layer]), 96, 72, PixelFormat::B8G8R8A8_UNorm, binding).IsOk() &&
			binding->m_lastSourceTextureObject == 0, "host replacement must not carry an old binding's source texture");
		id<MTLTexture> d = present(resized);
		Require(d != c && d.device == (id<MTLDevice>)binding->m_deviceObject && present(resized) == d,
			"the replacement host must create and then reuse its own native import");
	}

	void TestRequestedFrameEvidencePreservesPresentation()
	{
		CpuSource source;
		MacLoopbackIOSurfaceProvider provider(&source);
		MacLoopbackViewportPresenter presenter;
		ViewportDescriptor viewport;
		viewport.m_viewportId = 109;
		viewport.m_width = 64;
		viewport.m_height = 48;
		viewport.m_pixelFormat = PixelFormat::B8G8R8A8_UNorm;
		viewport.m_colorSpace = ColorSpace::Srgb;
		viewport.m_presentMode = PresentMode::Mailbox;
		MacViewportLoopbackBinding binding(viewport, provider, presenter);
		presenter.BindHostHandle(viewport.m_viewportId, LayerHandle([CAMetalLayer layer]));
		Require(binding.Create().IsOk(), "capture needs a real loopback surface");
		Require(presenter.CaptureFrameEvidence(viewport.m_viewportId).m_nativeCode == 2125, "cold start cannot capture a frame");
		Require(binding.PumpFrame().IsOk(), "first real frame must present");
		auto state = presenter.FindImportedState(viewport.m_viewportId);
		Require(state->m_evidenceCaptureCount == 0 && presenter.CaptureFrameEvidence(viewport.m_viewportId).IsOk(),
			"one explicit request must capture the real frame");
		Require(state->m_evidenceCaptureCount == 1 && state->m_evidenceFrameIndex == 1 &&
			state->m_lastFrameEvidence.m_center.m_r == 1 && state->m_lastFrameEvidence.m_sampledPixelCount == 64,
			"requested evidence must contain the presented frame's pixels and index");
		auto allocation = state->m_nativeAllocation;
		[(id<MTLCommandBuffer>)allocation->m_presentCommandBufferObject waitUntilCompleted];
		Require(binding.PumpFrame().IsOk(), "second real frame must present");
		for (uint32_t i = 0; i < 100; ++i) presenter.BuildViewportSummary(viewport.m_viewportId);
		Require(state->m_evidenceCaptureCount == 1 && state->m_evidenceFrameIndex == 1 && state->m_lastPresentedFrameIndex == 2,
			"later presentation and polling must retain the old capture's explicit frame identity");
		Require(presenter.CaptureFrameEvidence(viewport.m_viewportId).IsOk() && state->m_evidenceCaptureCount == 2 &&
			state->m_evidenceFrameIndex == 2 && state->m_lastFrameEvidence.m_center.m_r == 2,
			"the next explicit request must capture the new real pixels exactly once");

		auto& importedSurface = const_cast<MacNativePresentationState*>(state)->m_importedSurface;
		const auto savedSurface = importedSurface;
		importedSurface->m_surfaceObject = 0;
		const auto failedCapture = presenter.CaptureFrameEvidence(viewport.m_viewportId);
		importedSurface = savedSurface;
		Require(failedCapture.m_nativeCode == 2120 && state->m_evidenceCaptureCount == 3 && state->m_evidenceFrameIndex == 2 &&
			presenter.GetLastFailure().IsOk() && binding.GetRuntimeSession().GetLastPublishedFrameIndex() == 2,
			"native diagnostic failure must preserve prior evidence and presentation state");
		Require(presenter.CaptureFrameEvidence(viewport.m_viewportId).IsOk() && state->m_evidenceCaptureCount == 4,
			"failed capture must allow a later explicit retry");
		[(id<MTLCommandBuffer>)allocation->m_presentCommandBufferObject waitUntilCompleted];
		Require(binding.GetTransportBackend().BeginFrame(viewport, 1, 1).IsOk(), "next CPU source must prepare");
		Require(presenter.CaptureFrameEvidence(viewport.m_viewportId).m_nativeCode == 2126 && state->m_evidenceCaptureCount == 4,
			"prepared CPU writes must not be sampled as the previous exported frame");
		FramePacket unpresented;
		Require(binding.GetTransportBackend().ExportFrame(viewport, 1, 1, unpresented).IsOk(), "prepared CPU frame must export");
		Require(presenter.CaptureFrameEvidence(viewport.m_viewportId).m_nativeCode == 2126 && state->m_evidenceCaptureCount == 4,
			"new producer contents must not be sampled under an older presented frame index");
		Require(presenter.PresentFrame(viewport.m_viewportId, unpresented).IsOk() && presenter.CaptureFrameEvidence(viewport.m_viewportId).IsOk(),
			"presenting the completed frame must make capture available again");
		Require(state->m_evidenceFrameIndex == 3 && state->m_lastFrameEvidence.m_center.m_r == 3, "capture must use the newly presented frame");
		const auto resized = binding.Resize(80, 56);
		Require(resized.IsOk(), "capture test must allow resize: " + resized.m_message);
		state = presenter.FindImportedState(viewport.m_viewportId);
		Require(!state->m_hasFrameEvidence && state->m_evidenceCaptureCount == 0 &&
			presenter.CaptureFrameEvidence(viewport.m_viewportId).m_nativeCode == 2125, "new generation must not inherit stale evidence");
		Require(binding.PumpFrame().IsOk() && presenter.CaptureFrameEvidence(viewport.m_viewportId).IsOk() &&
			state->m_generation == 2 && state->m_evidenceFrameIndex == 1 && state->m_lastFrameEvidence.m_width == 80,
			"resize recovery must capture its own generation, dimensions and frame");
	}

	void TestFrameEvidenceChannelOrderAndFormat()
	{
		NativeSurface surface(4, 4);
		Require(IOSurfaceLock(surface.m_surface, 0, nullptr) == KERN_SUCCESS, "pixel fixture must lock for writing");
		auto pixels = static_cast<uint8_t*>(IOSurfaceGetBaseAddress(surface.m_surface));
		for (uint32_t i = 0; i < 16; ++i)
		{
			pixels[i * 4] = 0x11;
			pixels[i * 4 + 1] = 0x22;
			pixels[i * 4 + 2] = 0x33;
			pixels[i * 4 + 3] = 0xff;
		}
		IOSurfaceUnlock(surface.m_surface, 0, nullptr);
		MacNativeSurfaceFrameEvidence evidence;
		const auto& handle = surface.m_transport.m_macSurfaces.front();
		Require(CaptureMacIOSurfaceFrameEvidence(handle, 4, 4, PixelFormat::B8G8R8A8_UNorm, evidence).IsOk() &&
			evidence.m_center.m_r == 0x33 && evidence.m_center.m_b == 0x11 && evidence.m_nonBlackPixelCount == 64,
			"BGRA capture must preserve real channel values");
		const auto bgraChecksum = evidence.m_checksum;
		Require(CaptureMacIOSurfaceFrameEvidence(handle, 4, 4, PixelFormat::R8G8B8A8_UNorm, evidence).IsOk() &&
			evidence.m_center.m_r == 0x11 && evidence.m_center.m_b == 0x33 && evidence.m_checksum != bgraChecksum,
			"RGBA capture must account for channel order in samples and checksum");
		Require(CaptureMacIOSurfaceFrameEvidence(handle, 4, 4, PixelFormat::R16G16B16A16_Float, evidence).m_nativeCode == 2127 &&
			!evidence.m_hasReadablePixels && evidence.m_sampledPixelCount == 0, "unsupported format must not fabricate byte-color evidence");
	}

	void TestLoopbackDefersWritesUntilPresentationCompletes()
	{
		CpuSource source;
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
			uintptr_t blockedSource = 0;
			Require(CreateMacRendererIntermediateTexture(allocation->m_producerDeviceObject, 64, 48, PixelFormat::B8G8R8A8_UNorm, blockedSource).IsOk(), "reuse check needs a source texture");
			[(id)blockedSource autorelease];
			Require(CopyMacRendererIntermediateToProducerTexture(*allocation, blockedSource, info).m_nativeCode == 1034,
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

	void TestPresentFailuresAndCompletedReadRetention(bool immutable)
	{
		CpuSource source;
		if (immutable) source.m_immutableSource = MakeMacReadbackSource(64, 48, 0x66);
		MacLoopbackIOSurfaceProvider provider(&source);
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
		const auto initialCopy = allocation->m_currentCopyToken;
		layer->m_refuseDrawable = false;
		probe->m_refuseCommandBuffer = true;
		Require(!binding.PumpFrame().IsOk() && allocation->m_presentCommandBufferObject == 0, "command refusal must leave the surface reusable");
		probe->m_refuseCommandBuffer = false;
		probe->m_failCompletion = true;
		Require(binding.PumpFrame().IsOk(), "native submission may be accepted before its terminal result");
		id<MTLTexture> firstImport = [[(id<MTLTexture>)native->m_lastSourceTextureObject retain] autorelease];
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
				Require(native->m_lastSourceTextureObject == reinterpret_cast<uintptr_t>(firstImport),
					"presentation retry and new pixels must not recreate the IOSurface texture import");
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
		if (immutable)
		{
			Require(allocation->m_currentCopyToken == initialCopy && allocation->m_lastProducerCopyToken == initialCopy,
				"drawable, submit and completion retries must re-present an unchanged record without uploading it again");
			Require(presenter.CaptureFrameEvidence(viewport.m_viewportId).IsOk() &&
				presenter.FindImportedState(viewport.m_viewportId)->m_lastFrameEvidence.m_center.m_r == 0x66,
				"recovered native presentation must still expose the retained readback's actual pixels");
		}
	}

	void TestDelayedProducerCopyPublicationAndRetirement()
	{
		class MetalSource : public IMacRendererFrameSourceProvider
		{
		public:
			id<MTLSharedEvent> m_gate = nil;
			uint64_t m_waitValue = 9;
			uint32_t m_calls = 0;
			Sailor::TSharedPtr<std::atomic<uint32_t>> m_releases = Sailor::TSharedPtr<std::atomic<uint32_t>>::Make(0u);
			Failure AcquireFrameSource(const MacViewportSurfaceState& state, FrameIndex frame, MacRendererFrameSource& out) override
			{
				++m_calls;
				const auto& allocation = *state.m_nativeAllocation;
				Require(CreateMacRendererIntermediateTexture(allocation.m_producerDeviceObject, state.m_viewport.m_width,
					state.m_viewport.m_height, state.m_viewport.m_pixelFormat, out.m_textureObject).IsOk(), "source must create a native texture");
				const MacNativeBridgeProducerPattern pattern{ state.m_key.m_viewportId, state.m_key.m_epoch, state.m_key.m_generation,
					frame, state.m_viewport.m_width, state.m_viewport.m_height };
				Require(UploadMacRendererPatternToIntermediateTexture(out.m_textureObject, pattern.m_width, pattern.m_height, pattern).IsOk(),
					"source texture must contain its own frame pixels");
				ObserveNativeRelease((id)out.m_textureObject, m_releases);
				out.m_kind = MacRendererFrameSourceKind::RendererOwnedMetalTexture;
				out.m_width = pattern.m_width;
				out.m_height = pattern.m_height;
				out.m_pixelFormat = state.m_viewport.m_pixelFormat;
				out.m_sourceToken = frame;
				out.m_releaseTextureObjectAfterUse = true;
				out.m_crossApiSharedEventObject = reinterpret_cast<uintptr_t>([m_gate retain]);
				out.m_crossApiAcquireValue = m_waitValue;
				out.m_crossApiSyncKind = CrossApiSyncKind::MetalSharedEvent;
				return Failure::Ok();
			}
		} source;
		@autoreleasepool
		{
			MacLoopbackIOSurfaceProvider provider(&source);
			MacLoopbackViewportPresenter presenter;
			ViewportDescriptor viewport;
			viewport.m_viewportId = 106;
			viewport.m_width = 64;
			viewport.m_height = 48;
			viewport.m_pixelFormat = PixelFormat::B8G8R8A8_UNorm;
			viewport.m_colorSpace = ColorSpace::Srgb;
			viewport.m_presentMode = PresentMode::Mailbox;
			MacViewportLoopbackBinding binding(viewport, provider, presenter);
			presenter.BindHostHandle(viewport.m_viewportId, LayerHandle([CAMetalLayer layer]));
			Require(binding.Create().IsOk(), "delayed producer test needs a local surface");
			auto allocation = std::as_const(binding.GetTransportBackend()).FindSurface(viewport.m_viewportId, 1, 1)->m_nativeAllocation;
			const std::vector<uint8_t> priorPixels(64 * 48 * 4, 0x44);
			MacNativeBridgeRendererFrameInfo info;
			Require(UploadMacRendererBytesToProducerTexture(*allocation, priorPixels.data(), 64 * 4, info).IsOk(), "destination must have known prior pixels");
			source.m_gate = [[(id<MTLDevice>)allocation->m_producerDeviceObject newSharedEvent] autorelease];
			Require(source.m_gate != nil, "delayed producer test needs a native gate");
			std::promise<void> finished;
			auto finish = finished.get_future();
			std::thread watchdog([&]()
			{
				if (finish.wait_for(std::chrono::seconds(5)) == std::future_status::timeout) source.m_gate.signaledValue = UINT64_MAX;
			});
			try
			{
				Require(binding.PumpFrame().IsOk(), "pending native copy must submit without blocking the pump");
				Require(source.m_gate.signaledValue == 0 && allocation->m_copyCommandBufferObject != 0,
					"copy must remain pending on the real unsignaled source event");
				for (uint32_t i = 0; i < 100; ++i) Require(binding.PumpFrame().IsOk(), "pending copy must defer without failure");
				Require(source.m_calls == 1 && source.m_releases->load() == 0, "one native source must stay alive through the pending copy");
				Require(binding.GetRuntimeSession().GetLastPublishedFrameIndex() == 0 && allocation->m_lastWrittenFrameIndex == 0 &&
					allocation->m_lastProducerCopyToken == 0 && allocation->m_lastRendererSource.m_kind == MacRendererFrameSourceKind::Unknown,
					"an accepted copy must not publish unfinished pixels or provenance");
				Require(presenter.FindImportedState(viewport.m_viewportId)->m_presentedFrameCount == 0,
					"presenter must not see an unfinished producer frame");
				FramePacket early;
				Require(binding.GetTransportBackend().ExportFrame(viewport, 1, 1, early).m_nativeCode == 1036,
					"direct export must report pending completion without discarding preparation");
				Require(UploadMacRendererBytesToProducerTexture(*allocation, priorPixels.data(), 64 * 4, info).m_nativeCode == 1036,
					"CPU upload must not race a submitted copy");
				uintptr_t blockedSource = 0;
				Require(CreateMacRendererIntermediateTexture(allocation->m_producerDeviceObject, 64, 48, PixelFormat::B8G8R8A8_UNorm, blockedSource).IsOk(), "pending-copy check needs a source texture");
				[(id)blockedSource autorelease];
				Require(CopyMacRendererIntermediateToProducerTexture(*allocation, blockedSource, info).m_nativeCode == 1036,
					"a second direct copy must not replace outstanding work");
				MacNativeBridgePresentResult presented;
				const auto native = presenter.FindImportedState(viewport.m_viewportId)->m_layerBinding.GetRawPtr();
				Require(PresentMacNativeLayerFrame(*native, {}, early, presented, allocation.GetRawPtr()).m_nativeCode == 1036,
					"direct presentation must reject an unfinished copy");
				Require(ReadIOSurfaceBGRA8Pixel((IOSurfaceRef)allocation->m_surfaceObject, allocation->m_plane.m_bytesPerRow, 23, 11) == 0x44444444u,
					"pending copy must preserve the prior destination pixels");
				source.m_gate.signaledValue = 9;
				[(id<MTLCommandBuffer>)allocation->m_copyCommandBufferObject waitUntilCompleted];
				Require(binding.PumpFrame().IsOk() && binding.GetRuntimeSession().GetLastPublishedFrameIndex() == 1 && source.m_calls == 1,
					"completed copy must publish its original preparation exactly once");
				const MacNativeBridgeProducerPattern first{ 106, 1, 1, 1, 64, 48 };
				Require(ReadIOSurfaceBGRA8Pixel((IOSurfaceRef)allocation->m_surfaceObject, allocation->m_plane.m_bytesPerRow, 23, 11) ==
					ExpectedProducerPatternBGRA8(first, 23, 11), "completed copy must publish the correct source pixels");
				RequireNativeReleases(source.m_releases, 1, "completed source texture must release exactly once");
				Require(presenter.CaptureFrameEvidence(viewport.m_viewportId).IsOk(), "completed native copy must allow explicit capture");
				[(id<MTLCommandBuffer>)allocation->m_presentCommandBufferObject waitUntilCompleted];
				source.m_waitValue = 10;
				Require(binding.PumpFrame().IsOk() && source.m_calls == 2, "next native copy must submit independently");
				id<MTLCommandBuffer> retiredCopy = [[(id<MTLCommandBuffer>)allocation->m_copyCommandBufferObject retain] autorelease];
				id<MTLTexture> retiredTexture = [[(id<MTLTexture>)allocation->m_producerTextureObject retain] autorelease];
				Require(source.m_gate.signaledValue == 9 && retiredCopy.status != MTLCommandBufferStatusCompleted,
					"old generation's copy must remain pending before resize");
				Require(presenter.CaptureFrameEvidence(viewport.m_viewportId).m_nativeCode == 2126 &&
					presenter.FindImportedState(viewport.m_viewportId)->m_evidenceCaptureCount == 1 && source.m_gate.signaledValue == 9,
					"explicit capture must reject pending GPU writes without sampling or waiting for their event");
				Require(binding.Resize(80, 56).IsOk() && !allocation.IsShared(), "resize must unregister and unimport the old allocation");
				allocation.Clear();
				Require(source.m_releases->load() == 1, "native pending copy must retain its source after owner destruction");
				source.m_waitValue = 9;
				Require(binding.PumpFrame().IsOk(), "new generation must submit without waiting for the old copy");
				auto replacement = std::as_const(binding.GetTransportBackend()).FindSurface(viewport.m_viewportId, 1, 2)->m_nativeAllocation;
				[(id<MTLCommandBuffer>)replacement->m_copyCommandBufferObject waitUntilCompleted];
				if (binding.GetRuntimeSession().GetLastPublishedFrameIndex() == 0) Require(binding.PumpFrame().IsOk(), "new generation must publish after its own copy completes");
				Require(binding.GetRuntimeSession().GetLastPublishedFrameIndex() == 1 && source.m_calls == 3,
					"new generation must publish one independently prepared frame");
				source.m_gate.signaledValue = 10;
				[retiredCopy waitUntilCompleted];
				Require(retiredCopy.status == MTLCommandBufferStatusCompleted, "retired copy must finish after its source signals");
				uint32_t pixel = 0;
				[retiredTexture getBytes:&pixel bytesPerRow:4 fromRegion:MTLRegionMake2D(23, 11, 1, 1) mipmapLevel:0];
				const MacNativeBridgeProducerPattern second{ 106, 1, 1, 2, 64, 48 };
				Require(pixel == ExpectedProducerPatternBGRA8(second, 23, 11), "retired copy must preserve its own frame pixels");
			}
			catch (...)
			{
				source.m_gate.signaledValue = UINT64_MAX;
				finished.set_value();
				watchdog.join();
				throw;
			}
			finished.set_value();
			watchdog.join();
		}
		RequireNativeReleases(source.m_releases, 3, "all submitted native sources must retire after their owners and commands");
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
			MacViewportTestSource source;
			MacLoopbackIOSurfaceProvider provider(&source);
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
				[(id<MTLCommandBuffer>)allocation->m_copyCommandBufferObject waitUntilCompleted];
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
			const auto completedCopyToken = allocation->m_lastProducerCopyToken;
			Require(backend.BeginFrame(viewport, 1, 1).IsOk(), "copy submission must precede its terminal status");
			[(id<MTLCommandBuffer>)allocation->m_copyCommandBufferObject waitUntilCompleted];
			FramePacket failedFrame;
			Require(backend.ExportFrame(viewport, 1, 1, failedFrame).m_nativeCode == 1033, "failed native terminal status must propagate before export");
			Require(allocation->m_lastWrittenFrameIndex == 100 && !state->m_frameBegun, "failed native work must not publish success metadata");
			Require(allocation->m_lastProducerCopyToken == completedCopyToken && state->m_pendingRendererSource.m_kind == MacRendererFrameSourceKind::Unknown,
				"failed copy must discard preparation without changing the last completed provenance");
			FramePacket rejected;
			Require(!backend.ExportFrame(viewport, 1, 1, rejected).IsOk(), "failed copy must not export a frame");
			probe->m_failCompletion = false;
			Require(backend.BeginFrame(viewport, 1, 1).IsOk(), "retry must reuse the existing native queue");
			[(id<MTLCommandBuffer>)allocation->m_copyCommandBufferObject waitUntilCompleted];
			Require(backend.ExportFrame(viewport, 1, 1, rejected).IsOk() && rejected.m_frameIndex == 101, "retry must not skip failed frame indices");
			id<MTLSharedEvent> event = [[(id<MTLDevice>)allocation->m_producerDeviceObject newSharedEvent] autorelease];
			MacNativeBridgeRendererFrameInfo info;
			info.m_producerCopyToken = 99;
			uintptr_t invalidSyncSource = 0;
			Require(CreateMacRendererIntermediateTexture(allocation->m_producerDeviceObject, 64, 48, PixelFormat::B8G8R8A8_UNorm, invalidSyncSource).IsOk(), "sync check needs a source texture");
			[(id)invalidSyncSource autorelease];
			Require(!CopyMacRendererIntermediateToProducerTexture(*allocation, invalidSyncSource, info,
				reinterpret_cast<uintptr_t>(event), 0).IsOk(), "invalid native wait must fail without submission");
			Require(info.m_producerCopyToken == 0 && !info.m_usedGpuCopyIntoProducerTexture, "failed copy must clear previous caller provenance");
			Require(probe->m_commandBufferCount == 104, "all success and failure calls must use the same queue");
			Require(backend.ReleaseSurface(viewport.m_viewportId, 1, 1).IsOk(), "copy test must unregister its native allocation");
			Require(releases->load() == 0, "retained allocation must keep the producer queue alive after unregister");
		}
		RequireNativeReleases(releases, 1, "repeated copies and failures must release their one native queue");
	}

	void TestFailedNativeWriteInvalidatesReadbackReuse()
	{
		@autoreleasepool
		{
			CpuSource source;
			source.m_immutableSource = MakeMacReadbackSource(64, 48, 0x66);
			MacLoopbackIOSurfaceProvider provider(&source);
			MacViewportSurfaceState state;
			ViewportDescriptor viewport;
			viewport.m_viewportId = 113;
			viewport.m_width = 64;
			viewport.m_height = 48;
			viewport.m_pixelFormat = PixelFormat::B8G8R8A8_UNorm;
			FramePacket frame;
			Require(provider.CreateOrResizeSurface(viewport, 1, 1, state).IsOk() &&
				provider.BeginFrame(state).IsOk() && provider.ExportFrame(state, frame).IsOk(), "initial immutable frame must upload and export");
			auto allocation = state.m_nativeAllocation;
			const auto firstCopy = allocation->m_lastProducerCopyToken;
			id<MTLCommandQueue> queue = (id<MTLCommandQueue>)allocation->m_producerCommandQueueObject;
			ProducerQueueProbe* probe = [[ProducerQueueProbe alloc] initWithQueue:queue];
			[queue release];
			allocation->m_producerCommandQueueObject = reinterpret_cast<uintptr_t>(probe);
			uintptr_t texture = 0;
			Require(CreateMacRendererIntermediateTexture(allocation->m_producerDeviceObject, 64, 48, PixelFormat::B8G8R8A8_UNorm, texture).IsOk(),
				"native overwrite needs an actual source texture");
			[(id)texture autorelease];
			const MacNativeBridgeProducerPattern pattern{ viewport.m_viewportId, 1, 1, 99, 64, 48 };
			Require(UploadMacRendererPatternToIntermediateTexture(texture, 64, 48, pattern).IsOk(), "native overwrite source must contain known pixels");
			probe->m_failCompletion = true;
			MacNativeBridgeRendererFrameInfo write;
			Require(CopyMacRendererIntermediateToProducerTexture(*allocation, texture, write).IsOk(), "native overwrite must submit before its status is known");
			[(id<MTLCommandBuffer>)allocation->m_copyCommandBufferObject waitUntilCompleted];
			bool completed = false;
			Require(PollMacIOSurfaceCopyCompletion(*allocation, completed).m_nativeCode == 1033 &&
				allocation->m_currentCopyToken != firstCopy && allocation->m_lastProducerCopyToken == firstCopy,
				"failed write must retain last-export metadata but invalidate physical-content identity");
			Require(ReadIOSurfaceBGRA8Pixel((IOSurfaceRef)allocation->m_surfaceObject, allocation->m_plane.m_bytesPerRow, 23, 11) ==
				ExpectedProducerPatternBGRA8(pattern, 23, 11), "reported copy failure may leave newer pixels in the surface");
			probe->m_failCompletion = false;
			Require(provider.BeginFrame(state).IsOk() && provider.ExportFrame(state, frame).IsOk() &&
				allocation->m_currentCopyToken != write.m_producerCopyToken &&
				ReadIOSurfaceBGRA8Pixel((IOSurfaceRef)allocation->m_surfaceObject, allocation->m_plane.m_bytesPerRow, 23, 11) == 0x66666666u,
				"retrying the old immutable record must restore its pixels after a failed native overwrite");
			Require(provider.ReleaseSurface(state).IsOk(), "failed overwrite fixture must release its surface");
		}
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
		NativeSurface surface(64, 48);
		Require(PresentMacNativeLayerFrame(*binding, surface.m_transport.m_macSurfaces.front(), frame, present).IsOk(), "original layer must still present after queue creation failure");
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
			Require(state->m_layerBinding->m_importedIOSurfaceObject == surface.m_transport.m_macSurfaces.front().m_surfaceObject,
				"recovered presentation must read the real imported IOSurface");
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
			NativeSurface m_surface{ 64, 64 };
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
					state->m_surface.m_transport.m_macSurfaces.front(),
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
		{ "NativeSourceTextureReuse", TestNativeSourceTextureReuse },
		{ "NativeSourceImportReplacement", TestNativeSourceImportReplacement },
		{ "PresentationDoesNotCapturePixelsAutomatically", TestPresentationDoesNotCapturePixelsAutomatically },
		{ "RequestedFrameEvidencePreservesPresentation", TestRequestedFrameEvidencePreservesPresentation },
		{ "FrameEvidenceChannelOrderAndFormat", TestFrameEvidenceChannelOrderAndFormat },
		{ "MissingRendererFramesPreserveLastPresentation", TestMissingRendererFramesPreserveLastPresentation },
		{ "InvalidIOSurfaceDoesNotPresentSyntheticPixels", TestInvalidIOSurfaceDoesNotPresentSyntheticPixels },
		{ "DelayedProducerCopyPublicationAndRetirement", TestDelayedProducerCopyPublicationAndRetirement },
		{ "ProducerCopyReturnsBeforeSourceCompletion", TestProducerCopyReturnsBeforeSourceCompletion },
		{ "LoopbackDefersWritesUntilPresentationCompletes", TestLoopbackDefersWritesUntilPresentationCompletes },
		{ "PresentFailuresAndCompletedReadRetention", []() { TestPresentFailuresAndCompletedReadRetention(false); } },
		{ "ImmutableReadbackPresentationRetry", []() { TestPresentFailuresAndCompletedReadRetention(true); } },
		{ "FailedNativeWriteInvalidatesReadbackReuse", TestFailedNativeWriteInvalidatesReadbackReuse },
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
		{ "UnfinishedRendererSubmissionDoesNotExportMetalTexture", TestUnfinishedRendererSubmissionDoesNotExportMetalTexture },
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
