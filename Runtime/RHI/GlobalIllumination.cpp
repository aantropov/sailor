#include "RHI/GlobalIllumination.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <exception>
#include <limits>

#include <glm/gtc/packing.hpp>

using namespace Sailor;
using namespace Sailor::RHI;

namespace
{
	constexpr uint32_t LeafBit = 0x80000000u;
	constexpr float MaxHalfFloat = 65504.0f;

	void HashU32(uint64_t& hash, uint32_t value) noexcept
	{
		for (uint32_t shift = 0u; shift < 32u; shift += 8u)
		{
			hash ^= static_cast<uint8_t>((value >> shift) & 0xffu);
			hash *= 1099511628211ull;
		}
	}

	void HashU64(uint64_t& hash, uint64_t value) noexcept
	{
		HashU32(hash, static_cast<uint32_t>(value));
		HashU32(hash, static_cast<uint32_t>(value >> 32u));
	}

	void HashString(uint64_t& hash, const std::string& value) noexcept
	{
		for (const char character : value)
		{
			hash ^= static_cast<uint8_t>(character);
			hash *= 1099511628211ull;
		}
	}

	float EncodeUint(uint32_t value) noexcept
	{
		return std::bit_cast<float>(value);
	}

	uint32_t ClampAndPackHalf(float first, float second) noexcept
	{
		const glm::vec2 value(
			glm::clamp(first, -MaxHalfFloat, MaxHalfFloat),
			glm::clamp(second, -MaxHalfFloat, MaxHalfFloat));
		return glm::packHalf2x16(value);
	}

	class BvhBuilder final
	{
	public:
		explicit BvhBuilder(const ProbeVolumeData& data) : m_data(data)
		{
			m_indices.Resize(data.m_bricks.Num());
			for (uint32_t index = 0u; index < m_indices.Num(); ++index)
			{
				m_indices[index] = index;
			}
			m_nodes.Reserve(data.m_bricks.Num() * 2u - 1u);
		}

		uint32_t Build(uint32_t start, uint32_t count)
		{
			const uint32_t nodeIndex = static_cast<uint32_t>(m_nodes.Num());
			m_nodes.Add({});

			glm::vec3 boundsMin((std::numeric_limits<float>::max)());
			glm::vec3 boundsMax((std::numeric_limits<float>::lowest)());
			glm::vec3 centroidMin((std::numeric_limits<float>::max)());
			glm::vec3 centroidMax((std::numeric_limits<float>::lowest)());
			for (uint32_t offset = 0u; offset < count; ++offset)
			{
				const ProbeVolumeBrick& brick =
					m_data.m_bricks[m_indices[start + offset]];
				boundsMin = glm::min(boundsMin, brick.m_min);
				boundsMax = glm::max(boundsMax, brick.m_max);
				const glm::vec3 centroid = (brick.m_min + brick.m_max) * 0.5f;
				centroidMin = glm::min(centroidMin, centroid);
				centroidMax = glm::max(centroidMax, centroid);
			}

			RHIGlobalIlluminationGpuBvhNode node;
			node.m_minAndLeft = glm::vec4(boundsMin, 0.0f);
			node.m_maxAndRight = glm::vec4(boundsMax, 0.0f);
			if (count == 1u)
			{
				node.m_minAndLeft.w = EncodeUint(LeafBit | m_indices[start]);
				m_nodes[nodeIndex] = node;
				return nodeIndex;
			}

			const glm::vec3 centroidExtent = centroidMax - centroidMin;
			uint32_t axis = 0u;
			if (centroidExtent.y > centroidExtent.x)
			{
				axis = 1u;
			}
			if (centroidExtent.z > centroidExtent[axis])
			{
				axis = 2u;
			}
			const uint32_t leftCount = count / 2u;
			const uint32_t middle = start + leftCount;
			std::nth_element(
				m_indices.begin() + start,
				m_indices.begin() + middle,
				m_indices.begin() + start + count,
				[this, axis](uint32_t lhs, uint32_t rhs)
				{
					const ProbeVolumeBrick& lhsBrick = m_data.m_bricks[lhs];
					const ProbeVolumeBrick& rhsBrick = m_data.m_bricks[rhs];
					const float lhsCentroid =
						(lhsBrick.m_min[axis] + lhsBrick.m_max[axis]) * 0.5f;
					const float rhsCentroid =
						(rhsBrick.m_min[axis] + rhsBrick.m_max[axis]) * 0.5f;
					return lhsCentroid < rhsCentroid;
				});

			const uint32_t left = Build(start, leftCount);
			const uint32_t right = Build(middle, count - leftCount);
			node.m_minAndLeft.w = EncodeUint(left);
			node.m_maxAndRight.w = EncodeUint(right);
			m_nodes[nodeIndex] = node;
			return nodeIndex;
		}

		TVector<RHIGlobalIlluminationGpuBvhNode> TakeNodes()
		{
			return std::move(m_nodes);
		}

	private:
		const ProbeVolumeData& m_data;
		TVector<uint32_t> m_indices{};
		TVector<RHIGlobalIlluminationGpuBvhNode> m_nodes{};
	};
}

uint64_t Sailor::RHI::ComputeGlobalIlluminationLayoutSignature(
	const RHIGlobalIlluminationSnapshot& snapshot) noexcept
{
	if (!snapshot.m_layout)
	{
		return 0u;
	}
	uint64_t hash = 1469598103934665603ull;
	HashU64(hash, snapshot.m_layout->m_layoutHash);
	HashU64(hash, snapshot.m_layout->m_transportHash);
	HashU64(
		hash,
		static_cast<uint64_t>(reinterpret_cast<uintptr_t>(
			snapshot.m_layout.GetRawPtr())));
	HashU64(hash, snapshot.m_layout->m_bricks.Num());
	HashU64(hash, snapshot.m_layout->m_probes.Num());
	return hash;
}

uint64_t Sailor::RHI::ComputeGlobalIlluminationCoefficientSignature(
	const RHIGlobalIlluminationSnapshot& snapshot) noexcept
{
	uint64_t hash = 1469598103934665603ull;
	HashU64(hash, snapshot.m_states.Num());
	for (const RHIGlobalIlluminationState& state : snapshot.m_states)
	{
		HashU64(
			hash,
			static_cast<uint64_t>(reinterpret_cast<uintptr_t>(
				state.m_data.GetRawPtr())));
		HashU64(hash, state.m_data ? state.m_data->m_lightingHash : 0u);
	}
	return hash;
}

uint64_t Sailor::RHI::ComputeGlobalIlluminationStateSignature(
	const RHIGlobalIlluminationSnapshot& snapshot) noexcept
{
	uint64_t hash = 1469598103934665603ull;
	HashU64(hash, snapshot.m_generation);
	HashU64(hash, snapshot.m_lightingHash);
	HashU32(hash, snapshot.m_qualityBudget);
	for (const RHIGlobalIlluminationState& state : snapshot.m_states)
	{
		HashString(hash, state.m_name);
		HashU64(hash, state.m_asset.GetHash());
		HashU32(hash, std::bit_cast<uint32_t>(state.m_effectiveWeight));
		HashU32(hash, static_cast<uint32_t>(state.m_mode));
	}
	return hash;
}

RHIGlobalIlluminationGpuHeader
Sailor::RHI::BuildGlobalIlluminationGpuHeader(
	const RHIGlobalIlluminationSnapshot* snapshot,
	EGlobalIlluminationDebugVisualization debugVisualization) noexcept
{
	RHIGlobalIlluminationGpuHeader header;
	if (!snapshot || !snapshot->m_layout || snapshot->m_states.IsEmpty())
	{
		header.m_stateAndDebug.z = static_cast<uint32_t>(debugVisualization);
		return header;
	}

	const ProbeVolumeData& layout = *snapshot->m_layout;
	const uint32_t brickCount = static_cast<uint32_t>(layout.m_bricks.Num());
	const uint32_t nodeCount = brickCount > 0u ? brickCount * 2u - 1u : 0u;
	header.m_counts = glm::uvec4(
		1u,
		nodeCount,
		brickCount,
		static_cast<uint32_t>(layout.m_probes.Num()));
	header.m_stateAndDebug = glm::uvec4(
		static_cast<uint32_t>(snapshot->m_states.Num()),
		0u,
		static_cast<uint32_t>(debugVisualization),
		snapshot->m_qualityBudget);
	header.m_volumeMin = glm::vec4(
		layout.m_volumeMin,
		layout.m_bakeSettings.m_normalBias);
	header.m_volumeMax = glm::vec4(
		layout.m_volumeMax,
		layout.m_bakeSettings.m_viewBias);
	header.m_identity = glm::uvec4(
		static_cast<uint32_t>(snapshot->m_generation),
		static_cast<uint32_t>(snapshot->m_generation >> 32u),
		static_cast<uint32_t>(snapshot->m_lightingHash),
		static_cast<uint32_t>(snapshot->m_lightingHash >> 32u));
	return header;
}

bool Sailor::RHI::BuildGlobalIlluminationGpuLayout(
	const ProbeVolumeData& data,
	RHIGlobalIlluminationGpuLayout& outLayout,
	std::string& outDiagnostic) noexcept
{
	outLayout = {};
	outDiagnostic.clear();
	try
	{
		if (!data.Validate(outDiagnostic))
		{
			return false;
		}

		BvhBuilder builder(data);
		builder.Build(0u, static_cast<uint32_t>(data.m_bricks.Num()));
		outLayout.m_nodes = builder.TakeNodes();
		outLayout.m_bricks.Reserve(data.m_bricks.Num());
		for (const ProbeVolumeBrick& source : data.m_bricks)
		{
			RHIGlobalIlluminationGpuBrick brick;
			brick.m_minAndSubdivision = glm::vec4(
				source.m_min,
				EncodeUint(source.m_subdivisionLevel));
			brick.m_maxAndFirstProbe = glm::vec4(
				source.m_max,
				EncodeUint(source.m_firstProbeIndex));
			brick.m_probeCountsAndCount = glm::uvec4(
				source.m_probeCounts,
				source.m_probeCount);
			outLayout.m_bricks.Add(brick);
		}

		outLayout.m_probes.Reserve(data.m_probes.Num());
		for (const ProbeVolumeSample& source : data.m_probes)
		{
			RHIGlobalIlluminationGpuProbe probe;
			probe.m_positionAndValidity = glm::vec4(
				source.m_position,
				source.m_validity);
			probe.m_visibility01 = glm::vec4(
				source.m_visibility[0],
				source.m_visibility[1]);
			probe.m_visibility23 = glm::vec4(
				source.m_visibility[2],
				source.m_visibility[3]);
			probe.m_visibility45 = glm::vec4(
				source.m_visibility[4],
				source.m_visibility[5]);
			outLayout.m_probes.Add(probe);
		}

		outDiagnostic = "packed adaptive probe layout for GPU sampling";
		return true;
	}
	catch (const std::exception& exception)
	{
		outLayout = {};
		outDiagnostic =
			std::string("cannot pack adaptive probe layout: ") + exception.what();
		return false;
	}
	catch (...)
	{
		outLayout = {};
		outDiagnostic = "cannot pack adaptive probe layout: unknown failure";
		return false;
	}
}

bool Sailor::RHI::BuildGlobalIlluminationGpuCoefficients(
	const RHIGlobalIlluminationSnapshot& snapshot,
	TVector<RHIGlobalIlluminationGpuCoefficients>& outCoefficients,
	std::string& outDiagnostic) noexcept
{
	outCoefficients.Clear();
	outDiagnostic.clear();
	try
	{
		if (!snapshot.m_layout || snapshot.m_states.IsEmpty())
		{
			outDiagnostic = "global-illumination snapshot has no resident states";
			return false;
		}
		const size_t probeCount = snapshot.m_layout->m_probes.Num();
		if (probeCount > (std::numeric_limits<size_t>::max)() /
			snapshot.m_states.Num())
		{
			outDiagnostic = "global-illumination coefficient count overflows address space";
			return false;
		}
		outCoefficients.Reserve(probeCount * snapshot.m_states.Num());
		for (const RHIGlobalIlluminationState& state : snapshot.m_states)
		{
			if (!state.m_data || state.m_data->m_probes.Num() != probeCount)
			{
				outCoefficients.Clear();
				outDiagnostic = "global-illumination state has an incompatible probe count";
				return false;
			}
			for (const ProbeVolumeSample& source : state.m_data->m_probes)
			{
				float components[32]{};
				uint32_t componentIndex = 0u;
				for (const glm::vec3& coefficient : source.m_irradiance)
				{
					components[componentIndex++] = coefficient.x;
					components[componentIndex++] = coefficient.y;
					components[componentIndex++] = coefficient.z;
				}
				RHIGlobalIlluminationGpuCoefficients packed;
				for (uint32_t pairIndex = 0u; pairIndex < 16u; ++pairIndex)
				{
					packed.m_packed[pairIndex / 4u][pairIndex % 4u] =
						ClampAndPackHalf(
							components[pairIndex * 2u],
							components[pairIndex * 2u + 1u]);
				}
				outCoefficients.Add(packed);
			}
		}
		outDiagnostic = "packed baked probe coefficients for GPU sampling";
		return true;
	}
	catch (const std::exception& exception)
	{
		outCoefficients.Clear();
		outDiagnostic =
			std::string("cannot pack probe coefficients: ") + exception.what();
		return false;
	}
	catch (...)
	{
		outCoefficients.Clear();
		outDiagnostic = "cannot pack probe coefficients: unknown failure";
		return false;
	}
}

bool Sailor::RHI::BuildGlobalIlluminationGpuStates(
	const RHIGlobalIlluminationSnapshot& snapshot,
	TVector<RHIGlobalIlluminationGpuState>& outStates,
	std::string& outDiagnostic) noexcept
{
	outStates.Clear();
	outDiagnostic.clear();
	try
	{
		outStates.Reserve(snapshot.m_states.Num());
		for (const RHIGlobalIlluminationState& source : snapshot.m_states)
		{
			if (!source.m_data ||
				!std::isfinite(source.m_effectiveWeight) ||
				source.m_effectiveWeight < 0.0f)
			{
				outStates.Clear();
				outDiagnostic = "global-illumination state metadata is invalid";
				return false;
			}
			uint64_t assetHash = 1469598103934665603ull;
			HashString(assetHash, source.m_name);
			HashU64(assetHash, source.m_asset.GetHash());
			const float debugHue = static_cast<float>(assetHash & 0x00ffffffu) /
				static_cast<float>(0x00ffffffu);
			RHIGlobalIlluminationGpuState state;
			state.m_parameters = glm::vec4(
				source.m_effectiveWeight,
				static_cast<float>(source.m_mode),
				debugHue,
				0.0f);
			state.m_identity = glm::uvec4(
				static_cast<uint32_t>(assetHash),
				static_cast<uint32_t>(assetHash >> 32u),
				static_cast<uint32_t>(source.m_data->m_lightingHash),
				static_cast<uint32_t>(source.m_data->m_lightingHash >> 32u));
			outStates.Add(state);
		}
		outDiagnostic = "packed global-illumination state weights";
		return true;
	}
	catch (const std::exception& exception)
	{
		outStates.Clear();
		outDiagnostic =
			std::string("cannot pack probe state metadata: ") + exception.what();
		return false;
	}
	catch (...)
	{
		outStates.Clear();
		outDiagnostic = "cannot pack probe state metadata: unknown failure";
		return false;
	}
}
