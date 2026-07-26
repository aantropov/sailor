#pragma once

#include "Core/Defines.h"

#include <cstdint>

namespace Sailor::Protocol
{
	enum class EEditorEngineTransportStatus : int32_t
	{
		Ok = 0,
		InvalidArguments = 1,
		PayloadTooLarge = 2,
		ParseFailed = 3,
		SerializeFailed = 4,
		AllocationFailed = 5,
		ExecutionFailed = 6
	};

	constexpr uint32_t EditorEngineProtocolVersion = 1u;
	constexpr uint32_t EditorEngineProtocolMaxPayloadSize =
		64u * 1024u * 1024u;

	struct EditorEngineProtocolDependencies
	{
		using FPullEditorViewportEvents = uint32_t (*)(
			void* context,
			char** events,
			uint32_t capacity);

		void* m_context = nullptr;
		FPullEditorViewportEvents m_pullEditorViewportEvents = nullptr;
	};

	int32_t InvokeEditorEngineProtocol(
		const uint8_t* requestData,
		uint32_t requestSize,
		uint8_t** responseData,
		uint32_t* responseSize);

	SAILOR_SHARED_API int32_t InvokeEditorEngineProtocol(
		const uint8_t* requestData,
		uint32_t requestSize,
		uint8_t** responseData,
		uint32_t* responseSize,
		const EditorEngineProtocolDependencies& dependencies);

	void FreeEditorEngineProtocolBuffer(uint8_t* buffer) noexcept;
}
