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
	job.Lock()->m_dependencies.emplace_back(this);
	++m_numBlockers;
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
	SAILOR_PROFILE_FUNCTION();

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

	m_jobFinished.notify_all();
}

void IJob::Wait()
{
	std::unique_lock<std::mutex> lk(m_jobIsExecuting);
	m_jobFinished.wait(lk);
}

Job::Job(const std::string& name, const std::function<void()>& function, EThreadType thread) : IJob(name, thread)
{
	m_function = function;
}

bool Job::IsReadyToStart() const
{
	return !m_bIsFinished && m_numBlockers == 0 && m_function != nullptr;
}

bool Job::IsFinished() const
{
	return m_bIsFinished;
}

void Job::Execute()
{
	SAILOR_PROFILE_FUNCTION();
	m_function();
}

WorkerThread::WorkerThread(
	std::string threadName,
	EThreadType threadType,
	std::condition_variable& refresh,
	std::mutex& mutex,
	std::list<TSharedPtr<Job>>& pJobsQueue) :
	m_threadName(std::move(threadName)),
	m_threadType(threadType),
	m_refresh(refresh),
	m_mutex(mutex),
	m_pJobsQueue(pJobsQueue)
{
	m_pThread = std::make_unique<std::thread>(&WorkerThread::Process, this);
}

void WorkerThread::Join() const
{
	SAILOR_PROFILE_FUNCTION();
	m_pThread->join();
}

void WorkerThread::Process()
{
	Sailor::Utils::SetThreadName(m_threadName);
	DWORD randomColorHex = Sailor::Utils::GetRandomColorHex();

	Scheduler* scheduler = Scheduler::GetInstance();

	std::mutex threadExecutionMutex;
	while (!scheduler->m_bIsTerminating)
	{
		std::unique_lock<std::mutex> lk(threadExecutionMutex);
		m_refresh.wait(lk, [this, scheduler] { return  scheduler->TryFetchNextAvailiableJob(m_pJob, m_threadType) || (bool)scheduler->m_bIsTerminating; });
				
		if (m_pJob)
		{
			SAILOR_PROFILE_BLOCK(m_pJob->GetName(), randomColorHex);

			m_pJob->Execute();
			m_pJob->Complete();
			m_pJob = nullptr;

			scheduler->NotifyWorkerThread(m_threadType);

			SAILOR_PROFILE_END_BLOCK();
		}

		lk.unlock();
	}
}

void Scheduler::Initialize()
{
	SAILOR_PROFILE_FUNCTION();

	m_pInstance = new Scheduler();

	const unsigned coresCount = std::thread::hardware_concurrency();
	const unsigned numThreads = std::max(1u, coresCount - 2u);

	WorkerThread* newRenderingThread = new WorkerThread(
		"Rendering Thread",
		EThreadType::Rendering,
		m_pInstance->m_refreshCondVar[(uint32_t)EThreadType::Rendering],
		m_pInstance->m_queueMutex[(uint32_t)EThreadType::Rendering],
		m_pInstance->m_pJobsQueue[(uint32_t)EThreadType::Rendering]);

	m_pInstance->m_workerThreads.emplace_back(newRenderingThread);

	WorkerThread* newFilesystemThread = new WorkerThread(
		"Filesystem Thread",
		EThreadType::FileSystem,
		m_pInstance->m_refreshCondVar[(uint32_t)EThreadType::FileSystem],
		m_pInstance->m_queueMutex[(uint32_t)EThreadType::FileSystem],
		m_pInstance->m_pJobsQueue[(uint32_t)EThreadType::FileSystem]);

	m_pInstance->m_workerThreads.emplace_back(newFilesystemThread);

	for (uint32_t i = 0; i < numThreads; i++)
	{
		const std::string threadName = std::string("Worker Thread ") + std::to_string(i);
		WorkerThread* newThread = new WorkerThread(threadName, EThreadType::Worker,
			m_pInstance->m_refreshCondVar[(uint32_t)EThreadType::Worker],
			m_pInstance->m_queueMutex[(uint32_t)EThreadType::Worker],
			m_pInstance->m_pJobsQueue[(uint32_t)EThreadType::Worker]);

		m_pInstance->m_workerThreads.emplace_back(newThread);
	}

	SAILOR_LOG("Initialize JobSystem. Cores count: %d, Threads count: %zd", coresCount, m_pInstance->m_workerThreads.size());
}

Scheduler::~Scheduler()
{
	m_bIsTerminating = true;

	NotifyWorkerThread(EThreadType::Worker, true);
	NotifyWorkerThread(EThreadType::FileSystem, true);
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
}

TSharedPtr<Job> Scheduler::CreateJob(const std::string& name, const std::function<void()>& lambda, EThreadType thread)
{
	return TSharedPtr<Job>::Make(name, lambda, thread);
}

uint32_t Scheduler::GetNumWorkerThreads() const
{
	return static_cast<uint32_t>(m_workerThreads.size());
}

void Scheduler::Run(const TSharedPtr<Job>& pJob)
{
	SAILOR_PROFILE_FUNCTION();

	{
		std::mutex* pOutMutex;
		std::list<TSharedPtr<Job>>* pOutQueue;
		std::condition_variable* pOutCondVar;

		GetThreadSyncVarsByThreadType(pJob->GetThreadType(), pOutMutex, pOutQueue, pOutCondVar);

		const std::lock_guard<std::mutex> lock(*pOutMutex);
		pOutQueue->push_back(pJob);
	}

	NotifyWorkerThread(pJob->GetThreadType());
}

void Scheduler::GetThreadSyncVarsByThreadType(
	EThreadType threadType,
	std::mutex*& pOutMutex,
	std::list<TSharedPtr<Job>>*& pOutQueue,
	std::condition_variable*& pOutCondVar)
{
	SAILOR_PROFILE_FUNCTION();

	pOutMutex = &m_queueMutex[(uint32_t)threadType];
	pOutQueue = &m_pJobsQueue[(uint32_t)threadType];
	pOutCondVar = &m_refreshCondVar[(uint32_t)threadType];
}

bool Scheduler::TryFetchNextAvailiableJob(TSharedPtr<Job>& pOutJob, EThreadType threadType)
{
	SAILOR_PROFILE_FUNCTION();

	std::mutex* pOutMutex;
	std::list<TSharedPtr<Job>>* pOutQueue;
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
	std::list<TSharedPtr<Job>>* pOutQueue;
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
