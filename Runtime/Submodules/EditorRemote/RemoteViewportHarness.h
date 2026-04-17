#pragma once

#include <array>
#include <deque>
#include <optional>

#include "RemoteViewportFoundation.h"

namespace Sailor::EditorRemote
{
	class RemoteViewportHarness
	{
	public:
		explicit RemoteViewportHarness(const ViewportDescriptor& viewport) :
			m_desiredViewport(viewport),
			m_liveViewport(viewport)
		{
		}

		void EditorCreate()
		{
			if (m_editorState.GetState() == SessionState::Created)
			{
				(void)m_editorState.TransitionTo(SessionState::Negotiating);
				SendToEngine(MakeViewportCommand(CommandType::CreateViewport, m_desiredViewport));
			}
		}

		void BringUpActiveSession()
		{
			EditorCreate();
			PumpEditorToEngine();
			PumpEngineToEditor();
			EngineInjectFrameReady();
			PumpEngineToEditor();
		}

		void EditorResize(uint32_t width, uint32_t height)
		{
			m_desiredViewport.m_width = width;
			m_desiredViewport.m_height = height;
			m_editorGuards.AdvanceGeneration();
			(void)m_editorState.TransitionTo(SessionState::Resizing);
			SendToEngine(MakeViewportCommand(CommandType::ResizeViewport, CurrentViewportForCurrentGeneration()));
		}

		void EditorDestroy()
		{
			m_editorGuards.ResetTransportReady();
			(void)m_editorState.Destroy();
			SendToEngine(MakeCommand(CommandType::DestroyViewport));
		}

		void EditorSetVisible(bool visible)
		{
			m_visible = visible;
			if (m_editorState.GetState() == SessionState::Disposed)
			{
				return;
			}

			if (visible)
			{
				(void)m_editorState.TransitionTo(SessionState::Active);
				SendToEngine(MakeCommand(CommandType::ResumeViewport));
			}
			else
			{
				(void)m_editorState.TransitionTo(SessionState::Paused);
				SendToEngine(MakeCommand(CommandType::SuspendViewport));
			}
		}

		void Disconnect()
		{
			m_editorGuards.BeginNewConnectionEpoch(m_editorGuards.GetConnectionEpoch() + 1);
			(void)m_editorState.TransitionTo(SessionState::Recovering);
			m_engineGuards.ResetTransportReady();
			(void)m_engineState.TransitionTo(SessionState::Lost);
		}

		void Reconnect()
		{
			const auto nextEpoch = m_editorGuards.GetConnectionEpoch();
			m_engineGuards.BeginNewConnectionEpoch(nextEpoch);
			m_liveViewport = m_desiredViewport;
			m_liveViewport.m_width = m_desiredViewport.m_width;
			m_liveViewport.m_height = m_desiredViewport.m_height;
			m_engineInFlightFrame = false;
			m_nextFrameIndex = 0;
			(void)m_editorState.TransitionTo(SessionState::Negotiating);
			(void)m_engineState.TransitionTo(SessionState::Negotiating);
			SendToEngine(MakeViewportCommand(CommandType::CreateViewport, CurrentViewportForCurrentGeneration()));
		}

		void PumpEditorToEngine()
		{
			if (m_editorToEngine.empty())
			{
				return;
			}

			auto message = m_editorToEngine.front();
			m_editorToEngine.pop_front();
			HandleEngineMessage(message);
		}

		void PumpEngineToEditor()
		{
			if (m_engineToEditor.empty())
			{
				return;
			}

			auto message = m_engineToEditor.front();
			m_engineToEditor.pop_front();
			HandleEditorMessage(message);
		}

		void EngineReceiveExternalCommand(const ProtocolMessage& message)
		{
			HandleEngineMessage(message);
		}

		std::optional<ProtocolMessage> PopEngineEventForExternalEditor()
		{
			if (m_engineToEditor.empty())
			{
				return std::nullopt;
			}

			auto message = m_engineToEditor.front();
			m_engineToEditor.pop_front();
			return message;
		}

		void EngineBeginInFlightFrame()
		{
			m_engineInFlightFrame = true;
			m_inFlightFrame = MakeFramePacket(m_liveViewport, m_engineGuards.GetConnectionEpoch(), m_engineGuards.GetGeneration(), ++m_nextFrameIndex);
		}

		void EngineCompleteInFlightFrame()
		{
			if (!m_engineInFlightFrame)
			{
				return;
			}

			m_engineInFlightFrame = false;
			SendToEditor(ProtocolMessage::MakeFrameReady(m_inFlightFrame), true);
		}

		void EngineInjectFrameReady(SurfaceGeneration generation = 0, ConnectionEpoch epoch = 0)
		{
			const auto actualEpoch = epoch == 0 ? m_engineGuards.GetConnectionEpoch() : epoch;
			const auto actualGeneration = generation == 0 ? m_engineGuards.GetGeneration() : generation;
			const auto frame = MakeFramePacket(m_liveViewport, actualEpoch, actualGeneration, ++m_nextFrameIndex);
			SendToEditor(ProtocolMessage::MakeFrameReady(frame), true);
		}

		SessionState EditorState() const { return m_editorState.GetState(); }
		SessionState EngineState() const { return m_engineState.GetState(); }
		SurfaceGeneration CurrentGeneration() const { return m_editorGuards.GetGeneration(); }
		ConnectionEpoch CurrentEpoch() const { return m_editorGuards.GetConnectionEpoch(); }
		bool EngineHasInFlightFrame() const { return m_engineInFlightFrame; }
		bool EngineCanScheduleFrame() const { return m_engineCanScheduleFrame; }
		size_t EditorAcceptedFrameCount() const { return m_editorAcceptedFrameCount; }
		size_t EditorReadyAckCount() const { return m_editorReadyAckCount; }

		size_t EditorRejectedFrameCount(GuardDecision reason) const
		{
			return m_editorRejectedFrameCounts[static_cast<size_t>(reason)];
		}

	private:
		static constexpr size_t kGuardDecisionCount = 5;

		static ProtocolMessage MakeCommand(CommandType command)
		{
			ProtocolMessage result{};
			result.m_envelope.m_category = MessageCategory::Command;
			result.m_envelope.m_commandType = command;
			return result;
		}

		static ProtocolMessage MakeViewportCommand(CommandType command, const ViewportDescriptor& viewport)
		{
			auto result = MakeCommand(command);
			result.m_envelope.m_payload = viewport;
			return result;
		}

		static ProtocolMessage MakeTransportReadyEvent(const TransportDescriptor& transport)
		{
			ProtocolMessage result{};
			result.m_envelope.m_category = MessageCategory::Event;
			result.m_envelope.m_eventType = EventType::TransportReady;
			result.m_envelope.m_payload = transport;
			return result;
		}

		static FramePacket MakeFramePacket(const ViewportDescriptor& viewport, ConnectionEpoch epoch, SurfaceGeneration generation, FrameIndex frameIndex)
		{
			FramePacket frame{};
			frame.m_viewportId = viewport.m_viewportId;
			frame.m_connectionEpoch = epoch;
			frame.m_generation = generation;
			frame.m_frameIndex = frameIndex;
			frame.m_width = viewport.m_width;
			frame.m_height = viewport.m_height;
			frame.m_timestampNs = frameIndex;
			return frame;
		}

		ViewportDescriptor CurrentViewportForCurrentGeneration() const
		{
			auto viewport = m_desiredViewport;
			viewport.m_width = m_desiredViewport.m_width;
			viewport.m_height = m_desiredViewport.m_height;
			return viewport;
		}

		TransportDescriptor MakeCurrentTransportDescriptor() const
		{
			TransportDescriptor transport{};
			transport.m_transportType = TransportType::MailboxCpuCopy;
			transport.m_syncMode = SyncMode::Implicit;
			transport.m_protocolVersion = 1;
			transport.m_width = m_liveViewport.m_width;
			transport.m_height = m_liveViewport.m_height;
			transport.m_pixelFormat = m_liveViewport.m_pixelFormat;
			transport.m_usageFlags = m_liveViewport.m_usageFlags;
			transport.m_ready = true;
			return transport;
		}

		void SendToEngine(const ProtocolMessage& message)
		{
			m_editorToEngine.push_back(message);
		}

		void SendToEditor(const ProtocolMessage& message, bool front)
		{
			if (front)
			{
				m_engineToEditor.push_front(message);
			}
			else
			{
				m_engineToEditor.push_back(message);
			}
		}

		void HandleEngineMessage(const ProtocolMessage& message)
		{
			switch (message.m_envelope.m_commandType)
			{
			case CommandType::CreateViewport:
				HandleEngineCreate(std::get<ViewportDescriptor>(message.m_envelope.m_payload));
				break;
			case CommandType::ResizeViewport:
				HandleEngineResize(std::get<ViewportDescriptor>(message.m_envelope.m_payload));
				break;
			case CommandType::DestroyViewport:
				m_engineCanScheduleFrame = false;
				m_engineGuards.ResetTransportReady();
				(void)m_engineState.Destroy();
				break;
			case CommandType::SuspendViewport:
				m_engineCanScheduleFrame = false;
				(void)m_engineState.TransitionTo(SessionState::Paused);
				break;
			case CommandType::ResumeViewport:
				m_engineCanScheduleFrame = true;
				(void)m_engineState.TransitionTo(SessionState::Active);
				break;
			default:
				break;
			}
		}

		void HandleEngineCreate(const ViewportDescriptor& viewport)
		{
			m_liveViewport = viewport;
			m_engineCanScheduleFrame = m_visible;
			if (m_engineState.GetState() == SessionState::Created || m_engineState.GetState() == SessionState::Lost)
			{
				(void)m_engineState.TransitionTo(SessionState::Negotiating);
			}
			(void)m_engineState.TransitionTo(SessionState::Ready);
			m_engineGuards.ResetTransportReady();
			(void)m_engineGuards.AcknowledgeTransportReady(m_engineGuards.GetConnectionEpoch(), m_engineGuards.GetGeneration());
			SendToEditor(MakeTransportReadyEvent(MakeCurrentTransportDescriptor()), false);
		}

		void HandleEngineResize(const ViewportDescriptor& viewport)
		{
			m_liveViewport = viewport;
			m_engineCanScheduleFrame = m_visible;
			m_engineGuards.AdvanceGeneration();
			(void)m_engineState.TransitionTo(SessionState::Resizing);
			(void)m_engineState.TransitionTo(SessionState::Ready);
			(void)m_engineGuards.AcknowledgeTransportReady(m_engineGuards.GetConnectionEpoch(), m_engineGuards.GetGeneration());
			SendToEditor(MakeTransportReadyEvent(MakeCurrentTransportDescriptor()), false);
		}

		void HandleEditorMessage(const ProtocolMessage& message)
		{
			switch (message.m_envelope.m_eventType)
			{
			case EventType::TransportReady:
				HandleEditorTransportReady(std::get<TransportDescriptor>(message.m_envelope.m_payload));
				break;
			case EventType::FrameReady:
				HandleEditorFrameReady(std::get<FramePacket>(message.m_envelope.m_payload));
				break;
			default:
				break;
			}
		}

		void HandleEditorTransportReady(const TransportDescriptor& transport)
		{
			if (!transport.m_ready || m_editorState.GetState() == SessionState::Disposed)
			{
				return;
			}

			const auto decision = m_editorGuards.AcknowledgeTransportReady(m_editorGuards.GetConnectionEpoch(), m_editorGuards.GetGeneration());
			if (decision != GuardDecision::Accept)
			{
				return;
			}

			++m_editorReadyAckCount;
			if (!m_visible)
			{
				(void)m_editorState.TransitionTo(SessionState::Paused);
			}
			else if (m_editorState.GetState() == SessionState::Negotiating || m_editorState.GetState() == SessionState::Ready || m_editorState.GetState() == SessionState::Resizing || m_editorState.GetState() == SessionState::Recovering)
			{
				(void)m_editorState.TransitionTo(SessionState::Ready);
				(void)m_editorState.TransitionTo(SessionState::Active);
			}
		}

		void HandleEditorFrameReady(const FramePacket& frame)
		{
			const auto decision = m_editorState.GetState() == SessionState::Disposed ? GuardDecision::RejectNotReady : m_editorGuards.AcceptFrame(frame);
			if (decision == GuardDecision::Accept)
			{
				++m_editorAcceptedFrameCount;
				return;
			}

			++m_editorRejectedFrameCounts[static_cast<size_t>(decision)];
		}

		ViewportDescriptor m_desiredViewport{};
		ViewportDescriptor m_liveViewport{};
		SessionStateMachine m_editorState{};
		SessionStateMachine m_engineState{};
		SessionGuards m_editorGuards{};
		SessionGuards m_engineGuards{};
		std::deque<ProtocolMessage> m_editorToEngine{};
		std::deque<ProtocolMessage> m_engineToEditor{};
		std::array<size_t, kGuardDecisionCount> m_editorRejectedFrameCounts{};
		size_t m_editorAcceptedFrameCount = 0;
		size_t m_editorReadyAckCount = 0;
		FramePacket m_inFlightFrame{};
		FrameIndex m_nextFrameIndex = 0;
		bool m_engineInFlightFrame = false;
		bool m_engineCanScheduleFrame = false;
		bool m_visible = true;
	};
}
