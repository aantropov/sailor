#include "Core/Defines.h"
#include "EditorEngineProtocolInternal.h"
#include "EditorEngineWebSocketServer.h"
#include "Protocol/Generated/editor_engine.pb.h"
#include "Sailor.h"

#include <climits>
#include <cstdint>

namespace
{
	void RollbackLocalEditorHost() noexcept
	{
		try
		{
			Sailor::App::Stop();
		}
		catch (...)
		{
		}
		Sailor::Protocol::StopEditorEngineWebSocketServer();
		try
		{
			Sailor::Protocol::WaitForEditorEngineProtocolStartDrain();
			Sailor::App::Shutdown();
			Sailor::Protocol::ResetEditorEngineProtocolLifecycle();
		}
		catch (...)
		{
		}
	}
}

extern "C"
{
	SAILOR_API int32_t SailorProtocolStartLocalHost(
		const uint8_t* initializeRequestData,
		uint32_t initializeRequestSize,
		uint16_t port,
		const char* authorizationToken,
		uint32_t authorizationTokenSize) noexcept
	{
		using Sailor::Protocol::EEditorEngineTransportStatus;
		using Sailor::Protocol::EEditorEngineWebSocketHostStatus;
		using Sailor::Protocol::EditorEngineProtocolMaxPayloadSize;
		using sailor::editor::v1::ProtocolRequest;
		using sailor::editor::v1::ProtocolResponse;

		bool bOwnsLocalHost = false;
		if (!initializeRequestData ||
			initializeRequestSize == 0 ||
			initializeRequestSize > EditorEngineProtocolMaxPayloadSize ||
			initializeRequestSize > INT_MAX ||
			!authorizationToken ||
			authorizationTokenSize == 0)
		{
			return static_cast<int32_t>(
				EEditorEngineWebSocketHostStatus::InvalidArguments);
		}

		try
		{
			ProtocolRequest request;
			if (!request.ParseFromArray(
					initializeRequestData,
					static_cast<int>(initializeRequestSize)) ||
				request.command_case() != ProtocolRequest::kInitialize)
			{
				return static_cast<int32_t>(
					EEditorEngineWebSocketHostStatus::InvalidArguments);
			}

			const int32_t serverStatus =
				Sailor::Protocol::StartEditorEngineWebSocketServer(
					port,
					authorizationToken,
					authorizationTokenSize);
			if (serverStatus != static_cast<int32_t>(
					EEditorEngineWebSocketHostStatus::Ok))
			{
				return serverStatus;
			}
			bOwnsLocalHost = true;

			uint8_t* responseData = nullptr;
			uint32_t responseSize = 0;
			const int32_t invokeStatus =
				Sailor::Protocol::InvokeEditorEngineProtocol(
					initializeRequestData,
					initializeRequestSize,
					&responseData,
					&responseSize);
			if (invokeStatus != static_cast<int32_t>(
					EEditorEngineTransportStatus::Ok))
			{
				Sailor::Protocol::FreeEditorEngineProtocolBuffer(
					responseData);
				RollbackLocalEditorHost();
				return static_cast<int32_t>(
					EEditorEngineWebSocketHostStatus::InitializationFailed);
			}

			ProtocolResponse response;
			const bool bParsed = response.ParseFromArray(
				responseData,
				static_cast<int>(responseSize));
			Sailor::Protocol::FreeEditorEngineProtocolBuffer(responseData);
			if (!bParsed ||
				response.protocol_version() != request.protocol_version() ||
				response.request_id() != request.request_id() ||
				!response.success())
			{
				RollbackLocalEditorHost();
				return static_cast<int32_t>(
					EEditorEngineWebSocketHostStatus::InitializationFailed);
			}

			return static_cast<int32_t>(
				EEditorEngineWebSocketHostStatus::Ok);
		}
		catch (...)
		{
			if (bOwnsLocalHost)
			{
				RollbackLocalEditorHost();
			}
			return static_cast<int32_t>(
				EEditorEngineWebSocketHostStatus::ExecutionFailed);
		}
	}

	SAILOR_API void SailorProtocolRequestLocalHostStop() noexcept
	{
		try
		{
			Sailor::App::Stop();
		}
		catch (...)
		{
		}
	}

	SAILOR_API void SailorProtocolStopLocalHost(
		const bool bShutdownEngine) noexcept
	{
		try
		{
			Sailor::App::Stop();
		}
		catch (...)
		{
		}
		Sailor::Protocol::StopEditorEngineWebSocketServer();
		try
		{
			Sailor::Protocol::WaitForEditorEngineProtocolStartDrain();
			if (bShutdownEngine)
			{
				Sailor::App::Shutdown();
				Sailor::Protocol::ResetEditorEngineProtocolLifecycle();
			}
		}
		catch (...)
		{
		}
	}

	SAILOR_API int32_t SailorProtocolInvoke(
		const uint8_t* requestData,
		uint32_t requestSize,
		uint8_t** responseData,
		uint32_t* responseSize) noexcept
	{
		if (responseData)
		{
			*responseData = nullptr;
		}
		if (responseSize)
		{
			*responseSize = 0;
		}

		try
		{
			return Sailor::Protocol::InvokeEditorEngineProtocol(
				requestData,
				requestSize,
				responseData,
				responseSize);
		}
		catch (...)
		{
			if (responseData && *responseData)
			{
				Sailor::Protocol::FreeEditorEngineProtocolBuffer(*responseData);
				*responseData = nullptr;
			}
			if (responseSize)
			{
				*responseSize = 0;
			}
			return static_cast<int32_t>(
				Sailor::Protocol::EEditorEngineTransportStatus::ExecutionFailed);
		}
	}

	SAILOR_API void SailorProtocolFreeBuffer(uint8_t* buffer) noexcept
	{
		Sailor::Protocol::FreeEditorEngineProtocolBuffer(buffer);
	}
}
