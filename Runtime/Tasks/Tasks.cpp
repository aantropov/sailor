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

	TSharedPtr<ITask> pOtherJob = job.Lock();
	pOtherJob->AddDependency(m_self.Lock());
}

bool ITask::AddDependency(TSharedPtr<ITask> dependentJob)
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

void ITask::SetChainedTaskPrev(TWeakPtr<ITask>& job)
{
	check(!m_chainedTaskPrev);
	if (job)
	{
		//Job could be equal null
		m_chainedTaskPrev = job.Lock();
	}
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
	SAILOR_PROFILE_FUNCTION();

	check(!IsFinished());

	auto& syncBlock = App::GetSubmodule<Scheduler>()->GetTaskSyncBlock(*this);

	std::unique_lock<std::mutex> lk(syncBlock.m_mutex);

	check(magic_enum::enum_count<EThreadType>() == 4);
	TVector<uint32_t> threadTypesToRefresh({ 0,0,0,0 });

	for (auto& job : m_dependencies)
	{
		if (auto pJob = job.TryLock())
		{
			if (--pJob->m_numBlockers == 0)
			{
				threadTypesToRefresh[(uint32_t)pJob->GetThreadType()]++;
			}
		}
	}

	m_dependencies.Clear();

	for (uint32_t i = 0; i < threadTypesToRefresh.Num(); i++)
	{
		if (threadTypesToRefresh[i] > 0)
		{
			App::GetSubmodule<Tasks::Scheduler>()->NotifyWorkerThread((EThreadType)i, threadTypesToRefresh[i] > 1);
		}
	}

	m_state |= StateMask::IsFinishedBit;
	syncBlock.m_onComplete.notify_all();
}

void ITask::Wait()
{
	SAILOR_PROFILE_FUNCTION();

	auto& syncBlock = App::GetSubmodule<Scheduler>()->GetTaskSyncBlock(*this);

	std::unique_lock<std::mutex> lk(syncBlock.m_mutex);

	if (!IsFinished())
	{
		syncBlock.m_onComplete.wait(lk);
	}
}
