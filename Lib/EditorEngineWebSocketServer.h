#pragma once

#include "Core/Defines.h"

#include <cstdint>

namespace Sailor::Protocol
{
	enum class EEditorEngineWebSocketHostStatus : int32_t
	{
		Ok = 0,
		InvalidArguments = 1,
		AlreadyRunning = 2,
		ListenFailed = 3,
		InitializationFailed = 4,
		ExecutionFailed = 5,
		NetworkInitializationFailed = 6
	};

	inline constexpr const char* EditorEngineWebSocketPath =
		"/sailor/editor/v1";
	inline constexpr const char* EditorEngineWebSocketSubprotocol =
		"sailor.editor.v1";

	SAILOR_SHARED_API int32_t StartEditorEngineWebSocketServer(
		uint16_t port,
		const char* authorizationToken,
		uint32_t authorizationTokenSize);

	SAILOR_SHARED_API void StopEditorEngineWebSocketServer() noexcept;
}
