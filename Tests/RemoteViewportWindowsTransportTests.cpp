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
		enum class FailurePoint { Create, PartialCreate, InvalidTransport, ExtentMismatch, Import, ImportCleanup, PartialCreateCleanup };
		for (bool hidden : { false, true })
		{
			for (auto failurePoint : { FailurePoint::Create, FailurePoint::PartialCreate, FailurePoint::InvalidTransport,
				FailurePoint::ExtentMismatch, FailurePoint::Import, FailurePoint::ImportCleanup, FailurePoint::PartialCreateCleanup })
			{
				FakeWindowsSharedSurfaceProvider provider;
				FakeWindowsViewportPresenter presenter;
				WindowsViewportLoopbackBinding binding{ MakeViewport(), provider, presenter, 7 };
				auto& session = binding.GetRuntimeSession();
				Require(binding.Create().IsOk() && binding.PumpFrame().IsOk() && binding.SetFocused(true).IsOk(),
					"resize fixture must start with a presented, focused viewport");
				Require(binding.SetVisible(!hidden).IsOk(), "fixture visibility must be applied");
				const auto original = session.GetDescriptor();
				const auto state = hidden ? SessionState::Paused : SessionState::Active;
				presenter.m_onImport = [&]()
				{
					Require(session.GetGeneration() == 1 && session.GetDescriptor() == original &&
						session.GetState() == state && session.IsReady(),
						"candidate import must not publish a new runtime generation or discard the old ready state");
				};
				for (uint32_t attempt = 0; attempt < 3; ++attempt)
				{
					const auto failure = Failure::FromDomain(ErrorDomain::Transport, 901, "injected resize failure");
					switch (failurePoint)
					{
					case FailurePoint::Create: provider.m_nextCreateFailure = failure; break;
					case FailurePoint::PartialCreate: provider.m_nextCreatedFailure = failure; break;
					case FailurePoint::InvalidTransport: provider.m_invalidTransport = true; break;
					case FailurePoint::ExtentMismatch: provider.m_mismatchedExtent = true; break;
					case FailurePoint::Import: presenter.m_nextImportFailure = failure; break;
					case FailurePoint::ImportCleanup:
						presenter.m_nextImportFailure = failure;
						provider.m_nextReleaseFailure = failure;
						break;
					case FailurePoint::PartialCreateCleanup:
						provider.m_nextCreatedFailure = failure;
						provider.m_nextReleaseFailure = failure;
						break;
					}
					Require(!binding.Resize(1600, 900).IsOk(), "injected resize failure must reach the caller");
					Require(session.GetDescriptor() == original && session.GetGeneration() == 1 &&
						session.GetState() == state && session.IsReady() && presenter.m_importGeneration == 1,
						"failed resize must leave the original host and runtime usable");
					const size_t expectedSurfaces = failurePoint == FailurePoint::ImportCleanup ||
						failurePoint == FailurePoint::PartialCreateCleanup ? 2 : 1;
					Require(binding.GetTransportBackend().GetSurfaceCount() == expectedSurfaces &&
						provider.m_liveSurfaces.size() == expectedSurfaces,
						"only an explicitly failed release may retain a candidate allocation");
					Require(binding.PumpFrame().IsOk() && binding.GetTransportBackend().GetSurfaceCount() == 1 &&
						provider.m_liveSurfaces.size() == 1 && presenter.m_presentCalls.back().m_generation == 1,
						"pumping must retire leftovers and preserve old-generation presentation");
				}
				Require(binding.Resize(1600, 900).IsOk(), "resize must succeed after the failure is cleared");
				presenter.m_onImport = {};
				Require(session.GetGeneration() == 2 && presenter.m_importGeneration == 2 &&
					session.GetDescriptor().m_width == 1600 && session.GetDescriptor().m_height == 900 &&
					presenter.m_importViewport == session.GetDescriptor() && session.GetState() == state &&
					binding.GetTransportBackend().GetSurfaceCount() == 1 && provider.m_liveSurfaces.size() == 1 &&
					session.GetLastInput()->m_focused && session.GetLastInput()->m_generation == 2,
					"successful import must commit matching extents, generation, focus and visibility once");
				Require(binding.SetVisible(true).IsOk(), "resized viewport must become visible");
				provider.m_nextExportFailure = Failure::FromDomain(ErrorDomain::Transport, 902, "export failed");
				Require(!binding.PumpFrame().IsOk() && session.GetGeneration() == 2 && presenter.m_importGeneration == 2,
					"export failure must not revert or leak the committed resize");
				Require(binding.PumpFrame().IsOk() && presenter.m_presentCalls.back().m_generation == 2,
					"the committed generation must present after export retry");
				provider.m_nextReleaseFailure = Failure::FromDomain(ErrorDomain::Transport, 903, "release failed");
				Require(!binding.Destroy().IsOk() && session.GetState() == SessionState::Disposed &&
					binding.GetTransportBackend().GetSurfaceCount() == 1,
					"failed destroy release must keep the allocation available for cleanup");
				Require(binding.Destroy().IsOk() && binding.GetTransportBackend().GetSurfaceCount() == 0 &&
					provider.m_liveSurfaces.empty(), "destroy retry must release every tracked allocation");
				const auto creates = provider.m_createCalls.size();
				Require(!binding.Resize(320, 240).IsOk() && provider.m_createCalls.size() == creates,
					"disposed resize must not allocate another surface");
			}
		}
	}

	void TestWindowsLoopbackImportAndRetirementFailures()
	{
		for (bool releaseFails : { false, true })
		{
			FakeWindowsSharedSurfaceProvider provider;
			FakeWindowsViewportPresenter presenter;
			WindowsViewportLoopbackBinding binding{ MakeViewport(), provider, presenter, 7 };
			auto& session = binding.GetRuntimeSession();
			presenter.m_onImport = [&]()
			{
				Require(!session.IsReady() && session.GetState() == SessionState::Negotiating,
					"initial surface allocation must not make the viewport ready before host import");
			};
			presenter.m_nextImportFailure = Failure::FromDomain(ErrorDomain::Session, 904, "import refused");
			if (releaseFails) provider.m_nextReleaseFailure = Failure::FromDomain(ErrorDomain::Transport, 905, "release deferred");
			Require(!binding.Create().IsOk() && !session.IsReady() && session.GetState() == SessionState::Recovering &&
				binding.GetTransportBackend().GetSurfaceCount() == (releaseFails ? 1u : 0u) &&
				provider.m_liveSurfaces.size() == (releaseFails ? 1u : 0u),
				"failed initial import must leave no ready session and track only failed cleanup");
			Require(binding.PumpFrame().IsOk() && session.IsReady() && session.GetConnectionEpoch() == 8 &&
				presenter.m_importEpoch == 8 && presenter.m_presentCalls.back().m_connectionEpoch == 8 &&
				binding.GetTransportBackend().GetSurfaceCount() == 1 && provider.m_liveSurfaces.size() == 1,
				"initial import retry must clean leftovers before creating and presenting the new epoch");
			Require(binding.Destroy().IsOk() && provider.m_liveSurfaces.empty(), "import retry fixture must release all surfaces");
		}

		FakeWindowsSharedSurfaceProvider provider;
		FakeWindowsViewportPresenter presenter;
		WindowsViewportLoopbackBinding binding{ MakeViewport(), provider, presenter };
		auto& session = binding.GetRuntimeSession();
		Require(binding.Create().IsOk(), "retirement fixture must create a viewport");
		const auto failure = Failure::FromDomain(ErrorDomain::Transport, 906, "old surface release deferred");
		provider.m_nextReleaseFailure = failure;
		Require(!binding.Resize(1600, 900).IsOk() && session.GetGeneration() == 2 && session.IsReady() &&
			presenter.m_importGeneration == 2 && presenter.m_importViewport == session.GetDescriptor() &&
			binding.GetTransportBackend().GetSurfaceCount() == 2 && provider.m_liveSurfaces.size() == 2,
			"old-surface retirement failure must retain the successfully imported new generation");
		const auto creates = provider.m_createCalls.size();
		for (uint32_t attempt = 0; attempt < 3; ++attempt)
		{
			provider.m_nextReleaseFailure = failure;
			Require(!binding.Resize(1920, 1080).IsOk() && provider.m_createCalls.size() == creates &&
				session.GetGeneration() == 2 && presenter.m_importGeneration == 2 && provider.m_liveSurfaces.size() == 2,
				"repeated failed cleanup must not allocate more surfaces or change the active generation");
		}
		Require(binding.PumpFrame().IsOk() && presenter.m_presentCalls.back().m_generation == 2 &&
			binding.GetTransportBackend().GetSurfaceCount() == 1 && provider.m_liveSurfaces.size() == 1,
			"ordinary pump must retire the old surface and present the committed generation");
		Require(binding.Resize(1920, 1080).IsOk() && session.GetGeneration() == 3 &&
			presenter.m_importGeneration == 3 && provider.m_liveSurfaces.size() == 1,
			"resize must resume after successful retirement");
		Require(binding.Destroy().IsOk() && provider.m_liveSurfaces.empty(), "retirement fixture must release every surface");
	}

	void TestWindowsLoopbackRecoveryUsesElapsedTime()
	{
		constexpr uint64_t start = 3'600'000;
		for (size_t pumpCount : { size_t{1}, size_t{2000} })
		{
			FakeWindowsSharedSurfaceProvider provider;
			FakeWindowsViewportPresenter presenter;
			WindowsViewportLoopbackBinding binding{ MakeViewport(), provider, presenter, 17 };
			auto& session = binding.GetRuntimeSession();
			const auto createFailure = Failure::FromDomain(ErrorDomain::Session, 2, "surface unavailable");
			provider.m_nextCreateFailure = createFailure;
			Require(!binding.Create(start).IsOk() && session.GetState() == SessionState::Negotiating,
				"failed creation must retain a pending negotiation");
			for (size_t pump = 0; pump < pumpCount; ++pump)
			{
				Require(!binding.PumpFrame(start + 999).IsOk() && provider.m_createCalls.size() == 1,
					"pump frequency must not advance time or repeat surface creation");
			}
			provider.m_nextCreateFailure = createFailure;
			Require(!binding.PumpFrame(start + 1000).IsOk() && session.GetConnectionEpoch() == 18 &&
				provider.m_createCalls.size() == 2, "timeout must attempt a real recreation in a fresh epoch");
			Require(!binding.PumpFrame(start + 1999).IsOk() && provider.m_createCalls.size() == 2,
				"failed recreation must receive a new deadline");
			Require(binding.PumpFrame(start + 2000).IsOk() && session.GetState() == SessionState::Active &&
				session.GetConnectionEpoch() == 19 && presenter.m_importEpoch == 19 &&
				presenter.m_presentCalls.size() == 1 && presenter.m_presentCalls.back().m_connectionEpoch == 19,
				"recovery must import and present a real frame, not return a successful no-op");
			Require(binding.GetTransportBackend().GetSurfaceCount() == 1, "retries must not accumulate surfaces");

			Require(binding.SetFocused(true).IsOk() && session.MarkFailure(createFailure, start + 2100).IsOk() &&
				binding.SetVisible(false).IsOk(), "hidden recovery must preserve desired visibility and focus");
			provider.m_nextReleaseFailure = Failure::FromDomain(ErrorDomain::Session, 1, "release deferred");
			Require(!binding.PumpFrame(start + 2101).IsOk() && session.GetConnectionEpoch() == 19 &&
				binding.GetTransportBackend().GetSurfaceCount() == 1,
				"failed release must not discard the old resource identity");
			Require(binding.PumpFrame(start + 2102).IsOk() && session.GetState() == SessionState::Paused &&
				session.GetConnectionEpoch() == 20 && presenter.m_presentCalls.size() == 1 &&
				session.GetLastInput().has_value() && session.GetLastInput()->m_focused &&
				session.GetLastInput()->m_connectionEpoch == 20,
				"successful recreation must restore focus but not present a hidden viewport");
			Require(binding.SetVisible(true).IsOk() && binding.PumpFrame(start + 60'000).IsOk() &&
				presenter.m_presentCalls.size() == 2 && session.GetConnectionEpoch() == 20,
				"ready acknowledgement must cancel the timeout and resume normal presentation");

			Require(session.MarkFailure(Failure::FromDomain(ErrorDomain::Connection, 1, "disconnected"),
				start + 61'000).IsOk(), "connection loss must remain a connection-level failure");
			const auto creates = provider.m_createCalls.size();
			Require(!binding.PumpFrame(start + 61'001).IsOk() && session.GetState() == SessionState::Lost &&
				provider.m_createCalls.size() == creates, "a lost connection needs reconnection, not automatic local recreation");
			Require(binding.Destroy().IsOk() && binding.GetTransportBackend().GetSurfaceCount() == 0 &&
				!binding.PumpFrame(start + 70'000).IsOk() && provider.m_createCalls.size() == creates,
				"destroy must release recovery resources and prevent resurrection");
		}
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
		{ "WindowsLoopbackRecoveryUsesElapsedTime", TestWindowsLoopbackRecoveryUsesElapsedTime },
		{ "WindowsLoopbackResizeIsTransactional", TestWindowsLoopbackResizeIsTransactional },
		{ "WindowsLoopbackImportAndRetirementFailures", TestWindowsLoopbackImportAndRetirementFailures },
		{ "WindowsBackendFailurePropagationAndOrdering", TestWindowsBackendFailurePropagationAndOrdering },
		{ "WindowsBackendRetriesPreparedFrame", TestWindowsBackendRetriesPreparedFrame },
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
