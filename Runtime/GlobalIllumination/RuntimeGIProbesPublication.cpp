#include "GlobalIllumination/RuntimeGIProbesServiceInternal.h"
#include "GlobalIllumination/RuntimeGIProbesGrid.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <utility>

namespace Sailor
{
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
		SAILOR_PROFILE_FUNCTION();
		const std::lock_guard<std::mutex> lock(m_mutex);
		const TSharedPtr<Generation>& generation = job.m_generation;
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
		const uint32_t previousProgress = GetProgressSampleCount(*generation, current);
		current = job.m_work;
		generation->m_progressSampleCount += GetProgressSampleCount(*generation, current) - previousProgress;
		if (!bWasReady && current.m_bReady)
		{
			++generation->m_readyCount;
		}
		if (current.m_bReady)
		{
			generation->m_workingData->m_probes[job.m_probeIndex] = current.m_probe;
			++generation->m_dataRevision;
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
		m_status.m_bEnabled = m_generation.IsValid();
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
			m_status.m_lifecycle = generation.IsFullyRefined()
									   ? ERuntimeGIProbesLifecycle::Ready
									   : ERuntimeGIProbesLifecycle::Tracing;
		}
	}

	bool RuntimeGIProbesService::Impl::CapturePublication(Publication& outPublication)
	{
		SAILOR_PROFILE_FUNCTION();
		const std::lock_guard<std::mutex> lock(m_mutex);
		if (!m_generation || m_generation->m_bFailed ||
			m_generation->m_dataRevision == m_generation->m_publishedDataRevision)
		{
			UpdateStatusLocked();
			return false;
		}
		const uint32_t probeCount = static_cast<uint32_t>(m_generation->m_probes.size());
		const uint32_t configuredReadyCount = static_cast<uint32_t>(std::ceil(
			static_cast<float>(probeCount) * m_generation->m_request.m_qualitySettings.m_initialPublicationCoverage));
		const uint32_t minimumCellReadyCount = (std::min)(probeCount, 8u);
		const uint32_t requiredReadyCount = (std::max)(configuredReadyCount, minimumCellReadyCount);
		if (m_generation->m_publishedDataRevision == 0u && m_generation->m_readyCount < requiredReadyCount)
		{
			if (m_publishedData)
			{
				m_status.m_diagnostic = "retaining the last runtime GI snapshot while the replacement grid warms";
			}
			UpdateStatusLocked();
			return false;
		}
		const auto now = std::chrono::steady_clock::now();
		const double minimumInterval = 1.0 / m_generation->m_request.m_qualitySettings.m_maxPublicationsPerSecond;
		if (m_lastPublication.time_since_epoch().count() != 0 &&
			std::chrono::duration<double>(now - m_lastPublication).count() < minimumInterval)
		{
			UpdateStatusLocked();
			return false;
		}

		outPublication.m_generation = m_generation;
		outPublication.m_snapshot = GIProbesDataPtr::Make(*m_generation->m_workingData);
		outPublication.m_dataRevision = m_generation->m_dataRevision;
		auto& diagnostics = outPublication.m_snapshot->m_diagnostics;
		diagnostics.m_bakeDurationSeconds =
			static_cast<float>(std::chrono::duration<double>(now - m_generation->m_started).count());
		diagnostics.m_message = m_generation->IsFullyRefined()
			? "runtime GI probes reached the target sample count"
			: "runtime GI probes are refining with environment fallback for missing cells";
		return true;
	}

	std::string RuntimeGIProbesService::Impl::PreparePublication(Publication& publication)
	{
		SAILOR_PROFILE_FUNCTION();
		GIProbesData& data = *publication.m_snapshot;
		const uint32_t probeCount = static_cast<uint32_t>(data.m_probes.Num());
		publication.m_uploadBytes = RuntimeGIProbesInternal::GetPublicationUploadBytes(probeCount);
		if (publication.m_uploadBytes > publication.m_generation->m_request.m_qualitySettings.m_maxDirtyUploadBytesPerFrame)
		{
			return "runtime GI publication exceeded its configured upload budget";
		}
		uint32_t invalidCount = 0u;
		uint32_t relocatedCount = 0u;
		float validity = 0.0f;
		for (const GIProbe& probe : data.m_probes)
		{
			invalidCount += probe.m_validity <= 0.05f ? 1u : 0u;
			relocatedCount += (probe.m_flags & static_cast<uint32_t>(EGIProbeFlag::Relocated)) != 0u ? 1u : 0u;
			validity += probe.m_validity;
		}
		data.m_diagnostics.m_invalidProbeCount = invalidCount;
		data.m_diagnostics.m_relocatedProbeCount = relocatedCount;
		data.m_diagnostics.m_averageValidity = probeCount > 0u ? validity / static_cast<float>(probeCount) : 0.0f;
		data.m_layoutHash = ComputeGIProbesLayoutHash(data);
		if (!ComputeGIProbesTransportHash(data, data.m_transportHash, &publication.m_generation->m_cancel) ||
			!ComputeGIProbesLightingHash(data, data.m_lightingHash, &publication.m_generation->m_cancel))
		{
			return "runtime GI publication was cancelled";
		}
		std::string validationDiagnostic;
		if (!data.Validate(validationDiagnostic))
		{
			return "runtime GI probes rejected a publication: " + validationDiagnostic;
		}
		return {};
	}

	void RuntimeGIProbesService::Impl::CommitPublication(Publication& publication, std::string diagnostic)
	{
		SAILOR_PROFILE_FUNCTION();
		const std::lock_guard<std::mutex> lock(m_mutex);
		if (m_generation != publication.m_generation || publication.m_generation->m_cancel.load(std::memory_order_acquire) ||
			publication.m_dataRevision <= publication.m_generation->m_publishedDataRevision)
		{
			return;
		}
		if (!diagnostic.empty())
		{
			FailGenerationLocked(*m_generation, std::move(diagnostic));
			return;
		}

		m_status.m_diagnostic = publication.m_snapshot->m_diagnostics.m_message;
		// The caller releases the previous snapshot after leaving the service lock.
		std::swap(m_publishedData, publication.m_snapshot);
		m_generation->m_publishedDataRevision = publication.m_dataRevision;
		m_status.m_publishedRevision = m_nextPublishedRevision++;
		m_status.m_publishedBytes = publication.m_uploadBytes;
		m_lastPublication = std::chrono::steady_clock::now();
		UpdateStatusLocked();
	}

	void RuntimeGIProbesService::Impl::PublishIfNeeded()
	{
		SAILOR_PROFILE_FUNCTION();
		Publication publication;
		if (CapturePublication(publication))
		{
			CommitPublication(publication, PreparePublication(publication));
		}
	}
}
