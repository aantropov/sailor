#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "Submodules/EditorRemote/RemoteViewportMacTransport.h"

using namespace Sailor::EditorRemote;

namespace
{

#if defined(__APPLE__)
	uint32_t ReadIOSurfaceBGRA8Pixel(uintptr_t surfaceObject, uint32_t bytesPerRow, uint32_t x, uint32_t y)
	{
		IOSurfaceRef surface = reinterpret_cast<IOSurfaceRef>(surfaceObject);
		if (surface == nullptr)
		{
			throw std::runtime_error("test expected a live IOSurface object");
		}

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

	uint32_t ExpectedProducerPatternBGRA8(ViewportId viewportId, ConnectionEpoch epoch, SurfaceGeneration generation, FrameIndex frameIndex, uint32_t width, uint32_t height, uint32_t x, uint32_t y)
	{
		const uint8_t b = static_cast<uint8_t>((x + ((frameIndex * 17u + generation * 13u) & 0xffu)) & 0xffu);
		const uint8_t g = static_cast<uint8_t>((y + ((epoch * 29u + viewportId * 7u) & 0xffu)) & 0xffu);
		const uint8_t r = static_cast<uint8_t>((width + height + frameIndex * 3u) & 0xffu);
		return static_cast<uint32_t>(b) | (static_cast<uint32_t>(g) << 8u) | (static_cast<uint32_t>(r) << 16u) | 0xff000000u;
	}
#endif

	void Require(bool condition, const std::string& message)
	{
		if (!condition)
		{
			throw std::runtime_error(message);
		}
	}

	ViewportDescriptor MakeViewport(ViewportId viewportId = 51, uint32_t width = 1280, uint32_t height = 720)
	{
		ViewportDescriptor viewport{};
		viewport.m_viewportId = viewportId;
		viewport.m_width = width;
		viewport.m_height = height;
		viewport.m_pixelFormat = PixelFormat::B8G8R8A8_UNorm;
		viewport.m_colorSpace = ColorSpace::Srgb;
		viewport.m_presentMode = PresentMode::Mailbox;
		viewport.m_debugName = "MacViewport";
		return viewport;
	}

	class FakeMacIOSurfaceProvider : public IMacIOSurfaceProvider
	{
	public:
		Failure CreateOrResizeSurface(const ViewportDescriptor& viewport, ConnectionEpoch epoch, SurfaceGeneration generation, MacViewportSurfaceState& inOutState) override
		{
			m_createCalls.push_back({ viewport.m_viewportId, epoch, generation });
			if (!m_nextCreateFailure.IsOk())
			{
				m_lastFailure = m_nextCreateFailure;
				auto failure = m_nextCreateFailure;
				m_nextCreateFailure = Failure::Ok();
				return failure;
			}

			MacIOSurfaceHandle handle{};
			handle.m_surfaceId = 100u + static_cast<uint32_t>(generation);
			handle.m_registryId = (epoch << 32ull) | generation;
			handle.m_surfaceObject = handle.m_registryId;
			handle.m_planeIndex = 0;
			handle.m_planeCount = 1;
			handle.m_bytesPerRow = viewport.m_width * 4u;
			handle.m_bytesPerElement = 4;
			handle.m_framebufferOnly = false;
			inOutState.m_transport.m_macSurfaces = { handle };
			inOutState.m_transport.m_syncMode = SyncMode::ExplicitFence;
			inOutState.m_transport.m_width = viewport.m_width;
			inOutState.m_transport.m_height = viewport.m_height;
			inOutState.m_transport.m_pixelFormat = viewport.m_pixelFormat;
			m_lastFailure = Failure::Ok();
			return Failure::Ok();
		}

		Failure BeginFrame(MacViewportSurfaceState& state) override
		{
			m_beginCalls.push_back(state.m_key);
			if (!m_nextBeginFailure.IsOk())
			{
				m_lastFailure = m_nextBeginFailure;
				auto failure = m_nextBeginFailure;
				m_nextBeginFailure = Failure::Ok();
				return failure;
			}

			m_lastFailure = Failure::Ok();
			return Failure::Ok();
		}

		Failure ExportFrame(MacViewportSurfaceState& state, FramePacket& outFrame) override
		{
			m_exportCalls.push_back(state.m_key);
			if (!m_nextExportFailure.IsOk())
			{
				m_lastFailure = m_nextExportFailure;
				auto failure = m_nextExportFailure;
				m_nextExportFailure = Failure::Ok();
				return failure;
			}

			outFrame.m_viewportId = state.m_key.m_viewportId;
			outFrame.m_connectionEpoch = state.m_key.m_epoch;
			outFrame.m_generation = state.m_key.m_generation;
			outFrame.m_frameIndex = ++m_exportedFrameCounter;
			outFrame.m_width = state.m_viewport.m_width;
			outFrame.m_height = state.m_viewport.m_height;
			outFrame.m_timestampNs = m_exportedFrameCounter;
			outFrame.m_sync.m_acquireValue = outFrame.m_frameIndex;
			outFrame.m_sync.m_releaseValue = outFrame.m_frameIndex;
			outFrame.m_sync.m_requiresExplicitRelease = false;
			m_lastFailure = Failure::Ok();
			return Failure::Ok();
		}

		Failure ReleaseSurface(const MacViewportSurfaceState& state) override
		{
			m_releaseCalls.push_back(state.m_key);
			if (!m_nextReleaseFailure.IsOk())
			{
				m_lastFailure = m_nextReleaseFailure;
				auto failure = m_nextReleaseFailure;
				m_nextReleaseFailure = Failure::Ok();
				return failure;
			}

			m_lastFailure = Failure::Ok();
			return Failure::Ok();
		}

		Failure GetLastFailure() const override
		{
			return m_lastFailure;
		}

		std::vector<MacViewportSurfaceKey> m_createCalls{};
		std::vector<MacViewportSurfaceKey> m_beginCalls{};
		std::vector<MacViewportSurfaceKey> m_exportCalls{};
		std::vector<MacViewportSurfaceKey> m_releaseCalls{};
		Failure m_nextCreateFailure = Failure::Ok();
		Failure m_nextBeginFailure = Failure::Ok();
		Failure m_nextExportFailure = Failure::Ok();
		Failure m_nextReleaseFailure = Failure::Ok();
		Failure m_lastFailure = Failure::Ok();
		FrameIndex m_exportedFrameCounter = 0;
	};

	class FakeMacRendererFrameSourceProvider : public IMacRendererFrameSourceProvider
	{
	public:
		Failure AcquireFrameSource(const MacViewportSurfaceState& state, FrameIndex nextFrameIndex, MacRendererFrameSource& outSource) override
		{
			m_calls.push_back({ state.m_key, nextFrameIndex });
			if (!m_nextFailure.IsOk())
			{
				m_lastFailure = m_nextFailure;
				auto failure = m_nextFailure;
				m_nextFailure = Failure::Ok();
				return failure;
			}

			outSource = m_nextSource;
			m_lastFailure = Failure::Ok();
			return Failure::Ok();
		}

		Failure m_nextFailure = Failure::Ok();
		MacRendererFrameSource m_nextSource{};
		Failure m_lastFailure = Failure::Ok();
		std::vector<std::pair<MacViewportSurfaceKey, FrameIndex>> m_calls{};
	};

	class FakeMacViewportPresenter : public IMacViewportPresenter
	{
	public:
		void BindHostHandle(ViewportId viewportId, const MacNativeHostHandle& hostHandle) override
		{
			m_boundViewport = viewportId;
			m_boundHostHandle = hostHandle;
		}

		Failure ImportSurface(const ViewportDescriptor& viewport, const TransportDescriptor& transport, ConnectionEpoch epoch, SurfaceGeneration generation) override
		{
			m_importViewport = viewport;
			m_importTransport = transport;
			m_importEpoch = epoch;
			m_importGeneration = generation;
			if (!m_nextImportFailure.IsOk())
			{
				m_lastFailure = m_nextImportFailure;
				auto failure = m_nextImportFailure;
				m_nextImportFailure = Failure::Ok();
				return failure;
			}

			m_lastFailure = Failure::Ok();
			return Failure::Ok();
		}

		Failure PresentFrame(ViewportId viewportId, const FramePacket& frame) override
		{
			m_presentCalls.push_back(frame);
			if (!m_nextPresentFailure.IsOk())
			{
				m_lastFailure = m_nextPresentFailure;
				auto failure = m_nextPresentFailure;
				m_nextPresentFailure = Failure::Ok();
				return failure;
			}

			Require(viewportId == frame.m_viewportId, "present call should target accepted viewport");
			m_lastFailure = Failure::Ok();
			return Failure::Ok();
		}

		void ResetViewport(ViewportId viewportId) override
		{
			m_resets.push_back(viewportId);
		}

		Failure GetLastFailure() const override
		{
			return m_lastFailure;
		}

		ViewportId m_boundViewport = 0;
		MacNativeHostHandle m_boundHostHandle{};
		ViewportDescriptor m_importViewport{};
		TransportDescriptor m_importTransport{};
		ConnectionEpoch m_importEpoch = 0;
		SurfaceGeneration m_importGeneration = 0;
		std::vector<FramePacket> m_presentCalls{};
		std::vector<ViewportId> m_resets{};
		Failure m_nextImportFailure = Failure::Ok();
		Failure m_nextPresentFailure = Failure::Ok();
		Failure m_lastFailure = Failure::Ok();
	};

	void TestMacBackendCreateResizeExportAndRelease()
	{
		FakeMacIOSurfaceProvider provider{};
		MacViewportTransportBackend backend{ provider };
		auto viewport = MakeViewport(51, 1280, 720);

		TransportDescriptor transport{};
		Require(backend.EnsureSurface(viewport, 11, 1, transport).IsOk(), "mac backend should create generation-one surface");
		Require(transport.m_transportType == TransportType::MacIOSurface, "backend should negotiate MacIOSurface transport");
		Require(!transport.m_macSurfaces.empty() && transport.m_macSurfaces.front().IsValid(), "transport should export a valid IOSurface descriptor");
		Require(backend.GetSurfaceCount() == 1, "backend should track one live surface after create");

		FramePacket frame{};
		Require(backend.BeginFrame(viewport, 11, 1).IsOk(), "begin frame should succeed for live surface");
		Require(backend.ExportFrame(viewport, 11, 1, frame).IsOk(), "export frame should succeed after begin");
		Require(frame.m_connectionEpoch == 11 && frame.m_generation == 1, "frame metadata should preserve epoch/generation");
		Require(!frame.m_sync.m_requiresExplicitRelease, "mac frame should stay mailbox-style without editor release ack in this slice");

		auto resizedViewport = MakeViewport(51, 1920, 1080);
		Require(backend.EnsureSurface(resizedViewport, 11, 2, transport).IsOk(), "resize should create a new generation surface");
		Require(transport.m_width == 1920 && transport.m_height == 1080, "resized transport should match new extents");
		Require(backend.GetSurfaceCount() == 2, "backend should keep both generations until explicitly released");

		Require(backend.ReleaseSurface(51, 11, 1).IsOk(), "releasing stale generation should succeed");
		Require(backend.ReleaseSurface(51, 11, 2).IsOk(), "releasing active generation should succeed");
		Require(backend.GetSurfaceCount() == 0, "all generations should be released cleanly");
	}

	void TestMacBackendFailurePropagationAndOrdering()
	{
		FakeMacIOSurfaceProvider provider{};
		MacViewportTransportBackend backend{ provider };
		auto viewport = MakeViewport(52);

		FramePacket frame{};
		Require(!backend.BeginFrame(viewport, 12, 1).IsOk(), "begin frame without a surface should fail coherently");
		Require(backend.GetLastFailure().m_code == ResultCode::RecreateRequired, "missing surface should request recreate rather than crash");

		provider.m_nextCreateFailure = Failure::FromDomain(ErrorDomain::Transport, 911, "create failed");
		TransportDescriptor transport{};
		Require(!backend.EnsureSurface(viewport, 12, 1, transport).IsOk(), "provider create failure should surface");
		Require(backend.GetLastFailure().m_nativeCode == 911, "backend should retain provider create failure");

		Require(backend.EnsureSurface(viewport, 12, 1, transport).IsOk(), "second create attempt should recover after injected failure");
		Require(!backend.ExportFrame(viewport, 12, 1, frame).IsOk(), "export without begin should be rejected by backend ordering checks");
		Require(backend.GetLastFailure().m_code == ResultCode::FatalProtocolError, "ordering violation should be treated as protocol misuse");

		provider.m_nextExportFailure = Failure::FromDomain(ErrorDomain::Transport, 912, "export failed");
		Require(backend.BeginFrame(viewport, 12, 1).IsOk(), "begin should succeed before injected export failure");
		Require(!backend.ExportFrame(viewport, 12, 1, frame).IsOk(), "provider export failure should surface");
		Require(backend.GetLastFailure().m_nativeCode == 912, "backend should retain provider export failure");

		provider.m_nextReleaseFailure = Failure::FromDomain(ErrorDomain::Transport, 913, "release failed");
		Require(!backend.ReleaseSurface(52, 12, 1).IsOk(), "provider release failure should surface");
		Require(backend.GetLastFailure().m_nativeCode == 913, "backend should retain provider release failure");
	}

	void TestMacNativeHostImportPresentResetAndFailures()
	{
		FakeMacViewportPresenter presenter{};
		MacViewportNativeHost host{ presenter };
		auto viewport = MakeViewport(61);
		TransportDescriptor transport{};
		transport.m_transportType = TransportType::MacIOSurface;
		transport.m_syncMode = SyncMode::ExplicitFence;
		transport.m_protocolVersion = 1;
		transport.m_width = viewport.m_width;
		transport.m_height = viewport.m_height;
		transport.m_pixelFormat = viewport.m_pixelFormat;
		transport.m_ready = true;
		transport.m_macSurfaces = { MacIOSurfaceHandle{ 321u, 0x4444ull, 0x5555ull, 0u, 1u, viewport.m_width * 4u, 4u, false } };

		host.BindNativeHostHandle(61, MacNativeHostHandle{ MacNativeHostHandleKind::CAMetalLayer, 0x1234u });
		Require(presenter.m_boundViewport == 61 && presenter.m_boundHostHandle.IsValid(), "host should forward opaque native host handles to presenter");
		Require(host.ImportTransport(viewport, transport, 13, 3).IsOk(), "mac host should import a valid IOSurface transport");
		Require(presenter.m_importEpoch == 13 && presenter.m_importGeneration == 3, "presenter should observe imported epoch/generation");

		FramePacket frame{};
		frame.m_viewportId = 61;
		frame.m_connectionEpoch = 13;
		frame.m_generation = 3;
		frame.m_frameIndex = 17;
		frame.m_width = viewport.m_width;
		frame.m_height = viewport.m_height;
		Require(host.AcceptFrame(frame).IsOk(), "host should accept frames for the imported generation");
		Require(host.PresentLatestFrame(61).IsOk(), "host should present the latest accepted frame");
		Require(presenter.m_presentCalls.size() == 1 && presenter.m_presentCalls.front().m_frameIndex == 17, "presenter should receive the latest frame");

		FramePacket staleFrame = frame;
		staleFrame.m_generation = 2;
		Require(!host.AcceptFrame(staleFrame).IsOk(), "host should reject stale-generation frames");
		Require(host.GetLastFailure().m_code == ResultCode::RecreateRequired, "stale host frame should map to recreate-required session failure");

		TransportDescriptor wrongTransport = transport;
		wrongTransport.m_transportType = TransportType::WinSharedHandle;
		wrongTransport.m_macSurfaces.clear();
		wrongTransport.m_nativeHandles = { WindowsSharedSurfaceHandle{ 1ull, 2ull, 3ull, 4ull, viewport.m_width * 4u, 1u } };
		Require(!host.ImportTransport(viewport, wrongTransport, 13, 3).IsOk(), "mac host should reject non-mac transports");

		host.ResetViewport(61);
		Require(!host.PresentLatestFrame(61).IsOk(), "reset should drop the imported frame before the next present");
		Require(!presenter.m_resets.empty() && presenter.m_resets.back() == 61, "reset should be forwarded to the presenter");
	}

	void TestMacLoopbackBindingCreateResizeVisibilityAndDestroy()
	{
		FakeMacIOSurfaceProvider provider{};
		FakeMacViewportPresenter presenter{};
		MacViewportLoopbackBinding binding{ MakeViewport(71, 1440, 900), provider, presenter, 21 };

		Require(binding.Create().IsOk(), "loopback binding should negotiate a mac transport on create");
		Require(binding.GetRuntimeSession().GetState() == SessionState::Active, "created loopback binding should enter active state");
		Require(binding.GetRuntimeSession().IsReady(), "loopback binding should mark runtime transport ready");
		Require(presenter.m_importEpoch == 21 && presenter.m_importGeneration == 1, "presenter should import generation-one transport on create");

		Require(binding.PumpFrame().IsOk(), "loopback binding should export and present a frame through mac host contract");
		Require(binding.GetRuntimeSession().GetLastPublishedFrameIndex() == 1, "loopback binding should publish runtime frame indices");
		Require(presenter.m_presentCalls.size() == 1, "loopback binding should present the exported frame");

		Require(binding.SetVisible(false).IsOk(), "loopback binding should accept visibility changes");
		Require(binding.GetRuntimeSession().GetState() == SessionState::Paused, "hidden loopback binding should pause runtime session");
		Require(binding.PumpFrame().IsOk(), "paused loopback binding should no-op frame pump without failure");
		Require(presenter.m_presentCalls.size() == 1, "paused loopback binding should not present extra frames");

		Require(binding.SetVisible(true).IsOk(), "loopback binding should resume when made visible again");
		Require(binding.Resize(1728, 1117).IsOk(), "loopback binding should re-import a resized generation");
		Require(binding.GetRuntimeSession().GetGeneration() == 2, "resize should advance transport generation");
		Require(presenter.m_importGeneration == 2, "presenter should observe resized generation import");

		Require(binding.SetFocused(true).IsOk(), "loopback binding should forward focus as runtime input");
		Require(binding.GetRuntimeSession().GetInputCount() == 1, "focus should be counted as runtime input");
		Require(binding.PumpFrame().IsOk(), "resized loopback binding should continue presenting frames");
		Require(binding.GetRuntimeSession().GetLastPublishedFrameIndex() == 1, "resized generation should reset frame indexing");
		Require(presenter.m_presentCalls.back().m_generation == 2, "presented frame should match resized generation");

		Require(binding.Destroy().IsOk(), "loopback binding should release mac transport on destroy");
		Require(binding.GetTransportBackend().GetSurfaceCount() == 0, "destroy should release all tracked mac surfaces");
		Require(!presenter.m_resets.empty() && presenter.m_resets.back() == 71, "destroy should reset presenter viewport state");
		Require(binding.GetRuntimeSession().GetState() == SessionState::Disposed, "destroy should dispose runtime session");
	}

	void TestConcreteMacLoopbackProviderRecordsRendererOwnedSourceMetadata()
	{
		FakeMacRendererFrameSourceProvider sourceProvider{};
		sourceProvider.m_nextSource.m_kind = MacRendererFrameSourceKind::RendererOwnedRenderTargetMetadata;
		sourceProvider.m_nextSource.m_sourceToken = 0x12345678ull;
		sourceProvider.m_nextSource.m_width = 1920;
		sourceProvider.m_nextSource.m_height = 1080;
		sourceProvider.m_nextSource.m_pixelFormat = PixelFormat::R16G16B16A16_Float;
		sourceProvider.m_nextSource.m_debugName = "Renderer.SceneView.Main.Resolved";

		MacLoopbackIOSurfaceProvider provider{ &sourceProvider };
		MacViewportTransportBackend backend{ provider };
		auto viewport = MakeViewport(82, 800, 600);
		TransportDescriptor transport{};
		Require(backend.EnsureSurface(viewport, 32, 1, transport).IsOk(), "concrete mac provider should create transport when a renderer source provider is attached");
		Require(backend.BeginFrame(viewport, 32, 1).IsOk(), "begin frame should tolerate metadata-only renderer source adapters and keep the IOSurface copy path alive");

		auto key = MacViewportSurfaceKey{ viewport.m_viewportId, 32, 1 };
		auto* allocation = provider.FindAllocation(key);
		Require(allocation != nullptr && allocation->m_lastRendererSource.m_kind == MacRendererFrameSourceKind::RendererOwnedRenderTargetMetadata, "provider should record the renderer-owned source kind selected by the adapter seam");
		Require(allocation != nullptr && allocation->m_lastRendererSource.m_sourceToken == 0x12345678ull, "provider should preserve the renderer source token supplied by the adapter seam");
		Require(allocation != nullptr && allocation->m_lastRendererSource.m_debugName == "Renderer.SceneView.Main.Resolved", "provider should preserve the resolved scene-view renderer target name supplied by the adapter seam");
		Require(allocation != nullptr && allocation->m_lastRendererSource.m_pixelFormat == PixelFormat::R16G16B16A16_Float, "provider should preserve the renderer-owned final-color format rather than forcing backbuffer metadata");
		Require(allocation != nullptr && allocation->m_lastRendererTextureToken == 0x12345678ull, "metadata-only renderer source should still stamp renderer-side provenance onto the exported frame path");
		Require(sourceProvider.m_calls.size() == 1 && sourceProvider.m_calls.front().second == 1, "begin frame should query the renderer source adapter for the next frame");

		Require(backend.ReleaseSurface(viewport.m_viewportId, 32, 1).IsOk(), "provider should still release surfaces after metadata-backed begin frame");
	}

#if defined(__APPLE__)
	void TestMacProducerCpuUploadWritesIOSurface()
	{
		MacLoopbackIOSurfaceProvider provider{};
		MacViewportSurfaceState state{};
		auto viewport = MakeViewport(80, 4, 2);
		Require(provider.CreateOrResizeSurface(viewport, 30, 1, state).IsOk(), "concrete mac provider should create a test IOSurface for CPU upload validation");
		Require(state.m_nativeAllocation.has_value(), "cpu upload validation needs a live native allocation");

		const uint8_t pixels[] = {
			1, 2, 3, 255, 4, 5, 6, 255, 7, 8, 9, 255, 10, 11, 12, 255,
			13, 14, 15, 255, 16, 17, 18, 255, 19, 20, 21, 255, 22, 23, 24, 255
		};
		MacNativeBridgeRendererFrameInfo frameInfo{};
		Require(UploadMacRendererBytesToProducerTexture(state.m_nativeAllocation->m_producerTextureObject, viewport.m_width, viewport.m_height, pixels, viewport.m_width * 4u, frameInfo).IsOk(), "cpu upload helper should write bytes into the IOSurface-backed producer texture");
		Require(frameInfo.m_producerCopyToken != 0 && frameInfo.m_usedCpuUploadIntoProducerTexture, "cpu upload helper should stamp producer-copy provenance");
		Require(ReadIOSurfaceBGRA8Pixel(state.m_nativeAllocation->m_surfaceObject, state.m_nativeAllocation->m_plane.m_bytesPerRow, 0, 0) == 0xff030201u, "cpu upload helper should preserve the first BGRA pixel in the shared IOSurface");
		Require(ReadIOSurfaceBGRA8Pixel(state.m_nativeAllocation->m_surfaceObject, state.m_nativeAllocation->m_plane.m_bytesPerRow, 3, 1) == 0xff181716u, "cpu upload helper should preserve later BGRA pixels in the shared IOSurface");
		Require(provider.ReleaseSurface(state).IsOk(), "cpu upload validation should release the temporary IOSurface cleanly");
	}

	void TestConcreteMacLoopbackProviderCopiesRendererOwnedMetalTextureIntoIOSurface()
	{
		auto viewport = MakeViewport(83, 8, 4);
		MacLoopbackIOSurfaceProvider bootstrapProvider{};
		MacViewportSurfaceState bootstrapState{};
		Require(bootstrapProvider.CreateOrResizeSurface(viewport, 33, 1, bootstrapState).IsOk(), "metal-source validation should create a bootstrap IOSurface allocation");
		Require(bootstrapState.m_nativeAllocation.has_value(), "metal-source validation needs bootstrap native allocation metadata");

		uintptr_t rendererTextureObject = 0;
		Require(CreateMacRendererIntermediateTexture(bootstrapState.m_nativeAllocation->m_producerDeviceObject, viewport.m_width, viewport.m_height, viewport.m_pixelFormat, rendererTextureObject).IsOk(), "metal-source validation should create a renderer-owned Metal texture");
		MacNativeBridgeProducerPattern pattern{ viewport.m_viewportId, 33, 1, 1, viewport.m_width, viewport.m_height };
		Require(UploadMacRendererPatternToIntermediateTexture(rendererTextureObject, viewport.m_width, viewport.m_height, pattern).IsOk(), "metal-source validation should seed the renderer-owned Metal texture with a deterministic pattern");

		MacRendererFrameSource source{};
		source.m_kind = MacRendererFrameSourceKind::RendererOwnedMetalTexture;
		source.m_textureObject = rendererTextureObject;
		source.m_sourceToken = 0xfeedfaceull;
		source.m_width = viewport.m_width;
		source.m_height = viewport.m_height;
		source.m_pixelFormat = viewport.m_pixelFormat;
		source.m_debugName = "Renderer.SceneView.Main.Resolved";

		class InlineMetalSourceProvider final : public IMacRendererFrameSourceProvider
		{
		public:
			explicit InlineMetalSourceProvider(const MacRendererFrameSource& source) : m_source(source) {}
			Failure AcquireFrameSource(const MacViewportSurfaceState&, FrameIndex, MacRendererFrameSource& outSource) override
			{
				outSource = m_source;
				return Failure::Ok();
			}
			MacRendererFrameSource m_source{};
		} sourceProvider{ source };

		MacLoopbackIOSurfaceProvider liveProvider{ &sourceProvider };
		MacViewportTransportBackend backend{ liveProvider };
		TransportDescriptor transport{};
		Require(backend.EnsureSurface(viewport, 33, 1, transport).IsOk(), "metal-source validation should create transport using the live provider");
		Require(backend.BeginFrame(viewport, 33, 1).IsOk(), "metal-source validation should copy the renderer-owned Metal texture into the IOSurface-backed producer texture");

		auto key = MacViewportSurfaceKey{ viewport.m_viewportId, 33, 1 };
		auto* allocation = liveProvider.FindAllocation(key);
		Require(allocation != nullptr && allocation->m_lastRendererSource.m_kind == MacRendererFrameSourceKind::RendererOwnedMetalTexture, "begin frame should record the renderer-owned Metal texture source kind");
		Require(allocation != nullptr && allocation->m_lastRendererTextureToken == 0xfeedfaceull, "begin frame should preserve the renderer-owned Metal texture token");
		Require(allocation != nullptr && allocation->m_lastCrossApiAcquireValue == 0, "inline Metal source tests should not claim Vulkan->Metal sync metadata when the source is already a native Metal texture");
		Require(ReadIOSurfaceBGRA8Pixel(allocation->m_surfaceObject, allocation->m_plane.m_bytesPerRow, 0, 0) == ExpectedProducerPatternBGRA8(viewport.m_viewportId, 33, 1, 1, viewport.m_width, viewport.m_height, 0, 0), "renderer-owned Metal texture path should land the first deterministic pixel into the shared IOSurface");
		Require(ReadIOSurfaceBGRA8Pixel(allocation->m_surfaceObject, allocation->m_plane.m_bytesPerRow, 5, 2) == ExpectedProducerPatternBGRA8(viewport.m_viewportId, 33, 1, 1, viewport.m_width, viewport.m_height, 5, 2), "renderer-owned Metal texture path should preserve later pixels through the GPU copy into the IOSurface");
		Require(backend.ReleaseSurface(viewport.m_viewportId, 33, 1).IsOk(), "metal-source validation should release the live provider allocation");
		Require(bootstrapProvider.ReleaseSurface(bootstrapState).IsOk(), "metal-source validation should release the bootstrap allocation");
		ReleaseMacRendererIntermediateTexture(rendererTextureObject);
		Require(rendererTextureObject == 0, "metal-source validation should release the test renderer-owned Metal texture");
	}
#endif

	void TestMacExportCarriesCrossApiSyncMetadataFromRendererSource()
	{
#if defined(__APPLE__)
		auto viewport = MakeViewport(82, 48, 24);
		MacLoopbackIOSurfaceProvider bootstrapProvider{};
		MacViewportSurfaceState bootstrapState{};
		Require(bootstrapProvider.CreateOrResizeSurface(viewport, 34, 1, bootstrapState).IsOk(), "cross-api sync validation should create a bootstrap IOSurface allocation");
		uintptr_t rendererTextureObject = 0;
		Require(CreateMacRendererIntermediateTexture(bootstrapState.m_nativeAllocation->m_producerDeviceObject, viewport.m_width, viewport.m_height, viewport.m_pixelFormat, rendererTextureObject).IsOk(), "cross-api sync validation should create a Metal source texture");
		MacNativeBridgeProducerPattern pattern{ viewport.m_viewportId, 34, 1, 1, viewport.m_width, viewport.m_height };
		Require(UploadMacRendererPatternToIntermediateTexture(rendererTextureObject, viewport.m_width, viewport.m_height, pattern).IsOk(), "cross-api sync validation should populate the Metal source texture");

		MacRendererFrameSource source{};
		source.m_kind = MacRendererFrameSourceKind::RendererOwnedMetalTexture;
		source.m_textureObject = rendererTextureObject;
		source.m_sourceToken = 0xbeefull;
		source.m_crossApiAcquireValue = 77ull;
		source.m_crossApiSyncKind = CrossApiSyncKind::CpuDeviceIdle;
		source.m_crossApiCpuWaited = true;
		source.m_width = viewport.m_width;
		source.m_height = viewport.m_height;
		source.m_pixelFormat = viewport.m_pixelFormat;
		source.m_debugName = "Renderer.SceneView.Main.Resolved";

		class InlineSynchronizedMetalSourceProvider final : public IMacRendererFrameSourceProvider
		{
		public:
			explicit InlineSynchronizedMetalSourceProvider(const MacRendererFrameSource& source) : m_source(source) {}
			Failure AcquireFrameSource(const MacViewportSurfaceState&, FrameIndex, MacRendererFrameSource& outSource) override
			{
				outSource = m_source;
				return Failure::Ok();
			}
			MacRendererFrameSource m_source{};
		} sourceProvider{ source };

		MacLoopbackIOSurfaceProvider provider{ &sourceProvider };
		MacViewportTransportBackend backend{ provider };
		TransportDescriptor transport{};
		Require(backend.EnsureSurface(viewport, 34, 1, transport).IsOk(), "cross-api sync validation should create transport");
		Require(backend.BeginFrame(viewport, 34, 1).IsOk(), "cross-api sync validation should begin frame");
		FramePacket frame{};
		Require(backend.ExportFrame(viewport, 34, 1, frame).IsOk(), "cross-api sync validation should export frame");
		Require(frame.m_sync.m_crossApiSyncKind == CrossApiSyncKind::CpuDeviceIdle, "exported frame should preserve the explicit Vulkan->Metal sync kind");
		Require(frame.m_sync.m_crossApiAcquireValue == 77ull, "exported frame should preserve the explicit Vulkan->Metal acquire value");
		Require(frame.m_sync.m_crossApiCpuWaited, "exported frame should preserve that the current explicit Vulkan->Metal seam blocks on CPU");
		Require(backend.ReleaseSurface(viewport.m_viewportId, 34, 1).IsOk(), "cross-api sync validation should release the live provider allocation");
		Require(bootstrapProvider.ReleaseSurface(bootstrapState).IsOk(), "cross-api sync validation should release the bootstrap allocation");
		ReleaseMacRendererIntermediateTexture(rendererTextureObject);
#else
		std::cout << "[SKIP] cross-api sync threading test is macOS-only" << std::endl;
#endif
	}

	void TestConcreteMacLoopbackProviderAndPresenterCarryNativeMetadata()
	{
		MacLoopbackIOSurfaceProvider provider{};
		MacViewportTransportBackend backend{ provider };
		auto viewport = MakeViewport(81, 1600, 1000);

		TransportDescriptor transport{};
		Require(backend.EnsureSurface(viewport, 31, 1, transport).IsOk(), "concrete mac provider should create transport");
		auto key = MacViewportSurfaceKey{ viewport.m_viewportId, 31, 1 };
		auto* allocation = provider.FindAllocation(key);
		Require(allocation != nullptr && allocation->IsValid(), "provider should retain IOSurface allocation ownership metadata");
		Require(allocation->m_plane.m_bytesPerRow == viewport.m_width * 4u, "allocation plane layout should carry row pitch");
		Require(allocation->m_producerTextureObject != 0, "provider should materialize a producer-side Metal texture for the IOSurface allocation");
		Require(allocation->m_rendererIntermediateTextureObject != 0, "provider should materialize a renderer-shaped intermediate Metal texture before the IOSurface copy");
		Require(transport.m_macSurfaces.front().m_registryId == allocation->m_registryId, "transport handle should export allocation registry id");
		Require(transport.m_macSurfaces.front().m_surfaceObject == allocation->m_surfaceObject, "transport handle should carry the same-process IOSurface object for loopback import");

		FramePacket frame{};
		Require(backend.BeginFrame(viewport, 31, 1).IsOk(), "concrete mac provider should begin frame");
		allocation = provider.FindAllocation(key);
		Require(allocation != nullptr && allocation->m_lastWrittenFrameIndex == 1, "begin frame should populate producer content into the live IOSurface allocation");
		Require(allocation != nullptr && allocation->m_lastRendererTextureToken != 0, "begin frame should stamp renderer-intermediate activity");
		Require(allocation != nullptr && allocation->m_lastProducerCopyToken != 0, "begin frame should stamp GPU copy activity into the IOSurface-backed producer texture");
#if defined(__APPLE__)
		Require(ReadIOSurfaceBGRA8Pixel(allocation->m_surfaceObject, allocation->m_plane.m_bytesPerRow, 0, 0) == ExpectedProducerPatternBGRA8(viewport.m_viewportId, 31, 1, 1, viewport.m_width, viewport.m_height, 0, 0), "GPU copy path should land the renderer-intermediate BGRA pattern into the IOSurface before export");
		Require(ReadIOSurfaceBGRA8Pixel(allocation->m_surfaceObject, allocation->m_plane.m_bytesPerRow, 17, 9) == ExpectedProducerPatternBGRA8(viewport.m_viewportId, 31, 1, 1, viewport.m_width, viewport.m_height, 17, 9), "renderer-intermediate copy should vary per pixel and remain addressable from the shared IOSurface");
#endif
		Require(backend.ExportFrame(viewport, 31, 1, frame).IsOk(), "concrete mac provider should export frame");
		Require(frame.m_sync.m_acquireValue != 0 && frame.m_sync.m_acquireValue == frame.m_sync.m_releaseValue, "exported frame should carry explicit fence metadata");
		Require(frame.m_sync.m_crossApiSyncKind == CrossApiSyncKind::None, "synthetic intermediate path should not claim Vulkan->Metal sync metadata");
		Require(frame.m_sync.m_crossApiAcquireValue == 0 && !frame.m_sync.m_crossApiCpuWaited, "synthetic intermediate path should leave cross-API sync metadata empty");

		MacLoopbackViewportPresenter presenter{};
		MacViewportNativeHost host{ presenter };
		Require(host.ImportTransport(viewport, transport, 31, 1).IsOk(), "concrete presenter should import concrete mac transport");
		auto* nativeState = presenter.FindImportedState(viewport.m_viewportId);
		Require(nativeState != nullptr && nativeState->IsValid(), "presenter should materialize native presentation state");
		Require(nativeState->m_registryId == allocation->m_registryId, "presenter should keep imported IOSurface registry id");

		Require(host.AcceptFrame(frame).IsOk(), "host should accept frame exported from concrete provider");
		Require(host.PresentLatestFrame(viewport.m_viewportId).IsOk(), "host should present frame through concrete presenter");
		nativeState = presenter.FindImportedState(viewport.m_viewportId);
		Require(nativeState != nullptr && nativeState->m_presentedFrameCount == 1, "presenter should track native present count");
		Require(nativeState->m_currentDrawableToken != 0, "presenter should synthesize drawable ownership on present");
		Require(nativeState->m_importedSurface.has_value() && nativeState->m_importedSurface->m_surfaceObject == allocation->m_surfaceObject, "presenter should preserve the imported IOSurface object handle");
		Require(!nativeState->m_usesRealCAMetalLayer, "without a bound native host handle the concrete presenter should stay in synthetic bookkeeping mode");

		Require(backend.ReleaseSurface(viewport.m_viewportId, 31, 1).IsOk(), "concrete provider should release IOSurface allocation");
		Require(provider.FindAllocation(key) == nullptr, "provider should drop ownership metadata after release");
	}
}

int main()
{
	const std::pair<const char*, std::function<void()>> tests[] = {
		{ "MacBackendCreateResizeExportAndRelease", TestMacBackendCreateResizeExportAndRelease },
		{ "MacBackendFailurePropagationAndOrdering", TestMacBackendFailurePropagationAndOrdering },
		{ "MacNativeHostImportPresentResetAndFailures", TestMacNativeHostImportPresentResetAndFailures },
		{ "MacLoopbackBindingCreateResizeVisibilityAndDestroy", TestMacLoopbackBindingCreateResizeVisibilityAndDestroy },
#if defined(__APPLE__)
		{ "MacProducerCpuUploadWritesIOSurface", TestMacProducerCpuUploadWritesIOSurface },
		{ "ConcreteMacLoopbackProviderCopiesRendererOwnedMetalTextureIntoIOSurface", TestConcreteMacLoopbackProviderCopiesRendererOwnedMetalTextureIntoIOSurface },
		{ "MacExportCarriesCrossApiSyncMetadataFromRendererSource", TestMacExportCarriesCrossApiSyncMetadataFromRendererSource },
#endif
		{ "ConcreteMacLoopbackProviderRecordsRendererOwnedSourceMetadata", TestConcreteMacLoopbackProviderRecordsRendererOwnedSourceMetadata },
		{ "ConcreteMacLoopbackProviderAndPresenterCarryNativeMetadata", TestConcreteMacLoopbackProviderAndPresenterCarryNativeMetadata },
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
