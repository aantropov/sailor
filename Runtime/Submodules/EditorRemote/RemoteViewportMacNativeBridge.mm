#include "RemoteViewportMacNativeBridge.h"

#if defined(__APPLE__)
#import <AppKit/AppKit.h>
#import <QuartzCore/CAMetalLayer.h>
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

		id<MTLTexture> CreateSyntheticSourceTexture(id<MTLDevice> device, const FramePacket& frame, const MacNativeLayerBinding& binding)
		{
			if (device == nil || binding.m_width == 0 || binding.m_height == 0)
			{
				return nil;
			}

			MTLTextureDescriptor* descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:ToMetalPixelFormat(binding.m_pixelFormat)
				width:binding.m_width
				height:binding.m_height
				mipmapped:NO];
			descriptor.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite | MTLTextureUsageRenderTarget;
			descriptor.storageMode = MTLStorageModeShared;
			id<MTLTexture> texture = [device newTextureWithDescriptor:descriptor];
			if (texture == nil)
			{
				return nil;
			}

			const uint32_t bytesPerPixel = 4;
			const uint32_t bytesPerRow = binding.m_width * bytesPerPixel;
			const size_t byteCount = static_cast<size_t>(bytesPerRow) * static_cast<size_t>(binding.m_height);
			NSMutableData* data = [NSMutableData dataWithLength:byteCount];
			if (data == nil || data.length != byteCount)
			{
				return nil;
			}

			auto* pixels = static_cast<uint8_t*>(data.mutableBytes);
			const uint8_t seedA = static_cast<uint8_t>((frame.m_frameIndex * 17u) & 0xffu);
			const uint8_t seedB = static_cast<uint8_t>((frame.m_connectionEpoch * 29u) & 0xffu);
			const uint8_t seedC = static_cast<uint8_t>((frame.m_generation * 43u) & 0xffu);
			for (uint32_t y = 0; y < binding.m_height; y++)
			{
				for (uint32_t x = 0; x < binding.m_width; x++)
				{
					const size_t index = static_cast<size_t>(y) * bytesPerRow + static_cast<size_t>(x) * bytesPerPixel;
					pixels[index + 0] = static_cast<uint8_t>((x + seedA) & 0xffu);
					pixels[index + 1] = static_cast<uint8_t>((y + seedB) & 0xffu);
					pixels[index + 2] = seedC;
					pixels[index + 3] = 255u;
				}
			}

			MTLRegion region = MTLRegionMake2D(0, 0, binding.m_width, binding.m_height);
			[texture replaceRegion:region mipmapLevel:0 withBytes:data.bytes bytesPerRow:bytesPerRow];
			return texture;
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
		return alignment > 0 ? static_cast<uint32_t>(alignment) : 64u;
	}

	Failure CreateMacIOSurfaceProducerTexture(uintptr_t surfaceObject, uint32_t width, uint32_t height, PixelFormat pixelFormat, uint32_t planeIndex, uintptr_t& outDeviceObject, uintptr_t& outTextureObject)
	{
		outDeviceObject = 0;
		outTextureObject = 0;
		if (surfaceObject == 0 || width == 0 || height == 0)
		{
			return CreateProducerFailure(1004, "macOS producer texture creation requires a valid IOSurface allocation");
		}

		id<MTLDevice> device = MTLCreateSystemDefaultDevice();
		if (device == nil)
		{
			return CreateProducerFailure(1005, "macOS producer texture creation could not create a Metal device");
		}

		const auto metalPixelFormat = ToMetalPixelFormat(pixelFormat);
		if (metalPixelFormat == MTLPixelFormatInvalid)
		{
			return CreateProducerFailure(1006, "macOS producer texture creation only supports BGRA8 transport in this slice");
		}

		MTLTextureDescriptor* descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:metalPixelFormat
			width:width
			height:height
			mipmapped:NO];
		descriptor.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite | MTLTextureUsageRenderTarget;
		descriptor.storageMode = MTLStorageModeShared;
		id<MTLTexture> texture = [device newTextureWithDescriptor:descriptor iosurface:reinterpret_cast<IOSurfaceRef>(surfaceObject) plane:planeIndex];
		if (texture == nil)
		{
			return CreateProducerFailure(1007, "macOS producer texture creation could not create an IOSurface-backed Metal texture");
		}

		outDeviceObject = reinterpret_cast<uintptr_t>((__bridge void*)device);
		outTextureObject = reinterpret_cast<uintptr_t>((__bridge void*)texture);
		return Failure::Ok();
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

	Failure UploadMacRendererPatternToIntermediateTexture(uintptr_t textureObject, uint32_t width, uint32_t height, const MacNativeBridgeProducerPattern& pattern)
	{
		if (textureObject == 0 || width == 0 || height == 0)
		{
			return WriteProducerFailure(1012, "macOS renderer intermediate upload requires a valid Metal texture and extents");
		}

		id<MTLTexture> texture = (__bridge id<MTLTexture>)reinterpret_cast<void*>(textureObject);
		if (texture == nil)
		{
			return WriteProducerFailure(1013, "macOS renderer intermediate upload resolved the Metal texture to nil");
		}

		const uint32_t bytesPerRow = width * 4u;
		const size_t byteCount = static_cast<size_t>(bytesPerRow) * static_cast<size_t>(height);
		NSMutableData* data = [NSMutableData dataWithLength:byteCount];
		if (data == nil || data.length != byteCount)
		{
			return WriteProducerFailure(1014, "macOS renderer intermediate upload could not allocate staging bytes");
		}

		auto* pixels = static_cast<uint8_t*>(data.mutableBytes);
		const uint8_t seedA = static_cast<uint8_t>((pattern.m_frameIndex * 17u + pattern.m_generation * 13u) & 0xffu);
		const uint8_t seedB = static_cast<uint8_t>((pattern.m_epoch * 29u + pattern.m_viewportId * 7u) & 0xffu);
		const uint8_t seedC = static_cast<uint8_t>((pattern.m_width + pattern.m_height + pattern.m_frameIndex * 3u) & 0xffu);
		for (uint32_t y = 0; y < height; ++y)
		{
			for (uint32_t x = 0; x < width; ++x)
			{
				const size_t index = static_cast<size_t>(y) * bytesPerRow + static_cast<size_t>(x) * 4u;
				pixels[index + 0] = static_cast<uint8_t>((x + seedA) & 0xffu);
				pixels[index + 1] = static_cast<uint8_t>((y + seedB) & 0xffu);
				pixels[index + 2] = seedC;
				pixels[index + 3] = 255u;
			}
		}

		[texture replaceRegion:MTLRegionMake2D(0, 0, width, height) mipmapLevel:0 withBytes:data.bytes bytesPerRow:bytesPerRow];
		return Failure::Ok();
	}

	Failure UploadMacRendererBytesToProducerTexture(uintptr_t destinationTextureObject, uint32_t width, uint32_t height, const void* bytes, uint32_t bytesPerRow, MacNativeBridgeRendererFrameInfo& outFrameInfo)
	{
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

	Failure CopyMacRendererIntermediateToProducerTexture(uintptr_t deviceObject, uintptr_t sourceTextureObject, uintptr_t destinationTextureObject, uint32_t width, uint32_t height, MacNativeBridgeRendererFrameInfo& outFrameInfo, uintptr_t sharedEventObject, uint64_t sharedEventValue)
	{
		if (deviceObject == 0 || sourceTextureObject == 0 || destinationTextureObject == 0 || width == 0 || height == 0)
		{
			return WriteProducerFailure(1015, "macOS producer GPU copy requires valid Metal objects and extents");
		}

		id<MTLDevice> device = (__bridge id<MTLDevice>)reinterpret_cast<void*>(deviceObject);
		id<MTLTexture> sourceTexture = (__bridge id<MTLTexture>)reinterpret_cast<void*>(sourceTextureObject);
		id<MTLTexture> destinationTexture = (__bridge id<MTLTexture>)reinterpret_cast<void*>(destinationTextureObject);
		if (device == nil || sourceTexture == nil || destinationTexture == nil)
		{
			return WriteProducerFailure(1016, "macOS producer GPU copy resolved a Metal object to nil");
		}

		id<MTLCommandQueue> commandQueue = [device newCommandQueue];
		if (commandQueue == nil)
		{
			return WriteProducerFailure(1017, "macOS producer GPU copy could not create a Metal command queue");
		}

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
		[commandBuffer commit];
		[commandBuffer waitUntilCompleted];

		outFrameInfo.m_rendererTextureToken = NextRendererTextureToken();
		outFrameInfo.m_producerCopyToken = NextProducerCopyToken();
		outFrameInfo.m_crossApiWaitValue = sharedEventValue;
		outFrameInfo.m_usedRendererIntermediateTexture = true;
		outFrameInfo.m_usedGpuCopyIntoProducerTexture = true;
		outFrameInfo.m_usedCpuUploadIntoProducerTexture = false;
		outFrameInfo.m_waitedOnCrossApiSharedEvent = sharedEventObject != 0 && sharedEventValue != 0;
		return Failure::Ok();
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

	void ReleaseMacIOSurfaceProducerTexture(uintptr_t& inOutDeviceObject, uintptr_t& inOutTextureObject)
	{
		if (inOutTextureObject != 0)
		{
			CFRelease(reinterpret_cast<CFTypeRef>(inOutTextureObject));
			inOutTextureObject = 0;
		}
		if (inOutDeviceObject != 0)
		{
			CFRelease(reinterpret_cast<CFTypeRef>(inOutDeviceObject));
			inOutDeviceObject = 0;
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

	Failure BindMacNativeLayer(const MacNativeHostHandle& hostHandle, uint32_t width, uint32_t height, PixelFormat pixelFormat, MacNativeLayerBinding& inOutBinding)
	{
		return RunOnMainThreadSync([&]() -> Failure
		{
			if (!hostHandle.IsValid())
			{
				return MakeFailure(2101, "macOS native layer binding requires a valid host handle");
			}

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
					view.wantsLayer = YES;
					view.layer = metalLayer;
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

			id<MTLDevice> device = metalLayer.device;
			if (device == nil)
			{
				device = MTLCreateSystemDefaultDevice();
				if (device == nil)
				{
					return MakeFailure(2105, "macOS native layer binding could not create a Metal device");
				}
				metalLayer.device = device;
			}

			id<MTLCommandQueue> commandQueue = [device newCommandQueue];
			if (commandQueue == nil)
			{
				return MakeFailure(2107, "macOS native layer binding could not create a Metal command queue");
			}

			const auto metalPixelFormat = ToMetalPixelFormat(pixelFormat);
			if (metalPixelFormat == MTLPixelFormatInvalid)
			{
				return MakeFailure(2106, "macOS native layer binding only supports BGRA8 transport in this slice");
			}

			metalLayer.pixelFormat = metalPixelFormat;
			metalLayer.framebufferOnly = NO;
			metalLayer.drawableSize = CGSizeMake(width, height);
			if (view != nil)
			{
				metalLayer.contentsScale = view.window != nil ? view.window.backingScaleFactor : NSScreen.mainScreen.backingScaleFactor;
			}

			inOutBinding.m_hostObject = hostHandle.m_kind == MacNativeHostHandleKind::NSView ? hostHandle.m_value : 0;
			inOutBinding.m_layerObject = reinterpret_cast<uintptr_t>((__bridge void*)metalLayer);
			inOutBinding.m_drawableObject = 0;
			inOutBinding.m_deviceObject = reinterpret_cast<uintptr_t>((__bridge void*)device);
			inOutBinding.m_commandQueueObject = reinterpret_cast<uintptr_t>((__bridge void*)commandQueue);
			inOutBinding.m_importedIOSurfaceObject = 0;
			inOutBinding.m_lastSourceTextureObject = 0;
			inOutBinding.m_width = width;
			inOutBinding.m_height = height;
			inOutBinding.m_pixelFormat = pixelFormat;
			inOutBinding.m_hostOwnsLayer = hostOwnsLayer;
			inOutBinding.m_bindingToken++;
			inOutBinding.m_presentToken = 0;
			inOutBinding.m_sourceTextureToken = 0;
			inOutBinding.m_usesSyntheticSourceTexture = false;
			return Failure::Ok();
		});
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

	Failure PresentMacNativeLayerFrame(MacNativeLayerBinding& inOutBinding, const MacIOSurfaceHandle& surfaceHandle, const FramePacket& frame, MacNativeBridgePresentResult& outResult)
	{
		if (![NSThread isMainThread])
		{
			return RunOnMainThreadSync([&]() -> Failure
			{
				return PresentMacNativeLayerFrame(inOutBinding, surfaceHandle, frame, outResult);
			});
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

		id<CAMetalDrawable> drawable = RunOnMainThreadSync([&]() -> id<CAMetalDrawable>
		{
			return [metalLayer nextDrawable];
		});
		if (drawable == nil)
		{
			return MakeFailure(2113, "macOS native layer present could not acquire a drawable");
		}

		id<MTLTexture> sourceTexture = CreateIOSurfaceBackedSourceTexture(device, surfaceHandle, inOutBinding);
		const bool usedSyntheticSourceTexture = sourceTexture == nil;
		if (sourceTexture == nil)
		{
			sourceTexture = CreateSyntheticSourceTexture(device, frame, inOutBinding);
		}
		if (sourceTexture == nil)
		{
			return MakeFailure(2116, "macOS native layer present could not create a source Metal texture");
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
		[commandBuffer commit];
		[commandBuffer waitUntilScheduled];

		inOutBinding.m_drawableObject = reinterpret_cast<uintptr_t>((__bridge void*)drawable);
		inOutBinding.m_importedIOSurfaceObject = surfaceHandle.m_surfaceObject;
		inOutBinding.m_lastSourceTextureObject = reinterpret_cast<uintptr_t>((__bridge void*)sourceTexture);
		inOutBinding.m_presentToken++;
		inOutBinding.m_sourceTextureToken++;
		inOutBinding.m_usesSyntheticSourceTexture = usedSyntheticSourceTexture;
		outResult.m_drawableObject = inOutBinding.m_drawableObject;
		outResult.m_sourceTextureObject = inOutBinding.m_lastSourceTextureObject;
		outResult.m_presentToken = inOutBinding.m_presentToken;
		outResult.m_sourceTextureToken = inOutBinding.m_sourceTextureToken;
		outResult.m_usedRealCAMetalLayer = true;
		outResult.m_usedMetalCommandQueue = true;
		outResult.m_usedSyntheticSourceTexture = usedSyntheticSourceTexture;
		return Failure::Ok();
	}

	void ResetMacNativeLayerBinding(MacNativeLayerBinding& inOutBinding)
	{
		inOutBinding = {};
	}
#else
	uint32_t GetMacIOSurfaceBytesPerRowAlignment(PixelFormat)
	{
		return 64u;
	}

	Failure CreateMacIOSurfaceProducerTexture(uintptr_t, uint32_t, uint32_t, PixelFormat, uint32_t, uintptr_t& outDeviceObject, uintptr_t& outTextureObject)
	{
		outDeviceObject = 0;
		outTextureObject = 0;
		return Failure::Ok();
	}

	Failure CreateMacRendererIntermediateTexture(uintptr_t, uint32_t, uint32_t, PixelFormat, uintptr_t& outTextureObject)
	{
		outTextureObject = 0;
		return Failure::Ok();
	}

	Failure UploadMacRendererPatternToIntermediateTexture(uintptr_t, uint32_t, uint32_t, const MacNativeBridgeProducerPattern&)
	{
		return Failure::Ok();
	}

	Failure UploadMacRendererBytesToProducerTexture(uintptr_t, uint32_t, uint32_t, const void*, uint32_t, MacNativeBridgeRendererFrameInfo&)
	{
		return Failure::Ok();
	}

	Failure CopyMacRendererIntermediateToProducerTexture(uintptr_t, uintptr_t, uintptr_t, uint32_t, uint32_t, MacNativeBridgeRendererFrameInfo&, uintptr_t, uint64_t)
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

	void ReleaseMacIOSurfaceProducerTexture(uintptr_t& inOutDeviceObject, uintptr_t& inOutTextureObject)
	{
		inOutDeviceObject = 0;
		inOutTextureObject = 0;
	}

	void ReleaseMacRendererIntermediateTexture(uintptr_t& inOutTextureObject)
	{
		inOutTextureObject = 0;
	}

	void ReleaseMacExportedTexture(uintptr_t& inOutTextureObject)
	{
		inOutTextureObject = 0;
	}

	Failure BindMacNativeLayer(const MacNativeHostHandle&, uint32_t, uint32_t, PixelFormat, MacNativeLayerBinding&)
	{
		return Failure::FromDomain(ErrorDomain::Capability, 2199, "macOS native layer binding is unavailable on this platform");
	}

	Failure PresentMacNativeLayerFrame(MacNativeLayerBinding&, const MacIOSurfaceHandle&, const FramePacket&, MacNativeBridgePresentResult&)
	{
		return Failure::FromDomain(ErrorDomain::Capability, 2199, "macOS native layer present is unavailable on this platform");
	}

	Failure CaptureMacIOSurfaceFrameEvidence(const MacIOSurfaceHandle&, uint32_t, uint32_t, MacNativeSurfaceFrameEvidence&)
	{
		return Failure::FromDomain(ErrorDomain::Capability, 2199, "macOS frame evidence capture is unavailable on this platform");
	}

	void ResetMacNativeLayerBinding(MacNativeLayerBinding& inOutBinding)
	{
		inOutBinding = {};
	}
#endif
}
