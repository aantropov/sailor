#pragma once

#include "AssetRegistry/GlobalIllumination/ProbeVolumeData.h"
#include "Engine/GlobalIlluminationSettings.h"
#include "Memory/SharedPtr.hpp"

#include <cstdint>
#include <limits>
#include <string>

#include <glm/glm.hpp>

namespace Sailor::RHI
{
	struct SAILOR_SHARED_API RHIGlobalIlluminationState final
	{
		std::string m_name{};
		FileId m_asset{};
		ProbeVolumeDataPtr m_data{};
		float m_effectiveWeight = 0.0f;
		EGlobalIlluminationProbeMode m_mode =
			EGlobalIlluminationProbeMode::Blend;
	};

	struct SAILOR_SHARED_API RHIGlobalIlluminationSnapshot final
	{
		uint64_t m_generation = 0u;
		uint64_t m_lightingHash = 0u;
		ProbeVolumeDataPtr m_layout{};
		TVector<RHIGlobalIlluminationState> m_states{};
		uint32_t m_qualityBudget = 0u;
	};

	using RHIGlobalIlluminationSnapshotPtr =
		TSharedPtr<RHIGlobalIlluminationSnapshot>;

	struct SAILOR_SHARED_API RHIGlobalIlluminationRenderStats final
	{
		uint64_t m_activeRevision = 0u;
		// Unique immutable baked payload referenced by the active CPU snapshot.
		uint64_t m_cpuPayloadBytes = 0u;
		// Requested SSBO capacity owned by the current submission flight.
		uint64_t m_gpuAllocatedBytes = 0u;
		// Transient packed and uploaded bytes recorded for the current frame.
		uint64_t m_copiedCpuBytes = 0u;
		uint64_t m_uploadedGpuBytes = 0u;
		uint32_t m_flightSlot = (std::numeric_limits<uint32_t>::max)();
		// Complete snapshots are atomic: every brick is resident or none are.
		uint32_t m_loadedBricks = 0u;
		uint32_t m_totalBricks = 0u;
		uint32_t m_probeCount = 0u;
		uint32_t m_stateCount = 0u;
		uint32_t m_qualityBudget = 0u;
		EGlobalIlluminationMode m_mode =
			EGlobalIlluminationMode::RealtimeAndBaked;
		bool m_bEnabled = true;
		bool m_bActive = false;
	};

	enum class EGlobalIlluminationDebugVisualization : uint32_t
	{
		Lit = 0u,
		IndirectOnly,
		Probes,
		Bricks,
		Validity,
		Visibility,
		Residency,
		AssetIdentity,
		Fallback
	};

	struct alignas(16) RHIGlobalIlluminationGpuHeader final
	{
		// enabled, node count, brick count, probe count
		glm::uvec4 m_counts{};
		// state count, BVH root, debug visualization, quality budget
		glm::uvec4 m_stateAndDebug{};
		// diffuse GI enabled, world mode, min probe spacing bits, reserved
		glm::uvec4 m_settings{};
		glm::vec4 m_volumeMin{};
		glm::vec4 m_volumeMax{};
		// generation low/high, lighting hash low/high
		glm::uvec4 m_identity{};
	};

	struct alignas(16) RHIGlobalIlluminationGpuBvhNode final
	{
		// W contains the left child, or 0x80000000 | brick index for a leaf.
		glm::vec4 m_minAndLeft{};
		// W contains the right child for an internal node.
		glm::vec4 m_maxAndRight{};
	};

	struct alignas(16) RHIGlobalIlluminationGpuBrick final
	{
		// W stores uint metadata through uintBitsToFloat/floatBitsToUint.
		glm::vec4 m_minAndSubdivision{};
		glm::vec4 m_maxAndFirstProbe{};
		// XYZ contain grid dimensions; W contains the number of valid probes.
		glm::uvec4 m_probeCountsAndValidCount{};
	};

	struct alignas(16) RHIGlobalIlluminationGpuProbe final
	{
		glm::vec4 m_positionAndValidity{};
		glm::vec4 m_environmentVisibility0123{};
		// XY are environment visibility for +Z/-Z. Z stores the six blocked
		// direction bits through uintBitsToFloat/floatBitsToUint.
		glm::vec4 m_environmentVisibility45{};
	};

	struct alignas(16) RHIGlobalIlluminationGpuCoefficients final
	{
		// 27 signed RGB coefficient components packed sequentially as FP16.
		glm::uvec4 m_packed[4]{};
	};

	struct alignas(16) RHIGlobalIlluminationGpuState final
	{
		// effective weight, source mode, deterministic debug hue, reserved
		glm::vec4 m_parameters{};
		// asset hash low/high, baked lighting hash low/high
		glm::uvec4 m_identity{};
	};

	struct SAILOR_SHARED_API RHIGlobalIlluminationGpuLayout final
	{
		TVector<RHIGlobalIlluminationGpuBvhNode> m_nodes{};
		TVector<RHIGlobalIlluminationGpuBrick> m_bricks{};
		TVector<RHIGlobalIlluminationGpuProbe> m_probes{};
	};

	SAILOR_SHARED_API uint64_t ComputeGlobalIlluminationLayoutSignature(
		const RHIGlobalIlluminationSnapshot& snapshot) noexcept;
	SAILOR_SHARED_API uint64_t ComputeGlobalIlluminationCoefficientSignature(
		const RHIGlobalIlluminationSnapshot& snapshot) noexcept;
	SAILOR_SHARED_API uint64_t ComputeGlobalIlluminationStateSignature(
		const RHIGlobalIlluminationSnapshot& snapshot) noexcept;
	SAILOR_SHARED_API RHIGlobalIlluminationRenderStats
		BuildGlobalIlluminationRenderStats(
			const RHIGlobalIlluminationSnapshot* snapshot) noexcept;
	SAILOR_SHARED_API RHIGlobalIlluminationGpuHeader
		BuildGlobalIlluminationGpuHeader(
			const RHIGlobalIlluminationSnapshot* snapshot,
			EGlobalIlluminationDebugVisualization debugVisualization,
			EGlobalIlluminationMode mode,
			bool bEnabled) noexcept;
	SAILOR_SHARED_API bool BuildGlobalIlluminationGpuLayout(
		const ProbeVolumeData& data,
		RHIGlobalIlluminationGpuLayout& outLayout,
		std::string& outDiagnostic) noexcept;
	SAILOR_SHARED_API bool BuildGlobalIlluminationGpuCoefficients(
		const RHIGlobalIlluminationSnapshot& snapshot,
		TVector<RHIGlobalIlluminationGpuCoefficients>& outCoefficients,
		std::string& outDiagnostic) noexcept;
	SAILOR_SHARED_API bool BuildGlobalIlluminationGpuStates(
		const RHIGlobalIlluminationSnapshot& snapshot,
		TVector<RHIGlobalIlluminationGpuState>& outStates,
		std::string& outDiagnostic) noexcept;

	static_assert(sizeof(RHIGlobalIlluminationGpuHeader) == 96u);
	static_assert(sizeof(RHIGlobalIlluminationGpuBvhNode) == 32u);
	static_assert(sizeof(RHIGlobalIlluminationGpuBrick) == 48u);
	static_assert(sizeof(RHIGlobalIlluminationGpuProbe) == 48u);
	static_assert(sizeof(RHIGlobalIlluminationGpuCoefficients) == 64u);
	static_assert(sizeof(RHIGlobalIlluminationGpuState) == 32u);
}
