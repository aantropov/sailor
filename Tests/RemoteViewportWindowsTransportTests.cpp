#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "Submodules/EditorRemote/RemoteViewportWindowsTransport.h"

using namespace Sailor::EditorRemote;

namespace
{
	void Require(bool condition, const std::string& message)
	{
		if (!condition)
		{
			throw std::runtime_error(message);
		}
	}

	ViewportDescriptor MakeViewport(ViewportId viewportId = 31, uint32_t width = 1280, uint32_t height = 720)
	{
		ViewportDescriptor viewport{};
		viewport.m_viewportId = viewportId;
		viewport.m_width = width;
		viewport.m_height = height;
		viewport.m_pixelFormat = PixelFormat::B8G8R8A8_UNorm;
		viewport.m_colorSpace = ColorSpace::Srgb;
		viewport.m_presentMode = PresentMode::Mailbox;
		viewport.m_debugName = "WindowsViewport";
		return viewport;
	}

	class FakeWindowsSharedSurfaceProvider : public IWindowsSharedSurfaceProvider
	{
	public:
		Failure CreateOrResizeSurface(const ViewportDescriptor& viewport, ConnectionEpoch epoch, SurfaceGeneration generation, WindowsViewportSurfaceState& inOutState) override
		{
			m_createCalls.push_back({ viewport.m_viewportId, epoch, generation });
			if (!m_nextCreateFailure.IsOk())
			{
				m_lastFailure = m_nextCreateFailure;
				auto failure = m_nextCreateFailure;
				m_nextCreateFailure = Failure::Ok();
				return failure;
			}

			WindowsSharedSurfaceHandle handle{};
			handle.m_sharedTextureHandle = 0x1000ull + generation;
			handle.m_keyedMutexHandle = 0x2000ull + generation;
			handle.m_sharedFenceHandle = 0x3000ull + generation;
			handle.m_allocationId = (epoch << 32ull) | generation;
			handle.m_rowPitch = viewport.m_width * 4u;
			inOutState.m_transport.m_nativeHandles = { handle };
			inOutState.m_transport.m_syncMode = SyncMode::ExplicitFence;
			inOutState.m_transport.m_width = viewport.m_width;
			inOutState.m_transport.m_height = viewport.m_height;
			inOutState.m_transport.m_pixelFormat = viewport.m_pixelFormat;
			m_lastFailure = Failure::Ok();
			return Failure::Ok();
		}

		Failure BeginFrame(WindowsViewportSurfaceState& state) override
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

		Failure ExportFrame(WindowsViewportSurfaceState& state, FramePacket& outFrame) override
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
			outFrame.m_sync.m_releaseValue = outFrame.m_frameIndex + 1;
			outFrame.m_sync.m_requiresExplicitRelease = true;
			m_lastFailure = Failure::Ok();
			return Failure::Ok();
		}

		Failure ReleaseSurface(const WindowsViewportSurfaceState& state) override
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

		std::vector<WindowsViewportSurfaceKey> m_createCalls{};
		std::vector<WindowsViewportSurfaceKey> m_beginCalls{};
		std::vector<WindowsViewportSurfaceKey> m_exportCalls{};
		std::vector<WindowsViewportSurfaceKey> m_releaseCalls{};
		Failure m_nextCreateFailure = Failure::Ok();
		Failure m_nextBeginFailure = Failure::Ok();
		Failure m_nextExportFailure = Failure::Ok();
		Failure m_nextReleaseFailure = Failure::Ok();
		Failure m_lastFailure = Failure::Ok();
		FrameIndex m_exportedFrameCounter = 0;
	};

	class FakeWindowsViewportPresenter : public IWindowsViewportPresenter
	{
	public:
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

	void TestWindowsBackendCreateResizeExportAndRelease()
	{
		FakeWindowsSharedSurfaceProvider provider{};
		WindowsViewportTransportBackend backend{ provider };
		auto viewport = MakeViewport(31, 1280, 720);

		TransportDescriptor transport{};
		Require(backend.EnsureSurface(viewport, 7, 1, transport).IsOk(), "windows backend should create generation-one surface");
		Require(transport.m_transportType == TransportType::WinSharedHandle, "backend should negotiate WinSharedHandle transport");
		Require(!transport.m_nativeHandles.empty() && transport.m_nativeHandles.front().IsValid(), "transport should export a valid shared handle");
		Require(backend.GetSurfaceCount() == 1, "backend should track one live surface after create");

		FramePacket frame{};
		Require(backend.BeginFrame(viewport, 7, 1).IsOk(), "begin frame should succeed for live surface");
		Require(backend.ExportFrame(viewport, 7, 1, frame).IsOk(), "export frame should succeed after begin");
		Require(frame.m_connectionEpoch == 7 && frame.m_generation == 1, "frame metadata should preserve epoch/generation");
		Require(frame.m_sync.m_requiresExplicitRelease, "windows frame should carry explicit sync metadata");

		auto resizedViewport = MakeViewport(31, 1600, 900);
		Require(backend.EnsureSurface(resizedViewport, 7, 2, transport).IsOk(), "resize should create a new generation surface");
		Require(transport.m_width == 1600 && transport.m_height == 900, "resized transport should match new extents");
		Require(backend.GetSurfaceCount() == 2, "backend should keep both generations until explicitly released");

		Require(backend.ReleaseSurface(31, 7, 1).IsOk(), "releasing stale generation should succeed");
		Require(backend.ReleaseSurface(31, 7, 2).IsOk(), "releasing active generation should succeed");
		Require(backend.GetSurfaceCount() == 0, "all generations should be released cleanly");
	}

	void TestWindowsBackendFailurePropagationAndOrdering()
	{
		FakeWindowsSharedSurfaceProvider provider{};
		WindowsViewportTransportBackend backend{ provider };
		auto viewport = MakeViewport(32);

		FramePacket frame{};
		Require(!backend.BeginFrame(viewport, 5, 1).IsOk(), "begin frame without a surface should fail coherently");
		Require(backend.GetLastFailure().m_code == ResultCode::RecreateRequired, "missing surface should request recreate rather than crash");

		provider.m_nextCreateFailure = Failure::FromDomain(ErrorDomain::Transport, 901, "create failed");
		TransportDescriptor transport{};
		Require(!backend.EnsureSurface(viewport, 5, 1, transport).IsOk(), "provider create failure should surface");
		Require(backend.GetLastFailure().m_nativeCode == 901, "backend should retain provider create failure");

		Require(backend.EnsureSurface(viewport, 5, 1, transport).IsOk(), "second create attempt should recover after injected failure");
		Require(!backend.ExportFrame(viewport, 5, 1, frame).IsOk(), "export without begin should be rejected by backend ordering checks");
		Require(backend.GetLastFailure().m_code == ResultCode::FatalProtocolError, "ordering violation should be treated as protocol misuse");

		provider.m_nextExportFailure = Failure::FromDomain(ErrorDomain::Transport, 902, "export failed");
		Require(backend.BeginFrame(viewport, 5, 1).IsOk(), "begin should succeed before injected export failure");
		Require(!backend.ExportFrame(viewport, 5, 1, frame).IsOk(), "provider export failure should surface");
		Require(backend.GetLastFailure().m_nativeCode == 902, "backend should retain provider export failure");

		provider.m_nextReleaseFailure = Failure::FromDomain(ErrorDomain::Transport, 903, "release failed");
		Require(!backend.ReleaseSurface(32, 5, 1).IsOk(), "provider release failure should surface");
		Require(backend.GetLastFailure().m_nativeCode == 903, "backend should retain provider release failure");
	}

	void TestWindowsNativeHostImportPresentResetAndFailures()
	{
		FakeWindowsViewportPresenter presenter{};
		WindowsViewportNativeHost host{ presenter };
		auto viewport = MakeViewport(41);
		TransportDescriptor transport{};
		transport.m_transportType = TransportType::WinSharedHandle;
		transport.m_syncMode = SyncMode::ExplicitFence;
		transport.m_protocolVersion = 1;
		transport.m_width = viewport.m_width;
		transport.m_height = viewport.m_height;
		transport.m_pixelFormat = viewport.m_pixelFormat;
		transport.m_ready = true;
		transport.m_nativeHandles = { WindowsSharedSurfaceHandle{ 0x1111ull, 0x2222ull, 0x3333ull, 99ull, viewport.m_width * 4u, 1u } };

		Require(host.ImportTransport(viewport, transport, 9, 3).IsOk(), "windows host should import a valid shared-handle transport");
		Require(presenter.m_importEpoch == 9 && presenter.m_importGeneration == 3, "presenter should observe imported epoch/generation");

		FramePacket frame{};
		frame.m_viewportId = 41;
		frame.m_connectionEpoch = 9;
		frame.m_generation = 3;
		frame.m_frameIndex = 17;
		frame.m_width = viewport.m_width;
		frame.m_height = viewport.m_height;
		frame.m_sync.m_requiresExplicitRelease = true;
		Require(host.AcceptFrame(frame).IsOk(), "host should accept frames for the imported generation");
		Require(host.PresentLatestFrame(41).IsOk(), "host should present the latest accepted frame");
		Require(presenter.m_presentCalls.size() == 1 && presenter.m_presentCalls.front().m_frameIndex == 17, "presenter should receive the latest frame");

		FramePacket staleFrame = frame;
		staleFrame.m_generation = 2;
		Require(!host.AcceptFrame(staleFrame).IsOk(), "host should reject stale-generation frames");
		Require(host.GetLastFailure().m_code == ResultCode::RecreateRequired, "stale host frame should map to recreate-required session failure");

		TransportDescriptor wrongTransport = transport;
		wrongTransport.m_transportType = TransportType::MailboxCpuCopy;
		Require(!host.ImportTransport(viewport, wrongTransport, 9, 3).IsOk(), "windows host should reject non-Windows transports");

		host.ResetViewport(41);
		Require(!host.PresentLatestFrame(41).IsOk(), "reset should drop the imported frame before the next present");
		Require(!presenter.m_resets.empty() && presenter.m_resets.back() == 41, "reset should be forwarded to the presenter");
	}
}

int main()
{
	const std::pair<const char*, std::function<void()>> tests[] = {
		{ "WindowsBackendCreateResizeExportAndRelease", TestWindowsBackendCreateResizeExportAndRelease },
		{ "WindowsBackendFailurePropagationAndOrdering", TestWindowsBackendFailurePropagationAndOrdering },
		{ "WindowsNativeHostImportPresentResetAndFailures", TestWindowsNativeHostImportPresentResetAndFailures },
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
