#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "Submodules/EditorRemote/EditorViewportSession.h"
#include "Submodules/EditorRemote/RemoteViewportHarness.h"

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

	ViewportDescriptor MakeViewport(ViewportId viewportId = 7, uint32_t width = 1280, uint32_t height = 720)
	{
		ViewportDescriptor viewport{};
		viewport.m_viewportId = viewportId;
		viewport.m_width = width;
		viewport.m_height = height;
		viewport.m_pixelFormat = PixelFormat::R8G8B8A8_UNorm;
		viewport.m_colorSpace = ColorSpace::Srgb;
		viewport.m_presentMode = PresentMode::Mailbox;
		viewport.m_debugName = "EditorViewport";
		return viewport;
	}

	class HarnessBackedClient : public IEditorViewportClient
	{
	public:
		explicit HarnessBackedClient(RemoteViewportHarness& harness) :
			m_harness(harness)
		{
		}

		Failure Send(const ProtocolMessage& message) override
		{
			m_sent.push_back(message);
			m_harness.EngineReceiveExternalCommand(message);
			return Failure::Ok();
		}

		RemoteViewportHarness& m_harness;
		std::vector<ProtocolMessage> m_sent{};
	};

	class RecordingHost : public IEditorViewportHost
	{
	public:
		Failure ImportTransport(const ViewportDescriptor& viewport, const TransportDescriptor& transport, ConnectionEpoch epoch, SurfaceGeneration generation) override
		{
			m_transportViewport = viewport;
			m_transport = transport;
			m_transportEpoch = epoch;
			m_transportGeneration = generation;
			++m_transportReadyCount;
			m_lastFailure = Failure::Ok();
			return Failure::Ok();
		}

		Failure AcceptFrame(const FramePacket& frame) override
		{
			m_acceptedFrames.push_back(frame);
			m_latestFrame = frame;
			m_lastFailure = Failure::Ok();
			return Failure::Ok();
		}

		Failure PresentLatestFrame(ViewportId viewportId) override
		{
			if (!m_latestFrame.has_value() || m_latestFrame->m_viewportId != viewportId)
			{
				m_lastFailure = Failure::FromDomain(ErrorDomain::Transport, 1, "No accepted frame available for presentation");
				return m_lastFailure;
			}

			m_presentedFrames.push_back(*m_latestFrame);
			m_lastFailure = Failure::Ok();
			return Failure::Ok();
		}

		void ResetViewport(ViewportId viewportId) override
		{
			m_latestFrame.reset();
			m_resets.push_back(viewportId);
		}

		Failure GetLastFailure() const override
		{
			return m_lastFailure;
		}

		ViewportDescriptor m_transportViewport{};
		TransportDescriptor m_transport{};
		ConnectionEpoch m_transportEpoch = 0;
		SurfaceGeneration m_transportGeneration = 0;
		size_t m_transportReadyCount = 0;
		std::optional<FramePacket> m_latestFrame{};
		std::vector<FramePacket> m_acceptedFrames{};
		std::vector<FramePacket> m_presentedFrames{};
		std::vector<ViewportId> m_resets{};
		Failure m_lastFailure = Failure::Ok();
	};

	void DrainHarnessEvents(RemoteViewportHarness& harness, EditorViewportSession& session)
	{
		while (auto message = harness.PopEngineEventForExternalEditor())
		{
			(void)session.HandleMessage(*message);
		}
	}

	void TestEditorViewportSessionCreateHandshakeAndVisibilityReplay()
	{
		RemoteViewportHarness harness{ MakeViewport() };
		HarnessBackedClient client{ harness };
		RecordingHost host{};
		EditorViewportSession session{ MakeViewport(), client, host };

		Require(session.Create().IsOk(), "create should send initial viewport command");
		DrainHarnessEvents(harness, session);
		Require(session.GetState() == SessionState::Active, "transport ready should activate visible session");
		Require(session.IsReady(), "handshake should mark session ready");
		Require(host.m_transportReadyCount == 1, "host should import negotiated transport");
		Require(client.m_sent.size() == 1 && client.m_sent.front().m_envelope.m_commandType == CommandType::CreateViewport, "create should emit CreateViewport only once");

		Require(session.SetVisible(false).IsOk(), "hidden transition should be accepted");
		Require(session.GetState() == SessionState::Paused, "hidden ready session should pause");
		Require(client.m_sent.back().m_envelope.m_commandType == CommandType::SuspendViewport, "hiding should emit suspend command");
	}

	void TestEditorViewportSessionResizeDropsStaleGenerationFrames()
	{
		RemoteViewportHarness harness{ MakeViewport() };
		HarnessBackedClient client{ harness };
		RecordingHost host{};
		EditorViewportSession session{ MakeViewport(), client, host };

		Require(session.Create().IsOk(), "create should succeed");
		DrainHarnessEvents(harness, session);
		Require(session.Resize(1600, 900).IsOk(), "resize should update desired viewport and request new generation");
		Require(session.GetGeneration() == 2, "resize should advance generation");
		Require(session.GetState() == SessionState::Resizing, "resize should enter resizing state");

		FramePacket staleFrame{};
		staleFrame.m_viewportId = 7;
		staleFrame.m_connectionEpoch = session.GetConnectionEpoch();
		staleFrame.m_generation = 1;
		staleFrame.m_frameIndex = 99;
		staleFrame.m_width = 1280;
		staleFrame.m_height = 720;
		ProtocolMessage stale = ProtocolMessage::MakeFrameReady(staleFrame);
		Require(!session.HandleMessage(stale).IsOk(), "stale generation frame should be rejected");
		Require(session.GetRejectedFrameCount(GuardDecision::RejectStaleGeneration) == 1, "stale generation should be tracked");

		DrainHarnessEvents(harness, session);
		Require(session.IsReady(), "new generation transport should become ready");
		Require(host.m_transportGeneration == 2, "host should observe resized generation");

		harness.EngineInjectFrameReady();
		DrainHarnessEvents(harness, session);
		Require(session.GetAcceptedFrameCount() == 1, "current generation frame should present successfully");
		Require(session.GetLastPresentedFrameIndex() == 1, "frame index should track accepted frame");
		Require(!host.m_presentedFrames.empty() && host.m_presentedFrames.back().m_generation == 2, "host should present active-generation frame only");
	}

	void TestEditorViewportSessionReconnectReplaysDesiredStateAndFocus()
	{
		RemoteViewportHarness harness{ MakeViewport() };
		HarnessBackedClient client{ harness };
		RecordingHost host{};
		EditorViewportSession session{ MakeViewport(), client, host };

		Require(session.Create().IsOk(), "initial create should succeed");
		DrainHarnessEvents(harness, session);
		Require(session.SetVisible(false).IsOk(), "session should store hidden desired state");
		Require(session.SetFocused(true).IsOk(), "focus update should forward input packet");
		Require(session.GetLastForwardedInput().has_value() && session.GetLastForwardedInput()->m_kind == InputKind::Focus, "focus should be forwarded as input hook");

		Require(session.HandleDisconnect(2).IsOk(), "disconnect should begin recovery in next epoch");
		Require(session.GetConnectionEpoch() == 2, "disconnect should advance epoch");
		Require(session.GetGeneration() == 1, "new epoch should reset generation");
		Require(session.GetState() == SessionState::Recovering, "disconnect should place session into recovering state");

		auto sentBeforeReplay = client.m_sent.size();
		Require(session.ReplayDesiredState().IsOk(), "reconnect should replay desired viewport state");
		Require(client.m_sent.size() == sentBeforeReplay + 3, "replay should emit create + suspend + focus input");
		Require(client.m_sent[sentBeforeReplay + 0].m_envelope.m_commandType == CommandType::CreateViewport, "replay should recreate viewport first");
		Require(client.m_sent[sentBeforeReplay + 1].m_envelope.m_commandType == CommandType::SuspendViewport, "replay should restore hidden state");
		Require(client.m_sent[sentBeforeReplay + 2].m_envelope.m_commandType == CommandType::Input, "replay should restore focus hook");

		DrainHarnessEvents(harness, session);
		Require(session.GetState() == SessionState::Paused, "hidden desired state should keep recovered session paused after ready");
		Require(host.m_transportEpoch == 2, "host should import transport for the replayed epoch");
	}
	void TestEditorViewportSessionDiagnosticsAndTimeouts()
	{
		RemoteViewportHarness harness{ MakeViewport() };
		HarnessBackedClient client{ harness };
		RecordingHost host{};
		EditorViewportSession session{ MakeViewport(), client, host };

		Require(session.Create().IsOk(), "create should arm transport timeout");
		Require(session.TickTimeouts(1000).IsOk(), "transport timeout should be handled");
		Require(session.GetState() == SessionState::Recovering, "transport timeout should enter recovering state");
		Require(session.GetDiagnostics().m_lastCategory == DiagnosticCategory::Timeout, "diagnostics should mark transport timeout");

		Require(session.HandleDisconnect(3).IsOk(), "disconnect should start reconnect flow");
		auto backoff1 = session.ScheduleReconnectAttempt();
		auto backoff2 = session.ScheduleReconnectAttempt();
		Require(backoff1.m_delayMs == 100 && backoff2.m_delayMs == 200, "editor reconnect backoff should double between attempts");
		Require(session.TickTimeouts(5000).IsOk(), "reconnect timeout should be handled");
		Require(session.GetState() == SessionState::Lost, "reconnect timeout should promote editor session to lost");
		Require(session.GetDiagnostics().m_lastFailure.has_value() && session.GetDiagnostics().m_lastFailure->m_scope == FailureScope::Connection, "diagnostics should retain connection-scoped timeout");
	}

	void TestEditorViewportSessionResizeStormRejectsStaleFramesAndEndsOnLatestGeneration()
	{
		RemoteViewportHarness harness{ MakeViewport() };
		HarnessBackedClient client{ harness };
		RecordingHost host{};
		EditorViewportSession session{ MakeViewport(), client, host };

		Require(session.Create().IsOk(), "resize storm should create initial session");
		DrainHarnessEvents(harness, session);

		for (uint32_t i = 0; i < 32; ++i)
		{
			Require(session.Resize(1280 + i * 8, 720 + i * 4).IsOk(), "resize storm should accept sequential resizes");

			FramePacket staleFrame{};
			staleFrame.m_viewportId = 7;
			staleFrame.m_connectionEpoch = session.GetConnectionEpoch();
			staleFrame.m_generation = session.GetGeneration() - 1;
			staleFrame.m_frameIndex = 1000 + i;
			staleFrame.m_width = 1280;
			staleFrame.m_height = 720;
			ProtocolMessage stale = ProtocolMessage::MakeFrameReady(staleFrame);
			Require(!session.HandleMessage(stale).IsOk(), "resize storm should reject stale-generation frame immediately");

			DrainHarnessEvents(harness, session);
		}

		harness.EngineInjectFrameReady();
		DrainHarnessEvents(harness, session);
		Require(session.IsReady(), "resize storm should end with negotiated latest generation");
		Require(session.GetGeneration() == 33, "resize storm should monotonically advance generation");
		Require(session.GetRejectedFrameCount(GuardDecision::RejectStaleGeneration) == 32, "resize storm should count each rejected stale frame");
		Require(session.GetAcceptedFrameCount() == 1, "resize storm should present only the final active-generation frame in this scenario");
		Require(host.m_transportGeneration == 33, "host should import only the latest negotiated generation at the end of the storm");
		Require(session.GetLastPresentedFrameIndex() == 1, "resize storm should reset presentation tracking to the final negotiated generation only");
	}

	void TestEditorViewportSessionReconnectLoopReplaysStateWithoutGhostFrames()
	{
		RemoteViewportHarness harness{ MakeViewport() };
		HarnessBackedClient client{ harness };
		RecordingHost host{};
		EditorViewportSession session{ MakeViewport(), client, host };

		Require(session.Create().IsOk(), "reconnect loop should create session");
		DrainHarnessEvents(harness, session);
		Require(session.SetVisible(false).IsOk(), "reconnect loop should preserve hidden desired state");
		Require(session.SetFocused(true).IsOk(), "reconnect loop should preserve focused desired state");

		for (ConnectionEpoch epoch = 2; epoch < 8; ++epoch)
		{
			Require(session.HandleDisconnect(epoch).IsOk(), "disconnect loop should enter recovering");

			FramePacket staleFrame{};
			staleFrame.m_viewportId = 7;
			staleFrame.m_connectionEpoch = epoch - 1;
			staleFrame.m_generation = 1;
			staleFrame.m_frameIndex = 500 + epoch;
			staleFrame.m_width = 1280;
			staleFrame.m_height = 720;
			Require(!session.HandleMessage(ProtocolMessage::MakeFrameReady(staleFrame)).IsOk(), "reconnect loop should reject stale-epoch ghost frames");

			auto sentBeforeReplay = client.m_sent.size();
			Require(session.ReplayDesiredState().IsOk(), "reconnect loop should replay desired state after disconnect");
			Require(client.m_sent.size() == sentBeforeReplay + 3, "reconnect loop should replay create + suspend + focus each time");
			DrainHarnessEvents(harness, session);
			Require(session.GetState() == SessionState::Paused, "hidden desired state should remain paused after reconnect");
			Require(host.m_transportEpoch == epoch, "host should import transport for the current reconnect epoch");
		}

		Require(session.GetRejectedFrameCount(GuardDecision::RejectStaleEpoch) == 6, "reconnect loop should count each ghost frame rejection");
		Require(session.GetDiagnostics().m_recoveryAttemptCount == 6, "reconnect loop should record one recovery attempt per disconnect");
	}

	void TestEditorViewportSessionFrameFloodPresentsLatestFrameWithoutStateDrift()
	{
		RemoteViewportHarness harness{ MakeViewport() };
		HarnessBackedClient client{ harness };
		RecordingHost host{};
		EditorViewportSession session{ MakeViewport(), client, host };

		Require(session.Create().IsOk(), "frame flood should create session");
		DrainHarnessEvents(harness, session);

		constexpr size_t kFrameFloodCount = 128;
		for (size_t i = 0; i < kFrameFloodCount; ++i)
		{
			harness.EngineInjectFrameReady();
			DrainHarnessEvents(harness, session);
		}

		Require(session.GetAcceptedFrameCount() == kFrameFloodCount, "frame flood should accept every current-generation frame");
		Require(host.m_presentedFrames.size() == kFrameFloodCount, "frame flood should present each accepted frame without queue buildup in host contract");
		Require(session.GetLastPresentedFrameIndex() == kFrameFloodCount, "frame flood should retain the newest sequentially delivered frame index");
		Require(session.GetDiagnostics().m_lastGoodFrameIndex == kFrameFloodCount, "frame flood diagnostics should stay aligned with latest frame");
		Require(session.GetState() == SessionState::Active, "frame flood should not perturb active state");
	}

}

int main()
{
	const std::pair<const char*, std::function<void()>> tests[] = {
		{ "EditorViewportSessionCreateHandshakeAndVisibilityReplay", TestEditorViewportSessionCreateHandshakeAndVisibilityReplay },
		{ "EditorViewportSessionResizeDropsStaleGenerationFrames", TestEditorViewportSessionResizeDropsStaleGenerationFrames },
		{ "EditorViewportSessionReconnectReplaysDesiredStateAndFocus", TestEditorViewportSessionReconnectReplaysDesiredStateAndFocus },
		{ "EditorViewportSessionDiagnosticsAndTimeouts", TestEditorViewportSessionDiagnosticsAndTimeouts },
		{ "EditorViewportSessionResizeStormRejectsStaleFramesAndEndsOnLatestGeneration", TestEditorViewportSessionResizeStormRejectsStaleFramesAndEndsOnLatestGeneration },
		{ "EditorViewportSessionReconnectLoopReplaysStateWithoutGhostFrames", TestEditorViewportSessionReconnectLoopReplaysStateWithoutGhostFrames },
		{ "EditorViewportSessionFrameFloodPresentsLatestFrameWithoutStateDrift", TestEditorViewportSessionFrameFloodPresentsLatestFrameWithoutStateDrift },
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
