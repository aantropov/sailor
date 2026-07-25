#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "Platform/Win32/Input.h"
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

	std::string ReadText(const std::filesystem::path& path)
	{
		std::ifstream input(path, std::ios::binary);
		Require(input.is_open(), "test source should be readable: " + path.generic_string());
		return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
	}

	size_t CountOccurrences(const std::string& text, const std::string& value)
	{
		size_t count = 0;
		size_t offset = 0;
		while ((offset = text.find(value, offset)) != std::string::npos)
		{
			++count;
			offset += value.size();
		}
		return count;
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

	void TestEditorBridgeServerConnectionAddressStability()
	{
		EditorBridgeServer server{};
		auto makeRequest = [](uint64_t connectionId, ConnectionEpoch epoch)
		{
			BridgeConnectionRequest request{};
			request.m_connectionId = connectionId;
			request.m_epoch = epoch;
			request.m_protocolVersion = 1;
			return request;
		};

		Require(server.AcceptConnection(makeRequest(1, 10)).IsOk(),
			"address-stability server should accept its first connection");
		const BridgeConnectionInfo* firstAddress = server.FindConnection(1);
		for (uint64_t connectionId = 2; connectionId <= 64; ++connectionId)
		{
			Require(server.AcceptConnection(makeRequest(connectionId, connectionId + 10)).IsOk(),
				"address-stability server should accept growth connections");
		}

		const BridgeConnectionInfo* addressAfterGrowth = server.FindConnection(1);
		Require(firstAddress != nullptr && addressAfterGrowth == firstAddress && addressAfterGrowth->m_epoch == 10,
			"connection address and contents should survive unrelated map growth");
		Require(server.AcceptConnection(makeRequest(1, 99)).IsOk(),
			"address-stability server should update an existing connection");
		Require(server.FindConnection(1) == firstAddress && firstAddress->m_epoch == 99,
			"same-key connection updates should preserve the published address");

		EditorBridgeServer reentrantServer{};
		for (uint64_t connectionId = 1; connectionId <= 16; ++connectionId)
		{
			Require(reentrantServer.AcceptConnection(makeRequest(connectionId, connectionId)).IsOk(),
				"reentrant server should fill its initial connection capacity");
		}

		bool bHandlerReferenceStable = false;
		reentrantServer.SetCommandHandler([&](const BridgeConnectionInfo& connection, const ProtocolMessage&)
		{
			const BridgeConnectionInfo* addressBeforeInsert = &connection;
			const auto insertion = reentrantServer.AcceptConnection(makeRequest(17, 17));
			bHandlerReferenceStable = insertion.IsOk() &&
				addressBeforeInsert == reentrantServer.FindConnection(connection.m_connectionId) &&
				connection.m_connectionId == 1 && connection.m_epoch == 1;
			return Failure::Ok();
		});

		ProtocolMessage command{};
		command.m_envelope.m_category = MessageCategory::Command;
		Require(reentrantServer.RouteCommand(1, command).IsOk(),
			"reentrant connection handler should accept an unrelated insertion");
		Require(bHandlerReferenceStable,
			"connection handler reference should survive reentrant map growth");
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

	void TestGlobalInputResetClearsLifecycleState()
	{
		using Sailor::Win32::GlobalInput;
		using Sailor::Win32::KeyState;

		GlobalInput::SetKeyState('W', KeyState::Pressed);
		GlobalInput::SetMouseButtonState(0, KeyState::Pressed);
		GlobalInput::SetCursorPosition(320, 240);

		GlobalInput::Reset();
		const auto& state = GlobalInput::GetInputState();
		Require(!state.IsKeyDown('W'), "lifecycle reset should release keyboard input");
		Require(!state.IsButtonDown(0), "lifecycle reset should release mouse input");
		const glm::ivec2 cursor = state.GetCursorPos();
		Require(cursor.x == 0 && cursor.y == 0, "lifecycle reset should clear the stale cursor position");
	}

	void TestEditorRemoteInputAndViewportEventInteropContract()
	{
		const std::filesystem::path sourceRoot = SAILOR_TEST_SOURCE_DIR;
		const std::string bridgeSource = ReadText(sourceRoot / "Runtime/Editor/EditorRuntimeBridge.cpp");
		const std::string appSource = ReadText(sourceRoot / "Runtime/Sailor.cpp");
		const std::string editorInteropSource = ReadText(sourceRoot / "Runtime/Editor/EditorInterop.cpp");
		const std::string editorSource = ReadText(sourceRoot / "Runtime/Submodules/Editor.cpp");
		const std::string viewportControllerSource = ReadText(sourceRoot / "Runtime/Editor/EditorViewportController.cpp");
		const std::string gameObjectSource = ReadText(sourceRoot / "Runtime/Engine/GameObject.cpp");
		const std::string imGuiSource = ReadText(sourceRoot / "Runtime/Submodules/ImGuiApi.cpp");
		const std::string macInputSource = ReadText(sourceRoot / "Editor/Platforms/MacCatalyst/NativeSceneViewportHandler.MacCatalyst.cs");
		const std::string windowsExports = ReadText(sourceRoot / "Lib/DllMain.cpp");
		const std::string portableExports = ReadText(sourceRoot / "Lib/InteropExports.cpp");

		const size_t drainBegin = bridgeSource.find("void Sailor::EditorRuntime::DrainEditorRemoteViewportInputOnEngineThread()");
		const size_t resetBegin = bridgeSource.find("void Sailor::EditorRuntime::ResetForAppLifecycle()", drainBegin);
		Require(drainBegin != std::string::npos && resetBegin != std::string::npos,
			"editor runtime bridge must expose engine-thread input drain and lifecycle reset");
		const std::string drainBody = bridgeSource.substr(drainBegin, resetBegin - drainBegin);
		const size_t takePending = drainBody.find("pendingInput = std::move(g_pendingEditorInput)");
		const size_t validatePending = drainBody.find("IsEditorInputCurrent(input)");
		const size_t dispatchPending = drainBody.find("DispatchEditorInputToRuntime(input)");
		Require(takePending != std::string::npos && validatePending > takePending && dispatchPending > validatePending,
			"remote input must be removed from its FIFO and revalidated before engine-thread dispatch");
		Require(drainBody.find("ResetEditorInputStateOnEngineThread()") != std::string::npos,
			"stale or lifecycle-invalidated input must release runtime interaction state");
		const size_t inputResetBegin = bridgeSource.find("void ResetEditorInputStateOnEngineThread()");
		const size_t inputResetEnd = bridgeSource.find("bool IsEditorInputCurrent", inputResetBegin);
		Require(inputResetBegin != std::string::npos && inputResetEnd != std::string::npos,
			"editor runtime bridge must expose the complete input reset helper");
		const std::string inputResetBody = bridgeSource.substr(inputResetBegin, inputResetEnd - inputResetBegin);
		Require(inputResetBody.find("GlobalInput::Reset()") != std::string::npos &&
			inputResetBody.find("io.ClearInputKeys()") != std::string::npos &&
			inputResetBody.find("io.ClearInputMouse()") != std::string::npos &&
			inputResetBody.find("#if defined(__APPLE__)") == std::string::npos,
			"focus, capture, resize, and lifecycle reset must release native and ImGui input on every platform");
		Require(bridgeSource.find("case InputKind::Capture:\n\t\t\tSyncEditorMouseButtons") != std::string::npos &&
			bridgeSource.find("input.m_kind == InputKind::Capture && !input.m_captured") != std::string::npos,
			"capture loss must release native and ImGui mouse buttons");
		Require(bridgeSource.find("ResolveRemoteMouseButtonState(state, input)") != std::string::npos &&
			bridgeSource.find("HasInputModifier(input.m_modifiers, InputModifier::MouseLeft)") == std::string::npos,
			"mouse buttons must change only through explicit button input so resize cannot restart a held drag");

		const size_t sendBegin = bridgeSource.find("bool App::SendEditorRemoteViewportInput(");
		Require(sendBegin != std::string::npos, "editor runtime bridge must expose remote input interop");
		const std::string sendBody = bridgeSource.substr(sendBegin);
		Require(sendBody.find("g_pendingEditorInput.Add(input)") != std::string::npos,
			"remote input interop must enqueue accepted packets");
		Require(sendBody.find("DispatchEditorInputToRuntime(input)") == std::string::npos,
			"remote input interop must never mutate runtime input from the caller thread");

		const size_t startLoopDrain = appSource.find("EditorRuntime::DrainEditorRemoteViewportInputOnEngineThread()");
		const size_t frameInputCapture = appSource.find("FrameInputState inputState =", startLoopDrain);
		Require(startLoopDrain != std::string::npos && frameInputCapture > startLoopDrain,
			"engine loop must drain remote input before capturing the frame input state");

		const size_t resetEnd = bridgeSource.find("bool Sailor::EditorRuntime::HasAppliedEditorRenderArea()", resetBegin);
		const std::string resetBody = bridgeSource.substr(resetBegin, resetEnd - resetBegin);
		Require(resetBody.find("GlobalInput::Reset()") != std::string::npos &&
			resetBody.find("g_pendingEditorInput.Clear()") != std::string::npos,
			"application lifecycle reset must release active input and discard queued packets");

		const size_t pullBegin = editorInteropSource.find("uint32_t App::PullEditorViewportEvents(char** events, uint32_t num)");
		const size_t pullEnd = editorInteropSource.find("uint32_t App::SerializeCurrentWorld", pullBegin);
		Require(pullBegin != std::string::npos && pullEnd != std::string::npos,
			"editor interop must expose viewport event pulling");
		const std::string pullBody = editorInteropSource.substr(pullBegin, pullEnd - pullBegin);
		Require(pullBody.find("ExecuteOnEngineMainThread<uint32_t>") != std::string::npos &&
			pullBody.find("SetInteropString(event") != std::string::npos,
			"viewport events must be removed on the engine thread and returned as owned interop strings");

		constexpr const char* exportSignature =
			"SAILOR_API uint32_t PullEditorViewportEvents(char** events, uint32_t num)";
		Require(windowsExports.find(exportSignature) != std::string::npos &&
			portableExports.find(exportSignature) != std::string::npos,
			"Windows and portable libraries must export the same viewport event ABI");
		Require(windowsExports.find("SAILOR_API void FreeInteropString(char* text)") != std::string::npos &&
			portableExports.find("SAILOR_API void FreeInteropString(char* text)") != std::string::npos,
			"both libraries must expose the matching interop string release function");
		constexpr const char* mutationRevisionExport =
			"SAILOR_API uint64_t GetEditorManagedMutationRevision(uint32_t kind, const char* strInstanceId)";
		Require(windowsExports.find(mutationRevisionExport) != std::string::npos &&
			portableExports.find(mutationRevisionExport) != std::string::npos,
			"both libraries must expose the managed-mutation ordering fence");

		const size_t setSelectionBegin = editorInteropSource.find("bool App::SetEditorSelection(const char* strSelectionYaml)");
		const size_t setSelectionEnd = editorInteropSource.find("bool App::RenderPathTracedImage", setSelectionBegin);
		Require(setSelectionBegin != std::string::npos && setSelectionEnd > setSelectionBegin,
			"editor interop must expose a bounded managed selection path");
		const std::string setSelectionBody = editorInteropSource.substr(setSelectionBegin, setSelectionEnd - setSelectionBegin);
		const size_t applySelection = setSelectionBody.find("world->SetEditorSelection(selection);");
		const size_t advanceSelectionFence = setSelectionBody.find("editor->NotifyManagedSelectionMutation();");
		Require(applySelection != std::string::npos && advanceSelectionFence > applySelection &&
			setSelectionBody.find("if (world->SetEditorSelection(selection))") == std::string::npos,
			"every managed selection intent must advance the fence, including component-to-owner normalization");

		const size_t updateObjectBegin = editorSource.find("bool Editor::UpdateObject(");
		const size_t reparentObjectBegin = editorSource.find("bool Editor::ReparentObject(", updateObjectBegin);
		Require(updateObjectBegin != std::string::npos && reparentObjectBegin > updateObjectBegin,
			"editor object update source must be bounded");
		const std::string updateObjectBody = editorSource.substr(updateObjectBegin, reparentObjectBegin - updateObjectBegin);
		Require(CountOccurrences(updateObjectBody, "NotifyManagedObjectMutation(instanceId)") == 1,
			"only GameObject transform updates may invalidate a pending transform event; component-only edits must not");
		const size_t resetComponentBegin = editorSource.find("bool Editor::ResetComponentToDefaults(");
		const size_t addComponentBegin = editorSource.find("bool Editor::AddComponent(", resetComponentBegin);
		const size_t removeComponentBegin = editorSource.find("bool Editor::RemoveComponent(", addComponentBegin);
		const size_t instantiatePrefabBegin = editorSource.find("bool Editor::InstantiatePrefab(", removeComponentBegin);
		Require(resetComponentBegin != std::string::npos && addComponentBegin > resetComponentBegin &&
			removeComponentBegin > addComponentBegin && instantiatePrefabBegin > removeComponentBegin,
			"component mutation paths must be bounded for the transform-fence contract");
		Require(editorSource.substr(resetComponentBegin, addComponentBegin - resetComponentBegin).find("NotifyManagedObjectMutation") == std::string::npos &&
			editorSource.substr(addComponentBegin, removeComponentBegin - addComponentBegin).find("NotifyManagedObjectMutation") == std::string::npos &&
			editorSource.substr(removeComponentBegin, instantiatePrefabBegin - removeComponentBegin).find("NotifyManagedObjectMutation") == std::string::npos,
			"component reset/add/remove must preserve a queued transform event for the same owner");

		const size_t completeBegin = viewportControllerSource.find("void EditorViewportController::CompleteActiveTransform(World& world)");
		const size_t completeEnd = viewportControllerSource.find("void EditorViewportController::TickTransformGizmo", completeBegin);
		Require(completeBegin != std::string::npos && completeEnd > completeBegin,
			"viewport controller must expose a bounded transform completion path");
		const std::string completeBody = viewportControllerSource.substr(completeBegin, completeEnd - completeBegin);
		Require(CountOccurrences(completeBody, "QueueTransformEvent(") == 1 &&
			completeBody.find("m_dragInstanceId = InstanceId::Invalid") != std::string::npos &&
			completeBody.find("m_dragManagedObjectMutationRevision = 0") != std::string::npos,
			"one completed drag must enqueue at most one event and consume its active gesture state");

		const size_t transformTickBegin = viewportControllerSource.find("void EditorViewportController::TickTransformGizmo", completeEnd);
		const size_t selectionTickBegin = viewportControllerSource.find("void EditorViewportController::TickSelection", transformTickBegin);
		Require(transformTickBegin != std::string::npos && selectionTickBegin > transformTickBegin,
			"transform gizmo tick must be bounded");
		const std::string transformTickBody = viewportControllerSource.substr(transformTickBegin, selectionTickBegin - transformTickBegin);
		Require(CountOccurrences(transformTickBody, "CompleteActiveTransform(world);") == 4,
			"selection changes, camera loss, zero-sized viewports, and pointer release must each finalize an active drag");
		Require(transformTickBody.find("m_gizmoSubmittedThisFrame = true;") != std::string::npos,
			"pointer ownership must record that ImGuizmo was submitted in the current frame");
		const size_t queueSelectionBegin = viewportControllerSource.find("void EditorViewportController::QueueSelectionEvent", selectionTickBegin);
		Require(selectionTickBegin != std::string::npos && queueSelectionBegin > selectionTickBegin,
			"scene selection tick must be bounded");
		const std::string selectionTickBody = viewportControllerSource.substr(selectionTickBegin, queueSelectionBegin - selectionTickBegin);
		Require(selectionTickBody.find("DoesSubmittedGizmoOwnPointer(") != std::string::npos,
			"scene selection must ignore stale ImGuizmo hover or drag state from a frame without a submitted gizmo");
		Require(selectionTickBody.find("ResolveGameObjectBounds(gameObject, bounds, bUsesMeshBounds) &&") != std::string::npos &&
			selectionTickBody.find("bUsesMeshBounds)") != std::string::npos,
			"viewport picking must exclude empty-object selection fallback bounds");
		const size_t selectedGizmoBegin = gameObjectSource.find("void GameObject::DrawEditorSelectedGizmo()");
		const size_t selectedGizmoEnd = gameObjectSource.find("void GameObject::Tick(", selectedGizmoBegin);
		Require(selectedGizmoBegin != std::string::npos && selectedGizmoEnd > selectedGizmoBegin,
			"selected GameObject gizmo drawing must be bounded");
		const std::string selectedGizmoBody = gameObjectSource.substr(selectedGizmoBegin, selectedGizmoEnd - selectedGizmoBegin);
		Require(selectedGizmoBody.find("if (bUsesMeshBounds)") != std::string::npos &&
			selectedGizmoBody.find("DrawAABB(selectionBounds, selectionColor)") != std::string::npos &&
			selectedGizmoBody.find("DrawSphere(selectionBounds.GetCenter(), 1.0f, selectionColor)") != std::string::npos,
			"selected empty GameObjects must draw a unit center sphere without changing mesh selection bounds");
		const size_t viewportTickBegin = viewportControllerSource.find("void EditorViewportController::Tick(World& world)");
		const size_t viewportResetBegin = viewportControllerSource.find("void EditorViewportController::Reset()", viewportTickBegin);
		Require(viewportTickBegin != std::string::npos && viewportResetBegin > viewportTickBegin &&
			viewportControllerSource.substr(viewportTickBegin, viewportResetBegin - viewportTickBegin).find("ResetImGuizmoInteractionState();") != std::string::npos,
			"a frame without a submitted gizmo must explicitly cancel stale ImGuizmo interaction state");
		const size_t cancelInteractionBegin = viewportControllerSource.find("void EditorViewportController::CancelInteraction(World& world)");
		const size_t cancelInteractionEnd = viewportControllerSource.find("bool EditorViewportController::PullEvent", cancelInteractionBegin);
		Require(cancelInteractionBegin != std::string::npos && cancelInteractionEnd > cancelInteractionBegin &&
			viewportControllerSource.substr(cancelInteractionBegin, cancelInteractionEnd - cancelInteractionBegin).find("CompleteActiveTransform(world);") != std::string::npos &&
			editorSource.find("m_viewportController->CancelInteraction(*m_world);") != std::string::npos,
			"focus or capture loss must finalize the active drag and unlock the toolbar");

		Require(imGuiSource.find("case VK_MENU: return ImGuiKey_ModAlt") != std::string::npos &&
			imGuiSource.find("case VK_LWIN: return ImGuiKey_ModSuper") != std::string::npos,
			"Mac modifier keys must reach ImGui selection suppression");
		Require(macInputSource.find("SceneViewportPointerRouting.ShouldPublishHoverMove(activeMouseModifiers)") != std::string::npos &&
			macInputSource.find("SceneViewportPointerRouting.ShouldPublishCapturedMove(") != std::string::npos &&
			macInputSource.find("(deltaX * sensitivity) / scale") != std::string::npos &&
			macInputSource.find("PublishTouchButton(touches, activeLocalPointerModifier, false);") != std::string::npos &&
			macInputSource.find("activeMouseModifiers | activeKeyboardModifiers") != std::string::npos,
			"Mac pointer input must arbitrate hover and captured motion, normalize Retina deltas, release local trackpad presses, and preserve keyboard modifiers");
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
		{ "EditorBridgeServerConnectionAddressStability", TestEditorBridgeServerConnectionAddressStability },
		{ "EditorRenderFacadeBoundary", TestEditorRenderFacadeBoundary },
		{ "RemoteViewportReconnectTimeoutBackoffAndDiagnostics", TestRemoteViewportReconnectTimeoutBackoffAndDiagnostics },
		{ "RemoteViewportFrameFloodKeepsLatestFrameAndStableCounters", TestRemoteViewportFrameFloodKeepsLatestFrameAndStableCounters },
		{ "RemoteViewportDisconnectRecreateLoopResetsEpochGenerationAndState", TestRemoteViewportDisconnectRecreateLoopResetsEpochGenerationAndState },
		{ "GlobalInputResetClearsLifecycleState", TestGlobalInputResetClearsLifecycleState },
		{ "EditorRemoteInputAndViewportEventInteropContract", TestEditorRemoteInputAndViewportEventInteropContract },
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
