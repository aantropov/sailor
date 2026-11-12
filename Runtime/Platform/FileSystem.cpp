#include "Platform/FileSystem.h"

#include <algorithm>
#include <array>
#include <fstream>

using namespace Sailor;

TUniquePtr<std::istream> FileSystem::OpenRead(const std::filesystem::path& path)
{
	auto stream = TUniquePtr<std::ifstream>::Make(path, std::ios::binary);
	if (!stream->is_open())
	{
		return {};
	}
	return TUniquePtr<std::istream>(std::move(stream));
}

TUniquePtr<std::ostream> FileSystem::OpenWrite(const std::filesystem::path& path)
{
	auto stream = TUniquePtr<std::ofstream>::Make(path, std::ios::binary | std::ios::trunc);
	if (!stream->is_open())
	{
		return {};
	}
	return TUniquePtr<std::ostream>(std::move(stream));
}

bool FileSystem::ReadText(const std::filesystem::path& path, std::string& outText)
{
	outText.clear();
	auto input = OpenRead(path);
	if (!input)
	{
		return false;
	}
	std::array<char, 16384> bytes;
	while (input->read(bytes.data(), bytes.size()) || input->gcount() != 0)
	{
		outText.append(bytes.data(), static_cast<size_t>(input->gcount()));
	}
	if (input->bad() || !input->eof())
	{
		outText.clear();
		return false;
	}
	return true;
}

uint64_t FileSystem::FileSize(const std::filesystem::path& path, std::error_code& error)
{
	return std::filesystem::file_size(path, error);
}

std::filesystem::file_time_type FileSystem::LastWriteTime(
	const std::filesystem::path& path, std::error_code& error)
{
	return std::filesystem::last_write_time(path, error);
}

std::filesystem::file_status FileSystem::Status(const std::filesystem::path& path, std::error_code& error)
{
	return std::filesystem::status(path, error);
}

bool FileSystem::Enumerate(const std::filesystem::path& directory,
	TVector<std::filesystem::path>& outEntries, std::error_code& error)
{
	outEntries.Clear();
	error.clear();
	for (std::filesystem::directory_iterator current(directory, error), end;
		!error && current != end; current.increment(error))
	{
		outEntries.Add(current->path());
	}
	if (error)
	{
		outEntries.Clear();
		return false;
	}
	std::sort(outEntries.begin(), outEntries.end());
	return true;
}
