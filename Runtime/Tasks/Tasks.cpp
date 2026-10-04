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

ITask::ITask(const std::string& name, EThreadType thread, Scheduler* scheduler) :
	m_threadType(thread), m_pScheduler(scheduler), m_name(name)
{
	m_pSyncBlock = scheduler ? scheduler->AcquireTaskSyncBlock() : TUniquePtr<TaskSyncBlock>::Make();
}

void ITask::Join(const TWeakPtr<ITask>& job)
{
	if (auto other = job.TryLock())
	{
		other->AddDependency(m_self.Lock());
	}
}

bool ITask::AddDependency(ITaskPtr dependentJob)
{
	if (IsFinished())
	{
		return false;
	}
	auto& syncBlock = *m_pSyncBlock;
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
	std::lock_guard<std::mutex> lock(m_pSyncBlock->m_mutex);
	check(!m_chainedTaskPrev);
	m_chainedTaskPrev = std::move(job);
}

TVector<TWeakPtr<ITask>> ITask::GetChainedTasksNext() const
{
	std::lock_guard<std::mutex> lock(m_pSyncBlock.GetRawPtr()->m_mutex);
	return m_chainedTasksNext;
}

ITaskPtr ITask::GetChainedTaskPrev() const
{
	std::lock_guard<std::mutex> lock(m_pSyncBlock.GetRawPtr()->m_mutex);
	return m_chainedTaskPrev;
}

void ITask::ChainTasks(const ITaskPtr& nextTask)
{
	std::lock_guard<std::mutex> lock(m_pSyncBlock->m_mutex);
	// The new task is not published until this registration is complete.
	nextTask->m_chainedTaskPrev = m_self.TryLock();
	if (IsFinished())
	{
		SetContinuationArgs(*nextTask);
		return;
	}

	++nextTask->m_numBlockers;
	m_dependencies.Add(nextTask);
	m_chainedTasksNext.Add(nextTask);
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
	check(m_pScheduler);
	m_pScheduler->Run(res);
	return res;
}

void ITask::Complete()
{
	SAILOR_PROFILE_FUNCTION();

	check(!IsFinished());

	auto& syncBlock = *m_pSyncBlock;
	TVector<TWeakPtr<ITask>> dependencies;
	ITaskPtr previous;
	{
		std::unique_lock<std::mutex> lk(syncBlock.m_mutex);

		for (const auto& next : m_chainedTasksNext)
		{
			if (auto task = next.TryLock())
			{
				SetContinuationArgs(*task);
			}
		}

		previous = std::move(m_chainedTaskPrev);
		dependencies = std::move(m_dependencies);
		m_state |= StateMask::IsFinishedBit;
		syncBlock.m_bCompletionFlag = true;
	}

	syncBlock.m_onComplete.notify_all();
	for (const auto& dependency : dependencies)
	{
		if (auto task = dependency.TryLock())
		{
			if (--task->m_numBlockers == 0 && task->m_pScheduler)
			{
				task->m_pScheduler->NotifyTaskReady(*task);
			}
		}
	}
}

void ITask::Wait()
{
	SAILOR_PROFILE_FUNCTION();
	if (IsFinished())
	{
		return;
	}

	auto& syncBlock = *m_pSyncBlock;
	std::unique_lock<std::mutex> lk(syncBlock.m_mutex);

	if (!IsFinished())
	{
		syncBlock.m_onComplete.wait(lk, [&]() { return syncBlock.m_bCompletionFlag; });
	}
}
