#include "RHI/GlobalIllumination.h"

#include "Containers/Hash.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <exception>
#include <limits>
#include <vector>

#include <glm/gtc/packing.hpp>

using namespace Sailor;
using namespace Sailor::RHI;

namespace
{
	constexpr uint32_t LeafBit = 0x80000000u;
	constexpr float MaxHalfFloat = 65504.0f;
	constexpr float PackedCoefficientHeadroom = 60000.0f;

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
		explicit BvhBuilder(const GIProbesData& data) : m_data(data)
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
				const GIProbeBrick& brick =
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
					const GIProbeBrick& lhsBrick = m_data.m_bricks[lhs];
					const GIProbeBrick& rhsBrick = m_data.m_bricks[rhs];
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
		const GIProbesData& m_data;
		TVector<uint32_t> m_indices{};
		TVector<RHIGlobalIlluminationGpuBvhNode> m_nodes{};
	};

	bool OverlapsFace(
		const GIProbeBrick& brick,
		const GIProbeBrick& neighbor,
		uint32_t axis,
		bool bMaximumFace) noexcept
	{
		const auto maxAbsComponent = [](const glm::vec3& value)
			{
				const glm::vec3 absolute = glm::abs(value);
				return glm::max(absolute.x, glm::max(absolute.y, absolute.z));
			};
		const float coordinateMagnitude = glm::max(
			1.0f,
			glm::max(
				maxAbsComponent(brick.m_min),
				glm::max(
					maxAbsComponent(brick.m_max),
					glm::max(
						maxAbsComponent(neighbor.m_min),
						maxAbsComponent(neighbor.m_max)))));
		const float tolerance =
			coordinateMagnitude * std::numeric_limits<float>::epsilon() * 8.0f;
		const float brickFace =
			bMaximumFace ? brick.m_max[axis] : brick.m_min[axis];
		const float neighborFace =
			bMaximumFace ? neighbor.m_min[axis] : neighbor.m_max[axis];
		if (std::abs(brickFace - neighborFace) > tolerance)
		{
			return false;
		}

		for (uint32_t overlapAxis = 0u; overlapAxis < 3u; ++overlapAxis)
		{
			if (overlapAxis == axis)
			{
				continue;
			}
			const float overlapMin = glm::max(
				brick.m_min[overlapAxis],
				neighbor.m_min[overlapAxis]);
			const float overlapMax = glm::min(
				brick.m_max[overlapAxis],
				neighbor.m_max[overlapAxis]);
			if (overlapMax - overlapMin <= tolerance)
			{
				return false;
			}
		}
		return true;
	}

	uint32_t FindAdaptiveFaceMask(
		const GIProbesData& data,
		uint32_t brickIndex) noexcept
	{
		if (brickIndex >= data.m_bricks.Num())
		{
			return 0x3fu;
		}
		const GIProbeBrick& brick = data.m_bricks[brickIndex];
		uint32_t result = 0u;
		for (uint32_t neighborIndex = 0u;
			neighborIndex < data.m_bricks.Num();
			++neighborIndex)
		{
			if (neighborIndex == brickIndex)
			{
				continue;
			}
			const GIProbeBrick& neighbor = data.m_bricks[neighborIndex];
			if (neighbor.m_subdivisionLevel == brick.m_subdivisionLevel)
			{
				continue;
			}
			for (uint32_t axis = 0u; axis < 3u; ++axis)
			{
				if (OverlapsFace(brick, neighbor, axis, false))
				{
					result |= 1u << (axis * 2u);
				}
				if (OverlapsFace(brick, neighbor, axis, true))
				{
					result |= 1u << (axis * 2u + 1u);
				}
			}
		}
		return result;
	}
}

uint64_t Sailor::RHI::ComputeGlobalIlluminationLayoutSignature(
	const RHIGlobalIlluminationSnapshot& snapshot) noexcept
{
	if (!snapshot.m_layout)
	{
		return 0u;
	}
	uint64_t hash = Fnv1aOffsetBasis;
	HashValue(hash, snapshot.m_layout->m_layoutHash);
	HashValue(hash, snapshot.m_layout->m_transportHash);
	HashValue(
		hash,
		static_cast<uint64_t>(reinterpret_cast<uintptr_t>(
			snapshot.m_layout.GetRawPtr())));
	HashValue(hash, static_cast<uint64_t>(snapshot.m_layout->m_bricks.Num()));
	HashValue(hash, static_cast<uint64_t>(snapshot.m_layout->m_probes.Num()));
	return hash;
}

uint64_t Sailor::RHI::ComputeGlobalIlluminationCoefficientSignature(
	const RHIGlobalIlluminationSnapshot& snapshot) noexcept
{
	uint64_t hash = Fnv1aOffsetBasis;
	HashValue(hash, static_cast<uint64_t>(snapshot.m_states.Num()));
	for (const RHIGlobalIlluminationState& state : snapshot.m_states)
	{
		HashValue(
			hash,
			static_cast<uint64_t>(reinterpret_cast<uintptr_t>(
				state.m_data.GetRawPtr())));
		HashValue(hash, state.m_data ? state.m_data->m_lightingHash : 0u);
	}
	return hash;
}

uint64_t Sailor::RHI::ComputeGlobalIlluminationStateSignature(
	const RHIGlobalIlluminationSnapshot& snapshot) noexcept
{
	uint64_t hash = Fnv1aOffsetBasis;
	HashValues(
		hash,
		snapshot.m_generation,
		snapshot.m_lightingHash,
		snapshot.m_qualityBudget);
	for (const RHIGlobalIlluminationState& state : snapshot.m_states)
	{
		HashString(hash, state.m_name);
		HashValues(
			hash,
			static_cast<uint64_t>(state.m_asset.GetHash()),
			std::bit_cast<uint32_t>(state.m_effectiveWeight),
			static_cast<uint32_t>(state.m_mode));
	}
	return hash;
}

RHIGlobalIlluminationRenderStats
Sailor::RHI::BuildGlobalIlluminationRenderStats(
	const RHIGlobalIlluminationSnapshot* snapshot) noexcept
{
	RHIGlobalIlluminationRenderStats stats;
	if (!snapshot)
	{
		return stats;
	}

	stats.m_activeRevision = snapshot->m_generation;
	stats.m_stateCount = static_cast<uint32_t>(snapshot->m_states.Num());
	stats.m_qualityBudget = snapshot->m_qualityBudget;
	if (snapshot->m_layout)
	{
		stats.m_totalBricks = static_cast<uint32_t>(
			snapshot->m_layout->m_bricks.Num());
		stats.m_probeCount = static_cast<uint32_t>(
			snapshot->m_layout->m_probes.Num());
	}

	auto countPayload = [&stats](const GIProbesDataPtr& data)
	{
		if (!data)
		{
			return;
		}
		stats.m_cpuPayloadBytes +=
			static_cast<uint64_t>(data->m_bricks.Num()) *
				sizeof(GIProbeBrick) +
			static_cast<uint64_t>(data->m_probes.Num()) *
				sizeof(GIProbe);
	};
	countPayload(snapshot->m_layout);
	for (size_t stateIndex = 0u;
		stateIndex < snapshot->m_states.Num();
		++stateIndex)
	{
		const GIProbesDataPtr& data = snapshot->m_states[stateIndex].m_data;
		bool bAlreadyCounted = data && data == snapshot->m_layout;
		for (size_t previousIndex = 0u;
			!bAlreadyCounted && previousIndex < stateIndex;
			++previousIndex)
		{
			bAlreadyCounted = data ==
				snapshot->m_states[previousIndex].m_data;
		}
		if (!bAlreadyCounted)
		{
			countPayload(data);
		}
	}

	stats.m_bActive = snapshot->m_layout &&
		stats.m_totalBricks > 0u &&
		stats.m_probeCount > 0u &&
		stats.m_stateCount > 0u &&
		stats.m_stateCount <= stats.m_qualityBudget;
	stats.m_loadedBricks = stats.m_bActive ? stats.m_totalBricks : 0u;
	return stats;
}

RHIGlobalIlluminationGpuHeader
Sailor::RHI::BuildGlobalIlluminationGpuHeader(
	const RHIGlobalIlluminationSnapshot* snapshot,
	EGlobalIlluminationDebugVisualization debugVisualization,
	EGlobalIlluminationMode mode,
	bool bEnabled) noexcept
{
	RHIGlobalIlluminationGpuHeader header;
	header.m_settings = glm::uvec4(
		bEnabled ? 1u : 0u,
		static_cast<uint32_t>(mode),
		0u,
		0u);
	if (!snapshot || !snapshot->m_layout || snapshot->m_states.IsEmpty())
	{
		header.m_stateAndDebug.z = static_cast<uint32_t>(debugVisualization);
		return header;
	}

	const GIProbesData& layout = *snapshot->m_layout;
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
	header.m_settings.z =
		std::bit_cast<uint32_t>(layout.m_bakeSettings.m_minProbeSpacing);
	header.m_identity = glm::uvec4(
		static_cast<uint32_t>(snapshot->m_generation),
		static_cast<uint32_t>(snapshot->m_generation >> 32u),
		static_cast<uint32_t>(snapshot->m_lightingHash),
		static_cast<uint32_t>(snapshot->m_lightingHash >> 32u));
	return header;
}

bool Sailor::RHI::BuildGlobalIlluminationGpuLayout(
	const GIProbesData& data,
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
		std::vector<float> probeVisibilityMaxDistances(
			data.m_probes.Num(),
			data.m_bakeSettings.m_maxRayDistance);
		outLayout.m_bricks.Reserve(data.m_bricks.Num());
		for (uint32_t brickIndex = 0u;
			brickIndex < data.m_bricks.Num();
			++brickIndex)
		{
			const GIProbeBrick& source = data.m_bricks[brickIndex];
			const float visibilityMaxDistance =
				CalculateGIProbeVisibilityMaxDistance(data, source);
			uint32_t validProbeCount = 0u;
			bool bAllProbesFullyValid = true;
			for (uint32_t probeOffset = 0u;
				probeOffset < source.m_probeCount;
				++probeOffset)
			{
				const GIProbe& probe = data.m_probes[
					source.m_firstProbeIndex + probeOffset];
				probeVisibilityMaxDistances[
					source.m_firstProbeIndex + probeOffset] =
					visibilityMaxDistance;
				validProbeCount += probe.m_validity > 0.000001f ? 1u : 0u;
				bAllProbesFullyValid = bAllProbesFullyValid &&
					probe.m_validity >= 0.999999f;
			}
			const uint32_t adaptiveFaceMask = FindAdaptiveFaceMask(
				data,
				brickIndex);
			const uint32_t brickMetadata =
				(source.m_subdivisionLevel &
					GlobalIlluminationBrickSubdivisionMask) |
				(adaptiveFaceMask <<
					GlobalIlluminationBrickAdaptiveFaceShift) |
				(bAllProbesFullyValid
					? GlobalIlluminationBrickFullyValidBit
					: 0u);
			RHIGlobalIlluminationGpuBrick brick;
			brick.m_minAndSubdivision = glm::vec4(
				source.m_min,
				EncodeUint(brickMetadata));
			brick.m_maxAndFirstProbe = glm::vec4(
				source.m_max,
				EncodeUint(source.m_firstProbeIndex));
			brick.m_probeCountsAndValidCount = glm::uvec4(
				source.m_probeCounts,
				validProbeCount);
			outLayout.m_bricks.Add(brick);
		}

		outLayout.m_probes.Reserve(data.m_probes.Num());
		for (uint32_t probeIndex = 0u;
			probeIndex < data.m_probes.Num();
			++probeIndex)
		{
			const GIProbe& source = data.m_probes[probeIndex];
			const float visibilityMaxDistance = (std::max)(
				probeVisibilityMaxDistances[probeIndex],
				0.001f);
			const float visibilityMaxDistanceSquared =
				visibilityMaxDistance * visibilityMaxDistance;
			auto packVisibilityMoments = [&](uint32_t directionIndex)
				{
					const glm::vec2 moments =
						source.m_visibility[directionIndex];
					return ClampAndPackHalf(
						glm::clamp(
							moments.x / visibilityMaxDistance,
							0.0f,
							1.0f),
						glm::clamp(
							moments.y / visibilityMaxDistanceSquared,
							0.0f,
							1.0f));
				};
			RHIGlobalIlluminationGpuProbe probe;
			probe.m_positionAndValidity = glm::vec4(
				source.m_position,
				source.m_validity);
			probe.m_environmentVisibility0123 = glm::vec4(
				source.m_environmentVisibility[0],
				source.m_environmentVisibility[1],
				source.m_environmentVisibility[2],
				source.m_environmentVisibility[3]);
			probe.m_environmentVisibility45 = glm::vec4(
				source.m_environmentVisibility[4],
				source.m_environmentVisibility[5],
				visibilityMaxDistance,
				EncodeUint(
					source.m_flags & GIProbeBlockedDirectionMask));
			probe.m_visibilityMoments0123 = glm::uvec4(
				packVisibilityMoments(0u),
				packVisibilityMoments(1u),
				packVisibilityMoments(2u),
				packVisibilityMoments(3u));
			probe.m_visibilityMoments45 = glm::uvec4(
				packVisibilityMoments(4u),
				packVisibilityMoments(5u),
				0u,
				0u);
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
			for (const GIProbe& source : state.m_data->m_probes)
			{
				float components[32]{};
				uint32_t componentIndex = 0u;
				float maximumMagnitude = 0.0f;
				for (const glm::vec3& coefficient : source.m_irradiance)
				{
					for (glm::length_t channel = 0; channel < 3; ++channel)
					{
						const float value = coefficient[channel];
						if (!std::isfinite(value))
						{
							outCoefficients.Clear();
							outDiagnostic =
								"global-illumination coefficients contain a non-finite value";
							return false;
						}
						components[componentIndex++] = value;
						maximumMagnitude = (std::max)(
							maximumMagnitude,
							std::abs(value));
					}
				}
				const float coefficientScale = (std::max)(
					1.0f,
					maximumMagnitude / PackedCoefficientHeadroom);
				if (!std::isfinite(coefficientScale) ||
					coefficientScale > MaxHalfFloat)
				{
					outCoefficients.Clear();
					outDiagnostic =
						"global-illumination coefficients exceed the GPU HDR encoding range";
					return false;
				}
				for (uint32_t index = 0u; index < componentIndex; ++index)
				{
					components[index] /= coefficientScale;
				}
				components[componentIndex] = coefficientScale;
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
			uint64_t assetHash = Fnv1aOffsetBasis;
			HashString(assetHash, source.m_name);
			HashValue(
				assetHash,
				static_cast<uint64_t>(source.m_asset.GetHash()));
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
