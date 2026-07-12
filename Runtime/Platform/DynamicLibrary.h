#pragma once

#include "Core/Defines.h"

#include <filesystem>
#include <string>

namespace Sailor::Platform
{
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable: 4251)
#endif

	class SAILOR_SHARED_API DynamicLibrary final
	{
	public:
		DynamicLibrary() noexcept = default;
		explicit DynamicLibrary(const std::filesystem::path& path) noexcept;
		~DynamicLibrary() noexcept;

		DynamicLibrary(const DynamicLibrary&) = delete;
		DynamicLibrary& operator=(const DynamicLibrary&) = delete;

		DynamicLibrary(DynamicLibrary&& other) noexcept;
		DynamicLibrary& operator=(DynamicLibrary&& other) noexcept;

		bool Open(const std::filesystem::path& path) noexcept;
		void* GetSymbol(const char* symbolName) noexcept;
		bool Close() noexcept;

		bool IsOpen() const noexcept { return m_handle != nullptr; }
		const std::filesystem::path& GetPath() const noexcept { return m_path; }
		const std::string& GetError() const noexcept { return m_error; }

	private:
		void SetError(std::string error) noexcept;
		void SetError(const char* error) noexcept;

		void* m_handle = nullptr;
		std::filesystem::path m_path;
		std::string m_error;
	};

#if defined(_MSC_VER)
#pragma warning(pop)
#endif
}
