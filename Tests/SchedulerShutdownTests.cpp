#include "Tasks/Tasks.h"

#include <iostream>
#include <stdexcept>
#include <unordered_set>
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
			m_pSyncBlock = scheduler.AcquireTaskSyncBlock();
			m_numBlockers = 1;
		}

		~PendingTask() override
		{
			m_scheduler.ReleaseTaskSyncBlock(std::move(m_pSyncBlock));
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

	void TestSyncBlocksGrowAndReuseIndependently()
	{
		constexpr size_t taskCount = 16385;
		Tasks::Scheduler scheduler;
		TaskLifetime lifetime;
		std::vector<TSharedPtr<PendingTask>> tasks;
		std::unordered_set<Tasks::TaskSyncBlock*> blocks;
		for (size_t i = 0; i < taskCount; ++i)
		{
			auto task = TSharedPtr<PendingTask>::Make(scheduler, lifetime, EThreadType::Worker);
			auto& block = scheduler.GetTaskSyncBlock(*task);
			Require(blocks.insert(&block).second, "live tasks must have distinct sync blocks beyond the old capacity");
			Require(!block.m_bCompletionFlag, "new tasks must not inherit another task's completion");
			tasks.emplace_back(std::move(task));
		}

		auto& first = scheduler.GetTaskSyncBlock(*tasks.front());
		auto& last = scheduler.GetTaskSyncBlock(*tasks.back());
		last.m_bCompletionFlag = true;
		Require(!first.m_bCompletionFlag, "the overflow task must not complete the first task");
		auto* releasedBlock = &last;
		tasks.pop_back();
		auto replacement = TSharedPtr<PendingTask>::Make(scheduler, lifetime, EThreadType::Worker);
		auto& reusedBlock = scheduler.GetTaskSyncBlock(*replacement);
		Require(&reusedBlock == releasedBlock, "the pool must reuse released blocks");
		Require(!reusedBlock.m_bCompletionFlag, "reused blocks must reset completion");
		first.m_bCompletionFlag = true;
		Require(!reusedBlock.m_bCompletionFlag, "reused and still-live tasks must remain independent");
	}

	void TestCheckedOutSyncBlockOutlivesScheduler()
	{
		TUniquePtr<Tasks::TaskSyncBlock> block;
		{
			Tasks::Scheduler scheduler;
			block = scheduler.AcquireTaskSyncBlock();
		}
		std::lock_guard<std::mutex> lock(block->m_mutex);
		Require(!block->m_bCompletionFlag, "destroying the scheduler must not invalidate a task-owned block");
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
		TestSyncBlocksGrowAndReuseIndependently();
		std::cout << "[PASS] SyncBlocksGrowAndReuseIndependently\n";
		TestCheckedOutSyncBlockOutlivesScheduler();
		std::cout << "[PASS] CheckedOutSyncBlockOutlivesScheduler\n";
		return 0;
	}
	catch (const std::exception& error)
	{
		std::cerr << "[FAIL] " << error.what() << '\n';
		return 1;
	}
}
