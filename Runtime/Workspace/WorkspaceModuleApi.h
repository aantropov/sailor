#pragma once

#include <cstdint>
#include <type_traits>

#if defined(_MSC_VER)
#include <yvals_core.h>
#endif

#if defined(_WIN32)
#define SAILOR_WORKSPACE_CALL __cdecl
#if defined(SAILOR_WORKSPACE_MODULE_EXPORTS)
#define SAILOR_WORKSPACE_MODULE_EXPORT __declspec(dllexport)
#else
#define SAILOR_WORKSPACE_MODULE_EXPORT
#endif
#elif defined(__GNUC__) || defined(__clang__)
#define SAILOR_WORKSPACE_CALL
#define SAILOR_WORKSPACE_MODULE_EXPORT __attribute__((visibility("default")))
#else
#define SAILOR_WORKSPACE_CALL
#define SAILOR_WORKSPACE_MODULE_EXPORT
#endif

#define SAILOR_WORKSPACE_ABI_STRINGIFY_IMPL(value) #value
#define SAILOR_WORKSPACE_ABI_STRINGIFY(value) SAILOR_WORKSPACE_ABI_STRINGIFY_IMPL(value)
#define SAILOR_WORKSPACE_ABI_REVISION 2

#if defined(_M_X64) || defined(__x86_64__)
#define SAILOR_WORKSPACE_ABI_ARCH "x64"
#elif defined(_M_ARM64) || defined(__aarch64__)
#define SAILOR_WORKSPACE_ABI_ARCH "arm64"
#elif defined(_M_IX86) || defined(__i386__)
#define SAILOR_WORKSPACE_ABI_ARCH "x86"
#else
#define SAILOR_WORKSPACE_ABI_ARCH "unknown"
#endif

#if defined(__clang__) && defined(_MSC_VER)
#define SAILOR_WORKSPACE_ABI_COMPILER \
	"clang-cl-" SAILOR_WORKSPACE_ABI_STRINGIFY(__clang_major__) "." \
	SAILOR_WORKSPACE_ABI_STRINGIFY(__clang_minor__) "." \
	SAILOR_WORKSPACE_ABI_STRINGIFY(__clang_patchlevel__) \
	"-msvc-" SAILOR_WORKSPACE_ABI_STRINGIFY(_MSC_FULL_VER)
#elif defined(_MSC_VER)
#define SAILOR_WORKSPACE_ABI_COMPILER \
	"msvc-" SAILOR_WORKSPACE_ABI_STRINGIFY(_MSC_FULL_VER)
#elif defined(__clang__)
#define SAILOR_WORKSPACE_ABI_COMPILER \
	"clang-" SAILOR_WORKSPACE_ABI_STRINGIFY(__clang_major__) "." \
	SAILOR_WORKSPACE_ABI_STRINGIFY(__clang_minor__) "." \
	SAILOR_WORKSPACE_ABI_STRINGIFY(__clang_patchlevel__)
#elif defined(__GNUC__)
#define SAILOR_WORKSPACE_ABI_COMPILER \
	"gcc-" SAILOR_WORKSPACE_ABI_STRINGIFY(__GNUC__) "." \
	SAILOR_WORKSPACE_ABI_STRINGIFY(__GNUC_MINOR__) "." \
	SAILOR_WORKSPACE_ABI_STRINGIFY(__GNUC_PATCHLEVEL__)
#else
#define SAILOR_WORKSPACE_ABI_COMPILER "unknown"
#endif

#if defined(_MSC_VER) && defined(_DLL) && defined(_DEBUG)
#define SAILOR_WORKSPACE_ABI_CRT "md-debug"
#elif defined(_MSC_VER) && defined(_DLL)
#define SAILOR_WORKSPACE_ABI_CRT "md"
#elif defined(_MSC_VER) && defined(_MT) && defined(_DEBUG)
#define SAILOR_WORKSPACE_ABI_CRT "mt-debug"
#elif defined(_MSC_VER) && defined(_MT)
#define SAILOR_WORKSPACE_ABI_CRT "mt"
#elif defined(_LIBCPP_VERSION)
#define SAILOR_WORKSPACE_ABI_CRT "libc++-" SAILOR_WORKSPACE_ABI_STRINGIFY(_LIBCPP_VERSION)
#elif defined(__GLIBCXX__) && defined(_GLIBCXX_USE_CXX11_ABI)
#define SAILOR_WORKSPACE_ABI_CRT \
	"libstdc++-" SAILOR_WORKSPACE_ABI_STRINGIFY(__GLIBCXX__) \
	"-cxx11-" SAILOR_WORKSPACE_ABI_STRINGIFY(_GLIBCXX_USE_CXX11_ABI)
#else
#define SAILOR_WORKSPACE_ABI_CRT "unknown"
#endif

#if defined(_ITERATOR_DEBUG_LEVEL)
#define SAILOR_WORKSPACE_ABI_ITERATOR SAILOR_WORKSPACE_ABI_STRINGIFY(_ITERATOR_DEBUG_LEVEL)
#elif defined(_GLIBCXX_DEBUG)
#define SAILOR_WORKSPACE_ABI_ITERATOR "glibcxx-debug"
#elif defined(_LIBCPP_HARDENING_MODE)
#define SAILOR_WORKSPACE_ABI_ITERATOR SAILOR_WORKSPACE_ABI_STRINGIFY(_LIBCPP_HARDENING_MODE)
#else
#define SAILOR_WORKSPACE_ABI_ITERATOR "0"
#endif

#if defined(_DEBUG)
#define SAILOR_WORKSPACE_ABI_CONFIG "debug"
#elif defined(NDEBUG)
#define SAILOR_WORKSPACE_ABI_CONFIG "release"
#else
#define SAILOR_WORKSPACE_ABI_CONFIG "checked"
#endif

namespace Sailor::Workspace
{
	inline constexpr uint32_t WorkspaceModuleApiVersion = 1;
	inline constexpr uint32_t WorkspaceHostApiVersion = 1;
	inline constexpr uint32_t WorkspaceModuleAbiRevision = SAILOR_WORKSPACE_ABI_REVISION;
	inline constexpr uint32_t WorkspaceTypeMetadataVersion = 1;
	inline constexpr uint32_t WorkspaceTypeDescriptorFlagAmbiguousProperties = 1u << 0;
	inline constexpr uint32_t WorkspaceTypeDescriptorKnownFlags =
		WorkspaceTypeDescriptorFlagAmbiguousProperties;
	inline constexpr const char* WorkspaceModuleApiEntryPointV1 = "SailorGetWorkspaceModuleApiV1";
	inline constexpr const char* WorkspaceTypeMetadataEntryPointV1 = "SailorGetWorkspaceTypeMetadataV1";
	static constexpr char WorkspaceModuleAbiTagV1[] =
		"sailor-workspace-abi-" SAILOR_WORKSPACE_ABI_STRINGIFY(SAILOR_WORKSPACE_ABI_REVISION)
		";arch=" SAILOR_WORKSPACE_ABI_ARCH
		";compiler=" SAILOR_WORKSPACE_ABI_COMPILER
		";crt=" SAILOR_WORKSPACE_ABI_CRT
		";iterator=" SAILOR_WORKSPACE_ABI_ITERATOR
		";config=" SAILOR_WORKSPACE_ABI_CONFIG;
	static constexpr uint64_t WorkspaceModuleAbiTagV1Length = sizeof(WorkspaceModuleAbiTagV1) - 1;

	static constexpr const char* GetWorkspaceModuleAbiTagV1() noexcept
	{
		return WorkspaceModuleAbiTagV1;
	}

	static constexpr uint64_t GetWorkspaceModuleAbiTagV1Length() noexcept
	{
		return WorkspaceModuleAbiTagV1Length;
	}

	enum class EWorkspaceModuleResult : uint32_t
	{
		Success = 0,
		InvalidArgument = 1,
		BufferTooSmall = 2,
		SerializationFailed = 3,
		RegistrationFailed = 4
	};

	using TGetWorkspaceTypeMetadataV1 = uint32_t (SAILOR_WORKSPACE_CALL *)(
		char* destination,
		uint64_t destinationCapacity,
		uint64_t* outPayloadSize) noexcept;

	// Returns the zero-offset Component address at destination, or null after cleaning up on failure.
	using TWorkspacePlacementFactoryV1 = void* (SAILOR_WORKSPACE_CALL *)(void* destination) noexcept;

	struct WorkspaceTypeDescriptorV1
	{
		uint32_t structSize;
		const char* typeName;
		uint64_t typeNameLength;
		const char* baseTypeName;
		uint64_t baseTypeNameLength;
		const void* typeInfo;
		uint64_t typeSize;
		uint64_t typeAlignment;
		const char* canonicalDefaultValues;
		uint64_t canonicalDefaultValuesLength;
		uint32_t flags;
		TWorkspacePlacementFactoryV1 placementFactory;
	};

	using TCollectWorkspaceTypeV1 = uint32_t (SAILOR_WORKSPACE_CALL *)(
		void* context,
		const WorkspaceTypeDescriptorV1* descriptor) noexcept;

	struct WorkspaceHostApiV1
	{
		uint32_t structSize;
		uint32_t apiVersion;
		void* context;
		TCollectWorkspaceTypeV1 collectType;
	};

	using TRegisterWorkspaceTypesV1 = uint32_t (SAILOR_WORKSPACE_CALL *)(
		const WorkspaceHostApiV1* hostApi) noexcept;

	struct WorkspaceModuleApiV1
	{
		uint32_t structSize;
		uint32_t apiVersion;
		const char* moduleName;
		uint64_t moduleNameLength;
		const char* abiTag;
		uint64_t abiTagLength;
		TGetWorkspaceTypeMetadataV1 getMetadata;
		TRegisterWorkspaceTypesV1 registerTypes;
	};

	static_assert(std::is_trivial_v<WorkspaceTypeDescriptorV1> &&
		std::is_standard_layout_v<WorkspaceTypeDescriptorV1>);
	static_assert(std::is_trivial_v<WorkspaceHostApiV1> &&
		std::is_standard_layout_v<WorkspaceHostApiV1>);
	static_assert(std::is_trivial_v<WorkspaceModuleApiV1> &&
		std::is_standard_layout_v<WorkspaceModuleApiV1>);

	using TGetWorkspaceModuleApiV1 = const WorkspaceModuleApiV1* (SAILOR_WORKSPACE_CALL *)() noexcept;
}

#if !defined(SAILOR_WORKSPACE_NO_ENTRY_POINT_DECLARATIONS)
// A null destination with zero capacity queries the exact UTF-8 payload size.
// The payload is not null-terminated and no allocation ownership crosses the module boundary.
extern "C" SAILOR_WORKSPACE_MODULE_EXPORT uint32_t SAILOR_WORKSPACE_CALL SailorGetWorkspaceTypeMetadataV1(
	char* destination,
	uint64_t destinationCapacity,
	uint64_t* outPayloadSize) noexcept;

// The returned table and all pointed-to strings have process lifetime and remain module-owned.
extern "C" SAILOR_WORKSPACE_MODULE_EXPORT const Sailor::Workspace::WorkspaceModuleApiV1* SAILOR_WORKSPACE_CALL
	SailorGetWorkspaceModuleApiV1() noexcept;
#endif

#undef SAILOR_WORKSPACE_ABI_CONFIG
#undef SAILOR_WORKSPACE_ABI_ITERATOR
#undef SAILOR_WORKSPACE_ABI_CRT
#undef SAILOR_WORKSPACE_ABI_COMPILER
#undef SAILOR_WORKSPACE_ABI_ARCH
#undef SAILOR_WORKSPACE_ABI_STRINGIFY
#undef SAILOR_WORKSPACE_ABI_STRINGIFY_IMPL
#undef SAILOR_WORKSPACE_ABI_REVISION
