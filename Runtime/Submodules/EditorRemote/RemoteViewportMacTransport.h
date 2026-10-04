#pragma once
#include "Containers/Containers.h"
#include "Memory/SharedPtr.hpp"
#include "Memory/UniquePtr.hpp"
#include "RHI/Readback.h"

#include <algorithm>
#include <chrono>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#if defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#include <IOSurface/IOSurface.h>
#endif

#include "RemoteViewportBinding.h"
#include "RemoteViewportMacNativeBridge.h"
#include "RemoteViewportRuntime.h"

namespace Sailor::EditorRemote
{
	class IMacViewportPresenter;

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

		size_t GetHash() const noexcept
		{
			size_t seed = 0;
			HashCombine(seed, m_viewportId, m_epoch, m_generation);
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
		TSharedPtr<std::vector<uint8_t>> m_cpuBytes{};
		RHI::EditorReadbackFramePtr m_readback{};
		std::string m_debugName{};
		bool m_releaseTextureObjectAfterUse = false;
		bool m_crossApiCpuWaited = false;

		bool IsValid() const
		{
			return m_kind != MacRendererFrameSourceKind::Unknown && m_width != 0 && m_height != 0 && m_pixelFormat != PixelFormat::Unknown;
		}

		const uint8_t* GetCpuBytes() const
		{
			if (m_readback) return m_readback->GetBgraPixels();
			return m_cpuBytes && !m_cpuBytes->empty() ? m_cpuBytes->data() : nullptr;
		}

		auto operator<=>(const MacRendererFrameSource&) const = default;
	};

	struct MacIOSurfaceAllocation
	{
		MacIOSurfaceAllocation() = default;
#if defined(__APPLE__)
		~MacIOSurfaceAllocation();
#else
		~MacIOSurfaceAllocation() = default;
#endif
		MacIOSurfaceAllocation(const MacIOSurfaceAllocation&) = delete;
		MacIOSurfaceAllocation& operator=(const MacIOSurfaceAllocation&) = delete;

		uint32_t m_surfaceId = 0;
		uint64_t m_registryId = 0;
		uintptr_t m_surfaceObject = 0;
		uintptr_t m_producerDeviceObject = 0;
		uintptr_t m_producerTextureObject = 0;
		uintptr_t m_producerCommandQueueObject = 0;
		uintptr_t m_copyCommandBufferObject = 0;
		// Reader completion belongs to the surface, even when its host is rebound.
		uintptr_t m_presentCommandBufferObject = 0;
		uint64_t m_allocationToken = 0;
		uint64_t m_lastWrittenFrameIndex = 0;
		uint64_t m_lastRendererTextureToken = 0;
		uint64_t m_lastProducerCopyToken = 0;
		// Native writes can be newer than the last exported frame.
		uint64_t m_currentCopyToken = 0;
		// Uploaded pixel payload, excluding row padding.
		uint64_t m_cpuUploadedBytes = 0;
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
		uint64_t m_lastCrossApiAcquireValue = 0;
		uintptr_t m_sharedEventObject = 0;
		CrossApiSyncKind m_crossApiSyncKind = CrossApiSyncKind::None;
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
		FrameIndex m_lastPresentedFrameIndex = 0;
		FrameIndex m_evidenceFrameIndex = 0;
		uint64_t m_evidenceCaptureCount = 0;
		uint32_t m_width = 0;
		uint32_t m_height = 0;
		PixelFormat m_pixelFormat = PixelFormat::Unknown;
		bool m_framebufferOnly = false;
		MacNativeHostHandle m_hostHandle{};
		TUniquePtr<MacNativeLayerBinding> m_layerBinding{};
		TSharedPtr<MacIOSurfaceAllocation> m_nativeAllocation{};
		std::optional<MacIOSurfaceHandle> m_importedSurface{};
		MacNativeSurfaceFrameEvidence m_lastFrameEvidence{};
		bool m_hasFrameEvidence = false;
		bool m_usesRealCAMetalLayer = false;

		bool IsValid() const
		{
			return m_viewportId != 0 && m_epoch != 0 && m_generation != 0 && m_registryId != 0 && m_importToken != 0 &&
				m_nativeLayerToken != 0 && m_width != 0 && m_height != 0 && m_pixelFormat != PixelFormat::Unknown;
		}
	};

	struct MacViewportSurfaceState
	{
		MacViewportSurfaceKey m_key{};
		ViewportDescriptor m_viewport{};
		TransportDescriptor m_transport{};
		FrameIndex m_lastExportedFrameIndex = 0;
		bool m_frameBegun = false;
		bool m_needsHostReset = false;
		MacRendererFrameSource m_pendingRendererSource{};
		MacNativeBridgeRendererFrameInfo m_pendingFrameInfo{};
		TSharedPtr<MacIOSurfaceAllocation> m_nativeAllocation{};
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
		// No renderer frame leaves m_frameBegun false and is retried on the next pump.
		virtual Failure BeginFrame(MacViewportSurfaceState& state) = 0;
		virtual Failure PollFrameReady(MacViewportSurfaceState& state, bool& outReady) = 0;
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
			auto allocation = TSharedPtr<MacIOSurfaceAllocation>::Make();
			allocation->m_registryId = (epoch << 32ull) | generation;
			allocation->m_allocationToken = ++m_nextAllocationToken;
			allocation->m_pixelFormat = viewport.m_pixelFormat;
			allocation->m_colorSpace = viewport.m_colorSpace;
			allocation->m_usageFlags = viewport.m_usageFlags;
			allocation->m_framebufferOnly = false;
			allocation->m_debugLabel = viewport.m_debugName;
			allocation->m_plane.m_planeIndex = 0;
			allocation->m_plane.m_planeCount = 1;
			allocation->m_plane.m_width = viewport.m_width;
			allocation->m_plane.m_height = viewport.m_height;
			allocation->m_plane.m_bytesPerElement = 4u;
#if defined(__APPLE__)
			const uint32_t minimumStrideAlignment = std::max(GetMacIOSurfaceBytesPerRowAlignment(viewport.m_pixelFormat), allocation->m_plane.m_bytesPerElement);
			allocation->m_plane.m_bytesPerRow = AlignMacIOSurfaceStride(viewport.m_width * allocation->m_plane.m_bytesPerElement, minimumStrideAlignment);
#else
			allocation->m_plane.m_bytesPerRow = viewport.m_width * allocation->m_plane.m_bytesPerElement;
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
			setIntProperty(kIOSurfaceBytesPerElement, static_cast<int32_t>(allocation->m_plane.m_bytesPerElement));
			setIntProperty(kIOSurfaceBytesPerRow, static_cast<int32_t>(allocation->m_plane.m_bytesPerRow));
			setIntProperty(kIOSurfaceAllocSize, static_cast<int32_t>(allocation->m_plane.m_bytesPerRow * allocation->m_plane.m_height));
			setIntProperty(kIOSurfacePixelFormat, static_cast<int32_t>('BGRA'));

			IOSurfaceRef surface = IOSurfaceCreate(properties);
			releaseProperties();
			if (surface == nullptr)
			{
				m_lastFailure = Failure::FromDomain(ErrorDomain::Transport, 1001, "Failed to create real macOS IOSurface");
				return m_lastFailure;
			}

			allocation->m_surfaceObject = reinterpret_cast<uintptr_t>(surface);
			allocation->m_surfaceId = IOSurfaceGetID(surface);
			auto producerTextureResult = CreateMacIOSurfaceProducerTexture(*allocation);
			if (!producerTextureResult.IsOk())
			{
				m_lastFailure = producerTextureResult;
				return producerTextureResult;
			}
#else
			allocation->m_surfaceObject = allocation->m_registryId;
			allocation->m_surfaceId = ++m_nextSurfaceId;
#endif

			MacIOSurfaceExportMetadata exportMetadata{};
			exportMetadata.m_handle.m_surfaceId = allocation->m_surfaceId;
			exportMetadata.m_handle.m_registryId = allocation->m_registryId;
			exportMetadata.m_handle.m_surfaceObject = allocation->m_surfaceObject;
			exportMetadata.m_handle.m_sharedEventObject = 0;
			exportMetadata.m_handle.m_planeIndex = allocation->m_plane.m_planeIndex;
			exportMetadata.m_handle.m_planeCount = allocation->m_plane.m_planeCount;
			exportMetadata.m_handle.m_bytesPerRow = allocation->m_plane.m_bytesPerRow;
			exportMetadata.m_handle.m_bytesPerElement = allocation->m_plane.m_bytesPerElement;
			exportMetadata.m_handle.m_framebufferOnly = allocation->m_framebufferOnly;
			exportMetadata.m_exportToken = ++m_nextExportToken;

			if (!allocation->IsValid() || !exportMetadata.IsValid())
			{
				m_lastFailure = Failure::FromDomain(ErrorDomain::Transport, 1001, "Failed to materialize macOS IOSurface allocation metadata");
				return m_lastFailure;
			}

			inOutState.m_key = { viewport.m_viewportId, epoch, generation };
			inOutState.m_viewport = viewport;
			inOutState.m_transport.m_transportType = TransportType::MacIOSurface;
			inOutState.m_transport.m_syncMode = SyncMode::Implicit;
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
			m_liveAllocations[inOutState.m_key] = std::move(allocation);
			m_lastFailure = Failure::Ok();
			return Failure::Ok();
		}

		Failure BeginFrame(MacViewportSurfaceState& state) override
		{
			if (state.m_frameBegun)
			{
				m_lastFailure = Failure::Ok();
				return m_lastFailure;
			}
			if (!state.m_nativeAllocation || !state.m_nativeAllocation->IsValid())
			{
				m_lastFailure = Failure::FromDomain(ErrorDomain::Session, 1002, "macOS frame begin requires a live IOSurface allocation");
				return m_lastFailure;
			}

			bool readCompleted = false;
			m_lastFailure = PollMacIOSurfaceReadCompletion(*state.m_nativeAllocation, readCompleted);
			if (!m_lastFailure.IsOk()) return m_lastFailure;
			if (!readCompleted)
			{
				m_lastFailure = Failure::FromDomain(ErrorDomain::Session, 1034, "macOS IOSurface is still being read by the presenter");
				return m_lastFailure;
			}

			if (!m_rendererFrameSourceProvider)
			{
				m_lastFailure = Failure::Ok();
				return m_lastFailure;
			}

			MacRendererFrameSource rendererSource{};
			m_lastFailure = m_rendererFrameSourceProvider->AcquireFrameSource(state, state.m_lastExportedFrameIndex + 1, rendererSource);
			if (!m_lastFailure.IsOk())
			{
				ReleaseRendererFrameSourceResources(rendererSource);
				return m_lastFailure;
			}

			const bool bHasMetalTexture = rendererSource.m_textureObject != 0 &&
				(rendererSource.m_kind == MacRendererFrameSourceKind::RendererOwnedMetalTexture ||
				 rendererSource.m_kind == MacRendererFrameSourceKind::SyntheticIntermediate);
			const bool bHasCpuPayload = rendererSource.m_kind == MacRendererFrameSourceKind::RendererOwnedRenderTargetMetadata &&
				rendererSource.GetCpuBytes() != nullptr;
			if (!rendererSource.IsValid() || (!bHasMetalTexture && !bHasCpuPayload) ||
				rendererSource.m_width != state.m_viewport.m_width || rendererSource.m_height != state.m_viewport.m_height)
			{
				ReleaseRendererFrameSourceResources(rendererSource);
				return Failure::Ok();
			}

			if (bHasCpuPayload && rendererSource.m_bytesPerRow < state.m_nativeAllocation->m_plane.m_width * state.m_nativeAllocation->m_plane.m_bytesPerElement)
			{
				ReleaseRendererFrameSourceResources(rendererSource);
				m_lastFailure = Failure::FromDomain(ErrorDomain::Session, 1006, "macOS renderer source row stride is smaller than the current IOSurface row");
				return m_lastFailure;
			}

			MacNativeBridgeRendererFrameInfo rendererFrameInfo{};
			const auto& allocation = *state.m_nativeAllocation;
			const auto& previousSource = allocation.m_lastRendererSource;
			if (bHasCpuPayload && rendererSource.m_readback && rendererSource.m_readback == previousSource.m_readback &&
				rendererSource.m_bytesPerRow == previousSource.m_bytesPerRow && rendererSource.m_pixelFormat == previousSource.m_pixelFormat &&
				allocation.m_currentCopyToken != 0 && allocation.m_currentCopyToken == allocation.m_lastProducerCopyToken)
			{
				// Re-present immutable pixels, unless a later unexported write replaced them.
				rendererFrameInfo.m_rendererTextureToken = allocation.m_lastRendererTextureToken;
				rendererFrameInfo.m_producerCopyToken = allocation.m_lastProducerCopyToken;
			}
			else
			{
				m_lastFailure = bHasMetalTexture ?
					CopyMacRendererIntermediateToProducerTexture(*state.m_nativeAllocation, rendererSource.m_textureObject, rendererFrameInfo,
						rendererSource.m_crossApiSharedEventObject, rendererSource.m_crossApiAcquireValue) :
					UploadMacRendererBytesToProducerTexture(*state.m_nativeAllocation, rendererSource.GetCpuBytes(),
						rendererSource.m_bytesPerRow, rendererFrameInfo);
			}
			ReleaseRendererFrameSourceResources(rendererSource);
			if (!m_lastFailure.IsOk()) return m_lastFailure;

			state.m_pendingRendererSource = std::move(rendererSource);
			state.m_pendingFrameInfo = rendererFrameInfo;
			state.m_frameBegun = true;
			m_lastFailure = Failure::Ok();
			return Failure::Ok();
		}

		Failure PollFrameReady(MacViewportSurfaceState& state, bool& outReady) override
		{
			outReady = false;
			if (!state.m_frameBegun)
			{
				m_lastFailure = Failure::FromDomain(ErrorDomain::Protocol, 1, "macOS frame readiness requires BeginFrame first");
				return m_lastFailure;
			}
			m_lastFailure = PollMacIOSurfaceCopyCompletion(*state.m_nativeAllocation, outReady);
			if (!m_lastFailure.IsOk())
			{
				state.m_frameBegun = false;
				state.m_pendingRendererSource = {};
				state.m_pendingFrameInfo = {};
			}
			return m_lastFailure;
		}

		Failure ExportFrame(MacViewportSurfaceState& state, FramePacket& outFrame) override
		{
			if (!state.m_nativeAllocation || !state.m_lastExport.has_value())
			{
				m_lastFailure = Failure::FromDomain(ErrorDomain::Session, 1003, "macOS frame export requires IOSurface ownership metadata");
				return m_lastFailure;
			}

			bool ready = false;
			auto result = PollFrameReady(state, ready);
			if (!result.IsOk()) return result;
			if (!ready)
			{
				m_lastFailure = Failure::FromDomain(ErrorDomain::Session, 1036, "macOS producer copy is still pending");
				return m_lastFailure;
			}

			auto& exportMetadata = *state.m_lastExport;
			const auto frameIndex = ++state.m_lastExportedFrameIndex;
			auto& rendererSource = state.m_pendingRendererSource;
			state.m_nativeAllocation->m_lastWrittenFrameIndex = frameIndex;
			state.m_nativeAllocation->m_lastRendererTextureToken = rendererSource.m_sourceToken != 0 ? rendererSource.m_sourceToken : state.m_pendingFrameInfo.m_rendererTextureToken;
			state.m_nativeAllocation->m_lastProducerCopyToken = state.m_pendingFrameInfo.m_producerCopyToken;
			state.m_nativeAllocation->m_lastCrossApiAcquireValue = rendererSource.m_crossApiAcquireValue;
			exportMetadata.m_lastCrossApiAcquireValue = rendererSource.m_crossApiAcquireValue;
			exportMetadata.m_sharedEventObject = rendererSource.m_crossApiSharedEventObject;
			exportMetadata.m_handle.m_sharedEventObject = rendererSource.m_crossApiSharedEventObject;
			exportMetadata.m_crossApiSyncKind = rendererSource.m_crossApiSyncKind;
			exportMetadata.m_crossApiCpuWaited = rendererSource.m_crossApiCpuWaited;
			state.m_transport.m_macSurfaces.front().m_sharedEventObject = rendererSource.m_crossApiSharedEventObject;
			state.m_nativeAllocation->m_lastRendererSource = std::move(rendererSource);
			state.m_pendingRendererSource = {};
			state.m_pendingFrameInfo = {};

			outFrame.m_viewportId = state.m_key.m_viewportId;
			outFrame.m_connectionEpoch = state.m_key.m_epoch;
			outFrame.m_generation = state.m_key.m_generation;
			outFrame.m_frameIndex = frameIndex;
			outFrame.m_width = state.m_viewport.m_width;
			outFrame.m_height = state.m_viewport.m_height;
			outFrame.m_timestampNs = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
			outFrame.m_sync.m_acquireValue = 0;
			outFrame.m_sync.m_releaseValue = 0;
			outFrame.m_sync.m_crossApiAcquireValue = exportMetadata.m_lastCrossApiAcquireValue;
			outFrame.m_sync.m_crossApiSyncKind = exportMetadata.m_crossApiSyncKind;
			outFrame.m_sync.m_requiresExplicitRelease = false;
			outFrame.m_sync.m_crossApiCpuWaited = exportMetadata.m_crossApiCpuWaited;
			state.m_frameBegun = false;
			m_lastFailure = Failure::Ok();
			return Failure::Ok();
		}

		Failure ReleaseSurface(const MacViewportSurfaceState& state) override
		{
			const auto it = m_liveAllocations.Find(state.m_key);
			if (it != m_liveAllocations.end() && it.Value() == state.m_nativeAllocation)
			{
				m_liveAllocations.Remove(state.m_key);
			}
			m_lastFailure = Failure::Ok();
			return Failure::Ok();
		}

		Failure GetLastFailure() const override
		{
			return m_lastFailure;
		}

		const MacIOSurfaceAllocation* FindAllocation(const MacViewportSurfaceKey& key) const
		{
			auto it = m_liveAllocations.Find(key);
			return it != m_liveAllocations.end() ? it.Value().GetRawPtr() : nullptr;
		}

		size_t GetLiveAllocationCount() const { return m_liveAllocations.Num(); }

	private:
		static void ReleaseRendererFrameSourceResources(MacRendererFrameSource& rendererSource)
		{
			if (rendererSource.m_releaseTextureObjectAfterUse && rendererSource.m_textureObject != 0)
			{
				ReleaseMacExportedTexture(rendererSource.m_textureObject);
				rendererSource.m_textureObject = 0;
			}
			if (rendererSource.m_crossApiSharedEventObject != 0)
			{
#if defined(__APPLE__)
				CFRelease(reinterpret_cast<CFTypeRef>(rendererSource.m_crossApiSharedEventObject));
#endif
				rendererSource.m_crossApiSharedEventObject = 0;
			}
		}

		TMap<MacViewportSurfaceKey, TSharedPtr<MacIOSurfaceAllocation>> m_liveAllocations{};
		IMacRendererFrameSourceProvider* m_rendererFrameSourceProvider = nullptr;
		Failure m_lastFailure = Failure::Ok();
		uint32_t m_nextSurfaceId = 100;
		uint64_t m_nextAllocationToken = 0;
		uint64_t m_nextExportToken = 0;
	};

	class MacViewportTransportBackend : public IViewportTransportBackend
	{
	public:
		using Provider = IMacIOSurfaceProvider;
		Failure ImportSurface(IMacViewportPresenter& presenter, const ViewportDescriptor& viewport,
			const TransportDescriptor& transport, ConnectionEpoch epoch, SurfaceGeneration generation);

		explicit MacViewportTransportBackend(IMacIOSurfaceProvider& provider) :
			m_provider(provider)
		{
		}

		Failure EnsureSurface(const ViewportDescriptor& viewport, ConnectionEpoch epoch, SurfaceGeneration generation, TransportDescriptor& outTransport) override
		{
			auto release = ReleaseSurface(viewport.m_viewportId, epoch, generation);
			if (!release.IsOk()) return release;
			const MacViewportSurfaceKey key{ viewport.m_viewportId, epoch, generation };
			auto& storedState = m_surfaces[key];
			storedState = TUniquePtr<MacViewportSurfaceState>::Make();
			auto& state = *storedState;
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
				ReleaseSurface(viewport.m_viewportId, epoch, generation);
				m_lastFailure = result;
				return result;
			}

			state.m_transport.m_transportType = TransportType::MacIOSurface;
			state.m_transport.m_ready = true;
			result = state.m_transport.Validate();
			if (result.IsOk() && (state.m_transport.m_width != viewport.m_width ||
				state.m_transport.m_height != viewport.m_height || state.m_transport.m_pixelFormat != viewport.m_pixelFormat))
			{
				result = Failure::FromDomain(ErrorDomain::Protocol, 1, "Transport does not match its viewport");
			}
			if (!result.IsOk())
			{
				ReleaseSurface(viewport.m_viewportId, epoch, generation);
				m_lastFailure = result;
				return result;
			}

			outTransport = state.m_transport;
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

			if (state->m_frameBegun)
			{
				m_lastFailure = Failure::Ok();
				return m_lastFailure;
			}

			auto result = m_provider.BeginFrame(*state);
			if (!result.IsOk())
			{
				m_lastFailure = m_provider.GetLastFailure();
				return result;
			}

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
			auto it = m_surfaces.Find(key);
			if (it == m_surfaces.end())
			{
				m_lastFailure = Failure::Ok();
				return Failure::Ok();
			}

			auto result = m_provider.ReleaseSurface(*it.Value());
			if (!result.IsOk())
			{
				m_lastFailure = m_provider.GetLastFailure();
				return result;
			}

			m_surfaces.Remove(key);
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
			auto it = m_surfaces.Find(key);
			return it != m_surfaces.end() ? it.Value().GetRawPtr() : nullptr;
		}

		size_t GetSurfaceCount() const { return m_surfaces.Num(); }

		Failure ReleaseSurfaces(ViewportId viewportId, ConnectionEpoch keepEpoch = 0, SurfaceGeneration keepGeneration = 0)
		{
			Failure failure = Failure::Ok();
			for (const auto& key : m_surfaces.GetKeys())
			{
				if (key.m_viewportId == viewportId && (key.m_epoch != keepEpoch || key.m_generation != keepGeneration))
				{
					auto result = ReleaseSurface(key.m_viewportId, key.m_epoch, key.m_generation);
					if (!result.IsOk()) failure = result;
				}
			}
			m_lastFailure = failure;
			return failure;
		}

		Failure PrepareFrame(const ViewportDescriptor& viewport, ConnectionEpoch epoch, SurfaceGeneration generation, bool& outReady)
		{
			outReady = false;
			auto* state = FindSurface(viewport.m_viewportId, epoch, generation);
			if (!state)
			{
				m_lastFailure = Failure::FromDomain(ErrorDomain::Session, 2, "Missing macOS transport surface for frame begin");
				return m_lastFailure;
			}
			bool readCompleted = true;
			m_lastFailure = state->m_nativeAllocation ? PollMacIOSurfaceReadCompletion(*state->m_nativeAllocation, readCompleted) : Failure::Ok();
			if (!m_lastFailure.IsOk() || !readCompleted) return m_lastFailure;
			auto result = BeginFrame(viewport, epoch, generation);
			if (!result.IsOk() || !state->m_frameBegun) return result;
			m_lastFailure = m_provider.PollFrameReady(*state, outReady);
			return m_lastFailure;
		}

	private:
		MacViewportSurfaceState* FindSurface(ViewportId viewportId, ConnectionEpoch epoch, SurfaceGeneration generation)
		{
			MacViewportSurfaceKey key{ viewportId, epoch, generation };
			auto it = m_surfaces.Find(key);
			return it != m_surfaces.end() ? it.Value().GetRawPtr() : nullptr;
		}

		IMacIOSurfaceProvider& m_provider;
		TMap<MacViewportSurfaceKey, TUniquePtr<MacViewportSurfaceState>> m_surfaces{};
		Failure m_lastFailure = Failure::Ok();
	};

	class IMacViewportPresenter
	{
	public:
		virtual ~IMacViewportPresenter() = default;
		virtual void BindHostHandle(ViewportId viewportId, const MacNativeHostHandle& hostHandle) = 0;
		virtual Failure ImportSurface(const ViewportDescriptor& viewport, const TransportDescriptor& transport, ConnectionEpoch epoch, SurfaceGeneration generation, const TSharedPtr<MacIOSurfaceAllocation>& allocation = {}) = 0;
		virtual Failure PresentFrame(ViewportId viewportId, const FramePacket& frame) = 0;
		virtual void ResetViewport(ViewportId viewportId) = 0;
		virtual Failure GetLastFailure() const = 0;
	};

	class MacLoopbackViewportPresenter : public IMacViewportPresenter
	{
	public:
		void BindHostHandle(ViewportId viewportId, const MacNativeHostHandle& hostHandle) override
		{
			m_lastFailure = Failure::Ok();
			const auto currentHandle = m_hostHandles.Find(viewportId);
			const auto currentState = m_importedStates.Find(viewportId);
			if (currentHandle != m_hostHandles.end() &&
				currentHandle.Value() == hostHandle)
			{
				if (currentState == m_importedStates.end())
				{
					return;
				}

				const auto& state = *currentState.Value();
				const bool bHasExpectedLayer =
					hostHandle.IsValid()
						? state.m_layerBinding &&
							state.m_layerBinding->IsValid() &&
							state.m_usesRealCAMetalLayer
						: !state.m_layerBinding;
				if (state.m_hostHandle == hostHandle &&
					bHasExpectedLayer)
				{
					return;
				}
			}

			if (hostHandle.IsValid())
			{
				m_hostHandles[viewportId] = hostHandle;
			}
			else
			{
				m_hostHandles.Remove(viewportId);
			}

			auto it = m_importedStates.Find(viewportId);
			if (it != m_importedStates.end())
			{
				m_lastFailure = RefreshNativeLayerBinding(*it.Value(), hostHandle);
			}
		}

		Failure ImportSurface(const ViewportDescriptor& viewport, const TransportDescriptor& transport, ConnectionEpoch epoch, SurfaceGeneration generation, const TSharedPtr<MacIOSurfaceAllocation>& allocation = {}) override
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
			state.m_nativeAllocation = allocation;
			if (auto hostIt = m_hostHandles.Find(viewport.m_viewportId); hostIt != m_hostHandles.end())
			{
				state.m_hostHandle = hostIt.Value();
			}
			if (!state.IsValid())
			{
				m_lastFailure = Failure::FromDomain(ErrorDomain::Transport, 2002, "macOS presenter failed to materialize native host presentation state");
				return m_lastFailure;
			}

			auto bindingResult = RefreshNativeLayerBinding(state, state.m_hostHandle);
			if (!bindingResult.IsOk())
			{
				m_lastFailure = bindingResult;
				return bindingResult;
			}

			auto& storedState = m_importedStates[viewport.m_viewportId];
			if (storedState)
			{
				*storedState = std::move(state);
			}
			else
			{
				storedState = TUniquePtr<MacNativePresentationState>::Make(std::move(state));
			}
			m_lastFailure = Failure::Ok();
			return Failure::Ok();
		}

		Failure PresentFrame(ViewportId viewportId, const FramePacket& frame) override
		{
			auto it = m_importedStates.Find(viewportId);
			if (it == m_importedStates.end())
			{
				m_lastFailure = Failure::FromDomain(ErrorDomain::Session, 2003, "macOS presenter has no imported viewport state to present into");
				return m_lastFailure;
			}

			auto& state = *it.Value();
			if (frame.m_viewportId != viewportId || state.m_epoch != frame.m_connectionEpoch || state.m_generation != frame.m_generation)
			{
				m_lastFailure = Failure::FromDomain(ErrorDomain::Session, 2004, "macOS presenter rejected frame for stale imported generation");
				return m_lastFailure;
			}

			if (state.m_layerBinding)
			{
				MacNativeBridgePresentResult nativePresent{};
				auto nativeResult = PresentMacNativeLayerFrame(*state.m_layerBinding, state.m_importedSurface.value_or(MacIOSurfaceHandle{}), frame, nativePresent, state.m_nativeAllocation.GetRawPtr());
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
			state.m_lastPresentedFrameIndex = frame.m_frameIndex;
			m_lastPresentedFrame = frame;
			m_lastFailure = Failure::Ok();
			return Failure::Ok();
		}

		Failure CaptureFrameEvidence(ViewportId viewportId)
		{
			auto it = m_importedStates.Find(viewportId);
			if (it == m_importedStates.end() || !it.Value()->m_importedSurface || it.Value()->m_lastPresentedFrameIndex == 0)
			{
				return Failure::FromDomain(ErrorDomain::Session, 2125, "macOS viewport has no presented frame to capture");
			}
			auto& state = *it.Value();
			if (state.m_nativeAllocation && (state.m_nativeAllocation->m_copyCommandBufferObject != 0 ||
				state.m_nativeAllocation->m_currentCopyToken != state.m_nativeAllocation->m_lastProducerCopyToken ||
				state.m_nativeAllocation->m_lastWrittenFrameIndex != state.m_lastPresentedFrameIndex))
			{
				return Failure::FromDomain(ErrorDomain::Session, 2126, "macOS viewport producer frame is pending presentation");
			}

			MacNativeSurfaceFrameEvidence evidence;
			++state.m_evidenceCaptureCount;
			auto result = CaptureMacIOSurfaceFrameEvidence(*state.m_importedSurface, state.m_width, state.m_height,
				state.m_pixelFormat, evidence);
			if (result.IsOk())
			{
				state.m_lastFrameEvidence = evidence;
				state.m_evidenceFrameIndex = state.m_lastPresentedFrameIndex;
				state.m_hasFrameEvidence = true;
			}
			return result;
		}

		void ResetViewport(ViewportId viewportId) override
		{
			m_importedStates.Remove(viewportId);
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
			auto it = m_importedStates.Find(viewportId);
			return it != m_importedStates.end() ? it.Value().GetRawPtr() : nullptr;
		}

		std::string BuildViewportSummary(ViewportId viewportId) const
		{
			auto it = m_importedStates.Find(viewportId);
			if (it == m_importedStates.end())
			{
				return {};
			}

			const auto& state = *it.Value();
			std::ostringstream ss;
			ss << "nativeLayer=" << (state.m_usesRealCAMetalLayer ? 1 : 0)
				<< " host=" << static_cast<uint32_t>(state.m_hostHandle.m_kind)
				<< " presentCount=" << state.m_presentedFrameCount
				<< " captureCount=" << state.m_evidenceCaptureCount
				<< " drawableToken=" << state.m_currentDrawableToken
				<< " size=" << state.m_width << "x" << state.m_height;
			if (state.m_hasFrameEvidence)
			{
				const auto& evidence = state.m_lastFrameEvidence;
				const bool hasNonBlackEvidence = evidence.m_nonBlackPixelCount != 0;
				const uint32_t nonBlackPct = evidence.m_sampledPixelCount != 0 ? (evidence.m_nonBlackPixelCount * 100u) / evidence.m_sampledPixelCount : 0u;
				ss << " captureFrame=" << state.m_evidenceFrameIndex
					<< " captureEpoch=" << state.m_epoch
					<< " captureGen=" << state.m_generation
					<< " readable=" << (evidence.m_hasReadablePixels ? 1 : 0)
					<< " nonBlack=" << (hasNonBlackEvidence ? 1 : 0)
					<< " variance=" << (evidence.m_hasVisualVariance ? 1 : 0)
					<< " avgLuma=" << evidence.m_averageLuma
					<< " maxLuma=" << evidence.m_maxLuma
					<< " nonBlackPct=" << nonBlackPct
					<< " checksum=" << evidence.m_checksum
					<< " tl=" << static_cast<uint32_t>(evidence.m_topLeft.m_r) << "," << static_cast<uint32_t>(evidence.m_topLeft.m_g) << "," << static_cast<uint32_t>(evidence.m_topLeft.m_b)
					<< " c=" << static_cast<uint32_t>(evidence.m_center.m_r) << "," << static_cast<uint32_t>(evidence.m_center.m_g) << "," << static_cast<uint32_t>(evidence.m_center.m_b)
					<< " br=" << static_cast<uint32_t>(evidence.m_bottomRight.m_r) << "," << static_cast<uint32_t>(evidence.m_bottomRight.m_g) << "," << static_cast<uint32_t>(evidence.m_bottomRight.m_b);
			}
			return ss.str();
		}

		const std::optional<FramePacket>& GetLastPresentedFrame() const { return m_lastPresentedFrame; }

	private:
		Failure RefreshNativeLayerBinding(MacNativePresentationState& state, const MacNativeHostHandle& hostHandle)
		{
			if (!hostHandle.IsValid())
			{
				state.m_layerBinding.Clear();
				state.m_hostHandle = hostHandle;
				state.m_usesRealCAMetalLayer = false;
				return Failure::Ok();
			}

			auto result = BindMacNativeLayer(hostHandle, state.m_width, state.m_height, state.m_pixelFormat, state.m_layerBinding);
			if (!result.IsOk())
			{
				return result;
			}

			state.m_hostHandle = hostHandle;
			state.m_nativeLayerToken = state.m_layerBinding->m_bindingToken;
			state.m_usesRealCAMetalLayer = true;
			return Failure::Ok();
		}

		TMap<ViewportId, MacNativeHostHandle> m_hostHandles{};
		TMap<ViewportId, TUniquePtr<MacNativePresentationState>> m_importedStates{};
		std::optional<FramePacket> m_lastPresentedFrame{};
		Failure m_lastFailure = Failure::Ok();
		uint64_t m_nextImportToken = 0;
		uint64_t m_nextLayerToken = 0;
		uint64_t m_nextDrawableToken = 0;
	};

	inline Failure MacViewportTransportBackend::ImportSurface(IMacViewportPresenter& presenter,
		const ViewportDescriptor& viewport, const TransportDescriptor& transport, ConnectionEpoch epoch, SurfaceGeneration generation)
	{
		const auto* surface = std::as_const(*this).FindSurface(viewport.m_viewportId, epoch, generation);
		return presenter.ImportSurface(viewport, transport, epoch, generation, surface->m_nativeAllocation);
	}

	using MacViewportLoopbackBinding = TViewportLoopbackBinding<MacViewportTransportBackend, IMacViewportPresenter>;
}
