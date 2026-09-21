#include "Tasks/Tasks.h"

#include <iostream>
#include <stdexcept>
#include <vector>

using namespace Sailor;

namespace
{
	void Require(bool condition, const char* message)
	{
		if (!condition)
		{
			throw std::runtime_error(message);
		}
	}

	struct TaskLifetime
	{
		uint32_t m_executed = 0;
		uint32_t m_released = 0;
	};

	// A threadless scheduler keeps pending work deterministic. Return a handle
	// directly so this lifetime check needs neither the App singleton nor a window.
	class PendingTask final : public Tasks::ITask
	{
	public:
		PendingTask(Tasks::Scheduler& scheduler, TaskLifetime& lifetime, EThreadType type) :
			ITask("Pending shutdown task", type), m_scheduler(scheduler), m_lifetime(lifetime)
		{
			m_numBlockers = 1;
		}

		~PendingTask() override
		{
			m_scheduler.ReleaseTaskSyncBlock(*this);
			++m_lifetime.m_released;
		}

		void Execute() override
		{
			++m_lifetime.m_executed;
		}

	private:
		Tasks::Scheduler& m_scheduler;
		TaskLifetime& m_lifetime;
	};

	void TestPendingTasksReturnHandlesDuringShutdown()
	{
		TaskLifetime lifetime;
		std::vector<TWeakPtr<Tasks::ITask>> tasks;
		{
			Tasks::Scheduler scheduler;
			scheduler.AttachCurrentThreadAsMainThread();
			for (EThreadType type : magic_enum::enum_values<EThreadType>())
			{
				auto task = TSharedPtr<PendingTask>::Make(scheduler, lifetime, type);
				tasks.emplace_back(task);
				scheduler.Run(task, false);
			}
			Require(lifetime.m_released == 0, "queued tasks must remain owned by the scheduler");
		}
		Require(lifetime.m_executed == 0, "shutdown must not execute blocked work");
		Require(lifetime.m_released == tasks.size(), "shutdown must release every queued task");
		for (const auto& task : tasks)
		{
			Require(!task, "shutdown must not retain queued tasks");
		}
	}

	void TestPendingTaskReleasesItsPredecessor()
	{
		TaskLifetime lifetime;
		TWeakPtr<Tasks::ITask> predecessor;
		{
			Tasks::Scheduler scheduler;
			scheduler.AttachCurrentThreadAsMainThread();
			auto first = TSharedPtr<PendingTask>::Make(scheduler, lifetime, EThreadType::Worker);
			predecessor = first;
			auto next = TSharedPtr<PendingTask>::Make(scheduler, lifetime, EThreadType::Main);
			next->SetChainedTaskPrev(first);
			scheduler.Run(next, false);
		}
		Require(lifetime.m_executed == 0, "shutdown must not run a blocked continuation");
		Require(lifetime.m_released == 2 && !predecessor,
			"queued continuations must release their retained predecessor before the pool is destroyed");
	}
}

int main()
{
	try
	{
		TestPendingTasksReturnHandlesDuringShutdown();
		std::cout << "[PASS] PendingTasksReturnHandlesDuringShutdown\n";
		TestPendingTaskReleasesItsPredecessor();
		std::cout << "[PASS] PendingTaskReleasesItsPredecessor\n";
		return 0;
	}
	catch (const std::exception& error)
	{
		std::cerr << "[FAIL] " << error.what() << '\n';
		return 1;
	}
}
