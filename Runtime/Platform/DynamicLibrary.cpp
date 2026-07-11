#include "Platform/DynamicLibrary.h"

#include <cstring>
#include <exception>
#include <iterator>
#include <system_error>
#include <utility>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#else
#include <dlfcn.h>
#endif

namespace
{
	std::string DescribePath(const std::filesystem::path& path)
	{
#if defined(_WIN32)
		const std::wstring& widePath = path.native();
		if (widePath.empty())
		{
			return {};
		}

		const int requiredSize = WideCharToMultiByte(
			CP_UTF8,
			0,
			widePath.data(),
			static_cast<int>(widePath.size()),
			nullptr,
			0,
			nullptr,
			nullptr);

		if (requiredSize <= 0)
		{
			return "<unprintable path>";
		}

		std::string result(static_cast<size_t>(requiredSize), '\0');
		WideCharToMultiByte(
			CP_UTF8,
			0,
			widePath.data(),
			static_cast<int>(widePath.size()),
			result.data(),
			requiredSize,
			nullptr,
			nullptr);
		return result;
#else
		return path.string();
#endif
	}

#if defined(_WIN32)
	std::string DescribeWindowsError(DWORD errorCode)
	{
		wchar_t buffer[1024]{};
		const DWORD length = FormatMessageW(
			FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
			nullptr,
			errorCode,
			MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
			buffer,
			static_cast<DWORD>(std::size(buffer)),
			nullptr);

		std::wstring message(buffer, length);
		while (!message.empty() && (message.back() == L'\r' || message.back() == L'\n' || message.back() == L' '))
		{
			message.pop_back();
		}

		std::string result;
		if (!message.empty())
		{
			const int requiredSize = WideCharToMultiByte(
				CP_UTF8,
				0,
				message.data(),
				static_cast<int>(message.size()),
				nullptr,
				0,
				nullptr,
				nullptr);

			if (requiredSize > 0)
			{
				result.resize(static_cast<size_t>(requiredSize));
				WideCharToMultiByte(
					CP_UTF8,
					0,
					message.data(),
					static_cast<int>(message.size()),
					result.data(),
					requiredSize,
					nullptr,
					nullptr);
			}
		}

		if (!result.empty())
		{
			result += " (Windows error " + std::to_string(errorCode) + ")";
			return result;
		}

		return "Windows error " + std::to_string(errorCode);
	}
#endif
}

Sailor::Platform::DynamicLibrary::DynamicLibrary(const std::filesystem::path& path) noexcept
{
	Open(path);
}

Sailor::Platform::DynamicLibrary::~DynamicLibrary() noexcept
{
	Close();
}

Sailor::Platform::DynamicLibrary::DynamicLibrary(DynamicLibrary&& other) noexcept
	: m_handle(std::exchange(other.m_handle, nullptr))
	, m_path(std::move(other.m_path))
	, m_error(std::move(other.m_error))
{
	other.m_path.clear();
	other.m_error.clear();
}

Sailor::Platform::DynamicLibrary& Sailor::Platform::DynamicLibrary::operator=(DynamicLibrary&& other) noexcept
{
	if (this == &other)
	{
		return *this;
	}

	if (!Close())
	{
		return *this;
	}

	m_handle = std::exchange(other.m_handle, nullptr);
	m_path = std::move(other.m_path);
	m_error = std::move(other.m_error);
	other.m_path.clear();
	other.m_error.clear();
	return *this;
}

bool Sailor::Platform::DynamicLibrary::Open(const std::filesystem::path& path) noexcept
{
	try
	{
		if (!Close())
		{
			return false;
		}

		if (path.empty())
		{
			SetError("Cannot open dynamic library: path is empty.");
			return false;
		}

		std::error_code pathError;
		m_path = std::filesystem::absolute(path, pathError).lexically_normal();
		if (pathError)
		{
			m_path = path;
			SetError(
				"Cannot resolve dynamic library path '" + DescribePath(path) + "': " +
				pathError.message() + ".");
			return false;
		}

#if defined(_WIN32)
		constexpr DWORD safeSearchFlags = LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS;
		HMODULE handle = LoadLibraryExW(m_path.c_str(), nullptr, safeSearchFlags);
		if (handle == nullptr)
		{
			const DWORD errorCode = GetLastError();
			SetError(
				"Failed to load dynamic library '" + DescribePath(m_path) + "': " +
				DescribeWindowsError(errorCode) + ". Rebuild the module and verify its dependent DLLs are available.");
			return false;
		}
		m_handle = static_cast<void*>(handle);
#else
		dlerror();
		m_handle = dlopen(m_path.c_str(), RTLD_NOW | RTLD_LOCAL);
		if (m_handle == nullptr)
		{
			const char* loaderError = dlerror();
			SetError(
				"Failed to load dynamic library '" + DescribePath(m_path) + "': " +
				(loaderError != nullptr ? loaderError : "unknown loader error") + ".");
			return false;
		}
#endif

		m_error.clear();
		return true;
	}
	catch (const std::exception&)
	{
		SetError("Failed to open dynamic library: unexpected standard-library error.");
		return false;
	}
	catch (...)
	{
		SetError("Failed to open dynamic library: unexpected error.");
		return false;
	}
}

void* Sailor::Platform::DynamicLibrary::GetSymbol(const char* symbolName) noexcept
{
	try
	{
		if (symbolName == nullptr || symbolName[0] == '\0')
		{
			SetError("Cannot resolve dynamic-library symbol: name is empty.");
			return nullptr;
		}

		if (!IsOpen())
		{
			SetError("Cannot resolve symbol '" + std::string(symbolName) + "': no dynamic library is open.");
			return nullptr;
		}

#if defined(_WIN32)
		FARPROC symbol = GetProcAddress(static_cast<HMODULE>(m_handle), symbolName);
		if (symbol == nullptr)
		{
			const DWORD errorCode = GetLastError();
			SetError(
				"Failed to resolve symbol '" + std::string(symbolName) + "' from dynamic library '" +
				DescribePath(m_path) + "': " + DescribeWindowsError(errorCode) + ".");
			return nullptr;
		}
		static_assert(sizeof(void*) == sizeof(FARPROC));
		void* result = nullptr;
		std::memcpy(&result, &symbol, sizeof(result));
#else
		dlerror();
		void* result = dlsym(m_handle, symbolName);
		const char* loaderError = dlerror();
		if (loaderError != nullptr)
		{
			SetError(
				"Failed to resolve symbol '" + std::string(symbolName) + "' from dynamic library '" +
				DescribePath(m_path) + "': " + loaderError + ".");
			return nullptr;
		}
#endif

		m_error.clear();
		return result;
	}
	catch (const std::exception&)
	{
		SetError("Failed to resolve dynamic-library symbol: unexpected standard-library error.");
		return nullptr;
	}
	catch (...)
	{
		SetError("Failed to resolve dynamic-library symbol: unexpected error.");
		return nullptr;
	}
}

bool Sailor::Platform::DynamicLibrary::Close() noexcept
{
	if (m_handle == nullptr)
	{
		m_path.clear();
		m_error.clear();
		return true;
	}

	try
	{
#if defined(_WIN32)
		if (FreeLibrary(static_cast<HMODULE>(m_handle)) == 0)
		{
			const DWORD errorCode = GetLastError();
			SetError(
				"Failed to close dynamic library '" + DescribePath(m_path) + "': " +
				DescribeWindowsError(errorCode) + ".");
			return false;
		}
#else
		dlerror();
		if (dlclose(m_handle) != 0)
		{
			const char* loaderError = dlerror();
			SetError(
				"Failed to close dynamic library '" + DescribePath(m_path) + "': " +
				(loaderError != nullptr ? loaderError : "unknown loader error") + ".");
			return false;
		}
#endif

		m_handle = nullptr;
		m_path.clear();
		m_error.clear();
		return true;
	}
	catch (const std::exception&)
	{
		SetError("Failed to close dynamic library: unexpected standard-library error.");
		return false;
	}
	catch (...)
	{
		SetError("Failed to close dynamic library: unexpected error.");
		return false;
	}
}

void Sailor::Platform::DynamicLibrary::SetError(std::string error) noexcept
{
	try
	{
		m_error = std::move(error);
	}
	catch (...)
	{
		m_error.clear();
	}
}

void Sailor::Platform::DynamicLibrary::SetError(const char* error) noexcept
{
	try
	{
		m_error = error;
	}
	catch (...)
	{
		m_error.clear();
	}
}
