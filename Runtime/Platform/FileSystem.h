#pragma once

#include "Core/Defines.h"
#include "Containers/Containers.h"
#include "Memory/UniquePtr.hpp"

#include <filesystem>
#include <istream>
#include <limits>
#include <ostream>
#include <string>
#include <type_traits>

namespace Sailor::FileSystem
{
	// Each reader owns its seekable stream and backing storage, with an independent cursor.
	SAILOR_API TUniquePtr<std::istream> OpenRead(const std::filesystem::path& path);
	SAILOR_API TUniquePtr<std::ostream> OpenWrite(const std::filesystem::path& path);
	SAILOR_API bool ReadText(const std::filesystem::path& path, std::string& outText);
	SAILOR_API uint64_t FileSize(const std::filesystem::path& path, std::error_code& error);
	SAILOR_API std::filesystem::file_time_type LastWriteTime(
		const std::filesystem::path& path, std::error_code& error);
	SAILOR_API std::filesystem::file_status Status(
		const std::filesystem::path& path, std::error_code& error);
	SAILOR_API bool Enumerate(const std::filesystem::path& directory,
		TVector<std::filesystem::path>& outEntries, std::error_code& error);

	template<typename T>
	bool ReadBinary(const std::filesystem::path& path, TVector<T>& outBytes)
	{
		static_assert(std::is_trivially_copyable_v<T>);
		outBytes.Clear();
		auto input = OpenRead(path);
		if (!input)
		{
			return false;
		}
		input->seekg(0, std::ios::end);
		const auto end = input->tellg();
		if (end < 0 || static_cast<uint64_t>(end) > (std::numeric_limits<size_t>::max)() ||
			static_cast<uint64_t>(end) > static_cast<uint64_t>((std::numeric_limits<std::streamsize>::max)()))
		{
			return false;
		}
		const size_t byteCount = static_cast<size_t>(end);
		const size_t count = byteCount / sizeof(T) + (byteCount % sizeof(T) != 0);
		if (count > (std::numeric_limits<size_t>::max)() / sizeof(T))
		{
			return false;
		}
		outBytes.Resize(count);
		if (count != 0)
		{
			// Partial final elements retain the existing rounded binary-read contract.
			outBytes[count - 1] = T{};
		}
		input->seekg(0, std::ios::beg);
		if (byteCount != 0)
		{
			input->read(reinterpret_cast<char*>(outBytes.GetData()), static_cast<std::streamsize>(byteCount));
		}
		if (!*input || (byteCount != 0 && input->gcount() != static_cast<std::streamsize>(byteCount)))
		{
			outBytes.Clear();
			return false;
		}
		return true;
	}
}
