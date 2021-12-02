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
	namespace JobSystem
	{
		class Scheduler;
		
		enum class EThreadType : uint8_t
		{
			Rendering = 0,
			Worker = 1,
			Main = 2
		};

		template<typename T, typename R>
		class Task;

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

			// Wait other task's completion before start
			SAILOR_API void Join(const TWeakPtr<ITask>& job);
			SAILOR_API void Join(const std::vector<TWeakPtr<ITask>>& jobs);
			SAILOR_API void Join(ITask* job);

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
			SAILOR_API const TWeakPtr<ITask>& GetChainedTask() const { return m_chainedTask; }

		protected:

			virtual SAILOR_API void Complete();

			SAILOR_API ITask(const std::string& name, EThreadType thread) : m_numBlockers(0), m_name(name), m_threadType(thread)
			{
			}

			TWeakPtr<ITask> m_chainedTask;

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
		public:

			virtual SAILOR_API ~Task() = default;

			SAILOR_API void Execute() override
			{
				m_bIsStarted = true;

				if (m_function)
				{
					ITaskWithResult<TResult>::m_result = m_function(ITaskWithArgs<TArgs>::m_args);
				}

				if (m_chainedTaskInterface)
				{
					m_chainedTaskInterface.Lock()->SetArgs(ITaskWithResult<TResult>::m_result);
				}

				Complete();
			}

			SAILOR_API Task(TResult result) : ITask("TaskResult", EThreadType::Worker)
			{
				ITaskWithResult<TResult>::m_result = std::move(result);
				m_bIsFinished = true;
			}

			SAILOR_API Task(const std::string& name, std::function<TResult(TArgs)> function, EThreadType thread)
			{
				m_function = std::move(function);
			}

			template<typename TResult1, typename TArgs1>
			SAILOR_API TSharedPtr<Task<TResult1, TArgs1>> Then(std::function<TResult1(TArgs1)> function, std::string name = "ChainedTask", EThreadType thread = EThreadType::Worker)
			{
				auto res = Scheduler::CreateTask(std::move(name), std::move(function), thread);
				m_chainedTaskInterface = res;
				res->SetArgs(ITaskWithResult<TResult>::m_result);
				res->Join(this);

				if (m_bIsStarted || m_bIsInQueue)
				{
					App::GetSubmodule<Scheduler>()->Run(res);
				}
				return res;
			}

		protected:

			std::function<TResult(TArgs)> m_function;
			TWeakPtr<ITaskWithArgs<TResult>> m_chainedTaskInterface;
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
				m_chainedTaskInterface = res;
				res->Join(this);
				return res;
			}

		protected:

			std::function<void(TArgs)> m_function;
			TWeakPtr<ITask> m_chainedTaskInterface;
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

				if (m_chainedTaskInterface)
				{
					m_chainedTaskInterface.Lock()->SetArgs(ITaskWithResult<TResult>::m_result);
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
			SAILOR_API TSharedPtr<Task<TResult1, TArgs1>> Then(std::function<TResult1(TArgs1)> function, std::string name = "ChainedTask", EThreadType thread = EThreadType::Worker)
			{
				auto res = Scheduler::CreateTask(std::move(name), std::move(function), thread);
				m_chainedTaskInterface = res;
				m_chainedTask = res;
				res->SetArgs(ITaskWithResult<TResult>::m_result);
				res->Join(this);

				if (m_bIsStarted || m_bIsInQueue)
				{
					App::GetSubmodule<Scheduler>()->Run(res);
				}

				return res;
			}

		protected:

			std::function<TResult()> m_function;
			TWeakPtr<ITaskWithArgs<TResult>> m_chainedTaskInterface;			
		};

		template<typename TResult = void, typename TArgs = void>
		using TaskPtr = TSharedPtr<Task<TResult, TArgs>>;

		using ITaskPtr = TSharedPtr<ITask>;
	}
}