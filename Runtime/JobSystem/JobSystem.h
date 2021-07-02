#pragma once
#include <cstdio>
#include <functional>
#include <mutex>
#include <thread>
#include "Sailor.h"
#include "Singleton.hpp"

namespace Sailor
{
	namespace JobSystem
	{
		class WorkerThread;
		class Scheduler;

		class IJob
		{
		public:

			template<class TResult>
			SAILOR_API void GetResult(TResult& outResult) const {}

			virtual SAILOR_API float GetProgress() { return 0.0f; }
			virtual SAILOR_API bool IsFinished() const { return false; }

			virtual void Complete();
			virtual void Execute() = 0;

			virtual SAILOR_API ~IJob() = default;

			void Wait(std::weak_ptr<IJob> job);
			void WaitAll(const std::vector<std::weak_ptr<IJob>>& jobs);

		protected:

			IJob(const std::string& name) : m_numBlockers(0), m_name(name) {}

			std::atomic<uint32_t> m_numBlockers;
			std::vector<IJob*> m_dependencies;
			std::string m_name;
			
			friend class JobSystem;
			friend class WorkerThread;
		};

		class Job : public IJob
		{
		public:

			virtual ~Job() = default;

			bool IsReadyToStart() const;
			virtual bool IsFinished() const override;
			virtual void Execute() override;

			Job(const std::string& name, const std::function<void()>& function);

		protected:

			std::function<void()> m_function;
			std::atomic_bool m_bIsFinished = false;
		};

		class WorkerThread
		{
		public:

			WorkerThread(Scheduler* scheduler, const std::string& threadName);
			virtual ~WorkerThread() = default;

			void ForcelyAssignJob(const std::shared_ptr<Job>& pJob);

			void Process();
			void Join() const;

		protected:

			Scheduler* m_scheduler;

			std::string m_threadName;
			std::unique_ptr<std::thread> m_pThread;

			std::atomic_bool m_bIsBusy;
			std::shared_ptr<Job> m_pJob;

			friend class IJob;
		};

		class Scheduler final : public Singleton<Scheduler>
		{
		public:

			static SAILOR_API void Initialize();

			virtual ~Scheduler() override;

			uint32_t SAILOR_API GetNumWorkerThreads() const;

			static SAILOR_API std::shared_ptr<Job> CreateJob(const std::string& name, const std::function<void()>& lambda);
			SAILOR_API void Run(const std::shared_ptr<Job>& pJob);

			bool TryFetchNextAvailiableJob(std::shared_ptr<Job>& pOutJob);

			void NotifyWorkerThread(bool bNotifyAllThreads = false);
						
		protected:						

			Scheduler();
			
			std::mutex m_queueMutex;
			std::condition_variable m_refresh;

			std::list<std::shared_ptr<Job>> m_pJobsQueue;
			std::atomic<uint32_t> m_bNumAvailiableJobs;
			
			std::vector<WorkerThread*> m_workerThreads;
			std::atomic_bool m_bIsTerminating;

			friend class IJob;
			friend class WorkerThread;
		};
	}
}