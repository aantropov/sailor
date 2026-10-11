#include "EditorEngineProtocolInternal.h"
#include "EditorEngineProtocolLifecycle.h"
#include "EditorEngineWebSocketServer.h"
#include "Sailor.h"
#include "Support/TempDirectory.h"
#include "Support/EditorProtocolWire.h"
#include "Support/ScopeExit.h"

#include <ixwebsocket/IXGetFreePort.h>
#include <ixwebsocket/IXNetSystem.h>
#include <ixwebsocket/IXWebSocket.h>
#include <ixwebsocket/IXWebSocketHttpHeaders.h>
#include <ixwebsocket/IXWebSocketMessage.h>

#include <chrono>
#include <atomic>
#include <future>
#include <thread>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#if !defined(_WIN32)
#include <csignal>
#endif

extern "C"
{
	int32_t SailorProtocolStartLocalHost(const uint8_t* requestData, uint32_t requestSize,
		uint16_t port, const char* token, uint32_t tokenSize) noexcept;
	int32_t SailorProtocolStopLocalHost(bool bShutdownEngine) noexcept;
}

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

	using namespace Sailor::Tests::ProtocolWire;

	void Require(const bool condition, std::string_view message)
	{
		if (!condition)
		{
			throw std::runtime_error(std::string(message));
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

	void StopLocalHostAtProcessExit() noexcept
	{
		SailorProtocolStopLocalHost(false);
	}

	void TestValidBinaryProtobufRoundTrip(
		uint16_t port,
		const std::string& authorizationToken);

	void TestLateStopAfterStaticTeardown()
	{
		// Register before the protocol lifecycle gate and WebSocket server
		// state are first touched. atexit callbacks and function-static
		// destructors run in reverse registration order, so destructible
		// process state would be gone before this late host-stop callback.
		Require(
			std::atexit(StopLocalHostAtProcessExit) == 0,
			"late local-host stop callback must register");

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

		const std::string authorizationToken =
			"0123456789abcdef0123456789abcdef";
		RequireHostStatus(
			Sailor::Protocol::StartEditorEngineWebSocketServer(
				static_cast<uint16_t>(port),
				authorizationToken.data(),
				static_cast<uint32_t>(authorizationToken.size())),
			EEditorEngineWebSocketHostStatus::Ok,
			"editor-engine WebSocket server must start for late stop");

		TestValidBinaryProtobufRoundTrip(
			static_cast<uint16_t>(port),
			authorizationToken);

		// Deliberately leave the server running. StopLocalHostAtProcessExit
		// performs the only teardown after later-registered static finalizers.
	}

	void TestFailedLocalHostInitialization(const std::string& authorizationToken)
	{
		Sailor::Tests::TempDirectory workspace("local-host-initialization");
		Require(ix::initNetSystem(), "port reservation must initialize the network system");
		const int port = ix::getFreePort();
		Require(ix::uninitNetSystem() && port > 0 && port <= 65535, "port reservation must complete");
		std::string arguments;
		for (const auto& argument : { std::string("SailorEngine"), std::string("--workspace"),
			workspace.Path("missing").string(), std::string("--noconsole"), std::string("--new-world") })
		{
			AppendBytesField(arguments, 1u, argument);
		}
		const auto request = MakeRequest(1u, 10u, arguments);
		for (uint32_t attempt = 0; attempt < 3u; ++attempt)
		{
			const int32_t status = SailorProtocolStartLocalHost(
				reinterpret_cast<const uint8_t*>(request.data()), static_cast<uint32_t>(request.size()),
				static_cast<uint16_t>(port), authorizationToken.data(), static_cast<uint32_t>(authorizationToken.size()));
			const bool bRolledBack = Sailor::App::GetInstance() == nullptr;
			if (!bRolledBack) SailorProtocolStopLocalHost(true);
			RequireHostStatus(status, EEditorEngineWebSocketHostStatus::InitializationFailed,
				"local host must report the actual initialization failure on every attempt");
			Require(bRolledBack, "failed local host bootstrap must release its partial App");
			RequireHostStatus(Sailor::Protocol::StartEditorEngineWebSocketServer(static_cast<uint16_t>(port),
				authorizationToken.data(), static_cast<uint32_t>(authorizationToken.size())),
				EEditorEngineWebSocketHostStatus::Ok, "failed bootstrap must release its listening socket");
			Sailor::Protocol::StopEditorEngineWebSocketServer();
		}
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

	uint16_t ReserveLocalHostPort()
	{
		Require(ix::initNetSystem(), "port reservation must initialize the network system");
		const int port = ix::getFreePort();
		Require(ix::uninitNetSystem() && port > 0 && port <= 65535, "port reservation must complete");
		return static_cast<uint16_t>(port);
	}

	void TestBootstrapReservesBeforePublishingServer(const std::string& token)
	{
		using namespace std::chrono_literals;
		using Status = EEditorEngineWebSocketHostStatus;
		Sailor::Protocol::TEditorEngineProtocolLifecycleGate gate;
		std::atomic<uint32_t> initializeCalls{0u};
		Sailor::Protocol::EditorEngineProtocolDependencies dependencies{};
		dependencies.m_context = &initializeCalls;
		dependencies.m_lifecycleGate = &gate;
		dependencies.m_initialize = [](void* context, const char**, int32_t)
			{
				++*static_cast<std::atomic<uint32_t>*>(context);
				return Sailor::EAppInitializationResult::Ready;
			};
		dependencies.m_stop = [](void*) {};
		dependencies.m_shutdown = [](void*) { return true; };
		const auto request = MakeRequest(1u, 10u);
		const uint16_t port = ReserveLocalHostPort();
		std::string error;
		Require(gate.TryAcquireOperation(error, true), "bootstrap fixture must hold an earlier diagnostic operation");
		std::future<Status> bootstrap;
		Sailor::Tests::ScopeExit releaseOperation([&]() { gate.ReleaseOperation(); });
		bool bStopped = false;
		Sailor::Tests::ScopeExit cleanup([&]()
			{
				releaseOperation.Run();
				if (bootstrap.valid())
				{
					bootstrap.wait();
				}
				bStopped = Sailor::Protocol::StopEditorEngineLocalHost(true, dependencies);
			});
		bootstrap = std::async(std::launch::async, [&]()
			{
				return Sailor::Protocol::StartEditorEngineLocalHost(
					reinterpret_cast<const uint8_t*>(request.data()), static_cast<uint32_t>(request.size()),
					port, token.data(), static_cast<uint32_t>(token.size()), dependencies);
			});
		bool bReserved = false;
		const auto deadline = std::chrono::steady_clock::now() + 1s;
		while (std::chrono::steady_clock::now() < deadline)
		{
			if (!gate.TryAcquireOperation(error, true))
			{
				bReserved = true;
				break;
			}
			gate.ReleaseOperation();
			std::this_thread::yield();
		}
		Require(bReserved && initializeCalls == 0u, "bootstrap must reserve initialization before entering App");
		const auto probeStatus = static_cast<Status>(Sailor::Protocol::StartEditorEngineWebSocketServer(
			port, token.data(), static_cast<uint32_t>(token.size())));
		if (probeStatus == Status::Ok)
		{
			Sailor::Protocol::StopEditorEngineWebSocketServer();
		}
		releaseOperation.Run();
		const auto status = bootstrap.get();
		cleanup.Run();
		Require(probeStatus == Status::Ok && status == Status::Ok && initializeCalls == 1u && bStopped,
			"the host must not publish its server before initialization admission has drained earlier work");
	}

	void TestBootstrapRollbackAndRetry(const std::string& token)
	{
		using Status = EEditorEngineWebSocketHostStatus;
		struct TBootstrapSource
		{
			Sailor::Protocol::TEditorEngineProtocolLifecycleGate m_gate;
			Sailor::EAppInitializationResult m_result = Sailor::EAppInitializationResult::Failed;
			uint32_t m_numInitializations = 0u;
			uint32_t m_numShutdowns = 0u;
			bool m_bRollbackExclusive = true;
		} source;
		Sailor::Protocol::EditorEngineProtocolDependencies dependencies{};
		dependencies.m_context = &source;
		dependencies.m_lifecycleGate = &source.m_gate;
		dependencies.m_initialize = [](void* context, const char**, int32_t)
			{
				auto& state = *static_cast<TBootstrapSource*>(context);
				++state.m_numInitializations;
				return state.m_result;
			};
		dependencies.m_stop = [](void*) {};
		dependencies.m_shutdown = [](void* context)
			{
				auto& state = *static_cast<TBootstrapSource*>(context);
				std::string error;
				state.m_bRollbackExclusive &= !state.m_gate.TryBeginInitialization(error);
				return ++state.m_numShutdowns > 1u;
			};
		const auto request = MakeRequest(1u, 10u);
		const uint16_t port = ReserveLocalHostPort();
		auto start = [&]()
			{
				return Sailor::Protocol::StartEditorEngineLocalHost(
					reinterpret_cast<const uint8_t*>(request.data()), static_cast<uint32_t>(request.size()),
					port, token.data(), static_cast<uint32_t>(token.size()), dependencies);
			};
		Sailor::Tests::ScopeExit cleanup([&]() { Sailor::Protocol::StopEditorEngineLocalHost(true, dependencies); });
		Require(start() == Status::ShutdownFailed && source.m_numInitializations == 1u && source.m_numShutdowns == 1u,
			"a refused bootstrap rollback must retain the failed session");
		Require(start() == Status::AlreadyRunning && source.m_numInitializations == 1u,
			"a failed rollback must not admit another bootstrap");
		Require(Sailor::Protocol::StopEditorEngineLocalHost(true, dependencies), "bootstrap cleanup must allow an explicit retry");
		source.m_result = Sailor::EAppInitializationResult::Ready;
		Require(start() == Status::Ok && start() == Status::AlreadyRunning && source.m_numInitializations == 2u,
			"a successful bootstrap must reject duplicate initialization without destroying its session");
		Require(source.m_bRollbackExclusive, "bootstrap rollback must retain exclusive lifecycle ownership");
	}

	void TestBootstrapHandsRollbackToShutdown(const std::string& token, Sailor::EAppInitializationResult result)
	{
		using namespace std::chrono_literals;
		using Status = EEditorEngineWebSocketHostStatus;
		using DispatchState = Sailor::Protocol::TEditorEngineProtocolLifecycleGate::EEditorDispatchState;
		struct TBootstrapSource
		{
			Sailor::Protocol::TEditorEngineProtocolLifecycleGate m_gate;
			Sailor::EAppInitializationResult m_result;
			std::promise<void> m_initializationEntered;
			std::promise<void> m_releaseInitialization;
			std::shared_future<void> m_resume = m_releaseInitialization.get_future().share();
			std::promise<void> m_stopEntered;
			std::atomic<uint32_t> m_numStops{0u};
			std::atomic<uint32_t> m_numShutdowns{0u};
			std::atomic<bool> m_bIsInitializing{false};
			std::atomic<bool> m_bStoppedDuringInitialization{false};
			std::thread::id m_stopThread;
			std::thread::id m_shutdownThread;
		} source;
		source.m_result = result;
		Sailor::Protocol::EditorEngineProtocolDependencies dependencies{};
		dependencies.m_context = &source;
		dependencies.m_lifecycleGate = &source.m_gate;
		dependencies.m_initialize = [](void* context, const char**, int32_t)
			{
				auto& state = *static_cast<TBootstrapSource*>(context);
				state.m_bIsInitializing = true;
				state.m_initializationEntered.set_value();
				state.m_resume.wait();
				state.m_bIsInitializing = false;
				return state.m_result;
			};
		dependencies.m_stop = [](void* context)
			{
				auto& state = *static_cast<TBootstrapSource*>(context);
				if (state.m_bIsInitializing)
				{
					state.m_bStoppedDuringInitialization = true;
				}
				if (++state.m_numStops == 1u)
				{
					state.m_stopEntered.set_value();
				}
			};
		dependencies.m_shutdown = [](void* context)
			{
				auto& state = *static_cast<TBootstrapSource*>(context);
				state.m_shutdownThread = std::this_thread::get_id();
				return ++state.m_numShutdowns > 1u;
			};
		const auto request = MakeRequest(1u, 10u);
		const uint16_t port = ReserveLocalHostPort();
		auto start = [&]()
			{
				return Sailor::Protocol::StartEditorEngineLocalHost(
					reinterpret_cast<const uint8_t*>(request.data()), static_cast<uint32_t>(request.size()),
					port, token.data(), static_cast<uint32_t>(token.size()), dependencies);
			};
		std::future<Status> bootstrap;
		std::future<bool> shutdown;
		std::future<bool> stopObserved;
		std::atomic<DispatchState> observation{DispatchState::Queued};
		bool bRecovered = false;
		Sailor::Tests::ScopeExit releaseInitialization([&]() { source.m_releaseInitialization.set_value(); });
		Sailor::Tests::ScopeExit cleanup([&]()
			{
				releaseInitialization.Run();
				if (bootstrap.valid())
				{
					bootstrap.wait();
				}
				if (shutdown.valid())
				{
					shutdown.wait();
				}
				if (stopObserved.valid())
				{
					stopObserved.wait();
				}
				bRecovered = Sailor::Protocol::StopEditorEngineLocalHost(true, dependencies);
			});
		bootstrap = std::async(std::launch::async, start);
		Require(source.m_initializationEntered.get_future().wait_for(1s) == std::future_status::ready,
			"bootstrap must reach initialization before starting the shutdown race");
		// Observe the existing cancellation notification without taking ownership
		// of shutdown or completing any lifecycle transition from the test.
		stopObserved = std::async(std::launch::async, [&]() { return source.m_gate.WaitForEditorDispatch(observation); });
		shutdown = std::async(std::launch::async, [&]()
			{
				source.m_stopThread = std::this_thread::get_id();
				return Sailor::Protocol::StopEditorEngineLocalHost(true, dependencies);
			});
		Require(stopObserved.wait_for(1s) == std::future_status::ready && !stopObserved.get(),
			"native Stop must claim shutdown while initialization is still blocked");
		const bool bEnteredStopEarly = source.m_stopEntered.get_future().wait_for(30ms) == std::future_status::ready;
		Require(!bEnteredStopEarly && source.m_numShutdowns == 0u,
			"native Stop must not enter App stop or shutdown during initialization");
		releaseInitialization.Run();
		const auto status = bootstrap.get();
		const bool bStopped = shutdown.get();
		Require(status == Status::InitializationFailed && !bStopped && source.m_numShutdowns == 1u &&
			source.m_shutdownThread == source.m_stopThread && !source.m_bStoppedDuringInitialization,
			"bootstrap must leave the single teardown attempt to the concurrent native Stop owner");
		Require(start() == Status::AlreadyRunning, "failed concurrent shutdown must retain the closed session");
		cleanup.Run();
		Require(bRecovered && source.m_numStops == 2u && source.m_numShutdowns == 2u,
			"an explicit native Stop retry must complete the retained session");
	}

	void TestBootstrapPreservesAnExistingServer(uint16_t port, const std::string& token)
	{
		uint32_t appCalls = 0u;
		Sailor::Protocol::EditorEngineProtocolDependencies dependencies{};
		dependencies.m_context = &appCalls;
		dependencies.m_initialize = [](void* context, const char**, int32_t)
			{
				++*static_cast<uint32_t*>(context);
				return Sailor::EAppInitializationResult::Failed;
			};
		dependencies.m_shutdown = [](void* context)
			{
				++*static_cast<uint32_t*>(context);
				return true;
			};
		const auto request = MakeRequest(1u, 10u);
		Require(Sailor::Protocol::StartEditorEngineLocalHost(
			reinterpret_cast<const uint8_t*>(request.data()), static_cast<uint32_t>(request.size()),
			port, token.data(), static_cast<uint32_t>(token.size()), dependencies) ==
			EEditorEngineWebSocketHostStatus::AlreadyRunning && appCalls == 0u,
			"a rejected bootstrap must not initialize or shut down an existing host");
		TestValidBinaryProtobufRoundTrip(port, token);
	}

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
			response.m_bSuccess &&
				response.m_bSupportsStrictInstanceIds &&
				response.m_error.empty(),
			"get-exit-code request must succeed and advertise strict restore support");
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

int main(const int argc, const char* const argv[])
{
	try
	{
#if !defined(_WIN32)
		std::signal(SIGPIPE, SIG_IGN);
#endif
		if (argc == 2 &&
			std::string(argv[1]) == "--late-stop-after-static-teardown")
		{
			TestLateStopAfterStaticTeardown();
			return 0;
		}
		Require(
			argc == 1,
			"unknown EditorEngineWebSocketServerTests argument");

			const std::string authorizationToken =
				"0123456789abcdef0123456789abcdef";
			TestBootstrapReservesBeforePublishingServer(authorizationToken);
			TestBootstrapRollbackAndRetry(authorizationToken);
			TestBootstrapHandsRollbackToShutdown(authorizationToken, Sailor::EAppInitializationResult::Ready);
			TestBootstrapHandsRollbackToShutdown(authorizationToken, Sailor::EAppInitializationResult::Failed);
			TestInvalidServerArguments(authorizationToken);
			TestFailedLocalHostInitialization(authorizationToken);
			{
				const TServerGuard server(authorizationToken);
				TestBootstrapPreservesAnExistingServer(server.GetPort(), authorizationToken);

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
