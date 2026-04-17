#if defined(__APPLE__)

#import <QuartzCore/CAMetalLayer.h>
#import <Metal/Metal.h>
#import <IOSurface/IOSurface.h>

#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>

#include "Submodules/EditorRemote/RemoteViewportMacNativeBridge.h"

using namespace Sailor::EditorRemote;

namespace
{

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
		MacNativeLayerBinding binding{};
		auto bindResult = BindMacNativeLayer(hostHandle, 640, 360, PixelFormat::B8G8R8A8_UNorm, binding);
		Require(bindResult.IsOk(), "binding an existing CAMetalLayer should succeed");
		Require(binding.IsValid(), "binding should materialize a valid CAMetalLayer state");
		Require(binding.m_layerObject == hostHandle.m_value, "bridge should retain the exact CAMetalLayer pointer");
		Require(binding.m_deviceObject != 0, "binding should materialize a real Metal device");
		Require(binding.m_commandQueueObject != 0, "binding should materialize a real Metal command queue");

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

		auto present = PresentMacNativeLayerFrame(binding, surfaceHandle, frame, presentResult);
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
		Require(binding.m_importedIOSurfaceObject == surfaceHandle.m_surfaceObject, "binding should retain the imported IOSurface object used for the Metal texture import");
		Require(binding.m_lastSourceTextureObject == presentResult.m_sourceTextureObject, "binding should retain the last source Metal texture");
	}
}

int main()
{
	const std::pair<const char*, std::function<void()>> tests[] = {
		{ "GetMacRendererSourceSelectionPriorityPrefersSceneViewResolvedOutputs", TestGetMacRendererSourceSelectionPriorityPrefersSceneViewResolvedOutputs },
		{ "SelectMacVulkanSemaphoreForMetalExportPrefersDedicatedMainResolvedSeam", TestSelectMacVulkanSemaphoreForMetalExportPrefersDedicatedMainResolvedSeam },
		{ "SynchronizeMacVulkanRenderTargetPrefersMetalSharedEventWhenSemaphoreExportSeamExists", TestSynchronizeMacVulkanRenderTargetPrefersMetalSharedEventWhenSemaphoreExportSeamExists },
		{ "BridgeWaitsOnMetalSharedEventBeforeProducerCopy", TestBridgeWaitsOnMetalSharedEventBeforeProducerCopy },
		{ "BridgeBindsExistingCAMetalLayerAndPresentsDrawable", TestBridgeBindsExistingCAMetalLayerAndPresentsDrawable },
	};

	for (const auto& test : tests)
	{
		try
		{
			test.second();
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
