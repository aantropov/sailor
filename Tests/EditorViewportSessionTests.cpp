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
}

int main()
{
	const std::pair<const char*, std::function<void()>> tests[] = {
		{ "EditorViewportSessionCreateHandshakeAndVisibilityReplay", TestEditorViewportSessionCreateHandshakeAndVisibilityReplay },
		{ "EditorViewportSessionResizeDropsStaleGenerationFrames", TestEditorViewportSessionResizeDropsStaleGenerationFrames },
		{ "EditorViewportSessionReconnectReplaysDesiredStateAndFocus", TestEditorViewportSessionReconnectReplaysDesiredStateAndFocus },
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
