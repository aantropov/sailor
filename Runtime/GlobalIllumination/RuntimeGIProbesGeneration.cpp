#include "GlobalIllumination/RuntimeGIProbesServiceInternal.h"

#include "Containers/Hash.h"
#include "GlobalIllumination/RuntimeGIProbesGrid.h"
#include "Math/Math.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <utility>
#include <vector>

namespace Sailor
{
	using namespace RuntimeGIProbesInternal;

	namespace
	{
		constexpr uint32_t IrradianceBatchSize = 16u;

		uint32_t HashRuntimeProbeCell(const RuntimeGIProbesInternal::ProbeCellKey& key, uint32_t randomSeed) noexcept
		{
			uint64_t hash = Fnv1aOffsetBasis;
			HashValues(
				hash, randomSeed, key.m_worldCell.x, key.m_worldCell.y, key.m_worldCell.z, key.m_geometryGeneration);
			const uint32_t folded = static_cast<uint32_t>(hash ^ (hash >> 32u));
			return folded != 0u ? folded : 0x6d2b79f5u;
		}
	}

	uint32_t RuntimeGIProbesService::Impl::GetProgressSampleCount(const Generation& generation,
		const ProbeWork& probe) noexcept
	{
		return probe.m_bRefined ? generation.m_request.m_qualitySettings.m_targetSamplesPerProbe
								: probe.m_accumulator.m_sampleCount;
	}

	bool RuntimeGIProbesService::Impl::AreTransportSettingsCompatible(const GIProbesBakeSettings& lhs,
		const GIProbesBakeSettings& rhs) noexcept
	{
		return lhs.m_randomSeed == rhs.m_randomSeed && lhs.m_minProbeSpacing == rhs.m_minProbeSpacing &&
			   lhs.m_normalBias == rhs.m_normalBias && lhs.m_viewBias == rhs.m_viewBias &&
			   lhs.m_maxRayDistance == rhs.m_maxRayDistance;
	}

	bool RuntimeGIProbesService::Impl::AreIrradianceSettingsCompatible(const GIProbesBakeSettings& lhs,
		const GIProbesBakeSettings& rhs) noexcept
	{
		return AreTransportSettingsCompatible(lhs, rhs) && lhs.m_raysPerProbe == rhs.m_raysPerProbe &&
			   lhs.m_bounceCount == rhs.m_bounceCount && lhs.m_skyIndirectIntensity == rhs.m_skyIndirectIntensity &&
			   lhs.m_bIncludeSky == rhs.m_bIncludeSky && lhs.m_bIncludeEmissive == rhs.m_bIncludeEmissive &&
			   lhs.m_bIncludeDirectLighting == rhs.m_bIncludeDirectLighting;
	}

	std::vector<uint32_t> RuntimeGIProbesService::Impl::BuildProgressiveProbeOrder(const Generation& generation)
	{
		std::vector<uint32_t> result;
		result.reserve(generation.m_probes.size());
		if (generation.m_data->m_bricks.IsEmpty())
		{
			return result;
		}

		const GIProbeBrick& brick = generation.m_data->m_bricks[0];
		struct OrderedCell final
		{
			glm::uvec3 m_cell{};
			float m_distanceSquared = 0.0f;
		};

		const glm::uvec3 cellCounts = brick.m_probeCounts - glm::uvec3(1u);
		const glm::vec3 cellSpacing = (brick.m_max - brick.m_min) / glm::vec3(cellCounts);
		std::vector<OrderedCell> cells;
		cells.reserve(static_cast<size_t>(cellCounts.x) * cellCounts.y * cellCounts.z);
		for (uint32_t z = 0u; z < cellCounts.z; ++z)
		{
			for (uint32_t y = 0u; y < cellCounts.y; ++y)
			{
				for (uint32_t x = 0u; x < cellCounts.x; ++x)
				{
					const glm::vec3 center = brick.m_min + (glm::vec3(x, y, z) + glm::vec3(0.5f)) * cellSpacing;
					const glm::vec3 delta = center - generation.m_request.m_priorityPosition;
					cells.push_back(OrderedCell{glm::uvec3(x, y, z), glm::dot(delta, delta)});
				}
			}
		}
		std::sort(cells.begin(),
			cells.end(),
			[](const OrderedCell& lhs, const OrderedCell& rhs)
			{
				if (lhs.m_distanceSquared != rhs.m_distanceSquared)
				{
					return lhs.m_distanceSquared < rhs.m_distanceSquared;
				}
				if (lhs.m_cell.z != rhs.m_cell.z)
				{
					return lhs.m_cell.z < rhs.m_cell.z;
				}
				if (lhs.m_cell.y != rhs.m_cell.y)
				{
					return lhs.m_cell.y < rhs.m_cell.y;
				}
				return lhs.m_cell.x < rhs.m_cell.x;
			});

		std::vector<uint8_t> scheduled(generation.m_probes.size(), 0u);
		const auto appendProbe = [&result, &scheduled, &brick](uint32_t x, uint32_t y, uint32_t z)
		{
			const uint32_t probeIndex =
				brick.m_firstProbeIndex + x + brick.m_probeCounts.x * (y + brick.m_probeCounts.y * z);
			if (!scheduled[probeIndex])
			{
				scheduled[probeIndex] = 1u;
				result.push_back(probeIndex);
			}
		};
		for (const OrderedCell& cell : cells)
		{
			for (uint32_t z = 0u; z < 2u; ++z)
			{
				for (uint32_t y = 0u; y < 2u; ++y)
				{
					for (uint32_t x = 0u; x < 2u; ++x)
					{
						appendProbe(cell.m_cell.x + x, cell.m_cell.y + y, cell.m_cell.z + z);
					}
				}
			}
		}
		return result;
	}

	std::shared_ptr<RuntimeGIProbesService::Impl::Generation> RuntimeGIProbesService::Impl::BuildGeneration(
		const RuntimeGIProbesStartRequest& request,
		uint64_t generationId,
		std::string& outDiagnostic)
	{
		outDiagnostic.clear();
		if (!Math::AllFinite(request.m_priorityPosition))
		{
			outDiagnostic = "runtime GI probes require a finite priority position";
			return {};
		}
		const float spacing = request.m_worldSettings.m_minProbeSpacing * request.m_qualitySettings.m_spacingMultiplier;
		if (!std::isfinite(spacing) || spacing <= 0.0f)
		{
			outDiagnostic = "runtime GI probes resolved to invalid probe spacing";
			return {};
		}
		if (!request.m_geometryBounds.IsValid() ||
			glm::any(glm::lessThanEqual(request.m_geometryBounds.m_max, request.m_geometryBounds.m_min)))
		{
			outDiagnostic = "runtime GI probes require finite prepared-scene geometry bounds";
			return {};
		}

		auto generation = std::make_shared<Generation>();
		generation->m_request = request;
		generation->m_id = generationId;
		generation->m_started = std::chrono::steady_clock::now();
		generation->m_data = GIProbesDataPtr::Make();
		GIProbesData& data = *generation->m_data;
		data.m_stateName = "Runtime Experimental";
		data.m_bakerVersion = std::string(GIProbesCurrentBakerVersion);
		data.m_sourceWorldHash = request.m_geometryGeneration;
		data.m_representationHash =
			ComputeGIProbesRepresentationHash(data.m_formatVersion, data.m_shOrder, data.m_compression);
		data.m_bakeSettings = ResolveRuntimeGIProbesBakeSettings(
			request.m_worldSettings, request.m_qualitySettings, request.m_randomSeed);

		const uint32_t publicationLimitedCapacity = ResolvePublicationLimitedCapacity(request.m_qualitySettings);
		if (publicationLimitedCapacity < 8u)
		{
			outDiagnostic = "runtime GI probe upload budget cannot hold the minimum eight-probe grid";
			return {};
		}
		generation->m_effectiveCapacity = publicationLimitedCapacity;
		data.m_probes.Reserve(publicationLimitedCapacity);
		generation->m_probes.reserve(publicationLimitedCapacity);
		generation->m_initialQueue.reserve(publicationLimitedCapacity);
		const float geometryPadding =
			(std::max)(request.m_worldSettings.m_normalBias + request.m_worldSettings.m_viewBias, spacing * 0.01f);
		const ProbeGridBounds geometryBounds{request.m_geometryBounds.m_min - glm::vec3(geometryPadding),
			request.m_geometryBounds.m_max + glm::vec3(geometryPadding)};
		ProbeGrid grid;
		if (!TryBuildSceneProbeGrid(geometryBounds, spacing, publicationLimitedCapacity, grid))
		{
			outDiagnostic = "runtime GI probes could not fit the scene bounds into the active capacity";
			return {};
		}
		data.m_bakeSettings.m_maxSubdivisionLevel = 0u;
		data.m_bakeSettings.m_minProbeSpacing = grid.m_spacing;
		data.m_volumeMin = grid.m_min;
		data.m_volumeMax = grid.m_max;

		const uint32_t probeCount = grid.m_counts.x * grid.m_counts.y * grid.m_counts.z;
		GIProbeBrick brick;
		brick.m_min = grid.m_min;
		brick.m_max = grid.m_max;
		brick.m_subdivisionLevel = 0u;
		brick.m_firstProbeIndex = 0u;
		brick.m_probeCounts = grid.m_counts;
		brick.m_probeCount = probeCount;
		data.m_bricks.Add(brick);
		for (uint32_t z = 0u; z < grid.m_counts.z; ++z)
		{
			for (uint32_t y = 0u; y < grid.m_counts.y; ++y)
			{
				for (uint32_t x = 0u; x < grid.m_counts.x; ++x)
				{
					GIProbe probe;
					probe.m_position = brick.m_min + glm::vec3(x, y, z) * grid.m_spacing;
					probe.m_validity = 0.0f;
					probe.m_flags = 0u;
					data.m_probes.Add(probe);

					ProbeWork work;
					work.m_probe = probe;
					work.m_layoutPosition = probe.m_position;
					work.m_key.m_worldCell = glm::ivec3(x, y, z);
					work.m_key.m_geometryGeneration = request.m_geometryGeneration;
					generation->m_probes.push_back(std::move(work));
				}
			}
		}

		generation->m_initialQueue = BuildProgressiveProbeOrder(*generation);

		data.m_layoutHash = ComputeGIProbesLayoutHash(data);
		generation->m_initialLayoutHash = data.m_layoutHash;
		data.m_diagnostics.m_invalidProbeCount = static_cast<uint32_t>(data.m_probes.Num());
		data.m_diagnostics.m_message = "runtime GI probes are warming; missing cells use environment fallback";
		return generation;
	}

	void RuntimeGIProbesService::Impl::ReuseGenerationState(Generation& next, const Generation& previous)
	{
		std::vector<uint32_t> progressiveOrder = std::move(next.m_initialQueue);
		next.m_initialQueue.reserve(progressiveOrder.size());
		next.m_warmingQueue.clear();
		next.m_refinementQueue.clear();
		next.m_initialCursor = 0u;
		next.m_readyCount = 0u;
		next.m_refinedCount = 0u;
		next.m_dirtyCount = 0u;
		next.m_progressSampleCount = 0u;
		const bool bLayoutCompatible =
			next.m_request.m_geometryGeneration == previous.m_request.m_geometryGeneration &&
			next.m_initialLayoutHash == previous.m_initialLayoutHash &&
			next.m_probes.size() == previous.m_probes.size() &&
			AreTransportSettingsCompatible(previous.m_data->m_bakeSettings, next.m_data->m_bakeSettings);
		for (uint32_t probeIndex = 0u; probeIndex < next.m_probes.size(); ++probeIndex)
		{
			ProbeWork& probe = next.m_probes[probeIndex];
			const GIProbeBrick& brick = next.m_data->m_bricks[probe.m_brickIndex];
			const ProbeWork* previousProbe = bLayoutCompatible ? &previous.m_probes[probeIndex] : nullptr;
			const bool bSamePosition =
				previousProbe &&
				glm::all(glm::lessThanEqual(glm::abs(previousProbe->m_layoutPosition - probe.m_layoutPosition),
					glm::vec3(next.m_data->m_bakeSettings.m_minProbeSpacing * 0.0001f)));
			const bool bPreviousProbeFitsTraceVolume =
				previousProbe && Math::AllFinite(previousProbe->m_probe.m_position) &&
				glm::all(glm::greaterThanEqual(previousProbe->m_probe.m_position, brick.m_min)) &&
				glm::all(glm::lessThanEqual(previousProbe->m_probe.m_position, brick.m_max));
			const bool bReuseTransport =
				previousProbe && previousProbe->m_bHasTransport && bSamePosition && bPreviousProbeFitsTraceVolume;
			if (bReuseTransport)
			{
				probe.m_probe = previousProbe->m_probe;
				probe.m_bHasTransport = true;
				const bool bReuseIrradiance =
					next.m_request.m_lightingGeneration == previous.m_request.m_lightingGeneration &&
					AreIrradianceSettingsCompatible(previous.m_data->m_bakeSettings, next.m_data->m_bakeSettings);
				if (bReuseIrradiance)
				{
					probe.m_accumulator = previousProbe->m_accumulator;
					probe.m_bReady =
						probe.m_probe.m_validity <= 0.05f ||
						probe.m_accumulator.m_sampleCount >= next.m_request.m_qualitySettings.m_initialSamplesPerProbe;
					probe.m_bRefined =
						probe.m_probe.m_validity <= 0.05f ||
						probe.m_accumulator.m_sampleCount >= next.m_request.m_qualitySettings.m_targetSamplesPerProbe;
				}
				else if (probe.m_probe.m_validity <= 0.05f)
				{
					probe.m_bReady = true;
					probe.m_bRefined = true;
				}
				else
				{
					probe.m_probe.m_irradiance = {};
				}
			}

			if (probe.m_bReady)
			{
				next.m_data->m_probes[probeIndex] = probe.m_probe;
				probe.m_bDirty = true;
				++next.m_readyCount;
				++next.m_dirtyCount;
			}
			if (probe.m_bRefined)
			{
				++next.m_refinedCount;
			}
			next.m_progressSampleCount += GetProgressSampleCount(next, probe);
		}
		for (const uint32_t probeIndex : progressiveOrder)
		{
			const ProbeWork& probe = next.m_probes[probeIndex];
			if (!probe.m_bHasTransport)
			{
				next.m_initialQueue.push_back(probeIndex);
			}
			else if (!probe.m_bReady)
			{
				next.m_warmingQueue.push_back(probeIndex);
			}
			else if (!probe.m_bRefined)
			{
				next.m_refinementQueue.push_back(probeIndex);
			}
		}
	}

	bool RuntimeGIProbesService::Impl::ExecuteJob(Job& job, std::string& outDiagnostic)
	{
		Generation& generation = *job.m_generation;
		ProbeWork& work = job.m_work;
		GIProbeTraceRequest traceRequest;
		traceRequest.m_settings = generation.m_data->m_bakeSettings;
		const GIProbeBrick& brick = generation.m_data->m_bricks[work.m_brickIndex];
		traceRequest.m_volumeMin = brick.m_min;
		traceRequest.m_volumeMax = brick.m_max;
		traceRequest.m_cancel = &generation.m_cancel;
		const uint32_t probeSeed = HashRuntimeProbeCell(work.m_key, generation.m_request.m_randomSeed);
		if (!work.m_bHasTransport)
		{
			const float visibilityMaxDistance = CalculateGIProbeVisibilityMaxDistance(
				*generation.m_data, generation.m_data->m_bricks[work.m_brickIndex]);
			if (!TraceGIProbeTransport(traceRequest,
					*generation.m_request.m_sampler,
					probeSeed,
					visibilityMaxDistance,
					work.m_probe,
					outDiagnostic))
			{
				return false;
			}
			work.m_bHasTransport = true;
			if (work.m_probe.m_validity <= 0.05f)
			{
				work.m_bReady = true;
				work.m_bRefined = true;
				return true;
			}
		}

		const uint32_t targetSamples = generation.m_request.m_qualitySettings.m_targetSamplesPerProbe;
		const uint32_t initialSamples = generation.m_request.m_qualitySettings.m_initialSamplesPerProbe;
		const uint32_t nextMilestone =
			work.m_accumulator.m_sampleCount < initialSamples ? initialSamples : targetSamples;
		const uint32_t remainingSamples = nextMilestone - work.m_accumulator.m_sampleCount;
		const uint32_t batchSize = (std::min)(IrradianceBatchSize, remainingSamples);
		if (batchSize == 0u)
		{
			work.m_bReady = true;
			work.m_bRefined = true;
			return true;
		}
		if (!AccumulateGIProbeIrradianceRange(traceRequest,
				*generation.m_request.m_sampler,
				work.m_probe.m_position,
				probeSeed,
				work.m_accumulator.m_sampleCount,
				batchSize,
				targetSamples,
				work.m_accumulator,
				outDiagnostic))
		{
			return false;
		}
		if (work.m_accumulator.m_sampleCount >= initialSamples)
		{
			if (!ResolveGIProbeIrradiance(work.m_accumulator, work.m_probe, outDiagnostic))
			{
				return false;
			}
			work.m_bReady = true;
		}
		work.m_bRefined = work.m_accumulator.m_sampleCount >= targetSamples;
		return true;
	}

}
