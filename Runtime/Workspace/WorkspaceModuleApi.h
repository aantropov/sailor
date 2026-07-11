#pragma once

#include <cstdint>

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

namespace Sailor::Workspace
{
	inline constexpr uint32_t WorkspaceTypeMetadataVersion = 1;
	inline constexpr const char* WorkspaceTypeMetadataEntryPointV1 = "SailorGetWorkspaceTypeMetadataV1";

	enum class EWorkspaceModuleResult : uint32_t
	{
		Success = 0,
		InvalidArgument = 1,
		BufferTooSmall = 2,
		SerializationFailed = 3
	};

	using TGetWorkspaceTypeMetadataV1 = uint32_t (SAILOR_WORKSPACE_CALL *)(
		char* destination,
		uint64_t destinationCapacity,
		uint64_t* outPayloadSize);
}

// A null destination with zero capacity queries the exact UTF-8 payload size.
// The payload is not null-terminated and no allocation ownership crosses the module boundary.
extern "C" SAILOR_WORKSPACE_MODULE_EXPORT uint32_t SAILOR_WORKSPACE_CALL SailorGetWorkspaceTypeMetadataV1(
	char* destination,
	uint64_t destinationCapacity,
	uint64_t* outPayloadSize) noexcept;
