#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

#include "RemoteViewportFoundation.h"

namespace Sailor::EditorRemote
{
	class IViewportTransportBackend
	{
	public:
		virtual ~IViewportTransportBackend() = default;
		virtual Failure EnsureSurface(const ViewportDescriptor& viewport, ConnectionEpoch epoch, SurfaceGeneration generation, TransportDescriptor& outTransport) = 0;
		virtual Failure BeginFrame(const ViewportDescriptor& viewport, ConnectionEpoch epoch, SurfaceGeneration generation) = 0;
		virtual Failure ExportFrame(const ViewportDescriptor& viewport, ConnectionEpoch epoch, SurfaceGeneration generation, FramePacket& outFrame) = 0;
		virtual Failure ReleaseSurface(ViewportId viewportId, ConnectionEpoch epoch, SurfaceGeneration generation) = 0;
		virtual Failure GetLastFailure() const = 0;
	};

	class RemoteViewportSession
	{
	public:
		explicit RemoteViewportSession(const ViewportDescriptor& descriptor, ConnectionEpoch epoch = 1) :
			m_descriptor(descriptor),
			m_inputViewportId(descriptor.m_viewportId),
			m_connectionEpoch(epoch),
			m_guards(epoch, 1)
		{
		}

		ViewportId GetViewportId() const { return m_descriptor.m_viewportId; }
		ConnectionEpoch GetConnectionEpoch() const { return m_connectionEpoch; }
		const ViewportDescriptor& GetDescriptor() const { return m_descriptor; }
		SurfaceGeneration GetGeneration() const { return m_guards.GetGeneration(); }
		FrameIndex GetLastPublishedFrameIndex() const { return m_lastPublishedFrameIndex; }
		bool IsReady() const { return m_guards.IsTransportReady(); }
		bool IsVisible() const { return m_visible; }
		bool HasFailure() const { return !m_failure.IsOk(); }
		const Failure& GetFailure() const { return m_failure; }
		SessionState GetState() const { return m_state.GetState(); }
		size_t GetInputCount() const { std::lock_guard lock(m_inputMutex); return m_inputCount; }
		std::optional<InputPacket> GetLastInput() const { std::lock_guard lock(m_inputMutex); return m_lastInput; }
		const SessionDiagnostics& GetDiagnostics() const { return m_diagnostics; }
		const FramePacket& GetLastFrame() const { return m_lastFrame; }

		Failure BeginNegotiation(uint64_t nowMs = GetMonotonicTimeMs())
		{
			auto result = m_state.TransitionTo(SessionState::Negotiating);
			if (!result.IsOk()) return result;
			ArmTransportReadyTimeout(nowMs);
			RecordDiagnostic(DiagnosticCategory::Lifecycle, DiagnosticSeverity::Info, "BeginNegotiation");
			return Failure::Ok();
		}

		Failure EnsureBackendTransport(IViewportTransportBackend& backend, TransportDescriptor& transport)
		{
			auto result = backend.EnsureSurface(m_descriptor, m_connectionEpoch, m_guards.GetGeneration(), transport);
			if (!result.IsOk())
			{
				m_failure = backend.GetLastFailure();
				return result;
			}

			m_failure = Failure::Ok();
			return Failure::Ok();
		}

		Failure MarkTransportReady(const TransportDescriptor& transport)
		{
			auto validation = transport.Validate();
			if (!validation.IsOk())
			{
				return validation;
			}

			if (transport.m_width != m_descriptor.m_width || transport.m_height != m_descriptor.m_height)
			{
				return Failure::FromDomain(ErrorDomain::Protocol, 1, "Transport extents do not match viewport descriptor");
			}

			auto readyDecision = m_guards.AcknowledgeTransportReady(m_connectionEpoch, m_guards.GetGeneration());
			if (readyDecision != GuardDecision::Accept)
			{
				auto failure = Failure::FromDomain(ErrorDomain::Protocol, static_cast<int32_t>(readyDecision), "Transport ready acknowledgement rejected");
				m_failure = failure;
				RecordDiagnostic(DiagnosticCategory::Failure, DiagnosticSeverity::Error, "TransportReadyRejected", failure);
				return failure;
			}

			m_transportReadyTimeout.Reset();
			m_reconnectTimeout.Reset();
			m_failure = Failure::Ok();
			m_transportType = transport.m_transportType;
			auto transition = m_state.TransitionTo(SessionState::Ready);
			if (!transition.IsOk())
			{
				return transition;
			}

			auto result = m_visible ? m_state.TransitionTo(SessionState::Active) : m_state.TransitionTo(SessionState::Paused);
			RecordDiagnostic(DiagnosticCategory::Lifecycle, DiagnosticSeverity::Info, "TransportReady");
			return result;
		}

		Failure ValidateResize(const ViewportDescriptor& descriptor) const
		{
			auto validation = descriptor.Validate();
			if (!validation.IsOk())
			{
				return validation;
			}
			if (descriptor.m_viewportId != m_descriptor.m_viewportId)
			{
				return Failure::FromDomain(ErrorDomain::Protocol, 1, "Resize descriptor viewport id mismatch");
			}
			if (m_state.GetState() != SessionState::Resizing &&
				!SessionStateMachine::IsTransitionAllowed(m_state.GetState(), SessionState::Resizing))
			{
				return Failure::FromDomain(ErrorDomain::Protocol, 1, "Session cannot resize in its current state");
			}
			return Failure::Ok();
		}

		Failure HandleResize(const ViewportDescriptor& descriptor, uint64_t nowMs = GetMonotonicTimeMs())
		{
			auto validation = ValidateResize(descriptor);
			if (!validation.IsOk()) return validation;

			auto transition = m_state.TransitionTo(SessionState::Resizing);
			if (!transition.IsOk())
			{
				return transition;
			}

			m_descriptor = descriptor;
			{
				std::lock_guard lock(m_inputMutex);
				m_guards.AdvanceGeneration();
			}
			m_lastPublishedFrameIndex = 0;
			++m_diagnostics.m_resizeCount;
			ArmTransportReadyTimeout(nowMs);
			RecordDiagnostic(DiagnosticCategory::Lifecycle, DiagnosticSeverity::Info, "ResizeRequested");
			return Failure::Ok();
		}

		Failure PublishFrame(FramePacket frame)
		{
			frame.m_viewportId = m_descriptor.m_viewportId;
			frame.m_connectionEpoch = m_connectionEpoch;
			frame.m_generation = m_guards.GetGeneration();
			frame.m_width = m_descriptor.m_width;
			frame.m_height = m_descriptor.m_height;
			frame.m_frameIndex = ++m_lastPublishedFrameIndex;

			auto decision = m_guards.AcceptFrame(frame);
			if (decision != GuardDecision::Accept)
			{
				auto failure = Failure::FromDomain(ErrorDomain::Session, static_cast<int32_t>(decision), "Frame rejected by session guards");
				m_failure = failure;
				RecordDiagnostic(DiagnosticCategory::Failure, DiagnosticSeverity::Warning, "FrameRejected", failure);
				return failure;
			}

			m_lastFrame = frame;
			RefreshDiagnostics();
			RecordDiagnostic(DiagnosticCategory::Lifecycle, DiagnosticSeverity::Info, "FramePublished");
			if (m_state.GetState() == SessionState::Ready)
			{
				return m_state.TransitionTo(SessionState::Active);
			}
			return Failure::Ok();
		}

		Failure PublishFrameFromBackend(IViewportTransportBackend& backend)
		{
			auto beginResult = backend.BeginFrame(m_descriptor, m_connectionEpoch, m_guards.GetGeneration());
			if (!beginResult.IsOk())
			{
				m_failure = backend.GetLastFailure();
				return beginResult;
			}

			FramePacket frame{};
			auto exportResult = backend.ExportFrame(m_descriptor, m_connectionEpoch, m_guards.GetGeneration(), frame);
			if (!exportResult.IsOk())
			{
				m_failure = backend.GetLastFailure();
				return exportResult;
			}

			m_failure = Failure::Ok();
			return PublishFrame(frame);
		}

		Failure HandleInput(const InputPacket& input)
		{
			std::lock_guard lock(m_inputMutex);
			return HandleInputLocked(input);
		}

		// Input only needs session identity, never the frame transport's GPU lock.
		Failure StampAndHandleInput(InputPacket& input)
		{
			std::lock_guard lock(m_inputMutex);
			input.m_viewportId = m_inputViewportId;
			input.m_connectionEpoch = m_connectionEpoch;
			input.m_generation = m_guards.GetGeneration();
			input.m_timestampNs = ++m_inputTimestamp;
			return HandleInputLocked(input);
		}

		bool IsInputCurrent(const InputPacket& input) const
		{
			std::lock_guard lock(m_inputMutex);
			return !m_inputDisposed && input.m_viewportId == m_inputViewportId &&
				m_guards.AcceptInput(input) == GuardDecision::Accept;
		}

		Failure SetVisible(bool visible)
		{
			m_visible = visible;
			if (m_state.GetState() == SessionState::Disposed || !IsReady())
			{
				return Failure::Ok();
			}
			return m_state.TransitionTo(visible ? SessionState::Active : SessionState::Paused);
		}

		Failure MarkFailure(const Failure& failure, uint64_t nowMs = GetMonotonicTimeMs())
		{
			m_failure = failure;
			m_transportReadyTimeout.Reset();
			{
				std::lock_guard lock(m_inputMutex);
				m_guards.ResetTransportReady();
			}
			if (failure.m_scope == FailureScope::Connection)
			{
				++m_recoveryAttemptCount;
				ArmReconnectTimeout(nowMs);
			}
			RecordDiagnostic(DiagnosticCategory::Failure, failure.m_scope == FailureScope::Session ? DiagnosticSeverity::Warning : DiagnosticSeverity::Error, "SessionMarkedFailed", failure);
			auto nextState = failure.m_scope == FailureScope::Session ? SessionState::Recovering : SessionState::Lost;
			return m_state.TransitionTo(nextState);
		}

		Failure Recreate(ConnectionEpoch epoch, uint64_t nowMs = GetMonotonicTimeMs())
		{
			auto transition = m_state.TransitionTo(SessionState::Negotiating);
			if (!transition.IsOk()) return transition;
			++m_recoveryAttemptCount;
			{
				std::lock_guard lock(m_inputMutex);
				m_connectionEpoch = epoch;
				m_guards.BeginNewConnectionEpoch(epoch);
				m_inputDisposed = false;
			}
			m_transportType = TransportType::Unknown;
			ArmTransportReadyTimeout(nowMs);
			m_reconnectTimeout.Reset();
			m_lastPublishedFrameIndex = 0;
			m_failure = Failure::Ok();
			RecordDiagnostic(DiagnosticCategory::Lifecycle, DiagnosticSeverity::Info, "Recreate");
			return Failure::Ok();
		}

		Failure ReleaseBackendTransport(IViewportTransportBackend& backend)
		{
			auto result = backend.ReleaseSurface(m_descriptor.m_viewportId, m_connectionEpoch, m_guards.GetGeneration());
			if (!result.IsOk())
			{
				m_failure = backend.GetLastFailure();
				return result;
			}

			m_failure = Failure::Ok();
			m_guards.ResetTransportReady();
			m_transportType = TransportType::Unknown;
			RecordDiagnostic(DiagnosticCategory::Lifecycle, DiagnosticSeverity::Info, "ReleaseTransport");
			return Failure::Ok();
		}

		Failure Destroy()
		{
			{
				std::lock_guard lock(m_inputMutex);
				m_inputDisposed = true;
				m_lastInput.reset();
			}
			m_transportReadyTimeout.Reset();
			m_reconnectTimeout.Reset();
			RecordDiagnostic(DiagnosticCategory::Lifecycle, DiagnosticSeverity::Info, "Destroy");
			return m_state.Destroy();
		}

		Failure TickTimeouts(uint64_t nowMs)
		{
			if (m_transportReadyTimeout.HasExpired(nowMs))
			{
				m_transportReadyTimeout.Reset();
				auto failure = Failure::FromDomain(ErrorDomain::Session, 1, "Transport ready timeout");
				m_failure = failure;
				RecordDiagnostic(DiagnosticCategory::Timeout, DiagnosticSeverity::Warning, "TransportReadyTimeout", failure);
				return m_state.TransitionTo(SessionState::Recovering);
			}
			if (m_reconnectTimeout.HasExpired(nowMs))
			{
				m_reconnectTimeout.Reset();
				auto failure = Failure::FromDomain(ErrorDomain::Connection, 1, "Reconnect timeout");
				m_failure = failure;
				RecordDiagnostic(DiagnosticCategory::Timeout, DiagnosticSeverity::Error, "ReconnectTimeout", failure);
				return m_state.TransitionTo(SessionState::Lost);
			}
			return Failure::Ok();
		}

		RetryBackoffState ScheduleReconnectAttempt()
		{
			m_reconnectBackoff = NextReconnectBackoff(m_reconnectBackoff.m_attempt);
			RecordDiagnostic(DiagnosticCategory::Backoff, DiagnosticSeverity::Info, "ReconnectBackoffScheduled");
			return m_reconnectBackoff;
		}

	private:
		Failure HandleInputLocked(const InputPacket& input)
		{
			const auto decision = m_guards.AcceptInput(input);
			if (m_inputDisposed || input.m_viewportId != m_inputViewportId || decision != GuardDecision::Accept)
			{
				return Failure::FromDomain(ErrorDomain::Session, static_cast<int32_t>(decision), "Input rejected by session guards");
			}
			m_lastInput = input;
			++m_inputCount;
			return Failure::Ok();
		}

		void RefreshDiagnostics()
		{
			m_diagnostics.m_viewportId = m_descriptor.m_viewportId;
			m_diagnostics.m_connectionEpoch = m_connectionEpoch;
			m_diagnostics.m_generation = m_guards.GetGeneration();
			m_diagnostics.m_transportType = m_transportType;
			m_diagnostics.m_state = m_state.GetState();
			m_diagnostics.m_lastGoodFrameIndex = m_lastPublishedFrameIndex;
			m_diagnostics.m_recoveryAttemptCount = m_recoveryAttemptCount;
			m_diagnostics.m_lastFailure = m_failure.IsOk() ? std::optional<Failure>{} : std::optional<Failure>{ m_failure };
		}

		void RecordDiagnostic(DiagnosticCategory category, DiagnosticSeverity severity, std::string event, const Failure& failure = Failure::Ok())
		{
			RefreshDiagnostics();
			m_diagnostics.m_lastCategory = category;
			m_diagnostics.m_lastSeverity = severity;
			m_diagnostics.m_lastEvent = std::move(event);
			if (!failure.IsOk())
			{
				m_diagnostics.m_lastFailure = failure;
			}
		}

		void ArmTransportReadyTimeout(uint64_t nowMs) { m_transportReadyTimeout.Arm(nowMs, m_timeoutPolicy.m_transportReadyTimeoutMs); }
		void ArmReconnectTimeout(uint64_t nowMs) { m_reconnectTimeout.Arm(nowMs, m_timeoutPolicy.m_reconnectTimeoutMs); }

		ViewportDescriptor m_descriptor{};
		const ViewportId m_inputViewportId;
		mutable std::mutex m_inputMutex{};
		bool m_inputDisposed = false;
		uint64_t m_inputTimestamp = 0;
		ConnectionEpoch m_connectionEpoch = 1;
		SessionStateMachine m_state{};
		SessionGuards m_guards{};
		FramePacket m_lastFrame{};
		FrameIndex m_lastPublishedFrameIndex = 0;
		bool m_visible = true;
		Failure m_failure = Failure::Ok();
		std::optional<InputPacket> m_lastInput{};
		size_t m_inputCount = 0;
		TransportType m_transportType = TransportType::Unknown;
		TimeoutPolicy m_timeoutPolicy{};
		TimeoutCheckpoint m_transportReadyTimeout{};
		TimeoutCheckpoint m_reconnectTimeout{};
		RetryBackoffState m_reconnectBackoff{};
		uint32_t m_recoveryAttemptCount = 0;
		SessionDiagnostics m_diagnostics{};
	};
}
