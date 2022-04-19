#include "JobSystem.h"
#include <windows.h>
#include <fcntl.h>
#include <conio.h>
#include <algorithm>
#include <mutex>
#include <set>
#include <string>
#include "Core/Utils.h"
#include "Core/Submodule.h"
#include "JobSystem/JobSystem.h"
#include "Containers/Map.h"

using namespace std;
using namespace Sailor;
using namespace Sailor::JobSystem;

void ITask::Join(const TWeakPtr<ITask>& job)
{
	if (!job)
	{
		return;
	}

	TSharedPtr<ITask> pOtherJob = job.Lock();

	if (pOtherJob->AddDependency(m_self.Lock()))
	{
		this->m_numBlockers++;
	}
}

bool ITask::AddDependency(TSharedPtr<ITask> dependentJob)
{
	std::unique_lock<std::mutex> lk(m_mutex);

	if (IsFinished())
	{
		return false;
	}

	m_dependencies.Emplace(dependentJob);
	return true;
}

void ITask::SetChainedTaskPrev(TWeakPtr<ITask>& job)
{
	assert(!m_chainedTaskPrev);
	m_chainedTaskPrev = job;
}

void ITask::Join(const TVector<TWeakPtr<ITask>>& jobs)
{
	for (const auto& job : jobs)
	{
		Join(job);
	}
}

TSharedPtr<ITask> ITask::Run()
{
	TSharedPtr<ITask> res = m_self.Lock();
	App::GetSubmodule<Scheduler>()->Run(res);
	return res;
}

void ITask::Complete()
{
	assert(!m_bIsFinished);

	std::unique_lock<std::mutex> lk(m_mutex);

	TMap<EThreadType, uint32_t> threadTypesToRefresh;

	for (auto& job : m_dependencies)
	{
		if (auto pJob = job.TryLock())
		{
			if (--pJob->m_numBlockers == 0)
			{
				threadTypesToRefresh[pJob->GetThreadType()]++;
			}
		}
	}

	m_dependencies.Clear();

	for (auto& threadType : threadTypesToRefresh)
	{
		App::GetSubmodule<JobSystem::Scheduler>()->NotifyWorkerThread(threadType.m_first, threadType.m_second > 1);
	}

	m_bIsFinished = true;
	m_onComplete.notify_all();
}

void ITask::Wait()
{
	SAILOR_PROFILE_FUNCTION();
	std::unique_lock<std::mutex> lk(m_mutex);

	if (!m_bIsFinished)
	{
		m_onComplete.wait(lk);
	}
}
