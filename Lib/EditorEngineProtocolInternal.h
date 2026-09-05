#pragma once

#include "Core/Defines.h"

#include <cstdint>
#include <string>

namespace google::protobuf
{
	class Message;
}

namespace sailor::editor::v1
{
	class ProtocolRequest;
	class ProtocolResponse;
	class Vector4;
}

namespace Sailor::Protocol
{
	class TEditorEngineProtocolLifecycleGate;

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
	constexpr uint32_t EditorEngineProtocolMaxPayloadSize = 64u * 1024u * 1024u;

	struct EditorEngineProtocolDependencies
	{
		using FEditorEngineProtocolOperation = void (*)(void* context);
		// Must return only after operation has completed. The protocol caller
		// owns operationContext and synchronously consumes its response/error.
		using FDispatchEditorEngineProtocolOperation = bool (*)(void* dispatchContext,
			FEditorEngineProtocolOperation operation,
			void* operationContext);
		using FPullEditorViewportEvents = uint32_t (*)(void* context, char** events, uint32_t capacity);
		using FLifecycleRoutine = void (*)(void* context);

		void* m_context = nullptr;
		FPullEditorViewportEvents m_pullEditorViewportEvents = nullptr;
		FLifecycleRoutine m_start = nullptr;
		FLifecycleRoutine m_stop = nullptr;
		FLifecycleRoutine m_shutdown = nullptr;
		TEditorEngineProtocolLifecycleGate* m_lifecycleGate = nullptr;
		void* m_editorDispatchContext = nullptr;
		FDispatchEditorEngineProtocolOperation m_dispatchEditorOperation = nullptr;
		bool m_bAllowInitialize = true;
	};

	bool DispatchEditorEngineProtocolOperationOnEditorThread(void* dispatchContext,
		EditorEngineProtocolDependencies::FEditorEngineProtocolOperation operation,
		void* operationContext);

	void DispatchEditorEngineProtocolRequest(const sailor::editor::v1::ProtocolRequest& request,
		sailor::editor::v1::ProtocolResponse& response,
		const EditorEngineProtocolDependencies& dependencies);

	namespace EditorEngineProtocolCommands
	{
		void DispatchRequest(const sailor::editor::v1::ProtocolRequest& request,
			sailor::editor::v1::ProtocolResponse& response,
			const EditorEngineProtocolDependencies& dependencies);
		void StopEngine(const EditorEngineProtocolDependencies& dependencies);
		void SetError(sailor::editor::v1::ProtocolResponse& response, const std::string& error);
		void SetEmptyResult(sailor::editor::v1::ProtocolResponse& response);
		void SetBoolResult(sailor::editor::v1::ProtocolResponse& response, bool value);
		void SetStringResult(sailor::editor::v1::ProtocolResponse& response, const char* value, uint32_t length);
		void SetSuccess(sailor::editor::v1::ProtocolResponse& response);
		bool ValidateBatchCount(uint32_t count, sailor::editor::v1::ProtocolResponse& response);
		bool IsFiniteVector4(const sailor::editor::v1::Vector4& value);
		bool DispatchGICommand(const sailor::editor::v1::ProtocolRequest& request,
			sailor::editor::v1::ProtocolResponse& response);
		bool DispatchWorldCommand(const sailor::editor::v1::ProtocolRequest& request,
			sailor::editor::v1::ProtocolResponse& response);
		bool DispatchViewportCommand(const sailor::editor::v1::ProtocolRequest& request,
			sailor::editor::v1::ProtocolResponse& response,
			const EditorEngineProtocolDependencies& dependencies);
		bool TryFindEmbeddedNull(const google::protobuf::Message& message, std::string& outFieldPath);
	}

	int32_t InvokeEditorEngineProtocol(const uint8_t* requestData,
		uint32_t requestSize,
		uint8_t** responseData,
		uint32_t* responseSize);

	SAILOR_SHARED_API int32_t InvokeEditorEngineProtocol(const uint8_t* requestData,
		uint32_t requestSize,
		uint8_t** responseData,
		uint32_t* responseSize,
		const EditorEngineProtocolDependencies& dependencies);

	void FreeEditorEngineProtocolBuffer(uint8_t* buffer) noexcept;
	void WaitForEditorEngineProtocolStartDrain();
	void ResetEditorEngineProtocolLifecycle();
}
