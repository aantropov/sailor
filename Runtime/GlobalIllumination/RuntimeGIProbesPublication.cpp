#include "GlobalIllumination/RuntimeGIProbesServiceInternal.h"

#include "Containers/Hash.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <utility>

namespace Sailor
{
	namespace
	{
		uint64_t EstimatePublishedBytes(const GIProbesData& data) noexcept
		{
			return sizeof(GIProbesData) + data.m_bricks.Num() * sizeof(GIProbeBrick) +
				   data.m_probes.Num() * sizeof(GIProbe);
		}
	}

	void RuntimeGIProbesService::Impl::FailGenerationLocked(Generation& generation, std::string diagnostic)
	{
		generation.m_bFailed = true;
		generation.m_cancel.store(true, std::memory_order_release);
		m_status.m_lifecycle = ERuntimeGIProbesLifecycle::Failed;
		m_status.m_diagnostic = std::move(diagnostic);
	}

	void RuntimeGIProbesService::Impl::CommitJob(const Job& job,
		bool bSuccess,
		std::string diagnostic,
		double elapsedMilliseconds)
	{
		const std::lock_guard<std::mutex> lock(m_mutex);
		const std::shared_ptr<Generation>& generation = job.m_generation;
		if (!m_generation || m_generation != generation || generation->m_cancel.load(std::memory_order_acquire))
		{
			return;
		}
		ProbeWork& current = generation->m_probes[job.m_probeIndex];
		m_workTokensMilliseconds -= elapsedMilliseconds;
		if (!bSuccess)
		{
			FailGenerationLocked(
				*generation, diagnostic.empty() ? "runtime GI probe tracing failed" : std::move(diagnostic));
			return;
		}

		const bool bWasReady = current.m_bReady;
		const bool bWasRefined = current.m_bRefined;
		const uint32_t previousProgress = GetProgressSampleCount(*generation, current);
		current = job.m_work;
		generation->m_progressSampleCount += GetProgressSampleCount(*generation, current) - previousProgress;
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

	void RuntimeGIProbesService::Impl::UpdateStatusLocked()
	{
		m_status.m_bEnabled = m_generation != nullptr;
		m_status.m_bPaused = m_bPaused;
		m_status.m_workerCount = m_workerCount;
		if (!m_generation)
		{
			return;
		}
		const Generation& generation = *m_generation;
		const uint32_t probeCount = static_cast<uint32_t>(generation.m_probes.size());
		m_status.m_sceneGeneration = generation.m_request.m_geometryGeneration;
		m_status.m_lightingGeneration = generation.m_request.m_lightingGeneration;
		m_status.m_capacity = generation.m_effectiveCapacity;
		m_status.m_activeProbeCount = probeCount;
		m_status.m_readyProbeCount = generation.m_readyCount;
		m_status.m_coverage =
			probeCount > 0u ? static_cast<float>(generation.m_readyCount) / static_cast<float>(probeCount) : 0.0f;

		const uint32_t targetSamples = generation.m_request.m_qualitySettings.m_targetSamplesPerProbe;
		m_status.m_refinement = probeCount > 0u && targetSamples > 0u
									? static_cast<float>(generation.m_progressSampleCount) /
										  static_cast<float>(static_cast<uint64_t>(probeCount) * targetSamples)
									: 0.0f;
		if (generation.m_bFailed)
		{
			m_status.m_lifecycle = ERuntimeGIProbesLifecycle::Failed;
		}
		else if (m_bPaused)
		{
			m_status.m_lifecycle = ERuntimeGIProbesLifecycle::Paused;
		}
		else if (!m_bWorkAllowed || (HasQueuedWork(generation) && m_workTokensMilliseconds <= 0.0))
		{
			m_status.m_lifecycle = ERuntimeGIProbesLifecycle::Throttled;
		}
		else
		{
			m_status.m_lifecycle = generation.m_refinedCount == generation.m_probes.size()
									   ? ERuntimeGIProbesLifecycle::Ready
									   : ERuntimeGIProbesLifecycle::Tracing;
		}
	}

	void RuntimeGIProbesService::Impl::PublishIfNeeded()
	{
		const std::lock_guard<std::mutex> lock(m_mutex);
		if (!m_generation || m_generation->m_bFailed || m_generation->m_dirtyCount == 0u)
		{
			UpdateStatusLocked();
			return;
		}
		const uint32_t probeCount = static_cast<uint32_t>(m_generation->m_probes.size());
		const uint32_t configuredReadyCount = static_cast<uint32_t>(std::ceil(
			static_cast<float>(probeCount) * m_generation->m_request.m_qualitySettings.m_initialPublicationCoverage));
		const uint32_t minimumCellReadyCount = (std::min)(probeCount, 8u);
		const uint32_t requiredReadyCount = (std::max)(configuredReadyCount, minimumCellReadyCount);
		if (!m_generation->m_bPublished && m_generation->m_readyCount < requiredReadyCount)
		{
			if (m_publishedData)
			{
				m_status.m_diagnostic = "retaining the last runtime GI snapshot while the replacement grid warms";
			}
			UpdateStatusLocked();
			return;
		}
		const auto now = std::chrono::steady_clock::now();
		const double minimumInterval = 1.0 / m_generation->m_request.m_qualitySettings.m_maxPublicationsPerSecond;
		if (m_lastPublication.time_since_epoch().count() != 0 &&
			std::chrono::duration<double>(now - m_lastPublication).count() < minimumInterval)
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
			relocatedCount += (probe.m_flags & static_cast<uint32_t>(EGIProbeFlag::Relocated)) != 0u ? 1u : 0u;
			validity += probe.m_validity;
		}
		working.m_diagnostics.m_invalidProbeCount = invalidCount;
		working.m_diagnostics.m_relocatedProbeCount = relocatedCount;
		working.m_diagnostics.m_averageValidity = probeCount > 0u ? validity / static_cast<float>(probeCount) : 0.0f;
		working.m_diagnostics.m_bakeDurationSeconds =
			static_cast<float>(std::chrono::duration<double>(now - m_generation->m_started).count());
		working.m_diagnostics.m_message =
			m_generation->m_refinedCount == probeCount
				? "runtime GI probes reached the target sample count"
				: "runtime GI probes are refining with environment fallback for missing cells";
		working.m_layoutHash = ComputeGIProbesLayoutHash(working);
		working.m_transportHash = m_generation->m_request.m_geometryGeneration;
		HashCombine(working.m_transportHash, m_generation->m_readyCount);
		working.m_lightingHash = (m_generation->m_request.m_lightingGeneration << 32u) ^ m_nextPublishedRevision;

		GIProbesDataPtr published = GIProbesDataPtr::Make(working);
		const uint64_t publishedBytes = EstimatePublishedBytes(*published);
		if (publishedBytes > m_generation->m_request.m_qualitySettings.m_maxDirtyUploadBytesPerFrame)
		{
			FailGenerationLocked(*m_generation, "runtime GI publication exceeded its configured upload budget");
			return;
		}
		std::string validationDiagnostic;
		if (!published->Validate(validationDiagnostic))
		{
			FailGenerationLocked(*m_generation, "runtime GI probes rejected a publication: " + validationDiagnostic);
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
}
