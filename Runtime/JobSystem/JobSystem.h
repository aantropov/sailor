#pragma once
#include <cstdio>
#include <functional>
#include <mutex>
#include <atomic>
#include <thread>
#include "Sailor.h"
#include "Core/Submodule.h"
#include "Tasks.h"
#include "Memory/UniquePtr.hpp"

#define SAILOR_ENQUEUE_JOB(Name, Lambda) Sailor::App::GetSubmodule<JobSystem::Scheduler>()->Run(Sailor::JobSystem::Scheduler::CreateTask(Name, Lambda))
#define SAILOR_ENQUEUE_JOB_RENDER_THREAD(Name, Lambda) Sailor::App::GetSubmodule<JobSystem::Scheduler>()->Run(Sailor::JobSystem::Scheduler::CreateTask(Name, Lambda, Sailor::JobSystem::EThreadType::Rendering))

namespace Sailor
{
	namespace JobSystem
	{
		class WorkerThread;
		class Scheduler;

		class WorkerThread
		{
		public:

			SAILOR_API WorkerThread(
				std::string threadName,
				EThreadType threadType,
				std::condition_variable& refresh,
				std::mutex& mutex,
				TVector<ITaskPtr>& pJobsQueue);

			virtual SAILOR_API ~WorkerThread() = default;

			SAILOR_API WorkerThread(WorkerThread&& move) = delete;
			SAILOR_API WorkerThread(WorkerThread& copy) = delete;
			SAILOR_API WorkerThread& operator =(WorkerThread& rhs) = delete;

			SAILOR_API DWORD GetThreadId() const { return m_threadId; }
			SAILOR_API EThreadType GetThreadType() const { return m_threadType; }

			SAILOR_API void ForcelyPushJob(const ITaskPtr& pJob);

			SAILOR_API void Process();
			SAILOR_API void Join();
			SAILOR_API void WaitIdle();

		protected:

			SAILOR_API bool TryFetchJob(ITaskPtr& pOutJob);

			std::string m_threadName;
			TUniquePtr<std::thread> m_pThread;

			EThreadType m_threadType;
			DWORD m_threadId;

			std::atomic<bool> m_bIsBusy;

			// Specific jobs for this thread
			std::mutex m_queueMutex;
			TVector<ITaskPtr> m_pJobsQueue;

			// Assigned from scheduler
			std::condition_variable& m_refresh;
			std::mutex& m_commonQueueMutex;
			TVector<ITaskPtr>& m_pCommonJobsQueue;
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

			template<typename TResult = void, typename TArgs = void>
			static SAILOR_API TaskPtr<TResult, TArgs> CreateTask(const std::string& name, std::function<TResult(TArgs)> lambda, EThreadType thread = EThreadType::Worker)
			{
				auto task = TaskPtr<TResult, TArgs>::Make(name, std::move(lambda), thread);
				task->m_self = task;
				return task;
			}

			template<typename TArgs>
			static SAILOR_API TaskPtr<void, TArgs> CreateTaskWithArgs(const std::string& name, std::function<void(TArgs)> lambda, EThreadType thread = EThreadType::Worker)
			{
				auto task = TaskPtr<void, TArgs>::Make(name, std::move(lambda), thread);
				task->m_self = task;
				return task;
			}

			template<typename TResult>
			static SAILOR_API TaskPtr<TResult, void> CreateTaskWithResult(const std::string& name, std::function<TResult()> lambda, EThreadType thread = EThreadType::Worker)
			{
				auto task = TaskPtr<TResult, void>::Make(name, std::move(lambda), thread);
				task->m_self = task;
				return task;
			}

			static SAILOR_API TaskPtr<void, void> CreateTask(const std::string& name, std::function<void()> lambda, EThreadType thread = EThreadType::Worker)
			{
				auto task = TaskPtr<void, void>::Make(name, std::move(lambda), thread);
				task->m_self = task;
				return task;
			}

			SAILOR_API void Run(const ITaskPtr& pJob, bool bAutoRunChainedTasks = true);
			SAILOR_API void Run(const ITaskPtr& pJob, DWORD threadId, bool bAutoRunChainedTasks = true);
			SAILOR_API void ProcessJobsOnMainThread();

			SAILOR_API bool TryFetchNextAvailiableJob(ITaskPtr& pOutJob, EThreadType threadType);

			SAILOR_API void NotifyWorkerThread(EThreadType threadType, bool bNotifyAllThreads = false);

			SAILOR_API DWORD GetMainThreadId() const { return m_mainThreadId; };
			SAILOR_API DWORD GetRendererThreadId() const;

			SAILOR_API Scheduler() = default;

			SAILOR_API void RunChainedTasks(const ITaskPtr& pJob);

		protected:

			SAILOR_API void RunChainedTasks_Internal(const ITaskPtr& pJob, const ITaskPtr& pJobToIgnore);

			SAILOR_API void GetThreadSyncVarsByThreadType(
				EThreadType threadType,
				std::mutex*& pOutMutex,
				TVector<ITaskPtr>*& pOutQueue,
				std::condition_variable*& pOutCondVar);

			std::mutex m_queueMutex[3];
			std::condition_variable m_refreshCondVar[3];
			TVector<ITaskPtr> m_pCommonJobsQueue[3];

			std::atomic<uint32_t> m_numBusyThreads;
			TVector<WorkerThread*> m_workerThreads;
			std::atomic_bool m_bIsTerminating;

			DWORD m_mainThreadId = -1;
			DWORD m_renderingThreadId = -1;

			friend class WorkerThread;
		};
	}
}