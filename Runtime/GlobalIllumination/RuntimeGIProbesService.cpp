#include "GlobalIllumination/RuntimeGIProbesService.h"

#include "GlobalIllumination/GIProbesTracing.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <limits>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#if defined(_WIN32)
#include <Windows.h>
#elif defined(__APPLE__)
#include <pthread/qos.h>
#endif

using namespace Sailor;

namespace
{
	constexpr uint32_t IrradianceBatchSize = 16u;

	bool IsFinite(const glm::vec3& value) noexcept
	{
		return std::isfinite(value.x) &&
			std::isfinite(value.y) &&
			std::isfinite(value.z);
	}

	void ConfigureBackgroundWorker() noexcept
	{
#if defined(_WIN32)
		SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#elif defined(__APPLE__)
		pthread_set_qos_class_self_np(QOS_CLASS_UTILITY, 0);
#endif
	}

	uint32_t HashRuntimeProbeCell(
		const RuntimeGIProbeCellKey& key,
		uint32_t randomSeed) noexcept
	{
		uint32_t hash = randomSeed ^ 0x9e3779b9u;
		auto mix = [&hash](uint32_t value)
		{
			hash ^= value + 0x9e3779b9u + (hash << 6u) + (hash >> 2u);
		};
		mix(static_cast<uint32_t>(key.m_worldCell.x));
		mix(static_cast<uint32_t>(key.m_worldCell.y));
		mix(static_cast<uint32_t>(key.m_worldCell.z));
		mix(key.m_cascade);
		mix(static_cast<uint32_t>(key.m_geometryGeneration));
		mix(static_cast<uint32_t>(key.m_geometryGeneration >> 32u));
		return hash != 0u ? hash : 0x6d2b79f5u;
	}

	glm::uvec3 SelectGridDimensions(uint32_t capacity) noexcept
	{
		glm::uvec3 best(2u);
		uint32_t bestCount = 8u;
		for (uint32_t y = 2u; y <= 64u; ++y)
		{
			uint32_t horizontal = y * 2u;
			while (horizontal > 2u &&
				static_cast<uint64_t>(horizontal) * horizontal * y > capacity)
			{
				--horizontal;
			}
			const uint64_t count =
				static_cast<uint64_t>(horizontal) * horizontal * y;
			if (count <= capacity && count > bestCount)
			{
				best = glm::uvec3(horizontal, y, horizontal);
				bestCount = static_cast<uint32_t>(count);
			}
		}
		return best;
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
			static_cast<uint64_t>(settings.m_clipmapCascadeCount) *
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
		RuntimeGIProbeCellKey m_key{};
		GIProbeIrradianceAccumulator m_accumulator{};
		GIProbe m_probe{};
		ERuntimeGIProbeState m_state =
			ERuntimeGIProbeState::Uninitialized;
		uint32_t m_seed = 0u;
		uint32_t m_brickIndex = 0u;
		float m_spacing = 1.0f;
		glm::vec3 m_traceVolumeMin{};
		glm::vec3 m_traceVolumeMax{};
		bool m_bInFlight = false;
		bool m_bReady = false;
		bool m_bRefined = false;
		bool m_bDirty = false;
	};

	struct Generation final
	{
		RuntimeGIProbesStartRequest m_request{};
		GIProbeTraceRequest m_traceRequest{};
		GIProbesDataPtr m_data{};
		std::vector<ProbeWork> m_probes{};
		std::vector<uint32_t> m_initialQueue{};
		std::deque<uint32_t> m_warmingQueue{};
		std::deque<uint32_t> m_refinementQueue{};
		std::atomic<bool> m_cancel{ false };
		std::chrono::steady_clock::time_point m_started{};
		uint64_t m_id = 0u;
		uint32_t m_effectiveCapacity = 0u;
		size_t m_initialCursor = 0u;
		uint32_t m_readyCount = 0u;
		uint32_t m_refinedCount = 0u;
		uint32_t m_dirtyCount = 0u;
		uint64_t m_tracedRays = 0u;
		double m_workerCpuMilliseconds = 0.0;
		bool m_bFailed = false;
		bool m_bPublished = false;
		std::string m_diagnostic{};
	};

	struct ReusableCellKey final
	{
		glm::ivec3 m_worldCell{};
		uint64_t m_geometryGeneration = 0u;
		uint32_t m_cascade = 0u;

		bool operator==(const ReusableCellKey&) const noexcept = default;
	};

	struct ReusableCellKeyHash final
	{
		size_t operator()(const ReusableCellKey& key) const noexcept
		{
			size_t hash = static_cast<size_t>(key.m_geometryGeneration);
			auto mix = [&hash](uint64_t value)
			{
				hash ^= static_cast<size_t>(value) +
					0x9e3779b97f4a7c15ull + (hash << 6u) + (hash >> 2u);
			};
			mix(static_cast<uint32_t>(key.m_worldCell.x));
			mix(static_cast<uint32_t>(key.m_worldCell.y));
			mix(static_cast<uint32_t>(key.m_worldCell.z));
			mix(key.m_cascade);
			return hash;
		}
	};

	struct Job final
	{
		std::shared_ptr<Generation> m_generation{};
		ProbeWork m_work{};
		uint32_t m_probeIndex = 0u;
	};

	mutable std::mutex m_mutex;
	std::condition_variable m_condition;
	std::vector<std::thread> m_workers{};
	std::shared_ptr<Generation> m_generation{};
	GIProbesDataPtr m_publishedData{};
	RuntimeGIProbesStartRequest m_lastRequest{};
	RuntimeGIProbesStatus m_status{};
	std::chrono::steady_clock::time_point m_lastPublication{};
	uint64_t m_nextGenerationId = 1u;
	uint64_t m_nextPublishedRevision = 1u;
	uint32_t m_workerCount = 0u;
	bool m_bStopWorkers = false;
	bool m_bHasLastRequest = false;
	bool m_bPaused = false;
	bool m_bWorkAllowed = true;
	double m_workTokensMilliseconds = 0.0;

	~Impl()
	{
		StopWorkers();
	}

	void StopWorkers() noexcept
	{
		{
			const std::lock_guard<std::mutex> lock(m_mutex);
			m_bStopWorkers = true;
			if (m_generation)
			{
				m_generation->m_cancel.store(
					true,
					std::memory_order_release);
			}
		}
		m_condition.notify_all();
		for (std::thread& worker : m_workers)
		{
			if (worker.joinable())
			{
				worker.join();
			}
		}
		m_workers.clear();
		m_workerCount = 0u;
		m_bStopWorkers = false;
	}

	void EnsureWorkers(uint32_t workerCount)
	{
		if (m_workerCount == workerCount && !m_workers.empty())
		{
			return;
		}
		StopWorkers();
		m_workerCount = workerCount;
		m_workers.reserve(workerCount);
		for (uint32_t workerIndex = 0u;
			workerIndex < workerCount;
			++workerIndex)
		{
			m_workers.emplace_back([this]() { WorkerMain(); });
		}
	}

	std::shared_ptr<Generation> BuildGeneration(
		const RuntimeGIProbesStartRequest& request,
		uint64_t generationId,
		std::string& outDiagnostic)
	{
		outDiagnostic.clear();
		if (!IsFinite(request.m_cameraPosition))
		{
			outDiagnostic = "runtime GI probes require a finite camera position";
			return {};
		}
		const float spacing = request.m_worldSettings.m_minProbeSpacing *
			request.m_qualitySettings.m_spacingMultiplier;
		if (!std::isfinite(spacing) || spacing <= 0.0f)
		{
			outDiagnostic = "runtime GI probes resolved to invalid probe spacing";
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
		data.m_bakeSettings.m_raysPerProbe =
			request.m_qualitySettings.m_targetSamplesPerProbe;
		data.m_bakeSettings.m_bounceCount =
			request.m_worldSettings.m_bounceCount;
		data.m_bakeSettings.m_randomSeed = request.m_randomSeed;
		data.m_bakeSettings.m_maxSubdivisionLevel =
			request.m_qualitySettings.m_clipmapCascadeCount - 1u;
		data.m_bakeSettings.m_minProbeSpacing = spacing;
		data.m_bakeSettings.m_normalBias =
			request.m_worldSettings.m_normalBias;
		data.m_bakeSettings.m_viewBias = request.m_worldSettings.m_viewBias;
		data.m_bakeSettings.m_maxRayDistance =
			request.m_worldSettings.m_maxRayDistance;
		data.m_bakeSettings.m_bIncludeSky =
			request.m_worldSettings.m_bIncludeSky;
		data.m_bakeSettings.m_bIncludeEmissive =
			request.m_worldSettings.m_bIncludeEmissive;
		data.m_bakeSettings.m_bIncludeDirectLighting =
			request.m_worldSettings.m_bIncludeDirectLighting;

		const uint32_t cascadeCount =
			request.m_qualitySettings.m_clipmapCascadeCount;
		const uint32_t publicationLimitedCapacity =
			ResolvePublicationLimitedCapacity(request.m_qualitySettings);
		if (publicationLimitedCapacity < cascadeCount * 8u)
		{
			outDiagnostic =
				"runtime GI probe upload budget cannot hold the minimum eight probes per clipmap cascade";
			return {};
		}
		generation->m_effectiveCapacity = publicationLimitedCapacity;
		data.m_probes.Reserve(publicationLimitedCapacity);
		generation->m_probes.reserve(publicationLimitedCapacity);
		generation->m_initialQueue.reserve(publicationLimitedCapacity);
		uint32_t remainingCapacity = publicationLimitedCapacity;
		for (uint32_t cascadeIndex = 0u;
			cascadeIndex < cascadeCount;
			++cascadeIndex)
		{
			const uint32_t remainingCascades = cascadeCount - cascadeIndex;
			const uint32_t cascadeCapacity =
				remainingCapacity / remainingCascades;
			const glm::uvec3 grid = SelectGridDimensions(cascadeCapacity);
			const uint32_t probeCount = grid.x * grid.y * grid.z;
			const float cascadeSpacing = spacing *
				static_cast<float>(1u << cascadeIndex);
			const glm::vec3 snappedCenter = glm::round(
				request.m_cameraPosition / cascadeSpacing) * cascadeSpacing;
			const glm::vec3 halfExtent =
				glm::vec3(grid - glm::uvec3(1u)) * cascadeSpacing * 0.5f;

			GIProbeBrick brick;
			brick.m_min = snappedCenter - halfExtent;
			brick.m_max = snappedCenter + halfExtent;
			brick.m_subdivisionLevel = cascadeCount - cascadeIndex - 1u;
			brick.m_firstProbeIndex =
				static_cast<uint32_t>(data.m_probes.Num());
			brick.m_probeCounts = grid;
			brick.m_probeCount = probeCount;
			const uint32_t brickIndex =
				static_cast<uint32_t>(data.m_bricks.Num());
			data.m_bricks.Add(brick);
			if (cascadeIndex == 0u)
			{
				data.m_volumeMin = brick.m_min;
				data.m_volumeMax = brick.m_max;
			}
			else
			{
				data.m_volumeMin = glm::min(data.m_volumeMin, brick.m_min);
				data.m_volumeMax = glm::max(data.m_volumeMax, brick.m_max);
			}

			for (uint32_t z = 0u; z < grid.z; ++z)
			{
				for (uint32_t y = 0u; y < grid.y; ++y)
				{
					for (uint32_t x = 0u; x < grid.x; ++x)
					{
						GIProbe probe;
						probe.m_position = brick.m_min +
							glm::vec3(x, y, z) * cascadeSpacing;
						probe.m_validity = 0.0f;
						probe.m_flags = 0u;
						data.m_probes.Add(probe);

						ProbeWork work;
						work.m_probe = probe;
						work.m_key.m_worldCell = glm::ivec3(glm::round(
							probe.m_position / cascadeSpacing));
						work.m_key.m_geometryGeneration =
							request.m_geometryGeneration;
						work.m_key.m_cascade = cascadeIndex;
						work.m_key.m_slotGeneration =
							static_cast<uint32_t>(generationId);
						work.m_seed = HashRuntimeProbeCell(
							work.m_key,
							request.m_randomSeed);
						work.m_brickIndex = brickIndex;
						work.m_spacing = cascadeSpacing;
						work.m_traceVolumeMin = brick.m_min;
						work.m_traceVolumeMax = brick.m_max;
						generation->m_probes.push_back(std::move(work));
						generation->m_initialQueue.push_back(
							static_cast<uint32_t>(
								generation->m_probes.size() - 1u));
					}
				}
			}
			remainingCapacity -= probeCount;
		}
		std::sort(
			generation->m_initialQueue.begin(),
			generation->m_initialQueue.end(),
			[&data, &request, &generation](uint32_t lhs, uint32_t rhs)
			{
				const uint32_t lhsCascade =
					generation->m_probes[lhs].m_key.m_cascade;
				const uint32_t rhsCascade =
					generation->m_probes[rhs].m_key.m_cascade;
				if (lhsCascade != rhsCascade)
				{
					return lhsCascade < rhsCascade;
				}
				const glm::vec3 lhsDelta =
					data.m_probes[lhs].m_position - request.m_cameraPosition;
				const glm::vec3 rhsDelta =
					data.m_probes[rhs].m_position - request.m_cameraPosition;
				const float lhsDistance = glm::dot(lhsDelta, lhsDelta);
				const float rhsDistance = glm::dot(rhsDelta, rhsDelta);
				return lhsDistance != rhsDistance ?
					lhsDistance < rhsDistance : lhs < rhs;
			});

		data.m_layoutHash = ComputeGIProbesLayoutHash(data);
		data.m_diagnostics.m_invalidProbeCount =
			static_cast<uint32_t>(data.m_probes.Num());
		data.m_diagnostics.m_message =
			"runtime GI probes are warming; missing cells use environment fallback";
		generation->m_traceRequest.m_settings = data.m_bakeSettings;
		generation->m_traceRequest.m_volumeMin = data.m_volumeMin;
		generation->m_traceRequest.m_volumeMax = data.m_volumeMax;
		generation->m_traceRequest.m_cancel = &generation->m_cancel;
		return generation;
	}

	void ReuseGenerationState(
		Generation& next,
		const Generation& previous)
	{
		std::unordered_map<ReusableCellKey, const ProbeWork*, ReusableCellKeyHash>
			previousCells;
		previousCells.reserve(previous.m_probes.size());
		for (const ProbeWork& probe : previous.m_probes)
		{
			previousCells.emplace(
				ReusableCellKey{
					probe.m_key.m_worldCell,
					probe.m_key.m_geometryGeneration,
					probe.m_key.m_cascade },
				&probe);
		}

		const bool bReuseIrradiance =
			next.m_request.m_lightingGeneration ==
				previous.m_request.m_lightingGeneration &&
			next.m_request.m_randomSeed == previous.m_request.m_randomSeed &&
			next.m_request.m_qualitySettings.m_targetSamplesPerProbe ==
				previous.m_request.m_qualitySettings.m_targetSamplesPerProbe;
		next.m_initialQueue.clear();
		next.m_warmingQueue.clear();
		next.m_refinementQueue.clear();
		next.m_initialCursor = 0u;
		next.m_readyCount = 0u;
		next.m_refinedCount = 0u;
		next.m_dirtyCount = 0u;
		for (uint32_t probeIndex = 0u;
			probeIndex < next.m_probes.size();
			++probeIndex)
		{
			ProbeWork& probe = next.m_probes[probeIndex];
			const ReusableCellKey key{
				probe.m_key.m_worldCell,
				probe.m_key.m_geometryGeneration,
				probe.m_key.m_cascade };
			const auto found = previousCells.find(key);
			if (found != previousCells.end())
			{
				const ProbeWork& previousProbe = *found->second;
				const bool bHasTransport =
					previousProbe.m_state == ERuntimeGIProbeState::Warming ||
					previousProbe.m_state == ERuntimeGIProbeState::Ready ||
					previousProbe.m_state == ERuntimeGIProbeState::Refining;
				if (bHasTransport)
				{
					probe.m_probe = previousProbe.m_probe;
					if (bReuseIrradiance)
					{
						probe.m_accumulator = previousProbe.m_accumulator;
						probe.m_bReady = previousProbe.m_probe.m_validity <= 0.05f ||
							probe.m_accumulator.m_sampleCount >= next.m_request
								.m_qualitySettings.m_initialSamplesPerProbe;
						probe.m_bRefined = previousProbe.m_probe.m_validity <= 0.05f ||
							probe.m_accumulator.m_sampleCount >= next.m_request
								.m_qualitySettings.m_targetSamplesPerProbe;
						probe.m_state = probe.m_bRefined ?
							ERuntimeGIProbeState::Ready :
							(probe.m_bReady ?
								ERuntimeGIProbeState::Refining :
								ERuntimeGIProbeState::Warming);
					}
					else if (probe.m_probe.m_validity <= 0.05f)
					{
						probe.m_state = ERuntimeGIProbeState::Ready;
						probe.m_bReady = true;
						probe.m_bRefined = true;
					}
					else
					{
						probe.m_probe.m_irradiance = {};
						probe.m_state = ERuntimeGIProbeState::Warming;
					}
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
			if (probe.m_state == ERuntimeGIProbeState::Uninitialized)
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
		std::sort(
			next.m_initialQueue.begin(),
			next.m_initialQueue.end(),
			[&next](uint32_t lhs, uint32_t rhs)
			{
				const ProbeWork& lhsProbe = next.m_probes[lhs];
				const ProbeWork& rhsProbe = next.m_probes[rhs];
				if (lhsProbe.m_key.m_cascade != rhsProbe.m_key.m_cascade)
				{
					return lhsProbe.m_key.m_cascade <
						rhsProbe.m_key.m_cascade;
				}
				const glm::vec3 lhsDelta = lhsProbe.m_probe.m_position -
					next.m_request.m_cameraPosition;
				const glm::vec3 rhsDelta = rhsProbe.m_probe.m_position -
					next.m_request.m_cameraPosition;
				const float lhsDistance = glm::dot(lhsDelta, lhsDelta);
				const float rhsDistance = glm::dot(rhsDelta, rhsDelta);
				return lhsDistance != rhsDistance ?
					lhsDistance < rhsDistance : lhs < rhs;
			});
	}

	bool TakeJob(Job& outJob)
	{
		std::unique_lock<std::mutex> lock(m_mutex);
		for (;;)
		{
			m_condition.wait(lock, [this]()
			{
				return m_bStopWorkers ||
					(m_generation &&
						!m_bPaused &&
						m_bWorkAllowed &&
						m_workTokensMilliseconds > 0.0 &&
						!m_generation->m_bFailed &&
						!m_generation->m_cancel.load(
							std::memory_order_acquire) &&
						HasQueuedWork(*m_generation));
			});
			if (m_bStopWorkers)
			{
				return false;
			}

			auto generation = m_generation;
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

			ProbeWork& work = generation->m_probes[probeIndex];
			if (work.m_bInFlight)
			{
				continue;
			}
			work.m_bInFlight = true;
			outJob.m_generation = std::move(generation);
			outJob.m_probeIndex = probeIndex;
			outJob.m_work = work;
			return true;
		}
	}

	static bool HasQueuedWork(const Generation& generation) noexcept
	{
		return !generation.m_warmingQueue.empty() ||
			generation.m_initialCursor < generation.m_initialQueue.size() ||
			!generation.m_refinementQueue.empty();
	}

	void WorkerMain()
	{
		ConfigureBackgroundWorker();
		for (;;)
		{
			Job job;
			if (!TakeJob(job))
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

	bool ExecuteJob(Job& job, std::string& outDiagnostic)
	{
		Generation& generation = *job.m_generation;
		ProbeWork& work = job.m_work;
		GIProbeTraceRequest traceRequest = generation.m_traceRequest;
		traceRequest.m_settings.m_minProbeSpacing = work.m_spacing;
		traceRequest.m_volumeMin = work.m_traceVolumeMin;
		traceRequest.m_volumeMax = work.m_traceVolumeMax;
		if (work.m_state == ERuntimeGIProbeState::Uninitialized)
		{
			work.m_state = ERuntimeGIProbeState::Placement;
			const float visibilityMaxDistance =
				CalculateGIProbeVisibilityMaxDistance(
					*generation.m_data,
					generation.m_data->m_bricks[work.m_brickIndex]);
			work.m_state = ERuntimeGIProbeState::Transport;
			if (!TraceGIProbeTransport(
					traceRequest,
					*generation.m_request.m_sampler,
					work.m_seed,
					visibilityMaxDistance,
					work.m_probe,
					outDiagnostic))
			{
				return false;
			}
			if (work.m_probe.m_validity <= 0.05f)
			{
				work.m_state = ERuntimeGIProbeState::Ready;
				work.m_bReady = true;
				work.m_bRefined = true;
				return true;
			}
			work.m_state = ERuntimeGIProbeState::Warming;
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
			work.m_state = ERuntimeGIProbeState::Ready;
			work.m_bReady = true;
			work.m_bRefined = true;
			return true;
		}
		if (!AccumulateGIProbeIrradianceRange(
				traceRequest,
				*generation.m_request.m_sampler,
				work.m_probe.m_position,
				work.m_seed,
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
			work.m_state = work.m_accumulator.m_sampleCount < targetSamples ?
				ERuntimeGIProbeState::Refining :
				ERuntimeGIProbeState::Ready;
		}
		work.m_bRefined =
			work.m_accumulator.m_sampleCount >= targetSamples;
		return true;
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
		if (current.m_key.m_slotGeneration !=
			job.m_work.m_key.m_slotGeneration)
		{
			return;
		}
		current.m_bInFlight = false;
		generation->m_workerCpuMilliseconds += elapsedMilliseconds;
		m_workTokensMilliseconds = (std::max)(
			0.0,
			m_workTokensMilliseconds - elapsedMilliseconds);
		if (!bSuccess)
		{
			generation->m_bFailed = true;
			generation->m_diagnostic = diagnostic.empty() ?
				"runtime GI probe tracing failed" : std::move(diagnostic);
			generation->m_cancel.store(true, std::memory_order_release);
			m_status.m_lifecycle = ERuntimeGIProbesLifecycle::Failed;
			m_status.m_diagnostic = generation->m_diagnostic;
			return;
		}

		const bool bWasReady = current.m_bReady;
		const bool bWasRefined = current.m_bRefined;
		const uint32_t previousSamples = current.m_accumulator.m_sampleCount;
		current = job.m_work;
		current.m_bInFlight = false;
		generation->m_tracedRays +=
			current.m_accumulator.m_sampleCount - previousSamples;
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
		m_status.m_lifecycle =
			generation->m_refinedCount == generation->m_probes.size() ?
				ERuntimeGIProbesLifecycle::Ready :
				ERuntimeGIProbesLifecycle::Tracing;
		UpdateStatusLocked();
		m_condition.notify_all();
	}

	void UpdateStatusLocked()
	{
		m_status.m_bEnabled = m_generation != nullptr;
		m_status.m_bPaused = m_bPaused;
		m_status.m_bThrottled = m_generation &&
			(!m_bWorkAllowed ||
				(HasQueuedWork(*m_generation) &&
					m_workTokensMilliseconds <= 0.0));
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
		m_status.m_dirtyProbeCount = generation.m_dirtyCount;
		m_status.m_queuedProbeCount = probeCount -
			generation.m_refinedCount;
		m_status.m_coverage = probeCount > 0u ?
			static_cast<float>(generation.m_readyCount) /
				static_cast<float>(probeCount) : 0.0f;

		uint64_t accumulatedSamples = 0u;
		const uint32_t targetSamples = generation.m_request.m_qualitySettings
			.m_targetSamplesPerProbe;
		for (const ProbeWork& probe : generation.m_probes)
		{
			accumulatedSamples += probe.m_bRefined ? targetSamples :
				probe.m_accumulator.m_sampleCount;
		}
		m_status.m_refinement = probeCount > 0u && targetSamples > 0u ?
			static_cast<float>(accumulatedSamples) /
				static_cast<float>(
					static_cast<uint64_t>(probeCount) * targetSamples) :
			0.0f;
		m_status.m_tracedRayCount = generation.m_tracedRays;
		m_status.m_workerCpuMilliseconds = static_cast<float>(
			generation.m_workerCpuMilliseconds);
		const double elapsedSeconds = std::chrono::duration<double>(
			std::chrono::steady_clock::now() - generation.m_started).count();
		m_status.m_raysPerSecond = elapsedSeconds > 0.0 ?
			static_cast<float>(
				static_cast<double>(generation.m_tracedRays) / elapsedSeconds) :
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
		const uint32_t fineCascadeReadyCount =
			m_generation->m_data->m_bricks[0].m_probeCount;
		const uint32_t requiredReadyCount = (std::max)(
			configuredReadyCount,
			fineCascadeReadyCount);
		if (!m_generation->m_bPublished &&
			m_generation->m_readyCount < requiredReadyCount)
		{
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

		const auto publishStarted = std::chrono::steady_clock::now();
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
			(m_generation->m_request.m_geometryGeneration << 1u) ^
			m_generation->m_readyCount ^ 0x9e3779b97f4a7c15ull;
		working.m_lightingHash =
			(m_generation->m_request.m_lightingGeneration << 32u) ^
			m_nextPublishedRevision;

		GIProbesDataPtr published = GIProbesDataPtr::Make(working);
		const uint64_t publishedBytes = EstimatePublishedBytes(*published);
		if (publishedBytes > m_generation->m_request.m_qualitySettings
			.m_maxDirtyUploadBytesPerFrame)
		{
			m_generation->m_bFailed = true;
			m_generation->m_diagnostic =
				"runtime GI publication exceeded its configured upload budget";
			m_status.m_lifecycle = ERuntimeGIProbesLifecycle::Failed;
			m_status.m_diagnostic = m_generation->m_diagnostic;
			return;
		}
		std::string validationDiagnostic;
		if (!published->Validate(validationDiagnostic))
		{
			m_generation->m_bFailed = true;
			m_generation->m_diagnostic =
				"runtime GI probes rejected a publication: " +
				validationDiagnostic;
			m_status.m_lifecycle = ERuntimeGIProbesLifecycle::Failed;
			m_status.m_diagnostic = m_generation->m_diagnostic;
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
		m_status.m_lastPublicationMilliseconds = static_cast<float>(
			std::chrono::duration<double, std::milli>(
				std::chrono::steady_clock::now() - publishStarted).count());
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
	m_impl->EnsureWorkers(request.m_qualitySettings.m_workerCount);
	{
		const std::lock_guard<std::mutex> lock(m_impl->m_mutex);
		const uint64_t retainedRevision =
			m_impl->m_status.m_publishedRevision;
		const uint64_t retainedBytes =
			m_impl->m_status.m_publishedBytes;
		if (m_impl->m_generation)
		{
			if (request.m_bReuseExistingCells)
			{
				m_impl->ReuseGenerationState(
					*generation,
					*m_impl->m_generation);
			}
			m_impl->m_generation->m_cancel.store(
				true,
				std::memory_order_release);
		}
		m_impl->m_generation = std::move(generation);
		m_impl->m_lastRequest = request;
		m_impl->m_bHasLastRequest = true;
		m_impl->m_status = {};
		m_impl->m_status.m_bEnabled = true;
		m_impl->m_status.m_publishedRevision = retainedRevision;
		m_impl->m_status.m_publishedBytes = retainedBytes;
		m_impl->m_workTokensMilliseconds =
			request.m_qualitySettings.m_cpuBudgetMilliseconds;
		m_impl->m_status.m_lifecycle =
			m_impl->m_generation->m_refinedCount ==
				m_impl->m_generation->m_probes.size() ?
				ERuntimeGIProbesLifecycle::Ready :
				ERuntimeGIProbesLifecycle::Tracing;
		m_impl->m_status.m_diagnostic =
			m_impl->m_generation->m_effectiveCapacity <
				request.m_qualitySettings.m_maxActiveProbes ?
				"runtime GI capacity was limited by the publication upload budget" :
				"runtime GI probes started with an immutable prepared scene";
		m_impl->UpdateStatusLocked();
	}
	m_impl->m_condition.notify_all();
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
	request.m_bReuseExistingCells = false;
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
		m_impl->m_publishedData.Clear();
		m_impl->m_status = {};
		m_impl->m_bPaused = false;
		m_impl->m_workTokensMilliseconds = 0.0;
		m_impl->m_status.m_lifecycle = ERuntimeGIProbesLifecycle::Disabled;
		m_impl->m_status.m_diagnostic =
			"experimental runtime GI probes are disabled";
	}
	m_impl->m_condition.notify_all();
}

void RuntimeGIProbesService::SetPaused(bool bPaused) noexcept
{
	{
		const std::lock_guard<std::mutex> lock(m_impl->m_mutex);
		m_impl->m_bPaused = bPaused;
		m_impl->UpdateStatusLocked();
	}
	m_impl->m_condition.notify_all();
}

void RuntimeGIProbesService::SetWorkAllowed(bool bAllowed) noexcept
{
	{
		const std::lock_guard<std::mutex> lock(m_impl->m_mutex);
		if (m_impl->m_bWorkAllowed == bAllowed)
		{
			return;
		}
		m_impl->m_bWorkAllowed = bAllowed;
		m_impl->UpdateStatusLocked();
	}
	m_impl->m_condition.notify_all();
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
	m_impl->m_condition.notify_all();
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
