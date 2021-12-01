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
			virtual SAILOR_API bool IsReadyToStart() const
			{
				return !m_bIsStarted && !m_bIsFinished && m_numBlockers == 0;
			}

			virtual SAILOR_API void Execute() = 0;

			virtual SAILOR_API ~ITask() = default;

			SAILOR_API const std::string& GetName() const { return m_name; }

			SAILOR_API bool AddDependency(ITask* job);

			// Wait other threads completion before start
			SAILOR_API void Join(const TWeakPtr<ITask>& job);
			SAILOR_API void Join(const std::vector<TWeakPtr<ITask>>& jobs);
			SAILOR_API void Join(ITask* job);

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

		template<typename TResult>
		class ITaskWithResult
		{
		public:

			const TResult& GetResult() const { return m_result; }

		protected:

			TResult m_result{};
		};

		template<typename TArgs>
		class ITaskWithArgs
		{
		public:

			void SetArgs(const TArgs& args) { m_args = args; }

		protected:

			TArgs m_args{};
		};

		template<typename TResult = void, typename TArgs = void>
		class Task : public ITask, public ITaskWithResult<TResult>, public ITaskWithArgs<TArgs>
		{
		public:

			virtual SAILOR_API ~Task() = default;

			SAILOR_API void Execute() override
			{
				m_bIsStarted = true;

				if (m_function)
				{
					ITaskWithResult<TResult>::m_result = m_function(ITaskWithArgs<TArgs>::m_args);
				}

				if (m_chainedTask)
				{
					m_chainedTask.Lock()->SetArgs(ITaskWithResult<TResult>::m_result);
				}

				Complete();
			}

			SAILOR_API Task(TResult result) : ITask("TaskResult", EThreadType::Worker)
			{
				ITaskWithResult<TResult>::m_result = std::move(result);
			}

			SAILOR_API Task(const std::string& name, std::function<TResult(TArgs)> function, EThreadType thread)
			{
				m_function = std::move(function);
			}

			template<typename TResult1, typename TArgs1>
			TSharedPtr<Task<TResult1, TArgs1>> Then(std::function<TResult1(TArgs1)> function)
			{
				auto res = Scheduler::CreateTask(m_name + " chained", std::move(function), m_threadType);
				m_chainedTask = res;
				res->SetArgs(ITaskWithResult<TResult>::m_result);
				res->Join(this);
				return res;
			}

		protected:

			std::function<TResult(TArgs)> m_function;
			TWeakPtr<ITaskWithArgs<TResult>> m_chainedTask;
		};

		template<>
		class Task<void, void> : public ITask
		{
		public:

			virtual SAILOR_API ~Task() = default;

			virtual SAILOR_API void Execute() override
			{
				m_bIsStarted = true;

				if (m_function)
				{
					m_function();
				}

				Complete();
			}

			SAILOR_API Task(const std::string& name, std::function<void()> function, EThreadType thread) :
				ITask(name, thread)
			{
				m_function = std::move(function);
			}

		protected:

			std::function<void()> m_function;
		};

		template<typename TArgs>
		class Task<void, TArgs> : public ITask, public ITaskWithArgs<TArgs>
		{
		public:

			virtual SAILOR_API ~Task() = default;

			virtual SAILOR_API void Execute() override
			{
				m_bIsStarted = true;

				if (m_function)
				{
					m_function(ITaskWithArgs<TArgs>::m_args);
				}

				Complete();
			}

			SAILOR_API Task(const std::string& name, std::function<void(TArgs)> function, EThreadType thread) :
				ITask(name, thread)
			{
				m_function = std::move(function);
			}

			template<typename TResult1>
			SAILOR_API TSharedPtr<Task<TResult1, void>> Then(std::function<TResult1()> function)
			{
				auto res = Scheduler::CreateTask(m_name + " chained task", std::move(function), m_threadType);
				m_chainedTask = res;
				res->Join(this);
				return res;
			}

		protected:

			std::function<void(TArgs)> m_function;
			TWeakPtr<ITask> m_chainedTask;
		};

		template<typename TResult>
		class Task<TResult, void> : public ITask, public ITaskWithResult<TResult>
		{
		public:

			virtual SAILOR_API ~Task() = default;

			virtual SAILOR_API void Execute() override
			{
				m_bIsStarted = true;

				if (m_function)
				{
					ITaskWithResult<TResult>::m_result = m_function();
				}

				if (m_chainedTask)
				{
					m_chainedTask.Lock()->SetArgs(ITaskWithResult<TResult>::m_result);
				}

				Complete();
			}

			SAILOR_API Task(TResult result) : ITask("TaskResult", EThreadType::Worker)
			{
				ITaskWithResult<TResult>::m_result = std::move(result);
			}

			SAILOR_API Task(const std::string& name, std::function<TResult()> function, EThreadType thread) :
				ITask(name, thread)
			{
				m_function = std::move(function);
			}

			template<typename TResult1, typename TArgs1>
			SAILOR_API TSharedPtr<Task<TResult1, TArgs1>> Then(std::function<TResult1(TArgs1)> function)
			{
				auto res = Scheduler::CreateTask(m_name + " chained task", function, m_threadType);
				m_chainedTask = res;
				res->SetArgs(ITaskWithResult<TResult>::m_result);
				res->Join(this);
				return res;
			}

		protected:

			std::function<TResult()> m_function;
			TWeakPtr<ITaskWithArgs<TResult>> m_chainedTask;
		};

		template<typename TResult = void, typename TArgs = void>
		using TaskPtr = TSharedPtr<Task<TResult, TArgs>>;

		using ITaskPtr = TSharedPtr<ITask>;

		class WorkerThread
		{
		public:

			SAILOR_API WorkerThread(
				std::string threadName,
				EThreadType threadType,
				std::condition_variable& refresh,
				std::mutex& mutex,
				std::vector<ITaskPtr>& pJobsQueue);

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
			std::vector<ITaskPtr> m_pJobsQueue;

			// Assigned from scheduler
			std::condition_variable& m_refresh;
			std::mutex& m_commonQueueMutex;
			std::vector<ITaskPtr>& m_pCommonJobsQueue;
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
				return TaskPtr<TResult, TArgs>::Make(name, std::move(lambda), thread);
			}

			template<typename TArgs>
			static SAILOR_API TaskPtr<void, TArgs> CreateTaskWithArgs(const std::string& name, std::function<void(TArgs)> lambda, EThreadType thread = EThreadType::Worker)
			{
				return TaskPtr<void, TArgs>::Make(name, std::move(lambda), thread);
			}

			template<typename TResult>
			static SAILOR_API TaskPtr<TResult, void> CreateTaskWithResult(const std::string& name, std::function<TResult()> lambda, EThreadType thread = EThreadType::Worker)
			{
				return TaskPtr<TResult, void>::Make(name, std::move(lambda), thread);
			}

			static SAILOR_API TaskPtr<void, void> CreateTask(const std::string& name, std::function<void()> lambda, EThreadType thread = EThreadType::Worker)
			{
				return TaskPtr<void, void>::Make(name, std::move(lambda), thread);
			}

			SAILOR_API void Run(const ITaskPtr& pJob);
			SAILOR_API void Run(const ITaskPtr& pJob, DWORD threadId);
			SAILOR_API void ProcessJobsOnMainThread();

			SAILOR_API bool TryFetchNextAvailiableJob(ITaskPtr& pOutJob, EThreadType threadType);

			SAILOR_API void NotifyWorkerThread(EThreadType threadType, bool bNotifyAllThreads = false);

			SAILOR_API DWORD GetMainThreadId() const { return m_mainThreadId; };
			SAILOR_API DWORD GetRendererThreadId() const;

			SAILOR_API Scheduler() = default;

		protected:

			SAILOR_API void GetThreadSyncVarsByThreadType(
				EThreadType threadType,
				std::mutex*& pOutMutex,
				std::vector<ITaskPtr>*& pOutQueue,
				std::condition_variable*& pOutCondVar);

			std::mutex m_queueMutex[3];
			std::condition_variable m_refreshCondVar[3];
			std::vector<ITaskPtr> m_pCommonJobsQueue[3];

			std::atomic<uint32_t> m_numBusyThreads;
			std::vector<WorkerThread*> m_workerThreads;
			std::atomic_bool m_bIsTerminating;

			DWORD m_mainThreadId = -1;
			DWORD m_renderingThreadId = -1;

			friend class WorkerThread;
		};
	}
}