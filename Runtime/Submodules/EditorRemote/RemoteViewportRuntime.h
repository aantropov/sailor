#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "RemoteViewportFoundation.h"

namespace Sailor::EditorRemote
{
	struct BridgeConnectionRequest
	{
		uint64_t m_connectionId = 0;
		ConnectionEpoch m_epoch = 0;
		uint32_t m_protocolVersion = 0;
		uint64_t m_capabilityMask = 0;

		auto operator<=>(const BridgeConnectionRequest&) const = default;
	};

	struct BridgeConnectionInfo
	{
		uint64_t m_connectionId = 0;
		ConnectionEpoch m_epoch = 0;
		uint32_t m_negotiatedProtocolVersion = 0;
		uint64_t m_negotiatedCapabilityMask = 0;

		auto operator<=>(const BridgeConnectionInfo&) const = default;
	};

	class RemoteViewportSession
	{
	public:
		explicit RemoteViewportSession(const ViewportDescriptor& descriptor, ConnectionEpoch epoch = 1) :
			m_descriptor(descriptor),
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
		size_t GetInputCount() const { return m_inputCount; }
		const std::optional<InputPacket>& GetLastInput() const { return m_lastInput; }

		Failure BeginNegotiation()
		{
			return m_state.TransitionTo(SessionState::Negotiating);
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
				return Failure::FromDomain(ErrorDomain::Protocol, static_cast<int32_t>(readyDecision), "Transport ready acknowledgement rejected");
			}

			auto transition = m_state.TransitionTo(SessionState::Ready);
			if (!transition.IsOk())
			{
				return transition;
			}

			return m_visible ? m_state.TransitionTo(SessionState::Active) : m_state.TransitionTo(SessionState::Paused);
		}

		Failure HandleResize(const ViewportDescriptor& descriptor)
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

			auto transition = m_state.TransitionTo(SessionState::Resizing);
			if (!transition.IsOk())
			{
				return transition;
			}

			m_descriptor = descriptor;
			m_guards.AdvanceGeneration();
			m_lastPublishedFrameIndex = 0;
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
				return Failure::FromDomain(ErrorDomain::Session, static_cast<int32_t>(decision), "Frame rejected by session guards");
			}

			m_lastFrame = frame;
			if (m_state.GetState() == SessionState::Ready)
			{
				return m_state.TransitionTo(SessionState::Active);
			}
			return Failure::Ok();
		}

		Failure HandleInput(const InputPacket& input)
		{
			auto decision = m_guards.AcceptInput(input);
			if (decision != GuardDecision::Accept)
			{
				return Failure::FromDomain(ErrorDomain::Session, static_cast<int32_t>(decision), "Input rejected by session guards");
			}

			m_lastInput = input;
			++m_inputCount;
			return Failure::Ok();
		}

		Failure SetVisible(bool visible)
		{
			m_visible = visible;
			if (m_state.GetState() == SessionState::Disposed)
			{
				return Failure::Ok();
			}
			return m_state.TransitionTo(visible ? SessionState::Active : SessionState::Paused);
		}

		Failure MarkFailure(const Failure& failure)
		{
			m_failure = failure;
			auto nextState = failure.m_scope == FailureScope::Session ? SessionState::Recovering : SessionState::Lost;
			return m_state.TransitionTo(nextState);
		}

		Failure Recreate(ConnectionEpoch epoch)
		{
			m_connectionEpoch = epoch;
			m_guards.BeginNewConnectionEpoch(epoch);
			m_lastPublishedFrameIndex = 0;
			m_failure = Failure::Ok();
			return m_state.TransitionTo(SessionState::Negotiating);
		}

		Failure Destroy()
		{
			m_lastInput.reset();
			return m_state.Destroy();
		}

	private:
		ViewportDescriptor m_descriptor{};
		ConnectionEpoch m_connectionEpoch = 1;
		SessionStateMachine m_state{};
		SessionGuards m_guards{};
		FramePacket m_lastFrame{};
		FrameIndex m_lastPublishedFrameIndex = 0;
		bool m_visible = true;
		Failure m_failure = Failure::Ok();
		std::optional<InputPacket> m_lastInput{};
		size_t m_inputCount = 0;
	};

	class ViewportSessionManager
	{
	public:
		using SessionCleanupHook = std::function<void(ViewportId, ConnectionEpoch)>;

		RemoteViewportSession* FindSession(ViewportId viewportId)
		{
			auto it = m_sessions.find(viewportId);
			return it != m_sessions.end() ? it->second.get() : nullptr;
		}

		const RemoteViewportSession* FindSession(ViewportId viewportId) const
		{
			auto it = m_sessions.find(viewportId);
			return it != m_sessions.end() ? it->second.get() : nullptr;
		}

		RemoteViewportSession& CreateOrReplaceSession(const ViewportDescriptor& descriptor, ConnectionEpoch epoch)
		{
			auto session = std::make_unique<RemoteViewportSession>(descriptor, epoch);
			auto* result = session.get();
			m_sessions[descriptor.m_viewportId] = std::move(session);
			m_viewportsByEpoch[epoch].push_back(descriptor.m_viewportId);
			return *result;
		}

		bool DestroySession(ViewportId viewportId)
		{
			auto it = m_sessions.find(viewportId);
			if (it == m_sessions.end())
			{
				return false;
			}

			const auto epoch = it->second->GetConnectionEpoch();
			(void)it->second->Destroy();
			if (m_cleanupHook)
			{
				m_cleanupHook(viewportId, epoch);
			}
			m_sessions.erase(it);
			PruneEpochViewport(viewportId, epoch);
			return true;
		}

		size_t DestroySessionsForEpoch(ConnectionEpoch epoch)
		{
			auto epochIt = m_viewportsByEpoch.find(epoch);
			if (epochIt == m_viewportsByEpoch.end())
			{
				return 0;
			}

			auto viewportIds = epochIt->second;
			size_t destroyedCount = 0;
			for (auto viewportId : viewportIds)
			{
				destroyedCount += DestroySession(viewportId) ? 1u : 0u;
			}
			m_viewportsByEpoch.erase(epoch);
			return destroyedCount;
		}

		void SetCleanupHook(SessionCleanupHook hook)
		{
			m_cleanupHook = std::move(hook);
		}

		size_t GetSessionCount() const { return m_sessions.size(); }
		bool HasViewport(ViewportId viewportId) const { return m_sessions.contains(viewportId); }
		size_t GetViewportCountForEpoch(ConnectionEpoch epoch) const
		{
			auto it = m_viewportsByEpoch.find(epoch);
			return it != m_viewportsByEpoch.end() ? it->second.size() : 0;
		}

	private:
		void PruneEpochViewport(ViewportId viewportId, ConnectionEpoch epoch)
		{
			auto epochIt = m_viewportsByEpoch.find(epoch);
			if (epochIt == m_viewportsByEpoch.end())
			{
				return;
			}

			auto& viewports = epochIt->second;
			viewports.erase(std::remove(viewports.begin(), viewports.end(), viewportId), viewports.end());
			if (viewports.empty())
			{
				m_viewportsByEpoch.erase(epochIt);
			}
		}

		std::unordered_map<ViewportId, std::unique_ptr<RemoteViewportSession>> m_sessions{};
		std::unordered_map<ConnectionEpoch, std::vector<ViewportId>> m_viewportsByEpoch{};
		SessionCleanupHook m_cleanupHook{};
	};

	class EditorBridgeServer
	{
	public:
		using NegotiationHandler = std::function<Failure(const BridgeConnectionRequest&, BridgeConnectionInfo&)>;
		using CommandHandler = std::function<Failure(const BridgeConnectionInfo&, const ProtocolMessage&)>;
		using DisconnectHandler = std::function<void(const BridgeConnectionInfo&, const Failure&)>;

		Failure AcceptConnection(const BridgeConnectionRequest& request)
		{
			if (request.m_connectionId == 0 || request.m_epoch == 0 || request.m_protocolVersion == 0)
			{
				return Failure::FromDomain(ErrorDomain::Protocol, 1, "Connection request must provide id, epoch, and protocol version");
			}

			BridgeConnectionInfo negotiated{};
			negotiated.m_connectionId = request.m_connectionId;
			negotiated.m_epoch = request.m_epoch;
			negotiated.m_negotiatedProtocolVersion = request.m_protocolVersion;
			negotiated.m_negotiatedCapabilityMask = request.m_capabilityMask;

			if (m_negotiationHandler)
			{
				auto result = m_negotiationHandler(request, negotiated);
				if (!result.IsOk())
				{
					return result;
				}
			}

			m_connections[request.m_connectionId] = negotiated;
			return Failure::Ok();
		}

		Failure RouteCommand(uint64_t connectionId, const ProtocolMessage& command) const
		{
			auto it = m_connections.find(connectionId);
			if (it == m_connections.end())
			{
				return Failure::FromDomain(ErrorDomain::Connection, 1, "Unknown bridge connection");
			}
			if (command.m_envelope.m_category != MessageCategory::Command)
			{
				return Failure::FromDomain(ErrorDomain::Protocol, 1, "Bridge server only routes command messages");
			}
			if (!m_commandHandler)
			{
				return Failure::Ok();
			}
			return m_commandHandler(it->second, command);
		}

		bool Disconnect(uint64_t connectionId, const Failure& reason = Failure::FromDomain(ErrorDomain::Connection, 1, "Disconnected"))
		{
			auto it = m_connections.find(connectionId);
			if (it == m_connections.end())
			{
				return false;
			}
			if (m_disconnectHandler)
			{
				m_disconnectHandler(it->second, reason);
			}
			m_connections.erase(it);
			return true;
		}

		void SetNegotiationHandler(NegotiationHandler handler) { m_negotiationHandler = std::move(handler); }
		void SetCommandHandler(CommandHandler handler) { m_commandHandler = std::move(handler); }
		void SetDisconnectHandler(DisconnectHandler handler) { m_disconnectHandler = std::move(handler); }

		bool HasConnection(uint64_t connectionId) const { return m_connections.contains(connectionId); }
		size_t GetConnectionCount() const { return m_connections.size(); }
		const BridgeConnectionInfo* FindConnection(uint64_t connectionId) const
		{
			auto it = m_connections.find(connectionId);
			return it != m_connections.end() ? &it->second : nullptr;
		}

	private:
		std::unordered_map<uint64_t, BridgeConnectionInfo> m_connections{};
		NegotiationHandler m_negotiationHandler{};
		CommandHandler m_commandHandler{};
		DisconnectHandler m_disconnectHandler{};
	};

	struct RenderBindingRequest
	{
		ViewportId m_viewportId = 0;
		ConnectionEpoch m_connectionEpoch = 0;
		SurfaceGeneration m_generation = 0;
		uint32_t m_width = 0;
		uint32_t m_height = 0;
		bool m_visible = false;
		bool m_ready = false;
		FrameIndex m_lastFrameIndex = 0;

		auto operator<=>(const RenderBindingRequest&) const = default;
	};

	class IEditorRenderBridge
	{
	public:
		virtual ~IEditorRenderBridge() = default;
		virtual void ApplyBinding(const RenderBindingRequest& request) = 0;
		virtual void ReleaseBinding(ViewportId viewportId) = 0;
	};

	class EditorRenderFacade
	{
	public:
		explicit EditorRenderFacade(IEditorRenderBridge& renderBridge) :
			m_renderBridge(renderBridge)
		{
		}

		void SyncSession(const RemoteViewportSession& session)
		{
			const auto& descriptor = session.GetDescriptor();
			RenderBindingRequest request{};
			request.m_viewportId = descriptor.m_viewportId;
			request.m_connectionEpoch = session.GetConnectionEpoch();
			request.m_generation = session.GetGeneration();
			request.m_width = descriptor.m_width;
			request.m_height = descriptor.m_height;
			request.m_visible = session.IsVisible();
			request.m_ready = session.IsReady();
			request.m_lastFrameIndex = session.GetLastPublishedFrameIndex();
			m_renderBridge.ApplyBinding(request);
		}

		void ReleaseSession(ViewportId viewportId)
		{
			m_renderBridge.ReleaseBinding(viewportId);
		}

	private:
		IEditorRenderBridge& m_renderBridge;
	};
}
