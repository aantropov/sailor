#pragma once

#include "Containers/Vector.h"
#include "Core/FileRevision.h"

#include <cstdint>
#include <string>

namespace Sailor
{
	struct ShaderDependencyFile final
	{
		std::string m_virtualPath;
		std::string m_winnerIdentity;
		FileRevision m_revision{};
		uint32_t m_mountKind = 0;
	};

	inline uint64_t CalculateShaderDependencyFingerprint(
		const TVector<ShaderDependencyFile>& files) noexcept
	{
		constexpr uint64_t OffsetBasis = 14695981039346656037ull;
		constexpr uint64_t Prime = 1099511628211ull;
		uint64_t hash = OffsetBasis;

		auto appendByte = [&](uint8_t value)
		{
			hash ^= value;
			hash *= Prime;
		};
		auto appendUint64 = [&](uint64_t value)
		{
			for (uint32_t byte = 0; byte < sizeof(value); ++byte)
			{
				appendByte(static_cast<uint8_t>((value >> (byte * 8)) & 0xffu));
			}
		};
		auto appendString = [&](const std::string& value)
		{
			appendUint64(static_cast<uint64_t>(value.size()));
			for (unsigned char character : value)
			{
				appendByte(character);
			}
		};

		appendUint64(static_cast<uint64_t>(files.Num()));
		for (const ShaderDependencyFile& file : files)
		{
			if (!file.m_revision.m_bIsValid || file.m_virtualPath.empty() ||
				file.m_winnerIdentity.empty())
			{
				return 0;
			}

			appendString(file.m_virtualPath);
			appendString(file.m_winnerIdentity);
			appendUint64(file.m_mountKind);
			appendUint64(static_cast<uint64_t>(file.m_revision.m_modificationTimeNanoseconds));
		}

		return hash == 0 ? 1 : hash;
	}
}
