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

		// The scheduler must outlive scheduling and execution, not retained completed results.
		template<typename TResult = void, typename TArgs = void>
		TaskPtr<TResult, TArgs> CreateTask(Scheduler& scheduler, const std::string& name,
			typename TFunction<TResult, TArgs>::type lambda, EThreadType thread = EThreadType::Worker)
		{
			auto task = TaskPtr<TResult, TArgs>::Make(name, std::move(lambda), thread, &scheduler);
			task->m_self = task;
			return task;
		}

		template<typename TResult = void, typename TArgs = void>
		TaskPtr<TResult, TArgs> CreateTask(const std::string& name, typename TFunction<TResult, TArgs>::type lambda, EThreadType thread = EThreadType::Worker)
		{
			auto task = TaskPtr<TResult, TArgs>::Make(name, std::move(lambda), thread);
			task->m_self = task;
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

			// Register prerequisites before Run. The prerequisites may already be running.
			SAILOR_API void Join(const TWeakPtr<ITask>& taskDependent);
			SAILOR_API void Join(const TVector<TWeakPtr<ITask>>& tasksDependent);

			// Run current task and all chained
			SAILOR_API ITaskPtr Run();

			SAILOR_API bool IsInQueue() const { return m_state & StateMask::IsInQueueBit; }
			SAILOR_API bool TryEnqueue()
			{
				uint8_t state = 0;
				return m_state.compare_exchange_strong(state, StateMask::IsInQueueBit);
			}

			// Lock this thread while task is executing
			SAILOR_API void Wait();

			SAILOR_API EThreadType GetThreadType() const { return m_threadType; }

			SAILOR_API TVector<TWeakPtr<ITask>> GetChainedTasksNext() const;
			SAILOR_API ITaskPtr GetChainedTaskPrev() const;

			SAILOR_API void SetChainedTaskPrev(ITaskPtr task);

		protected:

			SAILOR_API bool AddDependency(ITaskPtr dependentTask);

			SAILOR_API virtual void Complete();
			SAILOR_API void ChainTasks(const ITaskPtr& nextTask);
			virtual void SetContinuationArgs(ITask&) const {}

			SAILOR_API ITask(const std::string& name, EThreadType thread,
				Scheduler* scheduler = App::GetSubmodule<Scheduler>());

			EThreadType m_threadType;
			std::atomic<uint8_t> m_state = 0;
			std::atomic<uint32_t> m_numBlockers = 0;
			TUniquePtr<TaskSyncBlock> m_pSyncBlock;
			// Scheduling, continuations and execution require a live scheduler; Wait and destruction do not.
			Scheduler* m_pScheduler;

			TWeakPtr<ITask> m_self;

			TVector<TWeakPtr<ITask>> m_chainedTasksNext;
			ITaskPtr m_chainedTaskPrev;

			TVector<TWeakPtr<ITask>> m_dependencies;

			std::string m_name; // TODO: remove name, to save 40 bytes

			friend class Scheduler;

			template<typename TResult, typename TArgs>
			friend TaskPtr<TResult, TArgs> CreateTask(const std::string& name, typename TFunction<TResult, TArgs>::type lambda, EThreadType thread);
			template<typename TResult, typename TArgs>
			friend TaskPtr<TResult, TArgs> CreateTask(Scheduler& scheduler, const std::string& name,
				typename TFunction<TResult, TArgs>::type lambda, EThreadType thread);
		};

		template<typename TResult>
		class ITaskWithResult
		{
		public:

			// Read after IsFinished/Wait, or from a registered continuation.
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
				if (auto* scheduler = App::GetSubmodule<Scheduler>())
				{
					scheduler->ReleaseTaskSyncBlock(std::move(ITask::m_pSyncBlock));
				}
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

				ITask::Complete();
			}

			template<typename TResult1>
			Task(TResult1 result, Scheduler* scheduler = App::GetSubmodule<Scheduler>()) requires NotVoid<TResult1>&& NotVoid<TResult>
				: ITask("TaskResult", EThreadType::Worker, scheduler)
			{
				ResultBase::m_result = std::move(result);
				ITask::m_state |= StateMask::IsFinishedBit;
			}

			Task(const std::string& name, Function function, EThreadType thread,
				Scheduler* scheduler = App::GetSubmodule<Scheduler>()) : ITask(name, thread, scheduler)
			{
				m_function = std::move(function);
			}

			template<typename TContinuationResult = void>
			TaskPtr<TContinuationResult, TResult> Then(
				typename TFunction<TContinuationResult, TResult>::type function,
				std::string name = "ChainedTask",
				EThreadType thread = EThreadType::Worker)
			{
				check(ITask::m_pScheduler);
				auto resultTask = Tasks::CreateTask<TContinuationResult, TResult>(*ITask::m_pScheduler,
					name, std::move(function), thread);

				ChainTasks(resultTask);
				RunTaskIfNeeded(resultTask);

				return resultTask;
			}

			SAILOR_API TaskPtr<TResult, void> ToTaskWithResult() requires NotVoid<TResult>
			{
				check(ITask::m_pScheduler);
				typename TFunction<TResult, void>::type function;
				if (ITask::IsFinished())
				{
					function = [result = ResultBase::m_result]() { return result; };
				}
				else
				{
					function = [this]() { return ResultBase::m_result; };
				}
				auto resultTask = Tasks::CreateTask<TResult>(*ITask::m_pScheduler, "Get result task",
					std::move(function), ITask::m_threadType);

				ChainTasks(resultTask);
				RunTaskIfNeeded(resultTask);

				return resultTask;
			}

		protected:

			void SetContinuationArgs(ITask& task) const override
			{
				if constexpr (NotVoid<TResult>)
				{
					if (auto* withArgs = dynamic_cast<ITaskWithArgs<TResult>*>(&task))
					{
						withArgs->SetArgs(ResultBase::m_result);
					}
				}
			}

			SAILOR_API __forceinline void RunTaskIfNeeded(const ITaskPtr& task)
			{
				if (ITask::IsInQueue() || ITask::IsStarted() || ITask::IsFinished())
				{
					ITask::m_pScheduler->Run(task);
				}
			}

			Function m_function;

			friend TaskPtr<TResult, TArgs> CreateTask(const std::string& name, typename TFunction<TResult, TArgs>::type lambda, EThreadType thread);
		};
	}
}
