#pragma once
#include <stdexcept>
#include "Submodules/EditorRemote/RemoteViewportRuntime.h"

namespace Sailor::Tests
{
	using namespace EditorRemote;

	inline void RequireViewport(bool condition, const char* message)
	{
		if (!condition) throw std::runtime_error(message);
	}

	template<typename TBinding, typename TProvider, typename TPresenter>
	void TestViewportResizeAndRecoveryLoop(const ViewportDescriptor& viewport)
	{
		TProvider provider;
		TPresenter presenter;
		TBinding binding(viewport, provider, presenter, 17);
		auto& session = binding.GetRuntimeSession();
		RequireViewport(binding.Create().IsOk() && binding.SetFocused(true).IsOk() && binding.PumpFrame().IsOk(),
			"resize loop must start with a focused, presented viewport");
		for (uint32_t resize = 1; resize <= 32; ++resize)
		{
			const auto oldInput = *session.GetLastInput();
			const auto width = viewport.m_width + resize * 8;
			const auto height = viewport.m_height + resize * 4;
			RequireViewport(binding.Resize(width, height).IsOk() && binding.PumpFrame().IsOk(),
				"each resize must import and present its new surface");
			const auto& frame = presenter.m_presentCalls.back();
			RequireViewport(session.GetGeneration() == resize + 1 && frame.m_generation == session.GetGeneration() &&
				frame.m_connectionEpoch == 17 && frame.m_frameIndex == 1 && frame.m_width == width && frame.m_height == height &&
				presenter.m_importViewport == session.GetDescriptor() && presenter.m_importGeneration == frame.m_generation,
				"repeated resize must keep runtime, imported surface and presented frame on the same generation and extent");
			RequireViewport(!session.IsInputCurrent(oldInput) && !session.HandleInput(oldInput).IsOk() &&
				session.GetLastInput()->m_focused && session.GetLastInput()->m_generation == frame.m_generation,
				"queued old-generation input must be rejected without replacing the restored focus");
			RequireViewport(provider.m_liveSurfaces.size() == 1 && binding.GetTransportBackend().GetSurfaceCount() == 1,
				"a resize storm must retire every superseded allocation");
		}

		RequireViewport(binding.SetVisible(false).IsOk(), "recovery loop must preserve hidden state");
		const auto presents = presenter.m_presentCalls.size();
		const auto descriptor = session.GetDescriptor();
		for (ConnectionEpoch epoch = 18; epoch < 24; ++epoch)
		{
			const auto oldInput = *session.GetLastInput();
			RequireViewport(session.MarkFailure(Failure::FromDomain(ErrorDomain::Session, 2, "recreate viewport")).IsOk() &&
				binding.PumpFrame().IsOk(), "a session failure must recreate its actual backend through the ordinary pump");
			RequireViewport(session.GetConnectionEpoch() == epoch && session.GetGeneration() == 1 &&
				session.GetDescriptor() == descriptor && session.GetState() == SessionState::Paused &&
				presenter.m_importEpoch == epoch && presenter.m_importViewport == descriptor &&
				presenter.m_presentCalls.size() == presents && session.GetLastPublishedFrameIndex() == 0,
				"hidden recovery must retain the latest extent and avoid presenting a prior-epoch frame");
			RequireViewport(!session.IsInputCurrent(oldInput) && !session.HandleInput(oldInput).IsOk() &&
				session.GetLastInput()->m_focused && session.GetLastInput()->m_connectionEpoch == epoch &&
				provider.m_liveSurfaces.size() == 1 && binding.GetTransportBackend().GetSurfaceCount() == 1,
				"each recovery must invalidate old input, restore focus and keep exactly one native surface");
		}
		RequireViewport(binding.SetVisible(true).IsOk() && binding.PumpFrame().IsOk() &&
			presenter.m_presentCalls.size() == presents + 1 && presenter.m_presentCalls.back().m_connectionEpoch == 23 &&
			presenter.m_presentCalls.back().m_frameIndex == 1, "showing the recovered viewport must present the current epoch");
		RequireViewport(binding.Destroy().IsOk() && provider.m_liveSurfaces.empty(), "loop fixture must release all its surfaces");
	}

	template<typename TBinding, typename TProvider, typename TPresenter>
	void TestViewportFrameFlood(const ViewportDescriptor& viewport)
	{
		TProvider provider;
		TPresenter presenter;
		TBinding binding(viewport, provider, presenter);
		auto& session = binding.GetRuntimeSession();
		RequireViewport(binding.Create().IsOk(), "frame flood must create its real transport binding");
		constexpr size_t frames = 128;
		for (size_t frame = 1; frame <= frames; ++frame)
		{
			RequireViewport(binding.PumpFrame().IsOk() && presenter.m_presentCalls.size() == frame &&
				presenter.m_presentCalls.back().m_frameIndex == frame &&
				presenter.m_presentCalls.back().m_generation == session.GetGeneration(),
				"each pump must reach the presenter exactly once with the newest frame");
		}
		RequireViewport(provider.m_beginCalls.size() == frames && provider.m_exportCalls.size() == frames &&
			session.GetDiagnostics().m_lastGoodFrameIndex == frames && session.GetState() == SessionState::Active &&
			provider.m_createCalls.size() == 1 && provider.m_liveSurfaces.size() == 1,
			"steady frame traffic must not duplicate native preparation, recreate surfaces or drift diagnostics");
		RequireViewport(binding.SetVisible(false).IsOk(), "frame flood viewport must hide");
		for (size_t frame = 0; frame < 32; ++frame) RequireViewport(binding.PumpFrame().IsOk(), "hidden pumping must be harmless");
		RequireViewport(presenter.m_presentCalls.size() == frames && provider.m_beginCalls.size() == frames &&
			provider.m_exportCalls.size() == frames, "a hidden binding must not schedule or present native frames");
		RequireViewport(binding.SetVisible(true).IsOk() && binding.PumpFrame().IsOk() &&
			presenter.m_presentCalls.back().m_frameIndex == frames + 1 && presenter.m_presentCalls.size() == frames + 1,
			"resuming must advance from the last actual frame without queued hidden work");
		RequireViewport(binding.Destroy().IsOk() && provider.m_liveSurfaces.empty() && !binding.PumpFrame().IsOk(),
			"destroyed frame flood must release its surface and stop presentation");
	}

	template<typename TBinding, typename TProvider, typename TPresenter>
	void TestViewportPresentFailure(const ViewportDescriptor& viewport)
	{
		TProvider provider;
		TPresenter presenter;
		TBinding binding(viewport, provider, presenter);
		RequireViewport(binding.Create().IsOk() && binding.PumpFrame().IsOk(), "presentation fixture must show its first frame");
		InputPacket input{};
		input.m_kind = InputKind::PointerMove;
		auto& session = binding.GetRuntimeSession();
		RequireViewport(session.StampAndHandleInput(input).IsOk() && binding.SetFocused(true).IsOk() &&
			session.GetLastInput()->m_timestampNs > input.m_timestampNs && session.GetLastInput()->m_focused,
			"focus and forwarded input must use the same session timestamp sequence");
		const auto first = presenter.m_presentCalls.back();
		presenter.m_nextPresentFailure = Failure::FromDomain(ErrorDomain::Transport, 921, "present refused");
		auto result = binding.PumpFrame();
		RequireViewport(!result.IsOk() && result.m_nativeCode == 921, "presenter failure must reach the live binding caller");
		RequireViewport(binding.PumpFrame().IsOk() && presenter.m_presentCalls.back().m_frameIndex > first.m_frameIndex &&
			presenter.m_presentCalls.back().m_generation == first.m_generation, "presentation must resume on the same imported surface");
		const auto attempts = presenter.m_presentCalls.size();
		RequireViewport(binding.Destroy().IsOk() && !presenter.m_resets.empty() &&
			presenter.m_resets.back() == viewport.m_viewportId && provider.m_liveSurfaces.empty(),
			"destroy must release provider and presenter state");
		RequireViewport(!binding.PumpFrame().IsOk() && presenter.m_presentCalls.size() == attempts,
			"a disposed binding must not forward another frame to the presenter");
	}

	template<typename TBinding, typename TProvider, typename TPresenter>
	void TestViewportResizeIsTransactional(const ViewportDescriptor& viewport)
	{
		enum class FailurePoint { Create, PartialCreate, InvalidTransport, ExtentMismatch, Import, ImportCleanup, PartialCreateCleanup };
		for (bool hidden : { false, true })
		{
			for (auto failurePoint : { FailurePoint::Create, FailurePoint::PartialCreate, FailurePoint::InvalidTransport,
				FailurePoint::ExtentMismatch, FailurePoint::Import, FailurePoint::ImportCleanup, FailurePoint::PartialCreateCleanup })
			{
				TProvider provider;
				TPresenter presenter;
				TBinding binding{ viewport, provider, presenter, 7 };
				auto& session = binding.GetRuntimeSession();
				RequireViewport(binding.Create().IsOk() && binding.PumpFrame().IsOk() && binding.SetFocused(true).IsOk(),
					"resize fixture must start with a presented, focused viewport");
				RequireViewport(binding.SetVisible(!hidden).IsOk(), "fixture visibility must be applied");
				const auto original = session.GetDescriptor();
				const auto state = hidden ? SessionState::Paused : SessionState::Active;
				presenter.m_onImport = [&]()
				{
					RequireViewport(session.GetGeneration() == 1 && session.GetDescriptor() == original &&
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
					RequireViewport(!binding.Resize(1600, 900).IsOk(), "injected resize failure must reach the caller");
					RequireViewport(session.GetDescriptor() == original && session.GetGeneration() == 1 &&
						session.GetState() == state && session.IsReady() && presenter.m_importGeneration == 1,
						"failed resize must leave the original host and runtime usable");
					const size_t expectedSurfaces = failurePoint == FailurePoint::ImportCleanup ||
						failurePoint == FailurePoint::PartialCreateCleanup ? 2 : 1;
					RequireViewport(binding.GetTransportBackend().GetSurfaceCount() == expectedSurfaces &&
						provider.m_liveSurfaces.size() == expectedSurfaces,
						"only an explicitly failed release may retain a candidate allocation");
					RequireViewport(binding.PumpFrame().IsOk() && binding.GetTransportBackend().GetSurfaceCount() == 1 &&
						provider.m_liveSurfaces.size() == 1 && presenter.m_presentCalls.back().m_generation == 1,
						"pumping must retire leftovers and preserve old-generation presentation");
				}
				RequireViewport(binding.Resize(1600, 900).IsOk(), "resize must succeed after the failure is cleared");
				presenter.m_onImport = {};
				RequireViewport(session.GetGeneration() == 2 && presenter.m_importGeneration == 2 &&
					session.GetDescriptor().m_width == 1600 && session.GetDescriptor().m_height == 900 &&
					presenter.m_importViewport == session.GetDescriptor() && session.GetState() == state &&
					binding.GetTransportBackend().GetSurfaceCount() == 1 && provider.m_liveSurfaces.size() == 1 &&
					session.GetLastInput()->m_focused && session.GetLastInput()->m_generation == 2,
					"successful import must commit matching extents, generation, focus and visibility once");
				RequireViewport(binding.SetVisible(true).IsOk(), "resized viewport must become visible");
				provider.m_nextExportFailure = Failure::FromDomain(ErrorDomain::Transport, 902, "export failed");
				RequireViewport(!binding.PumpFrame().IsOk() && session.GetGeneration() == 2 && presenter.m_importGeneration == 2,
					"export failure must not revert or leak the committed resize");
				RequireViewport(binding.PumpFrame().IsOk() && presenter.m_presentCalls.back().m_generation == 2,
					"the committed generation must present after export retry");
				provider.m_nextReleaseFailure = Failure::FromDomain(ErrorDomain::Transport, 903, "release failed");
				RequireViewport(!binding.Destroy().IsOk() && session.GetState() == SessionState::Disposed &&
					binding.GetTransportBackend().GetSurfaceCount() == 1,
					"failed destroy release must keep the allocation available for cleanup");
				RequireViewport(binding.Destroy().IsOk() && binding.GetTransportBackend().GetSurfaceCount() == 0 &&
					provider.m_liveSurfaces.empty(), "destroy retry must release every tracked allocation");
				const auto creates = provider.m_createCalls.size();
				RequireViewport(!binding.Resize(320, 240).IsOk() && provider.m_createCalls.size() == creates,
					"disposed resize must not allocate another surface");
			}
		}
	}

	template<typename TBinding, typename TProvider, typename TPresenter>
	void TestViewportImportAndRetirementFailures(const ViewportDescriptor& viewport)
	{
		for (bool releaseFails : { false, true })
		{
			TProvider provider;
			TPresenter presenter;
			TBinding binding{ viewport, provider, presenter, 7 };
			auto& session = binding.GetRuntimeSession();
			presenter.m_onImport = [&]()
			{
				RequireViewport(!session.IsReady() && session.GetState() == SessionState::Negotiating,
					"initial surface allocation must not make the viewport ready before host import");
			};
			presenter.m_nextImportFailure = Failure::FromDomain(ErrorDomain::Session, 904, "import refused");
			if (releaseFails) provider.m_nextReleaseFailure = Failure::FromDomain(ErrorDomain::Transport, 905, "release deferred");
			RequireViewport(!binding.Create().IsOk() && !session.IsReady() && session.GetState() == SessionState::Recovering &&
				binding.GetTransportBackend().GetSurfaceCount() == (releaseFails ? 1u : 0u) &&
				provider.m_liveSurfaces.size() == (releaseFails ? 1u : 0u),
				"failed initial import must leave no ready session and track only failed cleanup");
			RequireViewport(binding.PumpFrame().IsOk() && session.IsReady() && session.GetConnectionEpoch() == 8 &&
				presenter.m_importEpoch == 8 && presenter.m_presentCalls.back().m_connectionEpoch == 8 &&
				binding.GetTransportBackend().GetSurfaceCount() == 1 && provider.m_liveSurfaces.size() == 1,
				"initial import retry must clean leftovers before creating and presenting the new epoch");
			RequireViewport(binding.Destroy().IsOk() && provider.m_liveSurfaces.empty(), "import retry fixture must release all surfaces");
		}

		TProvider provider;
		TPresenter presenter;
		TBinding binding{ viewport, provider, presenter };
		auto& session = binding.GetRuntimeSession();
		RequireViewport(binding.Create().IsOk(), "retirement fixture must create a viewport");
		const auto failure = Failure::FromDomain(ErrorDomain::Transport, 906, "old surface release deferred");
		provider.m_nextReleaseFailure = failure;
		RequireViewport(!binding.Resize(1600, 900).IsOk() && session.GetGeneration() == 2 && session.IsReady() &&
			presenter.m_importGeneration == 2 && presenter.m_importViewport == session.GetDescriptor() &&
			binding.GetTransportBackend().GetSurfaceCount() == 2 && provider.m_liveSurfaces.size() == 2,
			"old-surface retirement failure must retain the successfully imported new generation");
		const auto creates = provider.m_createCalls.size();
		for (uint32_t attempt = 0; attempt < 3; ++attempt)
		{
			provider.m_nextReleaseFailure = failure;
			RequireViewport(!binding.Resize(1920, 1080).IsOk() && provider.m_createCalls.size() == creates &&
				session.GetGeneration() == 2 && presenter.m_importGeneration == 2 && provider.m_liveSurfaces.size() == 2,
				"repeated failed cleanup must not allocate more surfaces or change the active generation");
		}
		RequireViewport(binding.PumpFrame().IsOk() && presenter.m_presentCalls.back().m_generation == 2 &&
			binding.GetTransportBackend().GetSurfaceCount() == 1 && provider.m_liveSurfaces.size() == 1,
			"ordinary pump must retire the old surface and present the committed generation");
		RequireViewport(binding.Resize(1920, 1080).IsOk() && session.GetGeneration() == 3 &&
			presenter.m_importGeneration == 3 && provider.m_liveSurfaces.size() == 1,
			"resize must resume after successful retirement");
		RequireViewport(binding.Destroy().IsOk() && provider.m_liveSurfaces.empty(), "retirement fixture must release every surface");
	}

	template<typename TBinding, typename TProvider, typename TPresenter>
	void TestViewportRecoveryUsesElapsedTime(const ViewportDescriptor& viewport)
	{
		constexpr uint64_t start = 3'600'000;
		for (size_t pumpCount : { size_t{1}, size_t{2000} })
		{
			TProvider provider;
			TPresenter presenter;
			TBinding binding{ viewport, provider, presenter, 17 };
			auto& session = binding.GetRuntimeSession();
			const auto createFailure = Failure::FromDomain(ErrorDomain::Session, 2, "surface unavailable");
			provider.m_nextCreateFailure = createFailure;
			RequireViewport(!binding.Create(start).IsOk() && session.GetState() == SessionState::Negotiating,
				"failed creation must retain a pending negotiation");
			for (size_t pump = 0; pump < pumpCount; ++pump)
			{
				RequireViewport(!binding.PumpFrame(start + 999).IsOk() && provider.m_createCalls.size() == 1,
					"pump frequency must not advance time or repeat surface creation");
			}
			provider.m_nextCreateFailure = createFailure;
			RequireViewport(!binding.PumpFrame(start + 1000).IsOk() && session.GetConnectionEpoch() == 18 &&
				provider.m_createCalls.size() == 2, "timeout must attempt a real recreation in a fresh epoch");
			RequireViewport(!binding.PumpFrame(start + 1999).IsOk() && provider.m_createCalls.size() == 2,
				"failed recreation must receive a new deadline");
			RequireViewport(binding.PumpFrame(start + 2000).IsOk() && session.GetState() == SessionState::Active &&
				session.GetConnectionEpoch() == 19 && presenter.m_importEpoch == 19 &&
				presenter.m_presentCalls.size() == 1 && presenter.m_presentCalls.back().m_connectionEpoch == 19,
				"recovery must import and present a real frame, not return a successful no-op");
			RequireViewport(binding.GetTransportBackend().GetSurfaceCount() == 1, "retries must not accumulate surfaces");

			RequireViewport(binding.SetFocused(true).IsOk() && session.MarkFailure(createFailure, start + 2100).IsOk() &&
				binding.SetVisible(false).IsOk(), "hidden recovery must preserve desired visibility and focus");
			provider.m_nextReleaseFailure = Failure::FromDomain(ErrorDomain::Session, 1, "release deferred");
			RequireViewport(!binding.PumpFrame(start + 2101).IsOk() && session.GetConnectionEpoch() == 19 &&
				binding.GetTransportBackend().GetSurfaceCount() == 1,
				"failed release must not discard the old resource identity");
			RequireViewport(binding.PumpFrame(start + 2102).IsOk() && session.GetState() == SessionState::Paused &&
				session.GetConnectionEpoch() == 20 && presenter.m_presentCalls.size() == 1 &&
				session.GetLastInput().has_value() && session.GetLastInput()->m_focused &&
				session.GetLastInput()->m_connectionEpoch == 20,
				"successful recreation must restore focus but not present a hidden viewport");
			RequireViewport(binding.SetVisible(true).IsOk() && binding.PumpFrame(start + 60'000).IsOk() &&
				presenter.m_presentCalls.size() == 2 && session.GetConnectionEpoch() == 20,
				"ready acknowledgement must cancel the timeout and resume normal presentation");

			RequireViewport(session.MarkFailure(Failure::FromDomain(ErrorDomain::Connection, 1, "disconnected"),
				start + 61'000).IsOk(), "connection loss must remain a connection-level failure");
			const auto creates = provider.m_createCalls.size();
			RequireViewport(!binding.PumpFrame(start + 61'001).IsOk() && session.GetState() == SessionState::Lost &&
				provider.m_createCalls.size() == creates, "a lost connection needs reconnection, not automatic local recreation");
			RequireViewport(binding.Destroy().IsOk() && binding.GetTransportBackend().GetSurfaceCount() == 0 &&
				!binding.PumpFrame(start + 70'000).IsOk() && provider.m_createCalls.size() == creates,
				"destroy must release recovery resources and prevent resurrection");
		}
	}

}
