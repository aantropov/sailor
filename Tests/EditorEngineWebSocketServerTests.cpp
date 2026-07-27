#include "EditorEngineProtocolInternal.h"
#include "EditorEngineWebSocketServer.h"

#include <ixwebsocket/IXGetFreePort.h>
#include <ixwebsocket/IXNetSystem.h>
#include <ixwebsocket/IXWebSocket.h>
#include <ixwebsocket/IXWebSocketHttpHeaders.h>
#include <ixwebsocket/IXWebSocketMessage.h>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>

#if !defined(_WIN32)
#include <csignal>
#endif

namespace
{
	using Sailor::Protocol::EEditorEngineWebSocketHostStatus;
	using Sailor::Protocol::EditorEngineProtocolVersion;
	using Sailor::Protocol::EditorEngineWebSocketPath;
	using Sailor::Protocol::EditorEngineWebSocketSubprotocol;

	constexpr std::chrono::seconds c_eventTimeout = std::chrono::seconds(5);
	constexpr uint16_t c_closeUnauthorized = 4001u;
	constexpr uint16_t c_closeWrongEndpoint = 4004u;
	constexpr uint16_t c_closeUnsupportedData = 1003u;
	constexpr uint16_t c_closeInvalidPayload = 1007u;

	void AppendVarint(std::string& payload, uint64_t value)
	{
		while (value >= 0x80u)
		{
			payload.push_back(static_cast<char>(
				(value & 0x7fu) | 0x80u));
			value >>= 7u;
		}
		payload.push_back(static_cast<char>(value));
	}

	void AppendKey(
		std::string& payload,
		const uint32_t fieldNumber,
		const uint8_t wireType)
	{
		AppendVarint(
			payload,
			(static_cast<uint64_t>(fieldNumber) << 3u) | wireType);
	}

	void AppendVarintField(
		std::string& payload,
		const uint32_t fieldNumber,
		const uint64_t value)
	{
		AppendKey(payload, fieldNumber, 0u);
		AppendVarint(payload, value);
	}

	void AppendBytesField(
		std::string& payload,
		const uint32_t fieldNumber,
		const std::string& value)
	{
		AppendKey(payload, fieldNumber, 2u);
		AppendVarint(payload, value.size());
		payload.append(value);
	}

	std::string MakeRequest(
		const uint64_t requestId,
		const uint32_t commandField,
		const std::string& commandPayload = {})
	{
		std::string payload;
		AppendVarintField(
			payload,
			1u,
			EditorEngineProtocolVersion);
		AppendVarintField(payload, 2u, requestId);
		AppendBytesField(payload, commandField, commandPayload);
		return payload;
	}

	bool ReadVarint(
		const std::string& payload,
		size_t& offset,
		uint64_t& outValue)
	{
		outValue = 0u;
		for (uint32_t shift = 0u;
			shift < 64u && offset < payload.size();
			shift += 7u)
		{
			const uint8_t byte = static_cast<uint8_t>(
				payload[offset++]);
			outValue |= static_cast<uint64_t>(byte & 0x7fu) << shift;
			if ((byte & 0x80u) == 0u)
			{
				return true;
			}
		}
		return false;
	}

	bool ReadBytes(
		const std::string& payload,
		size_t& offset,
		std::string& outValue)
	{
		uint64_t length = 0u;
		if (!ReadVarint(payload, offset, length) ||
			length > payload.size() - offset)
		{
			return false;
		}

		outValue.assign(
			payload.data() + offset,
			static_cast<size_t>(length));
		offset += static_cast<size_t>(length);
		return true;
	}

	struct TProtocolResponseWire
	{
		uint64_t m_protocolVersion = 0u;
		uint64_t m_requestId = 0u;
		bool m_bSuccess = false;
		std::string m_error{};
		uint32_t m_resultField = 0u;
		std::string m_resultPayload{};
	};

	bool ParseResponse(
		const std::string& payload,
		TProtocolResponseWire& outResponse)
	{
		size_t offset = 0u;
		while (offset < payload.size())
		{
			uint64_t key = 0u;
			if (!ReadVarint(payload, offset, key))
			{
				return false;
			}

			const uint32_t fieldNumber =
				static_cast<uint32_t>(key >> 3u);
			const uint8_t wireType =
				static_cast<uint8_t>(key & 0x07u);
			if (wireType == 0u)
			{
				uint64_t value = 0u;
				if (!ReadVarint(payload, offset, value))
				{
					return false;
				}
				switch (fieldNumber)
				{
				case 1u:
					outResponse.m_protocolVersion = value;
					break;

				case 2u:
					outResponse.m_requestId = value;
					break;

				case 3u:
					outResponse.m_bSuccess = value != 0u;
					break;

				default:
					break;
				}
				continue;
			}
			if (wireType == 2u)
			{
				std::string value;
				if (!ReadBytes(payload, offset, value))
				{
					return false;
				}
				if (fieldNumber == 4u)
				{
					outResponse.m_error = std::move(value);
				}
				else if (fieldNumber >= 10u &&
					fieldNumber <= 19u)
				{
					outResponse.m_resultField = fieldNumber;
					outResponse.m_resultPayload = std::move(value);
				}
				continue;
			}

			// The protocol envelopes currently use only varint and
			// length-delimited fields. Rejecting other wire types keeps this
			// test decoder intentionally small and strict.
			return false;
		}
		return true;
	}

	bool ReadNestedScalar(
		const std::string& payload,
		uint64_t& outValue)
	{
		outValue = 0u;
		if (payload.empty())
		{
			return true;
		}

		size_t offset = 0u;
		uint64_t key = 0u;
		return ReadVarint(payload, offset, key) &&
			key == 8u &&
			ReadVarint(payload, offset, outValue) &&
			offset == payload.size();
	}

	void Require(const bool condition, const std::string& message)
	{
		if (!condition)
		{
			throw std::runtime_error(message);
		}
	}

	void RequireHostStatus(
		const int32_t actualStatus,
		const EEditorEngineWebSocketHostStatus expectedStatus,
		const std::string& context)
	{
		Require(
			actualStatus == static_cast<int32_t>(expectedStatus),
			context + ", status=" + std::to_string(actualStatus));
	}

	class TServerGuard final
	{
	public:
		explicit TServerGuard(const std::string& authorizationToken)
		{
			Require(
				ix::initNetSystem(),
				"IXWebSocket network system must initialize for port reservation");
			const int port = ix::getFreePort();
			Require(
				ix::uninitNetSystem(),
				"IXWebSocket network system must reset before testing host startup");
			Require(
				port > 0 && port <= 65535,
				"IXWebSocket must provide a valid free TCP port");
			m_port = static_cast<uint16_t>(port);

			const int32_t status =
				Sailor::Protocol::StartEditorEngineWebSocketServer(
					m_port,
					authorizationToken.data(),
					static_cast<uint32_t>(authorizationToken.size()));
			Require(
				status == static_cast<int32_t>(
					EEditorEngineWebSocketHostStatus::Ok),
				"editor-engine WebSocket server must start, status=" +
					std::to_string(status));
			m_bStarted = true;
		}

		~TServerGuard()
		{
			if (m_bStarted)
			{
				Sailor::Protocol::StopEditorEngineWebSocketServer();
			}
		}

		TServerGuard(const TServerGuard&) = delete;
		TServerGuard& operator=(const TServerGuard&) = delete;

		uint16_t GetPort() const
		{
			return m_port;
		}

	private:
		uint16_t m_port = 0;
		bool m_bStarted = false;
	};

	struct TReceivedMessage
	{
		std::string m_payload{};
		bool m_bBinary = false;
	};

	struct TCloseEvent
	{
		uint16_t m_code = 0;
		std::string m_reason{};
	};

	struct TWebSocketClientOptions
	{
		std::string m_path = EditorEngineWebSocketPath;
		std::string m_authorizationToken{};
		std::string m_origin{};
		std::string m_subprotocol = EditorEngineWebSocketSubprotocol;
		bool m_bIncludeAuthorization = true;
		bool m_bIncludeSubprotocol = true;
	};

	TWebSocketClientOptions MakeAuthorizedClientOptions(
		const std::string& authorizationToken)
	{
		TWebSocketClientOptions options;
		options.m_authorizationToken = authorizationToken;
		return options;
	}

	class TWebSocketClient final
	{
	public:
		TWebSocketClient(
			const uint16_t port,
			const TWebSocketClientOptions& options)
		{
			m_webSocket.setUrl(
				"ws://127.0.0.1:" + std::to_string(port) +
					options.m_path);
			if (options.m_bIncludeSubprotocol)
			{
				m_webSocket.addSubProtocol(options.m_subprotocol);
			}
			m_webSocket.disablePerMessageDeflate();
			m_webSocket.disableAutomaticReconnection();
			m_webSocket.setHandshakeTimeout(
				static_cast<int>(c_eventTimeout.count()));

			ix::WebSocketHttpHeaders headers;
			if (options.m_bIncludeAuthorization)
			{
				headers["Authorization"] =
					"Bearer " + options.m_authorizationToken;
			}
			headers["Origin"] = options.m_origin;
			m_webSocket.setExtraHeaders(headers);
			m_webSocket.setOnMessageCallback(
				[this](const ix::WebSocketMessagePtr& message)
				{
					HandleMessage(message);
				});
		}

		~TWebSocketClient()
		{
			m_webSocket.stop();
		}

		TWebSocketClient(const TWebSocketClient&) = delete;
		TWebSocketClient& operator=(const TWebSocketClient&) = delete;

		void Start()
		{
			m_webSocket.start();
		}

		void WaitForOpen()
		{
			std::unique_lock lock(m_mutex);
			Require(
				m_condition.wait_for(
					lock,
					c_eventTimeout,
					[this]()
					{
						return m_bOpened || m_bClosed || m_bFailed;
					}),
				"timed out waiting for WebSocket connection");
			Require(
				m_bOpened && !m_bClosed && !m_bFailed,
				"WebSocket connection did not stay open: " +
					GetFailureDescription());
		}

		TReceivedMessage WaitForBinaryMessage()
		{
			std::unique_lock lock(m_mutex);
			Require(
				m_condition.wait_for(
					lock,
					c_eventTimeout,
					[this]()
					{
						return m_bHasMessage || m_bClosed || m_bFailed;
					}),
				"timed out waiting for binary WebSocket response");
			Require(
				m_bHasMessage,
				"WebSocket closed before receiving a response: " +
					GetFailureDescription());
			Require(
				m_message.m_bBinary,
				"editor-engine WebSocket response must be binary");
			return m_message;
		}

		TCloseEvent WaitForClose()
		{
			std::unique_lock lock(m_mutex);
			Require(
				m_condition.wait_for(
					lock,
					c_eventTimeout,
					[this]()
					{
						return m_bClosed || m_bFailed;
					}),
				"timed out waiting for WebSocket close event");
			Require(
				m_bClosed,
				"WebSocket failed before receiving a close frame: " +
					GetFailureDescription());
			return m_closeEvent;
		}

		void SendBinary(const std::string& payload)
		{
			Require(
				m_webSocket.sendBinary(payload).success,
				"binary WebSocket request must be queued");
		}

		void SendText(const std::string& payload)
		{
			Require(
				m_webSocket.sendText(payload).success,
				"text WebSocket request must be queued");
		}

	private:
		void HandleMessage(const ix::WebSocketMessagePtr& message)
		{
			std::lock_guard lock(m_mutex);
			switch (message->type)
			{
			case ix::WebSocketMessageType::Open:
				m_bOpened = true;
				break;

			case ix::WebSocketMessageType::Message:
				m_message.m_payload = message->str;
				m_message.m_bBinary = message->binary;
				m_bHasMessage = true;
				break;

			case ix::WebSocketMessageType::Close:
				m_closeEvent.m_code = message->closeInfo.code;
				m_closeEvent.m_reason = message->closeInfo.reason;
				m_bClosed = true;
				break;

			case ix::WebSocketMessageType::Error:
				m_error = message->errorInfo.reason;
				m_bFailed = true;
				break;

			default:
				return;
			}
			m_condition.notify_all();
		}

		std::string GetFailureDescription() const
		{
			if (m_bFailed)
			{
				return "error=\"" + m_error + "\"";
			}
			if (m_bClosed)
			{
				return "close=" + std::to_string(m_closeEvent.m_code) +
					" reason=\"" + m_closeEvent.m_reason + "\"";
			}
			return "no terminal event";
		}

		ix::WebSocket m_webSocket{};
		mutable std::mutex m_mutex{};
		std::condition_variable m_condition{};
		TReceivedMessage m_message{};
		TCloseEvent m_closeEvent{};
		std::string m_error{};
		bool m_bOpened = false;
		bool m_bHasMessage = false;
		bool m_bClosed = false;
		bool m_bFailed = false;
	};

	void RequireCloseCode(
		const TCloseEvent& closeEvent,
		const uint16_t expectedCode,
		const std::string& context)
	{
		Require(
			closeEvent.m_code == expectedCode,
			context + ", actual=" + std::to_string(closeEvent.m_code) +
				" reason=\"" + closeEvent.m_reason + "\"");
	}

	void TestInvalidServerArguments(
		const std::string& validAuthorizationToken)
	{
		constexpr uint16_t unusedPort = 31337u;
		const std::string tooShortToken(31u, 'a');
		const std::string tooLongToken(257u, 'a');
		std::string invalidCharacterToken = validAuthorizationToken;
		invalidCharacterToken[0] = '!';

		RequireHostStatus(
			Sailor::Protocol::StartEditorEngineWebSocketServer(
				0u,
				validAuthorizationToken.data(),
				static_cast<uint32_t>(validAuthorizationToken.size())),
			EEditorEngineWebSocketHostStatus::InvalidArguments,
			"zero port must be rejected");
		RequireHostStatus(
			Sailor::Protocol::StartEditorEngineWebSocketServer(
				unusedPort,
				nullptr,
				static_cast<uint32_t>(validAuthorizationToken.size())),
			EEditorEngineWebSocketHostStatus::InvalidArguments,
			"null authorization token must be rejected");
		RequireHostStatus(
			Sailor::Protocol::StartEditorEngineWebSocketServer(
				unusedPort,
				tooShortToken.data(),
				static_cast<uint32_t>(tooShortToken.size())),
			EEditorEngineWebSocketHostStatus::InvalidArguments,
			"authorization token shorter than 32 bytes must be rejected");
		RequireHostStatus(
			Sailor::Protocol::StartEditorEngineWebSocketServer(
				unusedPort,
				tooLongToken.data(),
				static_cast<uint32_t>(tooLongToken.size())),
			EEditorEngineWebSocketHostStatus::InvalidArguments,
			"authorization token longer than 256 bytes must be rejected");
		RequireHostStatus(
			Sailor::Protocol::StartEditorEngineWebSocketServer(
				unusedPort,
				invalidCharacterToken.data(),
				static_cast<uint32_t>(invalidCharacterToken.size())),
			EEditorEngineWebSocketHostStatus::InvalidArguments,
			"authorization token with unsupported characters must be rejected");
	}

	void TestAlreadyRunningIsReported(
		const uint16_t port,
		const std::string& authorizationToken)
	{
		RequireHostStatus(
			Sailor::Protocol::StartEditorEngineWebSocketServer(
				port,
				authorizationToken.data(),
				static_cast<uint32_t>(authorizationToken.size())),
			EEditorEngineWebSocketHostStatus::AlreadyRunning,
			"starting a second editor-engine WebSocket server must fail");
	}

	void TestValidBinaryProtobufRoundTrip(
		const uint16_t port,
		const std::string& authorizationToken)
	{
		TWebSocketClient client(
			port,
			MakeAuthorizedClientOptions(authorizationToken));
		client.Start();
		client.WaitForOpen();

		constexpr uint64_t requestId = 42u;
		client.SendBinary(MakeRequest(
			requestId,
			16u));

		const TReceivedMessage message = client.WaitForBinaryMessage();
		TProtocolResponseWire response;
		Require(
			ParseResponse(message.m_payload, response),
			"WebSocket response must contain a valid protobuf envelope");
		Require(
			response.m_protocolVersion == EditorEngineProtocolVersion,
			"WebSocket response must preserve the protocol version");
		Require(
			response.m_requestId == requestId,
			"WebSocket response must preserve the request id");
		Require(
			response.m_bSuccess && response.m_error.empty(),
			"get-exit-code request must succeed");
		uint64_t exitCode = 1u;
		Require(
			response.m_resultField == 12u &&
				ReadNestedScalar(
					response.m_resultPayload,
					exitCode) &&
				exitCode == 0u,
				"get-exit-code response must contain the default exit code");
	}

	void TestReadinessBooleanRoundTrip(
		const uint16_t port,
		const std::string& authorizationToken)
	{
		TWebSocketClient client(
			port,
			MakeAuthorizedClientOptions(authorizationToken));
		client.Start();
		client.WaitForOpen();

		constexpr uint64_t requestId = 47u;
		client.SendBinary(MakeRequest(
			requestId,
			47u));

		const TReceivedMessage message = client.WaitForBinaryMessage();
		TProtocolResponseWire response;
		Require(
			ParseResponse(message.m_payload, response),
			"readiness WebSocket response must contain a protobuf envelope");
		Require(
			response.m_protocolVersion == EditorEngineProtocolVersion &&
				response.m_requestId == requestId,
			"readiness response must preserve its envelope");
		Require(
			response.m_bSuccess && response.m_error.empty(),
			"readiness request must succeed");
		uint64_t isReady = 1u;
		Require(
			response.m_resultField == 11u &&
				ReadNestedScalar(
					response.m_resultPayload,
					isReady) &&
				isReady == 0u,
			"uninitialized test Engine must report main thread not ready");
	}

	void TestEngineMutationIsRejectedBeforeInitialization(
		const uint16_t port,
		const std::string& authorizationToken)
	{
		TWebSocketClient client(
			port,
			MakeAuthorizedClientOptions(authorizationToken));
		client.Start();
		client.WaitForOpen();

		constexpr uint64_t requestId = 49u;
		client.SendBinary(MakeRequest(
			requestId,
			22u));

		const TReceivedMessage message = client.WaitForBinaryMessage();
		TProtocolResponseWire response;
		Require(
			ParseResponse(message.m_payload, response),
			"pre-initialization mutation must return a protocol response");
		Require(
			response.m_protocolVersion == EditorEngineProtocolVersion &&
				response.m_requestId == requestId &&
				!response.m_bSuccess &&
				response.m_error.find("initialization") != std::string::npos,
			"engine mutations must be rejected until local bootstrap initializes the Engine");
	}

	void TestInitializeIsRejectedAfterWebSocketAdmission(
		const uint16_t port,
		const std::string& authorizationToken)
	{
		TWebSocketClient client(
			port,
			MakeAuthorizedClientOptions(authorizationToken));
		client.Start();
		client.WaitForOpen();

		constexpr uint64_t requestId = 48u;
		std::string initializePayload;
		AppendBytesField(
			initializePayload,
			1u,
			"SailorEditor");
		client.SendBinary(MakeRequest(
			requestId,
			10u,
			initializePayload));

		const TReceivedMessage message = client.WaitForBinaryMessage();
		TProtocolResponseWire response;
		Require(
			ParseResponse(message.m_payload, response),
			"Initialize rejection must return a protocol response");
		Require(
			response.m_protocolVersion == EditorEngineProtocolVersion &&
				response.m_requestId == requestId &&
				!response.m_bSuccess &&
				response.m_error.find("bootstrap") != std::string::npos,
			"WebSocket Initialize must be rejected without invoking App initialization");
	}

	void TestWrongTokenIsRejected(
		const uint16_t port,
		const std::string& wrongAuthorizationToken)
	{
		TWebSocketClient client(
			port,
			MakeAuthorizedClientOptions(wrongAuthorizationToken));
		client.Start();

		RequireCloseCode(
			client.WaitForClose(),
			c_closeUnauthorized,
			"wrong bearer token must close with code 4001");
	}

	void TestMissingAuthorizationIsRejected(
		const uint16_t port,
		const std::string& authorizationToken)
	{
		auto options = MakeAuthorizedClientOptions(authorizationToken);
		options.m_bIncludeAuthorization = false;
		TWebSocketClient client(port, options);
		client.Start();

		RequireCloseCode(
			client.WaitForClose(),
			c_closeUnauthorized,
			"missing authorization header must close with code 4001");
	}

	void TestWrongPathIsRejected(
		const uint16_t port,
		const std::string& authorizationToken)
	{
		auto options = MakeAuthorizedClientOptions(authorizationToken);
		options.m_path = "/wrong/editor/path";
		TWebSocketClient client(port, options);
		client.Start();

		RequireCloseCode(
			client.WaitForClose(),
			c_closeWrongEndpoint,
			"wrong endpoint path must close with code 4004");
	}

	void TestNonEmptyOriginIsRejected(
		const uint16_t port,
		const std::string& authorizationToken)
	{
		auto options = MakeAuthorizedClientOptions(authorizationToken);
		options.m_origin = "https://example.test";
		TWebSocketClient client(port, options);
		client.Start();

		RequireCloseCode(
			client.WaitForClose(),
			c_closeUnauthorized,
			"non-empty Origin must close with code 4001");
	}

	void TestMissingSubprotocolIsRejected(
		const uint16_t port,
		const std::string& authorizationToken)
	{
		auto options = MakeAuthorizedClientOptions(authorizationToken);
		options.m_bIncludeSubprotocol = false;
		TWebSocketClient client(port, options);
		client.Start();

		RequireCloseCode(
			client.WaitForClose(),
			c_closeUnauthorized,
			"missing WebSocket subprotocol must close with code 4001");
	}

	void TestWrongSubprotocolIsRejected(
		const uint16_t port,
		const std::string& authorizationToken)
	{
		auto options = MakeAuthorizedClientOptions(authorizationToken);
		options.m_subprotocol = "sailor.editor.wrong";
		TWebSocketClient client(port, options);
		client.Start();

		RequireCloseCode(
			client.WaitForClose(),
			c_closeUnauthorized,
			"wrong WebSocket subprotocol must close with code 4001");
	}

	void TestTextFrameIsRejected(
		const uint16_t port,
		const std::string& authorizationToken)
	{
		TWebSocketClient client(
			port,
			MakeAuthorizedClientOptions(authorizationToken));
		client.Start();
		client.WaitForOpen();
		client.SendText("not-a-binary-protobuf-request");

		RequireCloseCode(
			client.WaitForClose(),
			c_closeUnsupportedData,
			"text request must close with code 1003");
	}

	void TestEmptyBinaryFrameIsRejected(
		const uint16_t port,
		const std::string& authorizationToken)
	{
		TWebSocketClient client(
			port,
			MakeAuthorizedClientOptions(authorizationToken));
		client.Start();
		client.WaitForOpen();
		client.SendBinary({});

		RequireCloseCode(
			client.WaitForClose(),
			c_closeInvalidPayload,
			"empty binary request must close with code 1007");
	}

	void TestMalformedBinaryFrameIsRejected(
		const uint16_t port,
		const std::string& authorizationToken)
	{
		TWebSocketClient client(
			port,
			MakeAuthorizedClientOptions(authorizationToken));
		client.Start();
		client.WaitForOpen();
		client.SendBinary(std::string(1, static_cast<char>(0x80)));

		RequireCloseCode(
			client.WaitForClose(),
			c_closeInvalidPayload,
			"malformed protobuf request must close with code 1007");
	}
}

int main()
{
	try
	{
#if !defined(_WIN32)
		std::signal(SIGPIPE, SIG_IGN);
#endif
			const std::string authorizationToken =
				"0123456789abcdef0123456789abcdef";
			TestInvalidServerArguments(authorizationToken);
			{
				const TServerGuard server(authorizationToken);

				TestAlreadyRunningIsReported(
					server.GetPort(),
					authorizationToken);
				TestValidBinaryProtobufRoundTrip(
					server.GetPort(),
					authorizationToken);
				TestReadinessBooleanRoundTrip(
					server.GetPort(),
					authorizationToken);
				TestEngineMutationIsRejectedBeforeInitialization(
					server.GetPort(),
					authorizationToken);
				TestInitializeIsRejectedAfterWebSocketAdmission(
					server.GetPort(),
					authorizationToken);
				TestWrongTokenIsRejected(
					server.GetPort(),
					"fedcba9876543210fedcba9876543210");
				TestMissingAuthorizationIsRejected(
					server.GetPort(),
					authorizationToken);
				TestWrongPathIsRejected(
					server.GetPort(),
					authorizationToken);
				TestNonEmptyOriginIsRejected(
					server.GetPort(),
					authorizationToken);
				TestMissingSubprotocolIsRejected(
					server.GetPort(),
					authorizationToken);
				TestWrongSubprotocolIsRejected(
					server.GetPort(),
					authorizationToken);
				TestTextFrameIsRejected(
					server.GetPort(),
					authorizationToken);
				TestEmptyBinaryFrameIsRejected(
					server.GetPort(),
					authorizationToken);
				TestMalformedBinaryFrameIsRejected(
					server.GetPort(),
					authorizationToken);
		}

		// In-process editor sessions can change workspaces repeatedly. Verify
		// that stopping the listener also releases its network-system lifetime.
		{
			const TServerGuard restartedServer(authorizationToken);
			TestValidBinaryProtobufRoundTrip(
				restartedServer.GetPort(),
				authorizationToken);
		}
	}
	catch (const std::exception& exception)
	{
		std::cerr
			<< "[FAIL] EditorEngineWebSocketServerTests: "
			<< exception.what()
			<< std::endl;
		return 1;
	}

	std::cout << "[PASS] EditorEngineWebSocketServerTests" << std::endl;
	return 0;
}
