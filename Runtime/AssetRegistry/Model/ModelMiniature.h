#pragma once

#include "AssetRegistry/FileId.h"

#include <cctype>
#include <filesystem>
#include <string_view>

namespace Sailor
{
	struct ModelMiniature final
	{
		static constexpr uint32_t Resolution = 256;
		static constexpr uint32_t TextureResolution = 512;
		static constexpr const char* CacheFolder = "Models";
		static constexpr const char* Extension = ".png";

		static bool IsValidFileIdFilename(std::string_view value)
		{
			const bool bHasBraces =
				value.size() == 38 &&
				value.front() == '{' &&
				value.back() == '}';
			if (!bHasBraces && value.size() != 36)
			{
				return false;
			}

			const size_t offset = bHasBraces ? 1u : 0u;
			for (size_t index = 0; index < 36; ++index)
			{
				const char character = value[offset + index];
				const bool bHyphen =
					index == 8 ||
					index == 13 ||
					index == 18 ||
					index == 23;
				if ((bHyphen && character != '-') ||
					(!bHyphen &&
						std::isxdigit(static_cast<unsigned char>(character)) == 0))
				{
					return false;
				}
			}
			return true;
		}

		static std::filesystem::path GetCachePath(
			const std::filesystem::path& cacheRoot,
			const FileId& fileId)
		{
			const std::string& fileIdString = fileId.ToString();
			if (!IsValidFileIdFilename(fileIdString))
			{
				return {};
			}

			return cacheRoot / CacheFolder / (fileIdString + Extension);
		}
	};
}
