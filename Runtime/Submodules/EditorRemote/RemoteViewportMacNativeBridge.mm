#include "RemoteViewportMacNativeBridge.h"
#include "RemoteViewportMacTransport.h"

#if defined(__APPLE__)
#import <AppKit/AppKit.h>
#import <QuartzCore/CAMetalLayer.h>
#import <CoreGraphics/CoreGraphics.h>
#import <Metal/Metal.h>
#import <IOSurface/IOSurface.h>
#import <dispatch/dispatch.h>
#define VK_USE_PLATFORM_METAL_EXT 1
#include "../../../External/renderdoc/renderdoc/driver/vulkan/official/vulkan.h"
#include <algorithm>
#include <dlfcn.h>
#endif

namespace Sailor::EditorRemote
{
#if defined(__APPLE__)
	namespace
	{
		MTLPixelFormat ToMetalPixelFormat(const PixelFormat pixelFormat)
		{
			switch (pixelFormat)
			{
			case PixelFormat::B8G8R8A8_UNorm:
				return MTLPixelFormatBGRA8Unorm;
			default:
				return MTLPixelFormatInvalid;
			}
		}

		Failure MakeFailure(const uint32_t code, const char* message)
		{
			return Failure::FromDomain(ErrorDomain::Capability, code, message);
		}

		Failure CreateProducerFailure(const uint32_t code, const char* message)
		{
			return Failure::FromDomain(ErrorDomain::Transport, code, message);
		}

		Failure WriteProducerFailure(const uint32_t code, const char* message)
		{
			return Failure::FromDomain(ErrorDomain::Session, code, message);
		}

		uint64_t NextRendererTextureToken()
		{
			static uint64_t s_token = 0;
			return ++s_token;
		}

		uint64_t NextProducerCopyToken()
		{
			static uint64_t s_token = 0;
			return ++s_token;
		}

		uint64_t NextCrossApiAcquireValue()
		{
			static uint64_t s_value = 0;
			return ++s_value;
		}

		uintptr_t RetainObjectiveCObject(id object)
		{
			if (object == nil)
			{
				return 0;
			}

			CFRetain((__bridge CFTypeRef)object);
			return reinterpret_cast<uintptr_t>((__bridge void*)object);
		}

		void ReleaseObjectiveCObject(uintptr_t& inOutObject)
		{
			if (inOutObject != 0)
			{
				CFRelease(reinterpret_cast<CFTypeRef>(inOutObject));
				inOutObject = 0;
			}
		}

		template <typename TFn>
		auto RunOnMainThreadSync(TFn&& fn) -> decltype(fn())
		{
			using TResult = decltype(fn());
			if ([NSThread isMainThread])
			{
				return fn();
			}

			__block TResult result{};
			dispatch_sync(dispatch_get_main_queue(), ^
			{
				result = fn();
			});
			return result;
		}

		bool& MacVulkanMetalInteropTestMode()
		{
			static bool s_enabled = false;
			return s_enabled;
		}

		id<MTLTexture> CreateIOSurfaceBackedSourceTexture(id<MTLDevice> device, const MacIOSurfaceHandle& surfaceHandle, const MacNativeLayerBinding& binding)
		{
			if (device == nil || surfaceHandle.m_surfaceObject == 0 || binding.m_width == 0 || binding.m_height == 0)
			{
				return nil;
			}

			IOSurfaceRef surface = reinterpret_cast<IOSurfaceRef>(surfaceHandle.m_surfaceObject);
			MTLTextureDescriptor* descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:ToMetalPixelFormat(binding.m_pixelFormat)
				width:binding.m_width
				height:binding.m_height
				mipmapped:NO];
			descriptor.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite | MTLTextureUsageRenderTarget;
			descriptor.storageMode = MTLStorageModeShared;
			return [device newTextureWithDescriptor:descriptor iosurface:surface plane:surfaceHandle.m_planeIndex];
		}
	}


	uint32_t GetMacIOSurfaceBytesPerRowAlignment(PixelFormat pixelFormat)
	{
		const auto metalPixelFormat = ToMetalPixelFormat(pixelFormat);
		if (metalPixelFormat == MTLPixelFormatInvalid)
		{
			return 64u;
		}

		id<MTLDevice> device = MTLCreateSystemDefaultDevice();
		if (device == nil)
		{
			return 64u;
		}

		const NSUInteger alignment = [device minimumLinearTextureAlignmentForPixelFormat:metalPixelFormat];
		[device release];
		return alignment > 0 ? static_cast<uint32_t>(alignment) : 64u;
	}

	Failure CreateMacIOSurfaceProducerTexture(MacIOSurfaceAllocation& allocation)
	{
		@autoreleasepool
		{
			if (allocation.m_surfaceObject == 0 || allocation.m_plane.m_width == 0 || allocation.m_plane.m_height == 0)
			{
				return CreateProducerFailure(1004, "macOS producer texture creation requires a valid IOSurface allocation");
			}

			const auto metalPixelFormat = ToMetalPixelFormat(allocation.m_pixelFormat);
			if (metalPixelFormat == MTLPixelFormatInvalid)
			{
				return CreateProducerFailure(1006, "macOS producer texture creation only supports BGRA8 transport");
			}

			id<MTLDevice> device = MTLCreateSystemDefaultDevice();
			if (device == nil)
			{
				return CreateProducerFailure(1005, "macOS producer texture creation could not create a Metal device");
			}

			allocation.m_producerDeviceObject = reinterpret_cast<uintptr_t>((__bridge void*)device);

			MTLTextureDescriptor* descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:metalPixelFormat
				width:allocation.m_plane.m_width
				height:allocation.m_plane.m_height
				mipmapped:NO];
			descriptor.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite | MTLTextureUsageRenderTarget;
			descriptor.storageMode = MTLStorageModeShared;
			id<MTLTexture> texture = [device newTextureWithDescriptor:descriptor iosurface:reinterpret_cast<IOSurfaceRef>(allocation.m_surfaceObject) plane:allocation.m_plane.m_planeIndex];
			if (texture == nil)
			{
				return CreateProducerFailure(1007, "macOS producer texture creation could not create an IOSurface-backed Metal texture");
			}

			allocation.m_producerTextureObject = reinterpret_cast<uintptr_t>((__bridge void*)texture);
			id<MTLCommandQueue> queue = [device newCommandQueue];
			if (queue == nil)
			{
				return CreateProducerFailure(1032, "macOS producer could not create its Metal command queue");
			}
			allocation.m_producerCommandQueueObject = reinterpret_cast<uintptr_t>((__bridge void*)queue);
			return Failure::Ok();
		}
	}

	Failure CreateMacRendererIntermediateTexture(uintptr_t deviceObject, uint32_t width, uint32_t height, PixelFormat pixelFormat, uintptr_t& outTextureObject)
	{
		outTextureObject = 0;
		if (deviceObject == 0 || width == 0 || height == 0)
		{
			return WriteProducerFailure(1008, "macOS renderer intermediate texture creation requires a valid Metal device and extents");
		}

		id<MTLDevice> device = (__bridge id<MTLDevice>)reinterpret_cast<void*>(deviceObject);
		if (device == nil)
		{
			return WriteProducerFailure(1009, "macOS renderer intermediate texture creation resolved the Metal device to nil");
		}

		const auto metalPixelFormat = ToMetalPixelFormat(pixelFormat);
		if (metalPixelFormat == MTLPixelFormatInvalid)
		{
			return WriteProducerFailure(1010, "macOS renderer intermediate texture creation only supports BGRA8 transport in this slice");
		}

		MTLTextureDescriptor* descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:metalPixelFormat width:width height:height mipmapped:NO];
		descriptor.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite | MTLTextureUsageRenderTarget;
		descriptor.storageMode = MTLStorageModeShared;
		id<MTLTexture> texture = [device newTextureWithDescriptor:descriptor];
		if (texture == nil)
		{
			return WriteProducerFailure(1011, "macOS renderer intermediate texture creation could not create a Metal texture");
		}

		outTextureObject = reinterpret_cast<uintptr_t>((__bridge void*)texture);
		return Failure::Ok();
	}

	static Failure PollMacCommandCompletion(uintptr_t& commandObject, bool& outCompleted, uint32_t errorCode, const char* errorMessage)
	{
		outCompleted = commandObject == 0;
		if (outCompleted)
		{
			return Failure::Ok();
		}

		id<MTLCommandBuffer> command = (__bridge id<MTLCommandBuffer>)reinterpret_cast<void*>(commandObject);
		const auto status = command.status;
		if (status == MTLCommandBufferStatusError)
		{
			const char* description = command.error.localizedDescription.UTF8String;
			auto failure = WriteProducerFailure(errorCode, description ? description : errorMessage);
			ReleaseObjectiveCObject(commandObject);
			return failure;
		}
		if (status == MTLCommandBufferStatusCompleted)
		{
			ReleaseObjectiveCObject(commandObject);
			outCompleted = true;
		}
		return Failure::Ok();
	}

	Failure PollMacIOSurfaceReadCompletion(MacIOSurfaceAllocation& allocation, bool& outCompleted)
	{
		return PollMacCommandCompletion(allocation.m_presentCommandBufferObject, outCompleted, 1035, "macOS IOSurface presentation failed");
	}

	Failure PollMacIOSurfaceCopyCompletion(MacIOSurfaceAllocation& allocation, bool& outCompleted)
	{
		return PollMacCommandCompletion(allocation.m_copyCommandBufferObject, outCompleted, 1033, "macOS producer GPU copy failed");
	}

	Failure UploadMacRendererBytesToProducerTexture(MacIOSurfaceAllocation& allocation, const void* bytes, uint32_t bytesPerRow, MacNativeBridgeRendererFrameInfo& outFrameInfo)
	{
		outFrameInfo = {};
		if (allocation.m_copyCommandBufferObject != 0) return WriteProducerFailure(1036, "macOS IOSurface copy completion has not been consumed");
		bool readCompleted = false;
		auto result = PollMacIOSurfaceReadCompletion(allocation, readCompleted);
		if (!result.IsOk()) return result;
		if (!readCompleted) return WriteProducerFailure(1034, "macOS IOSurface is still being read by the presenter");

		const auto destinationTextureObject = allocation.m_producerTextureObject;
		const auto width = allocation.m_plane.m_width;
		const auto height = allocation.m_plane.m_height;
		if (destinationTextureObject == 0 || width == 0 || height == 0 || bytes == nullptr || bytesPerRow == 0)
		{
			return WriteProducerFailure(1020, "macOS producer CPU upload requires a valid Metal texture and source bytes");
		}

		id<MTLTexture> destinationTexture = (__bridge id<MTLTexture>)reinterpret_cast<void*>(destinationTextureObject);
		if (destinationTexture == nil)
		{
			return WriteProducerFailure(1021, "macOS producer CPU upload resolved the destination Metal texture to nil");
		}

		[destinationTexture replaceRegion:MTLRegionMake2D(0, 0, width, height) mipmapLevel:0 withBytes:bytes bytesPerRow:bytesPerRow];
		outFrameInfo.m_rendererTextureToken = NextRendererTextureToken();
		outFrameInfo.m_producerCopyToken = NextProducerCopyToken();
		outFrameInfo.m_usedRendererIntermediateTexture = false;
		outFrameInfo.m_usedGpuCopyIntoProducerTexture = false;
		outFrameInfo.m_usedCpuUploadIntoProducerTexture = true;
		return Failure::Ok();
	}

	Failure CopyMacRendererIntermediateToProducerTexture(MacIOSurfaceAllocation& allocation, uintptr_t sourceTextureObject, MacNativeBridgeRendererFrameInfo& outFrameInfo, uintptr_t sharedEventObject, uint64_t sharedEventValue)
	{
		outFrameInfo = {};
		if (allocation.m_copyCommandBufferObject != 0) return WriteProducerFailure(1036, "macOS IOSurface copy completion has not been consumed");
		@autoreleasepool
		{
			bool readCompleted = false;
			auto result = PollMacIOSurfaceReadCompletion(allocation, readCompleted);
			if (!result.IsOk()) return result;
			if (!readCompleted) return WriteProducerFailure(1034, "macOS IOSurface is still being read by the presenter");

			const auto width = allocation.m_plane.m_width;
			const auto height = allocation.m_plane.m_height;
			if (allocation.m_producerCommandQueueObject == 0 || sourceTextureObject == 0 || allocation.m_producerTextureObject == 0 || width == 0 || height == 0)
			{
				return WriteProducerFailure(1015, "macOS producer GPU copy requires valid Metal objects and extents");
			}

			id<MTLTexture> sourceTexture = (__bridge id<MTLTexture>)reinterpret_cast<void*>(sourceTextureObject);
			id<MTLTexture> destinationTexture = (__bridge id<MTLTexture>)reinterpret_cast<void*>(allocation.m_producerTextureObject);
			if (sourceTexture == nil || destinationTexture == nil)
			{
				return WriteProducerFailure(1016, "macOS producer GPU copy resolved a Metal object to nil");
			}

			id<MTLCommandQueue> commandQueue = (__bridge id<MTLCommandQueue>)reinterpret_cast<void*>(allocation.m_producerCommandQueueObject);

			id<MTLCommandBuffer> commandBuffer = [commandQueue commandBuffer];
			if (commandBuffer == nil)
			{
				return WriteProducerFailure(1018, "macOS producer GPU copy could not create a Metal command buffer");
			}

			if (sharedEventObject != 0)
			{
				id<MTLSharedEvent> sharedEvent = (__bridge id<MTLSharedEvent>)reinterpret_cast<void*>(sharedEventObject);
				if (sharedEvent == nil)
				{
					return WriteProducerFailure(1029, "macOS producer GPU copy resolved the exported Metal shared event to nil");
				}
				if (sharedEventValue == 0)
				{
					return WriteProducerFailure(1030, "macOS producer GPU copy requires a non-zero Metal shared-event wait value");
				}
				if (![commandBuffer respondsToSelector:@selector(encodeWaitForEvent:value:)])
				{
					return WriteProducerFailure(1031, "macOS producer GPU copy cannot encode a Metal shared-event wait on this runtime");
				}
				[commandBuffer encodeWaitForEvent:sharedEvent value:sharedEventValue];
			}

			id<MTLBlitCommandEncoder> blitEncoder = [commandBuffer blitCommandEncoder];
			if (blitEncoder == nil)
			{
				return WriteProducerFailure(1019, "macOS producer GPU copy could not create a Metal blit encoder");
			}

			[blitEncoder copyFromTexture:sourceTexture sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(width, height, 1) toTexture:destinationTexture destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, 0)];
			[blitEncoder endEncoding];
			allocation.m_copyCommandBufferObject = RetainObjectiveCObject(commandBuffer);
			[commandBuffer commit];

			outFrameInfo.m_rendererTextureToken = NextRendererTextureToken();
			outFrameInfo.m_producerCopyToken = NextProducerCopyToken();
			outFrameInfo.m_crossApiWaitValue = sharedEventValue;
			outFrameInfo.m_usedRendererIntermediateTexture = true;
			outFrameInfo.m_usedGpuCopyIntoProducerTexture = true;
			outFrameInfo.m_usedCpuUploadIntoProducerTexture = false;
			outFrameInfo.m_waitedOnCrossApiSharedEvent = sharedEventObject != 0 && sharedEventValue != 0;
			return Failure::Ok();
		}
	}

	Failure SynchronizeMacVulkanRenderTargetForMetalExport(uintptr_t vulkanDeviceHandle, uintptr_t vulkanSemaphoreHandle, uintptr_t& outSharedEventObject, uint64_t& outAcquireValue, CrossApiSyncKind& outSyncKind, bool& outCpuWaited)
	{
		outSharedEventObject = 0;
		outAcquireValue = 0;
		outSyncKind = CrossApiSyncKind::None;
		outCpuWaited = false;
		if (vulkanDeviceHandle == 0)
		{
			return WriteProducerFailure(1026, "macOS Vulkan->Metal sync requires a live Vulkan device");
		}

		if (MacVulkanMetalInteropTestMode() && vulkanSemaphoreHandle != 0)
		{
			outSharedEventObject = 0x1;
			outAcquireValue = 1;
			outSyncKind = CrossApiSyncKind::MetalSharedEvent;
			outCpuWaited = false;
			return Failure::Ok();
		}

		const auto device = static_cast<VkDevice>(reinterpret_cast<VkDevice_T*>(vulkanDeviceHandle));
		auto getDeviceProcAddr = reinterpret_cast<PFN_vkGetDeviceProcAddr>(dlsym(RTLD_DEFAULT, "vkGetDeviceProcAddr"));
		auto waitIdle = reinterpret_cast<PFN_vkDeviceWaitIdle>(dlsym(RTLD_DEFAULT, "vkDeviceWaitIdle"));
		constexpr bool kEnableBinarySemaphoreSharedEventInterop = false;
		if (kEnableBinarySemaphoreSharedEventInterop && getDeviceProcAddr != nullptr && vulkanSemaphoreHandle != 0)
		{
			auto exportMetalObjects = reinterpret_cast<PFN_vkExportMetalObjectsEXT>(getDeviceProcAddr(device, "vkExportMetalObjectsEXT"));
			if (exportMetalObjects != nullptr)
			{
				VkExportMetalSharedEventInfoEXT sharedEventInfo{ VK_STRUCTURE_TYPE_EXPORT_METAL_SHARED_EVENT_INFO_EXT };
				sharedEventInfo.semaphore = static_cast<VkSemaphore>(reinterpret_cast<VkSemaphore_T*>(vulkanSemaphoreHandle));

				VkExportMetalObjectsInfoEXT exportInfo{ VK_STRUCTURE_TYPE_EXPORT_METAL_OBJECTS_INFO_EXT };
				exportInfo.pNext = &sharedEventInfo;
				exportMetalObjects(device, &exportInfo);
				if (sharedEventInfo.mtlSharedEvent != nil)
				{
					CFRetain((__bridge CFTypeRef)sharedEventInfo.mtlSharedEvent);
					outSharedEventObject = reinterpret_cast<uintptr_t>((__bridge void*)sharedEventInfo.mtlSharedEvent);
					outAcquireValue = 1;
					outSyncKind = CrossApiSyncKind::MetalSharedEvent;
					outCpuWaited = false;
					return Failure::Ok();
				}
			}
		}

		if (waitIdle == nullptr)
		{
			return Failure::FromDomain(ErrorDomain::Capability, 1027, "macOS Vulkan->Metal sync requires vkDeviceWaitIdle from the live Vulkan loader");
		}

		const VkResult result = waitIdle(device);
		if (result != VK_SUCCESS)
		{
			return Failure::FromDomain(ErrorDomain::Session, 1028, "macOS Vulkan->Metal sync failed to idle the Vulkan device before Metal consumption");
		}

		outAcquireValue = NextCrossApiAcquireValue();
		outSyncKind = CrossApiSyncKind::CpuDeviceIdle;
		outCpuWaited = true;
		return Failure::Ok();
	}

	void SetMacVulkanMetalInteropTestMode(bool enabled)
	{
		MacVulkanMetalInteropTestMode() = enabled;
	}

	Failure ExportMacMetalTextureFromVulkanRenderTarget(uintptr_t vulkanDeviceHandle, uintptr_t vulkanImageHandle, uintptr_t vulkanImageViewHandle, PixelFormat pixelFormat, uintptr_t& outTextureObject)
	{
		outTextureObject = 0;
		if (vulkanDeviceHandle == 0 || vulkanImageHandle == 0 || pixelFormat != PixelFormat::B8G8R8A8_UNorm)
		{
			return WriteProducerFailure(1022, "macOS Vulkan->Metal export requires a BGRA8 Vulkan render target");
		}

		const auto device = static_cast<VkDevice>(reinterpret_cast<VkDevice_T*>(vulkanDeviceHandle));
		const auto image = static_cast<VkImage>(reinterpret_cast<VkImage_T*>(vulkanImageHandle));
		const auto imageView = static_cast<VkImageView>(reinterpret_cast<VkImageView_T*>(vulkanImageViewHandle));
		auto getDeviceProcAddr = reinterpret_cast<PFN_vkGetDeviceProcAddr>(dlsym(RTLD_DEFAULT, "vkGetDeviceProcAddr"));
		if (getDeviceProcAddr == nullptr)
		{
			return Failure::FromDomain(ErrorDomain::Capability, 1024, "macOS Vulkan->Metal export requires a live Vulkan loader with vkGetDeviceProcAddr");
		}

		auto exportMetalObjects = reinterpret_cast<PFN_vkExportMetalObjectsEXT>(getDeviceProcAddr(device, "vkExportMetalObjectsEXT"));
		if (exportMetalObjects == nullptr)
		{
			return Failure::FromDomain(ErrorDomain::Capability, 1024, "macOS Vulkan->Metal export requires VK_EXT_metal_objects / MoltenVK metal export support");
		}

		VkExportMetalTextureInfoEXT textureInfo{ VK_STRUCTURE_TYPE_EXPORT_METAL_TEXTURE_INFO_EXT };
		textureInfo.image = image;
		textureInfo.imageView = imageView;
		textureInfo.plane = VK_IMAGE_ASPECT_COLOR_BIT;

		VkExportMetalObjectsInfoEXT exportInfo{ VK_STRUCTURE_TYPE_EXPORT_METAL_OBJECTS_INFO_EXT };
		exportInfo.pNext = &textureInfo;
		exportMetalObjects(device, &exportInfo);
		if (textureInfo.mtlTexture == nil)
		{
			return WriteProducerFailure(1025, "macOS Vulkan->Metal export did not produce a Metal texture for the renderer target");
		}

		CFRetain((__bridge CFTypeRef)textureInfo.mtlTexture);
		outTextureObject = reinterpret_cast<uintptr_t>((__bridge void*)textureInfo.mtlTexture);
		return Failure::Ok();
	}

	MacIOSurfaceAllocation::~MacIOSurfaceAllocation()
	{
		ReleaseObjectiveCObject(m_copyCommandBufferObject);
		ReleaseObjectiveCObject(m_presentCommandBufferObject);
		ReleaseObjectiveCObject(m_producerTextureObject);
		ReleaseObjectiveCObject(m_producerCommandQueueObject);
		ReleaseObjectiveCObject(m_producerDeviceObject);
		if (m_surfaceObject != 0)
		{
			CFRelease(reinterpret_cast<IOSurfaceRef>(m_surfaceObject));
		}
	}

	void ReleaseMacRendererIntermediateTexture(uintptr_t& inOutTextureObject)
	{
		if (inOutTextureObject != 0)
		{
			CFRelease(reinterpret_cast<CFTypeRef>(inOutTextureObject));
			inOutTextureObject = 0;
		}
	}

	void ReleaseMacExportedTexture(uintptr_t& inOutTextureObject)
	{
		if (inOutTextureObject != 0)
		{
			CFRelease(reinterpret_cast<CFTypeRef>(inOutTextureObject));
			inOutTextureObject = 0;
		}
	}

	Failure BindMacNativeLayer(const MacNativeHostHandle& hostHandle, uint32_t width, uint32_t height, PixelFormat pixelFormat, TUniquePtr<MacNativeLayerBinding>& inOutBinding)
	{
		if (!hostHandle.IsValid())
		{
			return MakeFailure(2101, "macOS native layer binding requires a valid host handle");
		}
		if (width == 0 || height == 0)
		{
			return MakeFailure(2108, "macOS native layer binding requires a nonzero extent");
		}
		const auto metalPixelFormat = ToMetalPixelFormat(pixelFormat);
		if (metalPixelFormat == MTLPixelFormatInvalid)
		{
			return MakeFailure(2106, "macOS native layer binding only supports BGRA8 transport");
		}

		auto bindNativeLayer = [&]() -> Failure
		{
			@autoreleasepool
			{
				CAMetalLayer* metalLayer = nil;
				NSView* view = nil;
				bool hostOwnsLayer = false;

				switch (hostHandle.m_kind)
				{
				case MacNativeHostHandleKind::NSView:
					view = (__bridge NSView*)reinterpret_cast<void*>(hostHandle.m_value);
					if (view == nil)
					{
						return MakeFailure(2102, "macOS native host NSView handle resolved to nil");
					}
					if ([view.layer isKindOfClass:[CAMetalLayer class]])
					{
						metalLayer = (CAMetalLayer*)view.layer;
					}
					else
					{
						metalLayer = [CAMetalLayer layer];
						hostOwnsLayer = true;
					}
					break;
				case MacNativeHostHandleKind::CAMetalLayer:
					metalLayer = (__bridge CAMetalLayer*)reinterpret_cast<void*>(hostHandle.m_value);
					if (metalLayer == nil)
					{
						return MakeFailure(2103, "macOS native host CAMetalLayer handle resolved to nil");
					}
					break;
				default:
					return MakeFailure(2104, "macOS native layer binding does not support the supplied host handle kind");
				}

				auto binding = TUniquePtr<MacNativeLayerBinding>::Make();
				binding->m_layerObject = RetainObjectiveCObject(metalLayer);
				id<MTLDevice> device = metalLayer.device;
				if (device != nil)
				{
					binding->m_deviceObject = RetainObjectiveCObject(device);
				}
				else
				{
					device = MTLCreateSystemDefaultDevice();
					if (device == nil)
					{
						return MakeFailure(2105, "macOS native layer binding could not create a Metal device");
					}
					// Create/new already transfers one retain to this binding.
					binding->m_deviceObject = reinterpret_cast<uintptr_t>((__bridge void*)device);
				}

				id<MTLCommandQueue> commandQueue = [device newCommandQueue];
				if (commandQueue == nil)
				{
					return MakeFailure(2107, "macOS native layer binding could not create a Metal command queue");
				}

				binding->m_commandQueueObject = reinterpret_cast<uintptr_t>((__bridge void*)commandQueue);

				metalLayer.device = device;
				metalLayer.pixelFormat = metalPixelFormat;
				CGColorSpaceRef srgbColorSpace = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
				metalLayer.colorspace = srgbColorSpace;
				CGColorSpaceRelease(srgbColorSpace);
				metalLayer.framebufferOnly = NO;
				metalLayer.drawableSize = CGSizeMake(width, height);
				if (view != nil)
				{
					metalLayer.contentsScale = view.window != nil ? view.window.backingScaleFactor : NSScreen.mainScreen.backingScaleFactor;
					if (hostOwnsLayer)
					{
						view.wantsLayer = YES;
						view.layer = metalLayer;
					}
				}

				binding->m_hostObject = hostHandle.m_kind == MacNativeHostHandleKind::NSView ? hostHandle.m_value : 0;
				binding->m_width = width;
				binding->m_height = height;
				binding->m_pixelFormat = pixelFormat;
				binding->m_hostOwnsLayer = hostOwnsLayer;
				binding->m_bindingToken = inOutBinding ? inOutBinding->m_bindingToken + 1 : 1;
				inOutBinding = std::move(binding);
				return Failure::Ok();
			}
		};

		if (hostHandle.m_kind == MacNativeHostHandleKind::NSView)
		{
			return RunOnMainThreadSync(bindNativeLayer);
		}

		// CAMetalLayer hosts are created and attached by the editor UI. Metal
		// presentation already runs on the Engine thread, so binding the
		// supplied layer must not synchronously re-enter the UI main thread.
		return bindNativeLayer();
	}

	Failure CaptureMacIOSurfaceFrameEvidence(const MacIOSurfaceHandle& surfaceHandle, uint32_t width, uint32_t height, MacNativeSurfaceFrameEvidence& outEvidence)
	{
		outEvidence = {};
		outEvidence.m_width = width;
		outEvidence.m_height = height;
		if (!surfaceHandle.IsValid() || surfaceHandle.m_surfaceObject == 0 || width == 0 || height == 0)
		{
			return MakeFailure(2120, "macOS frame evidence capture requires a valid IOSurface handle");
		}

		IOSurfaceRef ioSurface = (__bridge IOSurfaceRef)reinterpret_cast<void*>(surfaceHandle.m_surfaceObject);
		if (ioSurface == nullptr)
		{
			return MakeFailure(2121, "macOS frame evidence capture resolved IOSurface to nil");
		}

		const kern_return_t lockResult = IOSurfaceLock(ioSurface, kIOSurfaceLockReadOnly, nullptr);
		if (lockResult != KERN_SUCCESS)
		{
			return MakeFailure(2122, "macOS frame evidence capture could not lock IOSurface");
		}

		auto unlockSurface = [&]()
		{
			IOSurfaceUnlock(ioSurface, kIOSurfaceLockReadOnly, nullptr);
		};

		const uint8_t* baseAddress = static_cast<const uint8_t*>(IOSurfaceGetBaseAddressOfPlane(ioSurface, surfaceHandle.m_planeIndex));
		if (baseAddress == nullptr)
		{
			baseAddress = static_cast<const uint8_t*>(IOSurfaceGetBaseAddress(ioSurface));
		}
		const size_t bytesPerRow = surfaceHandle.m_bytesPerRow != 0 ? surfaceHandle.m_bytesPerRow : IOSurfaceGetBytesPerRowOfPlane(ioSurface, surfaceHandle.m_planeIndex);
		if (bytesPerRow == 0)
		{
			unlockSurface();
			return MakeFailure(2123, "macOS frame evidence capture could not resolve IOSurface row stride");
		}
		if (baseAddress == nullptr)
		{
			unlockSurface();
			return MakeFailure(2124, "macOS frame evidence capture could not resolve IOSurface base address");
		}

		auto sampleAt = [&](uint32_t x, uint32_t y)
		{
			MacNativeSurfacePixelSample sample{};
			const size_t offset = static_cast<size_t>(y) * bytesPerRow + static_cast<size_t>(x) * 4u;
			const uint8_t* pixel = baseAddress + offset;
			sample.m_b = pixel[0];
			sample.m_g = pixel[1];
			sample.m_r = pixel[2];
			sample.m_a = pixel[3];
			return sample;
		};

		outEvidence.m_topLeft = sampleAt(0, 0);
		outEvidence.m_center = sampleAt(width / 2u, height / 2u);
		outEvidence.m_bottomRight = sampleAt(width > 0 ? width - 1u : 0u, height > 0 ? height - 1u : 0u);
		uint64_t lumaSum = 0;
		uint32_t minLuma = 255;
		uint32_t maxLuma = 0;
		uint32_t checksum = 2166136261u;
		constexpr uint32_t kGridSize = 8;
		for (uint32_t yIndex = 0; yIndex < kGridSize; yIndex++)
		{
			const uint32_t y = height == 1 ? 0 : static_cast<uint32_t>((static_cast<uint64_t>(height - 1u) * yIndex) / (kGridSize - 1u));
			for (uint32_t xIndex = 0; xIndex < kGridSize; xIndex++)
			{
				const uint32_t x = width == 1 ? 0 : static_cast<uint32_t>((static_cast<uint64_t>(width - 1u) * xIndex) / (kGridSize - 1u));
				const auto sample = sampleAt(x, y);
				const uint32_t luma = (static_cast<uint32_t>(sample.m_r) * 54u + static_cast<uint32_t>(sample.m_g) * 183u + static_cast<uint32_t>(sample.m_b) * 19u) >> 8u;
				outEvidence.m_sampledPixelCount++;
				if (sample.m_r != 0 || sample.m_g != 0 || sample.m_b != 0)
				{
					outEvidence.m_nonBlackPixelCount++;
				}
				lumaSum += luma;
				minLuma = std::min(minLuma, luma);
				maxLuma = std::max(maxLuma, luma);
				checksum ^= static_cast<uint32_t>(sample.m_r) | (static_cast<uint32_t>(sample.m_g) << 8u) | (static_cast<uint32_t>(sample.m_b) << 16u) | (static_cast<uint32_t>(sample.m_a) << 24u);
				checksum *= 16777619u;
			}
		}
		outEvidence.m_averageLuma = outEvidence.m_sampledPixelCount != 0 ? static_cast<uint32_t>(lumaSum / outEvidence.m_sampledPixelCount) : 0;
		outEvidence.m_maxLuma = maxLuma;
		outEvidence.m_checksum = checksum;
		outEvidence.m_hasReadablePixels = true;
		outEvidence.m_hasVisualVariance = minLuma != maxLuma || outEvidence.m_topLeft != outEvidence.m_center || outEvidence.m_center != outEvidence.m_bottomRight;
		unlockSurface();
		return Failure::Ok();
	}

	Failure PresentMacNativeLayerFrame(MacNativeLayerBinding& inOutBinding, const MacIOSurfaceHandle& surfaceHandle, const FramePacket& frame, MacNativeBridgePresentResult& outResult, MacIOSurfaceAllocation* allocation)
	{
		outResult = {};
		@autoreleasepool
		{
			if (allocation)
			{
				if (allocation->m_copyCommandBufferObject != 0) return WriteProducerFailure(1036, "macOS IOSurface copy completion has not been consumed");
				bool readCompleted = false;
				auto result = PollMacIOSurfaceReadCompletion(*allocation, readCompleted);
				if (!result.IsOk()) return result;
				if (!readCompleted) return WriteProducerFailure(1034, "macOS IOSurface is still being read by the presenter");
			}
			if (!inOutBinding.IsValid())
			{
				return MakeFailure(2111, "macOS native layer present requires a valid CAMetalLayer binding");
			}

			CAMetalLayer* metalLayer = (__bridge CAMetalLayer*)reinterpret_cast<void*>(inOutBinding.m_layerObject);
			if (metalLayer == nil)
			{
				return MakeFailure(2112, "macOS native layer present resolved the CAMetalLayer to nil");
			}

			id<MTLDevice> device = (__bridge id<MTLDevice>)reinterpret_cast<void*>(inOutBinding.m_deviceObject);
			if (device == nil)
			{
				return MakeFailure(2114, "macOS native layer present resolved the Metal device to nil");
			}

			id<MTLCommandQueue> commandQueue = (__bridge id<MTLCommandQueue>)reinterpret_cast<void*>(inOutBinding.m_commandQueueObject);
			if (commandQueue == nil)
			{
				return MakeFailure(2115, "macOS native layer present resolved the Metal command queue to nil");
			}

			id<MTLTexture> sourceTexture = CreateIOSurfaceBackedSourceTexture(device, surfaceHandle, inOutBinding);
			if (sourceTexture == nil)
			{
				return MakeFailure(2116, "macOS native layer present could not import the IOSurface texture");
			}
			[sourceTexture autorelease];

			id<CAMetalDrawable> drawable = [metalLayer nextDrawable];
			if (drawable == nil)
			{
				return MakeFailure(2113, "macOS native layer present could not acquire a drawable");
			}

			id<MTLCommandBuffer> commandBuffer = [commandQueue commandBuffer];
			if (commandBuffer == nil)
			{
				return MakeFailure(2117, "macOS native layer present could not create a Metal command buffer");
			}

			id<MTLBlitCommandEncoder> blitEncoder = [commandBuffer blitCommandEncoder];
			if (blitEncoder == nil)
			{
				return MakeFailure(2118, "macOS native layer present could not create a Metal blit encoder");
			}

			const MTLSize copySize = MTLSizeMake((NSUInteger)inOutBinding.m_width, (NSUInteger)inOutBinding.m_height, 1);
			[blitEncoder copyFromTexture:sourceTexture
						sourceSlice:0
						sourceLevel:0
					   sourceOrigin:MTLOriginMake(0, 0, 0)
					     sourceSize:copySize
					      toTexture:drawable.texture
			 destinationSlice:0
			 destinationLevel:0
			destinationOrigin:MTLOriginMake(0, 0, 0)];
			[blitEncoder endEncoding];
			[commandBuffer presentDrawable:drawable];
			if (allocation)
			{
				allocation->m_presentCommandBufferObject = RetainObjectiveCObject(commandBuffer);
			}
			[commandBuffer commit];

			inOutBinding.m_drawableObject = reinterpret_cast<uintptr_t>((__bridge void*)drawable);
			inOutBinding.m_importedIOSurfaceObject = surfaceHandle.m_surfaceObject;
			ReleaseObjectiveCObject(inOutBinding.m_lastSourceTextureObject);
			inOutBinding.m_lastSourceTextureObject = RetainObjectiveCObject(sourceTexture);
			inOutBinding.m_presentToken++;
			inOutBinding.m_sourceTextureToken++;
			outResult.m_drawableObject = inOutBinding.m_drawableObject;
			outResult.m_sourceTextureObject = inOutBinding.m_lastSourceTextureObject;
			outResult.m_presentToken = inOutBinding.m_presentToken;
			outResult.m_sourceTextureToken = inOutBinding.m_sourceTextureToken;
			outResult.m_usedRealCAMetalLayer = true;
			outResult.m_usedMetalCommandQueue = true;
			return Failure::Ok();
		}
	}

	MacNativeLayerBinding::~MacNativeLayerBinding()
	{
		ReleaseObjectiveCObject(m_lastSourceTextureObject);
		ReleaseObjectiveCObject(m_commandQueueObject);
		ReleaseObjectiveCObject(m_deviceObject);
		ReleaseObjectiveCObject(m_layerObject);
	}
#else
	uint32_t GetMacIOSurfaceBytesPerRowAlignment(PixelFormat)
	{
		return 64u;
	}

	Failure CreateMacIOSurfaceProducerTexture(MacIOSurfaceAllocation&)
	{
		return Failure::Ok();
	}

	Failure CreateMacRendererIntermediateTexture(uintptr_t, uint32_t, uint32_t, PixelFormat, uintptr_t& outTextureObject)
	{
		outTextureObject = 0;
		return Failure::Ok();
	}

	Failure PollMacIOSurfaceReadCompletion(MacIOSurfaceAllocation&, bool& outCompleted)
	{
		outCompleted = true;
		return Failure::Ok();
	}

	Failure PollMacIOSurfaceCopyCompletion(MacIOSurfaceAllocation&, bool& outCompleted)
	{
		outCompleted = true;
		return Failure::Ok();
	}

	Failure UploadMacRendererBytesToProducerTexture(MacIOSurfaceAllocation&, const void*, uint32_t, MacNativeBridgeRendererFrameInfo&)
	{
		return Failure::Ok();
	}

	Failure CopyMacRendererIntermediateToProducerTexture(MacIOSurfaceAllocation&, uintptr_t, MacNativeBridgeRendererFrameInfo&, uintptr_t, uint64_t)
	{
		return Failure::Ok();
	}

	Failure SynchronizeMacVulkanRenderTargetForMetalExport(uintptr_t, uintptr_t, uintptr_t& outSharedEventObject, uint64_t& outAcquireValue, CrossApiSyncKind& outSyncKind, bool& outCpuWaited)
	{
		outSharedEventObject = 0;
		outAcquireValue = 0;
		outSyncKind = CrossApiSyncKind::None;
		outCpuWaited = false;
		return Failure::FromDomain(ErrorDomain::Capability, 2199, "macOS Vulkan->Metal sync is unavailable on this platform");
	}

	void SetMacVulkanMetalInteropTestMode(bool)
	{
	}

	Failure ExportMacMetalTextureFromVulkanRenderTarget(uintptr_t, uintptr_t, uintptr_t, PixelFormat, uintptr_t& outTextureObject)
	{
		outTextureObject = 0;
		return Failure::FromDomain(ErrorDomain::Capability, 2199, "macOS Vulkan->Metal export is unavailable on this platform");
	}

	void ReleaseMacRendererIntermediateTexture(uintptr_t& inOutTextureObject)
	{
		inOutTextureObject = 0;
	}

	void ReleaseMacExportedTexture(uintptr_t& inOutTextureObject)
	{
		inOutTextureObject = 0;
	}

	Failure BindMacNativeLayer(const MacNativeHostHandle&, uint32_t, uint32_t, PixelFormat, TUniquePtr<MacNativeLayerBinding>&)
	{
		return Failure::FromDomain(ErrorDomain::Capability, 2199, "macOS native layer binding is unavailable on this platform");
	}

	Failure PresentMacNativeLayerFrame(MacNativeLayerBinding&, const MacIOSurfaceHandle&, const FramePacket&, MacNativeBridgePresentResult&, MacIOSurfaceAllocation*)
	{
		return Failure::FromDomain(ErrorDomain::Capability, 2199, "macOS native layer present is unavailable on this platform");
	}

	Failure CaptureMacIOSurfaceFrameEvidence(const MacIOSurfaceHandle&, uint32_t, uint32_t, MacNativeSurfaceFrameEvidence&)
	{
		return Failure::FromDomain(ErrorDomain::Capability, 2199, "macOS frame evidence capture is unavailable on this platform");
	}
#endif
}
