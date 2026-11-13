#include "Platform/FileSystem.h"
#include "Core/JsonUtils.h"
#include "Memory/WeakPtr.hpp"
#include "Workspace/WorkspacePathEncoding.h"

#include <array>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <sstream>
#include <miniz/miniz.h>
#include <nlohmann/json.hpp>

using namespace Sailor;
using namespace Sailor::FileSystem;

namespace
{
	constexpr std::string_view ArchiveIndexName = ".sailor-files.json";
	constexpr size_t MaxArchiveIndexBytes = 64 * 1024 * 1024;
	std::mutex g_mountMutex;
	TVector<TWeakPtr<ArchiveMount>> g_mounts;

#if defined(_WIN32)
	std::string EntryKey(std::string name)
#else
	const std::string& EntryKey(const std::string& name)
#endif
	{
#if defined(_WIN32)
		std::transform(name.begin(), name.end(), name.begin(), [](unsigned char character)
		{
			return static_cast<char>(std::tolower(character));
		});
#endif
		return name;
	}

	bool ValidEntryName(std::string_view name)
	{
		if (name.empty() || name.front() == '/' || name.back() == '/' || name.find('\\') != name.npos ||
			name.find(':') != name.npos || name.find('\0') != name.npos)
		{
			return false;
		}
		for (size_t start = 0; start < name.size();)
		{
			const size_t end = name.find('/', start);
			const auto part = name.substr(start, end == name.npos ? name.size() - start : end - start);
			if (part.empty() || part == "." || part == ".." || part.back() == '.' || part.back() == ' ')
			{
				return false;
			}
			start = end == name.npos ? name.size() : end + 1;
		}
		// The filesystem contract uses UTF-8, including archives made by other tools.
		(void)nlohmann::json(name).dump();
		return true;
	}

	std::filesystem::file_time_type FromUnixNanoseconds(int64_t value)
	{
		const std::chrono::sys_time<std::chrono::nanoseconds> timestamp{std::chrono::nanoseconds(value)};
#if defined(_MSC_VER)
		return std::chrono::clock_cast<std::chrono::file_clock>(
			std::chrono::time_point_cast<std::filesystem::file_time_type::duration>(timestamp));
#else
		return std::chrono::time_point_cast<std::filesystem::file_time_type::duration>(
			std::chrono::file_clock::from_sys(timestamp));
#endif
	}

	bool ToUnixNanoseconds(std::filesystem::file_time_type timestamp, int64_t& result)
	{
#if defined(_MSC_VER)
		const auto systemTime = std::chrono::clock_cast<std::chrono::system_clock>(timestamp);
#else
		const auto systemTime = std::chrono::file_clock::to_sys(timestamp);
#endif
		const auto count = std::chrono::duration<long double, std::nano>(systemTime.time_since_epoch()).count();
		constexpr long double Limit = 9223372036854775808.0L;
		if (count < -Limit || count >= Limit)
		{
			return false;
		}
		result = std::chrono::duration_cast<std::chrono::nanoseconds>(systemTime.time_since_epoch()).count();
		return true;
	}

	size_t ReadArchiveBytes(void* context, mz_uint64 offset, void* bytes, size_t size)
	{
		if (offset > static_cast<uint64_t>((std::numeric_limits<std::streamoff>::max)()) ||
			size > static_cast<uint64_t>((std::numeric_limits<std::streamsize>::max)()))
		{
			return 0;
		}
		auto& stream = *static_cast<std::istream*>(context);
		stream.clear();
		stream.seekg(static_cast<std::streamoff>(offset));
		stream.read(static_cast<char*>(bytes), static_cast<std::streamsize>(size));
		return static_cast<size_t>(stream.gcount());
	}

	ArchiveMountPtr FindArchive(const std::filesystem::path& path, std::string& relative)
	{
		TVector<ArchiveMountPtr> candidates;
		{
			std::lock_guard lock(g_mountMutex);
			for (size_t index = 0; index < g_mounts.Num();)
			{
				auto mount = g_mounts[index].TryLock();
				if (!mount)
				{
					g_mounts.RemoveAt(index);
					continue;
				}
				candidates.Add(std::move(mount));
				++index;
			}
		}
		if (candidates.IsEmpty())
		{
			return {};
		}
		std::error_code error;
		const auto candidate = std::filesystem::absolute(path, error).lexically_normal();
		if (error)
		{
			return {};
		}
		for (const auto& mount : candidates)
		{
			std::filesystem::path logicalRoot = mount->GetRoot();
			bool within = Workspace::IsPathWithin(logicalRoot, candidate);
			if (!within)
			{
				// Resolve aliases leading to a mount, but never follow loose symlinks
				// underneath it: the archive owns that whole namespace.
				auto prefix = candidate.root_path();
				for (const auto& part : candidate.relative_path())
				{
					prefix /= part;
					const auto resolved = std::filesystem::weakly_canonical(prefix, error);
					if (!error && Workspace::IsPathWithin(logicalRoot, resolved) &&
						Workspace::IsPathWithin(resolved, logicalRoot))
					{
						logicalRoot = prefix;
						within = true;
						break;
					}
				}
			}
			if (within)
			{
				// IsPathWithin compares Windows prefixes without case sensitivity;
				// lexically_relative would compare those same components case-sensitively.
				auto component = candidate.begin();
				for (auto rootComponent = logicalRoot.begin(); rootComponent != logicalRoot.end(); ++rootComponent)
				{
					++component;
				}
				std::filesystem::path tail;
				for (; component != candidate.end(); ++component)
				{
					tail /= *component;
				}
				relative = Workspace::PathToUtf8(tail);
				return mount;
			}
		}
		return {};
	}
}

class ArchiveMount::Impl
{
public:
	class Reader;
	struct Entry
	{
		std::string name;
		uint32_t index = 0;
		uint64_t size = 0;
		std::filesystem::file_time_type timestamp{};
		bool directory = false;
		bool explicitEntry = false;
	};

	~Impl()
	{
		if (initialized)
		{
			mz_zip_reader_end(&zip);
		}
	}

	bool Insert(Entry entry)
	{
		const std::string key = EntryKey(entry.name);
		const auto found = entries.Find(key);
		if (found != entries.end())
		{
			if (!found.Value().directory || !entry.directory ||
				(found.Value().explicitEntry && entry.explicitEntry))
			{
				return false;
			}
			if (entry.explicitEntry)
			{
				found.Value() = std::move(entry);
			}
			return true;
		}
		const auto parent = Workspace::PathToUtf8(Workspace::PathFromUtf8(entry.name).parent_path());
		if (!entry.name.empty())
		{
			Entry directory;
			directory.name = parent;
			directory.directory = true;
			directory.timestamp = entry.timestamp;
			if (!Insert(std::move(directory)))
			{
				return false;
			}
			children[EntryKey(parent)].Add(entry.name);
		}
		entries.Insert(key, std::move(entry));
		return true;
	}

	const Entry* Find(const std::string& name, std::error_code& error) const
	{
		error.clear();
		const auto found = entries.Find(EntryKey(name));
		if (found == entries.end())
		{
			error = std::make_error_code(std::errc::no_such_file_or_directory);
			return nullptr;
		}
		return &found.Value();
	}

	bool Extract(uint32_t index, uint64_t size, std::string& output)
	{
		if (size > (std::numeric_limits<size_t>::max)() ||
			size > static_cast<uint64_t>((std::numeric_limits<std::streamsize>::max)()))
		{
			return false;
		}
		output.resize(static_cast<size_t>(size));
		std::lock_guard lock(readMutex);
		if (!mz_zip_reader_extract_to_mem(&zip, index, output.data(), output.size(), 0))
		{
			output.clear();
			return false;
		}
		return true;
	}

	bool Initialize(const std::filesystem::path& archive, std::string& diagnostic)
	{
		input = TUniquePtr<std::ifstream>::Make(archive, std::ios::binary);
		if (!*input)
		{
			diagnostic = "Cannot open ZIP archive: " + Workspace::PathToUtf8(archive);
			return false;
		}
		std::error_code error;
		const auto size = std::filesystem::file_size(archive, error);
		zip.m_pRead = ReadArchiveBytes;
		zip.m_pIO_opaque = static_cast<std::istream*>(input.GetRawPtr());
		initialized = !error && mz_zip_reader_init(&zip, size, 0);
		if (!initialized)
		{
			diagnostic = "Invalid or unreadable ZIP central directory.";
			return false;
		}
		Entry directory;
		directory.directory = true;
		directory.timestamp = std::filesystem::last_write_time(archive, error);
		Insert(std::move(directory));
		uint32_t indexEntry = (std::numeric_limits<uint32_t>::max)();
		uint64_t indexSize = 0;
		for (uint32_t index = 0; index < mz_zip_reader_get_num_files(&zip); ++index)
		{
			mz_zip_archive_file_stat stat{};
			const mz_uint length = mz_zip_reader_get_filename(&zip, index, nullptr, 0);
			TVector<char> name(length);
			if (length < 2 || !mz_zip_reader_file_stat(&zip, index, &stat) ||
				!mz_zip_reader_get_filename(&zip, index, name.GetData(), length) ||
				std::char_traits<char>::length(name.GetData()) != length - 1 ||
				!stat.m_is_supported || stat.m_is_encrypted ||
				((stat.m_external_attr >> 16) & 0170000) == 0120000)
			{
				diagnostic = "ZIP contains an unsupported entry, encryption, or a symbolic link.";
				return false;
			}
			Entry entry;
			entry.name.assign(name.GetData(), length - 1);
			entry.directory = stat.m_is_directory;
			if (entry.directory && entry.name.back() == '/')
			{
				entry.name.pop_back();
			}
			if (!ValidEntryName(entry.name))
			{
				diagnostic = "ZIP contains an unsafe entry name: " + entry.name;
				return false;
			}
			if (EntryKey(entry.name).starts_with(EntryKey(std::string(ArchiveIndexName)) + "/"))
			{
				diagnostic = "ZIP entry conflicts with the reserved Sailor file index.";
				return false;
			}
			if (EntryKey(entry.name) == EntryKey(std::string(ArchiveIndexName)))
			{
				if (indexEntry != (std::numeric_limits<uint32_t>::max)() || entry.directory)
				{
					diagnostic = "ZIP contains a duplicate or invalid Sailor file index.";
					return false;
				}
				indexEntry = index;
				indexSize = stat.m_uncomp_size;
				continue;
			}
			entry.index = index;
			entry.size = stat.m_uncomp_size;
			entry.timestamp = FromUnixNanoseconds(static_cast<int64_t>(stat.m_time) * 1000000000);
			entry.explicitEntry = true;
			if (!Insert(std::move(entry)))
			{
				diagnostic = "ZIP contains duplicate names or conflicting file/directory entries.";
				return false;
			}
		}
		if (indexEntry != (std::numeric_limits<uint32_t>::max)())
		{
			std::string payload;
			if (indexSize > MaxArchiveIndexBytes || !Extract(indexEntry, indexSize, payload))
			{
				diagnostic = "Cannot read the Sailor ZIP file index.";
				return false;
			}
			const auto document = Utils::ParseJsonWithUniqueKeys(payload);
			if (!document.is_object() || document.size() != 2 || !document.at("schemaVersion").is_number_unsigned() ||
				document.at("schemaVersion") != 1 ||
				!document.at("files").is_object())
			{
				diagnostic = "Invalid Sailor ZIP file index schema.";
				return false;
			}
			const auto& files = document.at("files");
			size_t fileCount = 0;
			for (const auto& pair : entries)
			{
				auto& entry = *pair.m_second;
				if (entry.directory)
				{
					continue;
				}
				++fileCount;
				const auto& metadata = files.at(entry.name);
				if (!metadata.is_object() || metadata.size() != 2 || !metadata.at("size").is_number_unsigned() ||
					metadata.at("size").get<uint64_t>() != entry.size || !metadata.at("modifiedUnixNs").is_number_integer() ||
					(metadata.at("modifiedUnixNs").is_number_unsigned() &&
						metadata.at("modifiedUnixNs").get<uint64_t>() > static_cast<uint64_t>((std::numeric_limits<int64_t>::max)())))
				{
					diagnostic = "Invalid size or timestamp in the Sailor ZIP file index.";
					return false;
				}
				entry.timestamp = FromUnixNanoseconds(metadata.at("modifiedUnixNs").get<int64_t>());
			}
			if (fileCount != files.size())
			{
				diagnostic = "Sailor ZIP file index does not match archive contents.";
				return false;
			}
		}
		return true;
	}

	std::filesystem::path root;
	TUniquePtr<std::ifstream> input;
	mz_zip_archive zip{};
	bool initialized = false;
	std::mutex readMutex;
	TMap<std::string, Entry> entries;
	TMap<std::string, TVector<std::string>> children;
};

class ArchiveMount::Impl::Reader final : public std::istream
{
	class Buffer final : public std::streambuf
	{
	public:
		Buffer(std::istream& owner, TSharedPtr<Impl> archive, uint32_t index, uint64_t size) :
			m_owner(owner), m_archive(std::move(archive)), m_index(index), m_size(size) {}
		~Buffer() override
		{
			std::lock_guard lock(m_archive->readMutex);
			if (m_iterator)
			{
				mz_zip_reader_extract_iter_free(m_iterator);
			}
		}
		void Initialize()
		{
			std::lock_guard lock(m_archive->readMutex);
			Reset();
			if (!m_failed && m_size == 0)
			{
				Finish();
			}
		}

	protected:
		int_type underflow() override
		{
			if (m_failed)
			{
				return traits_type::eof();
			}
			if (gptr() && gptr() < egptr())
			{
				return traits_type::to_int_type(*gptr());
			}
			if (m_next == m_size)
			{
				return traits_type::eof();
			}
			std::lock_guard lock(m_archive->readMutex);
			if (!m_iterator || m_decoderOffset > m_next)
			{
				Reset();
			}
			while (!m_failed && m_decoderOffset < m_next)
			{
				const size_t count = static_cast<size_t>((std::min)(m_next - m_decoderOffset,
					static_cast<uint64_t>(m_bytes.size())));
				Read(count);
			}
			const size_t count = static_cast<size_t>((std::min)(m_size - m_next,
				static_cast<uint64_t>(m_bytes.size())));
			if (!m_failed)
			{
				Read(count);
				if (m_decoderOffset == m_size)
				{
					Finish();
				}
			}
			if (m_failed)
			{
				return traits_type::eof();
			}
			m_next += count;
			setg(m_bytes.data(), m_bytes.data(), m_bytes.data() + count);
			return traits_type::to_int_type(*gptr());
		}

		pos_type seekoff(off_type offset, std::ios::seekdir direction, std::ios::openmode mode) override
		{
			if (!(mode & std::ios::in) || m_failed)
			{
				return pos_type(off_type(-1));
			}
			const uint64_t unread = gptr() ? static_cast<uint64_t>(egptr() - gptr()) : 0;
			const off_type base = static_cast<off_type>(direction == std::ios::beg ? 0 :
				(direction == std::ios::end ? m_size : m_next - unread));
			if ((offset >= 0 && static_cast<uint64_t>(offset) > m_size - static_cast<uint64_t>(base)) ||
				(offset < 0 && offset < -base))
			{
				return pos_type(off_type(-1));
			}
			const uint64_t target = static_cast<uint64_t>(base + offset);
			const uint64_t buffered = eback() ? static_cast<uint64_t>(egptr() - eback()) : 0;
			if (eback() && target >= m_next - buffered && target <= m_next)
			{
				setg(eback(), eback() + (target - (m_next - buffered)), egptr());
			}
			else
			{
				setg(nullptr, nullptr, nullptr);
				m_next = target;
			}
			return pos_type(static_cast<off_type>(target));
		}

		pos_type seekpos(pos_type position, std::ios::openmode mode) override
		{
			return seekoff(static_cast<off_type>(position), std::ios::beg, mode);
		}

	private:
		void Fail()
		{
			m_failed = true;
			m_owner.setstate(std::ios::badbit);
		}
		void Reset()
		{
			if (m_iterator)
			{
				mz_zip_reader_extract_iter_free(m_iterator);
			}
			m_iterator = mz_zip_reader_extract_iter_new(&m_archive->zip, m_index, 0);
			m_decoderOffset = 0;
			if (!m_iterator)
			{
				Fail();
			}
		}
		void Read(size_t count)
		{
			if (mz_zip_reader_extract_iter_read(m_iterator, m_bytes.data(), count) != count)
			{
				Fail();
				return;
			}
			m_decoderOffset += count;
		}
		void Finish()
		{
			// Drain the deflate terminator and validate CRC before publishing the
			// final buffer. Partial/rewound streams do not claim full-file integrity.
			char extra;
			const auto excess = mz_zip_reader_extract_iter_read(m_iterator, &extra, 1);
			const bool valid = mz_zip_reader_extract_iter_free(m_iterator);
			m_iterator = nullptr;
			if (excess != 0 || !valid)
			{
				Fail();
			}
		}
		std::istream& m_owner;
		TSharedPtr<Impl> m_archive;
		uint32_t m_index;
		uint64_t m_size;
		uint64_t m_next = 0;
		uint64_t m_decoderOffset = 0;
		std::array<char, 65536> m_bytes{};
		mz_zip_reader_extract_iter_state* m_iterator = nullptr;
		bool m_failed = false;
	};

public:
	Reader(TSharedPtr<Impl> archive, uint32_t index, uint64_t size) :
		std::istream(nullptr), m_buffer(*this, std::move(archive), index, size)
	{
		rdbuf(&m_buffer);
		m_buffer.Initialize();
	}

private:
	Buffer m_buffer;
};

ArchiveMount::ArchiveMount(TSharedPtr<Impl> implementation) : m_impl(std::move(implementation)) {}
ArchiveMount::~ArchiveMount() = default;
const std::filesystem::path& ArchiveMount::GetRoot() const { return m_impl->root; }

ArchiveMountPtr FileSystem::MountZipArchive(const std::filesystem::path& root,
	const std::filesystem::path& archive, std::string& outDiagnostic)
{
	outDiagnostic.clear();
	try
	{
		auto implementation = TSharedPtr<ArchiveMount::Impl>::Make();
		std::error_code error;
		implementation->root = std::filesystem::weakly_canonical(std::filesystem::absolute(root), error);
		if (error || root.empty())
		{
			outDiagnostic = "Cannot resolve the archive mount namespace.";
			return {};
		}
		const auto status = std::filesystem::status(implementation->root, error);
		const bool bMissing = error == std::errc::no_such_file_or_directory ||
			(!error && status.type() == std::filesystem::file_type::not_found);
		if ((!bMissing && (error || !std::filesystem::is_directory(status))) ||
			!implementation->Initialize(archive, outDiagnostic))
		{
			if (outDiagnostic.empty())
			{
				outDiagnostic = "Archive mount root must be a directory namespace.";
			}
			return {};
		}
		auto mount = ArchiveMountPtr(new ArchiveMount(std::move(implementation)));
		std::lock_guard lock(g_mountMutex);
		for (size_t index = 0; index < g_mounts.Num();)
		{
			auto existing = g_mounts[index].TryLock();
			if (!existing)
			{
				g_mounts.RemoveAt(index);
				continue;
			}
			if (Workspace::IsPathWithin(existing->GetRoot(), mount->GetRoot()) ||
				Workspace::IsPathWithin(mount->GetRoot(), existing->GetRoot()))
			{
				outDiagnostic = "ZIP mount roots must not overlap.";
				return {};
			}
			++index;
		}
		g_mounts.Add(TWeakPtr<ArchiveMount>(mount));
		return mount;
	}
	catch (const std::exception& exception)
	{
		outDiagnostic = "Cannot mount ZIP: " + std::string(exception.what());
		return {};
	}
}

TUniquePtr<std::istream> FileSystem::OpenRead(const std::filesystem::path& path)
{
	std::string relative;
	if (auto mount = FindArchive(path, relative))
	{
		std::error_code error;
		const auto* entry = mount->m_impl->Find(relative, error);
		if (!entry || entry->directory ||
			entry->size > static_cast<uint64_t>((std::numeric_limits<std::streamoff>::max)()))
		{
			return {};
		}
		auto stream = TUniquePtr<ArchiveMount::Impl::Reader>::Make(mount->m_impl, entry->index, entry->size);
		if (!*stream)
		{
			return {};
		}
		return TUniquePtr<std::istream>(std::move(stream));
	}
	auto stream = TUniquePtr<std::ifstream>::Make(path, std::ios::binary);
	if (!stream->is_open())
	{
		return {};
	}
	return TUniquePtr<std::istream>(std::move(stream));
}

TUniquePtr<std::ostream> FileSystem::OpenWrite(const std::filesystem::path& path)
{
	if (IsReadOnly(path))
	{
		return {};
	}
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
	std::string relative;
	if (auto mount = FindArchive(path, relative))
	{
		const auto* entry = mount->m_impl->Find(relative, error);
		if (entry && !entry->directory)
		{
			return entry->size;
		}
		if (entry)
		{
			error = std::make_error_code(std::errc::is_a_directory);
		}
		return static_cast<uint64_t>(-1);
	}
	return std::filesystem::file_size(path, error);
}

std::filesystem::file_time_type FileSystem::LastWriteTime(
	const std::filesystem::path& path, std::error_code& error)
{
	std::string relative;
	if (auto mount = FindArchive(path, relative))
	{
		const auto* entry = mount->m_impl->Find(relative, error);
		return entry ? entry->timestamp : std::filesystem::file_time_type::min();
	}
	return std::filesystem::last_write_time(path, error);
}

std::filesystem::file_status FileSystem::Status(const std::filesystem::path& path, std::error_code& error)
{
	std::string relative;
	if (auto mount = FindArchive(path, relative))
	{
		const auto* entry = mount->m_impl->Find(relative, error);
		return std::filesystem::file_status(entry ? (entry->directory ? std::filesystem::file_type::directory :
			std::filesystem::file_type::regular) : std::filesystem::file_type::not_found,
			std::filesystem::perms::owner_read | std::filesystem::perms::group_read | std::filesystem::perms::others_read);
	}
	return std::filesystem::status(path, error);
}

bool FileSystem::Enumerate(const std::filesystem::path& directory,
	TVector<std::filesystem::path>& outEntries, std::error_code& error)
{
	outEntries.Clear();
	error.clear();
	std::string relative;
	if (auto mount = FindArchive(directory, relative))
	{
		const auto* entry = mount->m_impl->Find(relative, error);
		if (!entry || !entry->directory)
		{
			if (entry)
			{
				error = std::make_error_code(std::errc::not_a_directory);
			}
			return false;
		}
		const auto children = mount->m_impl->children.Find(EntryKey(relative));
		if (children != mount->m_impl->children.end())
		{
			for (const auto& name : children.Value())
			{
				outEntries.Add(mount->GetRoot() / Workspace::PathFromUtf8(name));
			}
		}
		return true;
	}
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
	return true;
}

bool FileSystem::IsReadOnly(const std::filesystem::path& path)
{
	std::string relative;
	return static_cast<bool>(FindArchive(path, relative));
}

std::filesystem::path FileSystem::Canonical(const std::filesystem::path& path, std::error_code& error)
{
	std::string relative;
	if (auto mount = FindArchive(path, relative))
	{
		if (std::filesystem::exists(Status(path, error)) && !error)
		{
			return relative.empty() ? mount->GetRoot() : mount->GetRoot() / Workspace::PathFromUtf8(relative);
		}
		return {};
	}
	return std::filesystem::canonical(path, error);
}

namespace
{
	class ZipOutput final
	{
	public:
		explicit ZipOutput(std::filesystem::path destination) : path(std::move(destination)) {}
		~ZipOutput()
		{
			if (initialized)
			{
				mz_zip_writer_end(&zip);
			}
			if (file)
			{
				std::fclose(file);
			}
			if (created && !complete)
			{
				std::error_code ignored;
				std::filesystem::remove(path, ignored);
			}
		}

		bool Open()
		{
#if defined(_WIN32)
			_wfopen_s(&file, path.c_str(), L"wbx");
#else
			file = std::fopen(path.c_str(), "wbx");
#endif
			created = file != nullptr;
			initialized = created && mz_zip_writer_init_cfile(&zip, file, MZ_ZIP_FLAG_WRITE_ZIP64);
			return initialized;
		}

		bool Finish()
		{
			if (!mz_zip_writer_finalize_archive(&zip))
			{
				return false;
			}
			const bool ended = mz_zip_writer_end(&zip);
			initialized = false;
			const bool closed = std::fclose(file) == 0;
			file = nullptr;
			complete = ended && closed;
			return complete;
		}

		mz_zip_archive zip{};
		std::filesystem::path path;
		std::FILE* file = nullptr;
		bool initialized = false;
		bool created = false;
		bool complete = false;
	};
}

bool FileSystem::WriteZipArchive(const std::filesystem::path& source,
	const std::filesystem::path& destination, EArchiveCompression compression, std::string& outDiagnostic)
{
	outDiagnostic.clear();
	try
	{
		int level = 0;
		switch (compression)
		{
		case EArchiveCompression::Stored:
			level = 0;
			break;
		case EArchiveCompression::Fastest:
			level = 1;
			break;
		case EArchiveCompression::Balanced:
			level = 6;
			break;
		case EArchiveCompression::Smallest:
			level = 9;
			break;
		default:
			outDiagnostic = "Unsupported ZIP compression option.";
			return false;
		}
		std::error_code error;
		const auto root = std::filesystem::canonical(source, error);
		if (error || !std::filesystem::is_directory(root) || IsReadOnly(root))
		{
			outDiagnostic = "ZIP source must be a native directory.";
			return false;
		}
		const auto output = std::filesystem::weakly_canonical(destination, error);
		if (error || destination.filename().empty() || Workspace::IsPathWithin(root, output) || IsReadOnly(output))
		{
			outDiagnostic = "ZIP output must be outside the source and writable content mounts.";
			return false;
		}
		TVector<std::filesystem::path> paths;
		for (std::filesystem::recursive_directory_iterator current(root), end; current != end; ++current)
		{
			const auto status = current->symlink_status();
			if (std::filesystem::is_symlink(status) ||
				(!std::filesystem::is_regular_file(status) && !std::filesystem::is_directory(status)))
			{
				outDiagnostic = "ZIP source contains a symbolic link or special file: " + Workspace::PathToUtf8(current->path());
				return false;
			}
			const std::string name = Workspace::PathToUtf8(current->path().lexically_relative(root));
			if (!ValidEntryName(name) || EntryKey(name) == EntryKey(std::string(ArchiveIndexName)) ||
				EntryKey(name).starts_with(EntryKey(std::string(ArchiveIndexName)) + "/"))
			{
				outDiagnostic = "Invalid or reserved ZIP source name: " + name;
				return false;
			}
			paths.Add(current->path());
		}
		std::sort(paths.begin(), paths.end());
		ZipOutput writer(output);
		if (!writer.Open())
		{
			outDiagnostic = "Cannot create ZIP output; the destination must be a new file in an existing directory.";
			return false;
		}
		nlohmann::json index = { { "schemaVersion", 1 }, { "files", nlohmann::json::object() } };
		for (const auto& path : paths)
		{
			std::string name = Workspace::PathToUtf8(path.lexically_relative(root));
			if (std::filesystem::is_directory(path))
			{
				name += '/';
				if (!mz_zip_writer_add_mem(&writer.zip, name.c_str(), nullptr, 0, 0))
				{
					outDiagnostic = "Cannot add ZIP directory: " + name;
					return false;
				}
				continue;
			}
			const auto timestamp = std::filesystem::last_write_time(path);
			const uint64_t size = std::filesystem::file_size(path);
			int64_t unixNs = 0;
			if (!ToUnixNanoseconds(timestamp, unixNs))
			{
				outDiagnostic = "File timestamp is outside the ZIP index range: " + name;
				return false;
			}
			std::ifstream input(path, std::ios::binary);
			const MZ_TIME_T seconds = static_cast<MZ_TIME_T>(unixNs / 1000000000);
			if (!input || !mz_zip_writer_add_read_buf_callback(&writer.zip, name.c_str(), ReadArchiveBytes,
				static_cast<std::istream*>(&input), size, &seconds, nullptr, 0, static_cast<mz_uint>(level),
				nullptr, 0, nullptr, 0) || std::filesystem::last_write_time(path) != timestamp ||
				std::filesystem::file_size(path) != size)
			{
				outDiagnostic = "Cannot read stable ZIP source contents: " + name;
				return false;
			}
			index["files"][name] = { { "size", size }, { "modifiedUnixNs", unixNs } };
		}
		const std::string payload = index.dump();
		if (payload.size() > MaxArchiveIndexBytes)
		{
			outDiagnostic = "ZIP file index exceeds its supported size.";
			return false;
		}
		if (!mz_zip_writer_add_mem(&writer.zip, ArchiveIndexName.data(), payload.data(), payload.size(), static_cast<mz_uint>(level)) ||
			!writer.Finish())
		{
			outDiagnostic = "Cannot finalize the ZIP archive and its file index.";
			return false;
		}
		return true;
	}
	catch (const std::exception& exception)
	{
		outDiagnostic = "Cannot write ZIP: " + std::string(exception.what());
		return false;
	}
}

bool FileSystem::CopyDirectory(const std::filesystem::path& source,
	const std::filesystem::path& destination, std::string& outDiagnostic)
{
	struct CopyOutput
	{
		std::filesystem::path m_root;
		bool m_bOwned = false;
		bool m_bComplete = false;
		~CopyOutput()
		{
			if (m_bOwned && !m_bComplete)
			{
				std::error_code error;
				std::filesystem::remove_all(m_root, error);
			}
		}
	} output;
	outDiagnostic.clear();
	try
	{
		if (std::filesystem::exists(std::filesystem::symlink_status(destination)))
		{
			outDiagnostic = "The asset copy destination must be new.";
			return false;
		}
		const auto root = std::filesystem::canonical(source);
		output.m_root = std::filesystem::weakly_canonical(destination);
		if (!std::filesystem::is_directory(root) || IsReadOnly(root) || IsReadOnly(output.m_root) ||
			Workspace::IsPathWithin(root, output.m_root))
		{
			outDiagnostic = "Asset copies require native source folders and a destination outside the source tree.";
			return false;
		}
		output.m_bOwned = std::filesystem::create_directory(output.m_root);
		if (!output.m_bOwned)
		{
			outDiagnostic = "Cannot create the asset copy destination.";
			return false;
		}
		for (const auto& entry : std::filesystem::recursive_directory_iterator(root))
		{
			const auto status = entry.symlink_status();
			const auto target = output.m_root / entry.path().lexically_relative(root);
			if (std::filesystem::is_directory(status))
			{
				std::filesystem::create_directory(target);
			}
			else if (std::filesystem::is_regular_file(status))
			{
				const auto timestamp = entry.last_write_time();
				const auto size = entry.file_size();
				std::filesystem::copy_file(entry.path(), target);
				// The private export copy must allow CPU import and rollback on Windows.
				std::filesystem::permissions(target, std::filesystem::perms::owner_write,
					std::filesystem::perm_options::add);
				if (entry.last_write_time() != timestamp || entry.file_size() != size)
				{
					outDiagnostic = "An asset changed while it was being copied: " + Workspace::PathToUtf8(entry.path());
					return false;
				}
				std::filesystem::last_write_time(target, timestamp);
			}
			else
			{
				outDiagnostic = "Symbolic links and special files cannot be exported: " + Workspace::PathToUtf8(entry.path());
				return false;
			}
		}
		output.m_bComplete = true;
		return true;
	}
	catch (const std::exception& exception)
	{
		outDiagnostic = "Cannot copy assets: " + std::string(exception.what());
		return false;
	}
}
