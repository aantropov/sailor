#include "GlobalIllumination/RuntimeGIProbesServiceInternal.h"

#include <algorithm>
#include <cmath>

namespace Sailor
{
	RuntimeGIProbesService::RuntimeGIProbesService() : m_impl(std::make_unique<Impl>())
	{
	}

	RuntimeGIProbesService::~RuntimeGIProbesService() = default;

	bool RuntimeGIProbesService::Start(const RuntimeGIProbesStartRequest& request, std::string& outDiagnostic)
	{
		if (!request.m_worldSettings.Validate(outDiagnostic) || !request.m_qualitySettings.Validate(outDiagnostic))
		{
			return false;
		}
		if (!request.m_qualitySettings.m_bEnabled)
		{
			Disable();
			outDiagnostic = "experimental runtime GI probes are disabled by the active quality profile";
			return true;
		}
		if (!request.m_sampler)
		{
			outDiagnostic = "runtime GI probes require an immutable prepared ray sampler";
			return false;
		}

		uint64_t generationId = 0u;
		{
			const std::lock_guard<std::mutex> lock(m_impl->m_mutex);
			generationId = m_impl->m_nextGenerationId++;
		}
		std::shared_ptr<Impl::Generation> generation = m_impl->BuildGeneration(request, generationId, outDiagnostic);
		if (!generation)
		{
			return false;
		}
		{
			const std::lock_guard<std::mutex> lock(m_impl->m_mutex);
			m_impl->m_workerCount = request.m_qualitySettings.m_workerCount;
			const uint64_t retainedRevision = m_impl->m_status.m_publishedRevision;
			const uint64_t retainedBytes = m_impl->m_status.m_publishedBytes;
			if (request.m_bReuseExistingProbes && m_impl->m_generation)
			{
				m_impl->ReuseGenerationState(*generation, *m_impl->m_generation);
			}
			if (m_impl->m_generation)
			{
				m_impl->m_generation->m_cancel.store(true, std::memory_order_release);
			}
			m_impl->m_generation = std::move(generation);
			m_impl->m_lastRequest = request;
			m_impl->m_bHasLastRequest = true;
			m_impl->m_status = {};
			m_impl->m_status.m_publishedRevision = retainedRevision;
			m_impl->m_status.m_publishedBytes = retainedBytes;
			m_impl->m_workTokensMilliseconds = request.m_qualitySettings.m_cpuBudgetMilliseconds;
			m_impl->m_status.m_diagnostic =
				m_impl->m_generation->m_effectiveCapacity < request.m_qualitySettings.m_maxActiveProbes
					? "runtime GI capacity was limited by the publication upload budget"
					: "runtime GI probes started with an immutable prepared scene";
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
				m_impl->m_generation->m_cancel.store(true, std::memory_order_release);
			}
			m_impl->m_generation.reset();
			m_impl->m_workerCount = 0u;
			m_impl->m_publishedData.Clear();
			m_impl->m_status = {};
			m_impl->m_bPaused = false;
			m_impl->m_workTokensMilliseconds = 0.0;
			m_impl->m_status.m_lifecycle = ERuntimeGIProbesLifecycle::Disabled;
			m_impl->m_status.m_diagnostic = "experimental runtime GI probes are disabled";
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
			if (m_impl->m_generation && std::isfinite(deltaTimeSeconds) && deltaTimeSeconds > 0.0f)
			{
				const double budget = m_impl->m_generation->m_request.m_qualitySettings.m_cpuBudgetMilliseconds;
				constexpr double ReferenceFrameSeconds = 1.0 / 60.0;
				const double replenished = budget * (static_cast<double>(deltaTimeSeconds) / ReferenceFrameSeconds);
				m_impl->m_workTokensMilliseconds =
					(std::min)(budget * 2.0, m_impl->m_workTokensMilliseconds + replenished);
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
}
