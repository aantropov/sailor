#pragma once
#include <cstdio>
#include <functional>
#include <mutex>
#include <atomic>
#include <thread>
#include "Sailor.h"
#include "Core/Submodule.h"
#include "Memory/UniquePtr.hpp"

#define SAILOR_ENQUEUE_JOB(Name, Lambda) Sailor::App::GetSubmodule<JobSystem::Scheduler>()->Run(Sailor::JobSystem::Scheduler::CreateTask(Name, Lambda))
#define SAILOR_ENQUEUE_JOB_RENDER_THREAD(Name, Lambda) Sailor::App::GetSubmodule<JobSystem::Scheduler>()->Run(Sailor::JobSystem::Scheduler::CreateTask(Name, Lambda, Sailor::JobSystem::EThreadType::Rendering))

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
			Main = 2
		};

		class ITask
		{
		public:

			virtual SAILOR_API float GetProgress() { return 0.0f; }
			virtual SAILOR_API bool IsFinished() const { return m_bIsFinished; }
			virtual SAILOR_API bool IsExecuting() const { return m_bIsStarted && !m_bIsFinished; }
			virtual SAILOR_API bool IsStarted() const { return m_bIsStarted; }

			virtual SAILOR_API void Execute() = 0;

			virtual SAILOR_API ~ITask() = default;

			SAILOR_API const std::string& GetName() const { return m_name; }

			SAILOR_API bool AddDependency(ITask* job);

			// Wait other threads completion before start
			SAILOR_API void Join(const TWeakPtr<ITask>& job);
			SAILOR_API void Join(const std::vector<TWeakPtr<ITask>>& jobs);

			// Lock this thread while job is executing
			SAILOR_API void Wait();

			SAILOR_API EThreadType GetThreadType() const { return m_threadType; }

		protected:

			virtual SAILOR_API void Complete();

			SAILOR_API ITask(const std::string& name, EThreadType thread) : m_numBlockers(0), m_name(name), m_threadType(thread)
			{
			}

			std::atomic<bool> m_bIsFinished = false;
			std::atomic<bool> m_bIsStarted = false;
			std::atomic<bool> m_bIsInQueue = false;

			std::atomic<uint32_t> m_numBlockers;
			std::vector<ITask*> m_dependencies;
			std::string m_name;

			std::condition_variable m_onComplete;
			std::mutex m_mutex;

			EThreadType m_threadType;
		};

		class Task : public ITask
		{
		public:

			virtual SAILOR_API ~Task() = default;

			SAILOR_API bool IsReadyToStart() const;

			virtual SAILOR_API void Execute() override;
			SAILOR_API Task(const std::string& name, std::function<void()> function, EThreadType thread);

		protected:

			std::function<void()> m_function;
		};

		using TaskPtr = TSharedPtr<Task>;

		class WorkerThread
		{
		public:

			SAILOR_API WorkerThread(
				std::string threadName,
				EThreadType threadType,
				std::condition_variable& refresh,
				std::mutex& mutex,
				std::vector<TaskPtr>& pJobsQueue);

			virtual SAILOR_API ~WorkerThread() = default;

			SAILOR_API WorkerThread(WorkerThread&& move) = delete;
			SAILOR_API WorkerThread(WorkerThread& copy) = delete;
			SAILOR_API WorkerThread& operator =(WorkerThread& rhs) = delete;

			SAILOR_API DWORD GetThreadId() const { return m_threadId; }
			SAILOR_API EThreadType GetThreadType() const { return m_threadType; }

			SAILOR_API void ForcelyPushJob(const TaskPtr& pJob);

			SAILOR_API void Process();
			SAILOR_API void Join();
			SAILOR_API void WaitIdle();

		protected:

			SAILOR_API bool TryFetchJob(TaskPtr& pOutJob);

			std::string m_threadName;
			TUniquePtr<std::thread> m_pThread;

			EThreadType m_threadType;
			DWORD m_threadId;

			std::atomic_bool m_bIsBusy;

			// Specific jobs for this thread
			std::mutex m_queueMutex;
			std::vector<TaskPtr> m_pJobsQueue;

			// Assigned from scheduler
			std::condition_variable& m_refresh;
			std::mutex& m_commonQueueMutex;
			std::vector<TaskPtr>& m_pCommonJobsQueue;
		};

		class Scheduler final : public TSubmodule<Scheduler>
		{
		public:

			SAILOR_API void Initialize();

			virtual SAILOR_API ~Scheduler() override;

			// Lock thit thread until all jobs on thread type would be finished
			SAILOR_API void WaitIdle(EThreadType type);

			uint32_t SAILOR_API GetNumWorkerThreads() const;
			uint32_t SAILOR_API GetNumRenderingJobs() const;

			static SAILOR_API TaskPtr CreateTask(const std::string& name, std::function<void()> lambda, EThreadType thread = EThreadType::Worker);
			SAILOR_API void Run(const TaskPtr& pJob);
			SAILOR_API void Run(const TaskPtr& pJob, DWORD threadId);
			SAILOR_API void ProcessJobsOnMainThread();

			SAILOR_API bool TryFetchNextAvailiableJob(TaskPtr& pOutJob, EThreadType threadType);

			SAILOR_API void NotifyWorkerThread(EThreadType threadType, bool bNotifyAllThreads = false);

			SAILOR_API DWORD GetMainThreadId() const { return m_mainThreadId; };
			SAILOR_API DWORD GetRendererThreadId() const;

			SAILOR_API Scheduler() = default;

		protected:

			SAILOR_API void GetThreadSyncVarsByThreadType(
				EThreadType threadType,
				std::mutex*& pOutMutex,
				std::vector<TaskPtr>*& pOutQueue,
				std::condition_variable*& pOutCondVar);

			std::mutex m_queueMutex[3];
			std::condition_variable m_refreshCondVar[3];
			std::vector<TaskPtr> m_pCommonJobsQueue[3];

			std::atomic<uint32_t> m_numBusyThreads;
			std::vector<WorkerThread*> m_workerThreads;
			std::atomic_bool m_bIsTerminating;

			DWORD m_mainThreadId = -1;
			DWORD m_renderingThreadId = -1;

			friend class WorkerThread;
		};
	}
}