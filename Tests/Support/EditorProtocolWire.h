#pragma once

#include "EditorEngineProtocolInternal.h"
#include "Protocol/Generated/editor_engine.pb.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace Sailor::Tests::ProtocolWire
{
	inline void AppendVarint(std::string& payload, uint64_t value)
	{
		while (value >= 0x80u)
		{
			payload.push_back(static_cast<char>(
				(value & 0x7fu) | 0x80u));
			value >>= 7u;
		}
		payload.push_back(static_cast<char>(value));
	}

	inline void AppendKey(
		std::string& payload,
		const uint32_t fieldNumber,
		const uint8_t wireType)
	{
		AppendVarint(
			payload,
			(static_cast<uint64_t>(fieldNumber) << 3u) | wireType);
	}

	inline void AppendVarintField(
		std::string& payload,
		const uint32_t fieldNumber,
		const uint64_t value)
	{
		AppendKey(payload, fieldNumber, 0u);
		AppendVarint(payload, value);
	}

	inline void AppendBytesField(
		std::string& payload,
		const uint32_t fieldNumber,
		std::string_view value)
	{
		AppendKey(payload, fieldNumber, 2u);
		AppendVarint(payload, value.size());
		payload.append(value);
	}

	inline std::string MakeVersionedRequest(
		const uint32_t version,
		const uint64_t requestId,
		const uint32_t commandField = 0u,
		std::string_view commandPayload = {})
	{
		std::string payload;
		AppendVarintField(payload, 1u, version);
		if (requestId != 0u) AppendVarintField(payload, 2u, requestId);
		if (commandField != 0u) AppendBytesField(payload, commandField, commandPayload);
		return payload;
	}

	inline std::string MakeRequest(
		const uint64_t requestId,
		const uint32_t commandField,
		std::string_view commandPayload = {})
	{
		return MakeVersionedRequest(Protocol::EditorEngineProtocolVersion, requestId, commandField, commandPayload);
	}

	inline bool ReadVarint(
		std::string_view payload,
		size_t& offset,
		uint64_t& outValue)
	{
		outValue = 0u;
		for (uint32_t shift = 0u;
			shift < 64u && offset < payload.size();
			shift += 7u)
		{
			const uint8_t byte = static_cast<uint8_t>(
				payload[offset++]);
			outValue |= static_cast<uint64_t>(byte & 0x7fu) << shift;
			if ((byte & 0x80u) == 0u)
			{
				return true;
			}
		}
		return false;
	}

	inline bool ReadVarint(const uint8_t* data, size_t size, size_t& offset, uint64_t& outValue)
	{
		return ReadVarint(std::string_view(reinterpret_cast<const char*>(data), size), offset, outValue);
	}

	inline bool ReadBytes(
		std::string_view payload,
		size_t& offset,
		std::string_view& outValue)
	{
		uint64_t length = 0u;
		if (!ReadVarint(payload, offset, length) ||
			length > payload.size() - offset)
		{
			return false;
		}

		outValue = payload.substr(offset, static_cast<size_t>(length));
		offset += static_cast<size_t>(length);
		return true;
	}

	inline bool ReadNestedScalar(std::string_view payload, uint64_t& outValue)
	{
		outValue = 0u;
		if (payload.empty()) return true;

		size_t offset = 0u;
		uint64_t key = 0u;
		return ReadVarint(payload, offset, key) && key == 8u &&
			ReadVarint(payload, offset, outValue) && offset == payload.size();
	}

	struct TProtocolResponseWire
	{
		uint64_t m_protocolVersion = 0u;
		uint64_t m_requestId = 0u;
		bool m_bSuccess = false;
		bool m_bSupportsStrictInstanceIds = false;
		std::string m_error{};
		uint32_t m_resultField = 0u;
		std::string m_resultPayload{};
	};

	inline bool ParseResponse(
		std::string_view payload,
		TProtocolResponseWire& outResponse)
	{
		// Only field-number constants are used: generated message instances stay
		// in SailorLib, which owns the single protobuf descriptor registration.
		using Response = sailor::editor::v1::ProtocolResponse;
		outResponse = {};
		size_t offset = 0u;
		while (offset < payload.size())
		{
			uint64_t key = 0u;
			if (!ReadVarint(payload, offset, key))
			{
				return false;
			}

			const uint32_t fieldNumber =
				static_cast<uint32_t>(key >> 3u);
			const uint8_t wireType =
				static_cast<uint8_t>(key & 0x07u);
			if (wireType == 0u)
			{
				uint64_t value = 0u;
				if (!ReadVarint(payload, offset, value))
				{
					return false;
				}
				switch (fieldNumber)
				{
				case Response::kProtocolVersionFieldNumber:
					outResponse.m_protocolVersion = value;
					break;

				case Response::kRequestIdFieldNumber:
					outResponse.m_requestId = value;
					break;

				case Response::kSuccessFieldNumber:
					outResponse.m_bSuccess = value != 0u;
					break;

				case Response::kSupportsStrictInstanceIdsFieldNumber:
					outResponse.m_bSupportsStrictInstanceIds =
						value != 0u;
					break;

				default:
					break;
				}
				continue;
			}
			if (wireType == 2u)
			{
				std::string_view value;
				if (!ReadBytes(payload, offset, value))
				{
					return false;
				}
				if (fieldNumber == Response::kErrorFieldNumber)
				{
					outResponse.m_error = value;
				}
				else if ((fieldNumber >= Response::kEmptyResultFieldNumber &&
					fieldNumber <= Response::kGlobalIlluminationStateResultFieldNumber) ||
					fieldNumber == Response::kModelFingerprintStatusResultFieldNumber)
				{
					outResponse.m_resultField = fieldNumber;
					outResponse.m_resultPayload = value;
				}
				continue;
			}

			// The protocol envelopes currently use only varint and
			// length-delimited fields. Rejecting other wire types keeps this
			// test decoder intentionally small and strict.
			return false;
		}
		return true;
	}
}
