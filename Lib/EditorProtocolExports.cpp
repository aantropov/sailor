#include "Core/Defines.h"
#include "Editor/EditorRuntimeBridge.h"
#include "EditorEngineProtocolInternal.h"
#include "EditorEngineWebSocketServer.h"
#include "Protocol/Generated/editor_engine.pb.h"
#include "Sailor.h"

#include <climits>
#include <cstdint>

namespace
{
	bool StopLocalEditorHost(bool bShutdownEngine) noexcept
	{
		try
		{
			if (bShutdownEngine)
			{
				// Close admission before joining socket callbacks or the Start worker.
				if (!Sailor::Protocol::TryDrainEditorEngineProtocolForShutdown())
				{
					return false;
				}
			}
			else
			{
				Sailor::Protocol::RequestEditorEngineProtocolStop();
			}
			Sailor::Protocol::StopEditorEngineWebSocketServer();
			if (!bShutdownEngine)
			{
				Sailor::Protocol::WaitForEditorEngineProtocolStartDrain();
				return true;
			}
			if (Sailor::App::Shutdown())
			{
				Sailor::Protocol::ResetEditorEngineProtocolLifecycle();
				return true;
			}
		}
		catch (...)
		{
			Sailor::Protocol::StopEditorEngineWebSocketServer();
		}
		Sailor::Protocol::FailEditorEngineProtocolShutdown();
		return false;
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
				return static_cast<int32_t>(
					StopLocalEditorHost(true) ? EEditorEngineWebSocketHostStatus::InitializationFailed :
					EEditorEngineWebSocketHostStatus::ShutdownFailed);
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
				return static_cast<int32_t>(
					StopLocalEditorHost(true) ? EEditorEngineWebSocketHostStatus::InitializationFailed :
					EEditorEngineWebSocketHostStatus::ShutdownFailed);
			}

			return static_cast<int32_t>(
				EEditorEngineWebSocketHostStatus::Ok);
		}
		catch (...)
		{
			if (bOwnsLocalHost && !StopLocalEditorHost(true))
			{
				return static_cast<int32_t>(EEditorEngineWebSocketHostStatus::ShutdownFailed);
			}
			return static_cast<int32_t>(
				EEditorEngineWebSocketHostStatus::ExecutionFailed);
		}
	}

	SAILOR_API void SailorProtocolRequestLocalHostStop() noexcept
	{
		try
		{
			Sailor::Protocol::RequestEditorEngineProtocolStop();
		}
		catch (...)
		{
		}
	}

	SAILOR_API int32_t SailorProtocolStopLocalHost(
		const bool bShutdownEngine) noexcept
	{
		return StopLocalEditorHost(bShutdownEngine) ? 1 : 0;
	}

	SAILOR_API int32_t SailorProtocolSetMacViewportHost(uint64_t viewportId, uintptr_t layer) noexcept
	{
		try
		{
			return Sailor::Protocol::SetMacViewportHost(viewportId, layer) ? 1 : 0;
		}
		catch (...)
		{
			return 0;
		}
	}

	SAILOR_API int32_t SailorProtocolSetWindowsViewportHost(
		uint64_t viewportId,
		void* swapChainPanelInspectable,
		float compositionScale) noexcept
	{
		try
		{
			return Sailor::EditorRuntime::SetEditorRemoteViewportWindowsHost(
				viewportId,
				swapChainPanelInspectable,
				compositionScale) ? 1 : 0;
		}
		catch (...)
		{
			return 0;
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
