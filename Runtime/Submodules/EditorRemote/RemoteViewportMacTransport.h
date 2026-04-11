#pragma once

#include <algorithm>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>
#include <memory>

#if defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#include <IOSurface/IOSurface.h>
#endif

#include "EditorViewportSession.h"
#include "RemoteViewportMacNativeBridge.h"
#include "RemoteViewportRuntime.h"

namespace Sailor::EditorRemote
{
	inline constexpr uint32_t AlignMacIOSurfaceStride(const uint32_t value, const uint32_t alignment)
	{
		if (alignment <= 1u)
		{
			return value;
		}

		const uint32_t remainder = value % alignment;
		return remainder == 0u ? value : (value + alignment - remainder);
	}

	struct MacViewportSurfaceKey
	{
		ViewportId m_viewportId = 0;
		ConnectionEpoch m_epoch = 0;
		SurfaceGeneration m_generation = 0;

		auto operator<=>(const MacViewportSurfaceKey&) const = default;
	};

	struct MacViewportSurfaceKeyHasher
	{
		size_t operator()(const MacViewportSurfaceKey& key) const noexcept
		{
			size_t seed = std::hash<uint64_t>{}(key.m_viewportId);
			seed ^= std::hash<uint64_t>{}(key.m_epoch) + 0x9e3779b97f4a7c15ull + (seed << 6) + (seed >> 2);
			seed ^= std::hash<uint64_t>{}(key.m_generation) + 0x9e3779b97f4a7c15ull + (seed << 6) + (seed >> 2);
			return seed;
		}
	};

	struct MacIOSurfacePlaneLayout
	{
		uint32_t m_planeIndex = 0;
		uint32_t m_planeCount = 1;
		uint32_t m_width = 0;
		uint32_t m_height = 0;
		uint32_t m_bytesPerRow = 0;
		uint32_t m_bytesPerElement = 0;

		bool IsValid() const
		{
			return m_planeCount > 0 && m_width > 0 && m_height > 0 && m_bytesPerRow > 0 && m_bytesPerElement > 0;
		}

		auto operator<=>(const MacIOSurfacePlaneLayout&) const = default;
	};

	enum class MacRendererFrameSourceKind : uint8_t
	{
		Unknown = 0,
		SyntheticIntermediate,
		RendererOwnedMetalTexture,
		RendererOwnedRenderTargetMetadata
	};

	struct MacRendererFrameSource
	{
		MacRendererFrameSourceKind m_kind = MacRendererFrameSourceKind::Unknown;
		uintptr_t m_textureObject = 0;
		uintptr_t m_sourceObject = 0;
		uint64_t m_sourceToken = 0;
		uint64_t m_crossApiAcquireValue = 0;
		uintptr_t m_crossApiSharedEventObject = 0;
		uint32_t m_width = 0;
		uint32_t m_height = 0;
		uint32_t m_bytesPerRow = 0;
		PixelFormat m_pixelFormat = PixelFormat::Unknown;
		CrossApiSyncKind m_crossApiSyncKind = CrossApiSyncKind::None;
		std::shared_ptr<std::vector<uint8_t>> m_cpuBytes{};
		std::string m_debugName{};
		bool m_releaseTextureObjectAfterUse = false;
		bool m_crossApiCpuWaited = false;

		bool IsValid() const
		{
			return m_kind != MacRendererFrameSourceKind::Unknown && m_width != 0 && m_height != 0 && m_pixelFormat != PixelFormat::Unknown;
		}

		auto operator<=>(const MacRendererFrameSource&) const = default;
	};

	struct MacIOSurfaceAllocation
	{
		uint32_t m_surfaceId = 0;
		uint64_t m_registryId = 0;
		uintptr_t m_surfaceObject = 0;
		uintptr_t m_producerDeviceObject = 0;
		uintptr_t m_producerTextureObject = 0;
		uintptr_t m_rendererIntermediateTextureObject = 0;
		uint64_t m_allocationToken = 0;
		uint64_t m_lastWrittenFrameIndex = 0;
		uint64_t m_lastRendererTextureToken = 0;
		uint64_t m_lastProducerCopyToken = 0;
		uint64_t m_lastCrossApiAcquireValue = 0;
		PixelFormat m_pixelFormat = PixelFormat::Unknown;
		ColorSpace m_colorSpace = ColorSpace::Unknown;
		uint32_t m_usageFlags = 0;
		bool m_framebufferOnly = false;
		std::string m_debugLabel{};
		MacIOSurfacePlaneLayout m_plane{};
		MacRendererFrameSource m_lastRendererSource{};

		bool IsValid() const
		{
			return m_surfaceObject != 0 && m_surfaceId != 0 && m_registryId != 0 && m_allocationToken != 0 && m_plane.IsValid();
		}

		auto operator<=>(const MacIOSurfaceAllocation&) const = default;
	};

	struct MacIOSurfaceExportMetadata
	{
		MacIOSurfaceHandle m_handle{};
		uint64_t m_exportToken = 0;
		uint64_t m_lastAcquireValue = 0;
		uint64_t m_lastReleaseValue = 0;
		uint64_t m_lastCrossApiAcquireValue = 0;
		uintptr_t m_sharedEventObject = 0;
		CrossApiSyncKind m_crossApiSyncKind = CrossApiSyncKind::None;
		bool m_requiresHostRelease = false;
		bool m_crossApiCpuWaited = false;

		bool IsValid() const
		{
			return m_handle.IsValid() && m_exportToken != 0;
		}

		auto operator<=>(const MacIOSurfaceExportMetadata&) const = default;
	};

	struct MacNativePresentationState
	{
		ViewportId m_viewportId = 0;
		ConnectionEpoch m_epoch = 0;
		SurfaceGeneration m_generation = 0;
		uint64_t m_registryId = 0;
		uint64_t m_importToken = 0;
		uint64_t m_nativeLayerToken = 0;
		uint64_t m_currentDrawableToken = 0;
		uint64_t m_presentedFrameCount = 0;
		uint32_t m_width = 0;
		uint32_t m_height = 0;
		PixelFormat m_pixelFormat = PixelFormat::Unknown;
		bool m_framebufferOnly = false;
		MacNativeHostHandle m_hostHandle{};
		std::optional<MacNativeLayerBinding> m_layerBinding{};
		std::optional<MacIOSurfaceHandle> m_importedSurface{};
		bool m_usesRealCAMetalLayer = false;

		bool IsValid() const
		{
			return m_viewportId != 0 && m_epoch != 0 && m_generation != 0 && m_registryId != 0 && m_importToken != 0 &&
				m_nativeLayerToken != 0 && m_width != 0 && m_height != 0 && m_pixelFormat != PixelFormat::Unknown;
		}

		auto operator<=>(const MacNativePresentationState&) const = default;
	};

	struct MacViewportSurfaceState
	{
		MacViewportSurfaceKey m_key{};
		ViewportDescriptor m_viewport{};
		TransportDescriptor m_transport{};
		FrameIndex m_lastExportedFrameIndex = 0;
		bool m_frameBegun = false;
		bool m_needsHostReset = false;
		std::optional<MacIOSurfaceAllocation> m_nativeAllocation{};
		std::optional<MacIOSurfaceExportMetadata> m_lastExport{};
	};

	class IMacRendererFrameSourceProvider
	{
	public:
		virtual ~IMacRendererFrameSourceProvider() = default;
		virtual Failure AcquireFrameSource(const MacViewportSurfaceState& state, FrameIndex nextFrameIndex, MacRendererFrameSource& outSource) = 0;
	};

	class IMacIOSurfaceProvider
	{
	public:
		virtual ~IMacIOSurfaceProvider() = default;
		virtual Failure CreateOrResizeSurface(const ViewportDescriptor& viewport, ConnectionEpoch epoch, SurfaceGeneration generation, MacViewportSurfaceState& inOutState) = 0;
		virtual Failure BeginFrame(MacViewportSurfaceState& state) = 0;
		virtual Failure ExportFrame(MacViewportSurfaceState& state, FramePacket& outFrame) = 0;
		virtual Failure ReleaseSurface(const MacViewportSurfaceState& state) = 0;
		virtual Failure GetLastFailure() const = 0;
	};

	class MacLoopbackIOSurfaceProvider : public IMacIOSurfaceProvider
	{
	public:
		explicit MacLoopbackIOSurfaceProvider(IMacRendererFrameSourceProvider* rendererFrameSourceProvider = nullptr) :
			m_rendererFrameSourceProvider(rendererFrameSourceProvider)
		{
		}

		Failure CreateOrResizeSurface(const ViewportDescriptor& viewport, ConnectionEpoch epoch, SurfaceGeneration generation, MacViewportSurfaceState& inOutState) override
		{
			MacIOSurfaceAllocation allocation{};
			allocation.m_registryId = (epoch << 32ull) | generation;
			allocation.m_allocationToken = ++m_nextAllocationToken;
			allocation.m_pixelFormat = viewport.m_pixelFormat;
			allocation.m_colorSpace = viewport.m_colorSpace;
			allocation.m_usageFlags = viewport.m_usageFlags;
			allocation.m_framebufferOnly = false;
			allocation.m_debugLabel = viewport.m_debugName;
			allocation.m_plane.m_planeIndex = 0;
			allocation.m_plane.m_planeCount = 1;
			allocation.m_plane.m_width = viewport.m_width;
			allocation.m_plane.m_height = viewport.m_height;
			allocation.m_plane.m_bytesPerElement = 4u;
#if defined(__APPLE__)
			const uint32_t minimumStrideAlignment = std::max(GetMacIOSurfaceBytesPerRowAlignment(viewport.m_pixelFormat), allocation.m_plane.m_bytesPerElement);
			allocation.m_plane.m_bytesPerRow = AlignMacIOSurfaceStride(viewport.m_width * allocation.m_plane.m_bytesPerElement, minimumStrideAlignment);
#else
			allocation.m_plane.m_bytesPerRow = viewport.m_width * allocation.m_plane.m_bytesPerElement;
#endif

#if defined(__APPLE__)
			CFMutableDictionaryRef properties = CFDictionaryCreateMutable(kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
			if (properties == nullptr)
			{
				m_lastFailure = Failure::FromDomain(ErrorDomain::Transport, 1001, "Failed to allocate IOSurface property dictionary");
				return m_lastFailure;
			}

			auto releaseProperties = [&properties]()
			{
				if (properties != nullptr)
				{
					CFRelease(properties);
					properties = nullptr;
				}
			};

			auto setIntProperty = [properties](CFStringRef key, int32_t value)
			{
				int32_t copy = value;
				CFNumberRef number = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &copy);
				if (number != nullptr)
				{
					CFDictionarySetValue(properties, key, number);
					CFRelease(number);
				}
			};

			setIntProperty(kIOSurfaceWidth, static_cast<int32_t>(viewport.m_width));
			setIntProperty(kIOSurfaceHeight, static_cast<int32_t>(viewport.m_height));
			setIntProperty(kIOSurfaceBytesPerElement, static_cast<int32_t>(allocation.m_plane.m_bytesPerElement));
			setIntProperty(kIOSurfaceBytesPerRow, static_cast<int32_t>(allocation.m_plane.m_bytesPerRow));
			setIntProperty(kIOSurfaceAllocSize, static_cast<int32_t>(allocation.m_plane.m_bytesPerRow * allocation.m_plane.m_height));
			setIntProperty(kIOSurfacePixelFormat, static_cast<int32_t>('BGRA'));

			IOSurfaceRef surface = IOSurfaceCreate(properties);
			releaseProperties();
			if (surface == nullptr)
			{
				m_lastFailure = Failure::FromDomain(ErrorDomain::Transport, 1001, "Failed to create real macOS IOSurface");
				return m_lastFailure;
			}

			allocation.m_surfaceObject = reinterpret_cast<uintptr_t>(surface);
			allocation.m_surfaceId = IOSurfaceGetID(surface);
			auto producerTextureResult = CreateMacIOSurfaceProducerTexture(allocation.m_surfaceObject, allocation.m_plane.m_width, allocation.m_plane.m_height, allocation.m_pixelFormat, allocation.m_plane.m_planeIndex, allocation.m_producerDeviceObject, allocation.m_producerTextureObject);
			if (!producerTextureResult.IsOk())
			{
				CFRelease(surface);
				m_lastFailure = producerTextureResult;
				return producerTextureResult;
			}

			auto rendererTextureResult = CreateMacRendererIntermediateTexture(allocation.m_producerDeviceObject, allocation.m_plane.m_width, allocation.m_plane.m_height, allocation.m_pixelFormat, allocation.m_rendererIntermediateTextureObject);
			if (!rendererTextureResult.IsOk())
			{
				ReleaseMacIOSurfaceProducerTexture(allocation.m_producerDeviceObject, allocation.m_producerTextureObject);
				CFRelease(surface);
				m_lastFailure = rendererTextureResult;
				return rendererTextureResult;
			}
#else
			allocation.m_surfaceObject = allocation.m_registryId;
			allocation.m_surfaceId = ++m_nextSurfaceId;
#endif

			MacIOSurfaceExportMetadata exportMetadata{};
			exportMetadata.m_handle.m_surfaceId = allocation.m_surfaceId;
			exportMetadata.m_handle.m_registryId = allocation.m_registryId;
			exportMetadata.m_handle.m_surfaceObject = allocation.m_surfaceObject;
			exportMetadata.m_handle.m_sharedEventObject = 0;
			exportMetadata.m_handle.m_planeIndex = allocation.m_plane.m_planeIndex;
			exportMetadata.m_handle.m_planeCount = allocation.m_plane.m_planeCount;
			exportMetadata.m_handle.m_bytesPerRow = allocation.m_plane.m_bytesPerRow;
			exportMetadata.m_handle.m_bytesPerElement = allocation.m_plane.m_bytesPerElement;
			exportMetadata.m_handle.m_framebufferOnly = allocation.m_framebufferOnly;
			exportMetadata.m_exportToken = ++m_nextExportToken;
			exportMetadata.m_lastAcquireValue = 0;
			exportMetadata.m_lastReleaseValue = 0;
			exportMetadata.m_requiresHostRelease = false;

			if (!allocation.IsValid() || !exportMetadata.IsValid())
			{
				m_lastFailure = Failure::FromDomain(ErrorDomain::Transport, 1001, "Failed to materialize macOS IOSurface allocation metadata");
				return m_lastFailure;
			}

			inOutState.m_key = { viewport.m_viewportId, epoch, generation };
			inOutState.m_viewport = viewport;
			inOutState.m_transport.m_transportType = TransportType::MacIOSurface;
			inOutState.m_transport.m_syncMode = SyncMode::ExplicitFence;
			inOutState.m_transport.m_protocolVersion = 1;
			inOutState.m_transport.m_width = viewport.m_width;
			inOutState.m_transport.m_height = viewport.m_height;
			inOutState.m_transport.m_pixelFormat = viewport.m_pixelFormat;
			inOutState.m_transport.m_usageFlags = viewport.m_usageFlags;
			inOutState.m_transport.m_macSurfaces = { exportMetadata.m_handle };
			inOutState.m_transport.m_ready = true;
			inOutState.m_frameBegun = false;
			inOutState.m_nativeAllocation = allocation;
			inOutState.m_lastExport = exportMetadata;
			m_liveAllocations[inOutState.m_key] = allocation;
			m_lastFailure = Failure::Ok();
			return Failure::Ok();
		}

		Failure BeginFrame(MacViewportSurfaceState& state) override
		{
			if (!state.m_nativeAllocation.has_value() || !state.m_nativeAllocation->IsValid())
			{
				m_lastFailure = Failure::FromDomain(ErrorDomain::Session, 1002, "macOS frame begin requires a live IOSurface allocation");
				return m_lastFailure;
			}

			const auto nextFrameIndex = state.m_lastExportedFrameIndex + 1;
			MacRendererFrameSource rendererSource{};
			if (m_rendererFrameSourceProvider != nullptr)
			{
				auto sourceResult = m_rendererFrameSourceProvider->AcquireFrameSource(state, nextFrameIndex, rendererSource);
				if (!sourceResult.IsOk())
				{
					m_lastFailure = sourceResult;
					return sourceResult;
				}
			}

			MacNativeBridgeRendererFrameInfo rendererFrameInfo{};
			Failure copyResult = Failure::Ok();
			uintptr_t sourceTextureObject = state.m_nativeAllocation->m_rendererIntermediateTextureObject;
			if (rendererSource.m_kind == MacRendererFrameSourceKind::RendererOwnedMetalTexture)
			{
				sourceTextureObject = rendererSource.m_textureObject;
				copyResult = CopyMacRendererIntermediateToProducerTexture(state.m_nativeAllocation->m_producerDeviceObject, sourceTextureObject, state.m_nativeAllocation->m_producerTextureObject, state.m_nativeAllocation->m_plane.m_width, state.m_nativeAllocation->m_plane.m_height, rendererFrameInfo, rendererSource.m_crossApiSharedEventObject, rendererSource.m_crossApiAcquireValue);
				if (rendererSource.m_releaseTextureObjectAfterUse)
				{
					ReleaseMacExportedTexture(sourceTextureObject);
					rendererSource.m_textureObject = 0;
				}
			}
			else if (rendererSource.m_kind == MacRendererFrameSourceKind::RendererOwnedRenderTargetMetadata && rendererSource.m_cpuBytes && !rendererSource.m_cpuBytes->empty())
			{
				copyResult = UploadMacRendererBytesToProducerTexture(state.m_nativeAllocation->m_producerTextureObject, state.m_nativeAllocation->m_plane.m_width, state.m_nativeAllocation->m_plane.m_height, rendererSource.m_cpuBytes->data(), rendererSource.m_bytesPerRow, rendererFrameInfo);
			}
			else
			{
				MacNativeBridgeProducerPattern pattern{};
				pattern.m_viewportId = state.m_key.m_viewportId;
				pattern.m_epoch = state.m_key.m_epoch;
				pattern.m_generation = state.m_key.m_generation;
				pattern.m_frameIndex = nextFrameIndex;
				pattern.m_width = state.m_viewport.m_width;
				pattern.m_height = state.m_viewport.m_height;
				auto uploadResult = UploadMacRendererPatternToIntermediateTexture(state.m_nativeAllocation->m_rendererIntermediateTextureObject, state.m_nativeAllocation->m_plane.m_width, state.m_nativeAllocation->m_plane.m_height, pattern);
				if (!uploadResult.IsOk())
				{
					m_lastFailure = uploadResult;
					return uploadResult;
				}
				if (!rendererSource.IsValid())
				{
					rendererSource.m_kind = MacRendererFrameSourceKind::SyntheticIntermediate;
					rendererSource.m_textureObject = state.m_nativeAllocation->m_rendererIntermediateTextureObject;
					rendererSource.m_sourceToken = nextFrameIndex;
					rendererSource.m_width = state.m_viewport.m_width;
					rendererSource.m_height = state.m_viewport.m_height;
					rendererSource.m_pixelFormat = state.m_viewport.m_pixelFormat;
					rendererSource.m_debugName = "SyntheticIntermediate";
				}
				copyResult = CopyMacRendererIntermediateToProducerTexture(state.m_nativeAllocation->m_producerDeviceObject, state.m_nativeAllocation->m_rendererIntermediateTextureObject, state.m_nativeAllocation->m_producerTextureObject, state.m_nativeAllocation->m_plane.m_width, state.m_nativeAllocation->m_plane.m_height, rendererFrameInfo);
			}
			if (rendererSource.m_crossApiSharedEventObject != 0)
			{
#if defined(__APPLE__)
				CFRelease(reinterpret_cast<CFTypeRef>(rendererSource.m_crossApiSharedEventObject));
#endif
				rendererSource.m_crossApiSharedEventObject = 0;
			}
			if (!copyResult.IsOk())
			{
				m_lastFailure = copyResult;
				return copyResult;
			}

			state.m_nativeAllocation->m_lastWrittenFrameIndex = nextFrameIndex;
			state.m_nativeAllocation->m_lastRendererTextureToken = rendererSource.m_sourceToken != 0 ? rendererSource.m_sourceToken : rendererFrameInfo.m_rendererTextureToken;
			state.m_nativeAllocation->m_lastProducerCopyToken = rendererFrameInfo.m_producerCopyToken;
			state.m_nativeAllocation->m_lastCrossApiAcquireValue = rendererSource.m_crossApiAcquireValue;
			state.m_nativeAllocation->m_lastRendererSource = rendererSource;
			if (state.m_lastExport.has_value())
			{
				state.m_lastExport->m_lastCrossApiAcquireValue = rendererSource.m_crossApiAcquireValue;
				state.m_lastExport->m_sharedEventObject = rendererSource.m_crossApiSharedEventObject;
				state.m_lastExport->m_handle.m_sharedEventObject = rendererSource.m_crossApiSharedEventObject;
				state.m_lastExport->m_crossApiSyncKind = rendererSource.m_crossApiSyncKind;
				state.m_lastExport->m_crossApiCpuWaited = rendererSource.m_crossApiCpuWaited;
			}
			if (!state.m_transport.m_macSurfaces.empty())
			{
				state.m_transport.m_macSurfaces.front().m_sharedEventObject = rendererSource.m_crossApiSharedEventObject;
			}
			state.m_frameBegun = true;
			m_liveAllocations[state.m_key] = *state.m_nativeAllocation;
			m_lastFailure = Failure::Ok();
			return Failure::Ok();
		}

		Failure ExportFrame(MacViewportSurfaceState& state, FramePacket& outFrame) override
		{
			if (!state.m_nativeAllocation.has_value() || !state.m_lastExport.has_value())
			{
				m_lastFailure = Failure::FromDomain(ErrorDomain::Session, 1003, "macOS frame export requires IOSurface ownership metadata");
				return m_lastFailure;
			}

			auto& exportMetadata = *state.m_lastExport;
			const auto frameIndex = ++state.m_lastExportedFrameIndex;
			const uint64_t timelineValue = ++m_nextTimelineValue;
			exportMetadata.m_lastAcquireValue = timelineValue;
			exportMetadata.m_lastReleaseValue = timelineValue;

			outFrame.m_viewportId = state.m_key.m_viewportId;
			outFrame.m_connectionEpoch = state.m_key.m_epoch;
			outFrame.m_generation = state.m_key.m_generation;
			outFrame.m_frameIndex = frameIndex;
			outFrame.m_width = state.m_viewport.m_width;
			outFrame.m_height = state.m_viewport.m_height;
			outFrame.m_timestampNs = timelineValue;
			outFrame.m_sync.m_acquireValue = exportMetadata.m_lastAcquireValue;
			outFrame.m_sync.m_releaseValue = exportMetadata.m_lastReleaseValue;
			outFrame.m_sync.m_crossApiAcquireValue = exportMetadata.m_lastCrossApiAcquireValue;
			outFrame.m_sync.m_crossApiSyncKind = exportMetadata.m_crossApiSyncKind;
			outFrame.m_sync.m_requiresExplicitRelease = exportMetadata.m_requiresHostRelease;
			outFrame.m_sync.m_crossApiCpuWaited = exportMetadata.m_crossApiCpuWaited;
			state.m_frameBegun = false;
			m_lastFailure = Failure::Ok();
			return Failure::Ok();
		}

		Failure ReleaseSurface(const MacViewportSurfaceState& state) override
		{
#if defined(__APPLE__)
			if (auto it = m_liveAllocations.find(state.m_key); it != m_liveAllocations.end())
			{
				ReleaseMacRendererIntermediateTexture(it->second.m_rendererIntermediateTextureObject);
				ReleaseMacIOSurfaceProducerTexture(it->second.m_producerDeviceObject, it->second.m_producerTextureObject);
				if (it->second.m_surfaceObject != 0)
				{
					CFRelease(reinterpret_cast<IOSurfaceRef>(it->second.m_surfaceObject));
				}
			}
#endif
			m_liveAllocations.erase(state.m_key);
			m_lastFailure = Failure::Ok();
			return Failure::Ok();
		}

		Failure GetLastFailure() const override
		{
			return m_lastFailure;
		}

		const MacIOSurfaceAllocation* FindAllocation(const MacViewportSurfaceKey& key) const
		{
			auto it = m_liveAllocations.find(key);
			return it != m_liveAllocations.end() ? &it->second : nullptr;
		}

		size_t GetLiveAllocationCount() const { return m_liveAllocations.size(); }

	private:
		std::unordered_map<MacViewportSurfaceKey, MacIOSurfaceAllocation, MacViewportSurfaceKeyHasher> m_liveAllocations{};
		IMacRendererFrameSourceProvider* m_rendererFrameSourceProvider = nullptr;
		Failure m_lastFailure = Failure::Ok();
		uint32_t m_nextSurfaceId = 100;
		uint64_t m_nextAllocationToken = 0;
		uint64_t m_nextExportToken = 0;
		uint64_t m_nextTimelineValue = 0;
	};

	class MacViewportTransportBackend : public IViewportTransportBackend
	{
	public:
		explicit MacViewportTransportBackend(IMacIOSurfaceProvider& provider) :
			m_provider(provider)
		{
		}

		Failure EnsureSurface(const ViewportDescriptor& viewport, ConnectionEpoch epoch, SurfaceGeneration generation, TransportDescriptor& outTransport) override
		{
			MacViewportSurfaceState state{};
			state.m_key = { viewport.m_viewportId, epoch, generation };
			state.m_viewport = viewport;
			state.m_transport.m_transportType = TransportType::MacIOSurface;
			state.m_transport.m_syncMode = SyncMode::ExplicitFence;
			state.m_transport.m_protocolVersion = 1;
			state.m_transport.m_width = viewport.m_width;
			state.m_transport.m_height = viewport.m_height;
			state.m_transport.m_pixelFormat = viewport.m_pixelFormat;
			state.m_transport.m_usageFlags = viewport.m_usageFlags;
			state.m_transport.m_ready = false;

			auto result = m_provider.CreateOrResizeSurface(viewport, epoch, generation, state);
			if (!result.IsOk())
			{
				m_lastFailure = m_provider.GetLastFailure();
				return result;
			}

			state.m_transport.m_transportType = TransportType::MacIOSurface;
			state.m_transport.m_ready = true;
			result = state.m_transport.Validate();
			if (!result.IsOk())
			{
				m_lastFailure = result;
				return result;
			}

			outTransport = state.m_transport;
			m_surfaces[state.m_key] = state;
			m_lastFailure = Failure::Ok();
			return Failure::Ok();
		}

		Failure BeginFrame(const ViewportDescriptor& viewport, ConnectionEpoch epoch, SurfaceGeneration generation) override
		{
			auto* state = FindSurface(viewport.m_viewportId, epoch, generation);
			if (!state)
			{
				m_lastFailure = Failure::FromDomain(ErrorDomain::Session, 2, "Missing macOS transport surface for frame begin");
				return m_lastFailure;
			}

			auto result = m_provider.BeginFrame(*state);
			if (!result.IsOk())
			{
				m_lastFailure = m_provider.GetLastFailure();
				return result;
			}

			state->m_frameBegun = true;
			m_lastFailure = Failure::Ok();
			return Failure::Ok();
		}

		Failure ExportFrame(const ViewportDescriptor& viewport, ConnectionEpoch epoch, SurfaceGeneration generation, FramePacket& outFrame) override
		{
			auto* state = FindSurface(viewport.m_viewportId, epoch, generation);
			if (!state)
			{
				m_lastFailure = Failure::FromDomain(ErrorDomain::Session, 2, "Missing macOS transport surface for frame export");
				return m_lastFailure;
			}
			if (!state->m_frameBegun)
			{
				m_lastFailure = Failure::FromDomain(ErrorDomain::Protocol, 1, "macOS transport export requires BeginFrame first");
				return m_lastFailure;
			}

			auto result = m_provider.ExportFrame(*state, outFrame);
			if (!result.IsOk())
			{
				m_lastFailure = m_provider.GetLastFailure();
				return result;
			}

			state->m_lastExportedFrameIndex = outFrame.m_frameIndex;
			state->m_frameBegun = false;
			m_lastFailure = Failure::Ok();
			return Failure::Ok();
		}

		Failure ReleaseSurface(ViewportId viewportId, ConnectionEpoch epoch, SurfaceGeneration generation) override
		{
			MacViewportSurfaceKey key{ viewportId, epoch, generation };
			auto it = m_surfaces.find(key);
			if (it == m_surfaces.end())
			{
				m_lastFailure = Failure::Ok();
				return Failure::Ok();
			}

			auto result = m_provider.ReleaseSurface(it->second);
			if (!result.IsOk())
			{
				m_lastFailure = m_provider.GetLastFailure();
				return result;
			}

			m_surfaces.erase(it);
			m_lastFailure = Failure::Ok();
			return Failure::Ok();
		}

		Failure GetLastFailure() const override
		{
			return m_lastFailure;
		}

		const MacViewportSurfaceState* FindSurface(ViewportId viewportId, ConnectionEpoch epoch, SurfaceGeneration generation) const
		{
			MacViewportSurfaceKey key{ viewportId, epoch, generation };
			auto it = m_surfaces.find(key);
			return it != m_surfaces.end() ? &it->second : nullptr;
		}

		size_t GetSurfaceCount() const { return m_surfaces.size(); }

	private:
		MacViewportSurfaceState* FindSurface(ViewportId viewportId, ConnectionEpoch epoch, SurfaceGeneration generation)
		{
			MacViewportSurfaceKey key{ viewportId, epoch, generation };
			auto it = m_surfaces.find(key);
			return it != m_surfaces.end() ? &it->second : nullptr;
		}

		IMacIOSurfaceProvider& m_provider;
		std::unordered_map<MacViewportSurfaceKey, MacViewportSurfaceState, MacViewportSurfaceKeyHasher> m_surfaces{};
		Failure m_lastFailure = Failure::Ok();
	};

	class IMacViewportPresenter
	{
	public:
		virtual ~IMacViewportPresenter() = default;
		virtual void BindHostHandle(ViewportId viewportId, const MacNativeHostHandle& hostHandle) = 0;
		virtual Failure ImportSurface(const ViewportDescriptor& viewport, const TransportDescriptor& transport, ConnectionEpoch epoch, SurfaceGeneration generation) = 0;
		virtual Failure PresentFrame(ViewportId viewportId, const FramePacket& frame) = 0;
		virtual void ResetViewport(ViewportId viewportId) = 0;
		virtual Failure GetLastFailure() const = 0;
	};

	class MacLoopbackViewportPresenter : public IMacViewportPresenter
	{
	public:
		void BindHostHandle(ViewportId viewportId, const MacNativeHostHandle& hostHandle) override
		{
			if (hostHandle.IsValid())
			{
				m_hostHandles[viewportId] = hostHandle;
			}
			else
			{
				m_hostHandles.erase(viewportId);
			}

			auto it = m_importedStates.find(viewportId);
			if (it != m_importedStates.end())
			{
				it->second.m_hostHandle = hostHandle;
				RefreshNativeLayerBinding(it->second);
			}
		}

		Failure ImportSurface(const ViewportDescriptor& viewport, const TransportDescriptor& transport, ConnectionEpoch epoch, SurfaceGeneration generation) override
		{
			if (transport.m_transportType != TransportType::MacIOSurface || transport.m_macSurfaces.empty())
			{
				m_lastFailure = Failure::FromDomain(ErrorDomain::Capability, 2001, "macOS presenter requires IOSurface transport metadata");
				return m_lastFailure;
			}

			const auto& handle = transport.m_macSurfaces.front();
			MacNativePresentationState state{};
			state.m_viewportId = viewport.m_viewportId;
			state.m_epoch = epoch;
			state.m_generation = generation;
			state.m_registryId = handle.m_registryId;
			state.m_importToken = ++m_nextImportToken;
			state.m_nativeLayerToken = ++m_nextLayerToken;
			state.m_currentDrawableToken = 0;
			state.m_presentedFrameCount = 0;
			state.m_width = transport.m_width;
			state.m_height = transport.m_height;
			state.m_pixelFormat = transport.m_pixelFormat;
			state.m_framebufferOnly = handle.m_framebufferOnly;
			state.m_importedSurface = handle;
			if (auto hostIt = m_hostHandles.find(viewport.m_viewportId); hostIt != m_hostHandles.end())
			{
				state.m_hostHandle = hostIt->second;
			}
			if (!state.IsValid())
			{
				m_lastFailure = Failure::FromDomain(ErrorDomain::Transport, 2002, "macOS presenter failed to materialize native host presentation state");
				return m_lastFailure;
			}

			auto bindingResult = RefreshNativeLayerBinding(state);
			if (!bindingResult.IsOk())
			{
				m_lastFailure = bindingResult;
				return bindingResult;
			}

			m_importedStates[viewport.m_viewportId] = state;
			m_lastFailure = Failure::Ok();
			return Failure::Ok();
		}

		Failure PresentFrame(ViewportId viewportId, const FramePacket& frame) override
		{
			auto it = m_importedStates.find(viewportId);
			if (it == m_importedStates.end())
			{
				m_lastFailure = Failure::FromDomain(ErrorDomain::Session, 2003, "macOS presenter has no imported viewport state to present into");
				return m_lastFailure;
			}

			auto& state = it->second;
			if (state.m_epoch != frame.m_connectionEpoch || state.m_generation != frame.m_generation)
			{
				m_lastFailure = Failure::FromDomain(ErrorDomain::Session, 2004, "macOS presenter rejected frame for stale imported generation");
				return m_lastFailure;
			}

			if (state.m_layerBinding.has_value())
			{
				MacNativeBridgePresentResult nativePresent{};
				auto nativeResult = PresentMacNativeLayerFrame(*state.m_layerBinding, state.m_importedSurface.value_or(MacIOSurfaceHandle{}), frame, nativePresent);
				if (!nativeResult.IsOk())
				{
					m_lastFailure = nativeResult;
					return nativeResult;
				}

				state.m_currentDrawableToken = nativePresent.m_presentToken;
				state.m_nativeLayerToken = state.m_layerBinding->m_bindingToken;
				state.m_usesRealCAMetalLayer = nativePresent.m_usedRealCAMetalLayer;
			}
			else
			{
				state.m_currentDrawableToken = ++m_nextDrawableToken;
				state.m_usesRealCAMetalLayer = false;
			}

			state.m_presentedFrameCount++;
			m_lastPresentedFrame = frame;
			m_lastFailure = Failure::Ok();
			return Failure::Ok();
		}

		void ResetViewport(ViewportId viewportId) override
		{
			auto it = m_importedStates.find(viewportId);
			if (it != m_importedStates.end() && it->second.m_layerBinding.has_value())
			{
				ResetMacNativeLayerBinding(*it->second.m_layerBinding);
			}
			m_importedStates.erase(viewportId);
			if (m_lastPresentedFrame.has_value() && m_lastPresentedFrame->m_viewportId == viewportId)
			{
				m_lastPresentedFrame.reset();
			}
		}

		Failure GetLastFailure() const override
		{
			return m_lastFailure;
		}

		const MacNativePresentationState* FindImportedState(ViewportId viewportId) const
		{
			auto it = m_importedStates.find(viewportId);
			return it != m_importedStates.end() ? &it->second : nullptr;
		}

		const std::optional<FramePacket>& GetLastPresentedFrame() const { return m_lastPresentedFrame; }

	private:
		Failure RefreshNativeLayerBinding(MacNativePresentationState& state)
		{
			if (!state.m_hostHandle.IsValid())
			{
				if (state.m_layerBinding.has_value())
				{
					ResetMacNativeLayerBinding(*state.m_layerBinding);
					state.m_layerBinding.reset();
				}
				state.m_usesRealCAMetalLayer = false;
				return Failure::Ok();
			}

			MacNativeLayerBinding binding = state.m_layerBinding.value_or(MacNativeLayerBinding{});
			auto result = BindMacNativeLayer(state.m_hostHandle, state.m_width, state.m_height, state.m_pixelFormat, binding);
			if (!result.IsOk())
			{
				return result;
			}

			state.m_layerBinding = binding;
			state.m_nativeLayerToken = binding.m_bindingToken;
			state.m_usesRealCAMetalLayer = true;
			return Failure::Ok();
		}

		std::unordered_map<ViewportId, MacNativeHostHandle> m_hostHandles{};
		std::unordered_map<ViewportId, MacNativePresentationState> m_importedStates{};
		std::optional<FramePacket> m_lastPresentedFrame{};
		Failure m_lastFailure = Failure::Ok();
		uint64_t m_nextImportToken = 0;
		uint64_t m_nextLayerToken = 0;
		uint64_t m_nextDrawableToken = 0;
	};

	class MacViewportNativeHost : public IEditorViewportHost
	{
	public:
		explicit MacViewportNativeHost(IMacViewportPresenter& presenter) :
			m_presenter(presenter)
		{
		}

		void BindNativeHostHandle(ViewportId viewportId, const MacNativeHostHandle& hostHandle)
		{
			m_presenter.BindHostHandle(viewportId, hostHandle);
		}

		Failure ImportTransport(const ViewportDescriptor& viewport, const TransportDescriptor& transport, ConnectionEpoch epoch, SurfaceGeneration generation) override
		{
			if (transport.m_transportType != TransportType::MacIOSurface)
			{
				m_lastFailure = Failure::FromDomain(ErrorDomain::Capability, 1, "macOS host supports MacIOSurface transport only");
				return m_lastFailure;
			}

			auto validation = transport.Validate();
			if (!validation.IsOk())
			{
				m_lastFailure = validation;
				return validation;
			}

			auto result = m_presenter.ImportSurface(viewport, transport, epoch, generation);
			if (!result.IsOk())
			{
				m_lastFailure = m_presenter.GetLastFailure();
				return result;
			}

			m_importedViewport = viewport.m_viewportId;
			m_importedEpoch = epoch;
			m_importedGeneration = generation;
			m_lastAcceptedFrame.reset();
			m_lastFailure = Failure::Ok();
			return Failure::Ok();
		}

		Failure AcceptFrame(const FramePacket& frame) override
		{
			if (!m_importedViewport.has_value() || *m_importedViewport != frame.m_viewportId)
			{
				m_lastFailure = Failure::FromDomain(ErrorDomain::Session, 2, "Frame rejected because no matching macOS viewport is imported");
				return m_lastFailure;
			}
			if (frame.m_connectionEpoch != m_importedEpoch || frame.m_generation != m_importedGeneration)
			{
				m_lastFailure = Failure::FromDomain(ErrorDomain::Session, 2, "Frame rejected because imported macOS transport generation is stale");
				return m_lastFailure;
			}

			m_lastAcceptedFrame = frame;
			m_lastFailure = Failure::Ok();
			return Failure::Ok();
		}

		Failure PresentLatestFrame(ViewportId viewportId) override
		{
			if (!m_lastAcceptedFrame.has_value() || m_lastAcceptedFrame->m_viewportId != viewportId)
			{
				m_lastFailure = Failure::FromDomain(ErrorDomain::Transport, 1, "No accepted macOS frame is available for presentation");
				return m_lastFailure;
			}

			auto result = m_presenter.PresentFrame(viewportId, *m_lastAcceptedFrame);
			if (!result.IsOk())
			{
				m_lastFailure = m_presenter.GetLastFailure();
				return result;
			}

			m_lastFailure = Failure::Ok();
			return Failure::Ok();
		}

		void ResetViewport(ViewportId viewportId) override
		{
			if (m_importedViewport.has_value() && *m_importedViewport == viewportId)
			{
				m_importedViewport.reset();
				m_importedEpoch = 0;
				m_importedGeneration = 0;
				m_lastAcceptedFrame.reset();
			}
			m_presenter.ResetViewport(viewportId);
		}

		Failure GetLastFailure() const override
		{
			return m_lastFailure;
		}

	private:
		IMacViewportPresenter& m_presenter;
		std::optional<ViewportId> m_importedViewport{};
		ConnectionEpoch m_importedEpoch = 0;
		SurfaceGeneration m_importedGeneration = 0;
		std::optional<FramePacket> m_lastAcceptedFrame{};
		Failure m_lastFailure = Failure::Ok();
	};

	class MacViewportLoopbackBinding
	{
	public:
		MacViewportLoopbackBinding(ViewportDescriptor descriptor, IMacIOSurfaceProvider& provider, IMacViewportPresenter& presenter, ConnectionEpoch epoch = 1) :
			m_transportBackend(provider),
			m_host(presenter),
			m_runtimeSession(std::move(descriptor), epoch)
		{
		}

		RemoteViewportSession& GetRuntimeSession() { return m_runtimeSession; }
		const RemoteViewportSession& GetRuntimeSession() const { return m_runtimeSession; }
		MacViewportTransportBackend& GetTransportBackend() { return m_transportBackend; }
		const MacViewportTransportBackend& GetTransportBackend() const { return m_transportBackend; }
		MacViewportNativeHost& GetHost() { return m_host; }
		const MacViewportNativeHost& GetHost() const { return m_host; }

		Failure Create()
		{
			auto result = m_runtimeSession.BeginNegotiation();
			if (!result.IsOk())
			{
				return result;
			}

			m_visible = true;
			m_focused = false;
			m_created = true;
			return EnsureTransportImported();
		}

		Failure Resize(uint32_t width, uint32_t height)
		{
			const auto previousEpoch = m_runtimeSession.GetConnectionEpoch();
			const auto previousGeneration = m_runtimeSession.GetGeneration();
			auto descriptor = m_runtimeSession.GetDescriptor();
			descriptor.m_width = std::max(width, 1u);
			descriptor.m_height = std::max(height, 1u);
			auto result = m_runtimeSession.HandleResize(descriptor);
			if (!result.IsOk())
			{
				return result;
			}

			result = EnsureTransportImported();
			if (!result.IsOk())
			{
				return result;
			}

			return m_transportBackend.ReleaseSurface(m_runtimeSession.GetViewportId(), previousEpoch, previousGeneration);
		}

		Failure SetVisible(bool visible)
		{
			m_visible = visible;
			return m_runtimeSession.SetVisible(visible);
		}

		Failure SetFocused(bool focused)
		{
			m_focused = focused;
			InputPacket input{};
			input.m_viewportId = m_runtimeSession.GetViewportId();
			input.m_connectionEpoch = m_runtimeSession.GetConnectionEpoch();
			input.m_generation = m_runtimeSession.GetGeneration();
			input.m_kind = InputKind::Focus;
			input.m_focused = focused;
			input.m_timestampNs = ++m_inputTimestampNs;
			return m_runtimeSession.HandleInput(input);
		}

		Failure PumpFrame()
		{
			if (!m_created)
			{
				return Failure::FromDomain(ErrorDomain::Session, 1, "macOS loopback binding must be created before pumping frames");
			}
			if (m_runtimeSession.GetState() != SessionState::Active)
			{
				return Failure::Ok();
			}

			auto result = m_runtimeSession.PublishFrameFromBackend(m_transportBackend);
			if (!result.IsOk())
			{
				return result;
			}

			result = m_host.AcceptFrame(m_runtimeSession.GetLastFrame());
			if (!result.IsOk())
			{
				return result;
			}

			return m_host.PresentLatestFrame(m_runtimeSession.GetDescriptor().m_viewportId);
		}

		Failure Destroy()
		{
			if (!m_created)
			{
				return Failure::Ok();
			}

			auto release = m_runtimeSession.ReleaseBackendTransport(m_transportBackend);
			m_host.ResetViewport(m_runtimeSession.GetDescriptor().m_viewportId);
			auto destroy = m_runtimeSession.Destroy();
			m_created = false;
			return !release.IsOk() ? release : destroy;
		}

	private:
		Failure EnsureTransportImported()
		{
			auto result = m_runtimeSession.EnsureBackendTransport(m_transportBackend);
			if (!result.IsOk())
			{
				return result;
			}

			const auto* surface = std::as_const(m_transportBackend).FindSurface(
				m_runtimeSession.GetViewportId(),
				m_runtimeSession.GetConnectionEpoch(),
				m_runtimeSession.GetGeneration());
			if (!surface)
			{
				return Failure::FromDomain(ErrorDomain::Transport, 1, "macOS loopback binding could not resolve imported surface");
			}

			result = m_host.ImportTransport(
				m_runtimeSession.GetDescriptor(),
				surface->m_transport,
				m_runtimeSession.GetConnectionEpoch(),
				m_runtimeSession.GetGeneration());
			if (!result.IsOk())
			{
				return result;
			}

			if (!m_visible)
			{
				result = m_runtimeSession.SetVisible(false);
				if (!result.IsOk())
				{
					return result;
				}
			}
			if (m_focused)
			{
				result = SetFocused(true);
				if (!result.IsOk())
				{
					return result;
				}
			}

			return Failure::Ok();
		}

		MacViewportTransportBackend m_transportBackend;
		MacViewportNativeHost m_host;
		RemoteViewportSession m_runtimeSession;
		uint64_t m_inputTimestampNs = 0;
		bool m_created = false;
		bool m_visible = true;
		bool m_focused = false;
	};
}
