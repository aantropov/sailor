#include "Scheduler.h"
#if defined(_WIN32)
#include <windows.h>
#endif
#include <fcntl.h>
#include <algorithm>
#include <mutex>
#include <set>
#include <string>
#include "Core/Utils.h"
#include "Core/Submodule.h"
#include "Tasks/Tasks.h"
#include "Containers/Map.h"

using namespace std;
using namespace Sailor;
using namespace Sailor::Tasks;

void ITask::Join(const TWeakPtr<ITask>& job)
{
	if (!job)
	{
		return;
	}

	ITaskPtr pOtherJob = job.Lock();
	pOtherJob->AddDependency(m_self.Lock());
}

bool ITask::AddDependency(ITaskPtr dependentJob)
{
	if (!m_bOwnsTaskSyncBlock || IsFinished())
	{
		return false;
	}
	auto* scheduler = App::GetSubmodule<Scheduler>();
	if (!scheduler)
	{
		return false;
	}
	auto& syncBlock = scheduler->GetTaskSyncBlock(*this);
	std::unique_lock<std::mutex> lk(syncBlock.m_mutex);
	if (IsFinished())
	{
		return false;
	}

	dependentJob->m_numBlockers++;
	m_dependencies.Emplace(dependentJob);
	return true;
}

void ITask::SetChainedTaskPrev(ITaskPtr job)
{
	check(!m_chainedTaskPrev);
	if (job)
	{
		//Job could be equal null
		m_chainedTaskPrev = std::move(job);
	}
}

void ITask::Join(const TVector<TWeakPtr<ITask>>& jobs)
{
	for (const auto& job : jobs)
	{
		Join(job);
	}
}

ITaskPtr ITask::Run()
{
	ITaskPtr res = m_self.Lock();
	App::GetSubmodule<Scheduler>()->Run(res);
	return res;
}

void ITask::Complete()
{
	SAILOR_PROFILE_FUNCTION();

	check(!IsFinished());

	auto scheduler = App::GetSubmodule<Tasks::Scheduler>();
	auto& syncBlock = scheduler->GetTaskSyncBlock(*this);
	{
		std::unique_lock<std::mutex> lk(syncBlock.m_mutex);

		for (auto& job : m_dependencies)
		{
			if (auto pJob = job.TryLock())
			{
				if (--pJob->m_numBlockers == 0)
				{
					scheduler->NotifyWorkerThread(pJob->GetThreadType());
				}
			}
		}

		m_chainedTaskPrev.Clear();
		m_dependencies.Clear();
		m_state |= StateMask::IsFinishedBit;
		syncBlock.m_bCompletionFlag = true;
	}

	syncBlock.m_onComplete.notify_all();
}

void ITask::Wait()
{
	SAILOR_PROFILE_FUNCTION();
	if (IsFinished() || !m_bOwnsTaskSyncBlock)
	{
		return;
	}

	auto* scheduler = App::GetSubmodule<Scheduler>();
	if (!scheduler)
	{
		return;
	}
	auto& syncBlock = scheduler->GetTaskSyncBlock(*this);
	std::unique_lock<std::mutex> lk(syncBlock.m_mutex);

	if (!IsFinished())
	{
		syncBlock.m_onComplete.wait(lk, [&]() { return syncBlock.m_bCompletionFlag; });
	}
}
