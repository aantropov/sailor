#pragma once
#include "RemoteViewportRuntime.h"
#include <algorithm>
#include <utility>

namespace Sailor::EditorRemote
{
	template<typename TBackend, typename TPresenter>
	class TViewportLoopbackBinding
	{
	public:
		TViewportLoopbackBinding(ViewportDescriptor descriptor, typename TBackend::Provider& provider,
			TPresenter& presenter, ConnectionEpoch epoch = 1) :
			m_transportBackend(provider), m_presenter(presenter), m_runtimeSession(std::move(descriptor), epoch)
		{}

		RemoteViewportSession& GetRuntimeSession() { return m_runtimeSession; }
		const RemoteViewportSession& GetRuntimeSession() const { return m_runtimeSession; }
		TBackend& GetTransportBackend() { return m_transportBackend; }
		const TBackend& GetTransportBackend() const { return m_transportBackend; }

		Failure Create(uint64_t nowMs = GetMonotonicTimeMs())
		{
			if (m_created && (m_runtimeSession.GetState() == SessionState::Lost ||
				m_runtimeSession.GetState() == SessionState::Recovering)) return RecreateTransport(nowMs);
			auto result = m_runtimeSession.BeginNegotiation(nowMs);
			if (!result.IsOk()) return result;
			m_visible = true;
			m_focused = false;
			m_created = true;
			return EnsureTransportImported();
		}

		Failure Resize(uint32_t width, uint32_t height, uint64_t nowMs = GetMonotonicTimeMs())
		{
			const auto epoch = m_runtimeSession.GetConnectionEpoch();
			const auto generation = m_runtimeSession.GetGeneration();
			auto descriptor = m_runtimeSession.GetDescriptor();
			descriptor.m_width = std::max(width, 1u);
			descriptor.m_height = std::max(height, 1u);
			auto result = m_runtimeSession.ValidateResize(descriptor);
			if (!result.IsOk()) return result;
			result = m_transportBackend.ReleaseSurfaces(descriptor.m_viewportId, epoch, generation);
			if (!result.IsOk()) return result;

			TransportDescriptor transport;
			result = m_transportBackend.EnsureSurface(descriptor, epoch, generation + 1, transport);
			if (!result.IsOk()) return result;
			result = m_transportBackend.ImportSurface(m_presenter, descriptor, transport, epoch, generation + 1);
			if (!result.IsOk())
			{
				m_transportBackend.ReleaseSurface(descriptor.m_viewportId, epoch, generation + 1);
				return result;
			}

			// Keep the old generation usable until native import succeeds.
			m_runtimeSession.HandleResize(descriptor, nowMs);
			result = CompleteTransportImport(transport);
			if (!result.IsOk()) return result;
			return m_transportBackend.ReleaseSurfaces(descriptor.m_viewportId, epoch, generation + 1);
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
			input.m_kind = InputKind::Focus;
			input.m_focused = focused;
			return m_runtimeSession.StampAndHandleInput(input);
		}

		Failure PumpFrame(uint64_t nowMs = GetMonotonicTimeMs())
		{
			if (!m_created)
				return Failure::FromDomain(ErrorDomain::Session, 1, "Viewport must be created before pumping frames");
			auto result = m_runtimeSession.TickTimeouts(nowMs);
			if (!result.IsOk()) return result;
			if (m_runtimeSession.GetState() == SessionState::Recovering)
			{
				result = RecreateTransport(nowMs);
				if (!result.IsOk()) return result;
			}
			if (m_transportBackend.GetSurfaceCount() > 1)
			{
				result = m_transportBackend.ReleaseSurfaces(m_runtimeSession.GetViewportId(),
					m_runtimeSession.GetConnectionEpoch(), m_runtimeSession.GetGeneration());
				if (!result.IsOk()) return result;
			}
			if (m_runtimeSession.GetState() != SessionState::Active) return m_runtimeSession.GetFailure();

			if (!m_bHasPendingPresentation)
			{
				bool ready = false;
				result = m_transportBackend.PrepareFrame(m_runtimeSession.GetDescriptor(),
					m_runtimeSession.GetConnectionEpoch(), m_runtimeSession.GetGeneration(), ready);
				if (!result.IsOk()) return HandleFrameFailure(result, nowMs);
				if (!ready) return result;
				result = m_runtimeSession.PublishFrameFromBackend(m_transportBackend);
				if (!result.IsOk()) return HandleFrameFailure(result, nowMs);
				m_bHasPendingPresentation = true;
			}
			// The producer cannot reuse the shared surface until this frame is released.
			result = m_presenter.PresentFrame(m_runtimeSession.GetViewportId(), m_runtimeSession.GetLastFrame());
			if (!result.IsOk()) return HandleFrameFailure(result, nowMs);
			m_bHasPendingPresentation = false;
			return result;
		}

		Failure Destroy()
		{
			if (!m_created) return Failure::Ok();
			auto release = m_transportBackend.ReleaseSurfaces(m_runtimeSession.GetViewportId());
			m_presenter.ResetViewport(m_runtimeSession.GetViewportId());
			auto destroy = m_runtimeSession.Destroy();
			m_bHasPendingPresentation = false;
			m_created = !release.IsOk();
			return !release.IsOk() ? release : destroy;
		}

	private:
		Failure HandleFrameFailure(const Failure& failure, uint64_t nowMs)
		{
			if (failure.m_code != ResultCode::Retryable) m_runtimeSession.MarkFailure(failure, nowMs);
			return failure;
		}

		Failure RecreateTransport(uint64_t nowMs)
		{
			m_presenter.ResetViewport(m_runtimeSession.GetViewportId());
			auto result = m_transportBackend.ReleaseSurfaces(m_runtimeSession.GetViewportId());
			if (!result.IsOk()) return result;
			result = m_runtimeSession.Recreate(m_runtimeSession.GetConnectionEpoch() + 1, nowMs);
			if (!result.IsOk()) return result;
			return EnsureTransportImported();
		}

		Failure EnsureTransportImported()
		{
			TransportDescriptor transport;
			auto result = m_runtimeSession.EnsureBackendTransport(m_transportBackend, transport);
			if (!result.IsOk()) return result;
			result = m_transportBackend.ImportSurface(m_presenter, m_runtimeSession.GetDescriptor(), transport,
				m_runtimeSession.GetConnectionEpoch(), m_runtimeSession.GetGeneration());
			if (!result.IsOk())
			{
				m_transportBackend.ReleaseSurface(m_runtimeSession.GetViewportId(),
					m_runtimeSession.GetConnectionEpoch(), m_runtimeSession.GetGeneration());
				m_runtimeSession.MarkFailure(result);
				return result;
			}
			return CompleteTransportImport(transport);
		}

		Failure CompleteTransportImport(const TransportDescriptor& transport)
		{
			auto result = m_runtimeSession.MarkTransportReady(transport);
			if (!result.IsOk()) return result;
			m_bHasPendingPresentation = false;
			if (!m_visible)
			{
				result = m_runtimeSession.SetVisible(false);
				if (!result.IsOk()) return result;
			}
			return m_focused ? SetFocused(true) : Failure::Ok();
		}

		TBackend m_transportBackend;
		TPresenter& m_presenter;
		RemoteViewportSession m_runtimeSession;
		bool m_created = false;
		bool m_visible = true;
		bool m_focused = false;
		bool m_bHasPendingPresentation = false;
	};
}
