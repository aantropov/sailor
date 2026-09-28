#pragma once

#include "EditorEngineProtocolInternal.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

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
		const std::string& value)
	{
		AppendKey(payload, fieldNumber, 2u);
		AppendVarint(payload, value.size());
		payload.append(value);
	}

	inline std::string MakeRequest(
		const uint64_t requestId,
		const uint32_t commandField,
		const std::string& commandPayload = {})
	{
		std::string payload;
		AppendVarintField(
			payload,
			1u,
			Protocol::EditorEngineProtocolVersion);
		AppendVarintField(payload, 2u, requestId);
		AppendBytesField(payload, commandField, commandPayload);
		return payload;
	}

	inline bool ReadVarint(
		const std::string& payload,
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

	inline bool ReadBytes(
		const std::string& payload,
		size_t& offset,
		std::string& outValue)
	{
		uint64_t length = 0u;
		if (!ReadVarint(payload, offset, length) ||
			length > payload.size() - offset)
		{
			return false;
		}

		outValue.assign(
			payload.data() + offset,
			static_cast<size_t>(length));
		offset += static_cast<size_t>(length);
		return true;
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
		const std::string& payload,
		TProtocolResponseWire& outResponse)
	{
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
				case 1u:
					outResponse.m_protocolVersion = value;
					break;

				case 2u:
					outResponse.m_requestId = value;
					break;

				case 3u:
					outResponse.m_bSuccess = value != 0u;
					break;

				case 5u:
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
				std::string value;
				if (!ReadBytes(payload, offset, value))
				{
					return false;
				}
				if (fieldNumber == 4u)
				{
					outResponse.m_error = std::move(value);
				}
				else if (fieldNumber >= 10u &&
					fieldNumber <= 19u)
				{
					outResponse.m_resultField = fieldNumber;
					outResponse.m_resultPayload = std::move(value);
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
