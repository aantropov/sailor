#pragma once
#include <cstdio>
#include <functional>
#include <mutex>
#include <atomic>
#include <thread>
#include "Sailor.h"
#include "Core/Submodule.h"
#include "Memory/UniquePtr.hpp"
#include "Tasks/Tasks.h"

// TODO: Implement ConcurrentList
#include <concurrent_queue.h>

#define SAILOR_ENQUEUE_TASK(Name, Lambda) Sailor::App::GetSubmodule<Tasks::Scheduler>()->Run(Sailor::Tasks::CreateTask(Name, Lambda))
#define SAILOR_ENQUEUE_TASK_RENDER_THREAD(Name, Lambda) Sailor::App::GetSubmodule<Tasks::Scheduler>()->Run(Sailor::Tasks::CreateTask(Name, Lambda, Sailor::Tasks::EThreadType::Render))
#define SAILOR_ENQUEUE_TASK_RHI_THREAD(Name, Lambda) Sailor::App::GetSubmodule<Tasks::Scheduler>()->Run(Sailor::Tasks::CreateTask(Name, Lambda, Sailor::Tasks::EThreadType::RHI))

namespace Sailor
{
	namespace Tasks
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
				TVector<ITaskPtr>& pTasksQueue);

			SAILOR_API virtual ~WorkerThread() = default;

			SAILOR_API WorkerThread(WorkerThread&& move) = delete;
			SAILOR_API WorkerThread(WorkerThread& copy) = delete;
			SAILOR_API WorkerThread& operator =(WorkerThread& rhs) = delete;

			SAILOR_API void SetExecFlag();
			SAILOR_API std::mutex& GetExecMutex() { return m_execMutex; }
			SAILOR_API DWORD GetThreadId() const { return m_threadId; }
			SAILOR_API bool IsBusy() const { return m_bIsBusy.load(); }
			SAILOR_API EThreadType GetThreadType() const { return m_threadType; }

			SAILOR_API void ForcelyPushTask(const ITaskPtr& pTask);

			SAILOR_API void Process();
			SAILOR_API void Join();
			SAILOR_API void WaitIdle();

		protected:

			SAILOR_API void ProcessTask(ITaskPtr& task);
			SAILOR_API bool TryFetchTask(ITaskPtr& pOutTask);

			std::string m_threadName;
			TUniquePtr<std::thread> m_pThread;

			EThreadType m_threadType;
			DWORD m_threadId;

			size_t m_bExecFlag = 0;
			std::atomic<bool> m_bIsBusy;
			std::mutex m_execMutex;

			// Specific tasks for this thread
			std::mutex m_queueMutex;
			TVector<ITaskPtr> m_pTaskQueue;

			// Assigned from scheduler
			std::condition_variable& m_refresh;
			std::mutex& m_sharedQueueMutex;
			TVector<ITaskPtr>& m_pSharedTaskQueue;
		};

		class Scheduler final : public TSubmodule<Scheduler>
		{
			const uint8_t RHIThreadsNum = 2u;
			const size_t MaxTasksInPool = 16384;
			static const uint32_t MaxThreadTypes = (uint32_t)magic_enum::enum_count<EThreadType>();

		public:

			SAILOR_API void Initialize();

			SAILOR_API virtual ~Scheduler() override;

			// Lock this thread until all tasks on thread type would be finished
			SAILOR_API void WaitIdle(EThreadType type);

			// Lock this thread until all tasks are finished
			// In case of Main thread, the tasks would be executed
			SAILOR_API void WaitIdle(const TSet<EThreadType>& threads);

			SAILOR_API uint32_t GetNumWorkerThreads() const;
			SAILOR_API uint32_t GetNumTasks(EThreadType thread) const;
			SAILOR_API uint32_t GetNumRHIThreads() const { return RHIThreadsNum; }

			SAILOR_API void Run(const ITaskPtr& pTask, bool bAutoRunChainedTasks = true);
			SAILOR_API void Run(const ITaskPtr& pTask, DWORD threadId, bool bAutoRunChainedTasks = true);
			SAILOR_API void ProcessTasksOnMainThread();

			SAILOR_API bool TryFetchNextAvailiableTask(ITaskPtr& pOutTask, EThreadType threadType);

			SAILOR_API void NotifyWorkerThread(EThreadType threadType, bool bNotifyAllThreads = false);

			SAILOR_API bool IsMainThread() const;
			SAILOR_API bool IsRendererThread() const;

			SAILOR_API DWORD GetMainThreadId() const { return m_mainThreadId; }
			SAILOR_API DWORD GetRendererThreadId() const { return m_renderingThreadId; }
			SAILOR_API EThreadType GetCurrentThreadType() const;

			SAILOR_API Scheduler() = default;

			SAILOR_API void RunChainedTasks(const ITaskPtr& pTask);

			SAILOR_API TaskSyncBlock& GetTaskSyncBlock(const ITask& task) { return m_taskSyncPool[task.m_taskSyncBlockHandle]; }
			SAILOR_API uint16_t AcquireTaskSyncBlock();
			SAILOR_API void ReleaseTaskSyncBlock(const ITask& task);

		protected:

			SAILOR_API void RunChainedTasks_Internal(const ITaskPtr& pTask, const ITaskPtr& pTaskToIgnore);

			SAILOR_API void GetThreadSyncVarsByThreadType(
				EThreadType threadType,
				std::mutex*& pOutMutex,
				TVector<ITaskPtr>*& pOutQueue,
				std::condition_variable*& pOutCondVar);

			std::mutex m_queueMutex[MaxThreadTypes];
			std::condition_variable m_refreshCondVar[MaxThreadTypes];
			TVector<ITaskPtr> m_pSharedTaskQueue[MaxThreadTypes];

			std::atomic<uint32_t> m_numBusyThreads;
			TVector<WorkerThread*> m_workerThreads;
			std::atomic_bool m_bIsTerminating;

			DWORD m_mainThreadId = -1;
			DWORD m_renderingThreadId = -1;

			// Task Synchronization primitives pool
			concurrency::concurrent_queue<uint16_t> m_freeList{};
			TVector<TaskSyncBlock> m_taskSyncPool{};
			TMap<DWORD, EThreadType> m_threadTypes{};

			friend class WorkerThread;

			template<typename TResult, typename TArgs>
			friend TaskPtr<TResult, TArgs> CreateTask(const std::string& name, typename TFunction<TResult, TArgs>::type lambda, EThreadType thread);
		};
	}
}