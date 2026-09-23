#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>

namespace Sailor::Tests
{
	class TempDirectory final
	{
	public:
		explicit TempDirectory(const char* label)
		{
			static std::atomic<uint64_t> nextId{ 0 };
			const auto timestamp = std::chrono::steady_clock::now().time_since_epoch().count();
			const auto root = std::filesystem::temp_directory_path();
			const std::string prefix = "sailor-test-" + std::string(label) + "-" +
				std::to_string(timestamp) + "-";
			for (;;)
			{
				auto candidate = root / (prefix + std::to_string(nextId.fetch_add(1, std::memory_order_relaxed)));
				std::error_code error;
				if (std::filesystem::create_directory(candidate, error))
				{
					m_path = std::move(candidate);
					return;
				}
				if (error && error != std::errc::file_exists)
				{
					throw std::filesystem::filesystem_error("Cannot create test directory", candidate, error);
				}
			}
		}

		~TempDirectory()
		{
			std::error_code error;
			std::filesystem::remove_all(m_path, error);
		}

		TempDirectory(const TempDirectory&) = delete;
		TempDirectory& operator=(const TempDirectory&) = delete;

		const std::filesystem::path& Get() const noexcept { return m_path; }
		std::filesystem::path Path(const std::filesystem::path& relative) const { return m_path / relative; }

	private:
		std::filesystem::path m_path;
	};
}
