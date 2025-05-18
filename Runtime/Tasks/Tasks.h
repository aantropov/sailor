#pragma once
#include <cstdio>
#include <functional>
#include <mutex>
#include <atomic>
#include <thread>
#include "Sailor.h"
#include "Memory/UniquePtr.hpp"
#include "Memory/SharedPtr.hpp"
#include "Scheduler.h"

namespace Sailor
{
	namespace Tasks
	{
		class Scheduler;

		/* The tasks are using Tasks::Scheduler to run the activities on other threads.
		*  The main point to use tasks is to handle/get results of long term tasks without blocking the current thread.
		*  The chaining is implemented via linked list and there is no need to explicitely run the added(by calling ->Then) tasks.
		*  While Join is designed as low-level kind of call, so you have to run Joined threads explicitely.
		*  Api is designed to always pass the name of Task to it.
		*/

		template<typename T, typename R>
		class Task;

		template<typename TResult = void, typename TArgs = void>
		using TaskPtr = TSharedPtr<Task<TResult, TArgs>>;

		template<typename TResult = void, typename TArgs = void>
		TaskPtr<TResult, TArgs> CreateTask(const std::string& name, typename TFunction<TResult, TArgs>::type lambda, EThreadType thread = EThreadType::Worker)
		{
			auto task = TaskPtr<TResult, TArgs>::Make(name, std::move(lambda), thread);
			task->m_self = task;
			task->m_taskSyncBlockHandle = App::GetSubmodule<Tasks::Scheduler>()->AcquireTaskSyncBlock();
			return task;
		}

		template<typename TArgs>
		TaskPtr<void, TArgs> CreateTaskWithArgs(const std::string& name, typename TFunction<void, TArgs>::type lambda, EThreadType thread = EThreadType::Worker)
		{
			return CreateTask<void, TArgs>(name, lambda, thread);
		}

		template<typename TResult>
		TaskPtr<TResult, void> CreateTaskWithResult(const std::string& name, typename TFunction<TResult, void>::type lambda, EThreadType thread = EThreadType::Worker)
		{
			return CreateTask<TResult, void>(name, lambda, thread);
		}

		class ITask
		{
		protected:

			enum StateMask : uint8_t
			{
				IsInQueueBit = (uint8_t)(1),
				IsStartedBit = (uint8_t)(1 << 1),
				IsFinishedBit = (uint8_t)(1 << 2)
			};

		public:

			SAILOR_API virtual float GetProgress() { return 0.0f; }
			SAILOR_API virtual bool IsFinished() const { return m_state & StateMask::IsFinishedBit; }
			SAILOR_API virtual bool IsExecuting() const { return IsStarted() && !IsFinished(); }
			SAILOR_API virtual bool IsStarted() const { return m_state & StateMask::IsStartedBit; }
			SAILOR_API virtual bool IsReadyToStart() const
			{
				return !IsStarted() && !IsFinished() && m_numBlockers == 0;
			}

			SAILOR_API virtual void Execute() = 0;

			SAILOR_API virtual ~ITask() = default;

			SAILOR_API const std::string& GetName() const { return m_name; }

			// Wait other task's completion before start
			SAILOR_API void Join(const TWeakPtr<ITask>& taskDependent);
			SAILOR_API void Join(const TVector<TWeakPtr<ITask>>& tasksDependent);

			// Run current task and all chained
			SAILOR_API ITaskPtr Run();

			SAILOR_API bool IsInQueue() const { return m_state & StateMask::IsInQueueBit; }
			SAILOR_API void OnEnqueue() { m_state |= StateMask::IsInQueueBit; }

			// Lock this thread while task is executing
			SAILOR_API void Wait();

			SAILOR_API EThreadType GetThreadType() const { return m_threadType; }

			SAILOR_API const TVector<TWeakPtr<ITask>>& GetChainedTasksNext() const { return m_chainedTasksNext; }
			SAILOR_API const ITaskPtr& GetChainedTaskPrev() const { return m_chainedTaskPrev; }

			SAILOR_API void SetChainedTaskPrev(ITaskPtr task);

		protected:

			SAILOR_API bool AddDependency(ITaskPtr dependentTask);

			SAILOR_API virtual void Complete();

			SAILOR_API ITask(const std::string& name, EThreadType thread) : m_threadType(thread), m_numBlockers(0), m_name(name)
			{
			}

			EThreadType m_threadType;
			std::atomic<uint8_t> m_state = 0;
			std::atomic<uint16_t> m_numBlockers = 0;
			uint16_t m_taskSyncBlockHandle = 0;

			TWeakPtr<ITask> m_self;

			TVector<TWeakPtr<ITask>> m_chainedTasksNext;
			ITaskPtr m_chainedTaskPrev;

			TVector<TWeakPtr<ITask>> m_dependencies;

			std::string m_name; // TODO: remove name, to save 40 bytes

			friend class Scheduler;

			template<typename TResult, typename TArgs>
			friend TaskPtr<TResult, TArgs> CreateTask(const std::string& name, typename TFunction<TResult, TArgs>::type lambda, EThreadType thread);
		};

		template<typename TResult>
		class ITaskWithResult
		{
		public:

			SAILOR_API const TResult& GetResult() const { return m_result; }

			TResult m_result{};

		protected:

			ITaskWithResult() = default;
		};

		template<typename TArgs>
		class ITaskWithArgs
		{
		public:

			SAILOR_API void SetArgs(const TArgs& args) { m_args = args; }

		protected:

			ITaskWithArgs() = default;

			TArgs m_args{};
		};

		struct EmptyType1 {};
		struct EmptyType2 {};

		template<typename TResult = void, typename TArgs = void>
		class Task :
			public std::conditional_t<NotVoid<TResult>, ITaskWithResult<TResult>, EmptyType1>,
			public std::conditional_t<NotVoid<TArgs>, ITaskWithArgs<TArgs>, EmptyType2>,
			public ITask
		{

		public:

			using ResultBase = ITaskWithResult<TResult>;
			using ArgsBase = ITaskWithArgs<TArgs>;
			using Function = typename TFunction<TResult, TArgs>::type;

			SAILOR_API virtual ~Task()
			{
				App::GetSubmodule<Scheduler>()->ReleaseTaskSyncBlock(*this);
			}

			SAILOR_API void Execute() override
			{
				ITask::m_state |= StateMask::IsStartedBit;

				if (m_function)
				{
					if constexpr (NotVoid<TArgs>)
					{
						if constexpr (NotVoid<TResult>)
						{
							ResultBase::m_result = m_function(ArgsBase::m_args);
						}
						else
						{
							m_function(ArgsBase::m_args);
						}
					}
					else
					{
						if constexpr (NotVoid<TResult>)
						{
							ResultBase::m_result = m_function();
						}
						else
						{
							m_function();
						}
					}
				}

				if constexpr (NotVoid<TResult>)
				{
					const auto& result = ResultBase::m_result;

					for (auto& m_chainedTaskNext : ITask::m_chainedTasksNext)
					{
						if (auto task = m_chainedTaskNext.Lock())
						{
							if (auto taskWithArgs = dynamic_cast<ITaskWithArgs<TResult>*>(task.GetRawPtr()))
							{
								taskWithArgs->SetArgs(result);
							}
						}
					}
				}

				ITask::Complete();
			}

			template<typename TResult1>
			Task(TResult1 result) requires NotVoid<TResult1>&& NotVoid<TResult>
				: ITask("TaskResult", EThreadType::Worker)
			{
				ResultBase::m_result = std::move(result);
				ITask::m_state |= StateMask::IsFinishedBit;
			}

			Task(const std::string& name, Function function, EThreadType thread) : ITask(name, thread)
			{
				m_function = std::move(function);
			}

                        template<typename TContinuationResult = void>
                        TaskPtr<TContinuationResult, TResult> Then(
                                typename TFunction<TContinuationResult, TResult>::type function,
                                std::string name,
                                EThreadType thread)
                        {
                                auto resultTask = Tasks::CreateTask<TContinuationResult, TResult>(std::move(name), std::move(function), thread);
				if constexpr (NotVoid<TResult>)
				{
					resultTask->SetArgs(ResultBase::m_result);
				}

				ChainTasks(resultTask);
				RunTaskIfNeeded(resultTask);

                                return resultTask;
                        }

                        template<typename TContinuationResult = void>
                        TaskPtr<TContinuationResult, TResult> Then(
                                typename TFunction<TContinuationResult, TResult>::type function,
                                std::string name = "ChainedTask")
                        {
                                return Then<TContinuationResult>(std::move(function), std::move(name), ITask::m_threadType);
                        }

			SAILOR_API TaskPtr<TResult, void> ToTaskWithResult()
			{
				auto resultTask = Tasks::CreateTaskWithResult<TResult>("Get result task",
					std::move([=, this]()
						{
							return ITask::m_self.Lock(). template DynamicCast<ITaskWithResult<TResult>>()->GetResult();
						}), ITask::m_threadType);

				ChainTasks(resultTask);
				RunTaskIfNeeded(resultTask);

				return resultTask;
			}

		protected:

			SAILOR_API __forceinline void ChainTasks(ITaskPtr nextTask)
			{
				if (auto ptr = m_self.TryLock())
				{
					nextTask->SetChainedTaskPrev(ptr);
					nextTask->Join(ptr);
				}

				{
					auto& taskSyncBlock = App::GetSubmodule<Scheduler>()->GetTaskSyncBlock(*this);
					std::unique_lock<std::mutex> lk(taskSyncBlock.m_mutex);
					ITask::m_chainedTasksNext.Add(nextTask);
				}
			}

			SAILOR_API __forceinline void RunTaskIfNeeded(const ITaskPtr& task)
			{
				if (ITask::IsInQueue() || ITask::IsStarted() || ITask::IsFinished())
				{
					App::GetSubmodule<Scheduler>()->Run(task);
				}
			}

			Function m_function;

			friend TaskPtr<TResult, TArgs> CreateTask(const std::string& name, typename TFunction<TResult, TArgs>::type lambda, EThreadType thread);
		};
	}
}