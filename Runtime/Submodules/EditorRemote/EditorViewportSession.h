#pragma once

#include <functional>
#include <optional>
#include <utility>
#include <vector>

#include "RemoteViewportFoundation.h"

namespace Sailor::EditorRemote
{
	class IEditorViewportClient
	{
	public:
		virtual ~IEditorViewportClient() = default;
		virtual Failure Send(const ProtocolMessage& message) = 0;
	};

	class IEditorViewportHost
	{
	public:
		virtual ~IEditorViewportHost() = default;
		virtual Failure ImportTransport(const ViewportDescriptor& viewport, const TransportDescriptor& transport, ConnectionEpoch epoch, SurfaceGeneration generation) = 0;
		virtual Failure AcceptFrame(const FramePacket& frame) = 0;
		virtual Failure PresentLatestFrame(ViewportId viewportId) = 0;
		virtual void ResetViewport(ViewportId viewportId) = 0;
		virtual Failure GetLastFailure() const = 0;
	};

	class EditorViewportSession
	{
	public:
		EditorViewportSession(ViewportDescriptor descriptor, IEditorViewportClient& client, IEditorViewportHost& host, ConnectionEpoch epoch = 1) :
			m_desiredViewport(std::move(descriptor)),
			m_client(client),
			m_host(host),
			m_connectionEpoch(epoch),
			m_guards(epoch, 1)
		{
		}

		const ViewportDescriptor& GetDesiredViewport() const { return m_desiredViewport; }
		ConnectionEpoch GetConnectionEpoch() const { return m_connectionEpoch; }
		SurfaceGeneration GetGeneration() const { return m_guards.GetGeneration(); }
		SessionState GetState() const { return m_state.GetState(); }
		bool IsVisible() const { return m_visible; }
		bool IsFocused() const { return m_focused; }
		bool IsReady() const { return m_guards.IsTransportReady(); }
		FrameIndex GetLastPresentedFrameIndex() const { return m_lastPresentedFrameIndex; }
		size_t GetAcceptedFrameCount() const { return m_acceptedFrameCount; }
		size_t GetRejectedFrameCount(GuardDecision reason) const { return m_rejectedFrameCounts[static_cast<size_t>(reason)]; }
		const std::optional<FramePacket>& GetLastFrame() const { return m_lastFrame; }
		const std::optional<InputPacket>& GetLastForwardedInput() const { return m_lastForwardedInput; }
		const SessionDiagnostics& GetDiagnostics() const { return m_diagnostics; }

		Failure Create()
		{
			RefreshDiagnostics();
			auto validation = m_desiredViewport.Validate();
			if (!validation.IsOk())
			{
				return validation;
			}

			if (m_state.GetState() == SessionState::Created || m_state.GetState() == SessionState::Lost || m_state.GetState() == SessionState::Recovering)
			{
				auto transition = m_state.TransitionTo(SessionState::Negotiating);
				if (!transition.IsOk())
				{
					return transition;
				}
			}

			m_hasIssuedCreate = true;
			ArmTransportReadyTimeout(0);
			RecordDiagnostic(DiagnosticCategory::Lifecycle, DiagnosticSeverity::Info, "CreateRequested");
			return SendViewportCommand(CommandType::CreateViewport, m_desiredViewport);
		}

		Failure Resize(uint32_t width, uint32_t height)
		{
			m_desiredViewport.m_width = width;
			m_desiredViewport.m_height = height;
			auto validation = m_desiredViewport.Validate();
			if (!validation.IsOk())
			{
				return validation;
			}

			m_guards.AdvanceGeneration();
			m_lastPresentedFrameIndex = 0;
			m_lastFrame.reset();
			++m_diagnostics.m_resizeCount;
			ArmTransportReadyTimeout(0);
			RecordDiagnostic(DiagnosticCategory::Lifecycle, DiagnosticSeverity::Info, "ResizeRequested");

			auto transition = m_state.TransitionTo(SessionState::Resizing);
			if (!transition.IsOk())
			{
				return transition;
			}

			if (!m_hasIssuedCreate)
			{
				return Failure::Ok();
			}

			return SendViewportCommand(CommandType::ResizeViewport, m_desiredViewport);
		}

		Failure Destroy()
		{
			m_guards.ResetTransportReady();
			m_transportType = TransportType::Unknown;
			m_transportReadyTimeout.Reset();
			m_reconnectTimeout.Reset();
			m_host.ResetViewport(m_desiredViewport.m_viewportId);
			RecordDiagnostic(DiagnosticCategory::Failure, DiagnosticSeverity::Warning, "Disconnected");
			RecordDiagnostic(DiagnosticCategory::Lifecycle, DiagnosticSeverity::Info, "DestroyRequested");
			auto result = m_state.Destroy();
			if (!m_hasIssuedCreate)
			{
				return result;
			}

			auto sendResult = SendCommand(CommandType::DestroyViewport);
			return sendResult.IsOk() ? result : sendResult;
		}

		Failure SetVisible(bool visible)
		{
			m_visible = visible;
			if (m_state.GetState() == SessionState::Disposed)
			{
				return Failure::Ok();
			}

			if (m_guards.IsTransportReady())
			{
				auto transition = m_state.TransitionTo(visible ? SessionState::Active : SessionState::Paused);
				if (!transition.IsOk())
				{
					return transition;
				}
			}

			if (!m_hasIssuedCreate)
			{
				return Failure::Ok();
			}

			return SendCommand(visible ? CommandType::ResumeViewport : CommandType::SuspendViewport);
		}

		Failure SetFocused(bool focused)
		{
			m_focused = focused;
			InputPacket input{};
			input.m_kind = InputKind::Focus;
			input.m_focused = focused;
			return ForwardInput(input);
		}

		Failure ForwardInput(InputPacket input)
		{
			if (m_state.GetState() == SessionState::Disposed)
			{
				return Failure::Ok();
			}

			input.m_viewportId = m_desiredViewport.m_viewportId;
			input.m_connectionEpoch = m_connectionEpoch;
			input.m_generation = m_guards.GetGeneration();
			auto validation = input.Validate();
			if (!validation.IsOk())
			{
				return validation;
			}

			m_lastForwardedInput = input;
			ProtocolMessage message{};
			message.m_envelope.m_category = MessageCategory::Command;
			message.m_envelope.m_commandType = CommandType::Input;
			message.m_envelope.m_payload = input;
			return m_client.Send(message);
		}

		Failure HandleMessage(const ProtocolMessage& message)
		{
			switch (message.m_envelope.m_eventType)
			{
			case EventType::TransportReady:
				return HandleTransportReady(std::get<TransportDescriptor>(message.m_envelope.m_payload));
			case EventType::FrameReady:
				return HandleFrameReady(std::get<FramePacket>(message.m_envelope.m_payload));
			case EventType::SessionFailed:
				return HandleFailure(std::get<Failure>(message.m_envelope.m_payload));
			case EventType::Destroyed:
				m_host.ResetViewport(m_desiredViewport.m_viewportId);
				return m_state.Destroy();
			default:
				return Failure::Ok();
			}
		}

		Failure HandleDisconnect(ConnectionEpoch nextEpoch)
		{
			++m_recoveryAttemptCount;
			m_connectionEpoch = nextEpoch;
			m_guards.BeginNewConnectionEpoch(nextEpoch);
			m_transportType = TransportType::Unknown;
			m_transportReadyTimeout.Reset();
			ArmReconnectTimeout(0);
			m_lastPresentedFrameIndex = 0;
			m_lastFrame.reset();
			m_host.ResetViewport(m_desiredViewport.m_viewportId);

			auto state = m_state.GetState();
			if (state == SessionState::Disposed)
			{
				return Failure::Ok();
			}
			if (state == SessionState::Created)
			{
				return m_state.TransitionTo(SessionState::Lost);
			}
			if (state != SessionState::Recovering)
			{
				return m_state.TransitionTo(SessionState::Recovering);
			}
			return Failure::Ok();
		}

		Failure ReplayDesiredState()
		{
			if (m_state.GetState() == SessionState::Disposed)
			{
				return Failure::Ok();
			}

			auto createResult = Create();
			RecordDiagnostic(DiagnosticCategory::Lifecycle, DiagnosticSeverity::Info, "ReplayDesiredState");
			if (!createResult.IsOk())
			{
				return createResult;
			}

			if (!m_visible)
			{
				auto visibleResult = SendCommand(CommandType::SuspendViewport);
				if (!visibleResult.IsOk())
				{
					return visibleResult;
				}
			}

			if (m_focused)
			{
				InputPacket input{};
				input.m_kind = InputKind::Focus;
				input.m_focused = true;
				return ForwardInput(input);
			}

			return Failure::Ok();
		}

		Failure TickTimeouts(uint64_t nowMs)
		{
			if (m_transportReadyTimeout.HasExpired(nowMs))
			{
				m_transportReadyTimeout.Reset();
				auto failure = Failure::FromDomain(ErrorDomain::Session, 1, "Transport ready timeout");
				m_lastFailure = failure;
				RecordDiagnostic(DiagnosticCategory::Timeout, DiagnosticSeverity::Warning, "TransportReadyTimeout", failure);
				return m_state.TransitionTo(SessionState::Recovering);
			}

			if (m_reconnectTimeout.HasExpired(nowMs))
			{
				m_reconnectTimeout.Reset();
				auto failure = Failure::FromDomain(ErrorDomain::Connection, 1, "Reconnect timeout");
				m_lastFailure = failure;
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
		Failure SendCommand(CommandType command)
		{
			ProtocolMessage message{};
			message.m_envelope.m_category = MessageCategory::Command;
			message.m_envelope.m_commandType = command;
			return m_client.Send(message);
		}

		Failure SendViewportCommand(CommandType command, const ViewportDescriptor& viewport)
		{
			ProtocolMessage message{};
			message.m_envelope.m_category = MessageCategory::Command;
			message.m_envelope.m_commandType = command;
			message.m_envelope.m_payload = viewport;
			return m_client.Send(message);
		}

		Failure HandleTransportReady(const TransportDescriptor& transport)
		{
			auto validation = transport.Validate();
			if (!validation.IsOk())
			{
				return validation;
			}
			if (transport.m_width != m_desiredViewport.m_width || transport.m_height != m_desiredViewport.m_height)
			{
				return Failure::FromDomain(ErrorDomain::Protocol, 1, "Transport extents do not match desired viewport");
			}

			auto decision = m_guards.AcknowledgeTransportReady(m_connectionEpoch, m_guards.GetGeneration());
			if (decision != GuardDecision::Accept)
			{
				return Failure::FromDomain(ErrorDomain::Session, static_cast<int32_t>(decision), "Transport ready rejected by session guards");
			}

			auto hostResult = m_host.ImportTransport(m_desiredViewport, transport, m_connectionEpoch, m_guards.GetGeneration());
			if (!hostResult.IsOk())
			{
				m_lastFailure = m_host.GetLastFailure();
				RecordDiagnostic(DiagnosticCategory::Failure, DiagnosticSeverity::Error, "HostImportFailed", m_lastFailure);
				return m_lastFailure;
			}

			m_transportType = transport.m_transportType;
			m_transportReadyTimeout.Reset();
			m_reconnectTimeout.Reset();
			RefreshDiagnostics();
			RecordDiagnostic(DiagnosticCategory::Lifecycle, DiagnosticSeverity::Info, "TransportReady");

			auto readyResult = m_state.TransitionTo(SessionState::Ready);
			if (!readyResult.IsOk())
			{
				return readyResult;
			}
			return m_state.TransitionTo(m_visible ? SessionState::Active : SessionState::Paused);
		}

		Failure HandleFrameReady(const FramePacket& frame)
		{
			const auto decision = m_state.GetState() == SessionState::Disposed ? GuardDecision::RejectNotReady : m_guards.AcceptFrame(frame);
			if (decision != GuardDecision::Accept)
			{
				++m_rejectedFrameCounts[static_cast<size_t>(decision)];
				auto failure = Failure::FromDomain(ErrorDomain::Session, static_cast<int32_t>(decision), "Frame rejected by editor session guards");
				m_lastFailure = failure;
				RecordDiagnostic(DiagnosticCategory::Failure, DiagnosticSeverity::Warning, "FrameRejected", failure);
				return failure;
			}

			auto acceptResult = m_host.AcceptFrame(frame);
			if (!acceptResult.IsOk())
			{
				m_lastFailure = m_host.GetLastFailure();
				RecordDiagnostic(DiagnosticCategory::Failure, DiagnosticSeverity::Error, "AcceptFrameFailed", m_lastFailure);
				return m_lastFailure;
			}
			auto presentResult = m_host.PresentLatestFrame(m_desiredViewport.m_viewportId);
			if (!presentResult.IsOk())
			{
				m_lastFailure = m_host.GetLastFailure();
				RecordDiagnostic(DiagnosticCategory::Failure, DiagnosticSeverity::Error, "PresentFailed", m_lastFailure);
				return m_lastFailure;
			}

			m_lastFrame = frame;
			m_lastPresentedFrameIndex = frame.m_frameIndex;
			++m_acceptedFrameCount;
			RefreshDiagnostics();
			RecordDiagnostic(DiagnosticCategory::Lifecycle, DiagnosticSeverity::Info, "FramePresented");
			return Failure::Ok();
		}

		Failure HandleFailure(const Failure& failure)
		{
			m_lastFailure = failure;
			RecordDiagnostic(DiagnosticCategory::Failure, failure.m_scope == FailureScope::Session ? DiagnosticSeverity::Warning : DiagnosticSeverity::Error, "SessionFailed", failure);
			return m_state.TransitionTo(failure.m_scope == FailureScope::Session ? SessionState::Recovering : SessionState::Lost);
		}

		void RefreshDiagnostics()
		{
			m_diagnostics.m_viewportId = m_desiredViewport.m_viewportId;
			m_diagnostics.m_connectionEpoch = m_connectionEpoch;
			m_diagnostics.m_generation = m_guards.GetGeneration();
			m_diagnostics.m_transportType = m_transportType;
			m_diagnostics.m_state = m_state.GetState();
			m_diagnostics.m_lastGoodFrameIndex = m_lastPresentedFrameIndex;
			m_diagnostics.m_recoveryAttemptCount = m_recoveryAttemptCount;
			m_diagnostics.m_lastFailure = m_lastFailure.IsOk() ? std::optional<Failure>{} : std::optional<Failure>{ m_lastFailure };
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

		ViewportDescriptor m_desiredViewport{};
		IEditorViewportClient& m_client;
		IEditorViewportHost& m_host;
		SessionStateMachine m_state{};
		SessionGuards m_guards{};
		ConnectionEpoch m_connectionEpoch = 1;
		std::optional<FramePacket> m_lastFrame{};
		std::optional<InputPacket> m_lastForwardedInput{};
		FrameIndex m_lastPresentedFrameIndex = 0;
		std::vector<size_t> m_rejectedFrameCounts = std::vector<size_t>(5, 0);
		size_t m_acceptedFrameCount = 0;
		bool m_visible = true;
		bool m_focused = false;
		bool m_hasIssuedCreate = false;
		TransportType m_transportType = TransportType::Unknown;
		TimeoutPolicy m_timeoutPolicy{};
		TimeoutCheckpoint m_transportReadyTimeout{};
		TimeoutCheckpoint m_reconnectTimeout{};
		RetryBackoffState m_reconnectBackoff{};
		uint32_t m_recoveryAttemptCount = 0;
		Failure m_lastFailure = Failure::Ok();
		SessionDiagnostics m_diagnostics{};
	};
}
