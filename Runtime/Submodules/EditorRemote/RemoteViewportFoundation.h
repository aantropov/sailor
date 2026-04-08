#pragma once

#include <compare>
#include <cstdint>
#include <string>
#include <variant>

namespace Sailor::EditorRemote
{
	using ViewportId = uint64_t;
	using ConnectionEpoch = uint64_t;
	using SurfaceGeneration = uint64_t;
	using FrameIndex = uint64_t;

	enum class ErrorDomain : uint8_t;
	enum class ResultCode : uint8_t;
	enum class FailureScope : uint8_t;

	inline constexpr ResultCode ClassifyResult(ErrorDomain domain, int32_t nativeCode);
	inline constexpr FailureScope ClassifyScope(ErrorDomain domain);

	enum class PixelFormat : uint8_t
	{
		Unknown = 0,
		R8G8B8A8_UNorm,
		B8G8R8A8_UNorm,
		R16G16B16A16_Float,
	};

	enum class ColorSpace : uint8_t
	{
		Unknown = 0,
		Srgb,
		Linear,
		Hdr10,
	};

	enum class PresentMode : uint8_t
	{
		Unknown = 0,
		Mailbox,
		Immediate,
		Fifo,
	};

	enum class TransportType : uint8_t
	{
		Unknown = 0,
		MailboxCpuCopy,
		WinSharedHandle,
		MacIOSurface,
	};

	enum class SyncMode : uint8_t
	{
		Unknown = 0,
		Implicit,
		ExplicitFence,
		ExplicitSemaphore,
	};

	enum class MessageCategory : uint8_t
	{
		Unknown = 0,
		Command,
		Event,
		Diagnostic,
	};

	enum class CommandType : uint8_t
	{
		Unknown = 0,
		CreateViewport,
		ResizeViewport,
		DestroyViewport,
		Input,
		SuspendViewport,
		ResumeViewport,
	};

	enum class EventType : uint8_t
	{
		Unknown = 0,
		TransportReady,
		FrameReady,
		SessionStateChanged,
		SessionFailed,
		Destroyed,
	};

	enum class SessionState : uint8_t
	{
		Created = 0,
		Negotiating,
		Ready,
		Active,
		Paused,
		Resizing,
		Recovering,
		Lost,
		Terminating,
		Disposed,
	};

	enum class InputKind : uint8_t
	{
		Unknown = 0,
		PointerMove,
		PointerButton,
		PointerWheel,
		Key,
		Focus,
		Capture,
	};

	enum class InputModifier : uint16_t
	{
		None = 0,
		Shift = 1 << 0,
		Control = 1 << 1,
		Alt = 1 << 2,
		Meta = 1 << 3,
		MouseLeft = 1 << 4,
		MouseRight = 1 << 5,
		MouseMiddle = 1 << 6,
	};

	inline constexpr InputModifier operator|(InputModifier lhs, InputModifier rhs)
	{
		return static_cast<InputModifier>(static_cast<uint16_t>(lhs) | static_cast<uint16_t>(rhs));
	}

	inline constexpr InputModifier operator&(InputModifier lhs, InputModifier rhs)
	{
		return static_cast<InputModifier>(static_cast<uint16_t>(lhs) & static_cast<uint16_t>(rhs));
	}

	enum class ResultCode : uint8_t
	{
		Ok = 0,
		Retryable,
		RecreateRequired,
		ReconnectRequired,
		Unsupported,
		FatalProtocolError,
		FatalTransportError,
	};

	enum class FailureScope : uint8_t
	{
		None = 0,
		Session,
		Connection,
		Global,
	};

	enum class ErrorDomain : uint8_t
	{
		None = 0,
		Session,
		Connection,
		Capability,
		Protocol,
		Transport,
	};

	struct Failure
	{
		ResultCode m_code = ResultCode::Ok;
		FailureScope m_scope = FailureScope::None;
		int32_t m_nativeCode = 0;
		std::string m_message;

		bool IsOk() const { return m_code == ResultCode::Ok; }
		auto operator<=>(const Failure&) const = default;

		static Failure Ok()
		{
			return {};
		}

		static Failure FromDomain(ErrorDomain domain, int32_t nativeCode, std::string message)
		{
			Failure result{};
			result.m_code = ClassifyResult(domain, nativeCode);
			result.m_scope = ClassifyScope(domain);
			result.m_nativeCode = nativeCode;
			result.m_message = std::move(message);
			return result;
		}
	};

	inline constexpr ResultCode ClassifyResult(ErrorDomain domain, int32_t nativeCode)
	{
		switch (domain)
		{
		case ErrorDomain::None: return ResultCode::Ok;
		case ErrorDomain::Session: return nativeCode == 2 ? ResultCode::RecreateRequired : ResultCode::Retryable;
		case ErrorDomain::Connection: return ResultCode::ReconnectRequired;
		case ErrorDomain::Capability: return ResultCode::Unsupported;
		case ErrorDomain::Protocol: return ResultCode::FatalProtocolError;
		case ErrorDomain::Transport: return ResultCode::FatalTransportError;
		default: return ResultCode::FatalProtocolError;
		}
	}

	inline constexpr FailureScope ClassifyScope(ErrorDomain domain)
	{
		switch (domain)
		{
		case ErrorDomain::None: return FailureScope::None;
		case ErrorDomain::Session: return FailureScope::Session;
		case ErrorDomain::Connection: return FailureScope::Connection;
		case ErrorDomain::Capability:
		case ErrorDomain::Protocol:
		case ErrorDomain::Transport:
		default:
			return FailureScope::Global;
		}
	}

	struct ViewportDescriptor
	{
		ViewportId m_viewportId = 0;
		uint32_t m_width = 0;
		uint32_t m_height = 0;
		PixelFormat m_pixelFormat = PixelFormat::Unknown;
		ColorSpace m_colorSpace = ColorSpace::Unknown;
		bool m_hdr = false;
		uint8_t m_sampleCount = 1;
		uint32_t m_usageFlags = 0;
		PresentMode m_presentMode = PresentMode::Mailbox;
		std::string m_debugName;

		Failure Validate() const
		{
			if (m_viewportId == 0 || m_width == 0 || m_height == 0)
			{
				return Failure::FromDomain(ErrorDomain::Protocol, 1, "Viewport descriptor dimensions/id must be valid");
			}
			if (m_pixelFormat == PixelFormat::Unknown || m_colorSpace == ColorSpace::Unknown || m_presentMode == PresentMode::Unknown)
			{
				return Failure::FromDomain(ErrorDomain::Protocol, 1, "Viewport descriptor enums must be negotiated");
			}
			if (m_sampleCount == 0)
			{
				return Failure::FromDomain(ErrorDomain::Protocol, 1, "Viewport descriptor sample count must be non-zero");
			}
			return Failure::Ok();
		}

		auto operator<=>(const ViewportDescriptor&) const = default;
	};

	struct TransportDescriptor
	{
		TransportType m_transportType = TransportType::Unknown;
		SyncMode m_syncMode = SyncMode::Unknown;
		uint32_t m_protocolVersion = 0;
		uint32_t m_width = 0;
		uint32_t m_height = 0;
		PixelFormat m_pixelFormat = PixelFormat::Unknown;
		uint32_t m_usageFlags = 0;
		bool m_ready = false;

		Failure Validate() const
		{
			if (m_transportType == TransportType::Unknown || m_syncMode == SyncMode::Unknown)
			{
				return Failure::FromDomain(ErrorDomain::Protocol, 1, "Transport descriptor requires valid transport and sync mode");
			}
			if (m_protocolVersion == 0 || m_width == 0 || m_height == 0 || m_pixelFormat == PixelFormat::Unknown)
			{
				return Failure::FromDomain(ErrorDomain::Protocol, 1, "Transport descriptor requires protocol version, extents, and format");
			}
			return Failure::Ok();
		}

		auto operator<=>(const TransportDescriptor&) const = default;
	};

	struct FramePacket
	{
		ViewportId m_viewportId = 0;
		ConnectionEpoch m_connectionEpoch = 0;
		SurfaceGeneration m_generation = 0;
		FrameIndex m_frameIndex = 0;
		uint32_t m_width = 0;
		uint32_t m_height = 0;
		uint32_t m_statusFlags = 0;
		uint64_t m_timestampNs = 0;

		Failure Validate() const
		{
			if (m_viewportId == 0 || m_connectionEpoch == 0 || m_generation == 0 || m_width == 0 || m_height == 0)
			{
				return Failure::FromDomain(ErrorDomain::Protocol, 1, "Frame packet requires valid ids and extents");
			}
			return Failure::Ok();
		}

		auto operator<=>(const FramePacket&) const = default;
	};

	struct InputPacket
	{
		ViewportId m_viewportId = 0;
		ConnectionEpoch m_connectionEpoch = 0;
		SurfaceGeneration m_generation = 0;
		InputKind m_kind = InputKind::Unknown;
		InputModifier m_modifiers = InputModifier::None;
		float m_pointerX = 0.0f;
		float m_pointerY = 0.0f;
		float m_wheelDeltaX = 0.0f;
		float m_wheelDeltaY = 0.0f;
		uint32_t m_keyCode = 0;
		bool m_pressed = false;
		bool m_focused = false;
		bool m_captured = false;
		uint64_t m_timestampNs = 0;

		Failure Validate() const
		{
			if (m_viewportId == 0 || m_connectionEpoch == 0 || m_generation == 0 || m_kind == InputKind::Unknown)
			{
				return Failure::FromDomain(ErrorDomain::Protocol, 1, "Input packet requires valid ids and kind");
			}
			return Failure::Ok();
		}

		auto operator<=>(const InputPacket&) const = default;
	};

	struct MessageEnvelope
	{
		MessageCategory m_category = MessageCategory::Unknown;
		CommandType m_commandType = CommandType::Unknown;
		EventType m_eventType = EventType::Unknown;
		std::variant<std::monostate, ViewportDescriptor, TransportDescriptor, FramePacket, InputPacket, Failure> m_payload;

		auto operator<=>(const MessageEnvelope&) const = default;
	};

	struct ProtocolMessage
	{
		MessageEnvelope m_envelope;

		static ProtocolMessage MakeFrameReady(const FramePacket& frame)
		{
			ProtocolMessage result{};
			result.m_envelope.m_category = MessageCategory::Event;
			result.m_envelope.m_eventType = EventType::FrameReady;
			result.m_envelope.m_payload = frame;
			return result;
		}

		MessageEnvelope ToEnvelope() const
		{
			return m_envelope;
		}

		static ProtocolMessage FromEnvelope(const MessageEnvelope& envelope)
		{
			ProtocolMessage result{};
			result.m_envelope = envelope;
			return result;
		}

		auto operator<=>(const ProtocolMessage&) const = default;
	};

	class SessionStateMachine
	{
	public:
		SessionState GetState() const { return m_state; }

		Failure TransitionTo(SessionState nextState)
		{
			if (m_state == SessionState::Disposed)
			{
				return Failure::FromDomain(ErrorDomain::Protocol, 1, "Disposed session cannot transition");
			}

			if (m_state == nextState)
			{
				return Failure::Ok();
			}

			if (!IsTransitionAllowed(m_state, nextState))
			{
				return Failure::FromDomain(ErrorDomain::Protocol, 1, "Invalid session transition");
			}

			m_state = nextState;
			return Failure::Ok();
		}

		Failure Destroy()
		{
			if (m_state == SessionState::Disposed)
			{
				return Failure::Ok();
			}

			m_state = SessionState::Terminating;
			m_state = SessionState::Disposed;
			return Failure::Ok();
		}

		static bool IsTransitionAllowed(SessionState current, SessionState nextState)
		{
			switch (current)
			{
			case SessionState::Created:
				return nextState == SessionState::Negotiating || nextState == SessionState::Lost || nextState == SessionState::Terminating;
			case SessionState::Negotiating:
				return nextState == SessionState::Ready || nextState == SessionState::Recovering || nextState == SessionState::Lost || nextState == SessionState::Terminating;
			case SessionState::Ready:
				return nextState == SessionState::Active || nextState == SessionState::Resizing || nextState == SessionState::Paused || nextState == SessionState::Lost || nextState == SessionState::Terminating;
			case SessionState::Active:
				return nextState == SessionState::Paused || nextState == SessionState::Resizing || nextState == SessionState::Recovering || nextState == SessionState::Lost || nextState == SessionState::Terminating;
			case SessionState::Paused:
				return nextState == SessionState::Active || nextState == SessionState::Resizing || nextState == SessionState::Recovering || nextState == SessionState::Lost || nextState == SessionState::Terminating;
			case SessionState::Resizing:
				return nextState == SessionState::Ready || nextState == SessionState::Active || nextState == SessionState::Recovering || nextState == SessionState::Lost || nextState == SessionState::Terminating;
			case SessionState::Recovering:
				return nextState == SessionState::Negotiating || nextState == SessionState::Lost || nextState == SessionState::Terminating;
			case SessionState::Lost:
				return nextState == SessionState::Negotiating || nextState == SessionState::Recovering || nextState == SessionState::Terminating;
			case SessionState::Terminating:
				return nextState == SessionState::Disposed;
			case SessionState::Disposed:
			default:
				return false;
			}
		}

	private:
		SessionState m_state = SessionState::Created;
	};

	enum class GuardDecision : uint8_t
	{
		Accept = 0,
		RejectNotReady,
		RejectStaleEpoch,
		RejectStaleGeneration,
		RejectInvalidPayload,
	};

	class SessionGuards
	{
	public:
		SessionGuards(ConnectionEpoch connectionEpoch = 1, SurfaceGeneration generation = 1) :
			m_connectionEpoch(connectionEpoch),
			m_generation(generation)
		{
		}

		ConnectionEpoch GetConnectionEpoch() const { return m_connectionEpoch; }
		SurfaceGeneration GetGeneration() const { return m_generation; }
		bool IsTransportReady() const { return m_transportReady; }

		void BeginNewConnectionEpoch(ConnectionEpoch connectionEpoch)
		{
			m_connectionEpoch = connectionEpoch;
			m_generation = 1;
			m_transportReady = false;
		}

		void AdvanceGeneration()
		{
			++m_generation;
			m_transportReady = false;
		}

		void ResetTransportReady()
		{
			m_transportReady = false;
		}

		GuardDecision AcknowledgeTransportReady(ConnectionEpoch connectionEpoch, SurfaceGeneration generation)
		{
			if (connectionEpoch != m_connectionEpoch)
			{
				return GuardDecision::RejectStaleEpoch;
			}
			if (generation != m_generation)
			{
				return GuardDecision::RejectStaleGeneration;
			}

			m_transportReady = true;
			return GuardDecision::Accept;
		}

		GuardDecision AcceptFrame(const FramePacket& frame) const
		{
			auto validation = frame.Validate();
			if (!validation.IsOk())
			{
				return GuardDecision::RejectInvalidPayload;
			}
			if (frame.m_connectionEpoch != m_connectionEpoch)
			{
				return GuardDecision::RejectStaleEpoch;
			}
			if (frame.m_generation != m_generation)
			{
				return GuardDecision::RejectStaleGeneration;
			}
			if (!m_transportReady)
			{
				return GuardDecision::RejectNotReady;
			}
			return GuardDecision::Accept;
		}

		GuardDecision AcceptInput(const InputPacket& input) const
		{
			auto validation = input.Validate();
			if (!validation.IsOk())
			{
				return GuardDecision::RejectInvalidPayload;
			}
			if (input.m_connectionEpoch != m_connectionEpoch)
			{
				return GuardDecision::RejectStaleEpoch;
			}
			if (input.m_generation != m_generation)
			{
				return GuardDecision::RejectStaleGeneration;
			}
			return GuardDecision::Accept;
		}

	private:
		ConnectionEpoch m_connectionEpoch = 1;
		SurfaceGeneration m_generation = 1;
		bool m_transportReady = false;
	};
}
