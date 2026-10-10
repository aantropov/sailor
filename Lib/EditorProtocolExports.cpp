#include "Core/Defines.h"
#include "EditorEngineProtocolInternal.h"
#include "EditorEngineWebSocketServer.h"
#include <cstdint>

extern "C"
{
	SAILOR_API int32_t SailorProtocolStartLocalHost(
		const uint8_t* initializeRequestData,
		uint32_t initializeRequestSize,
		uint16_t port,
		const char* authorizationToken,
		uint32_t authorizationTokenSize) noexcept
	{
		return static_cast<int32_t>(Sailor::Protocol::StartEditorEngineLocalHost(
			initializeRequestData, initializeRequestSize, port, authorizationToken, authorizationTokenSize));
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
		return Sailor::Protocol::StopEditorEngineLocalHost(bShutdownEngine) ? 1 : 0;
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
			return Sailor::Protocol::SetWindowsViewportHost(
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
