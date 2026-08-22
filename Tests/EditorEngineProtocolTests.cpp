#include "EditorEngineProtocolInternal.h"
#include "EditorEngineProtocolLifecycle.h"
#include "Protocol/Generated/editor_engine.pb.h"

#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <condition_variable>
#include <future>
#include <iostream>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

#if defined(_WIN32)
#define SAILOR_PROTOCOL_TEST_IMPORT __declspec(dllimport)
#else
#define SAILOR_PROTOCOL_TEST_IMPORT
#endif

extern "C"
{
	SAILOR_PROTOCOL_TEST_IMPORT int32_t SailorProtocolInvoke(
		const uint8_t* requestData,
		uint32_t requestSize,
		uint8_t** responseData,
		uint32_t* responseSize) noexcept;

	SAILOR_PROTOCOL_TEST_IMPORT void SailorProtocolFreeBuffer(uint8_t* buffer) noexcept;
}

namespace
{
	using Sailor::Protocol::EEditorEngineTransportStatus;
	using Sailor::Protocol::EditorEngineProtocolStrictInstanceIdsVersion;
	using Sailor::Protocol::EditorEngineProtocolMaxPayloadSize;
	using Sailor::Protocol::EditorEngineProtocolVersion;

	constexpr uint32_t c_protocolVersionField = 1;
	constexpr uint32_t c_requestIdField = 2;
	constexpr uint32_t c_successField = 3;
	constexpr uint32_t c_errorField = 4;
	constexpr uint32_t c_supportsStrictInstanceIdsField = 5;
	constexpr uint32_t c_boolResultField = 11;
	constexpr uint32_t c_int32ResultField = 12;
	constexpr uint32_t c_uint64ResultField = 14;
	constexpr uint32_t c_instanceIdResultField = 18;
	constexpr uint32_t c_viewportEventBatchResultField = 19;
	constexpr uint32_t c_viewportToolStateResultField = 21;
	constexpr uint32_t c_animatorStateResultField = 22;
	constexpr uint32_t c_emptyResultField = 10;
	constexpr uint32_t c_initializeCommandField = 10;
	constexpr uint32_t c_startCommandField = 11;
	constexpr uint32_t c_stopCommandField = 12;
	constexpr uint32_t c_shutdownCommandField = 13;
	constexpr uint32_t c_getExitCodeCommandField = 16;
	constexpr uint32_t c_loadEditorWorldCommandField = 21;
	constexpr uint32_t c_pullEditorViewportEventsCommandField = 32;
	constexpr uint32_t c_getManagedMutationRevisionCommandField = 33;
	constexpr uint32_t c_renderPathTracedImageCommandField = 45;
	constexpr uint32_t c_isEngineRunningCommandField = 48;
	constexpr uint32_t c_instantiatePrefabFromYamlCommandField = 42;
	constexpr uint32_t c_createModelInstanceCommandField = 58;
	constexpr uint32_t c_setAnimatorParameterCommandField = 59;
	constexpr uint32_t c_setEditorSimulationCommandField = 61;
	constexpr uint32_t c_getEditorSimulationStateCommandField = 62;
	constexpr uint32_t c_previewAudioAssetCommandField = 63;
	constexpr uint32_t c_setEditorStatsModeCommandField = 64;

	void Require(bool condition, const std::string& message)
	{
		if (!condition)
		{
			throw std::runtime_error(message);
		}
	}

	void AppendVarint(std::string& output, uint64_t value)
	{
		while (value >= 0x80u)
		{
			output.push_back(static_cast<char>((value & 0x7fu) | 0x80u));
			value >>= 7u;
		}
		output.push_back(static_cast<char>(value));
	}

	void AppendKey(std::string& output, uint32_t fieldNumber, uint32_t wireType)
	{
		AppendVarint(output, (static_cast<uint64_t>(fieldNumber) << 3u) | wireType);
	}

	void AppendVarintField(std::string& output, uint32_t fieldNumber, uint64_t value)
	{
		AppendKey(output, fieldNumber, 0u);
		AppendVarint(output, value);
	}

	void AppendMessageField(
		std::string& output,
		uint32_t fieldNumber,
		const std::string& value)
	{
		AppendKey(output, fieldNumber, 2u);
		AppendVarint(output, value.size());
		output.append(value);
	}

	std::string MakeRequest(
		uint32_t version,
		uint64_t requestId,
		uint32_t commandField = 0,
		const std::string& command = {})
	{
		std::string request;
		AppendVarintField(request, c_protocolVersionField, version);
		if (requestId != 0)
		{
			AppendVarintField(request, c_requestIdField, requestId);
		}
		if (commandField != 0)
		{
			AppendMessageField(request, commandField, command);
		}
		return request;
	}

	bool ReadVarint(
		const uint8_t* data,
		size_t size,
		size_t& offset,
		uint64_t& outValue)
	{
		outValue = 0;
		for (uint32_t shift = 0; shift < 64 && offset < size; shift += 7)
		{
			const uint8_t byte = data[offset++];
			outValue |= static_cast<uint64_t>(byte & 0x7fu) << shift;
			if ((byte & 0x80u) == 0)
			{
				return true;
			}
		}
		return false;
	}

	struct TDecodedResponse
	{
		uint32_t m_protocolVersion = 0;
		uint64_t m_requestId = 0;
		bool m_success = false;
		bool m_supportsStrictInstanceIds = false;
		std::string m_error{};
		uint32_t m_resultField = 0;
		bool m_boolResult = false;
		int32_t m_int32Result = 0;
		std::string m_resultPayload{};
	};

	bool TryDecodeInt32Result(
		const uint8_t* data,
		size_t size,
		int32_t& outValue)
	{
		outValue = 0;
		size_t offset = 0;
		while (offset < size)
		{
			uint64_t key = 0;
			if (!ReadVarint(data, size, offset, key))
			{
				return false;
			}

			const uint32_t fieldNumber = static_cast<uint32_t>(key >> 3u);
			const uint32_t wireType = static_cast<uint32_t>(key & 0x7u);
			uint64_t value = 0;
			if (wireType != 0 || !ReadVarint(data, size, offset, value))
			{
				return false;
			}
			if (fieldNumber == 1)
			{
				outValue = static_cast<int32_t>(value);
			}
		}
		return true;
	}

	bool TryDecodeBoolResult(
		const uint8_t* data,
		size_t size,
		bool& outValue)
	{
		outValue = false;
		size_t offset = 0;
		while (offset < size)
		{
			uint64_t key = 0;
			if (!ReadVarint(data, size, offset, key))
			{
				return false;
			}

			const uint32_t fieldNumber = static_cast<uint32_t>(key >> 3u);
			const uint32_t wireType = static_cast<uint32_t>(key & 0x7u);
			uint64_t value = 0;
			if (wireType != 0 || !ReadVarint(data, size, offset, value))
			{
				return false;
			}
			if (fieldNumber == 1)
			{
				outValue = value != 0;
			}
		}
		return true;
	}

	TDecodedResponse DecodeResponse(const uint8_t* data, size_t size)
	{
		Require(data != nullptr && size > 0, "protocol response must not be empty");

		TDecodedResponse response;
		size_t offset = 0;
		while (offset < size)
		{
			uint64_t key = 0;
			Require(ReadVarint(data, size, offset, key), "response field key must be valid");
			const uint32_t fieldNumber = static_cast<uint32_t>(key >> 3u);
			const uint32_t wireType = static_cast<uint32_t>(key & 0x7u);

			if (wireType == 0)
			{
				uint64_t value = 0;
				Require(ReadVarint(data, size, offset, value), "response varint must be valid");
				if (fieldNumber == c_protocolVersionField)
				{
					response.m_protocolVersion = static_cast<uint32_t>(value);
				}
				else if (fieldNumber == c_requestIdField)
				{
					response.m_requestId = value;
				}
				else if (fieldNumber == c_successField)
				{
					response.m_success = value != 0;
				}
				else if (fieldNumber ==
					c_supportsStrictInstanceIdsField)
				{
					response.m_supportsStrictInstanceIds =
						value != 0;
				}
				continue;
			}

			Require(wireType == 2, "response must use supported protobuf wire types");
			uint64_t length = 0;
			Require(ReadVarint(data, size, offset, length), "response field length must be valid");
			Require(length <= size - offset, "response field length must fit its buffer");
			const uint8_t* value = data + offset;
			offset += static_cast<size_t>(length);

			if (fieldNumber == c_errorField)
			{
				response.m_error.assign(
					reinterpret_cast<const char*>(value),
					static_cast<size_t>(length));
			}
			else if (fieldNumber >= 10u &&
				fieldNumber <= c_animatorStateResultField)
			{
				response.m_resultField = fieldNumber;
				response.m_resultPayload.assign(
					reinterpret_cast<const char*>(value),
					static_cast<size_t>(length));
				if (fieldNumber == c_boolResultField ||
					fieldNumber == c_instanceIdResultField)
				{
					Require(
						TryDecodeBoolResult(
							value,
							static_cast<size_t>(length),
							response.m_boolResult),
						"bool result payload must be valid");
				}
				else if (fieldNumber == c_int32ResultField)
				{
					Require(
						TryDecodeInt32Result(
							value,
							static_cast<size_t>(length),
							response.m_int32Result),
						"int32 result payload must be valid");
				}
			}
		}
		return response;
	}

	bool SkipField(
		const uint8_t* data,
		size_t size,
		size_t& offset,
		uint32_t wireType)
	{
		if (wireType == 0)
		{
			uint64_t ignored = 0;
			return ReadVarint(data, size, offset, ignored);
		}
		if (wireType == 2)
		{
			uint64_t length = 0;
			if (!ReadVarint(data, size, offset, length) ||
				length > size - offset)
			{
				return false;
			}
			offset += static_cast<size_t>(length);
			return true;
		}
		if (wireType == 5)
		{
			if (size - offset < sizeof(uint32_t))
			{
				return false;
			}
			offset += sizeof(uint32_t);
			return true;
		}
		return false;
	}

	bool TryReadLengthDelimited(
		const uint8_t* data,
		size_t size,
		size_t& offset,
		const uint8_t*& outData,
		size_t& outSize)
	{
		uint64_t length = 0;
		if (!ReadVarint(data, size, offset, length) ||
			length > size - offset)
		{
			return false;
		}

		outData = data + offset;
		outSize = static_cast<size_t>(length);
		offset += outSize;
		return true;
	}

	bool TryDecodeViewportSelection(
		const uint8_t* data,
		size_t size,
		std::string& outSelectedInstanceId)
	{
		size_t offset = 0;
		while (offset < size)
		{
			uint64_t key = 0;
			if (!ReadVarint(data, size, offset, key))
			{
				return false;
			}

			const uint32_t fieldNumber = static_cast<uint32_t>(key >> 3u);
			const uint32_t wireType = static_cast<uint32_t>(key & 0x7u);
			if (fieldNumber != 1u)
			{
				if (!SkipField(data, size, offset, wireType))
				{
					return false;
				}
				continue;
			}
			if (wireType != 2u)
			{
				return false;
			}

			const uint8_t* value = nullptr;
			size_t valueSize = 0;
			if (!TryReadLengthDelimited(
					data,
					size,
					offset,
					value,
					valueSize))
			{
				return false;
			}
			outSelectedInstanceId.assign(
				reinterpret_cast<const char*>(value),
				valueSize);
		}
		return true;
	}

	bool TryReadFixed32(
		const uint8_t* data,
		size_t size,
		size_t& offset,
		uint32_t& outValue)
	{
		if (size - offset < sizeof(uint32_t))
		{
			return false;
		}

		outValue =
			static_cast<uint32_t>(data[offset]) |
			(static_cast<uint32_t>(data[offset + 1]) << 8u) |
			(static_cast<uint32_t>(data[offset + 2]) << 16u) |
			(static_cast<uint32_t>(data[offset + 3]) << 24u);
		offset += sizeof(uint32_t);
		return true;
	}

	bool TryDecodeViewportAssetDrop(
		const uint8_t* data,
		size_t size,
		std::string& outFileId,
		float& outNormalizedX,
		float& outNormalizedY)
	{
		bool bHasFileId = false;
		bool bHasNormalizedX = false;
		bool bHasNormalizedY = false;
		size_t offset = 0;
		while (offset < size)
		{
			uint64_t key = 0;
			if (!ReadVarint(data, size, offset, key))
			{
				return false;
			}

			const uint32_t fieldNumber =
				static_cast<uint32_t>(key >> 3u);
			const uint32_t wireType =
				static_cast<uint32_t>(key & 0x7u);
			if (fieldNumber == 1u)
			{
				const uint8_t* value = nullptr;
				size_t valueSize = 0;
				if (wireType != 2u ||
					!TryReadLengthDelimited(
						data,
						size,
						offset,
						value,
						valueSize))
				{
					return false;
				}
				outFileId.assign(
					reinterpret_cast<const char*>(value),
					valueSize);
				bHasFileId = true;
				continue;
			}

			if (fieldNumber == 2u || fieldNumber == 3u)
			{
				uint32_t bits = 0;
				if (wireType != 5u ||
					!TryReadFixed32(data, size, offset, bits))
				{
					return false;
				}

				float value = 0.0f;
				std::memcpy(&value, &bits, sizeof(value));
				if (fieldNumber == 2u)
				{
					outNormalizedX = value;
					bHasNormalizedX = true;
				}
				else
				{
					outNormalizedY = value;
					bHasNormalizedY = true;
				}
				continue;
			}

			if (!SkipField(data, size, offset, wireType))
			{
				return false;
			}
		}

		return bHasFileId &&
			bHasNormalizedX &&
			bHasNormalizedY;
	}

	bool TryDecodeViewportToolShortcut(
		const uint8_t* data,
		size_t size,
		uint32_t& outKeyCode)
	{
		bool bHasKeyCode = false;
		size_t offset = 0;
		while (offset < size)
		{
			uint64_t key = 0;
			if (!ReadVarint(data, size, offset, key))
			{
				return false;
			}

			const uint32_t fieldNumber =
				static_cast<uint32_t>(key >> 3u);
			const uint32_t wireType =
				static_cast<uint32_t>(key & 0x7u);
			if (fieldNumber == 1u)
			{
				uint64_t keyCode = 0;
				if (wireType != 0u ||
					!ReadVarint(data, size, offset, keyCode) ||
					keyCode >
						static_cast<uint64_t>(
							std::numeric_limits<uint32_t>::max()))
				{
					return false;
				}

				outKeyCode = static_cast<uint32_t>(keyCode);
				bHasKeyCode = true;
				continue;
			}

			if (!SkipField(data, size, offset, wireType))
			{
				return false;
			}
		}

		return bHasKeyCode;
	}

	struct TDecodedViewportEvent
	{
		uint64_t m_revision = 0;
		uint64_t m_managedMutationRevision = 0;
		bool m_hasSelection = false;
		std::string m_selectedInstanceId{};
		bool m_hasAssetDrop = false;
		std::string m_assetFileId{};
		float m_normalizedX = 0.0f;
		float m_normalizedY = 0.0f;
		bool m_hasToolShortcut = false;
		uint32_t m_toolShortcutKeyCode = 0;
	};

	bool TryDecodeViewportEvent(
		const uint8_t* data,
		size_t size,
		TDecodedViewportEvent& outEvent)
	{
		size_t offset = 0;
		while (offset < size)
		{
			uint64_t key = 0;
			if (!ReadVarint(data, size, offset, key))
			{
				return false;
			}

			const uint32_t fieldNumber = static_cast<uint32_t>(key >> 3u);
			const uint32_t wireType = static_cast<uint32_t>(key & 0x7u);
			if (fieldNumber == 1u || fieldNumber == 2u)
			{
				uint64_t value = 0;
				if (wireType != 0u ||
					!ReadVarint(data, size, offset, value))
				{
					return false;
				}
				if (fieldNumber == 1u)
				{
					outEvent.m_revision = value;
				}
				else
				{
					outEvent.m_managedMutationRevision = value;
				}
				continue;
			}
			if (fieldNumber == 10u)
			{
				const uint8_t* selection = nullptr;
				size_t selectionSize = 0;
				if (wireType != 2u ||
					!TryReadLengthDelimited(
						data,
						size,
						offset,
						selection,
						selectionSize) ||
					!TryDecodeViewportSelection(
						selection,
						selectionSize,
						outEvent.m_selectedInstanceId))
				{
					return false;
				}
				outEvent.m_hasSelection = true;
				continue;
			}
			if (fieldNumber == 12u)
			{
				const uint8_t* assetDrop = nullptr;
				size_t assetDropSize = 0;
				if (wireType != 2u ||
					!TryReadLengthDelimited(
						data,
						size,
						offset,
						assetDrop,
						assetDropSize) ||
					!TryDecodeViewportAssetDrop(
						assetDrop,
						assetDropSize,
						outEvent.m_assetFileId,
						outEvent.m_normalizedX,
						outEvent.m_normalizedY))
				{
					return false;
				}
				outEvent.m_hasAssetDrop = true;
				continue;
			}
			if (fieldNumber == 13u)
			{
				const uint8_t* toolShortcut = nullptr;
				size_t toolShortcutSize = 0;
				if (wireType != 2u ||
					!TryReadLengthDelimited(
						data,
						size,
						offset,
						toolShortcut,
						toolShortcutSize) ||
					!TryDecodeViewportToolShortcut(
						toolShortcut,
						toolShortcutSize,
						outEvent.m_toolShortcutKeyCode))
				{
					return false;
				}
				outEvent.m_hasToolShortcut = true;
				continue;
			}
			if (!SkipField(data, size, offset, wireType))
			{
				return false;
			}
		}
		return true;
	}

	bool TryDecodeViewportEventBatch(
		const std::string& payload,
		uint32_t& outNumEvents,
		TDecodedViewportEvent& outEvent)
	{
		outNumEvents = 0;
		const auto* data =
			reinterpret_cast<const uint8_t*>(payload.data());
		const size_t size = payload.size();
		size_t offset = 0;
		while (offset < size)
		{
			uint64_t key = 0;
			if (!ReadVarint(data, size, offset, key))
			{
				return false;
			}

			const uint32_t fieldNumber = static_cast<uint32_t>(key >> 3u);
			const uint32_t wireType = static_cast<uint32_t>(key & 0x7u);
			if (fieldNumber != 1u)
			{
				if (!SkipField(data, size, offset, wireType))
				{
					return false;
				}
				continue;
			}

			const uint8_t* eventData = nullptr;
			size_t eventSize = 0;
			TDecodedViewportEvent event;
			if (wireType != 2u ||
				!TryReadLengthDelimited(
					data,
					size,
					offset,
					eventData,
					eventSize) ||
				!TryDecodeViewportEvent(eventData, eventSize, event))
			{
				return false;
			}
			if (outNumEvents == 0)
			{
				outEvent = std::move(event);
			}
			++outNumEvents;
		}
		return true;
	}

	class TProtocolBuffer final
	{
	public:
		TProtocolBuffer() = default;
		TProtocolBuffer(const TProtocolBuffer&) = delete;
		TProtocolBuffer& operator=(const TProtocolBuffer&) = delete;

		~TProtocolBuffer()
		{
			SailorProtocolFreeBuffer(m_data);
		}

		uint8_t** GetDataOutput() { return &m_data; }
		uint32_t* GetSizeOutput() { return &m_size; }
		const uint8_t* GetData() const { return m_data; }
		uint32_t GetSize() const { return m_size; }

	private:
		uint8_t* m_data = nullptr;
		uint32_t m_size = 0;
	};

	int32_t Invoke(const std::string& request, TProtocolBuffer& response)
	{
		return SailorProtocolInvoke(
			reinterpret_cast<const uint8_t*>(request.data()),
			static_cast<uint32_t>(request.size()),
			response.GetDataOutput(),
			response.GetSizeOutput());
	}

	int32_t Invoke(
		const std::string& request,
		TProtocolBuffer& response,
		const Sailor::Protocol::EditorEngineProtocolDependencies& dependencies)
	{
		return Sailor::Protocol::InvokeEditorEngineProtocol(
			reinterpret_cast<const uint8_t*>(request.data()),
			static_cast<uint32_t>(request.size()),
			response.GetDataOutput(),
			response.GetSizeOutput(),
			dependencies);
	}

	TDecodedResponse RequireProtocolResponse(
		const std::string& request,
		TProtocolBuffer& response)
	{
		Require(
			Invoke(request, response) ==
				static_cast<int32_t>(EEditorEngineTransportStatus::Ok),
			"valid request envelope must return transport success");
		Require(
			response.GetSize() <= EditorEngineProtocolMaxPayloadSize,
			"response must stay within the 64 MiB transport limit");
		return DecodeResponse(response.GetData(), response.GetSize());
	}

	TDecodedResponse RequireProtocolResponse(
		const std::string& request,
		TProtocolBuffer& response,
		const Sailor::Protocol::EditorEngineProtocolDependencies& dependencies)
	{
		Require(
			Invoke(request, response, dependencies) ==
				static_cast<int32_t>(EEditorEngineTransportStatus::Ok),
			"valid request envelope must return transport success");
		Require(
			response.GetSize() <= EditorEngineProtocolMaxPayloadSize,
			"response must stay within the 64 MiB transport limit");
		return DecodeResponse(response.GetData(), response.GetSize());
	}

	void TestInvalidArgumentsResetOutputs()
	{
		uint8_t requestByte = 0;
		uint8_t* responseData = reinterpret_cast<uint8_t*>(uintptr_t{ 1 });
		uint32_t responseSize = 42;
		Require(
			SailorProtocolInvoke(nullptr, 1, &responseData, &responseSize) ==
				static_cast<int32_t>(EEditorEngineTransportStatus::InvalidArguments),
			"null request data must be rejected");
		Require(
			responseData == nullptr && responseSize == 0,
			"invalid arguments must reset response outputs");

		responseSize = 42;
		Require(
			SailorProtocolInvoke(&requestByte, 1, nullptr, &responseSize) ==
				static_cast<int32_t>(EEditorEngineTransportStatus::InvalidArguments),
			"null response data output must be rejected");
		Require(responseSize == 0, "available size output must be reset");

		responseData = reinterpret_cast<uint8_t*>(uintptr_t{ 1 });
		Require(
			SailorProtocolInvoke(&requestByte, 1, &responseData, nullptr) ==
				static_cast<int32_t>(EEditorEngineTransportStatus::InvalidArguments),
			"null response size output must be rejected");
		Require(responseData == nullptr, "available data output must be reset");

		responseData = reinterpret_cast<uint8_t*>(uintptr_t{ 1 });
		responseSize = 42;
		Require(
			SailorProtocolInvoke(&requestByte, 0, &responseData, &responseSize) ==
				static_cast<int32_t>(EEditorEngineTransportStatus::InvalidArguments),
			"empty payload must be rejected");
		Require(
			responseData == nullptr && responseSize == 0,
			"empty payload must not publish a response");

		SailorProtocolFreeBuffer(nullptr);
	}

	void TestOversizedAndMalformedPayloads()
	{
		const uint8_t requestByte = 0;
		uint8_t* responseData = reinterpret_cast<uint8_t*>(uintptr_t{ 1 });
		uint32_t responseSize = 42;
		Require(
			SailorProtocolInvoke(
				&requestByte,
				EditorEngineProtocolMaxPayloadSize + 1u,
				&responseData,
				&responseSize) ==
				static_cast<int32_t>(EEditorEngineTransportStatus::PayloadTooLarge),
			"payload larger than 64 MiB must be rejected before reading");
		Require(
			responseData == nullptr && responseSize == 0,
			"oversized payload must not publish a response");

		const std::string malformedRequest(1, static_cast<char>(0x80u));
		TProtocolBuffer response;
		Require(
			Invoke(malformedRequest, response) ==
				static_cast<int32_t>(EEditorEngineTransportStatus::ParseFailed),
			"malformed protobuf must report parse failure");
		Require(
			response.GetData() == nullptr && response.GetSize() == 0,
			"malformed protobuf must not publish a response");
	}

	void TestCommandExceptionIsContainedByTransportBoundary()
	{
		std::string initializeRequest;
		AppendMessageField(initializeRequest, 1u, "SailorEditor");
		AppendMessageField(initializeRequest, 1u, "--hwnd");
		AppendMessageField(initializeRequest, 1u, "not-a-number");

		const std::string request = MakeRequest(
			EditorEngineProtocolVersion,
			16,
			c_initializeCommandField,
			initializeRequest);
		uint8_t* responseData = reinterpret_cast<uint8_t*>(uintptr_t{ 1 });
		uint32_t responseSize = 42;
		Require(
			SailorProtocolInvoke(
				reinterpret_cast<const uint8_t*>(request.data()),
				static_cast<uint32_t>(request.size()),
				&responseData,
				&responseSize) ==
				static_cast<int32_t>(EEditorEngineTransportStatus::ExecutionFailed),
			"exceptions from a valid command payload must be contained by the C transport");
		Require(
			responseData == nullptr && responseSize == 0,
			"an execution failure must not publish a partial response");
	}

	void TestEnvelopeValidation()
	{
		{
			TProtocolBuffer buffer;
			const auto response = RequireProtocolResponse(
				MakeRequest(
					EditorEngineProtocolStrictInstanceIdsVersion + 1u,
					17,
					c_getExitCodeCommandField),
				buffer);
			Require(
				response.m_protocolVersion ==
					EditorEngineProtocolStrictInstanceIdsVersion &&
				response.m_requestId == 17 &&
				!response.m_success &&
				response.m_error.find("version") != std::string::npos &&
				response.m_resultField == 0,
				"version mismatch must return a correlated protocol error");
		}

		{
			TProtocolBuffer buffer;
			const auto response = RequireProtocolResponse(
				MakeRequest(
					EditorEngineProtocolVersion,
					0,
					c_getExitCodeCommandField),
				buffer);
			Require(
				response.m_protocolVersion == EditorEngineProtocolVersion &&
				response.m_requestId == 0 &&
				!response.m_success &&
				response.m_error.find("request_id") != std::string::npos &&
				response.m_resultField == 0,
				"zero request id must return a protocol error");
		}

		{
			TProtocolBuffer buffer;
			const auto response = RequireProtocolResponse(
				MakeRequest(EditorEngineProtocolVersion, 23),
				buffer);
			Require(
				response.m_requestId == 23 &&
				!response.m_success &&
				response.m_error.find("command") != std::string::npos &&
				response.m_resultField == 0,
				"missing command must return a correlated protocol error");
		}
	}

	void TestAnimatorParameterRequiresTypedValue()
	{
		Sailor::Protocol::TEditorEngineProtocolLifecycleGate gate;
		std::string admissionError;
		Require(
			gate.TryBeginInitialization(admissionError),
			"animator protocol fixture lifecycle must initialize");
		gate.CompleteInitialization(true);

		Sailor::Protocol::EditorEngineProtocolDependencies dependencies{};
		dependencies.m_lifecycleGate = &gate;
		std::string parameterRequest;
		AppendMessageField(parameterRequest, 1u, "Animator-1");
		AppendMessageField(parameterRequest, 2u, "Speed");

		TProtocolBuffer buffer;
		const auto response = RequireProtocolResponse(
			MakeRequest(
				EditorEngineProtocolVersion,
				27,
				c_setAnimatorParameterCommandField,
				parameterRequest),
			buffer,
			dependencies);
		Require(
			!response.m_success &&
				response.m_error.find("value is not set") != std::string::npos &&
				response.m_resultField == 0,
			"animator parameter mutations must carry exactly one typed value");
	}

	void TestStrictInstanceIdProtocolGate()
	{
		std::string strictInstantiateRequest;
		AppendVarintField(
			strictInstantiateRequest,
			3u,
			1u);

		{
			TProtocolBuffer buffer;
			const auto response = RequireProtocolResponse(
				MakeRequest(
					EditorEngineProtocolVersion,
					24,
					c_instantiatePrefabFromYamlCommandField,
					strictInstantiateRequest),
				buffer);
			Require(
				response.m_protocolVersion ==
					EditorEngineProtocolVersion &&
				response.m_requestId == 24 &&
				!response.m_success &&
				response.m_error.find("Strict instance-id") !=
					std::string::npos &&
				response.m_supportsStrictInstanceIds,
				"v1 strict restore must fail closed while advertising the compatible host capability");
		}

		{
			TProtocolBuffer buffer;
			const auto response = RequireProtocolResponse(
				MakeRequest(
					EditorEngineProtocolStrictInstanceIdsVersion,
					25,
					c_getExitCodeCommandField),
				buffer);
			Require(
				response.m_protocolVersion ==
					EditorEngineProtocolStrictInstanceIdsVersion &&
				response.m_requestId == 25 &&
				!response.m_success &&
				response.m_error.find("reserved") !=
					std::string::npos &&
				response.m_supportsStrictInstanceIds,
				"the strict protocol version must reject ordinary v1 commands");
		}

		{
			Sailor::Protocol::TEditorEngineProtocolLifecycleGate gate;
			std::string admissionError;
			Require(
				gate.TryBeginInitialization(admissionError),
				"strict protocol fixture lifecycle must initialize");
			gate.CompleteInitialization(true);
			Sailor::Protocol::EditorEngineProtocolDependencies
				dependencies{};
			dependencies.m_lifecycleGate = &gate;

			TProtocolBuffer buffer;
			const auto response = RequireProtocolResponse(
				MakeRequest(
					EditorEngineProtocolStrictInstanceIdsVersion,
					26,
					c_instantiatePrefabFromYamlCommandField,
					strictInstantiateRequest),
				buffer,
				dependencies);
			Require(
				response.m_protocolVersion ==
					EditorEngineProtocolStrictInstanceIdsVersion &&
				response.m_requestId == 26 &&
				response.m_success &&
				response.m_resultField == c_instanceIdResultField &&
				!response.m_boolResult &&
				response.m_supportsStrictInstanceIds,
				"a capability-compatible host must dispatch a version-gated strict restore request");
		}
	}

	void TestModelInstanceWireContract()
	{
		static_assert(
			sailor::editor::v1::ProtocolRequest::
				kCreateModelInstanceFieldNumber ==
			c_createModelInstanceCommandField);
		static_assert(
			sailor::editor::v1::CreateModelInstanceRequest::
				kModelFileIdFieldNumber == 1);
		static_assert(
			sailor::editor::v1::CreateModelInstanceRequest::
				kCreateHierarchyFieldNumber == 4);
		static_assert(
			sailor::editor::v1::CreateModelInstanceRequest::
				kWorldPositionFieldNumber == 6);
		static_assert(
			sailor::editor::v1::CreateModelInstanceRequest::
				kPreferredInstanceIdFieldNumber == 7);
	}

	void TestEditorSimulationWireContract()
	{
		static_assert(
			sailor::editor::v1::ProtocolRequest::
				kSetEditorSimulationFieldNumber ==
			c_setEditorSimulationCommandField);
		static_assert(
			sailor::editor::v1::ProtocolRequest::
				kGetEditorSimulationStateFieldNumber ==
			c_getEditorSimulationStateCommandField);
		static_assert(
			sailor::editor::v1::EditorSimulationRequest::
				kEnabledFieldNumber == 1);
	}

	void TestEditorStatsModeWireContract()
	{
		static_assert(
			sailor::editor::v1::ProtocolRequest::
				kSetEditorStatsModeFieldNumber ==
			c_setEditorStatsModeCommandField);
		static_assert(
			sailor::editor::v1::EditorStatsModeRequest::
				kModeFieldNumber == 1);
		static_assert(
			sailor::editor::v1::EDITOR_STATS_MODE_NONE == 1);
		static_assert(
			sailor::editor::v1::EDITOR_STATS_MODE_RENDER_STATS == 2);
		static_assert(
			sailor::editor::v1::
				EDITOR_STATS_MODE_RENDER_STATS_AND_QUERIES == 3);

		std::string invalidModeRequest;
		AppendVarintField(invalidModeRequest, 1u, 99u);
		Sailor::Protocol::TEditorEngineProtocolLifecycleGate gate;
		std::string admissionError;
		Require(
			gate.TryBeginInitialization(admissionError),
			"Stats mode protocol fixture lifecycle must initialize");
		gate.CompleteInitialization(true);
		Sailor::Protocol::EditorEngineProtocolDependencies dependencies{};
		dependencies.m_lifecycleGate = &gate;

		TProtocolBuffer buffer;
		const auto response = RequireProtocolResponse(
			MakeRequest(
				EditorEngineProtocolVersion,
				28,
				c_setEditorStatsModeCommandField,
				invalidModeRequest),
			buffer,
			dependencies);
		Require(
			response.m_requestId == 28 &&
			!response.m_success &&
			response.m_resultField == 0 &&
			response.m_error.find("stats mode") != std::string::npos,
			"an invalid Editor stats mode must fail without mutating runtime state");
	}

	void TestAudioPreviewWireContract()
	{
		static_assert(
			sailor::editor::v1::ProtocolRequest::
				kPreviewAudioAssetFieldNumber ==
			c_previewAudioAssetCommandField);
		static_assert(
			sailor::editor::v1::FileIdRequest::
				kFileIdFieldNumber == 1);
	}

	void TestEmbeddedNullIsRejected()
	{
		std::string fileIdRequest;
		const std::string fileIdWithNull("asset\0id", 8);
		AppendMessageField(fileIdRequest, 1u, fileIdWithNull);

		TProtocolBuffer buffer;
		const auto response = RequireProtocolResponse(
			MakeRequest(
				EditorEngineProtocolVersion,
				29,
				c_loadEditorWorldCommandField,
				fileIdRequest),
			buffer);
		Require(
			response.m_requestId == 29 &&
			!response.m_success &&
			response.m_error.find("NUL") != std::string::npos &&
			response.m_resultField == 0,
			"embedded NUL in a protobuf string must be rejected before App adaptation");
	}

	void TestUtf8StringIsAccepted()
	{
		std::string mutationRequest;
		AppendVarintField(mutationRequest, 1u, 2u);
		const std::string unicodeInstanceId =
			"Editor-" "\xd0\xa3\xd1\x82\xd0\xba\xd0\xb0";
		AppendMessageField(mutationRequest, 2u, unicodeInstanceId);

		Sailor::Protocol::TEditorEngineProtocolLifecycleGate gate;
		std::string admissionError;
		Require(
			gate.TryBeginInitialization(admissionError),
			"adapter test lifecycle must initialize");
		gate.CompleteInitialization(true);
		Sailor::Protocol::EditorEngineProtocolDependencies dependencies{};
		dependencies.m_lifecycleGate = &gate;

		TProtocolBuffer buffer;
		const auto response = RequireProtocolResponse(
			MakeRequest(
				EditorEngineProtocolVersion,
				30,
				c_getManagedMutationRevisionCommandField,
				mutationRequest),
			buffer,
			dependencies);
		Require(
			response.m_requestId == 30 &&
			response.m_success &&
			response.m_supportsStrictInstanceIds &&
			response.m_error.empty() &&
			response.m_resultField == c_uint64ResultField,
			"valid UTF-8 protobuf strings must pass native string validation");
	}

	void TestGetExitCodeRoundTripAndFree()
	{
		TProtocolBuffer buffer;
		const auto response = RequireProtocolResponse(
			MakeRequest(
				EditorEngineProtocolVersion,
				31,
				c_getExitCodeCommandField),
			buffer);
		Require(
			response.m_protocolVersion == EditorEngineProtocolVersion &&
			response.m_requestId == 31 &&
			response.m_success &&
			response.m_error.empty() &&
			response.m_resultField == c_int32ResultField &&
			response.m_int32Result == 0,
			"get-exit-code must round-trip through the exported C ABI");
	}

	struct TViewportEventSource
	{
		std::string m_events[2]{};
		uint32_t m_nextEvent = 0;
	};

	uint32_t PullViewportEvents(
		void* context,
		char** events,
		uint32_t capacity)
	{
		auto* source = static_cast<TViewportEventSource*>(context);
		if (!source || !events)
		{
			return 0;
		}

		uint32_t numEvents = 0;
		while (numEvents < capacity && source->m_nextEvent < 2)
		{
			const std::string& event = source->m_events[source->m_nextEvent++];
			events[numEvents] = new char[event.size() + 1];
			std::memcpy(events[numEvents], event.c_str(), event.size() + 1);
			++numEvents;
		}
		return numEvents;
	}

	void TestMalformedViewportEventDoesNotDiscardValidBatchEvents()
	{
		TViewportEventSource source{
			{
				"kind: [unterminated",
				"kind: selection\n"
				"revision: 42\n"
				"managedMutationRevision: 9\n"
				"selectedInstanceId: Duck-123\n"
			}
		};
		Sailor::Protocol::EditorEngineProtocolDependencies dependencies{};
		dependencies.m_context = &source;
		dependencies.m_pullEditorViewportEvents = PullViewportEvents;
		Sailor::Protocol::TEditorEngineProtocolLifecycleGate gate;
		std::string admissionError;
		Require(
			gate.TryBeginInitialization(admissionError),
			"viewport adapter test lifecycle must initialize");
		gate.CompleteInitialization(true);
		dependencies.m_lifecycleGate = &gate;

		std::string countRequest;
		AppendVarintField(countRequest, 1u, 2u);
		const std::string request = MakeRequest(
			EditorEngineProtocolVersion,
			41,
			c_pullEditorViewportEventsCommandField,
			countRequest);

		TProtocolBuffer buffer;
		Require(
			Invoke(request, buffer, dependencies) ==
				static_cast<int32_t>(EEditorEngineTransportStatus::Ok),
			"a malformed viewport event must not fail the protocol transport");

		const auto response = DecodeResponse(buffer.GetData(), buffer.GetSize());
		Require(
			response.m_protocolVersion == EditorEngineProtocolVersion &&
			response.m_requestId == 41 &&
			response.m_success &&
			response.m_error.empty() &&
			response.m_resultField == c_viewportEventBatchResultField,
			"viewport event response must report protocol success");

		uint32_t numEvents = 0;
		TDecodedViewportEvent event;
		Require(
			TryDecodeViewportEventBatch(
				response.m_resultPayload,
				numEvents,
				event) &&
			numEvents == 1,
			"only the valid event from the pulled batch must be returned");
		Require(
			event.m_revision == 42 &&
			event.m_managedMutationRevision == 9 &&
			event.m_hasSelection &&
			event.m_selectedInstanceId == "Duck-123",
			"the valid event following malformed YAML must be preserved");
	}

	void TestViewportAssetDropEventIsTypedAndValidated()
	{
		const std::string fileId =
			"{12345678-1234-1234-1234-123456789ABC}";
		TViewportEventSource source{
			{
				"kind: assetDrop\n"
				"revision: 43\n"
				"managedMutationRevision: 10\n"
				"fileId: \"" + fileId + "\"\n"
				"normalizedX: 0.25\n"
				"normalizedY: 0.75\n",
				"kind: assetDrop\n"
				"revision: 44\n"
				"managedMutationRevision: 10\n"
				"fileId: \"" + fileId + "\"\n"
				"normalizedX: 1.5\n"
				"normalizedY: 0.5\n"
			}
		};
		Sailor::Protocol::EditorEngineProtocolDependencies dependencies{};
		dependencies.m_context = &source;
		dependencies.m_pullEditorViewportEvents = PullViewportEvents;
		Sailor::Protocol::TEditorEngineProtocolLifecycleGate gate;
		std::string admissionError;
		Require(
			gate.TryBeginInitialization(admissionError),
			"asset-drop adapter test lifecycle must initialize");
		gate.CompleteInitialization(true);
		dependencies.m_lifecycleGate = &gate;

		std::string countRequest;
		AppendVarintField(countRequest, 1u, 2u);
		TProtocolBuffer buffer;
		const auto response = RequireProtocolResponse(
			MakeRequest(
				EditorEngineProtocolVersion,
				42,
				c_pullEditorViewportEventsCommandField,
				countRequest),
			buffer,
			dependencies);
		Require(
			response.m_success &&
				response.m_resultField ==
					c_viewportEventBatchResultField,
			"viewport asset-drop response must report protocol success");

		uint32_t numEvents = 0;
		TDecodedViewportEvent event;
		Require(
			TryDecodeViewportEventBatch(
				response.m_resultPayload,
				numEvents,
				event) &&
				numEvents == 1,
			"invalid normalized asset-drop events must be filtered");
		Require(
			event.m_revision == 43 &&
				event.m_managedMutationRevision == 10 &&
				event.m_hasAssetDrop &&
				event.m_assetFileId == fileId &&
				std::abs(event.m_normalizedX - 0.25f) < 0.0001f &&
				std::abs(event.m_normalizedY - 0.75f) < 0.0001f,
			"valid asset-drop data must survive native typed conversion");
	}

	void TestViewportToolShortcutEventIsTypedAndValidated()
	{
		TViewportEventSource source{
			{
				"kind: toolShortcut\n"
				"revision: 45\n"
				"managedMutationRevision: 11\n"
				"keyCode: 87\n",
				"kind: toolShortcut\n"
				"revision: 46\n"
				"managedMutationRevision: 11\n"
				"keyCode: 88\n"
			}
		};
		Sailor::Protocol::EditorEngineProtocolDependencies dependencies{};
		dependencies.m_context = &source;
		dependencies.m_pullEditorViewportEvents = PullViewportEvents;
		Sailor::Protocol::TEditorEngineProtocolLifecycleGate gate;
		std::string admissionError;
		Require(
			gate.TryBeginInitialization(admissionError),
			"tool-shortcut adapter test lifecycle must initialize");
		gate.CompleteInitialization(true);
		dependencies.m_lifecycleGate = &gate;

		std::string countRequest;
		AppendVarintField(countRequest, 1u, 2u);
		TProtocolBuffer buffer;
		const auto response = RequireProtocolResponse(
			MakeRequest(
				EditorEngineProtocolVersion,
				43,
				c_pullEditorViewportEventsCommandField,
				countRequest),
			buffer,
			dependencies);
		Require(
			response.m_success &&
				response.m_resultField ==
					c_viewportEventBatchResultField,
			"viewport tool-shortcut response must report protocol success");

		uint32_t numEvents = 0;
		TDecodedViewportEvent event;
		Require(
			TryDecodeViewportEventBatch(
				response.m_resultPayload,
				numEvents,
				event) &&
				numEvents == 1,
			"unsupported viewport shortcut keys must be filtered");
		Require(
			event.m_revision == 45 &&
				event.m_managedMutationRevision == 11 &&
				event.m_hasToolShortcut &&
				event.m_toolShortcutKeyCode == 'W',
			"valid viewport shortcuts must survive native typed conversion");
	}

	void TestLifecycleGateDrainsStartAndOperationsBeforeShutdown()
	{
		using namespace std::chrono_literals;

		Sailor::Protocol::TEditorEngineProtocolLifecycleGate gate;
		std::string error;
		Require(
			gate.TryAcquireOperation(error, true),
			"idle protocol gate must admit explicit diagnostics");
		gate.ReleaseOperation();
		Require(
			!gate.TryAcquireOperation(error, false),
			"idle protocol gate must reject engine mutations");

		Require(
			gate.TryBeginInitialization(error),
			"first initialization must be admitted");
		Require(
			!gate.TryAcquireOperation(error, true) &&
				!gate.TryBeginInitialization(error),
			"initialization must be exclusive");
		gate.CompleteInitialization(true);
		Require(
			!gate.TryBeginInitialization(error),
			"completed initialization must reject duplicates");

		Require(
			gate.TryBeginStart(error),
			"initialized lifecycle must admit one Start");
		Require(
			!gate.TryBeginStart(error),
			"active Start must reject duplicates");
		Require(
			gate.TryAcquireOperation(error, false),
			"regular operations must remain concurrent with Start");
		Require(
			gate.TryBeginShutdown(error),
			"Shutdown must close admission while Start is active");
		Require(
			!gate.TryAcquireOperation(error, true) &&
				!gate.TryBeginShutdown(error),
			"closed shutdown admission must reject new work and duplicates");

		auto drain = std::async(std::launch::async, [&gate]()
			{
				gate.WaitForShutdownDrain();
			});
		Require(
			drain.wait_for(20ms) == std::future_status::timeout,
			"Shutdown must wait for both Start and regular operation leases");
		gate.ReleaseOperation();
		Require(
			drain.wait_for(20ms) == std::future_status::timeout,
			"Shutdown must continue waiting while Start is active");
		gate.CompleteStart();
		Require(
			drain.wait_for(1s) == std::future_status::ready,
			"Shutdown must continue after Start and operations drain");
		drain.get();
		gate.CompleteShutdown();
		Require(
			!gate.TryAcquireOperation(error, true),
			"completed Shutdown must keep the old session closed");

		Require(
			gate.TryBeginInitialization(error),
			"a new initialization must reopen a completed session");
		gate.CompleteInitialization(true);
		Require(
			gate.TryBeginStart(error),
			"a new session must not inherit the previous session's Stop");
		gate.CompleteStart();

		Sailor::Protocol::TEditorEngineProtocolLifecycleGate stoppedGate;
		Require(
			stoppedGate.TryBeginInitialization(error),
			"fresh gate must admit initialization");
		stoppedGate.CompleteInitialization(true);
		stoppedGate.NoteStopRequested();
		Require(
			!stoppedGate.TryBeginStart(error),
			"Stop before Start must prevent a late Start race");

		Sailor::Protocol::TEditorEngineProtocolLifecycleGate initializingGate;
		Require(
			initializingGate.TryBeginInitialization(error),
			"fresh gate must admit initialization");
		Require(
			!initializingGate.NoteStopRequested(),
			"Stop during initialization must not enter partially built App state");
		initializingGate.CompleteInitialization(true);
		Require(
			!initializingGate.TryBeginStart(error),
			"Stop during initialization must prevent the subsequent Start");

		Sailor::Protocol::TEditorEngineProtocolLifecycleGate shutdownInitGate;
		Require(
			shutdownInitGate.TryBeginInitialization(error),
			"shutdown race gate must admit initialization");
		Require(
			shutdownInitGate.TryBeginShutdown(error),
			"Shutdown must close admission while Initialize is active");
		auto initializationDrain = std::async(
			std::launch::async,
			[&shutdownInitGate]()
			{
				shutdownInitGate.WaitForInitializationDrain();
			});
		Require(
			initializationDrain.wait_for(20ms) == std::future_status::timeout,
			"Shutdown must wait for the active Initialize owner");
		shutdownInitGate.CompleteInitialization(true);
		Require(
			initializationDrain.wait_for(1s) == std::future_status::ready,
			"Shutdown must continue when Initialize completes");
		initializationDrain.get();
		shutdownInitGate.WaitForShutdownDrain();
		shutdownInitGate.CompleteShutdown();
	}

	struct TBlockingLifecycleSource
	{
		std::mutex m_mutex{};
		std::condition_variable m_condition{};
		bool m_bStartEntered = false;
		bool m_bReleaseStart = false;
		bool m_bStartExited = false;
		bool m_bShutdownObservedStartExit = false;
		uint32_t m_numStops = 0;
		uint32_t m_numShutdowns = 0;
	};

	void BlockingStart(void* context)
	{
		auto& source =
			*static_cast<TBlockingLifecycleSource*>(context);
		std::unique_lock<std::mutex> lock(source.m_mutex);
		source.m_bStartEntered = true;
		source.m_condition.notify_all();
		source.m_condition.wait(lock, [&source]()
			{
				return source.m_bReleaseStart;
			});
		source.m_bStartExited = true;
		source.m_condition.notify_all();
	}

	void ReleaseBlockingStart(void* context)
	{
		auto& source =
			*static_cast<TBlockingLifecycleSource*>(context);
		{
			const std::lock_guard<std::mutex> lock(source.m_mutex);
			++source.m_numStops;
			source.m_bReleaseStart = true;
		}
		source.m_condition.notify_all();
	}

	void RecordShutdown(void* context)
	{
		auto& source =
			*static_cast<TBlockingLifecycleSource*>(context);
		const std::lock_guard<std::mutex> lock(source.m_mutex);
		++source.m_numShutdowns;
		source.m_bShutdownObservedStartExit = source.m_bStartExited;
	}

	class TBlockingLifecycleRelease final
	{
	public:
		explicit TBlockingLifecycleRelease(
			TBlockingLifecycleSource& source)
			: m_source(source)
		{
		}

		~TBlockingLifecycleRelease()
		{
			{
				const std::lock_guard<std::mutex> lock(m_source.m_mutex);
				m_source.m_bReleaseStart = true;
			}
			m_source.m_condition.notify_all();
		}

	private:
		TBlockingLifecycleSource& m_source;
	};

	Sailor::Protocol::EditorEngineProtocolDependencies
		MakeBlockingLifecycleDependencies(
			Sailor::Protocol::TEditorEngineProtocolLifecycleGate& gate,
			TBlockingLifecycleSource& source)
	{
		Sailor::Protocol::EditorEngineProtocolDependencies dependencies{};
		dependencies.m_context = &source;
		dependencies.m_start = BlockingStart;
		dependencies.m_stop = ReleaseBlockingStart;
		dependencies.m_shutdown = RecordShutdown;
		dependencies.m_lifecycleGate = &gate;
		return dependencies;
	}

	void PrepareInitializedLifecycle(
		Sailor::Protocol::TEditorEngineProtocolLifecycleGate& gate)
	{
		std::string error;
		Require(
			gate.TryBeginInitialization(error),
			"async lifecycle test must initialize its session");
		gate.CompleteInitialization(true);
	}

	TDecodedResponse InvokeStartPromptly(
		const uint64_t requestId,
		const Sailor::Protocol::EditorEngineProtocolDependencies& dependencies,
		TBlockingLifecycleSource& source)
	{
		using namespace std::chrono_literals;

		auto invocation = std::async(
			std::launch::async,
			[requestId, &dependencies]()
			{
				TProtocolBuffer buffer;
				return RequireProtocolResponse(
					MakeRequest(
						EditorEngineProtocolVersion,
						requestId,
						c_startCommandField),
					buffer,
					dependencies);
			});
		if (invocation.wait_for(500ms) != std::future_status::ready)
		{
			{
				const std::lock_guard<std::mutex> lock(source.m_mutex);
				source.m_bReleaseStart = true;
			}
			source.m_condition.notify_all();
			invocation.wait();
			Require(
				false,
				"Start acknowledgement must not wait for the Engine loop to exit");
		}
		return invocation.get();
	}

	void TestStartAcknowledgesBeforeWorkerExitAndStopJoins()
	{
		using namespace std::chrono_literals;

		TBlockingLifecycleSource source;
		Sailor::Protocol::TEditorEngineProtocolLifecycleGate gate;
		const TBlockingLifecycleRelease release(source);
		PrepareInitializedLifecycle(gate);
		const auto dependencies =
			MakeBlockingLifecycleDependencies(gate, source);

		const auto startResponse =
			InvokeStartPromptly(51, dependencies, source);
		Require(
			startResponse.m_success &&
				startResponse.m_resultField == c_emptyResultField,
			"Start must acknowledge an admitted async worker");

		{
			std::unique_lock<std::mutex> lock(source.m_mutex);
			Require(
				source.m_condition.wait_for(
					lock,
					1s,
					[&source]()
					{
						return source.m_bStartEntered;
					}),
				"the admitted Start worker must begin execution");
			Require(
				!source.m_bStartExited,
				"the Start worker must still be blocked after its acknowledgement");
		}

		TProtocolBuffer livenessBuffer;
		const auto livenessResponse = RequireProtocolResponse(
			MakeRequest(
				EditorEngineProtocolVersion,
				58,
				c_isEngineRunningCommandField),
			livenessBuffer,
			dependencies);
		Require(
			livenessResponse.m_success &&
				livenessResponse.m_resultField == c_boolResultField &&
				livenessResponse.m_boolResult,
			"lifecycle probe must report the admitted Start worker as running");

		TProtocolBuffer duplicateBuffer;
		const auto duplicateResponse = RequireProtocolResponse(
			MakeRequest(
				EditorEngineProtocolVersion,
				52,
				c_startCommandField),
			duplicateBuffer,
			dependencies);
		Require(
			!duplicateResponse.m_success,
			"an active async Start must still reject duplicate starts");

		TProtocolBuffer stopBuffer;
		const auto stopResponse = RequireProtocolResponse(
			MakeRequest(
				EditorEngineProtocolVersion,
				53,
				c_stopCommandField),
			stopBuffer,
			dependencies);
		Require(
			stopResponse.m_success &&
				stopResponse.m_resultField == c_emptyResultField,
			"Stop must acknowledge after releasing and joining the Start worker");
		{
			const std::lock_guard<std::mutex> lock(source.m_mutex);
			Require(
				source.m_bStartExited && source.m_numStops == 1,
				"Stop must not return before the Start worker exits");
		}

		TProtocolBuffer stoppedLivenessBuffer;
		const auto stoppedLivenessResponse = RequireProtocolResponse(
			MakeRequest(
				EditorEngineProtocolVersion,
				59,
				c_isEngineRunningCommandField),
			stoppedLivenessBuffer,
			dependencies);
		Require(
			stoppedLivenessResponse.m_success &&
				stoppedLivenessResponse.m_resultField == c_boolResultField &&
				!stoppedLivenessResponse.m_boolResult,
			"lifecycle probe must report a joined Start worker as stopped");
	}

	void TestImmediateStopAfterStartAcknowledgementCannotBeLost()
	{
		TBlockingLifecycleSource source;
		Sailor::Protocol::TEditorEngineProtocolLifecycleGate gate;
		const TBlockingLifecycleRelease release(source);
		PrepareInitializedLifecycle(gate);
		const auto dependencies =
			MakeBlockingLifecycleDependencies(gate, source);

		const auto startResponse =
			InvokeStartPromptly(56, dependencies, source);
		Require(
			startResponse.m_success,
			"immediate Stop test must receive the Start acknowledgement");

		// Do not wait for BlockingStart to enter. Stop must publish its request
		// before or after the worker reaches the routine without losing it.
		TProtocolBuffer stopBuffer;
		const auto stopResponse = RequireProtocolResponse(
			MakeRequest(
				EditorEngineProtocolVersion,
				57,
				c_stopCommandField),
			stopBuffer,
			dependencies);
		Require(
			stopResponse.m_success,
			"immediate Stop must release and join the admitted Start worker");
		{
			const std::lock_guard<std::mutex> lock(source.m_mutex);
			Require(
				source.m_bStartEntered &&
					source.m_bStartExited &&
					source.m_numStops == 1,
				"Stop immediately after ACK must not be lost before Start enters");
		}
	}

	void TestShutdownStopsAndJoinsWorkerBeforeShutdownRoutine()
	{
		using namespace std::chrono_literals;

		TBlockingLifecycleSource source;
		Sailor::Protocol::TEditorEngineProtocolLifecycleGate gate;
		const TBlockingLifecycleRelease release(source);
		PrepareInitializedLifecycle(gate);
		const auto dependencies =
			MakeBlockingLifecycleDependencies(gate, source);

		const auto startResponse =
			InvokeStartPromptly(54, dependencies, source);
		Require(
			startResponse.m_success,
			"Shutdown ordering test must admit Start");
		{
			std::unique_lock<std::mutex> lock(source.m_mutex);
			Require(
				source.m_condition.wait_for(
					lock,
					1s,
					[&source]()
					{
						return source.m_bStartEntered;
					}),
				"Shutdown ordering test Start worker must begin");
		}

		TProtocolBuffer shutdownBuffer;
		const auto shutdownResponse = RequireProtocolResponse(
			MakeRequest(
				EditorEngineProtocolVersion,
				55,
				c_shutdownCommandField),
			shutdownBuffer,
			dependencies);
		Require(
			shutdownResponse.m_success &&
				shutdownResponse.m_resultField == c_emptyResultField,
			"Shutdown must complete after draining async lifecycle work");
		{
			const std::lock_guard<std::mutex> lock(source.m_mutex);
			Require(
				source.m_numStops == 1 &&
					source.m_numShutdowns == 1 &&
					source.m_bShutdownObservedStartExit,
				"Shutdown must Stop and join Start before invoking App shutdown");
		}
	}

	struct TBlockingEditorDispatchSource
	{
		std::mutex m_mutex{};
		std::condition_variable m_condition{};
		bool m_bDispatchEntered = false;
		bool m_bReleaseDispatch = false;
		bool m_bOperationExecuted = false;
		uint32_t m_numDispatches = 0;
		uint32_t m_numStops = 0;
		uint32_t m_numShutdowns = 0;
	};

	bool DispatchBlockingEditorOperation(
		void* context,
		Sailor::Protocol::EditorEngineProtocolDependencies::
			FEditorEngineProtocolOperation operation,
		void* operationContext)
	{
		auto& source =
			*static_cast<TBlockingEditorDispatchSource*>(context);
		{
			std::unique_lock<std::mutex> lock(source.m_mutex);
			++source.m_numDispatches;
			source.m_bDispatchEntered = true;
			source.m_condition.notify_all();
			source.m_condition.wait(lock, [&source]()
				{
					return source.m_bReleaseDispatch;
				});
		}

		operation(operationContext);
		{
			const std::lock_guard<std::mutex> lock(source.m_mutex);
			source.m_bOperationExecuted = true;
		}
		source.m_condition.notify_all();
		return true;
	}

	void ReleaseBlockingEditorOperation(void* context)
	{
		auto& source =
			*static_cast<TBlockingEditorDispatchSource*>(context);
		{
			const std::lock_guard<std::mutex> lock(source.m_mutex);
			++source.m_numStops;
			source.m_bReleaseDispatch = true;
		}
		source.m_condition.notify_all();
	}

	void RecordEditorDispatchShutdown(void* context)
	{
		auto& source =
			*static_cast<TBlockingEditorDispatchSource*>(context);
		const std::lock_guard<std::mutex> lock(source.m_mutex);
		++source.m_numShutdowns;
	}

	struct TEditorExceptionSource
	{
		std::thread::id m_dispatchThreadId{};
	};

	bool DispatchEditorOperationOnTestThread(
		void* context,
		Sailor::Protocol::EditorEngineProtocolDependencies::
			FEditorEngineProtocolOperation operation,
		void* operationContext)
	{
		auto& source = *static_cast<TEditorExceptionSource*>(context);
		std::thread editorThread(
			[&source, operation, operationContext]()
			{
				source.m_dispatchThreadId = std::this_thread::get_id();
				operation(operationContext);
			});
		editorThread.join();
		return true;
	}

	uint32_t ThrowFromEditorOperation(void*, char**, uint32_t)
	{
		throw std::runtime_error("test Editor worker failure");
	}

	void TestEditorWorkerExceptionIsRethrownOnInvoker()
	{
		using namespace std::chrono_literals;

		TEditorExceptionSource source;
		Sailor::Protocol::TEditorEngineProtocolLifecycleGate gate;
		PrepareInitializedLifecycle(gate);

		Sailor::Protocol::EditorEngineProtocolDependencies dependencies{};
		dependencies.m_context = &source;
		dependencies.m_pullEditorViewportEvents = ThrowFromEditorOperation;
		dependencies.m_lifecycleGate = &gate;
		dependencies.m_editorDispatchContext = &source;
		dependencies.m_dispatchEditorOperation =
			DispatchEditorOperationOnTestThread;

		std::string countRequest;
		AppendVarintField(countRequest, 1u, 1u);
		TProtocolBuffer buffer;
		bool bExceptionRethrown = false;
		try
		{
			Invoke(
				MakeRequest(
					EditorEngineProtocolVersion,
					60,
					c_pullEditorViewportEventsCommandField,
					countRequest),
				buffer,
				dependencies);
		}
		catch (const std::runtime_error& exception)
		{
			bExceptionRethrown =
				std::string(exception.what()) ==
					"test Editor worker failure";
		}

		Require(
			source.m_dispatchThreadId != std::thread::id{} &&
				source.m_dispatchThreadId != std::this_thread::get_id(),
			"regular protocol operation must execute on the dispatched thread");
		Require(
			bExceptionRethrown,
			"Editor worker exception must be rethrown on the protocol invoker");

		std::string shutdownError;
		Require(
			gate.TryBeginShutdown(shutdownError),
			"Editor worker exception must release its lifecycle operation lease");
		auto drain = std::async(
			std::launch::async,
			[&gate]()
			{
				gate.WaitForShutdownDrain();
			});
		Require(
			drain.wait_for(1s) == std::future_status::ready,
			"shutdown must not remain blocked by a failed Editor operation");
		drain.get();
		gate.CompleteShutdown();
	}

	void TestPathTracingDoesNotBlockLifecycleDispatch()
	{
		using namespace std::chrono_literals;

		TBlockingEditorDispatchSource source;
		Sailor::Protocol::TEditorEngineProtocolLifecycleGate gate;
		PrepareInitializedLifecycle(gate);

		Sailor::Protocol::EditorEngineProtocolDependencies dependencies{};
		dependencies.m_context = &source;
		dependencies.m_stop = ReleaseBlockingEditorOperation;
		dependencies.m_shutdown = RecordEditorDispatchShutdown;
		dependencies.m_lifecycleGate = &gate;
		dependencies.m_editorDispatchContext = &source;
		dependencies.m_dispatchEditorOperation =
			DispatchBlockingEditorOperation;

		std::string renderRequest;
		AppendMessageField(renderRequest, 1u, "PathTrace.png");
		auto render = std::async(
			std::launch::async,
			[&dependencies, &renderRequest]()
			{
				TProtocolBuffer buffer;
				return RequireProtocolResponse(
					MakeRequest(
						EditorEngineProtocolVersion,
						61,
						c_renderPathTracedImageCommandField,
						renderRequest),
					buffer,
					dependencies);
			});

		{
			std::unique_lock<std::mutex> lock(source.m_mutex);
			Require(
				source.m_condition.wait_for(
					lock,
					1s,
					[&source]()
					{
						return source.m_bDispatchEntered;
					}),
				"path tracing must enter the dedicated Editor dispatcher");
		}

		auto shutdown = std::async(
			std::launch::async,
			[&dependencies]()
			{
				TProtocolBuffer buffer;
				return RequireProtocolResponse(
					MakeRequest(
						EditorEngineProtocolVersion,
						62,
						c_shutdownCommandField),
					buffer,
					dependencies);
			});

		if (shutdown.wait_for(1s) != std::future_status::ready)
		{
			{
				const std::lock_guard<std::mutex> lock(source.m_mutex);
				source.m_bReleaseDispatch = true;
			}
			source.m_condition.notify_all();
		}

		const auto shutdownResponse = shutdown.get();
		const auto renderResponse = render.get();
		Require(
			shutdownResponse.m_success &&
				shutdownResponse.m_resultField == c_emptyResultField,
			"Shutdown must bypass the Editor worker and drain it safely");
		Require(
			renderResponse.m_success &&
				renderResponse.m_resultField == c_boolResultField,
			"path tracing must complete its regular Editor operation response");
		{
			const std::lock_guard<std::mutex> lock(source.m_mutex);
			Require(
				source.m_numDispatches == 1 &&
					source.m_numStops == 1 &&
					source.m_numShutdowns == 1 &&
					source.m_bOperationExecuted,
				"lifecycle must interrupt and drain path tracing without entering the Editor dispatcher");
		}
	}
}

int main()
{
	try
	{
		TestInvalidArgumentsResetOutputs();
		TestOversizedAndMalformedPayloads();
		TestCommandExceptionIsContainedByTransportBoundary();
		TestEnvelopeValidation();
		TestAnimatorParameterRequiresTypedValue();
		TestStrictInstanceIdProtocolGate();
		TestModelInstanceWireContract();
		TestEditorSimulationWireContract();
		TestEditorStatsModeWireContract();
		TestAudioPreviewWireContract();
		TestEmbeddedNullIsRejected();
		TestUtf8StringIsAccepted();
		TestGetExitCodeRoundTripAndFree();
		TestMalformedViewportEventDoesNotDiscardValidBatchEvents();
		TestViewportAssetDropEventIsTypedAndValidated();
		TestViewportToolShortcutEventIsTypedAndValidated();
		TestLifecycleGateDrainsStartAndOperationsBeforeShutdown();
		TestStartAcknowledgesBeforeWorkerExitAndStopJoins();
		TestImmediateStopAfterStartAcknowledgementCannotBeLost();
		TestShutdownStopsAndJoinsWorkerBeforeShutdownRoutine();
		TestEditorWorkerExceptionIsRethrownOnInvoker();
		TestPathTracingDoesNotBlockLifecycleDispatch();
	}
	catch (const std::exception& exception)
	{
		std::cerr << "[FAIL] EditorEngineProtocolTests: " << exception.what() << std::endl;
		return 1;
	}

	std::cout << "[PASS] EditorEngineProtocolTests" << std::endl;
	return 0;
}
