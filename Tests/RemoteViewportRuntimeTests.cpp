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
}

int main()
{
	const std::pair<const char*, std::function<void()>> tests[] = {
		{ "RemoteViewportSessionLifecycle", TestRemoteViewportSessionLifecycle },
		{ "RemoteViewportSessionResizeFailureAndRecreate", TestRemoteViewportSessionResizeFailureAndRecreate },
		{ "ViewportSessionManagerLifecycleAndEpochCleanup", TestViewportSessionManagerLifecycleAndEpochCleanup },
		{ "EditorBridgeServerNegotiationRoutingAndDisconnect", TestEditorBridgeServerNegotiationRoutingAndDisconnect },
		{ "EditorRenderFacadeBoundary", TestEditorRenderFacadeBoundary },
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
