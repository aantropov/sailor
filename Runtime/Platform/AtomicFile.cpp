#include "Platform/AtomicFile.h"
#include "Platform/AtomicFileTestAccess.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <limits>
#include <system_error>

#if defined(_WIN32)
#include <Windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace
{
	using namespace Sailor::Platform;

	std::atomic<uint64_t> TemporaryFileCounter = 0;

	std::string Quote(const std::string& value)
	{
		return "'" + value + "'";
	}

	uint64_t GetProcessIdentity() noexcept
	{
#if defined(_WIN32)
		return static_cast<uint64_t>(GetCurrentProcessId());
#else
		return static_cast<uint64_t>(getpid());
#endif
	}

	std::filesystem::path MakeTemporaryPath(const std::filesystem::path& target)
	{
		const uint64_t counter = TemporaryFileCounter.fetch_add(1, std::memory_order_relaxed) + 1;
		const auto timestamp = std::chrono::steady_clock::now().time_since_epoch().count();
		std::filesystem::path filename = target.filename();
		filename += "." + std::to_string(GetProcessIdentity()) + "." +
			std::to_string(timestamp) + "." +
			std::to_string(counter) + ".tmp";
		return target.parent_path() / filename;
	}

	class TemporaryFileCleanup final
	{
	public:
		explicit TemporaryFileCleanup(std::filesystem::path path) : m_path(std::move(path)) {}

		~TemporaryFileCleanup() noexcept
		{
			if (!m_bReleased)
			{
				std::error_code error;
				std::filesystem::remove(m_path, error);
			}
		}

		void Release() noexcept { m_bReleased = true; }

	private:
		std::filesystem::path m_path;
		bool m_bReleased = false;
	};

#if defined(_WIN32)
	std::string WindowsErrorMessage(DWORD error)
	{
		return std::system_category().message(static_cast<int>(error));
	}

	bool WriteTemporaryFile(
		const std::filesystem::path& temporaryPath,
		const void* data,
		uint64_t size,
		std::string& outDiagnostic)
	{
		HANDLE file = CreateFileW(
			temporaryPath.c_str(),
			GENERIC_WRITE,
			0,
			nullptr,
			CREATE_NEW,
			FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_WRITE_THROUGH,
			nullptr);
		if (file == INVALID_HANDLE_VALUE)
		{
			const DWORD error = GetLastError();
			outDiagnostic = "Cannot create file temporary file " +
				Quote(temporaryPath.generic_string()) + ": " + WindowsErrorMessage(error) + ".";
			return false;
		}

		const uint8_t* cursor = static_cast<const uint8_t*>(data);
		uint64_t remaining = size;
		while (remaining > 0)
		{
			const DWORD chunk = static_cast<DWORD>((std::min)(
				remaining,
				static_cast<uint64_t>((std::numeric_limits<DWORD>::max)())));
			DWORD written = 0;
			if (!WriteFile(file, cursor, chunk, &written, nullptr) || written != chunk)
			{
				const DWORD error = GetLastError();
				CloseHandle(file);
				outDiagnostic = "Cannot write file temporary file " +
					Quote(temporaryPath.generic_string()) + ": " + WindowsErrorMessage(error) + ".";
				return false;
			}
			cursor += written;
			remaining -= written;
		}

		if (!FlushFileBuffers(file))
		{
			const DWORD error = GetLastError();
			CloseHandle(file);
			outDiagnostic = "Cannot flush file temporary file " +
				Quote(temporaryPath.generic_string()) + ": " + WindowsErrorMessage(error) + ".";
			return false;
		}

		if (!CloseHandle(file))
		{
			const DWORD error = GetLastError();
			outDiagnostic = "Cannot close file temporary file " +
				Quote(temporaryPath.generic_string()) + ": " + WindowsErrorMessage(error) + ".";
			return false;
		}

		return true;
	}
#else
	std::string PosixErrorMessage(int error)
	{
		return std::generic_category().message(error);
	}

	bool WriteTemporaryFile(
		const std::filesystem::path& temporaryPath,
		const void* data,
		uint64_t size,
		std::string& outDiagnostic)
	{
		int flags = O_WRONLY | O_CREAT | O_EXCL;
#if defined(O_CLOEXEC)
		flags |= O_CLOEXEC;
#endif
		const int file = open(temporaryPath.c_str(), flags, 0666);
		if (file < 0)
		{
			const int error = errno;
			outDiagnostic = "Cannot create file temporary file " +
				Quote(temporaryPath.generic_string()) + ": " + PosixErrorMessage(error) + ".";
			return false;
		}

		const uint8_t* cursor = static_cast<const uint8_t*>(data);
		uint64_t remaining = size;
		while (remaining > 0)
		{
			const size_t chunk = static_cast<size_t>((std::min)(
				remaining,
				static_cast<uint64_t>((std::numeric_limits<ssize_t>::max)())));
			const ssize_t written = write(file, cursor, chunk);
			if (written < 0)
			{
				if (errno == EINTR)
				{
					continue;
				}

				const int error = errno;
				close(file);
				outDiagnostic = "Cannot write file temporary file " +
					Quote(temporaryPath.generic_string()) + ": " + PosixErrorMessage(error) + ".";
				return false;
			}
			if (written == 0)
			{
				close(file);
				outDiagnostic = "Cannot write file temporary file " +
					Quote(temporaryPath.generic_string()) + ": the write made no progress.";
				return false;
			}

			cursor += written;
			remaining -= static_cast<uint64_t>(written);
		}

		if (fsync(file) != 0)
		{
			const int error = errno;
			close(file);
			outDiagnostic = "Cannot flush file temporary file " +
				Quote(temporaryPath.generic_string()) + ": " + PosixErrorMessage(error) + ".";
			return false;
		}

		if (close(file) != 0)
		{
			const int error = errno;
			outDiagnostic = "Cannot close file temporary file " +
				Quote(temporaryPath.generic_string()) + ": " + PosixErrorMessage(error) + ".";
			return false;
		}

		return true;
	}

	bool IsUnsupportedDirectorySyncError(int error) noexcept
	{
		return error == EINVAL
#if defined(ENOTSUP)
			|| error == ENOTSUP
#endif
#if defined(EOPNOTSUPP) && (!defined(ENOTSUP) || EOPNOTSUPP != ENOTSUP)
			|| error == EOPNOTSUPP
#endif
			;
	}

	EAtomicWriteResult FlushDirectory(
		const std::filesystem::path& directory,
		std::string& outDiagnostic,
		[[maybe_unused]] EAtomicWriteFailurePoint failurePoint)
	{
		int flags = O_RDONLY;
#if defined(O_CLOEXEC)
		flags |= O_CLOEXEC;
#endif
#if defined(O_DIRECTORY)
		flags |= O_DIRECTORY;
#endif
		const int directoryFile = open(directory.c_str(), flags);
		if (directoryFile < 0)
		{
			const int error = errno;
			outDiagnostic = "File was replaced, but its directory could not be opened for durability sync " +
				Quote(directory.generic_string()) + ": " + PosixErrorMessage(error) + ".";
			return EAtomicWriteResult::Published;
		}

		int syncError = 0;
#if defined(SAILOR_FILE_IO_TEST_HOOKS)
		if (failurePoint == EAtomicWriteFailurePoint::DirectorySyncUnsupported)
		{
			syncError = EINVAL;
		}
		else
#endif
		if (fsync(directoryFile) != 0)
		{
			syncError = errno;
		}
		if (syncError != 0)
		{
			close(directoryFile);
			if (IsUnsupportedDirectorySyncError(syncError))
			{
				outDiagnostic = "File was published; its filesystem does not support directory sync " +
					Quote(directory.generic_string()) + ".";
				return EAtomicWriteResult::DirectorySyncUnsupported;
			}
			outDiagnostic = "File was replaced, but its directory durability sync failed " +
				Quote(directory.generic_string()) + ": " + PosixErrorMessage(syncError) + ".";
			return EAtomicWriteResult::Published;
		}

		if (close(directoryFile) != 0)
		{
			const int error = errno;
			outDiagnostic = "File was replaced, but its directory handle could not be closed " +
				Quote(directory.generic_string()) + ": " + PosixErrorMessage(error) + ".";
			return EAtomicWriteResult::Published;
		}

		return EAtomicWriteResult::Synced;
	}
#endif

	EAtomicWriteResult AtomicWriteFileImpl(
		const std::filesystem::path& target,
		const void* data,
		uint64_t size,
		std::string& outDiagnostic,
		[[maybe_unused]] EAtomicWriteFailurePoint failurePoint,
		EAtomicWriteMode writeMode) noexcept
	{
		outDiagnostic.clear();
		if (target.empty() || target.filename().empty())
		{
			outDiagnostic = "Cannot atomically replace file: the target path is empty or has no filename.";
			return EAtomicWriteResult::NotPublished;
		}
		if (size > 0 && data == nullptr)
		{
			outDiagnostic = "Cannot atomically replace file " + Quote(target.generic_string()) +
				": non-empty data has a null address.";
			return EAtomicWriteResult::NotPublished;
		}

		const std::filesystem::path parent = target.parent_path().empty()
			? std::filesystem::path(".")
			: target.parent_path();
		std::error_code directoryError;
		std::filesystem::create_directories(parent, directoryError);
		if (directoryError)
		{
			outDiagnostic = "Cannot create file directory " + Quote(parent.generic_string()) +
				": " + directoryError.message() + ".";
			return EAtomicWriteResult::NotPublished;
		}

		const std::filesystem::path temporaryPath = MakeTemporaryPath(target);
		TemporaryFileCleanup cleanup(temporaryPath);
		if (!WriteTemporaryFile(temporaryPath, data, size, outDiagnostic))
		{
			return EAtomicWriteResult::NotPublished;
		}

#if defined(SAILOR_FILE_IO_TEST_HOOKS)
		if (failurePoint == EAtomicWriteFailurePoint::BeforePublish)
		{
			outDiagnostic = "Injected file replacement failure before replacing " +
				Quote(target.generic_string()) + ".";
			return EAtomicWriteResult::NotPublished;
		}
#endif
#if defined(_WIN32)
		DWORD moveFlags = MOVEFILE_WRITE_THROUGH;
		if (writeMode == EAtomicWriteMode::ReplaceExisting)
		{
			moveFlags |= MOVEFILE_REPLACE_EXISTING;
		}
		if (!MoveFileExW(
			temporaryPath.c_str(),
			target.c_str(),
			moveFlags))
		{
			const DWORD error = GetLastError();
			outDiagnostic = "Cannot atomically publish file " + Quote(target.generic_string()) +
				": " + WindowsErrorMessage(error) + ".";
			return EAtomicWriteResult::NotPublished;
		}
		cleanup.Release();
#if defined(SAILOR_FILE_IO_TEST_HOOKS)
		if (failurePoint == EAtomicWriteFailurePoint::DirectorySync)
		{
			outDiagnostic = "Injected sync confirmation failure after publishing '" + target.generic_string() + "'.";
			return EAtomicWriteResult::Published;
		}
#endif
#else
		if (writeMode == EAtomicWriteMode::FailIfExists)
		{
			if (link(temporaryPath.c_str(), target.c_str()) != 0)
			{
				const int error = errno;
				outDiagnostic = "Cannot atomically create file " + Quote(target.generic_string()) +
					": " + PosixErrorMessage(error) + ".";
				return EAtomicWriteResult::NotPublished;
			}
			bool cleanupFailed = false;
#if defined(SAILOR_FILE_IO_TEST_HOOKS)
			cleanupFailed = failurePoint == EAtomicWriteFailurePoint::TemporaryCleanup;
#endif
			if (cleanupFailed || unlink(temporaryPath.c_str()) != 0)
			{
				const int error = cleanupFailed ? EIO : errno;
				outDiagnostic = "File was created; removing its temporary link failed " +
					Quote(temporaryPath.generic_string()) + ": " + PosixErrorMessage(error) + ". Retrying cleanup on exit.";
			}
			else
			{
				cleanup.Release();
			}
		}
		else if (rename(temporaryPath.c_str(), target.c_str()) != 0)
		{
			const int error = errno;
			outDiagnostic = "Cannot atomically replace file " + Quote(target.generic_string()) +
				": " + PosixErrorMessage(error) + ".";
			return EAtomicWriteResult::NotPublished;
		}
		else
		{
			cleanup.Release();
		}
#if defined(SAILOR_FILE_IO_TEST_HOOKS)
		if (failurePoint == EAtomicWriteFailurePoint::DirectorySync)
		{
			outDiagnostic = "Injected directory sync failure after publishing '" + target.generic_string() + "'.";
			return EAtomicWriteResult::Published;
		}
#endif
		std::string syncDiagnostic;
		const auto syncResult = FlushDirectory(parent, syncDiagnostic, failurePoint);
		if (syncResult != EAtomicWriteResult::Synced)
		{
			outDiagnostic += (outDiagnostic.empty() ? "" : " ") + syncDiagnostic;
			return syncResult;
		}
#endif

		if (outDiagnostic.empty())
		{
			outDiagnostic = writeMode == EAtomicWriteMode::FailIfExists ?
				"Atomically created file " + Quote(target.generic_string()) + "." :
				"Atomically replaced file " + Quote(target.generic_string()) + ".";
		}
		return EAtomicWriteResult::Synced;
	}
}

Sailor::Platform::EAtomicWriteResult Sailor::Platform::AtomicWriteFile(
	const std::filesystem::path& target,
	const void* data,
	uint64_t size,
	std::string& outDiagnostic,
	EAtomicWriteMode mode) noexcept
{
	return AtomicWriteFileImpl(target, data, size, outDiagnostic, EAtomicWriteFailurePoint::None, mode);
}

#if defined(SAILOR_FILE_IO_TEST_HOOKS)
Sailor::Platform::EAtomicWriteResult Sailor::Platform::AtomicWriteFileForTests(
	const std::filesystem::path& target,
	const void* data,
	uint64_t size,
	std::string& outDiagnostic,
	EAtomicWriteFailurePoint failurePoint,
	EAtomicWriteMode mode) noexcept
{
	return AtomicWriteFileImpl(target, data, size, outDiagnostic, failurePoint, mode);
}
#endif
