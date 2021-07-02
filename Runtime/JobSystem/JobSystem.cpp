#include "JobSystem.h"
#include <windows.h>
#include <fcntl.h>
#include <conio.h>
#include <mutex>
#include <string>
#include "Utils.h"

using namespace Sailor;
using namespace Sailor::JobSystem;

void IJob::Wait(std::weak_ptr<IJob> job)
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
	uint32_t numUnlockedJobs = 0;
	for (auto& job : m_dependencies)
	{
		if (--job->m_numBlockers == 0)
		{
			numUnlockedJobs++;
		}
	}

	m_dependencies.clear();

	if (numUnlockedJobs > 0)
	{
		Scheduler::GetInstance()->NotifyWorkerThread(numUnlockedJobs > 1);
	}
}

Job::Job(const std::string& name, const std::function<void()>& function) : IJob(name)
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

WorkerThread::WorkerThread(Scheduler* scheduler, const std::string& threadName)
{
	m_threadName = threadName;
	m_scheduler = scheduler;
	m_pThread = std::make_unique<std::thread>(&WorkerThread::Process, this);
}

void WorkerThread::Join() const
{
	m_pThread->join();
}

void WorkerThread::Process()
{
	std::mutex threadExecutionMutex;
	while (!m_scheduler->m_bIsTerminating)
	{
		std::unique_lock<std::mutex> lk(threadExecutionMutex);
		m_scheduler->m_refresh.wait(lk, [this] { return  m_scheduler->TryFetchNextAvailiableJob(m_pJob) || (bool)m_scheduler->m_bIsTerminating; });
		
		if (m_pJob)
		{
			m_scheduler->NotifyWorkerThread();

			m_pJob->Execute();
			m_pJob->Complete();
			m_pJob = nullptr;
		}
		
		lk.unlock();
	}
}

void Scheduler::Initialize()
{
	m_pInstance = new Scheduler();
}

Scheduler::Scheduler()
{
	const unsigned coresCount = std::thread::hardware_concurrency();
	const unsigned numThreads = max(1, coresCount - 2);

	m_workerThreads.emplace_back(new WorkerThread(this, "Rendering Thread"));

	for (uint32_t i = 0; i < numThreads; i++)
	{
		std::string threadName = std::string("Worker Thread ") + std::to_string(i);
		m_workerThreads.emplace_back(new WorkerThread(this, threadName));
	}

	SAILOR_LOG("Initialize JobSystem. Cores count: %zd, Threads count: %zd", coresCount, m_workerThreads.size());
}

Scheduler::~Scheduler()
{
	m_bIsTerminating = true;
	NotifyWorkerThread(true);

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

std::shared_ptr<Job> Scheduler::CreateJob(const std::string& name, const std::function<void()>& lambda)
{
	return std::make_shared<Job>(name, lambda);
}

uint32_t Scheduler::GetNumWorkerThreads() const
{
	return static_cast<uint32_t>(m_workerThreads.size());
}

void Scheduler::Run(const std::shared_ptr<Job>& pJob)
{
	{
		const std::lock_guard<std::mutex> lock(m_queueMutex);
		m_pJobsQueue.push_back(pJob);
	}

	NotifyWorkerThread();
}

bool Scheduler::TryFetchNextAvailiableJob(std::shared_ptr<Job>& pOutJob)
{
	const std::lock_guard<std::mutex> lock(m_queueMutex);

	if (!m_pJobsQueue.empty())
	{
		auto result = std::find_if(m_pJobsQueue.cbegin(), m_pJobsQueue.cend(), [&](std::shared_ptr<Job> job) { return job->IsReadyToStart(); });
		if (result != m_pJobsQueue.cend())
		{
			pOutJob = *result;
			m_pJobsQueue.erase(result);

			return true;
		}
	}

	return false;
}

void Scheduler::NotifyWorkerThread(bool bNotifyAllThreads)
{
	if (bNotifyAllThreads)
	{
		m_refresh.notify_all();
	}
	else
	{
		m_refresh.notify_one();
	}
}
