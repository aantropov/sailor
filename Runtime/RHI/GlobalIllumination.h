#pragma once

#include "AssetRegistry/GlobalIllumination/ProbeVolumeData.h"
#include "Engine/GlobalIlluminationSettings.h"
#include "Memory/SharedPtr.hpp"

#include <cstdint>
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
		glm::uvec4 m_probeCountsAndCount{};
	};

	struct alignas(16) RHIGlobalIlluminationGpuProbe final
	{
		glm::vec4 m_positionAndValidity{};
		glm::vec4 m_visibility01{};
		glm::vec4 m_visibility23{};
		glm::vec4 m_visibility45{};
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
	SAILOR_SHARED_API RHIGlobalIlluminationGpuHeader
		BuildGlobalIlluminationGpuHeader(
			const RHIGlobalIlluminationSnapshot* snapshot,
			EGlobalIlluminationDebugVisualization debugVisualization) noexcept;
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

	static_assert(sizeof(RHIGlobalIlluminationGpuHeader) == 80u);
	static_assert(sizeof(RHIGlobalIlluminationGpuBvhNode) == 32u);
	static_assert(sizeof(RHIGlobalIlluminationGpuBrick) == 48u);
	static_assert(sizeof(RHIGlobalIlluminationGpuProbe) == 64u);
	static_assert(sizeof(RHIGlobalIlluminationGpuCoefficients) == 64u);
	static_assert(sizeof(RHIGlobalIlluminationGpuState) == 32u);
}
