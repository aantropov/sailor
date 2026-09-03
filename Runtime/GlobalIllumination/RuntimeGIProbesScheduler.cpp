#include "GlobalIllumination/RuntimeGIProbesServiceInternal.h"

#include "Sailor.h"

#include <algorithm>
#include <chrono>
#include <thread>
#include <utility>

namespace Sailor
{
	RuntimeGIProbesService::Impl::~Impl()
	{
		StopWorkerTasks();
	}

	void RuntimeGIProbesService::Impl::StopWorkerTasks() noexcept
	{
		std::vector<Tasks::ITaskPtr> workerTasks;
		{
			const std::lock_guard<std::mutex> lock(m_mutex);
			if (m_generation)
			{
				m_generation->m_cancel.store(true, std::memory_order_release);
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

	bool RuntimeGIProbesService::Impl::TryTakeJob(const std::shared_ptr<Generation>& generation, Job& outJob)
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
		else if (generation->m_initialCursor < generation->m_initialQueue.size())
		{
			probeIndex = generation->m_initialQueue[generation->m_initialCursor++];
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

	bool RuntimeGIProbesService::Impl::HasQueuedWork(const Generation& generation) noexcept
	{
		return !generation.m_warmingQueue.empty() || generation.m_initialCursor < generation.m_initialQueue.size() ||
			   !generation.m_refinementQueue.empty();
	}

	bool RuntimeGIProbesService::Impl::CanDispatchWorkLocked() const noexcept
	{
		return m_generation && !m_bPaused && m_bWorkAllowed && m_workTokensMilliseconds > 0.0 &&
			   !m_generation->m_bFailed && !m_generation->m_cancel.load(std::memory_order_acquire) &&
			   HasQueuedWork(*m_generation);
	}

	RuntimeGIProbesService::Impl::DispatchBatch RuntimeGIProbesService::Impl::GetDispatchBatch() const noexcept
	{
		const std::lock_guard<std::mutex> lock(m_mutex);
		return CanDispatchWorkLocked() ? DispatchBatch{m_generation, m_workerCount} : DispatchBatch{};
	}

	void RuntimeGIProbesService::Impl::WorkerBatch(std::shared_ptr<Generation> generation, uint32_t maximumJobCount)
	{
		for (uint32_t completedJobCount = 0u; completedJobCount < maximumJobCount; ++completedJobCount)
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
			const double elapsedMilliseconds = std::chrono::duration<double, std::milli>(finished - started).count();
			CommitJob(job, bSuccess, std::move(diagnostic), elapsedMilliseconds);

			const float duty = job.m_generation->m_request.m_qualitySettings.m_cpuDutyFraction;
			if (bSuccess && duty < 0.999f)
			{
				const double sleepMilliseconds =
					(std::min)(100.0, elapsedMilliseconds * (1.0 / static_cast<double>(duty) - 1.0));
				if (sleepMilliseconds > 0.05)
				{
					std::this_thread::sleep_for(std::chrono::duration<double, std::milli>(sleepMilliseconds));
				}
			}
		}
	}

	void RuntimeGIProbesService::Impl::PumpWorkerTasks()
	{
		m_workerTasks.erase(std::remove_if(m_workerTasks.begin(),
								m_workerTasks.end(),
								[](const Tasks::ITaskPtr& task) { return !task || task->IsFinished(); }),
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
			const std::shared_ptr<Generation> generation = dispatch.m_generation;
			Tasks::ITaskPtr workerTask = Tasks::CreateTask(
				"Runtime GI Probe Trace", [this, generation]() { WorkerBatch(generation); }, EThreadType::GI);
			m_workerTasks.push_back(workerTask);
			scheduler->Run(workerTask);
		}
	}

}
