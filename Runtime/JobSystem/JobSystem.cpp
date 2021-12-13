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

WorkerThread::WorkerThread(
	std::string threadName,
	EThreadType threadType,
	std::condition_variable& refresh,
	std::mutex& mutex,
	TVector<ITaskPtr>& pJobsQueue) :
	m_threadName(std::move(threadName)),
	m_threadType(threadType),
	m_refresh(refresh),
	m_commonQueueMutex(mutex),
	m_pCommonJobsQueue(pJobsQueue)
{
	m_pThread = TUniquePtr<std::thread>::Make(&WorkerThread::Process, this);
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

void WorkerThread::ForcelyPushJob(const ITaskPtr& pJob)
{
	SAILOR_PROFILE_FUNCTION();
	{
		const std::lock_guard<std::mutex> lock(m_queueMutex);
		m_pJobsQueue.Add(pJob);
	}

	if (!m_bIsBusy)
	{
		m_refresh.notify_all();
	}
}

bool WorkerThread::TryFetchJob(ITaskPtr& pOutJob)
{
	SAILOR_PROFILE_FUNCTION();

	const std::lock_guard<std::mutex> lock(m_queueMutex);
	if (m_pJobsQueue.Num() > 0)
	{
		pOutJob = m_pJobsQueue[m_pJobsQueue.Num() - 1];
		m_pJobsQueue.RemoveLast();
		return true;
	}
	return false;
}

void WorkerThread::Process()
{
	Sailor::Utils::SetThreadName(m_threadName);
	EASY_THREAD_SCOPE(m_threadName.c_str());
	m_threadId = GetCurrentThreadId();

	Scheduler* scheduler = App::GetSubmodule<JobSystem::Scheduler>();

	ITaskPtr pCurrentJob;

	std::mutex threadExecutionMutex;
	while (!scheduler->m_bIsTerminating)
	{
		std::unique_lock<std::mutex> lk(threadExecutionMutex);
		m_refresh.wait(lk, [this, &pCurrentJob, scheduler]
			{
				return TryFetchJob(pCurrentJob) ||
					scheduler->TryFetchNextAvailiableJob(pCurrentJob, m_threadType) ||
					(bool)scheduler->m_bIsTerminating;
			});

		if (pCurrentJob)
		{
			m_bIsBusy = true;
			SAILOR_PROFILE_BLOCK(pCurrentJob->GetName());

			pCurrentJob->Execute();
			pCurrentJob.Clear();

			scheduler->NotifyWorkerThread(m_threadType);

			SAILOR_PROFILE_END_BLOCK()
				m_bIsBusy = false;
		}

		lk.unlock();
	}
}

void Scheduler::Initialize()
{
	m_mainThreadId = GetCurrentThreadId();

	const unsigned coresCount = std::thread::hardware_concurrency();
	const unsigned numThreads = std::max(1u, coresCount - 2u);

	WorkerThread* newRenderingThread = new WorkerThread(
		"Rendering Thread",
		EThreadType::Rendering,
		m_refreshCondVar[(uint32_t)EThreadType::Rendering],
		m_queueMutex[(uint32_t)EThreadType::Rendering],
		m_pCommonJobsQueue[(uint32_t)EThreadType::Rendering]);

	m_workerThreads.Emplace(newRenderingThread);

	for (uint32_t i = 0; i < numThreads; i++)
	{
		const std::string threadName = std::string("Worker Thread ") + std::to_string(i);
		WorkerThread* newThread = new WorkerThread(threadName, EThreadType::Worker,
			m_refreshCondVar[(uint32_t)EThreadType::Worker],
			m_queueMutex[(uint32_t)EThreadType::Worker],
			m_pCommonJobsQueue[(uint32_t)EThreadType::Worker]);

		m_workerThreads.Emplace(newThread);
	}

	SAILOR_LOG("Initialize JobSystem. Cores count: %d, Threads count: %zd", coresCount, m_workerThreads.size());
}

DWORD Scheduler::GetRendererThreadId() const
{
	return App::GetSubmodule<Scheduler>()->m_workerThreads[0]->GetThreadId();
}

Scheduler::~Scheduler()
{
	m_bIsTerminating = true;

	NotifyWorkerThread(EThreadType::Worker, true);
	NotifyWorkerThread(EThreadType::Rendering, true);

	for (auto& worker : m_workerThreads)
	{
		worker->Join();
	}

	for (auto worker : m_workerThreads)
	{
		delete worker;
	}
	m_workerThreads.Clear();

	App::GetSubmodule<JobSystem::Scheduler>()->ProcessJobsOnMainThread();
}

uint32_t Scheduler::GetNumWorkerThreads() const
{
	return static_cast<uint32_t>(m_workerThreads.Num());
}

void Scheduler::ProcessJobsOnMainThread()
{
	SAILOR_PROFILE_FUNCTION();
	ITaskPtr pCurrentJob;
	while (TryFetchNextAvailiableJob(pCurrentJob, EThreadType::Main))
	{
		if (pCurrentJob)
		{
			SAILOR_PROFILE_BLOCK(pCurrentJob->GetName());

			pCurrentJob->Execute();
			pCurrentJob.Clear();
		}
	}
}

void Scheduler::RunChainedTasks(const ITaskPtr& pJob)
{
	ITaskPtr pCurrentChainedTask;
	while (pCurrentChainedTask = pJob->GetChainedTaskNext().TryLock())
	{
		if (pCurrentChainedTask->IsInQueue() || pCurrentChainedTask->IsStarted())
		{
			// No point to trace next
			break;
		}

		Run(pCurrentChainedTask, false);
	}

	while (pCurrentChainedTask = pJob->GetChainedTaskPrev().TryLock())
	{
		if (pCurrentChainedTask->IsInQueue() || pCurrentChainedTask->IsStarted())
		{
			// No point to trace next
			break;
		}

		Run(pCurrentChainedTask, false);
	}
}

void Scheduler::Run(const ITaskPtr& pJob, bool bAutoRunChainedTasks)
{
	SAILOR_PROFILE_FUNCTION();

	assert(!pJob->IsStarted() && !pJob->IsExecuting() && !pJob->IsFinished() && !pJob->IsInQueue());

	if (bAutoRunChainedTasks)
	{
		RunChainedTasks(pJob);
	}

	pJob.GetRawPtr()->OnEnqueue();

	{
		std::mutex* pOutMutex;
		TVector<ITaskPtr>* pOutQueue;
		std::condition_variable* pOutCondVar;

		GetThreadSyncVarsByThreadType(pJob->GetThreadType(), pOutMutex, pOutQueue, pOutCondVar);

		const std::lock_guard<std::mutex> lock(*pOutMutex);
		pOutQueue->Add(pJob);
	}

	NotifyWorkerThread(pJob->GetThreadType());
}

void Scheduler::Run(const ITaskPtr& pJob, DWORD threadId, bool bAutoRunChainedTasks)
{
	SAILOR_PROFILE_FUNCTION();

	assert(!pJob->IsStarted() && !pJob->IsExecuting() && !pJob->IsFinished() && !pJob->IsInQueue());

	if (bAutoRunChainedTasks)
	{
		RunChainedTasks(pJob);
	}

	pJob.GetRawPtr()->OnEnqueue();

	auto result = m_workerThreads.FindIf(
		[&](const auto& worker)
		{
			return worker->GetThreadId() == threadId;
		});

	if (result != -1)
	{
		m_workerThreads[result]->ForcelyPushJob(pJob);
		return;
	}

	assert(m_mainThreadId == threadId);

	// Add to Main thread if cannot find the thread in workers
	{
		std::mutex* pOutMutex;
		TVector<ITaskPtr>* pOutQueue;
		std::condition_variable* pOutCondVar;

		GetThreadSyncVarsByThreadType(EThreadType::Main, pOutMutex, pOutQueue, pOutCondVar);

		const std::lock_guard<std::mutex> lock(*pOutMutex);
		pOutQueue->Add(pJob);
	}
}

void Scheduler::GetThreadSyncVarsByThreadType(
	EThreadType threadType,
	std::mutex*& pOutMutex,
	TVector<ITaskPtr>*& pOutQueue,
	std::condition_variable*& pOutCondVar)
{
	SAILOR_PROFILE_FUNCTION();

	pOutMutex = &m_queueMutex[(uint32_t)threadType];
	pOutQueue = &m_pCommonJobsQueue[(uint32_t)threadType];
	pOutCondVar = &m_refreshCondVar[(uint32_t)threadType];
}

bool Scheduler::TryFetchNextAvailiableJob(ITaskPtr& pOutJob, EThreadType threadType)
{
	SAILOR_PROFILE_FUNCTION();

	std::mutex* pOutMutex;
	TVector<ITaskPtr>* pOutQueue;
	std::condition_variable* pOutCondVar;

	GetThreadSyncVarsByThreadType(threadType, pOutMutex, pOutQueue, pOutCondVar);

	const std::lock_guard<std::mutex> lock(*pOutMutex);

	if (!(*pOutQueue).IsEmpty())
	{
		const auto result = (*pOutQueue).FindIf(
			[&](const ITaskPtr& job)
			{
				return job->IsReadyToStart();
			});

		if (result != -1)
		{
			pOutJob = (*pOutQueue)[result];
			(*pOutQueue).RemoveAt(result);

			return true;
		}
	}

	return false;
}

void Scheduler::NotifyWorkerThread(EThreadType threadType, bool bNotifyAllThreads)
{
	SAILOR_PROFILE_FUNCTION();

	std::mutex* pOutMutex;
	TVector<ITaskPtr>* pOutQueue;
	std::condition_variable* pOutCondVar;

	GetThreadSyncVarsByThreadType(threadType, pOutMutex, pOutQueue, pOutCondVar);

	if (bNotifyAllThreads)
	{
		pOutCondVar->notify_all();
	}
	else
	{
		pOutCondVar->notify_one();
	}
}

uint32_t Scheduler::GetNumRenderingJobs() const
{
	return (uint32_t)m_pCommonJobsQueue[(uint32_t)EThreadType::Rendering].Num();
}

void Scheduler::WaitIdle(EThreadType type)
{
	SAILOR_PROFILE_FUNCTION();

	std::mutex* pOutMutex;
	TVector<ITaskPtr>* pOutQueue;
	std::condition_variable* pOutCondVar;
	TVector<ITaskPtr> waitFor;

	GetThreadSyncVarsByThreadType(type, pOutMutex, pOutQueue, pOutCondVar);

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
