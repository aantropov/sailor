#pragma once

#include "Containers/Hash.h"
#include "Containers/Vector.h"
#include "Core/FileRevision.h"

#include <cstdint>
#include <string>
#include <string_view>

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
		uint64_t hash = Fnv1aOffsetBasis;
		HashValue(hash, static_cast<uint64_t>(files.Num()));
		for (const ShaderDependencyFile& file : files)
		{
			if (!file.m_revision.m_bIsValid || file.m_virtualPath.empty() ||
				file.m_winnerIdentity.empty())
			{
				return 0;
			}

			HashValue(hash, std::string_view(file.m_virtualPath));
			HashValue(hash, std::string_view(file.m_winnerIdentity));
			HashValues(
				hash,
				static_cast<uint64_t>(file.m_mountKind),
				static_cast<uint64_t>(
					file.m_revision.m_modificationTimeNanoseconds));
		}

		return hash == 0 ? 1 : hash;
	}
}
