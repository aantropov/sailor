#pragma once
#include <cstdio>
#include <functional>
#include <mutex>
#include <atomic>
#include <thread>
#include "Sailor.h"
#include "Core/Singleton.hpp"

namespace Sailor
{
	namespace JobSystem
	{
		class WorkerThread;
		class Scheduler;

		enum class EThreadType : uint8_t
		{
			Rendering = 0,
			Worker = 1,
			FileSystem = 2
		};

		class IJob
		{
		public:

			virtual SAILOR_API float GetProgress() { return 0.0f; }
			virtual SAILOR_API bool IsFinished() const { return false; }

			virtual SAILOR_API void Complete();
			virtual SAILOR_API void Execute() = 0;

			virtual SAILOR_API ~IJob() = default;

			SAILOR_API const std::string& GetName() const { return m_name; }

			SAILOR_API void Wait(const std::weak_ptr<IJob>& job);
			SAILOR_API void WaitAll(const std::vector<std::weak_ptr<IJob>>& jobs);

			EThreadType GetThreadType() const { return m_threadType; }

		protected:

			SAILOR_API IJob(const std::string& name, EThreadType thread) : m_numBlockers(0), m_name(name), m_threadType(thread) {}

			std::atomic_bool m_bIsFinished = false;
			std::atomic<uint32_t> m_numBlockers;
			std::vector<IJob*> m_dependencies;
			std::string m_name;

			EThreadType m_threadType;
		};

		class Job : public IJob
		{
		public:

			virtual SAILOR_API ~Job() = default;

			SAILOR_API bool IsReadyToStart() const;
			virtual SAILOR_API bool IsFinished() const override;
			virtual SAILOR_API void Execute() override;

			SAILOR_API Job(const std::string& name, const std::function<void()>& function, EThreadType thread);

		protected:

			std::function<void()> m_function;
		};

		class WorkerThread
		{
		public:

			SAILOR_API WorkerThread(
				std::string threadName,
				EThreadType threadType,
				std::condition_variable& refresh,
				std::mutex& mutex,
				std::list<std::shared_ptr<Job>>& pJobsQueue);


			virtual SAILOR_API ~WorkerThread() = default;

			SAILOR_API WorkerThread(WorkerThread&& move) = delete;
			SAILOR_API WorkerThread(WorkerThread& copy) = delete;
			SAILOR_API WorkerThread& operator =(WorkerThread& rhs) = delete;

			SAILOR_API void ForcelyAssignJob(const std::shared_ptr<Job>& pJob);

			SAILOR_API void Process();
			SAILOR_API void Join() const;

		protected:

			std::string m_threadName;
			std::unique_ptr<std::thread> m_pThread;

			std::atomic_bool m_bIsBusy;
			std::shared_ptr<Job> m_pJob;

			EThreadType m_threadType;

			// Assigned from scheduler
			std::condition_variable& m_refresh;
			std::mutex& m_mutex;
			std::list<std::shared_ptr<Job>>& m_pJobsQueue;
		};

		class Scheduler final : public Singleton<Scheduler>
		{
		public:

			static SAILOR_API void Initialize();

			virtual SAILOR_API ~Scheduler() override;

			uint32_t SAILOR_API GetNumWorkerThreads() const;

			static SAILOR_API std::shared_ptr<Job> CreateJob(const std::string& name, const std::function<void()>& lambda, EThreadType thread = EThreadType::Worker);
			SAILOR_API void Run(const std::shared_ptr<Job>& pJob);

			SAILOR_API bool TryFetchNextAvailiableJob(std::shared_ptr<Job>& pOutJob, EThreadType threadType);

			SAILOR_API void NotifyWorkerThread(EThreadType threadType, bool bNotifyAllThreads = false);

		protected:

			SAILOR_API void GetThreadSyncVarsByThreadType(
				EThreadType threadType,
				std::mutex*& pOutMutex,
				std::list<std::shared_ptr<Job>>*& pOutQueue,
				std::condition_variable*& pOutCondVar);

			SAILOR_API Scheduler() = default;

			std::mutex m_queueMutex[3];
			std::condition_variable m_refreshCondVar[3];
			std::list<std::shared_ptr<Job>> m_pJobsQueue[3];

			std::atomic<uint32_t> m_numBusyThreads;
			std::vector<WorkerThread*> m_workerThreads;
			std::atomic_bool m_bIsTerminating;

			friend class WorkerThread;
		};
	}
}