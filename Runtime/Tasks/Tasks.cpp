#include "Scheduler.h"
#include <windows.h>
#include <fcntl.h>
#include <conio.h>
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
	auto& syncBlock = App::GetSubmodule<Scheduler>()->GetTaskSyncBlock(*this);
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

	auto& syncBlock = App::GetSubmodule<Scheduler>()->GetTaskSyncBlock(*this);
	std::unique_lock<std::mutex> lk(syncBlock.m_mutex);

	if (!IsFinished())
	{
		syncBlock.m_onComplete.wait(lk, [&]() { return syncBlock.m_bCompletionFlag; });
	}
}
