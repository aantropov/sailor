#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>

#include "Submodules/EditorRemote/RemoteViewportFoundation.h"

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

	void TestProtocolValidationAndRoundtrip()
	{
		ViewportDescriptor viewport{};
		viewport.m_viewportId = 7;
		viewport.m_width = 1920;
		viewport.m_height = 1080;
		viewport.m_pixelFormat = PixelFormat::R8G8B8A8_UNorm;
		viewport.m_colorSpace = ColorSpace::Srgb;
		viewport.m_sampleCount = 1;
		viewport.m_debugName = "MainViewport";

		Require(viewport.Validate().IsOk(), "viewport descriptor should validate");

		TransportDescriptor transport{};
		transport.m_transportType = TransportType::MailboxCpuCopy;
		transport.m_syncMode = SyncMode::Implicit;
		transport.m_protocolVersion = 1;
		transport.m_width = viewport.m_width;
		transport.m_height = viewport.m_height;
		transport.m_pixelFormat = viewport.m_pixelFormat;
		transport.m_ready = true;

		Require(transport.Validate().IsOk(), "transport descriptor should validate");

		FramePacket frame{};
		frame.m_viewportId = viewport.m_viewportId;
		frame.m_connectionEpoch = 3;
		frame.m_generation = 5;
		frame.m_frameIndex = 11;
		frame.m_width = viewport.m_width;
		frame.m_height = viewport.m_height;
		frame.m_timestampNs = 42;

		Require(frame.Validate().IsOk(), "frame packet should validate");

		ProtocolMessage original = ProtocolMessage::MakeFrameReady(frame);
		auto roundtrip = ProtocolMessage::FromEnvelope(original.ToEnvelope());
		Require(roundtrip == original, "frame packet envelope roundtrip should preserve payload");

		InputPacket input{};
		input.m_viewportId = viewport.m_viewportId;
		input.m_connectionEpoch = 3;
		input.m_generation = 5;
		input.m_kind = InputKind::PointerMove;
		input.m_modifiers = InputModifier::Shift | InputModifier::MouseLeft;
		input.m_pointerX = 15.0f;
		input.m_pointerY = 30.0f;
		input.m_timestampNs = 100;

		Require(input.Validate().IsOk(), "input packet should validate");
		Require((input.m_modifiers & InputModifier::Shift) == InputModifier::Shift, "modifier bitmask should work");

		ViewportDescriptor invalidViewport = viewport;
		invalidViewport.m_width = 0;
		Require(!invalidViewport.Validate().IsOk(), "zero width viewport should be rejected");

		TransportDescriptor invalidTransport = transport;
		invalidTransport.m_transportType = TransportType::Unknown;
		Require(!invalidTransport.Validate().IsOk(), "unknown transport should be rejected");
	}

	void TestSessionStateTransitions()
	{
		SessionStateMachine session{};
		Require(session.GetState() == SessionState::Created, "session should start in Created");
		Require(session.TransitionTo(SessionState::Negotiating).IsOk(), "Created -> Negotiating should succeed");
		Require(session.TransitionTo(SessionState::Ready).IsOk(), "Negotiating -> Ready should succeed");
		Require(session.TransitionTo(SessionState::Active).IsOk(), "Ready -> Active should succeed");
		Require(session.TransitionTo(SessionState::Resizing).IsOk(), "Active -> Resizing should succeed");
		Require(session.TransitionTo(SessionState::Active).IsOk(), "Resizing -> Active should succeed");
		Require(session.TransitionTo(SessionState::Paused).IsOk(), "Active -> Paused should succeed");
		Require(session.TransitionTo(SessionState::Active).IsOk(), "Paused -> Active should succeed");
		Require(session.TransitionTo(SessionState::Recovering).IsOk(), "Active -> Recovering should succeed");
		Require(session.TransitionTo(SessionState::Lost).IsOk(), "Recovering -> Lost should succeed");
		Require(session.TransitionTo(SessionState::Negotiating).IsOk(), "Lost -> Negotiating should succeed");
		Require(session.TransitionTo(SessionState::Ready).IsOk(), "Negotiating -> Ready after loss should succeed");

		SessionStateMachine invalid{};
		Require(!invalid.TransitionTo(SessionState::Active).IsOk(), "Created -> Active should fail");
		Require(invalid.GetState() == SessionState::Created, "invalid transition must not mutate state");

		Require(invalid.Destroy().IsOk(), "destroy should work from Created");
		Require(invalid.GetState() == SessionState::Disposed, "destroy should end in Disposed");
		Require(invalid.Destroy().IsOk(), "destroy should be idempotent");
		Require(invalid.GetState() == SessionState::Disposed, "idempotent destroy keeps Disposed state");
	}

	void TestGenerationAndEpochGuards()
	{
		SessionGuards guards{ 4, 2 };

		FramePacket beforeReady{};
		beforeReady.m_viewportId = 1;
		beforeReady.m_connectionEpoch = 4;
		beforeReady.m_generation = 2;
		beforeReady.m_frameIndex = 1;
		beforeReady.m_width = 1280;
		beforeReady.m_height = 720;
		beforeReady.m_timestampNs = 1;

		Require(guards.AcceptFrame(beforeReady) == GuardDecision::RejectNotReady, "frame before ready must be rejected");
		Require(guards.AcknowledgeTransportReady(4, 2) == GuardDecision::Accept, "matching ready ack should be accepted");
		Require(guards.AcceptFrame(beforeReady) == GuardDecision::Accept, "matching frame should be accepted after ready ack");

		FramePacket staleGeneration = beforeReady;
		staleGeneration.m_generation = 1;
		Require(guards.AcceptFrame(staleGeneration) == GuardDecision::RejectStaleGeneration, "old generation frame should be rejected");

		FramePacket staleEpoch = beforeReady;
		staleEpoch.m_connectionEpoch = 3;
		Require(guards.AcceptFrame(staleEpoch) == GuardDecision::RejectStaleEpoch, "old epoch frame should be rejected");

		InputPacket input{};
		input.m_viewportId = 1;
		input.m_connectionEpoch = 4;
		input.m_generation = 2;
		input.m_kind = InputKind::PointerMove;
		input.m_timestampNs = 2;
		Require(guards.AcceptInput(input) == GuardDecision::Accept, "matching input should be accepted");

		guards.AdvanceGeneration();
		Require(guards.GetGeneration() == 3, "generation should increment");
		Require(guards.AcceptFrame(beforeReady) == GuardDecision::RejectStaleGeneration, "old frame after resize must be rejected");

		guards.BeginNewConnectionEpoch(5);
		Require(guards.GetConnectionEpoch() == 5, "epoch should update");
		Require(guards.AcceptInput(input) == GuardDecision::RejectStaleEpoch, "old epoch input after reconnect must be rejected");
		Require(guards.AcknowledgeTransportReady(4, 3) == GuardDecision::RejectStaleEpoch, "old epoch ready ack must be rejected");
	}

	void TestFailureClassification()
	{
		Require(ClassifyResult(ErrorDomain::None, 0) == ResultCode::Ok, "no error should map to Ok");
		Require(ClassifyResult(ErrorDomain::Session, 1) == ResultCode::Retryable, "transient session error should be retryable");
		Require(ClassifyResult(ErrorDomain::Session, 2) == ResultCode::RecreateRequired, "session recreate error should map correctly");
		Require(ClassifyResult(ErrorDomain::Connection, 1) == ResultCode::ReconnectRequired, "connection error should require reconnect");
		Require(ClassifyResult(ErrorDomain::Capability, 1) == ResultCode::Unsupported, "capability mismatch should be unsupported");
		Require(ClassifyResult(ErrorDomain::Protocol, 1) == ResultCode::FatalProtocolError, "protocol violation should be fatal protocol error");
		Require(ClassifyResult(ErrorDomain::Transport, 1) == ResultCode::FatalTransportError, "transport failure should be fatal transport error");

		Failure ok = Failure::Ok();
		Require(ok.IsOk(), "ok failure wrapper should report ok");

		Failure reconnect = Failure::FromDomain(ErrorDomain::Connection, 7, "disconnect");
		Require(reconnect.m_code == ResultCode::ReconnectRequired, "connection failure wrapper should classify as reconnect");
		Require(reconnect.m_scope == FailureScope::Connection, "connection failure should be connection-scoped");
	}
}

int main()
{
	const std::pair<const char*, std::function<void()>> tests[] = {
		{ "ProtocolValidationAndRoundtrip", TestProtocolValidationAndRoundtrip },
		{ "SessionStateTransitions", TestSessionStateTransitions },
		{ "GenerationAndEpochGuards", TestGenerationAndEpochGuards },
		{ "FailureClassification", TestFailureClassification },
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
