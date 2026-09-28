#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "Submodules/EditorRemote/RemoteViewportWindowsTransport.h"
#include "Support/ViewportBindingLifecycle.h"

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
			m_liveSurfaces.push_back(inOutState.m_key);
			if (std::exchange(m_invalidTransport, false)) inOutState.m_transport.m_width = 0;
			if (std::exchange(m_mismatchedExtent, false)) ++inOutState.m_transport.m_width;
			if (!m_nextCreatedFailure.IsOk())
			{
				m_lastFailure = std::exchange(m_nextCreatedFailure, Failure::Ok());
				return m_lastFailure;
			}
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

			std::erase(m_liveSurfaces, state.m_key);
			m_lastFailure = Failure::Ok();
			return Failure::Ok();
		}

		Failure GetLastFailure() const override
		{
			return m_lastFailure;
		}

		std::vector<WindowsViewportSurfaceKey> m_createCalls{};
		std::vector<WindowsViewportSurfaceKey> m_liveSurfaces{};
		Failure m_nextCreatedFailure = Failure::Ok();
		bool m_invalidTransport = false;
		bool m_mismatchedExtent = false;
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
			if (m_onImport) m_onImport();
			if (!m_nextImportFailure.IsOk())
			{
				m_lastFailure = m_nextImportFailure;
				auto failure = m_nextImportFailure;
				m_nextImportFailure = Failure::Ok();
				return failure;
			}

			m_importViewport = viewport;
			m_importTransport = transport;
			m_importEpoch = epoch;
			m_importGeneration = generation;
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
		std::function<void()> m_onImport;
		TransportDescriptor m_importTransport{};
		ConnectionEpoch m_importEpoch = 0;
		SurfaceGeneration m_importGeneration = 0;
		std::vector<FramePacket> m_presentCalls{};
		std::vector<ViewportId> m_resets{};
		Failure m_nextImportFailure = Failure::Ok();
		Failure m_nextPresentFailure = Failure::Ok();
		Failure m_lastFailure = Failure::Ok();
	};

	void TestWindowsLoopbackResizeIsTransactional()
	{
		Sailor::Tests::TestViewportResizeIsTransactional<WindowsViewportLoopbackBinding, FakeWindowsSharedSurfaceProvider, FakeWindowsViewportPresenter>(MakeViewport());
	}

	void TestWindowsLoopbackImportAndRetirementFailures()
	{
		Sailor::Tests::TestViewportImportAndRetirementFailures<WindowsViewportLoopbackBinding, FakeWindowsSharedSurfaceProvider, FakeWindowsViewportPresenter>(MakeViewport());
	}

	void TestWindowsLoopbackRecoveryUsesElapsedTime()
	{
		Sailor::Tests::TestViewportRecoveryUsesElapsedTime<WindowsViewportLoopbackBinding, FakeWindowsSharedSurfaceProvider, FakeWindowsViewportPresenter>(MakeViewport());
	}

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

	void TestWindowsBackendRetriesPreparedFrame()
	{
		FakeWindowsSharedSurfaceProvider provider;
		WindowsViewportTransportBackend backend(provider);
		const auto viewport = MakeViewport(33);
		TransportDescriptor transport;
		Require(backend.EnsureSurface(viewport, 5, 1, transport).IsOk(), "retry fixture must create a surface");
		provider.m_nextBeginFailure = Failure::FromDomain(ErrorDomain::Transport, 904, "copy pending");
		Require(!backend.BeginFrame(viewport, 5, 1).IsOk(), "pending copy must fail begin");
		FramePacket frame;
		Require(!backend.ExportFrame(viewport, 5, 1, frame).IsOk() && provider.m_exportCalls.empty(),
			"an incomplete begin must not reach the provider export");
		Require(backend.BeginFrame(viewport, 5, 1).IsOk(), "begin must retry the pending provider copy");
		Require(backend.BeginFrame(viewport, 5, 1).IsOk() && provider.m_beginCalls.size() == 2u,
			"a prepared frame must not begin another provider copy before export");
		provider.m_nextExportFailure = Failure::FromDomain(ErrorDomain::Transport, 905, "export failed");
		Require(!backend.ExportFrame(viewport, 5, 1, frame).IsOk(), "export failure must reach the caller");
		Require(backend.BeginFrame(viewport, 5, 1).IsOk() && provider.m_beginCalls.size() == 2u,
			"retry after export failure must reuse the prepared frame");
		Require(backend.ExportFrame(viewport, 5, 1, frame).IsOk() && frame.m_frameIndex == 1u,
			"only successful export may advance the frame index");
		Require(!backend.ExportFrame(viewport, 5, 1, frame).IsOk(), "one begin permits only one successful export");
		Require(backend.BeginFrame(viewport, 5, 1).IsOk() && provider.m_beginCalls.size() == 3u,
			"the next frame must begin a fresh provider copy");
		Require(backend.ExportFrame(viewport, 5, 1, frame).IsOk() && frame.m_frameIndex == 2u,
			"subsequent export must advance once");
	}

	void TestWindowsLoopbackPresentFailure()
	{
		Sailor::Tests::TestViewportPresentFailure<WindowsViewportLoopbackBinding, FakeWindowsSharedSurfaceProvider, FakeWindowsViewportPresenter>(MakeViewport());
	}
}

int main()
{
	const std::pair<const char*, std::function<void()>> tests[] = {
		{ "WindowsBackendCreateResizeExportAndRelease", TestWindowsBackendCreateResizeExportAndRelease },
		{ "WindowsLoopbackRecoveryUsesElapsedTime", TestWindowsLoopbackRecoveryUsesElapsedTime },
		{ "WindowsLoopbackResizeIsTransactional", TestWindowsLoopbackResizeIsTransactional },
		{ "WindowsLoopbackImportAndRetirementFailures", TestWindowsLoopbackImportAndRetirementFailures },
		{ "WindowsBackendFailurePropagationAndOrdering", TestWindowsBackendFailurePropagationAndOrdering },
		{ "WindowsBackendRetriesPreparedFrame", TestWindowsBackendRetriesPreparedFrame },
		{ "WindowsLoopbackPresentFailure", TestWindowsLoopbackPresentFailure },
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
