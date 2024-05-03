#include "Scheduler.h"
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
using namespace Sailor::Tasks;

WorkerThread::WorkerThread(
	std::string threadName,
	EThreadType threadType,
	std::condition_variable& refresh,
	std::mutex& mutex,
	TVector<ITaskPtr>& pTasksQueue) :
	m_threadName(std::move(threadName)),
	m_threadType(threadType),
	m_refresh(refresh),
	m_sharedQueueMutex(mutex),
	m_pSharedTaskQueue(pTasksQueue)
{
	m_pThread = TUniquePtr<std::thread>::Make(&WorkerThread::Process, this);
	HANDLE threadHandle = m_pThread->native_handle();
	m_threadId = ::GetThreadId(threadHandle);
}

void WorkerThread::Join()
{
	SAILOR_PROFILE_FUNCTION();
	m_pThread->join();
}

void WorkerThread::WaitIdle()
{
	SAILOR_PROFILE_FUNCTION();
	while (m_bIsBusy);
}

void WorkerThread::ForcelyPushTask(const ITaskPtr& pTask)
{
	SAILOR_PROFILE_FUNCTION();
	{
		const std::lock_guard<std::mutex> lock(m_queueMutex);
		m_pTaskQueue.Add(pTask);
	}

	if (!m_bIsBusy)
	{
		m_refresh.notify_all();
	}
}

bool WorkerThread::TryFetchTask(ITaskPtr& pOutTask)
{
	SAILOR_PROFILE_FUNCTION();

	const std::lock_guard<std::mutex> lock(m_queueMutex);
	if (m_pTaskQueue.Num() > 0)
	{
		pOutTask = m_pTaskQueue[m_pTaskQueue.Num() - 1];
		m_pTaskQueue.RemoveLast();
		return true;
	}
	return false;
}

void WorkerThread::ProcessTask(ITaskPtr& task)
{
	if (task)
	{
		SAILOR_PROFILE_BLOCK(task->GetName());
		m_bIsBusy = true;

		task->Execute();
		task.Clear();

		m_bIsBusy = false;
		SAILOR_PROFILE_END_BLOCK();
	}
}

void WorkerThread::SetExecFlag()
{
	std::unique_lock<std::mutex> lk(m_execMutex);
	m_bExecFlag++;
}

void WorkerThread::Process()
{
	Sailor::Utils::SetThreadName(m_threadName);

#if defined(BUILD_WITH_EASY_PROFILER)
	EASY_THREAD_SCOPE(m_threadName.c_str());
#endif

	m_threadId = GetCurrentThreadId();

	if (m_threadType == EThreadType::Render || m_threadType == EThreadType::RHI)
	{
		SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
	}

	Scheduler* scheduler = App::GetSubmodule<Tasks::Scheduler>();

	ITaskPtr pCurrentTask;
	while (!scheduler->m_bIsTerminating)
	{
		{
			std::unique_lock<std::mutex> lk(m_execMutex);
			m_refresh.wait(lk, [=, &pCurrentTask]()
				{
					const bool res = ((m_bExecFlag > 0) && (TryFetchTask(pCurrentTask) ||
						scheduler->TryFetchNextAvailiableTask(pCurrentTask, m_threadType))) ||
						(bool)scheduler->m_bIsTerminating;
					return res;
				});
			m_bExecFlag--;
		}

		ProcessTask(pCurrentTask);
	}
}

void Scheduler::Initialize()
{
	m_taskSyncPool.AddDefault(MaxTasksInPool);

	for (uint32_t i = 0; i < MaxTasksInPool; i++)
	{
		m_freeList.push((uint16_t)(MaxTasksInPool - i - 1));
	}

	m_mainThreadId = GetCurrentThreadId();
	m_threadTypes[m_mainThreadId] = EThreadType::Main;

	SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);

	const unsigned coresCount = std::thread::hardware_concurrency();
	const unsigned numRHIThreads = RHIThreadsNum;
	const unsigned numThreads = std::max(1u, coresCount - 2u - numRHIThreads);

	WorkerThread* newRenderingThread = new WorkerThread(
		"Render Thread",
		EThreadType::Render,
		m_refreshCondVar[(uint32_t)EThreadType::Render],
		m_queueMutex[(uint32_t)EThreadType::Render],
		m_pSharedTaskQueue[(uint32_t)EThreadType::Render]);

	m_renderingThreadId = newRenderingThread->GetThreadId();
	m_threadTypes[m_renderingThreadId] = EThreadType::Render;
	m_workerThreads.Emplace(newRenderingThread);

	for (uint32_t i = 0; i < numThreads; i++)
	{
		const std::string threadName = std::string("Worker Thread ") + std::to_string(i);
		WorkerThread* newThread = new WorkerThread(threadName, EThreadType::Worker,
			m_refreshCondVar[(uint32_t)EThreadType::Worker],
			m_queueMutex[(uint32_t)EThreadType::Worker],
			m_pSharedTaskQueue[(uint32_t)EThreadType::Worker]);

		m_threadTypes[newThread->GetThreadId()] = EThreadType::Worker;

		m_workerThreads.Emplace(newThread);
	}

	for (uint32_t i = 0; i < numRHIThreads; i++)
	{
		const std::string threadName = std::string("RHI Thread ") + std::to_string(i);
		WorkerThread* newThread = new WorkerThread(threadName, EThreadType::RHI,
			m_refreshCondVar[(uint32_t)EThreadType::RHI],
			m_queueMutex[(uint32_t)EThreadType::RHI],
			m_pSharedTaskQueue[(uint32_t)EThreadType::RHI]);

		m_threadTypes[newThread->GetThreadId()] = EThreadType::RHI;
		m_workerThreads.Emplace(newThread);
	}

	SAILOR_LOG("Initialize Tasks::Scheduler. Cores count: %d, Worker threads count: %zd", coresCount, m_workerThreads.Num());
}

Scheduler::~Scheduler()
{
	m_bIsTerminating = true;

	NotifyWorkerThread(EThreadType::Worker, true);
	NotifyWorkerThread(EThreadType::Render, true);
	NotifyWorkerThread(EThreadType::RHI, true);

	for (auto& worker : m_workerThreads)
	{
		worker->Join();
	}

	App::GetSubmodule<Tasks::Scheduler>()->ProcessTasksOnMainThread();

	for (auto worker : m_workerThreads)
	{
		delete worker;
	}

	m_workerThreads.Clear();
}

uint32_t Scheduler::GetNumWorkerThreads() const
{
	return static_cast<uint32_t>(m_workerThreads.Num());
}

void Scheduler::ProcessTasksOnMainThread()
{
	SAILOR_PROFILE_FUNCTION();
	ITaskPtr pCurrentTask;
	while (TryFetchNextAvailiableTask(pCurrentTask, EThreadType::Main))
	{
		if (pCurrentTask)
		{
			SAILOR_PROFILE_BLOCK(pCurrentTask->GetName());

			pCurrentTask->Execute();
			pCurrentTask.Clear();

			SAILOR_PROFILE_END_BLOCK();
		}
	}
}

void Scheduler::RunChainedTasks_Internal(const ITaskPtr& pTask, const ITaskPtr& pTaskToIgnore)
{
	for (auto& chainedTasksNext : pTask->GetChainedTasksNext())
	{
		ITaskPtr pCurrentChainedTask;
		if (pCurrentChainedTask = chainedTasksNext.TryLock())
		{
			if (pCurrentChainedTask->IsInQueue() || pCurrentChainedTask->IsStarted())
			{
				// No point to trace next
				break;
			}

			if (pCurrentChainedTask != pTaskToIgnore)
			{
				Run(pCurrentChainedTask, false);
				RunChainedTasks_Internal(pCurrentChainedTask, pTask);
			}
		}
	}

	ITaskPtr pCurrentChainedTask;
	if (pCurrentChainedTask = pTask->GetChainedTaskPrev())
	{
		if (pCurrentChainedTask->IsInQueue() || pCurrentChainedTask->IsStarted() || pCurrentChainedTask == pTaskToIgnore)
		{
			// No point to trace next
			return;
		}

		Run(pCurrentChainedTask, false);
		RunChainedTasks_Internal(pCurrentChainedTask, pTask);
	}
}

void Scheduler::RunChainedTasks(const ITaskPtr& pTask)
{
	RunChainedTasks_Internal(pTask, nullptr);
}

void Scheduler::Run(const ITaskPtr& pTask, bool bAutoRunChainedTasks)
{
	SAILOR_PROFILE_FUNCTION();

	check(!pTask->IsStarted() && !pTask->IsExecuting() && !pTask->IsFinished() && !pTask->IsInQueue());

	if (bAutoRunChainedTasks)
	{
		RunChainedTasks(pTask);
	}

	pTask.GetRawPtr()->OnEnqueue();

	{
		std::mutex* pOutQueueMutex;
		TVector<ITaskPtr>* pOutQueue;
		std::condition_variable* pOutCondVar;

		GetThreadSyncVarsByThreadType(pTask->GetThreadType(), pOutQueueMutex, pOutQueue, pOutCondVar);

		const std::lock_guard<std::mutex> lock(*pOutQueueMutex);
		pOutQueue->Add(pTask);
	}

	NotifyWorkerThread(pTask->GetThreadType());
}

void Scheduler::Run(const ITaskPtr& pTask, DWORD threadId, bool bAutoRunChainedTasks)
{
	SAILOR_PROFILE_FUNCTION();

	check(!pTask->IsStarted() && !pTask->IsExecuting() && !pTask->IsFinished() && !pTask->IsInQueue());

	if (bAutoRunChainedTasks)
	{
		RunChainedTasks(pTask);
	}

	pTask.GetRawPtr()->OnEnqueue();

	auto result = m_workerThreads.FindIf(
		[&](const auto& worker)
		{
			return worker->GetThreadId() == threadId;
		});

	if (result != -1)
	{
		m_workerThreads[result]->SetExecFlag();
		m_workerThreads[result]->ForcelyPushTask(pTask);
		return;
	}
	check(m_mainThreadId == threadId);
	// Add to Main thread if cannot find the thread in workers
	{
		std::mutex* pOutQueueMutex;
		TVector<ITaskPtr>* pOutQueue;
		std::condition_variable* pOutCondVar;

		GetThreadSyncVarsByThreadType(EThreadType::Main, pOutQueueMutex, pOutQueue, pOutCondVar);

		const std::lock_guard<std::mutex> lock(*pOutQueueMutex);
		pOutQueue->Add(pTask);
	}
}

void Scheduler::GetThreadSyncVarsByThreadType(
	EThreadType threadType,
	std::mutex*& pOutQueueMutex,
	TVector<ITaskPtr>*& pOutQueue,
	std::condition_variable*& pOutCondVar)
{
	SAILOR_PROFILE_FUNCTION();

	pOutQueueMutex = &m_queueMutex[(uint32_t)threadType];
	pOutQueue = &m_pSharedTaskQueue[(uint32_t)threadType];
	pOutCondVar = &m_refreshCondVar[(uint32_t)threadType];
}

bool Scheduler::TryFetchNextAvailiableTask(ITaskPtr& pOutTask, EThreadType threadType)
{
	SAILOR_PROFILE_FUNCTION();

	std::mutex* pOutQueueMutex;
	TVector<ITaskPtr>* pOutQueue;
	std::condition_variable* pOutCondVar;

	GetThreadSyncVarsByThreadType(threadType, pOutQueueMutex, pOutQueue, pOutCondVar);

	const std::lock_guard<std::mutex> lock(*pOutQueueMutex);

	if (!(*pOutQueue).IsEmpty())
	{
		const auto result = (*pOutQueue).FindIf(
			[&](const ITaskPtr& Task)
			{
				return Task->IsReadyToStart();
			});

		if (result != -1)
		{
			pOutTask = (*pOutQueue)[result];
			(*pOutQueue).RemoveAt(result);

			return true;
		}
	}

	return false;
}

void Scheduler::NotifyWorkerThread(EThreadType threadType, bool bNotifyAllThreads)
{
	SAILOR_PROFILE_FUNCTION();
	std::mutex* pOutQueueMutex;
	TVector<ITaskPtr>* pOutQueue;
	std::condition_variable* pOutCondVar;

	GetThreadSyncVarsByThreadType(threadType, pOutQueueMutex, pOutQueue, pOutCondVar);

	for (uint32_t i = 0; i < m_workerThreads.Num(); i++)
	{
		if (m_workerThreads[i]->GetThreadType() == threadType)
		{
			m_workerThreads[i]->SetExecFlag();
		}
	}

	if (bNotifyAllThreads)
	{
		pOutCondVar->notify_all();
	}
	else
	{
		pOutCondVar->notify_one();
	}
}

EThreadType Scheduler::GetCurrentThreadType() const
{
	return m_threadTypes[GetCurrentThreadId()];
}

uint32_t Scheduler::GetNumTasks(EThreadType thread) const
{
	return (uint32_t)m_pSharedTaskQueue[(uint32_t)thread].Num();
}

void Scheduler::WaitIdle(const TSet<EThreadType>& threads)
{
	SAILOR_PROFILE_FUNCTION();

	for (const auto& thread : threads)
	{
		if (GetNumTasks(thread) > 0)
		{
			if (thread == EThreadType::Main && IsMainThread())
			{
				ProcessTasksOnMainThread();
			}
			else
			{
				WaitIdle(thread);
			}
		}
	}
}

void Scheduler::WaitIdle(EThreadType type)
{
	SAILOR_PROFILE_FUNCTION();

	std::mutex* pOutQueueMutex;
	TVector<ITaskPtr>* pOutQueue;
	std::condition_variable* pOutCondVar;
	TVector<ITaskPtr> waitFor;

	GetThreadSyncVarsByThreadType(type, pOutQueueMutex, pOutQueue, pOutCondVar);

	{
		const std::unique_lock<std::mutex> lock(m_queueMutex[(uint8_t)type]);
		waitFor = *pOutQueue;
	}

	do
	{
		for (auto& wait : waitFor)
		{
			wait->Wait();
		}

		{
			const std::unique_lock<std::mutex> lock(m_queueMutex[(uint8_t)type]);
			waitFor = *pOutQueue;
		}

	} while (waitFor.Num() != 0);

	for (auto& worker : m_workerThreads)
	{
		if (worker->GetThreadType() == type)
		{
			worker->WaitIdle();
		}
	}
}

bool Scheduler::IsMainThread() const
{
	return m_mainThreadId == GetCurrentThreadId();
}

bool Scheduler::IsRendererThread() const
{
	return m_renderingThreadId == GetCurrentThreadId();
}

uint16_t Scheduler::AcquireTaskSyncBlock()
{
	uint16_t last = 0;
	if (m_freeList.try_pop(last))
	{
		m_taskSyncPool[last].m_bCompletionFlag = false;
		return last;
	}

	ensure(false, "Increase the pool size of task block primitives! Scheduler::MaxTasksInPool");

	return 0;
}

void Scheduler::ReleaseTaskSyncBlock(const ITask& task)
{
	m_freeList.push(task.m_taskSyncBlockHandle);
}