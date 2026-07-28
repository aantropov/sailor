#include "EditorEngineWebSocketServer.h"

#include "EditorEngineProtocolInternal.h"

#include <ixwebsocket/IXConnectionState.h>
#include <ixwebsocket/IXNetSystem.h>
#include <ixwebsocket/IXWebSocket.h>
#include <ixwebsocket/IXWebSocketHttpHeaders.h>
#include <ixwebsocket/IXWebSocketMessage.h>
#include <ixwebsocket/IXWebSocketServer.h>

#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

namespace
{
	using Sailor::Protocol::EEditorEngineTransportStatus;
	using Sailor::Protocol::EEditorEngineWebSocketHostStatus;
	using Sailor::Protocol::EditorEngineProtocolMaxPayloadSize;
	using Sailor::Protocol::EditorEngineWebSocketPath;
	using Sailor::Protocol::EditorEngineWebSocketSubprotocol;

	constexpr uint16_t c_closeUnauthorized = 4001u;
	constexpr uint16_t c_closeWrongEndpoint = 4004u;
	constexpr int c_listenBacklog = 5;
	constexpr int c_maxConnections = 16;

	class TEditorEngineConnectionState final : public ix::ConnectionState
	{
	public:
		bool m_bAuthorized = false;
	};

	bool ConstantTimeEquals(
		const std::string_view lhs,
		const std::string_view rhs)
	{
		if (lhs.size() != rhs.size())
		{
			return false;
		}

		uint8_t difference = 0;
		for (size_t i = 0; i < lhs.size(); ++i)
		{
			difference |= static_cast<uint8_t>(lhs[i] ^ rhs[i]);
		}
		return difference == 0;
	}

	bool HasExactHeader(
		const ix::WebSocketHttpHeaders& headers,
		const char* name,
		const std::string_view expectedValue)
	{
		const auto it = headers.find(name);
		return it != headers.end() &&
			ConstantTimeEquals(it->second, expectedValue);
	}

	bool IsValidAuthorizationToken(const std::string_view token)
	{
		for (const char value : token)
		{
			const bool bAlphaNumeric =
				(value >= 'a' && value <= 'z') ||
				(value >= 'A' && value <= 'Z') ||
				(value >= '0' && value <= '9');
			if (!bAlphaNumeric && value != '-' && value != '_')
			{
				return false;
			}
		}
		return true;
	}

	class TEditorEngineWebSocketServer final
	{
	public:
		TEditorEngineWebSocketServer(
			const uint16_t port,
			std::string authorizationToken)
			: m_authorizationHeader(
				"Bearer " + std::move(authorizationToken))
			, m_server(
				std::make_unique<ix::WebSocketServer>(
					static_cast<int>(port),
					"127.0.0.1",
					c_listenBacklog,
					c_maxConnections))
		{
			m_server->disablePerMessageDeflate();
			m_server->setConnectionStateFactory([]()
				{
					return std::make_shared<TEditorEngineConnectionState>();
				});
			m_server->setOnClientMessageCallback(
				[this](
					const std::shared_ptr<ix::ConnectionState>& connectionState,
					ix::WebSocket& webSocket,
					const ix::WebSocketMessagePtr& message)
				{
					HandleMessage(connectionState, webSocket, message);
				});
		}

		bool Start(std::string& outError)
		{
			const auto listenResult = m_server->listen();
			if (!listenResult.first)
			{
				outError = listenResult.second;
				return false;
			}

			m_server->start();
			return true;
		}

	private:
		void HandleMessage(
			const std::shared_ptr<ix::ConnectionState>& connectionState,
			ix::WebSocket& webSocket,
			const ix::WebSocketMessagePtr& message)
		{
			auto state = std::static_pointer_cast<
				TEditorEngineConnectionState>(connectionState);

			switch (message->type)
			{
			case ix::WebSocketMessageType::Open:
				HandleOpen(*state, webSocket, message->openInfo);
				return;

			case ix::WebSocketMessageType::Message:
				if (!state->m_bAuthorized)
				{
					webSocket.close(
						c_closeUnauthorized,
						"Editor protocol authorization is required.");
					return;
				}
				HandleRequest(webSocket, *message);
				return;

			default:
				return;
			}
		}

		void HandleOpen(
			TEditorEngineConnectionState& state,
			ix::WebSocket& webSocket,
			const ix::WebSocketOpenInfo& openInfo)
		{
			if (openInfo.uri != EditorEngineWebSocketPath)
			{
				webSocket.close(
					c_closeWrongEndpoint,
					"Unsupported editor protocol endpoint.");
				return;
			}

			const auto origin = openInfo.headers.find("Origin");
			if (origin != openInfo.headers.end() && !origin->second.empty())
			{
				webSocket.close(
					c_closeUnauthorized,
					"Browser origins are not accepted.");
				return;
			}

			if (!HasExactHeader(
					openInfo.headers,
					"Sec-WebSocket-Protocol",
					EditorEngineWebSocketSubprotocol) ||
				!HasExactHeader(
					openInfo.headers,
					"Authorization",
					m_authorizationHeader))
			{
				webSocket.close(
					c_closeUnauthorized,
					"Editor protocol authorization failed.");
				return;
			}

			state.m_bAuthorized = true;
		}

		void HandleRequest(
			ix::WebSocket& webSocket,
			const ix::WebSocketMessage& message)
		{
			if (!message.binary)
			{
				webSocket.close(
					1003u,
					"Editor protocol accepts binary messages only.");
				return;
			}
			if (message.str.empty())
			{
				webSocket.close(
					1007u,
					"Editor protocol request is empty.");
				return;
			}
			if (message.str.size() > EditorEngineProtocolMaxPayloadSize)
			{
				webSocket.close(
					1009u,
					"Editor protocol request exceeds the payload limit.");
				return;
			}

			uint8_t* responseData = nullptr;
			uint32_t responseSize = 0;
			int32_t status = static_cast<int32_t>(
				EEditorEngineTransportStatus::ExecutionFailed);
			try
			{
				Sailor::Protocol::EditorEngineProtocolDependencies
					dependencies{};
				dependencies.m_bAllowInitialize = false;
				status = Sailor::Protocol::InvokeEditorEngineProtocol(
					reinterpret_cast<const uint8_t*>(message.str.data()),
					static_cast<uint32_t>(message.str.size()),
					&responseData,
					&responseSize,
					dependencies);
			}
			catch (...)
			{
				status = static_cast<int32_t>(
					EEditorEngineTransportStatus::ExecutionFailed);
			}

			if (status != static_cast<int32_t>(
					EEditorEngineTransportStatus::Ok))
			{
				Sailor::Protocol::FreeEditorEngineProtocolBuffer(responseData);
				const auto transportStatus =
					static_cast<EEditorEngineTransportStatus>(status);
				if (transportStatus ==
					EEditorEngineTransportStatus::PayloadTooLarge)
				{
					webSocket.close(
						1009u,
						"Editor protocol payload exceeds the limit.");
				}
				else if (
					transportStatus ==
						EEditorEngineTransportStatus::ParseFailed ||
					transportStatus ==
						EEditorEngineTransportStatus::InvalidArguments)
				{
					webSocket.close(
						1007u,
						"Editor protocol request is malformed.");
				}
				else
				{
					webSocket.close(
						1011u,
						"Editor protocol request failed.");
				}
				return;
			}

			if (!responseData ||
				responseSize == 0 ||
				responseSize > EditorEngineProtocolMaxPayloadSize)
			{
				Sailor::Protocol::FreeEditorEngineProtocolBuffer(responseData);
				webSocket.close(
					responseSize > EditorEngineProtocolMaxPayloadSize
						? 1009u
						: 1011u,
					"Editor protocol produced an invalid response.");
				return;
			}

			const std::string response(
				reinterpret_cast<const char*>(responseData),
				responseSize);
			Sailor::Protocol::FreeEditorEngineProtocolBuffer(responseData);

			if (!webSocket.sendBinary(response).success)
			{
				webSocket.close(
					1011u,
					"Editor protocol response could not be sent.");
			}
		}

		std::string m_authorizationHeader{};
		std::unique_ptr<ix::WebSocketServer> m_server{};
	};

	struct TEditorEngineWebSocketServerState final
	{
		std::mutex m_mutex{};
		std::unique_ptr<TEditorEngineWebSocketServer> m_server{};
		bool m_bNetworkSystemInitialized = false;
	};

	TEditorEngineWebSocketServerState& GetServerState()
	{
		// Native host control can be called from managed termination cleanup.
		// Explicit Stop owns resource release; the mutex itself must outlive
		// C++ static destruction so a late noexcept export cannot lock a
		// destroyed synchronization primitive.
		static auto* const state =
			new TEditorEngineWebSocketServerState();
		return *state;
	}
}

int32_t Sailor::Protocol::StartEditorEngineWebSocketServer(
	const uint16_t port,
	const char* authorizationToken,
	const uint32_t authorizationTokenSize)
{
	if (port == 0 ||
		!authorizationToken ||
		authorizationTokenSize < 32u ||
		authorizationTokenSize > 256u)
	{
		return static_cast<int32_t>(
			EEditorEngineWebSocketHostStatus::InvalidArguments);
	}

	const std::string_view token(
		authorizationToken,
		authorizationTokenSize);
	if (!IsValidAuthorizationToken(token))
	{
		return static_cast<int32_t>(
			EEditorEngineWebSocketHostStatus::InvalidArguments);
	}

	auto& state = GetServerState();
	std::lock_guard lock(state.m_mutex);
	if (state.m_server)
	{
		return static_cast<int32_t>(
			EEditorEngineWebSocketHostStatus::AlreadyRunning);
	}

	try
	{
		if (!state.m_bNetworkSystemInitialized &&
			!ix::initNetSystem())
		{
			return static_cast<int32_t>(
				EEditorEngineWebSocketHostStatus::NetworkInitializationFailed);
		}
		state.m_bNetworkSystemInitialized = true;

		auto candidate = std::make_unique<TEditorEngineWebSocketServer>(
			port,
			std::string(token));
		std::string error;
		if (!candidate->Start(error))
		{
			candidate.reset();
			ix::uninitNetSystem();
			state.m_bNetworkSystemInitialized = false;
			return static_cast<int32_t>(
				EEditorEngineWebSocketHostStatus::ListenFailed);
		}

		state.m_server = std::move(candidate);
		return static_cast<int32_t>(
			EEditorEngineWebSocketHostStatus::Ok);
	}
	catch (...)
	{
		state.m_server.reset();
		if (state.m_bNetworkSystemInitialized)
		{
			ix::uninitNetSystem();
			state.m_bNetworkSystemInitialized = false;
		}
		return static_cast<int32_t>(
			EEditorEngineWebSocketHostStatus::ExecutionFailed);
	}
}

void Sailor::Protocol::StopEditorEngineWebSocketServer() noexcept
{
	try
	{
		auto& state = GetServerState();
		std::lock_guard lock(state.m_mutex);
		if (state.m_server)
		{
			state.m_server.reset();
		}
		if (state.m_bNetworkSystemInitialized)
		{
			ix::uninitNetSystem();
			state.m_bNetworkSystemInitialized = false;
		}
	}
	catch (...)
	{
	}
}
