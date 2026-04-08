#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>

#include "Submodules/EditorRemote/RemoteViewportFoundation.h"
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

	ViewportDescriptor MakeViewport(uint32_t width = 1280, uint32_t height = 720)
	{
		ViewportDescriptor viewport{};
		viewport.m_viewportId = 1;
		viewport.m_width = width;
		viewport.m_height = height;
		viewport.m_pixelFormat = PixelFormat::R8G8B8A8_UNorm;
		viewport.m_colorSpace = ColorSpace::Srgb;
		viewport.m_presentMode = PresentMode::Mailbox;
		viewport.m_debugName = "HarnessViewport";
		return viewport;
	}

	void TestCreateReadyHandshake()
	{
		RemoteViewportHarness harness{ MakeViewport() };
		harness.EditorCreate();
		Require(harness.EditorState() == SessionState::Negotiating, "editor should negotiate after create");
		Require(harness.EngineState() == SessionState::Created, "engine should stay created until messages are pumped");

		harness.PumpEditorToEngine();
		Require(harness.EngineState() == SessionState::Ready, "engine should reach ready after create processing");
		Require(harness.EditorAcceptedFrameCount() == 0, "no frames should be accepted before ready ack");

		harness.EngineInjectFrameReady();
		harness.PumpEngineToEditor();
		Require(harness.EditorAcceptedFrameCount() == 0, "frame must not be accepted before ready ack");
		Require(harness.EditorRejectedFrameCount(GuardDecision::RejectNotReady) == 1, "frame before ready should be rejected as not ready");

		harness.PumpEngineToEditor();
		Require(harness.EditorState() == SessionState::Active, "editor should become active after ready ack");
		Require(harness.EditorReadyAckCount() == 1, "editor should acknowledge transport readiness once");

		harness.EngineInjectFrameReady();
		harness.PumpEngineToEditor();
		Require(harness.EditorAcceptedFrameCount() == 1, "frame should be accepted after ready ack");
	}

	void TestResizeWhileFrameInFlight()
	{
		RemoteViewportHarness harness{ MakeViewport() };
		harness.BringUpActiveSession();
		Require(harness.CurrentGeneration() == 1, "initial generation should be one");

		harness.EngineBeginInFlightFrame();
		Require(harness.EngineHasInFlightFrame(), "engine should track in-flight frame");

		harness.EditorResize(1600, 900);
		Require(harness.EditorState() == SessionState::Resizing, "editor should enter resizing state");
		Require(harness.CurrentGeneration() == 2, "resize should advance generation immediately");

		harness.PumpEditorToEngine();
		Require(harness.EngineState() == SessionState::Ready, "engine should recreate and report ready on resize");

		harness.EngineCompleteInFlightFrame();
		harness.PumpEngineToEditor();
		Require(harness.EditorRejectedFrameCount(GuardDecision::RejectStaleGeneration) == 1, "old generation frame should be dropped after resize");
		Require(harness.EditorAcceptedFrameCount() == 1, "stale frame must not replace prior accepted frame count");

		harness.PumpEngineToEditor();
		Require(harness.EditorState() == SessionState::Active, "editor should return active after resized transport is ready");

		harness.EngineInjectFrameReady();
		harness.PumpEngineToEditor();
		Require(harness.EditorAcceptedFrameCount() == 2, "new generation frame should be accepted");
	}

	void TestDestroyDuringInFlightWork()
	{
		RemoteViewportHarness harness{ MakeViewport() };
		harness.BringUpActiveSession();
		harness.EngineBeginInFlightFrame();

		harness.EditorDestroy();
		Require(harness.EditorState() == SessionState::Disposed, "editor destroy should be immediate and idempotent");

		harness.PumpEditorToEngine();
		Require(harness.EngineState() == SessionState::Disposed, "engine should dispose after destroy command");

		harness.EngineCompleteInFlightFrame();
		harness.PumpEngineToEditor();
		Require(harness.EditorAcceptedFrameCount() == 1, "destroyed session must not accept post-destroy frames");
		Require(harness.EditorRejectedFrameCount(GuardDecision::RejectNotReady) == 1, "post-destroy frame should be rejected safely");
	}

	void TestDisconnectReconnectWithNewEpoch()
	{
		RemoteViewportHarness harness{ MakeViewport() };
		harness.BringUpActiveSession();
		Require(harness.CurrentEpoch() == 1, "initial epoch should be one");

		harness.Disconnect();
		Require(harness.EditorState() == SessionState::Recovering, "disconnect should move editor into recovering");
		Require(harness.EngineState() == SessionState::Lost, "disconnect should move engine into lost");

		harness.EngineInjectFrameReady(1, 1);
		harness.PumpEngineToEditor();
		Require(harness.EditorRejectedFrameCount(GuardDecision::RejectStaleEpoch) == 1, "old epoch frame should be rejected after disconnect");

		harness.Reconnect();
		Require(harness.CurrentEpoch() == 2, "reconnect should advance epoch");
		Require(harness.EditorState() == SessionState::Negotiating, "editor should renegotiate desired session on reconnect");

		harness.PumpEditorToEngine();
		harness.PumpEngineToEditor();
		Require(harness.EditorState() == SessionState::Active, "reconnected session should return active");

		harness.EngineInjectFrameReady(1, 2);
		harness.PumpEngineToEditor();
		Require(harness.EditorAcceptedFrameCount() == 2, "new epoch frame should be accepted after reconnect");
	}

	void TestVisibilityPausedSchedulingContract()
	{
		RemoteViewportHarness harness{ MakeViewport() };
		harness.BringUpActiveSession();
		Require(harness.EngineCanScheduleFrame(), "engine should schedule while active");

		harness.EditorSetVisible(false);
		Require(harness.EditorState() == SessionState::Paused, "editor should enter paused when hidden");
		harness.PumpEditorToEngine();
		Require(harness.EngineState() == SessionState::Paused, "engine should pause when hidden");
		Require(!harness.EngineCanScheduleFrame(), "engine should stop scheduling frames while paused");

		harness.EditorSetVisible(true);
		Require(harness.EditorState() == SessionState::Active, "editor should return active when visible again");
		harness.PumpEditorToEngine();
		Require(harness.EngineState() == SessionState::Active, "engine should resume when visible again");
		Require(harness.EngineCanScheduleFrame(), "engine should schedule again after resume");
	}
}

int main()
{
	const std::pair<const char*, std::function<void()>> tests[] = {
		{ "CreateReadyHandshake", TestCreateReadyHandshake },
		{ "ResizeWhileFrameInFlight", TestResizeWhileFrameInFlight },
		{ "DestroyDuringInFlightWork", TestDestroyDuringInFlightWork },
		{ "DisconnectReconnectWithNewEpoch", TestDisconnectReconnectWithNewEpoch },
		{ "VisibilityPausedSchedulingContract", TestVisibilityPausedSchedulingContract },
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
