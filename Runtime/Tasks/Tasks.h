#pragma once
#include <cstdio>
#include <functional>
#include <mutex>
#include <atomic>
#include <thread>
#include "Sailor.h"
#include "Memory/UniquePtr.hpp"
#include "Memory/SharedPtr.hpp"

namespace Sailor
{
	namespace Tasks
	{
		class Scheduler;

		enum class EThreadType : uint8_t
		{
			Render = 0,
			Worker = 1,
			Main = 2,

			// RHI Threads are used for creating RHI resources 
			// to decrease the num of RHIThreadContext that could lead to
			// cache misses and extra consumed memory (ram and vram)
			RHI = 3
		};

		/* The tasks are using JobSystem::Scheduler to run the activities on other threads.
		*  The main point to use tasks is to handle/get results of long term tasks without blocking the current thread.
		*  The chaining is implemented via linked list and there is no need to explicitely run the added(by calling ->Then) tasks.
		*  While Join is designed as low-level kind of call, so you have to run Joined threads explicitely.
		*  Api is designed to always pass the name of Task to it.
		*/
		template<typename T, typename R>
		class Task;

		template<typename TResult = void, typename TArgs = void>
		using TaskPtr = TSharedPtr<Task<TResult, TArgs>>;

		using ITaskPtr = TSharedPtr <class ITask>;

		class ITask
		{
		public:

			SAILOR_API virtual float GetProgress() { return 0.0f; }
			SAILOR_API virtual bool IsFinished() const { return m_bIsFinished; }
			SAILOR_API virtual bool IsExecuting() const { return m_bIsStarted && !m_bIsFinished; }
			SAILOR_API virtual bool IsStarted() const { return m_bIsStarted; }
			SAILOR_API virtual bool IsReadyToStart() const
			{
				return !m_bIsStarted && !m_bIsFinished && m_numBlockers == 0;
			}

			SAILOR_API virtual void Execute() = 0;

			SAILOR_API virtual ~ITask() = default;

			SAILOR_API const std::string& GetName() const { return m_name; }

			// Wait other task's completion before start
			SAILOR_API void Join(const TWeakPtr<ITask>& jobDependent);
			SAILOR_API void Join(const TVector<TWeakPtr<ITask>>& jobsDependent);

			// Run current task and all chained
			SAILOR_API TSharedPtr<ITask> Run();

			SAILOR_API bool IsInQueue() const { return m_bIsInQueue; }
			SAILOR_API void OnEnqueue() { m_bIsInQueue = true; }

			// Add new one task that waits this task before start
			/*
			template<typename TResult>
			TSharedPtr<Task<TResult, void>> Then(std::function<TResult()> function, std::string name = "ChainedTask", EThreadType thread = EThreadType::Worker, bool bAutoRun = true)
			{
				auto res = Scheduler::CreateTask<TResult>(std::move(name), std::move(function), thread);
				res->Join(this);

				if (bAutoRun)
				{
				}
				return res;
			}*/

			// Lock this thread while job is executing
			SAILOR_API void Wait();

			SAILOR_API EThreadType GetThreadType() const { return m_threadType; }

			SAILOR_API const TVector<TWeakPtr<ITask>>& GetChainedTasksNext() const { return m_chainedTasksNext; }
			SAILOR_API const TSharedPtr<ITask>& GetChainedTaskPrev() const { return m_chainedTaskPrev; }

			SAILOR_API void SetChainedTaskPrev(TWeakPtr<ITask>& job);

		protected:

			SAILOR_API bool AddDependency(TSharedPtr<ITask> dependentJob);

			SAILOR_API virtual void Complete();

			SAILOR_API ITask(const std::string& name, EThreadType thread) : m_numBlockers(0), m_name(name), m_threadType(thread)
			{
			}

			TWeakPtr<ITask> m_self;
			TVector<TWeakPtr<ITask>> m_chainedTasksNext;
			TSharedPtr<ITask> m_chainedTaskPrev;

			std::atomic<bool> m_bIsFinished = false;
			std::atomic<bool> m_bIsStarted = false;
			std::atomic<bool> m_bIsInQueue = false;

			std::atomic<uint32_t> m_numBlockers;

			TVector<TWeakPtr<ITask>> m_dependencies;
			std::string m_name;

			std::condition_variable m_onComplete;
			std::mutex m_mutex;

			EThreadType m_threadType;

			friend class Scheduler;
		};

		template<typename TResult>
		class ITaskWithResult
		{
		public:

			SAILOR_API const TResult& GetResult() const { return m_result; }

		protected:

			TResult m_result{};
		};

		template<typename TArgs>
		class ITaskWithArgs
		{
		public:

			SAILOR_API void SetArgs(const TArgs& args) { m_args = args; }

		protected:

			TArgs m_args{};
		};

		template<typename TResult = void, typename TArgs = void>
		class Task : public ITask, public ITaskWithResult<TResult>, public ITaskWithArgs<TArgs>
		{
			using Task::ITask::m_chainedTasksNext;
			using Task::ITask::m_chainedTaskPrev;

		public:

			SAILOR_API virtual ~Task() = default;

			SAILOR_API void Execute() override
			{
				m_bIsStarted = true;

				if (m_function)
				{
					ITaskWithResult<TResult>::m_result = m_function(ITaskWithArgs<TArgs>::m_args);
				}

				for (auto& m_chainedTaskNext : m_chainedTasksNext)
				{
					// We could have a chained task without Args
					if (auto taskWithArgs = dynamic_cast<ITaskWithArgs<TResult>*>(m_chainedTaskNext.Lock().GetRawPtr()))
					{
						taskWithArgs->SetArgs(ITaskWithResult<TResult>::m_result);
					}
				}

				Complete();
			}

			SAILOR_API Task(TResult result) : ITask("TaskResult", EThreadType::Worker)
			{
				ITaskWithResult<TResult>::m_result = std::move(result);
				m_bIsFinished = true;
			}

			SAILOR_API Task(const std::string& name, std::function<TResult(TArgs)> function, EThreadType thread) : ITask(name, thread)
			{
				m_function = std::move(function);
			}

			template<typename TResult1, typename TArgs1>
			SAILOR_API TaskPtr<TResult1, TArgs1> Then(std::function<TResult1(TArgs1)> function, std::string name = "ChainedTask", EThreadType thread = EThreadType::Worker)
			{
				auto res = Scheduler::CreateTask(std::move(name), std::move(function), thread);
				res->SetChainedTaskPrev(m_self);
				res->SetArgs(ITaskWithResult<TResult>::m_result);
				res->Join(m_self);
				m_chainedTasksNext.Add(res);

				if (m_bIsStarted || m_bIsInQueue)
				{
					App::GetSubmodule<Scheduler>()->Run(res);
				}
				return res;
			}

			SAILOR_API TaskPtr<TResult, void> ToTaskWithResult()
			{
				auto res = Scheduler::CreateTaskWithResult<TResult>("Get result task", std::move([=]()
				{
					return m_self.Lock().DynamicCast<ITaskWithResult<TResult>>()->GetResult();
				}), m_threadType);

				res->SetChainedTaskPrev(m_self);
				m_chainedTasksNext.Add(res);
				res->Join(m_self);

				if (m_bIsStarted || m_bIsInQueue)
				{
					App::GetSubmodule<Scheduler>()->Run(res);
				}
				return res;
			}

		protected:

			std::function<TResult(TArgs)> m_function;
		};

		template<>
		class Task<void, void> : public ITask
		{
			using Task::ITask::m_chainedTasksNext;
			using Task::ITask::m_chainedTaskPrev;

		public:

			SAILOR_API virtual ~Task() = default;

			SAILOR_API virtual void Execute() override
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

			template<typename TResult1>
			SAILOR_API TSharedPtr<Task<TResult1, void>> Then(std::function<TResult1()> function)
			{
				auto res = Scheduler::CreateTask(m_name + " chained task", std::move(function), m_threadType);				
				res->SetChainedTaskPrev(m_self);
				res->Join(m_self);
				m_chainedTasksNext.Add(res);
				if (m_bIsStarted || m_bIsInQueue)
				{
					App::GetSubmodule<Scheduler>()->Run(res);
				}
				return res;
			}

		protected:

			std::function<void()> m_function;
		};

		template<typename TArgs>
		class Task<void, TArgs> : public ITask, public ITaskWithArgs<TArgs>
		{
			using Task::ITask::m_chainedTasksNext;
			using Task::ITask::m_chainedTaskPrev;

		public:

			SAILOR_API virtual ~Task() = default;

			SAILOR_API virtual void Execute() override
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
			SAILOR_API TaskPtr<TResult1, void> Then(std::function<TResult1()> function)
			{
				auto res = Scheduler::CreateTask(m_name + " chained task", std::move(function), m_threadType);				
				res->SetChainedTaskPrev(m_self);
				res->Join(m_self);
				m_chainedTasksNext.Add(res);

				if (m_bIsStarted || m_bIsInQueue)
				{
					App::GetSubmodule<Scheduler>()->Run(res);
				}
				return res;
			}

		protected:

			std::function<void(TArgs)> m_function;
		};

		template<typename TResult>
		class Task<TResult, void> : public ITask, public ITaskWithResult<TResult>
		{
			using Task::ITask::m_chainedTasksNext;
			using Task::ITask::m_chainedTaskPrev;

		public:

			SAILOR_API virtual ~Task() = default;

			SAILOR_API virtual void Execute() override
			{
				m_bIsStarted = true;

				if (m_function)
				{
					ITaskWithResult<TResult>::m_result = m_function();
				}

				for (auto& m_chainedTaskNext : m_chainedTasksNext)
				{
					// We could have a chained task without Args
					if (auto taskWithArgs = dynamic_cast<ITaskWithArgs<TResult>*>(m_chainedTaskNext.Lock().GetRawPtr()))
					{
						taskWithArgs->SetArgs(ITaskWithResult<TResult>::m_result);
					}
				}

				Complete();
			}

			SAILOR_API Task(TResult result) : ITask("TaskResult", EThreadType::Worker)
			{
				ITaskWithResult<TResult>::m_result = std::move(result);
				m_bIsFinished = true;
			}

			SAILOR_API Task(const std::string& name, std::function<TResult()> function, EThreadType thread) :
				ITask(name, thread)
			{
				m_function = std::move(function);
			}

			template<typename TResult1, typename TArgs1>
			SAILOR_API TaskPtr<TResult1, TArgs1 > Then(std::function<TResult1(TArgs1)> function, std::string name = "ChainedTask", EThreadType thread = EThreadType::Worker)
			{
				auto res = Scheduler::CreateTask(std::move(name), std::move(function), thread);
				res->SetChainedTaskPrev(m_self);
				res->SetArgs(ITaskWithResult<TResult>::GetResult());
				res->Join(m_self);

				m_chainedTasksNext.Add(res);

				if (m_bIsStarted || m_bIsInQueue)
				{
					App::GetSubmodule<Scheduler>()->Run(res);
				}
				return res;
			}

		protected:

			std::function<TResult()> m_function;
		};
	}
}