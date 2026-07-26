#include "Core/Defines.h"
#include "EditorEngineProtocolInternal.h"

#include <cstdint>

extern "C"
{
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
