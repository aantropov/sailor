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

		Failure Create()
		{
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
			m_host.ResetViewport(m_desiredViewport.m_viewportId);
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
			m_connectionEpoch = nextEpoch;
			m_guards.BeginNewConnectionEpoch(nextEpoch);
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
				return m_host.GetLastFailure();
			}

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
				return Failure::FromDomain(ErrorDomain::Session, static_cast<int32_t>(decision), "Frame rejected by editor session guards");
			}

			auto acceptResult = m_host.AcceptFrame(frame);
			if (!acceptResult.IsOk())
			{
				return m_host.GetLastFailure();
			}
			auto presentResult = m_host.PresentLatestFrame(m_desiredViewport.m_viewportId);
			if (!presentResult.IsOk())
			{
				return m_host.GetLastFailure();
			}

			m_lastFrame = frame;
			m_lastPresentedFrameIndex = frame.m_frameIndex;
			++m_acceptedFrameCount;
			return Failure::Ok();
		}

		Failure HandleFailure(const Failure& failure)
		{
			return m_state.TransitionTo(failure.m_scope == FailureScope::Session ? SessionState::Recovering : SessionState::Lost);
		}

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
	};
}
