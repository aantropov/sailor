#include "JobSystem.h"
#include <windows.h>
#include <fcntl.h>
#include <conio.h>
#include <mutex>
#include <set>
#include <string>
#include "Utils.h"

using namespace Sailor;
using namespace Sailor::JobSystem;

void IJob::Wait(const std::weak_ptr<IJob>& job)
{
	job.lock()->m_dependencies.emplace_back(this);
	++m_numBlockers;
}

void IJob::WaitAll(const std::vector<std::weak_ptr<IJob>>& jobs)
{
	for (const auto& job : jobs)
	{
		Wait(job);
	}
}

void IJob::Complete()
{
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
	m_function();
}

WorkerThread::WorkerThread(
	std::string threadName,
	EThreadType threadType,
	std::condition_variable& refresh,
	std::mutex& mutex,
	std::list<std::shared_ptr<Job>>& pJobsQueue) :
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
	m_pThread->join();
}

void WorkerThread::Process()
{
	Scheduler* scheduler = Scheduler::GetInstance();

	std::mutex threadExecutionMutex;
	while (!scheduler->m_bIsTerminating)
	{
		std::unique_lock<std::mutex> lk(threadExecutionMutex);
		m_refresh.wait(lk, [this, scheduler] { return  scheduler->TryFetchNextAvailiableJob(m_pJob, m_threadType) || (bool)scheduler->m_bIsTerminating; });

		if (m_pJob)
		{
			m_pJob->Execute();
			m_pJob->Complete();
			m_pJob = nullptr;

			scheduler->NotifyWorkerThread(m_threadType);
		}

		lk.unlock();
	}
}

void Scheduler::Initialize()
{
	m_pInstance = new Scheduler();

	const unsigned coresCount = std::thread::hardware_concurrency();
	const unsigned numThreads = max(1, coresCount - 2);

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

std::shared_ptr<Job> Scheduler::CreateJob(const std::string& name, const std::function<void()>& lambda, EThreadType thread)
{
	return std::make_shared<Job>(name, lambda, thread);
}

uint32_t Scheduler::GetNumWorkerThreads() const
{
	return static_cast<uint32_t>(m_workerThreads.size());
}

void Scheduler::Run(const std::shared_ptr<Job>& pJob)
{
	{
		std::mutex* pOutMutex;
		std::list<std::shared_ptr<Job>>* pOutQueue;
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
	std::list<std::shared_ptr<Job>>*& pOutQueue,
	std::condition_variable*& pOutCondVar)
{

	pOutMutex = &m_queueMutex[(uint32_t)threadType];
	pOutQueue = &m_pJobsQueue[(uint32_t)threadType];
	pOutCondVar = &m_refreshCondVar[(uint32_t)threadType];
}

bool Scheduler::TryFetchNextAvailiableJob(std::shared_ptr<Job>& pOutJob, EThreadType threadType)
{
	std::mutex* pOutMutex;
	std::list<std::shared_ptr<Job>>* pOutQueue;
	std::condition_variable* pOutCondVar;

	GetThreadSyncVarsByThreadType(threadType, pOutMutex, pOutQueue, pOutCondVar);

	const std::lock_guard<std::mutex> lock(*pOutMutex);

	if (!(*pOutQueue).empty())
	{
		const auto result = std::find_if((*pOutQueue).cbegin(), (*pOutQueue).cend(),
			[&](const std::shared_ptr<Job>& job)
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
	std::mutex* pOutMutex;
	std::list<std::shared_ptr<Job>>* pOutQueue;
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
