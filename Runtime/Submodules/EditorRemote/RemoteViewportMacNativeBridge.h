#pragma once

#include <cstdint>

#include "RemoteViewportFoundation.h"

namespace Sailor::EditorRemote
{
	enum class MacNativeHostHandleKind : uint8_t
	{
		None = 0,
		NSView = 1,
		CAMetalLayer = 2
	};

	struct MacNativeHostHandle
	{
		MacNativeHostHandleKind m_kind = MacNativeHostHandleKind::None;
		uintptr_t m_value = 0;

		bool IsValid() const
		{
			return m_kind != MacNativeHostHandleKind::None && m_value != 0;
		}

		auto operator<=>(const MacNativeHostHandle&) const = default;
	};

	struct MacNativeLayerBinding
	{
		uintptr_t m_hostObject = 0;
		uintptr_t m_layerObject = 0;
		uintptr_t m_drawableObject = 0;
		uintptr_t m_deviceObject = 0;
		uintptr_t m_commandQueueObject = 0;
		uintptr_t m_importedIOSurfaceObject = 0;
		uintptr_t m_lastSourceTextureObject = 0;
		uint64_t m_bindingToken = 0;
		uint64_t m_presentToken = 0;
		uint64_t m_sourceTextureToken = 0;
		uint32_t m_width = 0;
		uint32_t m_height = 0;
		PixelFormat m_pixelFormat = PixelFormat::Unknown;
		bool m_hostOwnsLayer = false;
		bool m_usesSyntheticSourceTexture = false;

		bool IsValid() const
		{
			return m_bindingToken != 0 && m_layerObject != 0 && m_deviceObject != 0 && m_commandQueueObject != 0 && m_width != 0 && m_height != 0 && m_pixelFormat != PixelFormat::Unknown;
		}

		auto operator<=>(const MacNativeLayerBinding&) const = default;
	};

	struct MacNativeBridgePresentResult
	{
		uintptr_t m_drawableObject = 0;
		uintptr_t m_sourceTextureObject = 0;
		uint64_t m_presentToken = 0;
		uint64_t m_sourceTextureToken = 0;
		bool m_usedRealCAMetalLayer = false;
		bool m_usedMetalCommandQueue = false;
		bool m_usedSyntheticSourceTexture = false;

		bool IsValid() const
		{
			return m_presentToken != 0;
		}
	};


	struct MacNativeBridgeProducerPattern
	{
		ViewportId m_viewportId = 0;
		ConnectionEpoch m_epoch = 0;
		SurfaceGeneration m_generation = 0;
		FrameIndex m_frameIndex = 0;
		uint32_t m_width = 0;
		uint32_t m_height = 0;
	};

	struct MacNativeBridgeRendererFrameInfo
	{
		uint64_t m_rendererTextureToken = 0;
		uint64_t m_producerCopyToken = 0;
		bool m_usedRendererIntermediateTexture = false;
		bool m_usedGpuCopyIntoProducerTexture = false;
		bool m_usedCpuUploadIntoProducerTexture = false;
	};

	Failure CreateMacIOSurfaceProducerTexture(uintptr_t surfaceObject, uint32_t width, uint32_t height, PixelFormat pixelFormat, uint32_t planeIndex, uintptr_t& outDeviceObject, uintptr_t& outTextureObject);
	Failure CreateMacRendererIntermediateTexture(uintptr_t deviceObject, uint32_t width, uint32_t height, PixelFormat pixelFormat, uintptr_t& outTextureObject);
	Failure UploadMacRendererPatternToIntermediateTexture(uintptr_t textureObject, uint32_t width, uint32_t height, const MacNativeBridgeProducerPattern& pattern);
	Failure UploadMacRendererBytesToProducerTexture(uintptr_t destinationTextureObject, uint32_t width, uint32_t height, const void* bytes, uint32_t bytesPerRow, MacNativeBridgeRendererFrameInfo& outFrameInfo);
	Failure CopyMacRendererIntermediateToProducerTexture(uintptr_t deviceObject, uintptr_t sourceTextureObject, uintptr_t destinationTextureObject, uint32_t width, uint32_t height, MacNativeBridgeRendererFrameInfo& outFrameInfo);
	Failure SynchronizeMacVulkanRenderTargetForMetalExport(uintptr_t vulkanDeviceHandle, uint64_t& outAcquireValue, CrossApiSyncKind& outSyncKind, bool& outCpuWaited);
	Failure ExportMacMetalTextureFromVulkanRenderTarget(uintptr_t vulkanDeviceHandle, uintptr_t vulkanImageHandle, uintptr_t vulkanImageViewHandle, PixelFormat pixelFormat, uintptr_t& outTextureObject);
	void ReleaseMacIOSurfaceProducerTexture(uintptr_t& inOutDeviceObject, uintptr_t& inOutTextureObject);
	void ReleaseMacRendererIntermediateTexture(uintptr_t& inOutTextureObject);
	void ReleaseMacExportedTexture(uintptr_t& inOutTextureObject);

	Failure BindMacNativeLayer(const MacNativeHostHandle& hostHandle, uint32_t width, uint32_t height, PixelFormat pixelFormat, MacNativeLayerBinding& inOutBinding);
	Failure PresentMacNativeLayerFrame(MacNativeLayerBinding& inOutBinding, const MacIOSurfaceHandle& surfaceHandle, const FramePacket& frame, MacNativeBridgePresentResult& outResult);
	void ResetMacNativeLayerBinding(MacNativeLayerBinding& inOutBinding);
}
