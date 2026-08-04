#pragma once

#include "Core/Defines.h"
#include "Core/FileRevision.h"
#include "Containers/Map.h"

#include <mutex>
#include <string>

namespace Sailor
{
	// A scan-local snapshot. Every physical source revision is captured once while
	// staging and once more immediately before the staged generation is committed.
	class AssetScanSourceRevisionCache final
	{
	public:
		SAILOR_API bool TryGet(
			const std::string& physicalSourcePath,
			FileRevision& outRevision);
		SAILOR_API bool ValidateAll(
			std::string& outChangedPhysicalPath) const;
		SAILOR_API void Reset();

	private:
		struct Entry final
		{
			std::string m_physicalPath;
			FileRevision m_revision{};
			bool m_bSucceeded = false;
		};

		mutable std::mutex m_mutex;
		TMap<std::string, Entry> m_entries;
	};

	class AssetScanSourceRevisionScope final
	{
	public:
		explicit AssetScanSourceRevisionScope(
			AssetScanSourceRevisionCache& cache) noexcept;
		~AssetScanSourceRevisionScope() noexcept;

		AssetScanSourceRevisionScope(
			const AssetScanSourceRevisionScope&) = delete;
		AssetScanSourceRevisionScope& operator=(
			const AssetScanSourceRevisionScope&) = delete;

	private:
		AssetScanSourceRevisionCache* m_previous = nullptr;
	};

	AssetScanSourceRevisionCache* GetActiveAssetScanSourceRevisionCache() noexcept;
}
