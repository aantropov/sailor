#include "JobSystem.h"
#include <windows.h>
#include <fcntl.h>
#include <conio.h>
#include <algorithm>
#include <mutex>
#include <set>
#include <string>
#include "Utils.h"

using namespace std;
using namespace Sailor;
using namespace Sailor::JobSystem;

void IJob::Join(const TWeakPtr<IJob>& job)
{
	if (job && job.Lock()->AddDependency(this))
	{
		++m_numBlockers;
	}
}

bool IJob::AddDependency(IJob* job)
{
	std::unique_lock<std::mutex> lk(m_mutex);

	if (IsFinished())
	{
		return false;
	}

	m_dependencies.emplace_back(job);
	return true;
}

void IJob::Join(const std::vector<TWeakPtr<IJob>>& jobs)
{
	for (const auto& job : jobs)
	{
		Join(job);
	}
}

void IJob::Complete()
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
		Scheduler::GetInstance()->NotifyWorkerThread(threadType.first, threadType.second > 1);
	}

	m_bIsFinished = true;
	m_onComplete.notify_all();
}

void IJob::Wait()
{
	SAILOR_PROFILE_FUNCTION();
	std::unique_lock<std::mutex> lk(m_mutex);

	if (!m_bIsFinished)
	{
		m_onComplete.wait(lk);
	}
}

Job::Job(const std::string& name, const std::function<void()>& function, EThreadType thread) : IJob(name, thread)
{
	m_function = function;
}

bool Job::IsReadyToStart() const
{
	return !m_bIsStarted && !m_bIsFinished && m_numBlockers == 0 && m_function != nullptr;
}

void Job::Execute()
{
	m_bIsStarted = true;
	m_function();
}

WorkerThread::WorkerThread(
	std::string threadName,
	EThreadType threadType,
	std::condition_variable& refresh,
	std::mutex& mutex,
	std::vector<TSharedPtr<Job>>& pJobsQueue) :
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

void WorkerThread::ForcelyPushJob(const TSharedPtr<Job>& pJob)
{
	SAILOR_PROFILE_FUNCTION();
	{
		const std::lock_guard<std::mutex> lock(m_queueMutex);
		m_pJobsQueue.push_back(pJob);
	}

	if (!m_bIsBusy)
	{
		m_refresh.notify_all();
	}
}

bool WorkerThread::TryFetchJob(TSharedPtr<Job>& pOutJob)
{
	SAILOR_PROFILE_FUNCTION();

	const std::lock_guard<std::mutex> lock(m_queueMutex);
	if (m_pJobsQueue.size() > 0)
	{
		pOutJob = m_pJobsQueue[m_pJobsQueue.size() - 1];
		m_pJobsQueue.pop_back();
		return true;
	}
	return false;
}

void WorkerThread::Process()
{
	Sailor::Utils::SetThreadName(m_threadName);
	EASY_THREAD_SCOPE(m_threadName.c_str());
	m_threadId = GetCurrentThreadId();

	Scheduler* scheduler = Scheduler::GetInstance();

	TSharedPtr<Job> pCurrentJob;

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
			pCurrentJob->Complete();
			pCurrentJob.Clear();

			scheduler->NotifyWorkerThread(m_threadType);

			SAILOR_PROFILE_END_BLOCK()
				m_bIsBusy = false;;
		}

		lk.unlock();
	}
}

void Scheduler::Initialize()
{
	m_pInstance = new Scheduler();

	m_pInstance->m_mainThreadId = GetCurrentThreadId();

	const unsigned coresCount = std::thread::hardware_concurrency();
	const unsigned numThreads = std::max(1u, coresCount - 2u);

	WorkerThread* newRenderingThread = new WorkerThread(
		"Rendering Thread",
		EThreadType::Rendering,
		m_pInstance->m_refreshCondVar[(uint32_t)EThreadType::Rendering],
		m_pInstance->m_queueMutex[(uint32_t)EThreadType::Rendering],
		m_pInstance->m_pCommonJobsQueue[(uint32_t)EThreadType::Rendering]);

	m_pInstance->m_workerThreads.emplace_back(newRenderingThread);

	for (uint32_t i = 0; i < numThreads; i++)
	{
		const std::string threadName = std::string("Worker Thread ") + std::to_string(i);
		WorkerThread* newThread = new WorkerThread(threadName, EThreadType::Worker,
			m_pInstance->m_refreshCondVar[(uint32_t)EThreadType::Worker],
			m_pInstance->m_queueMutex[(uint32_t)EThreadType::Worker],
			m_pInstance->m_pCommonJobsQueue[(uint32_t)EThreadType::Worker]);

		m_pInstance->m_workerThreads.emplace_back(newThread);
	}

	SAILOR_LOG("Initialize JobSystem. Cores count: %d, Threads count: %zd", coresCount, m_pInstance->m_workerThreads.size());
}

DWORD Scheduler::GetRendererThreadId() const
{
	return m_pInstance->m_workerThreads[0]->GetThreadId();
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
	m_workerThreads.clear();

	JobSystem::Scheduler::GetInstance()->ProcessJobsOnMainThread();
}

TSharedPtr<Job> Scheduler::CreateJob(const std::string& name, const std::function<void()>& lambda, EThreadType thread)
{
	return TSharedPtr<Job>::Make(name, lambda, thread);
}

uint32_t Scheduler::GetNumWorkerThreads() const
{
	return static_cast<uint32_t>(m_workerThreads.size());
}

void Scheduler::ProcessJobsOnMainThread()
{
	SAILOR_PROFILE_FUNCTION();
	TSharedPtr<Job> pCurrentJob;
	while (TryFetchNextAvailiableJob(pCurrentJob, EThreadType::Main))
	{
		if (pCurrentJob)
		{
			SAILOR_PROFILE_BLOCK(pCurrentJob->GetName());

			pCurrentJob->Execute();
			pCurrentJob->Complete();
			pCurrentJob.Clear();
		}
	}
}

void Scheduler::Run(const TSharedPtr<Job>& pJob)
{
	SAILOR_PROFILE_FUNCTION();

	{
		std::mutex* pOutMutex;
		std::vector<TSharedPtr<Job>>* pOutQueue;
		std::condition_variable* pOutCondVar;

		GetThreadSyncVarsByThreadType(pJob->GetThreadType(), pOutMutex, pOutQueue, pOutCondVar);

		const std::lock_guard<std::mutex> lock(*pOutMutex);
		pOutQueue->push_back(pJob);
	}

	NotifyWorkerThread(pJob->GetThreadType());
}

void Scheduler::Run(const TSharedPtr<Job>& pJob, DWORD threadId)
{
	SAILOR_PROFILE_FUNCTION();

	auto result = std::find_if(m_workerThreads.cbegin(), m_workerThreads.cend(),
		[&](const auto& worker)
		{
			return worker->GetThreadId() == threadId;
		});

	if (result != m_workerThreads.cend())
	{
		(*result)->ForcelyPushJob(pJob);
		return;
	}

	assert(m_mainThreadId == threadId);

	// Add to Main thread if cannot find the thread in workers
	{
		std::mutex* pOutMutex;
		std::vector<TSharedPtr<Job>>* pOutQueue;
		std::condition_variable* pOutCondVar;

		GetThreadSyncVarsByThreadType(EThreadType::Main, pOutMutex, pOutQueue, pOutCondVar);

		const std::lock_guard<std::mutex> lock(*pOutMutex);
		pOutQueue->push_back(pJob);
	}
}

void Scheduler::GetThreadSyncVarsByThreadType(
	EThreadType threadType,
	std::mutex*& pOutMutex,
	std::vector<TSharedPtr<Job>>*& pOutQueue,
	std::condition_variable*& pOutCondVar)
{
	SAILOR_PROFILE_FUNCTION();

	pOutMutex = &m_queueMutex[(uint32_t)threadType];
	pOutQueue = &m_pCommonJobsQueue[(uint32_t)threadType];
	pOutCondVar = &m_refreshCondVar[(uint32_t)threadType];
}

bool Scheduler::TryFetchNextAvailiableJob(TSharedPtr<Job>& pOutJob, EThreadType threadType)
{
	SAILOR_PROFILE_FUNCTION();

	std::mutex* pOutMutex;
	std::vector<TSharedPtr<Job>>* pOutQueue;
	std::condition_variable* pOutCondVar;

	GetThreadSyncVarsByThreadType(threadType, pOutMutex, pOutQueue, pOutCondVar);

	const std::lock_guard<std::mutex> lock(*pOutMutex);

	if (!(*pOutQueue).empty())
	{
		const auto result = std::find_if((*pOutQueue).cbegin(), (*pOutQueue).cend(),
			[&](const TSharedPtr<Job>& job)
			{
				return job->IsReadyToStart();
			});

		if (result != (*pOutQueue).cend())
		{
			pOutJob = *result;
			(*pOutQueue).erase(result);

			return true;
		}
	}

	return false;
}

void Scheduler::NotifyWorkerThread(EThreadType threadType, bool bNotifyAllThreads)
{
	SAILOR_PROFILE_FUNCTION();

	std::mutex* pOutMutex;
	std::vector<TSharedPtr<Job>>* pOutQueue;
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
	return (uint32_t)m_pCommonJobsQueue[(uint32_t)EThreadType::Rendering].size();
}

SAILOR_API void Scheduler::StopAll(EThreadType type)
{
	std::mutex* pOutMutex;
	std::vector<TSharedPtr<Job>>* pOutQueue;
	std::condition_variable* pOutCondVar;
	std::vector<TSharedPtr<Job>> waitFor;

	GetThreadSyncVarsByThreadType(type, pOutMutex, pOutQueue, pOutCondVar);
	{
		const std::unique_lock<std::mutex> lock(*pOutMutex);
		pOutQueue->clear();
	}
}

void Scheduler::WaitIdle(EThreadType type)
{
	SAILOR_PROFILE_FUNCTION();

	std::mutex* pOutMutex;
	std::vector<TSharedPtr<Job>>* pOutQueue;
	std::condition_variable* pOutCondVar;
	std::vector<TSharedPtr<Job>> waitFor;

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

	} while (waitFor.size() != 0);
}
