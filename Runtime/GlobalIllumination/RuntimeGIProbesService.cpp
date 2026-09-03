#include "GlobalIllumination/RuntimeGIProbesService.h"

#include "Containers/Hash.h"
#include "GlobalIllumination/GIProbesTracing.h"
#include "Math/Math.h"
#include "Sailor.h"
#include "Tasks/Tasks.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <deque>
#include <limits>
#include <mutex>
#include <thread>
#include <vector>

using namespace Sailor;

namespace
{
	constexpr uint32_t IrradianceBatchSize = 16u;

	struct ProbeGridBounds final
	{
		glm::vec3 m_min{};
		glm::vec3 m_max{};
	};

	struct ProbeGrid final
	{
		glm::vec3 m_min{};
		glm::vec3 m_max{};
		glm::uvec3 m_counts{ 2u };
		float m_spacing = 1.0f;
	};

	struct RuntimeProbeCellKey final
	{
		glm::ivec3 m_worldCell{};
		uint64_t m_geometryGeneration = 0u;

		bool operator==(const RuntimeProbeCellKey&) const noexcept = default;
	};

	uint32_t HashRuntimeProbeCell(
		const RuntimeProbeCellKey& key,
		uint32_t randomSeed) noexcept
	{
		uint64_t hash = Fnv1aOffsetBasis;
		HashValues(
			hash,
			randomSeed,
			key.m_worldCell.x,
			key.m_worldCell.y,
			key.m_worldCell.z,
			key.m_geometryGeneration);
		const uint32_t folded = static_cast<uint32_t>(hash ^ (hash >> 32u));
		return folded != 0u ? folded : 0x6d2b79f5u;
	}

	bool TryResolveProbeGrid(
		const ProbeGridBounds& bounds,
		float spacing,
		uint32_t capacity,
		ProbeGrid& outGrid) noexcept
	{
		if (!std::isfinite(spacing) || spacing <= 0.0f || capacity < 8u)
		{
			return false;
		}

		const glm::vec3 snappedMin =
			(glm::floor(bounds.m_min / spacing - 0.5f) + 0.5f) * spacing;
		if (!Math::AllFinite(snappedMin))
		{
			return false;
		}

		glm::uvec3 counts(2u);
		uint64_t probeCount = 1u;
		for (uint32_t axis = 0u; axis < 3u; ++axis)
		{
			const double extent = (std::max)(
				0.0,
				static_cast<double>(bounds.m_max[axis]) -
					static_cast<double>(snappedMin[axis]));
			const double count = std::ceil(
				extent / static_cast<double>(spacing)) + 1.0;
			if (!std::isfinite(count) || count > static_cast<double>(
				(std::numeric_limits<uint32_t>::max)()))
			{
				return false;
			}
			counts[axis] = (std::max)(2u, static_cast<uint32_t>(count));
			const float axisMax = snappedMin[axis] +
				static_cast<float>(counts[axis] - 1u) * spacing;
			if (axisMax < bounds.m_max[axis])
			{
				if (counts[axis] == (std::numeric_limits<uint32_t>::max)())
				{
					return false;
				}
				++counts[axis];
			}
			if (probeCount > capacity / counts[axis])
			{
				return false;
			}
			probeCount *= counts[axis];
		}

		const glm::vec3 gridMax = snappedMin +
			glm::vec3(counts - glm::uvec3(1u)) * spacing;
		if (!Math::AllFinite(gridMax))
		{
			return false;
		}
		outGrid.m_min = snappedMin;
		outGrid.m_max = gridMax;
		outGrid.m_counts = counts;
		outGrid.m_spacing = spacing;
		return true;
	}

	bool TryBuildSceneProbeGrid(
		const ProbeGridBounds& bounds,
		float minimumSpacing,
		uint32_t capacity,
		ProbeGrid& outGrid) noexcept
	{
		if (TryResolveProbeGrid(
				bounds,
				minimumSpacing,
				capacity,
				outGrid))
		{
			return true;
		}

		float lowerSpacing = minimumSpacing;
		float upperSpacing = minimumSpacing;
		ProbeGrid upperGrid;
		bool bFoundUpperBound = false;
		for (uint32_t iteration = 0u; iteration < 31u; ++iteration)
		{
			lowerSpacing = upperSpacing;
			upperSpacing *= 2.0f;
			if (!std::isfinite(upperSpacing))
			{
				return false;
			}
			if (TryResolveProbeGrid(
					bounds,
					upperSpacing,
					capacity,
					upperGrid))
			{
				bFoundUpperBound = true;
				break;
			}
		}
		if (!bFoundUpperBound)
		{
			return false;
		}

		for (uint32_t iteration = 0u; iteration < 24u; ++iteration)
		{
			const float candidateSpacing = static_cast<float>(std::sqrt(
				static_cast<double>(lowerSpacing) * upperSpacing));
			ProbeGrid candidateGrid;
			if (TryResolveProbeGrid(
					bounds,
					candidateSpacing,
					capacity,
					candidateGrid))
			{
				upperSpacing = candidateSpacing;
				upperGrid = candidateGrid;
			}
			else
			{
				lowerSpacing = candidateSpacing;
			}
		}
		outGrid = upperGrid;
		return true;
	}

	uint64_t EstimatePublishedBytes(const GIProbesData& data) noexcept
	{
		return sizeof(GIProbesData) +
			data.m_bricks.Num() * sizeof(GIProbeBrick) +
			data.m_probes.Num() * sizeof(GIProbe);
	}

	uint32_t ResolvePublicationLimitedCapacity(
		const RuntimeGIProbesQualitySettings& settings) noexcept
	{
		const uint64_t fixedBytes = sizeof(GIProbesData) +
			sizeof(GIProbeBrick);
		if (settings.m_maxDirtyUploadBytesPerFrame <= fixedBytes)
		{
			return 0u;
		}
		const uint64_t capacityFromPublication =
			(settings.m_maxDirtyUploadBytesPerFrame - fixedBytes) /
				sizeof(GIProbe);
		return static_cast<uint32_t>((std::min)(
			static_cast<uint64_t>(settings.m_maxActiveProbes),
			capacityFromPublication));
	}

}

struct RuntimeGIProbesService::Impl final
{
	struct ProbeWork final
	{
		RuntimeProbeCellKey m_key{};
		GIProbeIrradianceAccumulator m_accumulator{};
		GIProbe m_probe{};
		glm::vec3 m_layoutPosition{};
		uint32_t m_brickIndex = 0u;
		bool m_bHasTransport = false;
		bool m_bReady = false;
		bool m_bRefined = false;
		bool m_bDirty = false;
	};

	struct Generation final
	{
		RuntimeGIProbesStartRequest m_request{};
		GIProbesDataPtr m_data{};
		std::vector<ProbeWork> m_probes{};
		std::vector<uint32_t> m_initialQueue{};
		std::deque<uint32_t> m_warmingQueue{};
		std::deque<uint32_t> m_refinementQueue{};
		std::atomic<bool> m_cancel{ false };
		std::chrono::steady_clock::time_point m_started{};
		uint64_t m_id = 0u;
		uint64_t m_initialLayoutHash = 0u;
		uint32_t m_effectiveCapacity = 0u;
		size_t m_initialCursor = 0u;
		uint32_t m_readyCount = 0u;
		uint32_t m_refinedCount = 0u;
		uint32_t m_dirtyCount = 0u;
		uint64_t m_progressSampleCount = 0u;
		bool m_bFailed = false;
		bool m_bPublished = false;
	};

	struct Job final
	{
		std::shared_ptr<Generation> m_generation{};
		ProbeWork m_work{};
		uint32_t m_probeIndex = 0u;
	};

	struct DispatchBatch final
	{
		std::shared_ptr<Generation> m_generation{};
		uint32_t m_workerCount = 0u;
	};

	mutable std::mutex m_mutex;
	std::vector<Tasks::ITaskPtr> m_workerTasks{};
	std::shared_ptr<Generation> m_generation{};
	GIProbesDataPtr m_publishedData{};
	RuntimeGIProbesStartRequest m_lastRequest{};
	RuntimeGIProbesStatus m_status{};
	std::chrono::steady_clock::time_point m_lastPublication{};
	uint64_t m_nextGenerationId = 1u;
	uint64_t m_nextPublishedRevision = 1u;
	uint32_t m_workerCount = 0u;
	bool m_bHasLastRequest = false;
	bool m_bPaused = false;
	bool m_bWorkAllowed = true;
	double m_workTokensMilliseconds = 0.0;

	~Impl()
	{
		StopWorkerTasks();
	}

	void StopWorkerTasks() noexcept
	{
		std::vector<Tasks::ITaskPtr> workerTasks;
		{
			const std::lock_guard<std::mutex> lock(m_mutex);
			if (m_generation)
			{
				m_generation->m_cancel.store(
					true,
					std::memory_order_release);
			}
			workerTasks.swap(m_workerTasks);
			m_workerCount = 0u;
		}
		for (const Tasks::ITaskPtr& workerTask : workerTasks)
		{
			if (workerTask)
			{
				workerTask->Wait();
			}
		}
	}

	static uint32_t GetProgressSampleCount(
		const Generation& generation,
		const ProbeWork& probe) noexcept
	{
		return probe.m_bRefined ?
			generation.m_request.m_qualitySettings.m_targetSamplesPerProbe :
			probe.m_accumulator.m_sampleCount;
	}

	static bool AreTransportSettingsCompatible(
		const GIProbesBakeSettings& lhs,
		const GIProbesBakeSettings& rhs) noexcept
	{
		return lhs.m_randomSeed == rhs.m_randomSeed &&
			lhs.m_minProbeSpacing == rhs.m_minProbeSpacing &&
			lhs.m_normalBias == rhs.m_normalBias &&
			lhs.m_viewBias == rhs.m_viewBias &&
			lhs.m_maxRayDistance == rhs.m_maxRayDistance;
	}

	static bool AreIrradianceSettingsCompatible(
		const GIProbesBakeSettings& lhs,
		const GIProbesBakeSettings& rhs) noexcept
	{
		return AreTransportSettingsCompatible(lhs, rhs) &&
			lhs.m_raysPerProbe == rhs.m_raysPerProbe &&
			lhs.m_bounceCount == rhs.m_bounceCount &&
			lhs.m_skyIndirectIntensity == rhs.m_skyIndirectIntensity &&
			lhs.m_bIncludeSky == rhs.m_bIncludeSky &&
			lhs.m_bIncludeEmissive == rhs.m_bIncludeEmissive &&
			lhs.m_bIncludeDirectLighting == rhs.m_bIncludeDirectLighting;
	}

	static std::vector<uint32_t> BuildProgressiveProbeOrder(
		const Generation& generation)
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

		const glm::uvec3 cellCounts =
			brick.m_probeCounts - glm::uvec3(1u);
		const glm::vec3 cellSpacing =
			(brick.m_max - brick.m_min) / glm::vec3(cellCounts);
		std::vector<OrderedCell> cells;
		cells.reserve(static_cast<size_t>(cellCounts.x) *
			cellCounts.y * cellCounts.z);
		for (uint32_t z = 0u; z < cellCounts.z; ++z)
		{
			for (uint32_t y = 0u; y < cellCounts.y; ++y)
			{
				for (uint32_t x = 0u; x < cellCounts.x; ++x)
				{
					const glm::vec3 center = brick.m_min +
						(glm::vec3(x, y, z) + glm::vec3(0.5f)) *
							cellSpacing;
					const glm::vec3 delta = center -
						generation.m_request.m_priorityPosition;
					cells.push_back(OrderedCell{
						glm::uvec3(x, y, z),
						glm::dot(delta, delta) });
				}
			}
		}
		std::sort(
			cells.begin(),
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
		const auto appendProbe = [&result, &scheduled, &brick](
			uint32_t x,
			uint32_t y,
			uint32_t z)
		{
			const uint32_t probeIndex = brick.m_firstProbeIndex + x +
				brick.m_probeCounts.x *
					(y + brick.m_probeCounts.y * z);
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
						appendProbe(
							cell.m_cell.x + x,
							cell.m_cell.y + y,
							cell.m_cell.z + z);
					}
				}
			}
		}
		return result;
	}

	std::shared_ptr<Generation> BuildGeneration(
		const RuntimeGIProbesStartRequest& request,
		uint64_t generationId,
		std::string& outDiagnostic)
	{
		outDiagnostic.clear();
		if (!Math::AllFinite(request.m_priorityPosition))
		{
			outDiagnostic =
				"runtime GI probes require a finite priority position";
			return {};
		}
		const float spacing = request.m_worldSettings.m_minProbeSpacing *
			request.m_qualitySettings.m_spacingMultiplier;
		if (!std::isfinite(spacing) || spacing <= 0.0f)
		{
			outDiagnostic = "runtime GI probes resolved to invalid probe spacing";
			return {};
		}
		if (!request.m_geometryBounds.IsValid() ||
			glm::any(glm::lessThanEqual(
				request.m_geometryBounds.m_max,
				request.m_geometryBounds.m_min)))
		{
			outDiagnostic =
				"runtime GI probes require finite prepared-scene geometry bounds";
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
		data.m_representationHash = ComputeGIProbesRepresentationHash(
			data.m_formatVersion,
			data.m_shOrder,
			data.m_compression);
		data.m_bakeSettings = ResolveRuntimeGIProbesBakeSettings(
			request.m_worldSettings,
			request.m_qualitySettings,
			request.m_randomSeed);

		const uint32_t publicationLimitedCapacity =
			ResolvePublicationLimitedCapacity(request.m_qualitySettings);
		if (publicationLimitedCapacity < 8u)
		{
			outDiagnostic =
				"runtime GI probe upload budget cannot hold the minimum eight-probe grid";
			return {};
		}
		generation->m_effectiveCapacity = publicationLimitedCapacity;
		data.m_probes.Reserve(publicationLimitedCapacity);
		generation->m_probes.reserve(publicationLimitedCapacity);
		generation->m_initialQueue.reserve(publicationLimitedCapacity);
		const float geometryPadding = (std::max)(
			request.m_worldSettings.m_normalBias +
				request.m_worldSettings.m_viewBias,
			spacing * 0.01f);
		const ProbeGridBounds geometryBounds{
			request.m_geometryBounds.m_min - glm::vec3(geometryPadding),
			request.m_geometryBounds.m_max + glm::vec3(geometryPadding) };
		ProbeGrid grid;
		if (!TryBuildSceneProbeGrid(
				geometryBounds,
				spacing,
				publicationLimitedCapacity,
				grid))
		{
			outDiagnostic =
				"runtime GI probes could not fit the scene bounds into the active capacity";
			return {};
		}
		data.m_bakeSettings.m_maxSubdivisionLevel = 0u;
		data.m_bakeSettings.m_minProbeSpacing = grid.m_spacing;
		data.m_volumeMin = grid.m_min;
		data.m_volumeMax = grid.m_max;

		const uint32_t probeCount =
			grid.m_counts.x * grid.m_counts.y * grid.m_counts.z;
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
					probe.m_position = brick.m_min +
						glm::vec3(x, y, z) * grid.m_spacing;
					probe.m_validity = 0.0f;
					probe.m_flags = 0u;
					data.m_probes.Add(probe);

					ProbeWork work;
					work.m_probe = probe;
					work.m_layoutPosition = probe.m_position;
					work.m_key.m_worldCell = glm::ivec3(x, y, z);
					work.m_key.m_geometryGeneration =
						request.m_geometryGeneration;
					generation->m_probes.push_back(std::move(work));
				}
			}
		}

		generation->m_initialQueue = BuildProgressiveProbeOrder(*generation);

		data.m_layoutHash = ComputeGIProbesLayoutHash(data);
		generation->m_initialLayoutHash = data.m_layoutHash;
		data.m_diagnostics.m_invalidProbeCount =
			static_cast<uint32_t>(data.m_probes.Num());
		data.m_diagnostics.m_message =
			"runtime GI probes are warming; missing cells use environment fallback";
		return generation;
	}

	void ReuseGenerationState(
		Generation& next,
		const Generation& previous)
	{
		std::vector<uint32_t> progressiveOrder =
			std::move(next.m_initialQueue);
		next.m_initialQueue.reserve(progressiveOrder.size());
		next.m_warmingQueue.clear();
		next.m_refinementQueue.clear();
		next.m_initialCursor = 0u;
		next.m_readyCount = 0u;
		next.m_refinedCount = 0u;
		next.m_dirtyCount = 0u;
		next.m_progressSampleCount = 0u;
		const bool bLayoutCompatible =
			next.m_request.m_geometryGeneration ==
				previous.m_request.m_geometryGeneration &&
			next.m_initialLayoutHash == previous.m_initialLayoutHash &&
			next.m_probes.size() == previous.m_probes.size() &&
			AreTransportSettingsCompatible(
				previous.m_data->m_bakeSettings,
				next.m_data->m_bakeSettings);
		for (uint32_t probeIndex = 0u;
			probeIndex < next.m_probes.size();
			++probeIndex)
		{
			ProbeWork& probe = next.m_probes[probeIndex];
			const GIProbeBrick& brick =
				next.m_data->m_bricks[probe.m_brickIndex];
			const ProbeWork* previousProbe = bLayoutCompatible ?
				&previous.m_probes[probeIndex] : nullptr;
			const bool bSamePosition = previousProbe &&
				glm::all(glm::lessThanEqual(
					glm::abs(previousProbe->m_layoutPosition -
						probe.m_layoutPosition),
					glm::vec3(next.m_data->m_bakeSettings.m_minProbeSpacing *
						0.0001f)));
			const bool bPreviousProbeFitsTraceVolume = previousProbe &&
				Math::AllFinite(previousProbe->m_probe.m_position) &&
				glm::all(glm::greaterThanEqual(
					previousProbe->m_probe.m_position,
					brick.m_min)) &&
				glm::all(glm::lessThanEqual(
					previousProbe->m_probe.m_position,
					brick.m_max));
			const bool bReuseTransport = previousProbe &&
				previousProbe->m_bHasTransport &&
				bSamePosition &&
				bPreviousProbeFitsTraceVolume;
			if (bReuseTransport)
			{
				probe.m_probe = previousProbe->m_probe;
				probe.m_bHasTransport = true;
				const bool bReuseIrradiance =
					next.m_request.m_lightingGeneration ==
						previous.m_request.m_lightingGeneration &&
					AreIrradianceSettingsCompatible(
						previous.m_data->m_bakeSettings,
						next.m_data->m_bakeSettings);
				if (bReuseIrradiance)
				{
					probe.m_accumulator = previousProbe->m_accumulator;
					probe.m_bReady = probe.m_probe.m_validity <= 0.05f ||
						probe.m_accumulator.m_sampleCount >= next.m_request
							.m_qualitySettings.m_initialSamplesPerProbe;
					probe.m_bRefined = probe.m_probe.m_validity <= 0.05f ||
						probe.m_accumulator.m_sampleCount >= next.m_request
							.m_qualitySettings.m_targetSamplesPerProbe;
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
			next.m_progressSampleCount +=
				GetProgressSampleCount(next, probe);
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

	bool TryTakeJob(
		const std::shared_ptr<Generation>& generation,
		Job& outJob)
	{
		const std::lock_guard<std::mutex> lock(m_mutex);
		if (!CanDispatchWorkLocked() || m_generation != generation)
		{
			return false;
		}

		uint32_t probeIndex = 0u;
		if (!generation->m_warmingQueue.empty())
		{
			probeIndex = generation->m_warmingQueue.front();
			generation->m_warmingQueue.pop_front();
		}
		else if (generation->m_initialCursor <
			generation->m_initialQueue.size())
		{
			probeIndex = generation->m_initialQueue[
				generation->m_initialCursor++];
		}
		else
		{
			probeIndex = generation->m_refinementQueue.front();
			generation->m_refinementQueue.pop_front();
		}

		outJob.m_work = generation->m_probes[probeIndex];
		outJob.m_generation = generation;
		outJob.m_probeIndex = probeIndex;
		return true;
	}

	static bool HasQueuedWork(const Generation& generation) noexcept
	{
		return !generation.m_warmingQueue.empty() ||
			generation.m_initialCursor < generation.m_initialQueue.size() ||
			!generation.m_refinementQueue.empty();
	}

	bool CanDispatchWorkLocked() const noexcept
	{
		return m_generation &&
			!m_bPaused &&
			m_bWorkAllowed &&
			m_workTokensMilliseconds > 0.0 &&
			!m_generation->m_bFailed &&
			!m_generation->m_cancel.load(std::memory_order_acquire) &&
			HasQueuedWork(*m_generation);
	}

	DispatchBatch GetDispatchBatch() const noexcept
	{
		const std::lock_guard<std::mutex> lock(m_mutex);
		return CanDispatchWorkLocked() ?
			DispatchBatch{ m_generation, m_workerCount } : DispatchBatch{};
	}

	void WorkerBatch(
		std::shared_ptr<Generation> generation,
		uint32_t maximumJobCount =
			(std::numeric_limits<uint32_t>::max)())
	{
		for (uint32_t completedJobCount = 0u;
			completedJobCount < maximumJobCount;
			++completedJobCount)
		{
			Job job;
			if (!TryTakeJob(generation, job))
			{
				return;
			}
			const auto started = std::chrono::steady_clock::now();
			std::string diagnostic;
			const bool bSuccess = ExecuteJob(job, diagnostic);
			const auto finished = std::chrono::steady_clock::now();
			const double elapsedMilliseconds =
				std::chrono::duration<double, std::milli>(finished - started)
					.count();
			CommitJob(
				job,
				bSuccess,
				std::move(diagnostic),
				elapsedMilliseconds);

			const float duty = job.m_generation->m_request.m_qualitySettings
				.m_cpuDutyFraction;
			if (bSuccess && duty < 0.999f)
			{
				const double sleepMilliseconds = (std::min)(
					100.0,
					elapsedMilliseconds *
						(1.0 / static_cast<double>(duty) - 1.0));
				if (sleepMilliseconds > 0.05)
				{
					std::this_thread::sleep_for(
						std::chrono::duration<double, std::milli>(
							sleepMilliseconds));
				}
			}
		}
	}

	void PumpWorkerTasks()
	{
		m_workerTasks.erase(
			std::remove_if(
				m_workerTasks.begin(),
				m_workerTasks.end(),
				[](const Tasks::ITaskPtr& task)
				{
					return !task || task->IsFinished();
				}),
			m_workerTasks.end());
		const DispatchBatch dispatch = GetDispatchBatch();
		if (!dispatch.m_generation || dispatch.m_workerCount == 0u)
		{
			return;
		}

		auto* scheduler = App::GetSubmodule<Tasks::Scheduler>();
		if (!scheduler)
		{
			WorkerBatch(dispatch.m_generation, 1u);
			return;
		}

		while (m_workerTasks.size() < dispatch.m_workerCount)
		{
			const std::shared_ptr<Generation> generation =
				dispatch.m_generation;
			Tasks::ITaskPtr workerTask = Tasks::CreateTask(
				"Runtime GI Probe Trace",
				[this, generation]() { WorkerBatch(generation); },
				EThreadType::GI);
			m_workerTasks.push_back(workerTask);
			scheduler->Run(workerTask);
		}
	}

	bool ExecuteJob(Job& job, std::string& outDiagnostic)
	{
		Generation& generation = *job.m_generation;
		ProbeWork& work = job.m_work;
		GIProbeTraceRequest traceRequest;
		traceRequest.m_settings = generation.m_data->m_bakeSettings;
		const GIProbeBrick& brick =
			generation.m_data->m_bricks[work.m_brickIndex];
		traceRequest.m_volumeMin = brick.m_min;
		traceRequest.m_volumeMax = brick.m_max;
		traceRequest.m_cancel = &generation.m_cancel;
		const uint32_t probeSeed = HashRuntimeProbeCell(
			work.m_key,
			generation.m_request.m_randomSeed);
		if (!work.m_bHasTransport)
		{
			const float visibilityMaxDistance =
				CalculateGIProbeVisibilityMaxDistance(
					*generation.m_data,
					generation.m_data->m_bricks[work.m_brickIndex]);
			if (!TraceGIProbeTransport(
					traceRequest,
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

		const uint32_t targetSamples = generation.m_request.m_qualitySettings
			.m_targetSamplesPerProbe;
		const uint32_t initialSamples = generation.m_request.m_qualitySettings
			.m_initialSamplesPerProbe;
		const uint32_t nextMilestone =
			work.m_accumulator.m_sampleCount < initialSamples ?
				initialSamples : targetSamples;
		const uint32_t remainingSamples = nextMilestone -
			work.m_accumulator.m_sampleCount;
		const uint32_t batchSize = (std::min)(
			IrradianceBatchSize,
			remainingSamples);
		if (batchSize == 0u)
		{
			work.m_bReady = true;
			work.m_bRefined = true;
			return true;
		}
		if (!AccumulateGIProbeIrradianceRange(
				traceRequest,
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
			if (!ResolveGIProbeIrradiance(
					work.m_accumulator,
					work.m_probe,
					outDiagnostic))
			{
				return false;
			}
			work.m_bReady = true;
		}
		work.m_bRefined =
			work.m_accumulator.m_sampleCount >= targetSamples;
		return true;
	}

	void FailGenerationLocked(
		Generation& generation,
		std::string diagnostic)
	{
		generation.m_bFailed = true;
		generation.m_cancel.store(true, std::memory_order_release);
		m_status.m_lifecycle = ERuntimeGIProbesLifecycle::Failed;
		m_status.m_diagnostic = std::move(diagnostic);
	}

	void CommitJob(
		const Job& job,
		bool bSuccess,
		std::string diagnostic,
		double elapsedMilliseconds)
	{
		const std::lock_guard<std::mutex> lock(m_mutex);
		const std::shared_ptr<Generation>& generation = job.m_generation;
		if (!m_generation ||
			m_generation != generation ||
			generation->m_cancel.load(std::memory_order_acquire))
		{
			return;
		}
		ProbeWork& current = generation->m_probes[job.m_probeIndex];
		m_workTokensMilliseconds -= elapsedMilliseconds;
		if (!bSuccess)
		{
			FailGenerationLocked(
				*generation,
				diagnostic.empty() ?
					"runtime GI probe tracing failed" :
					std::move(diagnostic));
			return;
		}

		const bool bWasReady = current.m_bReady;
		const bool bWasRefined = current.m_bRefined;
		const uint32_t previousProgress =
			GetProgressSampleCount(*generation, current);
		current = job.m_work;
		generation->m_progressSampleCount +=
			GetProgressSampleCount(*generation, current) - previousProgress;
		if (!bWasReady && current.m_bReady)
		{
			++generation->m_readyCount;
		}
		if (!bWasRefined && current.m_bRefined)
		{
			++generation->m_refinedCount;
		}
		if (current.m_bReady)
		{
			generation->m_data->m_probes[job.m_probeIndex] = current.m_probe;
			if (!current.m_bDirty)
			{
				current.m_bDirty = true;
				++generation->m_dirtyCount;
			}
		}
		if (!current.m_bReady)
		{
			generation->m_warmingQueue.push_back(job.m_probeIndex);
		}
		else if (!current.m_bRefined)
		{
			generation->m_refinementQueue.push_back(job.m_probeIndex);
		}
		UpdateStatusLocked();
	}

	void UpdateStatusLocked()
	{
		m_status.m_bEnabled = m_generation != nullptr;
		m_status.m_bPaused = m_bPaused;
		m_status.m_workerCount = m_workerCount;
		if (!m_generation)
		{
			return;
		}
		const Generation& generation = *m_generation;
		const uint32_t probeCount = static_cast<uint32_t>(
			generation.m_probes.size());
		m_status.m_sceneGeneration =
			generation.m_request.m_geometryGeneration;
		m_status.m_lightingGeneration =
			generation.m_request.m_lightingGeneration;
		m_status.m_capacity = generation.m_effectiveCapacity;
		m_status.m_activeProbeCount = probeCount;
		m_status.m_readyProbeCount = generation.m_readyCount;
		m_status.m_coverage = probeCount > 0u ?
			static_cast<float>(generation.m_readyCount) /
				static_cast<float>(probeCount) : 0.0f;

		const uint32_t targetSamples = generation.m_request.m_qualitySettings
			.m_targetSamplesPerProbe;
		m_status.m_refinement = probeCount > 0u && targetSamples > 0u ?
			static_cast<float>(generation.m_progressSampleCount) /
				static_cast<float>(
					static_cast<uint64_t>(probeCount) * targetSamples) :
			0.0f;
		if (generation.m_bFailed)
		{
			m_status.m_lifecycle = ERuntimeGIProbesLifecycle::Failed;
		}
		else if (m_bPaused)
		{
			m_status.m_lifecycle = ERuntimeGIProbesLifecycle::Paused;
		}
		else if (!m_bWorkAllowed ||
			(HasQueuedWork(generation) &&
				m_workTokensMilliseconds <= 0.0))
		{
			m_status.m_lifecycle = ERuntimeGIProbesLifecycle::Throttled;
		}
		else
		{
			m_status.m_lifecycle =
				generation.m_refinedCount == generation.m_probes.size() ?
					ERuntimeGIProbesLifecycle::Ready :
					ERuntimeGIProbesLifecycle::Tracing;
		}
	}

	void PublishIfNeeded()
	{
		const std::lock_guard<std::mutex> lock(m_mutex);
		if (!m_generation ||
			m_generation->m_bFailed ||
			m_generation->m_dirtyCount == 0u)
		{
			UpdateStatusLocked();
			return;
		}
		const uint32_t probeCount = static_cast<uint32_t>(
			m_generation->m_probes.size());
		const uint32_t configuredReadyCount = static_cast<uint32_t>(
			std::ceil(static_cast<float>(probeCount) *
				m_generation->m_request.m_qualitySettings
					.m_initialPublicationCoverage));
		const uint32_t minimumCellReadyCount = (std::min)(probeCount, 8u);
		const uint32_t requiredReadyCount = (std::max)(
			configuredReadyCount,
			minimumCellReadyCount);
		if (!m_generation->m_bPublished &&
			m_generation->m_readyCount < requiredReadyCount)
		{
			if (m_publishedData)
			{
				m_status.m_diagnostic =
					"retaining the last runtime GI snapshot while the replacement grid warms";
			}
			UpdateStatusLocked();
			return;
		}
		const auto now = std::chrono::steady_clock::now();
		const double minimumInterval = 1.0 /
			m_generation->m_request.m_qualitySettings
				.m_maxPublicationsPerSecond;
		if (m_lastPublication.time_since_epoch().count() != 0 &&
			std::chrono::duration<double>(now - m_lastPublication).count() <
				minimumInterval)
		{
			UpdateStatusLocked();
			return;
		}

		GIProbesData& working = *m_generation->m_data;
		uint32_t invalidCount = 0u;
		uint32_t relocatedCount = 0u;
		float validity = 0.0f;
		for (const GIProbe& probe : working.m_probes)
		{
			invalidCount += probe.m_validity <= 0.05f ? 1u : 0u;
			relocatedCount +=
				(probe.m_flags & static_cast<uint32_t>(
					EGIProbeFlag::Relocated)) != 0u ? 1u : 0u;
			validity += probe.m_validity;
		}
		working.m_diagnostics.m_invalidProbeCount = invalidCount;
		working.m_diagnostics.m_relocatedProbeCount = relocatedCount;
		working.m_diagnostics.m_averageValidity = probeCount > 0u ?
			validity / static_cast<float>(probeCount) : 0.0f;
		working.m_diagnostics.m_bakeDurationSeconds = static_cast<float>(
			std::chrono::duration<double>(
				now - m_generation->m_started).count());
		working.m_diagnostics.m_message =
			m_generation->m_refinedCount == probeCount ?
				"runtime GI probes reached the target sample count" :
				"runtime GI probes are refining with environment fallback for missing cells";
		working.m_layoutHash = ComputeGIProbesLayoutHash(working);
		working.m_transportHash =
			m_generation->m_request.m_geometryGeneration;
		HashCombine(
			working.m_transportHash,
			m_generation->m_readyCount);
		working.m_lightingHash =
			(m_generation->m_request.m_lightingGeneration << 32u) ^
			m_nextPublishedRevision;

		GIProbesDataPtr published = GIProbesDataPtr::Make(working);
		const uint64_t publishedBytes = EstimatePublishedBytes(*published);
		if (publishedBytes > m_generation->m_request.m_qualitySettings
			.m_maxDirtyUploadBytesPerFrame)
		{
			FailGenerationLocked(
				*m_generation,
				"runtime GI publication exceeded its configured upload budget");
			return;
		}
		std::string validationDiagnostic;
		if (!published->Validate(validationDiagnostic))
		{
			FailGenerationLocked(
				*m_generation,
				"runtime GI probes rejected a publication: " +
					validationDiagnostic);
			return;
		}

		m_publishedData = std::move(published);
		m_generation->m_bPublished = true;
		m_status.m_publishedRevision = m_nextPublishedRevision++;
		m_status.m_publishedBytes = publishedBytes;
		m_lastPublication = now;
		for (ProbeWork& probe : m_generation->m_probes)
		{
			probe.m_bDirty = false;
		}
		m_generation->m_dirtyCount = 0u;
		m_status.m_diagnostic = working.m_diagnostics.m_message;
		UpdateStatusLocked();
	}
};

RuntimeGIProbesService::RuntimeGIProbesService() :
	m_impl(std::make_unique<Impl>())
{}

RuntimeGIProbesService::~RuntimeGIProbesService() = default;

bool RuntimeGIProbesService::Start(
	const RuntimeGIProbesStartRequest& request,
	std::string& outDiagnostic)
{
	if (!request.m_worldSettings.Validate(outDiagnostic) ||
		!request.m_qualitySettings.Validate(outDiagnostic))
	{
		return false;
	}
	if (!request.m_qualitySettings.m_bEnabled)
	{
		Disable();
		outDiagnostic =
			"experimental runtime GI probes are disabled by the active quality profile";
		return true;
	}
	if (!request.m_sampler)
	{
		outDiagnostic =
			"runtime GI probes require an immutable prepared ray sampler";
		return false;
	}

	uint64_t generationId = 0u;
	{
		const std::lock_guard<std::mutex> lock(m_impl->m_mutex);
		generationId = m_impl->m_nextGenerationId++;
	}
	std::shared_ptr<Impl::Generation> generation = m_impl->BuildGeneration(
		request,
		generationId,
		outDiagnostic);
	if (!generation)
	{
		return false;
	}
	{
		const std::lock_guard<std::mutex> lock(m_impl->m_mutex);
		m_impl->m_workerCount = request.m_qualitySettings.m_workerCount;
		const uint64_t retainedRevision =
			m_impl->m_status.m_publishedRevision;
		const uint64_t retainedBytes =
			m_impl->m_status.m_publishedBytes;
		if (request.m_bReuseExistingProbes && m_impl->m_generation)
		{
			m_impl->ReuseGenerationState(
				*generation,
				*m_impl->m_generation);
		}
		if (m_impl->m_generation)
		{
			m_impl->m_generation->m_cancel.store(
				true,
				std::memory_order_release);
		}
		m_impl->m_generation = std::move(generation);
		m_impl->m_lastRequest = request;
		m_impl->m_bHasLastRequest = true;
		m_impl->m_status = {};
		m_impl->m_status.m_publishedRevision = retainedRevision;
		m_impl->m_status.m_publishedBytes = retainedBytes;
		m_impl->m_workTokensMilliseconds =
			request.m_qualitySettings.m_cpuBudgetMilliseconds;
		m_impl->m_status.m_diagnostic =
			m_impl->m_generation->m_effectiveCapacity <
				request.m_qualitySettings.m_maxActiveProbes ?
				"runtime GI capacity was limited by the publication upload budget" :
				"runtime GI probes started with an immutable prepared scene";
		m_impl->UpdateStatusLocked();
	}
	m_impl->PumpWorkerTasks();
	outDiagnostic = "started experimental runtime GI probes";
	return true;
}

bool RuntimeGIProbesService::Restart(std::string& outDiagnostic)
{
	RuntimeGIProbesStartRequest request;
	{
		const std::lock_guard<std::mutex> lock(m_impl->m_mutex);
		if (!m_impl->m_bHasLastRequest)
		{
			outDiagnostic = "runtime GI probes have no previous start request";
			return false;
		}
		request = m_impl->m_lastRequest;
	}
	request.m_bReuseExistingProbes = false;
	return Start(request, outDiagnostic);
}

void RuntimeGIProbesService::Disable() noexcept
{
	{
		const std::lock_guard<std::mutex> lock(m_impl->m_mutex);
		if (m_impl->m_generation)
		{
			m_impl->m_generation->m_cancel.store(
				true,
				std::memory_order_release);
		}
		m_impl->m_generation.reset();
		m_impl->m_workerCount = 0u;
		m_impl->m_publishedData.Clear();
		m_impl->m_status = {};
		m_impl->m_bPaused = false;
		m_impl->m_workTokensMilliseconds = 0.0;
		m_impl->m_status.m_lifecycle = ERuntimeGIProbesLifecycle::Disabled;
		m_impl->m_status.m_diagnostic =
			"experimental runtime GI probes are disabled";
	}
}

void RuntimeGIProbesService::SetPaused(bool bPaused) noexcept
{
	const std::lock_guard<std::mutex> lock(m_impl->m_mutex);
	m_impl->m_bPaused = bPaused;
	m_impl->UpdateStatusLocked();
}

void RuntimeGIProbesService::SetWorkAllowed(bool bAllowed) noexcept
{
	const std::lock_guard<std::mutex> lock(m_impl->m_mutex);
	if (m_impl->m_bWorkAllowed == bAllowed)
	{
		return;
	}
	m_impl->m_bWorkAllowed = bAllowed;
	m_impl->UpdateStatusLocked();
}

void RuntimeGIProbesService::Tick(float deltaTimeSeconds)
{
	{
		const std::lock_guard<std::mutex> lock(m_impl->m_mutex);
		if (m_impl->m_generation &&
			std::isfinite(deltaTimeSeconds) &&
			deltaTimeSeconds > 0.0f)
		{
			const double budget = m_impl->m_generation->m_request
				.m_qualitySettings.m_cpuBudgetMilliseconds;
			constexpr double ReferenceFrameSeconds = 1.0 / 60.0;
			const double replenished = budget *
				(static_cast<double>(deltaTimeSeconds) /
					ReferenceFrameSeconds);
			m_impl->m_workTokensMilliseconds = (std::min)(
				budget * 2.0,
				m_impl->m_workTokensMilliseconds + replenished);
			m_impl->UpdateStatusLocked();
		}
	}
	m_impl->PumpWorkerTasks();
	m_impl->PublishIfNeeded();
}

GIProbesDataPtr RuntimeGIProbesService::GetPublishedData() const
{
	const std::lock_guard<std::mutex> lock(m_impl->m_mutex);
	return m_impl->m_publishedData;
}

RuntimeGIProbesStatus RuntimeGIProbesService::GetStatus() const
{
	const std::lock_guard<std::mutex> lock(m_impl->m_mutex);
	m_impl->UpdateStatusLocked();
	return m_impl->m_status;
}
