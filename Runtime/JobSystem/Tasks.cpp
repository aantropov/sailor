#include "JobSystem.h"
#include <windows.h>
#include <fcntl.h>
#include <conio.h>
#include <algorithm>
#include <mutex>
#include <set>
#include <string>
#include "Core/Utils.h"

using namespace std;
using namespace Sailor;
using namespace Sailor::JobSystem;

void ITask::Join(const TWeakPtr<ITask>& job)
{
	if (job && job.Lock()->AddDependency(this))
	{
		++m_numBlockers;
	}
}

void ITask::Join(ITask* job)
{
	if (job && job->AddDependency(this))
	{
		++m_numBlockers;
	}
}

bool ITask::AddDependency(ITask* job)
{
	std::unique_lock<std::mutex> lk(m_mutex);

	if (IsFinished())
	{
		return false;
	}

	m_dependencies.emplace_back(job);
	return true;
}

void ITask::Join(const std::vector<TWeakPtr<ITask>>& jobs)
{
	for (const auto& job : jobs)
	{
		Join(job);
	}
}

void ITask::Complete()
{
	std::unique_lock<std::mutex> lk(m_mutex);

	std::unordered_map<EThreadType, uint32_t> threadTypesToRefresh;

	for (auto& job : m_dependencies)
	{
		if (--job->m_numBlockers == 0)
		{
			threadTypesToRefresh[job->GetThreadType()]++;
		}
	}

	m_dependencies.clear();

	for (auto& threadType : threadTypesToRefresh)
	{
		App::GetSubmodule<JobSystem::Scheduler>()->NotifyWorkerThread(threadType.first, threadType.second > 1);
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
