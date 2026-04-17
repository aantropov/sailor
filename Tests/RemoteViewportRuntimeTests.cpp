#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "Submodules/EditorRemote/RemoteViewportRuntime.h"

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

	ViewportDescriptor MakeViewport(ViewportId viewportId = 1, uint32_t width = 1280, uint32_t height = 720)
	{
		ViewportDescriptor viewport{};
		viewport.m_viewportId = viewportId;
		viewport.m_width = width;
		viewport.m_height = height;
		viewport.m_pixelFormat = PixelFormat::R8G8B8A8_UNorm;
		viewport.m_colorSpace = ColorSpace::Srgb;
		viewport.m_presentMode = PresentMode::Mailbox;
		viewport.m_debugName = "RuntimeViewport";
		return viewport;
	}

	TransportDescriptor MakeTransport(const ViewportDescriptor& viewport)
	{
		TransportDescriptor transport{};
		transport.m_transportType = TransportType::MailboxCpuCopy;
		transport.m_syncMode = SyncMode::Implicit;
		transport.m_protocolVersion = 1;
		transport.m_width = viewport.m_width;
		transport.m_height = viewport.m_height;
		transport.m_pixelFormat = viewport.m_pixelFormat;
		transport.m_usageFlags = viewport.m_usageFlags;
		transport.m_ready = true;
		return transport;
	}

	InputPacket MakeInput(ViewportId viewportId, ConnectionEpoch epoch, SurfaceGeneration generation)
	{
		InputPacket input{};
		input.m_viewportId = viewportId;
		input.m_connectionEpoch = epoch;
		input.m_generation = generation;
		input.m_kind = InputKind::PointerMove;
		input.m_pointerX = 32.0f;
		input.m_pointerY = 48.0f;
		input.m_timestampNs = 5;
		return input;
	}

	class FakeViewportTransportBackend : public IViewportTransportBackend
	{
	public:
		Failure EnsureSurface(const ViewportDescriptor& viewport, ConnectionEpoch epoch, SurfaceGeneration generation, TransportDescriptor& outTransport) override
		{
			m_ensureCalls.push_back({ viewport.m_viewportId, epoch, generation });
			if (!m_nextEnsureFailure.IsOk())
			{
				m_lastFailure = m_nextEnsureFailure;
				auto failure = m_nextEnsureFailure;
				m_nextEnsureFailure = Failure::Ok();
				return failure;
			}

			outTransport = MakeTransport(viewport);
			m_lastFailure = Failure::Ok();
			return Failure::Ok();
		}

		Failure BeginFrame(const ViewportDescriptor& viewport, ConnectionEpoch epoch, SurfaceGeneration generation) override
		{
			m_beginCalls.push_back({ viewport.m_viewportId, epoch, generation });
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

		Failure ExportFrame(const ViewportDescriptor& viewport, ConnectionEpoch epoch, SurfaceGeneration generation, FramePacket& outFrame) override
		{
			m_exportCalls.push_back({ viewport.m_viewportId, epoch, generation });
			if (!m_nextExportFailure.IsOk())
			{
				m_lastFailure = m_nextExportFailure;
				auto failure = m_nextExportFailure;
				m_nextExportFailure = Failure::Ok();
				return failure;
			}

			outFrame.m_viewportId = viewport.m_viewportId;
			outFrame.m_connectionEpoch = epoch;
			outFrame.m_generation = generation;
			outFrame.m_width = viewport.m_width;
			outFrame.m_height = viewport.m_height;
			outFrame.m_timestampNs = ++m_exportedFrameCounter;
			m_lastFailure = Failure::Ok();
			return Failure::Ok();
		}

		Failure ReleaseSurface(ViewportId viewportId, ConnectionEpoch epoch, SurfaceGeneration generation) override
		{
			m_releaseCalls.push_back({ viewportId, epoch, generation });
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

		struct Call
		{
			ViewportId m_viewportId = 0;
			ConnectionEpoch m_epoch = 0;
			SurfaceGeneration m_generation = 0;
		};

		std::vector<Call> m_ensureCalls{};
		std::vector<Call> m_beginCalls{};
		std::vector<Call> m_exportCalls{};
		std::vector<Call> m_releaseCalls{};
		Failure m_nextEnsureFailure = Failure::Ok();
		Failure m_nextBeginFailure = Failure::Ok();
		Failure m_nextExportFailure = Failure::Ok();
		Failure m_nextReleaseFailure = Failure::Ok();
		Failure m_lastFailure = Failure::Ok();
		uint64_t m_exportedFrameCounter = 0;
	};

	void TestRemoteViewportSessionLifecycle()
	{
		auto viewport = MakeViewport();
		RemoteViewportSession session{ viewport, 7 };
		Require(session.GetState() == SessionState::Created, "session should start created");
		Require(session.BeginNegotiation().IsOk(), "session should enter negotiation");
		Require(session.MarkTransportReady(MakeTransport(viewport)).IsOk(), "matching transport should make session ready/active");
		Require(session.GetState() == SessionState::Active, "visible ready session should become active");
		Require(session.IsReady(), "session should report ready after transport ack");

		FramePacket frame{};
		Require(session.PublishFrame(frame).IsOk(), "ready session should publish frames");
		Require(session.GetLastPublishedFrameIndex() == 1, "frame index should increment on publish");

		auto input = MakeInput(viewport.m_viewportId, 7, session.GetGeneration());
		Require(session.HandleInput(input).IsOk(), "matching input should be accepted");
		Require(session.GetInputCount() == 1, "input count should increment");
		Require(session.GetLastInput().has_value(), "session should retain last input");

		Require(session.SetVisible(false).IsOk(), "session should pause when hidden");
		Require(session.GetState() == SessionState::Paused, "hidden session should be paused");
		Require(!session.IsVisible(), "visibility flag should update");
		Require(session.SetVisible(true).IsOk(), "session should resume when visible");
		Require(session.GetState() == SessionState::Active, "visible session should return active");
	}

	void TestRemoteViewportSessionBackendContract()
	{
		auto viewport = MakeViewport(5, 1600, 900);
		RemoteViewportSession session{ viewport, 9 };
		FakeViewportTransportBackend backend{};

		Require(session.BeginNegotiation().IsOk(), "backend-driven session should start negotiation");
		Require(session.EnsureBackendTransport(backend).IsOk(), "backend should provide transport descriptor");
		Require(session.IsReady(), "backend transport should mark session ready");
		Require(backend.m_ensureCalls.size() == 1, "ensure should be invoked exactly once");
		Require(backend.m_ensureCalls.front().m_generation == 1, "initial ensure should use generation one");

		Require(session.PublishFrameFromBackend(backend).IsOk(), "backend should drive frame export");
		Require(session.GetLastPublishedFrameIndex() == 1, "exported frame should publish through session");
		Require(backend.m_beginCalls.size() == 1 && backend.m_exportCalls.size() == 1, "frame publish should invoke begin and export once");

		Require(session.ReleaseBackendTransport(backend).IsOk(), "backend release should succeed");
		Require(!session.IsReady(), "release should invalidate ready state");
		Require(backend.m_releaseCalls.size() == 1, "release should hit backend exactly once");
	}

	void TestRemoteViewportSessionBackendFailurePropagation()
	{
		auto viewport = MakeViewport(6);
		RemoteViewportSession session{ viewport, 4 };
		FakeViewportTransportBackend backend{};
		Require(session.BeginNegotiation().IsOk(), "negotiation should start before backend failure tests");

		backend.m_nextEnsureFailure = Failure::FromDomain(ErrorDomain::Transport, 41, "ensure failed");
		Require(!session.EnsureBackendTransport(backend).IsOk(), "backend ensure failure should surface");
		Require(session.GetFailure().m_nativeCode == 41, "session should retain backend ensure failure");

		Require(session.EnsureBackendTransport(backend).IsOk(), "backend ensure should recover after injected failure");
		backend.m_nextExportFailure = Failure::FromDomain(ErrorDomain::Transport, 42, "export failed");
		Require(!session.PublishFrameFromBackend(backend).IsOk(), "backend export failure should surface");
		Require(session.GetFailure().m_nativeCode == 42, "session should retain backend export failure");

		backend.m_nextReleaseFailure = Failure::FromDomain(ErrorDomain::Transport, 43, "release failed");
		Require(!session.ReleaseBackendTransport(backend).IsOk(), "backend release failure should surface");
		Require(session.GetFailure().m_nativeCode == 43, "session should retain backend release failure");
	}

	void TestRemoteViewportSessionResizeFailureAndRecreate()
	{
		auto viewport = MakeViewport();
		RemoteViewportSession session{ viewport, 3 };
		Require(session.BeginNegotiation().IsOk(), "negotiation should start");
		Require(session.MarkTransportReady(MakeTransport(viewport)).IsOk(), "initial transport should succeed");

		auto resizedViewport = MakeViewport(viewport.m_viewportId, 1600, 900);
		Require(session.HandleResize(resizedViewport).IsOk(), "resize should be accepted");
		Require(session.GetState() == SessionState::Resizing, "resize should transition to resizing");
		Require(session.GetGeneration() == 2, "resize should advance generation");
		Require(!session.IsReady(), "resize should invalidate ready flag until renegotiated");

		Failure recreateRequired = Failure::FromDomain(ErrorDomain::Session, 2, "recreate");
		Require(session.MarkFailure(recreateRequired).IsOk(), "session-scoped failure should enter recovering");
		Require(session.GetState() == SessionState::Recovering, "session failure should transition to recovering");
		Require(session.HasFailure(), "failure should be retained");

		Require(session.Recreate(4).IsOk(), "recreate should begin a fresh epoch");
		Require(session.GetConnectionEpoch() == 4, "recreate should replace connection epoch");
		Require(session.GetGeneration() == 1, "new epoch should reset generation");
		Require(!session.HasFailure(), "recreate should clear retained failure");
		Require(session.MarkTransportReady(MakeTransport(resizedViewport)).IsOk(), "recreated session should negotiate new transport");
		Require(session.GetState() == SessionState::Active, "recreated session should return active");
	}

	void TestViewportSessionManagerLifecycleAndEpochCleanup()
	{
		ViewportSessionManager manager{};
		std::vector<std::pair<ViewportId, ConnectionEpoch>> cleanedUp{};
		manager.SetCleanupHook([&cleanedUp](ViewportId viewportId, ConnectionEpoch epoch)
		{
			cleanedUp.emplace_back(viewportId, epoch);
		});

		auto& a = manager.CreateOrReplaceSession(MakeViewport(1), 11);
		auto& b = manager.CreateOrReplaceSession(MakeViewport(2), 11);
		auto& c = manager.CreateOrReplaceSession(MakeViewport(3), 12);
		(void)a;
		(void)b;
		(void)c;

		Require(manager.GetSessionCount() == 3, "manager should own created sessions");
		Require(manager.GetViewportCountForEpoch(11) == 2, "epoch bookkeeping should group sessions");
		Require(manager.HasViewport(2), "manager should find viewport by id");

		Require(manager.DestroySession(2), "manager should destroy a specific session");
		Require(!manager.HasViewport(2), "destroyed viewport should be removed");
		Require(manager.GetSessionCount() == 2, "destroy should shrink session map");
		Require(manager.GetViewportCountForEpoch(11) == 1, "epoch bookkeeping should prune removed viewport");

		Require(manager.DestroySessionsForEpoch(11) == 1, "epoch cleanup should destroy remaining matching session");
		Require(manager.GetSessionCount() == 1, "epoch cleanup should preserve other epochs");
		Require(manager.HasViewport(3), "non-matching epoch session should remain");
		Require(cleanedUp.size() == 2, "cleanup hook should run for both destroyed sessions");
	}

	void TestViewportSessionManagerReplacementStormPrunesEpochBookkeeping()
	{
		ViewportSessionManager manager{};

		for (ConnectionEpoch epoch = 21; epoch < 53; ++epoch)
		{
			auto& session = manager.CreateOrReplaceSession(MakeViewport(91, 1280 + static_cast<uint32_t>(epoch), 720), epoch);
			Require(session.GetConnectionEpoch() == epoch, "replacement storm should keep latest epoch on session");
			Require(manager.GetSessionCount() == 1, "replacement storm should not multiply live sessions");
		}

		for (ConnectionEpoch epoch = 21; epoch < 52; ++epoch)
		{
			Require(manager.GetViewportCountForEpoch(epoch) == 0, "replacement storm should prune superseded epoch bookkeeping");
		}

		Require(manager.GetViewportCountForEpoch(52) == 1, "latest replacement should retain exactly one epoch binding");
		Require(manager.DestroySessionsForEpoch(52) == 1, "latest epoch cleanup should destroy replaced viewport exactly once");
		Require(manager.GetSessionCount() == 0, "replacement storm cleanup should leave manager empty");
	}

	void TestEditorBridgeServerNegotiationRoutingAndDisconnect()
	{
		EditorBridgeServer server{};
		BridgeConnectionInfo disconnected{};
		ProtocolMessage routedMessage{};
		BridgeConnectionInfo routedConnection{};

		server.SetNegotiationHandler([](const BridgeConnectionRequest& request, BridgeConnectionInfo& negotiated)
		{
			if (request.m_protocolVersion != 1)
			{
				return Failure::FromDomain(ErrorDomain::Capability, 1, "unsupported protocol");
			}
			negotiated.m_negotiatedCapabilityMask = request.m_capabilityMask & 0x3ull;
			return Failure::Ok();
		});
		server.SetCommandHandler([&routedMessage, &routedConnection](const BridgeConnectionInfo& connection, const ProtocolMessage& message)
		{
			routedConnection = connection;
			routedMessage = message;
			return Failure::Ok();
		});
		server.SetDisconnectHandler([&disconnected](const BridgeConnectionInfo& connection, const Failure&)
		{
			disconnected = connection;
		});

		BridgeConnectionRequest request{};
		request.m_connectionId = 99;
		request.m_epoch = 5;
		request.m_protocolVersion = 1;
		request.m_capabilityMask = 0xfull;
		Require(server.AcceptConnection(request).IsOk(), "supported connection should be accepted");
		Require(server.GetConnectionCount() == 1, "accepted connection should be stored");

		ProtocolMessage create{};
		create.m_envelope.m_category = MessageCategory::Command;
		create.m_envelope.m_commandType = CommandType::CreateViewport;
		create.m_envelope.m_payload = MakeViewport(17);
		Require(server.RouteCommand(99, create).IsOk(), "command should route through accepted connection");
		Require(routedConnection.m_connectionId == 99, "command handler should receive negotiated connection info");
		Require(routedConnection.m_negotiatedCapabilityMask == 0x3ull, "negotiation handler should be able to clamp capability mask");
		Require(routedMessage.m_envelope.m_commandType == CommandType::CreateViewport, "routed message should preserve command type");

		BridgeConnectionRequest badRequest = request;
		badRequest.m_connectionId = 100;
		badRequest.m_protocolVersion = 2;
		Require(!server.AcceptConnection(badRequest).IsOk(), "unsupported protocol should be rejected");
		Require(server.Disconnect(99), "disconnect should drop stored connection");
		Require(disconnected.m_connectionId == 99, "disconnect handler should receive dropped connection info");
		Require(server.GetConnectionCount() == 0, "disconnect should remove connection from server");
	}

	class FakeRenderBridge : public IEditorRenderBridge
	{
	public:
		void ApplyBinding(const RenderBindingRequest& request) override
		{
			m_applied.push_back(request);
		}

		void ReleaseBinding(ViewportId viewportId) override
		{
			m_released.push_back(viewportId);
		}

		std::vector<RenderBindingRequest> m_applied{};
		std::vector<ViewportId> m_released{};
	};

	void TestEditorRenderFacadeBoundary()
	{
		auto viewport = MakeViewport(44, 1920, 1080);
		RemoteViewportSession session{ viewport, 8 };
		Require(session.BeginNegotiation().IsOk(), "session negotiation should start");
		Require(session.MarkTransportReady(MakeTransport(viewport)).IsOk(), "session transport should become ready");
		FramePacket frame{};
		Require(session.PublishFrame(frame).IsOk(), "session should publish a frame before facade sync");

		FakeRenderBridge renderBridge{};
		EditorRenderFacade facade{ renderBridge };
		facade.SyncSession(session);
		Require(renderBridge.m_applied.size() == 1, "facade should emit one binding request");
		Require(renderBridge.m_applied.front().m_viewportId == 44, "binding should use viewport id only, not editor types");
		Require(renderBridge.m_applied.front().m_ready, "binding should reflect session readiness");
		Require(renderBridge.m_applied.front().m_lastFrameIndex == 1, "binding should expose latest frame index");

		facade.ReleaseSession(44);
		Require(renderBridge.m_released.size() == 1 && renderBridge.m_released.front() == 44, "facade should release viewport binding explicitly");
	}
	void TestRemoteViewportReconnectTimeoutBackoffAndDiagnostics()
	{
		auto viewport = MakeViewport(77, 1024, 768);
		RemoteViewportSession session{ viewport, 5 };
		Require(session.BeginNegotiation().IsOk(), "negotiation should arm transport timeout");
		Require(session.TickTimeouts(1000).IsOk(), "timeout transition should be accepted");
		Require(session.GetState() == SessionState::Recovering, "transport ready timeout should enter recovering");
		Require(session.GetDiagnostics().m_lastCategory == DiagnosticCategory::Timeout, "diagnostics should classify transport timeout");
		Require(session.GetDiagnostics().m_lastFailure.has_value() && session.GetDiagnostics().m_lastFailure->m_scope == FailureScope::Session, "transport timeout should be session-scoped");

		auto backoff1 = session.ScheduleReconnectAttempt();
		auto backoff2 = session.ScheduleReconnectAttempt();
		Require(backoff1.m_delayMs == 100 && backoff2.m_delayMs == 200, "reconnect backoff should be bounded exponential");

		Failure disconnect = Failure::FromDomain(ErrorDomain::Connection, 9, "connection dropped");
		Require(session.MarkFailure(disconnect).IsOk(), "connection failure should be accepted");
		Require(session.TickTimeouts(5000).IsOk(), "reconnect timeout transition should be accepted");
		Require(session.GetState() == SessionState::Lost, "reconnect timeout should enter lost");
		Require(session.GetDiagnostics().m_lastFailure.has_value() && session.GetDiagnostics().m_lastFailure->m_scope == FailureScope::Connection, "diagnostics should retain connection-scoped timeout failure");
	}

	void TestRemoteViewportFrameFloodKeepsLatestFrameAndStableCounters()
	{
		auto viewport = MakeViewport(88, 1920, 1080);
		RemoteViewportSession session{ viewport, 13 };
		Require(session.BeginNegotiation().IsOk(), "flood test negotiation should start");
		Require(session.MarkTransportReady(MakeTransport(viewport)).IsOk(), "flood test transport should become ready");

		constexpr size_t kFrameFloodCount = 256;
		for (size_t i = 0; i < kFrameFloodCount; ++i)
		{
			FramePacket frame{};
			Require(session.PublishFrame(frame).IsOk(), "frame flood should keep accepting current-generation frames");
		}

		Require(session.GetLastPublishedFrameIndex() == kFrameFloodCount, "frame flood should retain latest published frame index only");
		Require(session.GetDiagnostics().m_lastGoodFrameIndex == kFrameFloodCount, "diagnostics should track latest frame without drift");
		Require(session.GetState() == SessionState::Active, "frame flood should keep ready session active");
	}

	void TestRemoteViewportDisconnectRecreateLoopResetsEpochGenerationAndState()
	{
		auto viewport = MakeViewport(89, 1280, 720);
		RemoteViewportSession session{ viewport, 30 };
		Require(session.BeginNegotiation().IsOk(), "loop test should start negotiation");
		Require(session.MarkTransportReady(MakeTransport(viewport)).IsOk(), "loop test should become ready");

		for (ConnectionEpoch epoch = 31; epoch < 39; ++epoch)
		{
			Failure disconnect = Failure::FromDomain(ErrorDomain::Connection, 70 + static_cast<int32_t>(epoch), "disconnect loop");
			Require(session.MarkFailure(disconnect).IsOk(), "disconnect loop should enter lost state");
			Require(session.GetState() == SessionState::Lost, "connection-scoped failure should transition to lost");
			Require(session.Recreate(epoch).IsOk(), "recreate loop should start new negotiation epoch");
			Require(session.GetConnectionEpoch() == epoch, "recreate loop should advance epoch deterministically");
			Require(session.GetGeneration() == 1, "recreate loop should reset generation to one");
			Require(!session.HasFailure(), "recreate loop should clear retained failures");
			Require(session.MarkTransportReady(MakeTransport(viewport)).IsOk(), "recreated session should renegotiate transport");
			FramePacket frame{};
			Require(session.PublishFrame(frame).IsOk(), "recreated session should publish immediately after ready");
		}

		Require(session.GetDiagnostics().m_recoveryAttemptCount == 16, "disconnect loop should count both failure and recreate recovery attempts without blowup");
		Require(session.GetLastPublishedFrameIndex() == 1, "each recreate loop should reset frame indexing for the new epoch");
	}

}

int main()
{
	const std::pair<const char*, std::function<void()>> tests[] = {
		{ "RemoteViewportSessionLifecycle", TestRemoteViewportSessionLifecycle },
		{ "RemoteViewportSessionBackendContract", TestRemoteViewportSessionBackendContract },
		{ "RemoteViewportSessionBackendFailurePropagation", TestRemoteViewportSessionBackendFailurePropagation },
		{ "RemoteViewportSessionResizeFailureAndRecreate", TestRemoteViewportSessionResizeFailureAndRecreate },
		{ "ViewportSessionManagerLifecycleAndEpochCleanup", TestViewportSessionManagerLifecycleAndEpochCleanup },
		{ "ViewportSessionManagerReplacementStormPrunesEpochBookkeeping", TestViewportSessionManagerReplacementStormPrunesEpochBookkeeping },
		{ "EditorBridgeServerNegotiationRoutingAndDisconnect", TestEditorBridgeServerNegotiationRoutingAndDisconnect },
		{ "EditorRenderFacadeBoundary", TestEditorRenderFacadeBoundary },
		{ "RemoteViewportReconnectTimeoutBackoffAndDiagnostics", TestRemoteViewportReconnectTimeoutBackoffAndDiagnostics },
		{ "RemoteViewportFrameFloodKeepsLatestFrameAndStableCounters", TestRemoteViewportFrameFloodKeepsLatestFrameAndStableCounters },
		{ "RemoteViewportDisconnectRecreateLoopResetsEpochGenerationAndState", TestRemoteViewportDisconnectRecreateLoopResetsEpochGenerationAndState },
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
