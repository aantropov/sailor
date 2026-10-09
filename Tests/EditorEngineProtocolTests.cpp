#include "EditorEngineProtocolInternal.h"
#include "EditorEngineProtocolLifecycle.h"
#include "Protocol/Generated/editor_engine.pb.h"
#include "Sailor.h"
#include "Editor/EditorScene.h"
#include "AssetRegistry/Animation/AnimationController.h"
#include "Editor/EditorViewportEvent.h"
#include "Editor/EditorViewportController.h"
#include "AssetRegistry/Model/ModelImporter.h"
#include "Support/TempDirectory.h"
#include "Support/EditorProtocolWire.h"
#include "Support/ScopeExit.h"
#include "Support/TaskTestApp.h"
#include "Tasks/Tasks.h"
#include "Workspace/WorkspacePathEncoding.h"

#include <atomic>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <condition_variable>
#include <future>
#include <fstream>
#include <iostream>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

extern "C"
{
	SAILOR_SHARED_API int32_t SailorProtocolInvoke(
		const uint8_t* requestData,
		uint32_t requestSize,
		uint8_t** responseData,
		uint32_t* responseSize) noexcept;

	SAILOR_SHARED_API void SailorProtocolFreeBuffer(uint8_t* buffer) noexcept;
	SAILOR_SHARED_API int32_t SailorProtocolStopLocalHost(bool bShutdownEngine) noexcept;
}

namespace
{
	using Sailor::Protocol::EEditorEngineTransportStatus;
	using Sailor::Protocol::EditorEngineProtocolMaxPayloadSize;
	using Sailor::Protocol::EditorEngineProtocolVersion;

	using namespace Sailor::Tests::ProtocolWire;
	using TDecodedResponse = TProtocolResponseWire;

	constexpr uint32_t c_boolResultField = 11;
	constexpr uint32_t c_int32ResultField = 12;
	constexpr uint32_t c_uint64ResultField = 14;
	constexpr uint32_t c_instanceIdResultField = 18;
	constexpr uint32_t c_viewportEventBatchResultField = 19;
	constexpr uint32_t c_viewportToolStateResultField = 21;
	constexpr uint32_t c_animatorStateResultField = 22;
	constexpr uint32_t c_editorRenderModeResultField = 23;
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
	constexpr uint32_t c_setEditorRenderModeCommandField = 65;
	constexpr uint32_t c_getEditorRenderModeCommandField = 66;
	constexpr uint32_t c_setRuntimeGIProbesPreviewBudgetCommandField = 76;

	void Require(bool condition, std::string_view message)
	{
		if (!condition)
		{
			throw std::runtime_error(std::string(message));
		}
	}

	TDecodedResponse DecodeResponse(const uint8_t* data, size_t size)
	{
		Require(data != nullptr && size > 0, "protocol response must not be empty");
		TDecodedResponse response;
		Require(ParseResponse(std::string_view(reinterpret_cast<const char*>(data), size), response),
			"response must contain valid protobuf envelope fields");
		return response;
	}

	uint64_t ReadResult(const TDecodedResponse& response)
	{
		uint64_t value = 0;
		Require(ReadNestedScalar(response.m_resultPayload, value), "result must contain a valid scalar payload");
		return value;
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
		std::string_view value;
		if (!ReadBytes(std::string_view(reinterpret_cast<const char*>(data), size), offset, value))
		{
			return false;
		}
		outData = reinterpret_cast<const uint8_t*>(value.data());
		outSize = value.size();
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

	struct TDecodedViewportTransform
	{
		std::string m_instanceId;
		uint64_t m_operation = 0;
		uint64_t m_space = 0;
		std::array<glm::vec4, 6> m_vectors{};
	};

	bool TryDecodeVector4(const uint8_t* data, size_t size, glm::vec4& value)
	{
		size_t offset = 0;
		while (offset < size)
		{
			uint64_t key;
			if (!ReadVarint(data, size, offset, key)) return false;
			const auto field = key >> 3u;
			const auto wire = key & 7u;
			if (field >= 1 && field <= 4 && wire == 5)
			{
				uint32_t bits;
				if (!TryReadFixed32(data, size, offset, bits)) return false;
				std::memcpy(&value[static_cast<int>(field - 1)], &bits, sizeof(float));
			}
			else if (!SkipField(data, size, offset, static_cast<uint32_t>(wire))) return false;
		}
		return true;
	}

	bool TryDecodeViewportTransform(const uint8_t* data, size_t size, TDecodedViewportTransform& value)
	{
		size_t offset = 0;
		while (offset < size)
		{
			uint64_t key;
			if (!ReadVarint(data, size, offset, key)) return false;
			const auto field = key >> 3u;
			const auto wire = key & 7u;
			if ((field == 2 || field == 3) && wire == 0)
			{
				if (!ReadVarint(data, size, offset, field == 2 ? value.m_operation : value.m_space)) return false;
			}
			else if ((field == 1 || (field >= 4 && field <= 9)) && wire == 2)
			{
				const uint8_t* bytes;
				size_t length;
				if (!TryReadLengthDelimited(data, size, offset, bytes, length)) return false;
				if (field == 1) value.m_instanceId.assign(reinterpret_cast<const char*>(bytes), length);
				else if (!TryDecodeVector4(bytes, length, value.m_vectors[field - 4])) return false;
			}
			else if (!SkipField(data, size, offset, static_cast<uint32_t>(wire))) return false;
		}
		return true;
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
		bool m_bHasTransform = false;
		TDecodedViewportTransform m_transform;
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
			if (fieldNumber == 11u)
			{
				const uint8_t* transform;
				size_t length;
				if (wireType != 2u || !TryReadLengthDelimited(data, size, offset, transform, length) ||
					!TryDecodeViewportTransform(transform, length, outEvent.m_transform)) return false;
				outEvent.m_bHasTransform = true;
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

	void TestGoldenProtocolWire()
	{
		using namespace std::literals;
		// Literal bytes are independent of the shared encoder. The managed tests
		// parse and serialize the same version-one cases with generated messages.
		constexpr auto request = "\x08\x01\x10\x96\x01\x82\x01\x00"sv;
		Require(MakeVersionedRequest(1, 150, c_getExitCodeCommandField) == request &&
			MakeRequest(150, c_getExitCodeCommandField) == request,
			"get-exit-code encoding must match the golden request with a multibyte ID");
		TProtocolBuffer buffer;
		Require(Invoke(std::string(request), buffer) == static_cast<int32_t>(EEditorEngineTransportStatus::Ok),
			"the native generated decoder must accept the golden request");
		Require(std::string_view(reinterpret_cast<const char*>(buffer.GetData()), buffer.GetSize()) ==
			"\x08\x01\x10\x96\x01\x18\x01\x28\x01\x62\x00"sv,
			"the native generated serializer must emit the golden zero exit-code response");

		constexpr std::pair<std::string_view, uint32_t> results[] = {
			{ "\x52\x00"sv, 10 }, { "\x5a\x00"sv, 11 }, { "\x62\x00"sv, 12 },
			{ "\x6a\x00"sv, 13 }, { "\x72\x00"sv, 14 }, { "\x7a\x00"sv, 15 },
			{ "\x82\x01\x00"sv, 16 }, { "\x8a\x01\x00"sv, 17 }, { "\x92\x01\x00"sv, 18 },
			{ "\x9a\x01\x00"sv, 19 }, { "\xa2\x01\x00"sv, 20 }, { "\xaa\x01\x00"sv, 21 },
			{ "\xb2\x01\x00"sv, 22 }, { "\xba\x01\x00"sv, 23 }, { "\xc2\x01\x00"sv, 24 },
			{ "\xca\x01\x00"sv, 25 }, { "\xa2\x06\x00"sv, 100 }
		};
		TDecodedResponse response;
		for (const auto& [bytes, field] : results)
		{
			Require(ParseResponse(bytes, response) && response.m_resultField == field && response.m_resultPayload.empty(),
				"the wire reader must retain the exact result kind, including an empty oneof payload");
		}

		constexpr auto negative = "\x08\x01\x10\x96\x01\x18\x01\x28\x01\x62\x0b\x08\xfd\xff\xff\xff\xff\xff\xff\xff\xff\x01"sv;
		Require(ParseResponse(negative, response) && response.m_protocolVersion == 1 && response.m_requestId == 150 &&
			response.m_bSuccess && response.m_bSupportsStrictInstanceIds && response.m_resultField == 12 &&
			static_cast<int32_t>(ReadResult(response)) == -3,
			"negative int32 results must retain protobuf's ten-byte signed varint encoding");

		std::string owned("\x08\x01\x10\x96\x01\x18\x01\x28\x01\x5a\x02\x08\x01"sv);
		Require(ParseResponse(owned, response), "the golden bool response must decode");
		owned.assign(owned.size(), '\0');
		Require(response.m_resultField == 11 && ReadResult(response) == 1,
			"decoded results must own their bytes after the input buffer is overwritten");
		owned = "\x08\x01\x10\x96\x01\x22\x03" "bad";
		Require(ParseResponse(owned, response), "the golden error response must decode");
		owned.clear();
		Require(response.m_error == "bad" && response.m_requestId == 150 && !response.m_bSuccess &&
			!response.m_bSupportsStrictInstanceIds && response.m_resultField == 0 && response.m_resultPayload.empty(),
			"reusing the wire reader must clear the prior result and retain its own error text");

		for (auto bytes : { "\x80"sv, "\x10\x80"sv, "\x5a\x02\x08"sv, "\x5a\x80"sv, "\x0d\0\0\0\0"sv })
		{
			Require(!ParseResponse(bytes, response), "truncated fields and unsupported envelope wire types must fail");
		}
		std::cout << "Protocol wire: golden request/native response, 17 result kinds, signed scalar and owned payloads passed\n";
	}

	void TestViewportEvidenceRequestReportsNoFrame()
	{
		Sailor::Protocol::TEditorEngineProtocolLifecycleGate gate;
		std::string error;
		Require(gate.TryBeginInitialization(error), "capture protocol test must initialize its lifecycle");
		gate.CompleteInitialization(true);
		Sailor::Protocol::EditorEngineProtocolDependencies dependencies;
		dependencies.m_lifecycleGate = &gate;
		std::string viewport;
		AppendVarintField(viewport, 1u, 1u);
		TProtocolBuffer buffer;
		const auto response = RequireProtocolResponse(MakeVersionedRequest(1u, 151,
			sailor::editor::v1::ProtocolRequest::kCaptureRemoteViewportFrameEvidence, viewport), buffer, dependencies);
		Require(!response.m_bSuccess && response.m_requestId == 151 && response.m_resultField == 0,
			"capture without a viewport must return a correlated request failure, not empty successful evidence");
#if defined(__APPLE__)
		Require(response.m_error == "Viewport does not exist.", "capture command must reach the native viewport handler");
#else
		Require(response.m_error == "Viewport pixel evidence is only available on macOS.", "other platforms must report unsupported capture");
#endif
		TProtocolBuffer diagnostics;
		Require(RequireProtocolResponse(MakeVersionedRequest(1u, 152,
			sailor::editor::v1::ProtocolRequest::kGetRemoteViewportDiagnostics, viewport), diagnostics, dependencies).m_bSuccess,
			"ordinary diagnostics must remain a separate read-only query after capture failure");
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
		AppendBytesField(initializeRequest, 1u, "SailorEditor");
		AppendBytesField(initializeRequest, 1u, "--hwnd");
		AppendBytesField(initializeRequest, 1u, "not-a-number");

		const std::string request = MakeVersionedRequest(
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
		Require(SailorProtocolStopLocalHost(true) != 0,
			"an initialization exception must be rolled back before starting another session");
		Require(SailorProtocolStopLocalHost(true) != 0,
			"a completed local-host shutdown must allow repeated cleanup");
	}

	void TestEnvelopeValidation()
	{
		{
			TProtocolBuffer buffer;
			const auto response = RequireProtocolResponse(
				MakeVersionedRequest(
					EditorEngineProtocolVersion + 1u,
					17,
					c_getExitCodeCommandField),
				buffer);
			Require(
				response.m_protocolVersion ==
					EditorEngineProtocolVersion &&
				response.m_requestId == 17 &&
				!response.m_bSuccess &&
				response.m_error.find("version") != std::string::npos &&
				response.m_resultField == 0,
				"version mismatch must return a correlated protocol error");
		}

		{
			TProtocolBuffer buffer;
			const auto response = RequireProtocolResponse(
				MakeVersionedRequest(
					EditorEngineProtocolVersion,
					0,
					c_getExitCodeCommandField),
				buffer);
			Require(
				response.m_protocolVersion == EditorEngineProtocolVersion &&
				response.m_requestId == 0 &&
				!response.m_bSuccess &&
				response.m_error.find("request_id") != std::string::npos &&
				response.m_resultField == 0,
				"zero request id must return a protocol error");
		}

		{
			TProtocolBuffer buffer;
			const auto response = RequireProtocolResponse(
				MakeVersionedRequest(EditorEngineProtocolVersion, 23),
				buffer);
			Require(
				response.m_requestId == 23 &&
				!response.m_bSuccess &&
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
		AppendBytesField(parameterRequest, 1u, "Animator-1");
		AppendBytesField(parameterRequest, 2u, "Speed");

		TProtocolBuffer buffer;
		const auto response = RequireProtocolResponse(
			MakeVersionedRequest(
				EditorEngineProtocolVersion,
				27,
				c_setAnimatorParameterCommandField,
				parameterRequest),
			buffer,
			dependencies);
		Require(
			!response.m_bSuccess &&
				response.m_error.find("value is not set") != std::string::npos &&
				response.m_resultField == 0,
			"animator parameter mutations must carry exactly one typed value");
	}

	void TestAnimatorStateWithoutEditor()
	{
		for (const char* instanceId : { static_cast<const char*>(nullptr), "Animator-1" })
		{
			bool bHasController = true;
			uint64_t controllerRevision = 42;
			uint64_t activeStateId = 43;
			std::string activeStateName = "Previous active state";
			float activeStateTime = 2.0f;
			bool bTransitioning = true;
			uint64_t destinationStateId = 44;
			std::string destinationStateName = "Previous destination state";
			float destinationStateTime = 3.0f;
			float transitionAlpha = 0.5f;
			Require(!Sailor::EditorRuntime::GetEditorAnimatorState(instanceId,
				bHasController, controllerRevision, activeStateId, activeStateName, activeStateTime,
				bTransitioning, destinationStateId, destinationStateName, destinationStateTime, transitionAlpha),
				"an unavailable editor must not report an animator state");
			Require(!bHasController && controllerRevision == 0 &&
				activeStateId == Sailor::InvalidAnimationControllerNodeId && activeStateName.empty() &&
				activeStateTime == 0.0f && !bTransitioning &&
				destinationStateId == Sailor::InvalidAnimationControllerNodeId && destinationStateName.empty() &&
				destinationStateTime == 0.0f && transitionAlpha == 0.0f,
				"a failed query must clear caller-owned names and all previous state");
		}

		Sailor::Protocol::TEditorEngineProtocolLifecycleGate gate;
		std::string error;
		Require(gate.TryBeginInitialization(error), "animator state protocol fixture must initialize");
		gate.CompleteInitialization(true);
		Sailor::Protocol::EditorEngineProtocolDependencies dependencies;
		dependencies.m_lifecycleGate = &gate;
		std::string stateRequest;
		AppendBytesField(stateRequest, 1u, "Animator-1");
		TProtocolBuffer buffer;
		const auto response = RequireProtocolResponse(
			MakeVersionedRequest(EditorEngineProtocolVersion, 28,
				sailor::editor::v1::ProtocolRequest::kGetAnimatorState, stateRequest),
			buffer, dependencies);
		Require(!response.m_bSuccess && response.m_requestId == 28 && response.m_resultField == 0 &&
			response.m_error == "Animator component was not found.",
			"an unavailable animator must preserve the protocol error without a stale state result");
	}

	void TestStrictInstanceIdProtocolGate()
	{
		std::string strictInstantiateRequest;
		AppendVarintField(
			strictInstantiateRequest,
			3u,
			1u);

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
				MakeVersionedRequest(
					EditorEngineProtocolVersion,
					26,
					c_instantiatePrefabFromYamlCommandField,
					strictInstantiateRequest),
				buffer,
				dependencies);
			Require(
				response.m_protocolVersion ==
					EditorEngineProtocolVersion &&
				response.m_requestId == 26 &&
				response.m_bSuccess &&
				response.m_resultField == c_instanceIdResultField &&
				ReadResult(response) == 0 &&
				response.m_bSupportsStrictInstanceIds,
				"a capable host must dispatch strict restoration through protocol v1");
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
			MakeVersionedRequest(
				EditorEngineProtocolVersion,
				28,
				c_setEditorStatsModeCommandField,
				invalidModeRequest),
			buffer,
			dependencies);
		Require(
			response.m_requestId == 28 &&
			!response.m_bSuccess &&
			response.m_resultField == 0 &&
			response.m_error.find("stats mode") != std::string::npos,
			"an invalid Editor stats mode must fail without mutating runtime state");
	}

	void TestEditorRenderModeWireContract()
	{
		static_assert(
			sailor::editor::v1::ProtocolRequest::
				kSetEditorRenderModeFieldNumber ==
			c_setEditorRenderModeCommandField);
		static_assert(
			sailor::editor::v1::ProtocolRequest::
				kGetEditorRenderModeFieldNumber ==
			c_getEditorRenderModeCommandField);
		static_assert(
			sailor::editor::v1::ProtocolResponse::
				kEditorRenderModeResultFieldNumber ==
			c_editorRenderModeResultField);
		static_assert(
			sailor::editor::v1::EditorRenderModeRequest::
				kModeFieldNumber == 1);
		static_assert(
			sailor::editor::v1::EditorRenderModeResult::
				kModeFieldNumber == 1);
		static_assert(sailor::editor::v1::EDITOR_RENDER_MODE_LIT == 1);
		static_assert(
			sailor::editor::v1::EDITOR_RENDER_MODE_AMBIENT_OCCLUSION == 2);
		static_assert(
			sailor::editor::v1::EDITOR_RENDER_MODE_CASCADES == 3);
		static_assert(
			sailor::editor::v1::EDITOR_RENDER_MODE_LIGHT_TILES == 4);
		static_assert(
			sailor::editor::v1::EDITOR_RENDER_MODE_GLOBAL_ILLUMINATION_ONLY == 5);
		static_assert(
			sailor::editor::v1::EDITOR_RENDER_MODE_GLOBAL_ILLUMINATION_PROBES == 6);
		static_assert(
			sailor::editor::v1::EDITOR_RENDER_MODE_GLOBAL_ILLUMINATION_BRICKS == 7);
		static_assert(
			sailor::editor::v1::EDITOR_RENDER_MODE_GLOBAL_ILLUMINATION_VALIDITY == 8);
		static_assert(
			sailor::editor::v1::EDITOR_RENDER_MODE_GLOBAL_ILLUMINATION_VISIBILITY == 9);
		static_assert(
			sailor::editor::v1::EDITOR_RENDER_MODE_GLOBAL_ILLUMINATION_RESIDENCY == 10);
		static_assert(
			sailor::editor::v1::EDITOR_RENDER_MODE_GLOBAL_ILLUMINATION_ASSET_IDENTITY == 11);
		static_assert(
			sailor::editor::v1::EDITOR_RENDER_MODE_GLOBAL_ILLUMINATION_FALLBACK == 12);
		static_assert(
			sailor::editor::v1::EDITOR_RENDER_MODE_GLOBAL_ILLUMINATION_SUBDIVISIONS == 13);
		static_assert(
			sailor::editor::v1::ProtocolRequest::
				kSetRuntimeGiProbesPreviewBudgetFieldNumber ==
			c_setRuntimeGIProbesPreviewBudgetCommandField);
		static_assert(
			sailor::editor::v1::RuntimeGIProbesPreviewBudgetRequest::
				kBudgetFieldNumber == 1);
		static_assert(
			sailor::editor::v1::RuntimeGIProbesState::
				kPreviewBudgetFieldNumber == 16);
		static_assert(
			sailor::editor::v1::RUNTIME_GI_PROBES_PREVIEW_BUDGET_ECO == 1);
		static_assert(
			sailor::editor::v1::RUNTIME_GI_PROBES_PREVIEW_BUDGET_BALANCED == 2);

		Sailor::Protocol::TEditorEngineProtocolLifecycleGate gate;
		std::string admissionError;
		Require(
			gate.TryBeginInitialization(admissionError),
			"Render mode protocol fixture lifecycle must initialize");
		gate.CompleteInitialization(true);
		Sailor::Protocol::EditorEngineProtocolDependencies dependencies{};
		dependencies.m_lifecycleGate = &gate;

		std::string visibilityRequest;
		AppendVarintField(
			visibilityRequest,
			1u,
			sailor::editor::v1::EDITOR_RENDER_MODE_GLOBAL_ILLUMINATION_VISIBILITY);
		TProtocolBuffer setBuffer;
		const auto setResponse = RequireProtocolResponse(
			MakeVersionedRequest(
				EditorEngineProtocolVersion,
				129,
				c_setEditorRenderModeCommandField,
				visibilityRequest),
			setBuffer,
			dependencies);
		Require(
			setResponse.m_bSuccess &&
			setResponse.m_resultField == c_boolResultField &&
			ReadResult(setResponse) != 0,
			"a valid Editor render mode must update runtime state");

		TProtocolBuffer getBuffer;
		const auto getResponse = RequireProtocolResponse(
			MakeVersionedRequest(
				EditorEngineProtocolVersion,
				130,
				c_getEditorRenderModeCommandField),
			getBuffer,
			dependencies);
		uint64_t currentMode = 0;
		Require(
			getResponse.m_bSuccess &&
			getResponse.m_resultField == c_editorRenderModeResultField &&
			ReadNestedScalar(getResponse.m_resultPayload, currentMode) &&
			currentMode ==
				sailor::editor::v1::EDITOR_RENDER_MODE_GLOBAL_ILLUMINATION_VISIBILITY,
			"the typed render-mode query must return Engine truth");

		std::string invalidModeRequest;
		AppendVarintField(invalidModeRequest, 1u, 99u);
		TProtocolBuffer invalidBuffer;
		const auto invalidResponse = RequireProtocolResponse(
			MakeVersionedRequest(
				EditorEngineProtocolVersion,
				131,
				c_setEditorRenderModeCommandField,
				invalidModeRequest),
			invalidBuffer,
			dependencies);
		Require(
			!invalidResponse.m_bSuccess &&
			invalidResponse.m_resultField == 0 &&
			invalidResponse.m_error.find("render mode") != std::string::npos,
			"an invalid Editor render mode must be rejected");

		std::string litRequest;
		AppendVarintField(
			litRequest,
			1u,
			sailor::editor::v1::EDITOR_RENDER_MODE_LIT);
		TProtocolBuffer resetBuffer;
		const auto resetResponse = RequireProtocolResponse(
			MakeVersionedRequest(
				EditorEngineProtocolVersion,
				132,
				c_setEditorRenderModeCommandField,
				litRequest),
			resetBuffer,
			dependencies);
		Require(
			resetResponse.m_bSuccess && ReadResult(resetResponse) != 0,
			"the render-mode fixture must restore Lit mode");
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

	void TestModelFingerprintProtocol()
	{
		using Status = Sailor::ModelImporter::EFingerprintStatus;
		using namespace sailor::editor::v1;
		static_assert(static_cast<uint32_t>(Status::Unavailable) == MODEL_FINGERPRINT_STATUS_UNAVAILABLE);
		static_assert(static_cast<uint32_t>(Status::Pending) == MODEL_FINGERPRINT_STATUS_PENDING);
		static_assert(static_cast<uint32_t>(Status::Ready) == MODEL_FINGERPRINT_STATUS_READY);
		static_assert(static_cast<uint32_t>(Status::Failed) == MODEL_FINGERPRINT_STATUS_FAILED);
		Sailor::Protocol::TEditorEngineProtocolLifecycleGate gate;
		std::string error;
		Require(gate.TryBeginInitialization(error), "preview protocol fixture must initialize");
		gate.CompleteInitialization(true);
		Sailor::Protocol::EditorEngineProtocolDependencies dependencies;
		dependencies.m_lifecycleGate = &gate;
		std::string fileId;
		AppendBytesField(fileId, FileIdRequest::kFileIdFieldNumber, "01234567-89AB-CDEF-0123-456789ABCDEF");
		TProtocolBuffer admitted;
		const auto response = RequireProtocolResponse(MakeVersionedRequest(EditorEngineProtocolVersion, 143,
			ProtocolRequest::kRequestModelFingerprintFieldNumber, fileId), admitted, dependencies);
		Require(response.m_bSuccess && response.m_requestId == 143 &&
			response.m_resultField == c_boolResultField && ReadResult(response) == 0,
			"an unavailable importer must refuse generation rather than report a ready image");
		TProtocolBuffer queried;
		const auto status = RequireProtocolResponse(MakeVersionedRequest(EditorEngineProtocolVersion, 144,
			ProtocolRequest::kGetModelFingerprintStatusFieldNumber, fileId), queried, dependencies);
		uint64_t value = 0;
		Require(status.m_bSuccess && status.m_requestId == 144 &&
			status.m_resultField == ProtocolResponse::kModelFingerprintStatusResultFieldNumber &&
			ReadNestedScalar(status.m_resultPayload, value) &&
			value == MODEL_FINGERPRINT_STATUS_UNAVAILABLE,
			"status queries must distinguish an absent request/importer from pending or ready output");
	}

	void TestPathTracedExportIsUnsupported()
	{
		Sailor::Tests::TempDirectory output("unsupported-export");
		Sailor::Protocol::TEditorEngineProtocolLifecycleGate gate;
		std::string error;
		Require(gate.TryBeginInitialization(error), "export protocol fixture must initialize");
		gate.CompleteInitialization(true);
		Sailor::Protocol::EditorEngineProtocolDependencies dependencies;
		dependencies.m_lifecycleGate = &gate;

		for (const auto* instanceId : { "", "game-object", "component" })
		{
			std::string render;
			AppendBytesField(render, 1u, Sailor::Workspace::PathToUtf8(output.Path("export/image.png")));
			AppendBytesField(render, 2u, instanceId);
			AppendVarintField(render, 3u, 720);
			AppendVarintField(render, 4u, 64);
			AppendVarintField(render, 5u, 4);
			TProtocolBuffer buffer;
			const auto response = RequireProtocolResponse(MakeVersionedRequest(EditorEngineProtocolVersion, 145,
				c_renderPathTracedImageCommandField, render), buffer, dependencies);
			Require(response.m_protocolVersion == EditorEngineProtocolVersion && response.m_requestId == 145 &&
				!response.m_bSuccess && response.m_resultField == 0 &&
				response.m_error == "Path-traced image export is not supported by the editor.",
				"unsupported export must return an explicit protocol error, not a failed render result");
			Require(std::filesystem::is_empty(output.Get()), "unsupported export must not create output files or directories");
		}
	}

	void TestEmbeddedNullIsRejected()
	{
		std::string fileIdRequest;
		const std::string fileIdWithNull("asset\0id", 8);
		AppendBytesField(fileIdRequest, 1u, fileIdWithNull);

		TProtocolBuffer buffer;
		const auto response = RequireProtocolResponse(
			MakeVersionedRequest(
				EditorEngineProtocolVersion,
				29,
				c_loadEditorWorldCommandField,
				fileIdRequest),
			buffer);
		Require(
			response.m_requestId == 29 &&
			!response.m_bSuccess &&
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
		AppendBytesField(mutationRequest, 2u, unicodeInstanceId);

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
			MakeVersionedRequest(
				EditorEngineProtocolVersion,
				30,
				c_getManagedMutationRevisionCommandField,
				mutationRequest),
			buffer,
			dependencies);
		Require(
			response.m_requestId == 30 &&
			response.m_bSuccess &&
			response.m_bSupportsStrictInstanceIds &&
			response.m_error.empty() &&
			response.m_resultField == c_uint64ResultField,
			"valid UTF-8 protobuf strings must pass native string validation");
	}

	void TestGetExitCodeRoundTripAndFree()
	{
		TProtocolBuffer buffer;
		const auto response = RequireProtocolResponse(
			MakeVersionedRequest(
				EditorEngineProtocolVersion,
				31,
				c_getExitCodeCommandField),
			buffer);
		Require(
			response.m_protocolVersion == EditorEngineProtocolVersion &&
			response.m_requestId == 31 &&
			response.m_bSuccess &&
			response.m_error.empty() &&
			response.m_resultField == c_int32ResultField &&
			ReadResult(response) == 0,
			"get-exit-code must round-trip through the exported C ABI");
	}

	struct TViewportEventSource
	{
		Sailor::TVector<Sailor::EditorViewport::Event> m_events;
		uint32_t m_nextEvent = 0;
	};

	Sailor::TVector<Sailor::EditorViewport::Event> PullViewportEvents(
		void* context,
		uint32_t capacity)
	{
		auto* source = static_cast<TViewportEventSource*>(context);
		Sailor::TVector<Sailor::EditorViewport::Event> events;
		while (events.Num() < capacity && source->m_nextEvent < source->m_events.Num())
		{
			events.Add(std::move(source->m_events[source->m_nextEvent++]));
		}
		return events;
	}

	Sailor::TVector<Sailor::EditorViewport::Event> PullControllerViewportEvents(void* context, uint32_t capacity)
	{
		auto& controller = *static_cast<Sailor::EditorViewport::EditorViewportController*>(context);
		Sailor::TVector<Sailor::EditorViewport::Event> events;
		Sailor::EditorViewport::Event event;
		while (events.Num() < capacity && controller.PullEvent(event)) events.Add(std::move(event));
		return events;
	}

	void TestViewportEventBatchKeepsOrderAndOwnsPayload()
	{
		using namespace Sailor::EditorViewport;
		TViewportEventSource source{
			{
				Event{ 42, 9, SelectionEvent{ Sailor::InstanceId("Duck-123") } },
				Event{ 43, 10, SelectionEvent{} }
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
		AppendVarintField(countRequest, 1u, 1u);
		const std::string request = MakeVersionedRequest(
			EditorEngineProtocolVersion,
			41,
			c_pullEditorViewportEventsCommandField,
			countRequest);

		TProtocolBuffer buffer;
		Require(
			Invoke(request, buffer, dependencies) ==
				static_cast<int32_t>(EEditorEngineTransportStatus::Ok),
			"a typed viewport event must pass through the protocol transport");

		const auto response = DecodeResponse(buffer.GetData(), buffer.GetSize());
		Require(
			response.m_protocolVersion == EditorEngineProtocolVersion &&
			response.m_requestId == 41 &&
			response.m_bSuccess &&
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
			"a bounded pull must leave the next event in the queue");
		Require(
			event.m_revision == 42 &&
			event.m_managedMutationRevision == 9 &&
			event.m_hasSelection &&
			event.m_selectedInstanceId == "Duck-123",
			"the first typed selection event must preserve its identity and revisions");
		Require(source.m_nextEvent == 1, "the source must respect the requested capacity");

		TProtocolBuffer nextBuffer;
		const auto next = RequireProtocolResponse(request, nextBuffer, dependencies);
		source.m_events.Clear();
		Require(TryDecodeViewportEventBatch(next.m_resultPayload, numEvents, event) && numEvents == 1 &&
			event.m_revision == 43 && event.m_managedMutationRevision == 10 && event.m_hasSelection &&
			event.m_selectedInstanceId.empty(), "selection clear must follow selection and own its wire payload");
		TProtocolBuffer emptyBuffer;
		const auto empty = RequireProtocolResponse(request, emptyBuffer, dependencies);
		Require(TryDecodeViewportEventBatch(empty.m_resultPayload, numEvents, event) && numEvents == 0,
			"draining the queue must not duplicate the last event");
	}

	void TestViewportTransformPreservesValuesAndToolState()
	{
		using namespace Sailor::EditorViewport;
		const Sailor::InstanceId id("Duck-123");
		const Sailor::Math::Transform before({ 1, 2, -3, 1 }, glm::normalize(glm::quat(1, 2, 3, 4)), { 2, 3, 4, 1 });
		const Sailor::Math::Transform after({ -5, 6, 7, 1 }, glm::normalize(glm::quat(4, 3, 2, 1)), { 5, 6, 7, 1 });
		const std::array expected{ before.m_position,
			glm::vec4(before.GetRotation().x, before.GetRotation().y, before.GetRotation().z, before.GetRotation().w), before.m_scale,
			after.m_position, glm::vec4(after.GetRotation().x, after.GetRotation().y, after.GetRotation().z, after.GetRotation().w), after.m_scale };
		Sailor::Protocol::TEditorEngineProtocolLifecycleGate gate;
		std::string error;
		Require(gate.TryBeginInitialization(error), "transform test lifecycle must initialize");
		gate.CompleteInitialization(true);
		std::string count;
		AppendVarintField(count, 1, 1);
		for (auto operation : { ETransformOperation::Select, ETransformOperation::Translate,
			ETransformOperation::Rotate, ETransformOperation::Scale })
		{
			for (auto space : { ETransformSpace::World, ETransformSpace::Local })
			{
				TViewportEventSource source{ { Event{ 51, 12, TransformEvent{ id, before, after, operation, space } } } };
				Sailor::Protocol::EditorEngineProtocolDependencies dependencies{};
				dependencies.m_context = &source;
				dependencies.m_lifecycleGate = &gate;
				dependencies.m_pullEditorViewportEvents = PullViewportEvents;
				TProtocolBuffer buffer;
				const auto response = RequireProtocolResponse(
					MakeVersionedRequest(EditorEngineProtocolVersion, 44, c_pullEditorViewportEventsCommandField, count), buffer, dependencies);
				uint32_t numEvents;
				TDecodedViewportEvent event;
				Require(response.m_bSuccess && TryDecodeViewportEventBatch(response.m_resultPayload, numEvents, event) &&
					numEvents == 1 && event.m_bHasTransform && event.m_revision == 51 && event.m_managedMutationRevision == 12,
					"transform event must retain its kind, order and mutation revisions");
				Require(event.m_transform.m_instanceId == id.ToString() && event.m_transform.m_vectors == expected &&
					event.m_transform.m_operation == static_cast<uint64_t>(operation) + 1 &&
					event.m_transform.m_space == static_cast<uint64_t>(space) + 1,
					"transform wire fields must retain identity, tool state and before/after XYZW values");
			}
		}
	}

	void TestViewportAssetDropEventIsTypedAndValidated()
	{
		const std::string fileId =
			"{12345678-1234-1234-1234-123456789ABC}";
		Sailor::EditorViewport::EditorViewportController controller;
		controller.SetManagedMutationRevisions(10, 0);
		std::string source = fileId + "ignored suffix";
		Require(controller.QueueAssetDropEvent(std::string_view(source.data(), fileId.size()), 0.25f, 0.75f),
			"valid bounded asset-drop text must enter the viewport queue");
		source.assign("replaced");
		Require(!controller.QueueAssetDropEvent(fileId, 1.5f, 0.5f),
			"out-of-viewport drops must be rejected before queueing");
		Sailor::Protocol::EditorEngineProtocolDependencies dependencies{};
		dependencies.m_context = &controller;
		dependencies.m_pullEditorViewportEvents = PullControllerViewportEvents;
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
			MakeVersionedRequest(
				EditorEngineProtocolVersion,
				42,
				c_pullEditorViewportEventsCommandField,
				countRequest),
			buffer,
			dependencies);
		Require(
			response.m_bSuccess &&
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
			"only the accepted asset-drop event must reach the protocol");
		Require(
			event.m_revision == 1 &&
				event.m_managedMutationRevision == 10 &&
				event.m_hasAssetDrop &&
				event.m_assetFileId == fileId &&
				std::abs(event.m_normalizedX - 0.25f) < 0.0001f &&
				std::abs(event.m_normalizedY - 0.75f) < 0.0001f,
			"valid asset-drop data must survive native typed conversion");
	}

	void TestViewportToolShortcutEventIsTypedAndValidated()
	{
		Sailor::EditorViewport::EditorViewportController controller;
		controller.SetManagedMutationRevisions(11, 0);
		Require(controller.QueueToolShortcutEvent('W'), "supported tool shortcuts must enter the queue");
		Require(!controller.QueueToolShortcutEvent('X'), "unsupported keys must be rejected before queueing");
		Sailor::Protocol::EditorEngineProtocolDependencies dependencies{};
		dependencies.m_context = &controller;
		dependencies.m_pullEditorViewportEvents = PullControllerViewportEvents;
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
			MakeVersionedRequest(
				EditorEngineProtocolVersion,
				43,
				c_pullEditorViewportEventsCommandField,
				countRequest),
			buffer,
			dependencies);
		Require(
			response.m_bSuccess &&
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
			"only the accepted tool shortcut must reach the protocol");
		Require(
			event.m_revision == 1 &&
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
		Sailor::Tests::ScopeExit completeStart([&]() { gate.CompleteStart(); });
		Sailor::Tests::ScopeExit releaseOperation([&]() { gate.ReleaseOperation(); });
		Require(
			drain.wait_for(20ms) == std::future_status::timeout,
			"Shutdown must wait for both Start and regular operation leases");
		releaseOperation.Run();
		Require(
			drain.wait_for(20ms) == std::future_status::timeout,
			"Shutdown must continue waiting while Start is active");
		completeStart.Run();
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
		Require(stoppedGate.TryAcquireStop(), "Stop must acquire an initialized session");
		stoppedGate.ReleaseOperation();
		Require(
			!stoppedGate.TryBeginStart(error),
			"Stop before Start must prevent a late Start race");

		Sailor::Protocol::TEditorEngineProtocolLifecycleGate initializingGate;
		Require(
			initializingGate.TryBeginInitialization(error),
			"fresh gate must admit initialization");
		Require(
			!initializingGate.TryAcquireStop(),
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
		Sailor::Tests::ScopeExit completeInitialization([&]() { shutdownInitGate.CompleteInitialization(true); });
		Require(
			initializationDrain.wait_for(20ms) == std::future_status::timeout,
			"Shutdown must wait for the active Initialize owner");
		completeInitialization.Run();
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

	bool RecordShutdown(void* context)
	{
		auto& source =
			*static_cast<TBlockingLifecycleSource*>(context);
		const std::lock_guard<std::mutex> lock(source.m_mutex);
		++source.m_numShutdowns;
		source.m_bShutdownObservedStartExit = source.m_bStartExited;
		return true;
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

	void TestFailedShutdownKeepsAdmissionClosedUntilRetry()
	{
		Sailor::Protocol::TEditorEngineProtocolLifecycleGate gate;
		PrepareInitializedLifecycle(gate);
		uint32_t attempts = 0u;
		Sailor::Protocol::EditorEngineProtocolDependencies dependencies{};
		dependencies.m_context = &attempts;
		dependencies.m_lifecycleGate = &gate;
		dependencies.m_stop = [](void*) {};
		dependencies.m_shutdown = [](void* context)
			{
				const auto attempt = ++*static_cast<uint32_t*>(context);
				if (attempt == 2u) throw std::runtime_error("shutdown test failure");
				return attempt == 3u;
			};
		for (uint32_t attempt = 1u; attempt <= 3u; ++attempt)
		{
			TProtocolBuffer buffer;
			TDecodedResponse response{};
			bool bThrew = false;
			try
			{
				response = RequireProtocolResponse(
					MakeVersionedRequest(EditorEngineProtocolVersion, attempt, c_shutdownCommandField), buffer, dependencies);
			}
			catch (const std::runtime_error& exception)
			{
				Require(attempt == 2u && std::string(exception.what()) == "shutdown test failure",
					"internal invocation must propagate the original shutdown exception to its transport boundary");
				bThrew = true;
			}
			Require(bThrew == (attempt == 2u), "throwing shutdown must reach the native transport boundary");
			Require(attempts == attempt, "Shutdown must execute each explicit retry exactly once");
			Require(response.m_bSuccess == (attempt == 3u), "Shutdown must report native completion, not just dispatch");
			std::string error;
			if (attempt < 3u)
			{
				Require(bThrew || (!response.m_error.empty() && response.m_resultField != c_emptyResultField),
					"failed Shutdown must carry an error instead of an empty success result");
				Require(!gate.TryBeginInitialization(error) && !gate.TryBeginStart(error) &&
					!gate.TryAcquireOperation(error, true), "failed Shutdown must keep the old session closed");
			}
			else
			{
				Require(response.m_resultField == c_emptyResultField && gate.TryBeginInitialization(error),
					"successful Shutdown retry must allow a fresh session");
				gate.CompleteInitialization(true);
			}
		}
	}

	void TestFailedInitializationRequiresRollbackBeforeRetry()
	{
		Sailor::Tests::TempDirectory workspace("protocol-initialization");
		std::ofstream(workspace.Path("file")) << "not a directory";
		std::filesystem::create_directory(workspace.Path("invalid"));
		std::ofstream(workspace.Path("invalid/project.sailor")) << "manifestVersion: [";
		Sailor::Protocol::TEditorEngineProtocolLifecycleGate gate;
		Sailor::Protocol::EditorEngineProtocolDependencies dependencies{};
		dependencies.m_lifecycleGate = &gate;

		for (const char* path : { "missing", "file", "invalid", "missing" })
		{
			std::string arguments;
			for (const auto& argument : { std::string("SailorEngine"), std::string("--workspace"),
				workspace.Path(path).string(), std::string("--noconsole"), std::string("--new-world") })
			{
				AppendBytesField(arguments, 1u, argument);
			}
			TProtocolBuffer buffer;
			const auto response = RequireProtocolResponse(MakeVersionedRequest(EditorEngineProtocolVersion, 1u,
				c_initializeCommandField, arguments), buffer, dependencies);
			Require(!response.m_bSuccess && !response.m_error.empty(),
				"Initialize must report the real App failure, not successful dispatch");
			Require(Sailor::App::GetInstance() && Sailor::App::GetExitCode() != 0 &&
				Sailor::App::Initialize() == Sailor::EAppInitializationResult::Failed,
				"failed initialization must retain its partial App and diagnostic until rollback");
			std::string error;
			Require(!gate.TryBeginStart(error) && !gate.TryAcquireOperation(error, false) &&
				!gate.TryBeginInitialization(error),
				"failed initialization must not admit commands or another App before rollback");
			TProtocolBuffer shutdownBuffer;
			Require(RequireProtocolResponse(MakeVersionedRequest(EditorEngineProtocolVersion, 2u,
				c_shutdownCommandField), shutdownBuffer, dependencies).m_bSuccess && !Sailor::App::GetInstance(),
				"shutdown must release a partially initialized App and allow the next attempt");
		}
	}

	void TestInitializationSupersededByShutdownIsNotAcknowledged()
	{
		Sailor::Protocol::TEditorEngineProtocolLifecycleGate gate;
		Sailor::Protocol::EditorEngineProtocolDependencies dependencies{};
		dependencies.m_context = &gate;
		dependencies.m_lifecycleGate = &gate;
		dependencies.m_initialize = [](void* context, const char**, int32_t)
			{
				auto& lifecycle = *static_cast<Sailor::Protocol::TEditorEngineProtocolLifecycleGate*>(context);
				std::string error;
				Require(lifecycle.TryBeginShutdown(error), "shutdown must supersede the active initialization");
				return Sailor::EAppInitializationResult::Ready;
			};
		TProtocolBuffer buffer;
		const auto response = RequireProtocolResponse(MakeVersionedRequest(EditorEngineProtocolVersion, 1u,
			c_initializeCommandField), buffer, dependencies);
		gate.WaitForInitializationDrain();
		gate.CompleteShutdown();
		Require(!response.m_bSuccess && !response.m_error.empty(),
			"initialization superseded by shutdown must not acknowledge a ready session");
	}

	void TestStopWaitsForInitializationAndSkipsClosedSessions()
	{
		Sailor::Protocol::TEditorEngineProtocolLifecycleGate gate;
		uint32_t stops = 0u;
		Sailor::Protocol::EditorEngineProtocolDependencies dependencies{};
		dependencies.m_context = &stops;
		dependencies.m_lifecycleGate = &gate;
		dependencies.m_stop = [](void* context) { ++*static_cast<uint32_t*>(context); };
		auto stop = [&]()
			{
				TProtocolBuffer buffer;
				Require(RequireProtocolResponse(MakeVersionedRequest(EditorEngineProtocolVersion, 1u,
					c_stopCommandField), buffer, dependencies).m_bSuccess, "Stop must acknowledge its request");
			};

		std::string error;
		Require(gate.TryBeginInitialization(error), "Stop test must admit initialization");
		stop();
		stop();
		Require(stops == 0u, "Stop must not access App while initialization owns its construction");
		gate.CompleteInitialization(true);
		Require(!gate.TryBeginStart(error), "Stop during initialization must prevent a late Start");
		stop();
		Require(stops == 1u, "Stop must reach the initialized App");

		Require(gate.TryBeginShutdown(error), "Stop test must admit shutdown");
		stop();
		gate.WaitForShutdownDrain();
		gate.CompleteShutdown();
		stop();
		Require(stops == 1u, "late Stop must not enter App during or after teardown");

		PrepareInitializedLifecycle(gate);
		Require(gate.TryBeginStart(error), "a new session must not inherit the previous Stop");
		gate.CompleteStart();
	}

	void TestShutdownDrainsAnAdmittedStop()
	{
		using namespace std::chrono_literals;
		struct TStopSource
		{
			std::promise<void> m_stopEntered;
			std::promise<void> m_releaseStop;
			std::promise<void> m_shutdownEntered;
			std::atomic<bool> m_bStopExited{false};
			bool m_bShutdownAfterStop = false;
		} source;

		Sailor::Protocol::TEditorEngineProtocolLifecycleGate gate;
		PrepareInitializedLifecycle(gate);
		Sailor::Protocol::EditorEngineProtocolDependencies stopDependencies{};
		stopDependencies.m_context = &source;
		stopDependencies.m_lifecycleGate = &gate;
		stopDependencies.m_stop = [](void* context)
			{
				auto& state = *static_cast<TStopSource*>(context);
				state.m_stopEntered.set_value();
				state.m_releaseStop.get_future().wait();
				state.m_bStopExited = true;
			};
		auto shutdownDependencies = stopDependencies;
		shutdownDependencies.m_stop = [](void* context)
			{
				static_cast<TStopSource*>(context)->m_shutdownEntered.set_value();
			};
		shutdownDependencies.m_shutdown = [](void* context)
			{
				auto& state = *static_cast<TStopSource*>(context);
				state.m_bShutdownAfterStop = state.m_bStopExited;
				return true;
			};

		auto stop = std::async(std::launch::async, [&]()
			{
				TProtocolBuffer buffer;
				return RequireProtocolResponse(MakeVersionedRequest(EditorEngineProtocolVersion, 1u,
					c_stopCommandField), buffer, stopDependencies).m_bSuccess;
			});
		const bool bStopEntered = source.m_stopEntered.get_future().wait_for(1s) == std::future_status::ready;
		auto shutdown = std::async(std::launch::async, [&]()
			{
				TProtocolBuffer buffer;
				return RequireProtocolResponse(MakeVersionedRequest(EditorEngineProtocolVersion, 2u,
					c_shutdownCommandField), buffer, shutdownDependencies).m_bSuccess;
			});
		const bool bShutdownEntered = source.m_shutdownEntered.get_future().wait_for(1s) == std::future_status::ready;
		const bool bShutdownWaited = shutdown.wait_for(20ms) == std::future_status::timeout;
		source.m_releaseStop.set_value();
		const bool bStopped = stop.get();
		const bool bShutdown = shutdown.get();
		Require(bStopEntered && bShutdownEntered && bStopped && bShutdown,
			"Stop and Shutdown must enter and complete their lifecycle callbacks");
		Require(bShutdownWaited && source.m_bShutdownAfterStop,
			"Shutdown must retain App until every admitted Stop callback has returned");
	}

	void TestThrowingStopReleasesItsOperation()
	{
		Sailor::Protocol::TEditorEngineProtocolLifecycleGate gate;
		PrepareInitializedLifecycle(gate);
		Sailor::Protocol::EditorEngineProtocolDependencies dependencies{};
		dependencies.m_lifecycleGate = &gate;
		dependencies.m_stop = [](void*) { throw std::runtime_error("stop failure"); };
		bool bThrew = false;
		try
		{
			TProtocolBuffer buffer;
			RequireProtocolResponse(MakeVersionedRequest(EditorEngineProtocolVersion, 1u,
				c_stopCommandField), buffer, dependencies);
		}
		catch (const std::runtime_error& error)
		{
			bThrew = std::string(error.what()) == "stop failure";
		}
		Require(bThrew, "Stop must propagate its exception to the transport boundary");
		std::string error;
		Require(gate.TryBeginShutdown(error), "a failed Stop must still allow shutdown");
		gate.WaitForShutdownDrain();
		gate.CompleteShutdown();
	}

	void TestLocalHostDrainStopsBeforeWaitingForStart()
	{
		using namespace std::chrono_literals;
		struct TDrainSource
		{
			Sailor::Protocol::TEditorEngineProtocolLifecycleGate m_gate;
			std::promise<void> m_startEntered;
			std::promise<void> m_releaseStart;
			std::promise<void> m_stopEntered;
			bool m_bAdmissionClosedAtStop = false;
		} source;

		PrepareInitializedLifecycle(source.m_gate);
		Sailor::Protocol::EditorEngineProtocolDependencies dependencies{};
		dependencies.m_context = &source;
		dependencies.m_lifecycleGate = &source.m_gate;
		dependencies.m_start = [](void* context)
			{
				auto& state = *static_cast<TDrainSource*>(context);
				state.m_startEntered.set_value();
				state.m_releaseStart.get_future().wait();
			};
		dependencies.m_stop = [](void* context)
			{
				auto& state = *static_cast<TDrainSource*>(context);
				std::string error;
				state.m_bAdmissionClosedAtStop = !state.m_gate.TryAcquireOperation(error, false);
				if (!state.m_bAdmissionClosedAtStop)
				{
					state.m_gate.ReleaseOperation();
				}
				state.m_stopEntered.set_value();
			};

		TProtocolBuffer buffer;
		const auto start = RequireProtocolResponse(MakeVersionedRequest(EditorEngineProtocolVersion, 1u,
			c_startCommandField), buffer, dependencies);
		std::future<void> drain;
		Sailor::Tests::ScopeExit releaseStart([&]() { source.m_releaseStart.set_value(); });
		Require(start.m_bSuccess && source.m_startEntered.get_future().wait_for(1s) == std::future_status::ready,
			"local-host drain fixture must enter the admitted Start worker");
		std::string error;
		Require(source.m_gate.TryAcquireOperation(error, false), "the active session must admit its existing operation");
		Sailor::Tests::ScopeExit releaseOperation([&]() { source.m_gate.ReleaseOperation(); });
		drain = std::async(std::launch::async, [&]()
			{
				Sailor::Protocol::TryDrainEditorEngineProtocolForShutdown(dependencies);
			});

		const bool bStopEntered = source.m_stopEntered.get_future().wait_for(1s) == std::future_status::ready;
		const bool bWaitedForStart = drain.wait_for(20ms) == std::future_status::timeout;
		releaseStart.Run();
		const bool bWaitedForOperation = drain.wait_for(20ms) == std::future_status::timeout;
		releaseOperation.Run();
		drain.get();
		Require(bStopEntered && source.m_bAdmissionClosedAtStop,
			"local-host drain must close admission and request Stop before waiting for Start");
		Require(bWaitedForStart && bWaitedForOperation && !source.m_gate.IsStartActive(),
			"local-host drain must retain the session until Start and admitted operations finish");
		source.m_gate.CompleteShutdown();
	}

	void TestLocalHostDrainHasOneShutdownOwner()
	{
		using namespace std::chrono_literals;
		struct TShutdownSource
		{
			std::atomic<uint32_t> m_numStops{0u};
			std::promise<void> m_stopEntered;
			std::promise<void> m_releaseStop;
			std::shared_future<void> m_stopRelease = m_releaseStop.get_future().share();
		} source;

		Sailor::Protocol::TEditorEngineProtocolLifecycleGate gate;
		PrepareInitializedLifecycle(gate);
		Sailor::Protocol::EditorEngineProtocolDependencies dependencies{};
		dependencies.m_context = &source;
		dependencies.m_lifecycleGate = &gate;
		dependencies.m_stop = [](void* context)
			{
				auto& state = *static_cast<TShutdownSource*>(context);
				if (state.m_numStops.fetch_add(1u) == 0u)
				{
					state.m_stopEntered.set_value();
				}
				state.m_stopRelease.wait();
			};

		std::future<bool> owner;
		std::future<bool> duplicate;
		Sailor::Tests::ScopeExit releaseStop([&]() { source.m_releaseStop.set_value(); });
		owner = std::async(std::launch::async, [&]()
			{
				return Sailor::Protocol::TryDrainEditorEngineProtocolForShutdown(dependencies);
			});
		Require(source.m_stopEntered.get_future().wait_for(1s) == std::future_status::ready,
			"the shutdown owner must enter Stop before the duplicate request");
		duplicate = std::async(std::launch::async, [&]()
			{
				return Sailor::Protocol::TryDrainEditorEngineProtocolForShutdown(dependencies);
			});
		const bool bDuplicateReturned = duplicate.wait_for(1s) == std::future_status::ready;
		const bool bOwnerStillDraining = owner.wait_for(20ms) == std::future_status::timeout;
		releaseStop.Run();
		const bool bOwnerAdmitted = owner.get();
		const bool bDuplicateAdmitted = duplicate.get();
		Require(bOwnerAdmitted && !bDuplicateAdmitted && bDuplicateReturned &&
			bOwnerStillDraining && source.m_numStops == 1u,
			"a duplicate native shutdown must not acquire the existing owner's teardown");
		gate.CompleteShutdown();
		Require(Sailor::Protocol::TryDrainEditorEngineProtocolForShutdown(dependencies) && source.m_numStops == 2u,
			"native host cleanup must also close a session that protocol shutdown already completed");
		std::string error;
		Require(!gate.TryBeginInitialization(error), "host finalization must retain exclusive lifecycle ownership");
		gate.Reset();
		Require(gate.TryBeginInitialization(error), "only completed native cleanup may admit the next session");
		gate.CompleteInitialization(true);
		Require(gate.TryAcquireOperation(error, false), "the next initialized session must remain ready");
		gate.ReleaseOperation();
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
					MakeVersionedRequest(
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
			startResponse.m_bSuccess &&
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
			MakeVersionedRequest(
				EditorEngineProtocolVersion,
				58,
				c_isEngineRunningCommandField),
			livenessBuffer,
			dependencies);
		Require(
			livenessResponse.m_bSuccess &&
				livenessResponse.m_resultField == c_boolResultField &&
				ReadResult(livenessResponse) != 0,
			"lifecycle probe must report the admitted Start worker as running");

		TProtocolBuffer duplicateBuffer;
		const auto duplicateResponse = RequireProtocolResponse(
			MakeVersionedRequest(
				EditorEngineProtocolVersion,
				52,
				c_startCommandField),
			duplicateBuffer,
			dependencies);
		Require(
			!duplicateResponse.m_bSuccess,
			"an active async Start must still reject duplicate starts");

		TProtocolBuffer stopBuffer;
		const auto stopResponse = RequireProtocolResponse(
			MakeVersionedRequest(
				EditorEngineProtocolVersion,
				53,
				c_stopCommandField),
			stopBuffer,
			dependencies);
		Require(
			stopResponse.m_bSuccess &&
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
			MakeVersionedRequest(
				EditorEngineProtocolVersion,
				59,
				c_isEngineRunningCommandField),
			stoppedLivenessBuffer,
			dependencies);
		Require(
			stoppedLivenessResponse.m_bSuccess &&
				stoppedLivenessResponse.m_resultField == c_boolResultField &&
				ReadResult(stoppedLivenessResponse) == 0,
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
			startResponse.m_bSuccess,
			"immediate Stop test must receive the Start acknowledgement");

		// Do not wait for BlockingStart to enter. Stop must publish its request
		// before or after the worker reaches the routine without losing it.
		TProtocolBuffer stopBuffer;
		const auto stopResponse = RequireProtocolResponse(
			MakeVersionedRequest(
				EditorEngineProtocolVersion,
				57,
				c_stopCommandField),
			stopBuffer,
			dependencies);
		Require(
			stopResponse.m_bSuccess,
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
			startResponse.m_bSuccess,
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
			MakeVersionedRequest(
				EditorEngineProtocolVersion,
				55,
				c_shutdownCommandField),
			shutdownBuffer,
			dependencies);
		Require(
			shutdownResponse.m_bSuccess &&
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
			FEditorEngineProtocolOperation operation)
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

		operation();
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

	bool RecordEditorDispatchShutdown(void* context)
	{
		auto& source =
			*static_cast<TBlockingEditorDispatchSource*>(context);
		const std::lock_guard<std::mutex> lock(source.m_mutex);
		++source.m_numShutdowns;
		return true;
	}

	struct TEditorExceptionSource
	{
		std::thread::id m_dispatchThreadId{};
	};

	bool DispatchEditorOperationOnTestThread(
		void* context,
		Sailor::Protocol::EditorEngineProtocolDependencies::
			FEditorEngineProtocolOperation operation)
	{
		auto& source = *static_cast<TEditorExceptionSource*>(context);
		std::thread editorThread(
			[&source, operation = std::move(operation)]()
			{
				source.m_dispatchThreadId = std::this_thread::get_id();
				operation();
			});
		editorThread.join();
		return true;
	}

	Sailor::TVector<Sailor::EditorViewport::Event> ThrowFromEditorOperation(void*, uint32_t)
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
				MakeVersionedRequest(
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

	void TestEditorCommandDoesNotBlockLifecycleDispatch(bool bStopReleasesDispatch = true)
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
		if (!bStopReleasesDispatch)
		{
			dependencies.m_stop = [](void* context)
			{
				auto& blocked = *static_cast<TBlockingEditorDispatchSource*>(context);
				const std::lock_guard lock(blocked.m_mutex);
				++blocked.m_numStops;
			};
		}

		auto command = std::async(
			std::launch::async,
			[&dependencies]()
			{
				TProtocolBuffer buffer;
				return RequireProtocolResponse(
					MakeVersionedRequest(
						EditorEngineProtocolVersion,
						61,
						c_getEditorSimulationStateCommandField),
					buffer,
					dependencies);
			});
		std::future<TDecodedResponse> shutdown;
		Sailor::Tests::ScopeExit releaseDispatch([&]()
		{
			{
				const std::lock_guard lock(source.m_mutex);
				source.m_bReleaseDispatch = true;
			}
			source.m_condition.notify_all();
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
				"a regular editor command must enter the dedicated Editor dispatcher");
		}

		shutdown = std::async(
			std::launch::async,
			[&dependencies]()
			{
				TProtocolBuffer buffer;
				return RequireProtocolResponse(
					MakeVersionedRequest(
						EditorEngineProtocolVersion,
						62,
						c_shutdownCommandField),
					buffer,
					dependencies);
			});

		const bool bShutdownCompletedBeforeCleanup = shutdown.wait_for(1s) == std::future_status::ready;
		releaseDispatch.Run();

		const auto shutdownResponse = shutdown.get();
		const auto commandResponse = command.get();
		Require(bShutdownCompletedBeforeCleanup,
			"Shutdown must finish before test cleanup releases the Editor worker");
		Require(
			shutdownResponse.m_bSuccess &&
				shutdownResponse.m_resultField == c_emptyResultField,
			"Shutdown must bypass the Editor worker and drain it safely");
		Require(
			commandResponse.m_bSuccess &&
				commandResponse.m_resultField == c_boolResultField,
			"the editor command must complete its regular operation response");
		{
			const std::lock_guard<std::mutex> lock(source.m_mutex);
			Require(
				source.m_numDispatches == 1 &&
					source.m_numStops == 1 &&
					source.m_numShutdowns == 1 &&
					source.m_bOperationExecuted,
				"lifecycle must interrupt and drain editor commands without entering the Editor dispatcher");
		}
	}

	void TestShutdownOrderingRejectsCleanupAssistedCompletion()
	{
		bool bRejected = false;
		try
		{
			TestEditorCommandDoesNotBlockLifecycleDispatch(false);
		}
		catch (const std::runtime_error& error)
		{
			if (std::string_view(error.what()) != "Shutdown must finish before test cleanup releases the Editor worker") throw;
			bRejected = true;
		}
		Require(bRejected, "a Stop callback that never releases its worker must fail the ordering check");
		std::cout << "Shutdown ordering: cleanup-assisted completion rejected after joining both workers" << std::endl;
	}

	void TestNativeStopCancelsQueuedEditorRequests(bool bQueued)
	{
		using namespace std::chrono_literals;
		Sailor::Tests::TaskTestApp app;
		auto& scheduler = app.GetScheduler();
		scheduler.Initialize();
		struct TSource
		{
			std::promise<void> m_entered;
			std::promise<void> m_stopEntered;
			std::promise<void> m_release;
			std::shared_future<void> m_resume = m_release.get_future().share();
			std::atomic<uint32_t> m_numCalls{0u};
			std::atomic<uint32_t> m_capacity{0u};
			std::atomic<uint32_t> m_numShutdowns{0u};
			bool m_bBlockOperation = false;
		} source;
		source.m_bBlockOperation = !bQueued;
		Sailor::Protocol::TEditorEngineProtocolLifecycleGate gate;
		PrepareInitializedLifecycle(gate);
		Sailor::Protocol::EditorEngineProtocolDependencies dependencies{};
		dependencies.m_context = &source;
		dependencies.m_lifecycleGate = &gate;
		dependencies.m_dispatchEditorOperation = Sailor::Protocol::DispatchEditorEngineProtocolOperationOnEditorThread;
		dependencies.m_pullEditorViewportEvents = [](void* context, uint32_t capacity)
			{
				auto& state = *static_cast<TSource*>(context);
				++state.m_numCalls;
				state.m_capacity = capacity;
				if (state.m_bBlockOperation)
				{
					state.m_entered.set_value();
					state.m_resume.wait();
				}
				return Sailor::TVector<Sailor::EditorViewport::Event>{};
			};
		dependencies.m_stop = [](void* context) { static_cast<TSource*>(context)->m_stopEntered.set_value(); };
		dependencies.m_shutdown = [](void* context)
			{
				++static_cast<TSource*>(context)->m_numShutdowns;
				return true;
			};
		std::future<TDecodedResponse> command;
		std::future<bool> shutdown;
		Sailor::Tests::ScopeExit releaseWorker([&]()
			{
				source.m_release.set_value();
				scheduler.WaitIdle(Sailor::EThreadType::Editor);
				if (command.valid())
				{
					command.wait();
				}
				if (shutdown.valid())
				{
					shutdown.wait();
				}
			});
		if (bQueued)
		{
			auto holdWorker = Sailor::Tasks::CreateTask("Hold Editor worker"_h, [&]()
				{
					source.m_entered.set_value();
					source.m_resume.wait();
				}, Sailor::EThreadType::Editor);
			scheduler.Run(holdWorker);
		}
		command = std::async(std::launch::async, [&]()
			{
				std::string payload;
				AppendVarintField(payload, 1u, 7u);
				TProtocolBuffer buffer;
				return RequireProtocolResponse(MakeVersionedRequest(EditorEngineProtocolVersion, 71u,
					c_pullEditorViewportEventsCommandField, payload), buffer, dependencies);
			});
		Require(source.m_entered.get_future().wait_for(1s) == std::future_status::ready,
			"the actual Editor worker must reach the test barrier");
		if (bQueued)
		{
			const auto deadline = std::chrono::steady_clock::now() + 1s;
			while (scheduler.GetNumTasks(Sailor::EThreadType::Editor) == 0 && std::chrono::steady_clock::now() < deadline)
			{
				std::this_thread::yield();
			}
			Require(scheduler.GetNumTasks(Sailor::EThreadType::Editor) == 1,
				"the protocol request must be queued behind the blocked Editor worker");
		}
		shutdown = std::async(std::launch::async, [&]()
			{
				return Sailor::Protocol::StopEditorEngineLocalHost(true, dependencies);
			});
		Require(source.m_stopEntered.get_future().wait_for(1s) == std::future_status::ready,
			"native shutdown must reach Stop before waiting for Editor commands");
		const bool bStoppedBeforeRelease = shutdown.wait_for(bQueued ? 1s : 30ms) == std::future_status::ready;
		const bool bCommandCompletedBeforeRelease = command.wait_for(bQueued ? 1s : 0ms) == std::future_status::ready;
		const uint32_t numShutdownsBeforeRelease = source.m_numShutdowns;
		releaseWorker.Run();
		const bool bStopped = shutdown.get();
		const auto response = command.get();
		Require(bStoppedBeforeRelease == bQueued && bCommandCompletedBeforeRelease == bQueued,
			"native shutdown must cancel queued requests without interrupting an executing Editor command");
		Require(numShutdownsBeforeRelease == (bQueued ? 1u : 0u) && bStopped && source.m_numShutdowns == 1u,
			"native teardown must wait only for commands that started executing");
		Require(response.m_bSuccess != bQueued && source.m_numCalls == (bQueued ? 0u : 1u),
			"a cancelled task must not enter the scene callback when its worker later resumes");
		Require(bQueued ? !response.m_error.empty() : source.m_capacity == 7u,
			"cancellation must report failure; completed work must receive the original request payload");
	}
}

SAILOR_SHARED_API int RunEditorEngineProtocolTests()
{
	try
	{
		TestGoldenProtocolWire();
		TestInvalidArgumentsResetOutputs();
		TestViewportEvidenceRequestReportsNoFrame();
		TestOversizedAndMalformedPayloads();
		TestCommandExceptionIsContainedByTransportBoundary();
		TestEnvelopeValidation();
		TestAnimatorParameterRequiresTypedValue();
		TestAnimatorStateWithoutEditor();
		TestStrictInstanceIdProtocolGate();
		TestModelInstanceWireContract();
		TestEditorSimulationWireContract();
		TestEditorStatsModeWireContract();
		TestEditorRenderModeWireContract();
		TestAudioPreviewWireContract();
		TestModelFingerprintProtocol();
		TestPathTracedExportIsUnsupported();
		TestEmbeddedNullIsRejected();
		TestUtf8StringIsAccepted();
		TestGetExitCodeRoundTripAndFree();
		TestViewportEventBatchKeepsOrderAndOwnsPayload();
		TestViewportTransformPreservesValuesAndToolState();
		TestViewportAssetDropEventIsTypedAndValidated();
		TestViewportToolShortcutEventIsTypedAndValidated();
		TestLifecycleGateDrainsStartAndOperationsBeforeShutdown();
		TestFailedShutdownKeepsAdmissionClosedUntilRetry();
		TestFailedInitializationRequiresRollbackBeforeRetry();
		TestInitializationSupersededByShutdownIsNotAcknowledged();
		TestShutdownDrainsAnAdmittedStop();
		TestStopWaitsForInitializationAndSkipsClosedSessions();
		TestThrowingStopReleasesItsOperation();
		TestLocalHostDrainStopsBeforeWaitingForStart();
		TestLocalHostDrainHasOneShutdownOwner();
		TestStartAcknowledgesBeforeWorkerExitAndStopJoins();
		TestImmediateStopAfterStartAcknowledgementCannotBeLost();
		TestShutdownStopsAndJoinsWorkerBeforeShutdownRoutine();
		TestEditorWorkerExceptionIsRethrownOnInvoker();
		TestEditorCommandDoesNotBlockLifecycleDispatch();
		TestShutdownOrderingRejectsCleanupAssistedCompletion();
		TestNativeStopCancelsQueuedEditorRequests(false);
		TestNativeStopCancelsQueuedEditorRequests(true);
	}
	catch (const std::exception& exception)
	{
		std::cerr << "[FAIL] EditorEngineProtocolTests: " << exception.what() << std::endl;
		return 1;
	}

	std::cout << "[PASS] EditorEngineProtocolTests" << std::endl;
	return 0;
}
