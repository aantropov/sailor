#include "Tasks/Tasks.h"

#include <array>
#include <atomic>
#include <barrier>
#include <chrono>
#include <cstddef>
#include <iostream>
#include <latch>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>
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

	bool WaitForTask(Tasks::Scheduler& scheduler, const Tasks::ITaskPtr& task)
	{
		auto& block = scheduler.GetTaskSyncBlock(*task);
		std::unique_lock<std::mutex> lock(block.m_mutex);
		return block.m_onComplete.wait_for(lock, std::chrono::seconds(5),
			[&]() { return block.m_bCompletionFlag; });
	}

	// Observe the real worker's readiness check without changing its result.
	class ObservedPinnedTask final : public Tasks::Task<int>
	{
	public:
		ObservedPinnedTask(Tasks::Scheduler& scheduler, Function function) :
			Tasks::Task<int>("Pinned dependent", std::move(function), EThreadType::Audio, &scheduler)
		{
		}

		static TSharedPtr<ObservedPinnedTask> Create(Tasks::Scheduler& scheduler, Function function)
		{
			auto task = TSharedPtr<ObservedPinnedTask>::Make(scheduler, std::move(function));
			task->m_self = task;
			return task;
		}

		bool IsReadyToStart() const override
		{
			const bool ready = Tasks::ITask::IsReadyToStart();
			if (!ready)
			{
				++m_blockedChecks;
			}
			return ready;
		}

		uint32_t GetBlockedCheckCount() const { return m_blockedChecks.load(); }

		bool WaitForBlockedCheckAfter(uint32_t previous) const
		{
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
			while (m_blockedChecks <= previous)
			{
				if (std::chrono::steady_clock::now() >= deadline)
				{
					return false;
				}
				std::this_thread::yield();
			}
			return true;
		}

	private:
		mutable std::atomic<uint32_t> m_blockedChecks = 0;
	};

	struct Result
	{
		std::array<uint64_t, 64> m_values{};
		bool operator==(const Result&) const = default;
	};

	Result MakeResult(uint64_t seed)
	{
		Result result;
		for (size_t i = 0; i < result.m_values.size(); ++i)
		{
			result.m_values[i] = seed * (i + 1) + i * i;
		}
		return result;
	}

	void TestContinuationsBeforeDuringAndAfterCompletion()
	{
		Tasks::Scheduler scheduler;
		scheduler.AttachCurrentThreadAsMainThread();
		const Result expected = MakeResult(37);
		std::latch started(1);
		std::latch finish(1);
		std::atomic<uint32_t> executions = 0;
		std::atomic<uint32_t> mismatches = 0;
		auto parent = Tasks::CreateTask<Result>(scheduler, "Publish result", [&]()
			{
				++executions;
				started.count_down();
				finish.wait();
				return expected;
			}, EThreadType::Main);

		constexpr size_t phaseSize = 32;
		std::array<std::atomic<uint32_t>, phaseSize * 3> calls{};
		std::vector<Tasks::ITaskPtr> continuations;
		auto addContinuations = [&](size_t first)
			{
				for (size_t i = first; i < first + phaseSize; ++i)
				{
					continuations.emplace_back(parent->Then([&, i](Result value)
						{
							++calls[i];
							if (value != expected || !parent->IsFinished())
							{
								++mismatches;
							}
						}, "Read result", EThreadType::Main));
				}
			};

		addContinuations(0);
		// An already queued sibling must not prevent scheduling later siblings.
		scheduler.Run(continuations.front(), false);
		parent->Run();
		std::jthread executor([&]() { scheduler.ProcessTasksOnMainThread(); });
		started.wait();
		addContinuations(phaseSize);
		std::jthread waiter([&]()
			{
				parent->Wait();
				if (parent->GetResult() != expected)
				{
					++mismatches;
				}
			});
		finish.count_down();
		executor.join();
		waiter.join();
		addContinuations(phaseSize * 2);
		scheduler.ProcessTasksOnMainThread();

		Require(executions == 1, "the parent must execute once");
		Require(mismatches == 0, "continuations and Wait must observe the published result");
		for (size_t i = 0; i < calls.size(); ++i)
		{
			Require(calls[i] == 1 && continuations[i]->IsFinished(),
				"every continuation must execute exactly once in all three registration phases");
		}
	}

	void TestConcurrentRegistrationAndRun()
	{
		constexpr size_t registrarCount = 4;
		constexpr size_t perRegistrar = 32;
		constexpr size_t continuationCount = registrarCount * perRegistrar;
		for (uint64_t iteration = 0; iteration < 16; ++iteration)
		{
			const Result expected = MakeResult(iteration + 1);
			std::atomic<uint32_t> parentCalls = 0;
			std::atomic<uint32_t> mismatches = 0;
			std::array<std::atomic<uint32_t>, continuationCount> calls{};
			Tasks::Scheduler scheduler;
			scheduler.AttachCurrentThreadAsMainThread();
			std::array<Tasks::ITaskPtr, continuationCount> continuations;
			auto parent = Tasks::CreateTask<Result>(scheduler, "Concurrent publication", [&]()
				{
					++parentCalls;
					return expected;
				}, EThreadType::Main);

			std::atomic<bool> registrationFinished = false;
			std::atomic<bool> timedOut = false;
			std::jthread executor([&]()
				{
					const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
					while (!registrationFinished || scheduler.GetNumTasks(EThreadType::Main) != 0)
					{
						scheduler.ProcessTasksOnMainThread();
						if (std::chrono::steady_clock::now() >= deadline)
						{
							timedOut = true;
							break;
						}
						std::this_thread::yield();
					}
				});
			std::barrier start(static_cast<std::ptrdiff_t>(registrarCount + 1));
			std::vector<std::jthread> registrars;
			for (size_t registrar = 0; registrar < registrarCount; ++registrar)
			{
				registrars.emplace_back([&, registrar]()
					{
						start.arrive_and_wait();
						for (size_t offset = 0; offset < perRegistrar; ++offset)
						{
							const size_t index = registrar * perRegistrar + offset;
							continuations[index] = parent->Then([&, index, parent](Result result)
								{
									++calls[index];
									if (result != expected || !parent->IsFinished())
									{
										++mismatches;
									}
								}, "Concurrent continuation", EThreadType::Main);
							parent->Run();
						}
					});
			}
			start.arrive_and_wait();
			parent->Run();
			for (auto& registrar : registrars)
			{
				registrar.join();
			}
			registrationFinished = true;
			executor.join();

			Require(!timedOut, "concurrently registered continuations must not remain blocked");
			Require(parentCalls == 1, "concurrent Run calls must admit the task once");
			Require(mismatches == 0, "concurrent registration must never read an unpublished result");
			for (size_t i = 0; i < continuationCount; ++i)
			{
				Require(calls[i] == 1 && continuations[i]->IsFinished(),
					"concurrent continuation registration must neither lose nor duplicate work");
			}
		}
	}

	void TestRunFromLeafRetainsPredecessors()
	{
		Tasks::Scheduler scheduler;
		scheduler.AttachCurrentThreadAsMainThread();
		Tasks::TaskPtr<int> result;
		TWeakPtr<Tasks::ITask> predecessor;
		{
			auto first = Tasks::CreateTask<int>(scheduler, "First result", []() { return 20; }, EThreadType::Main);
			predecessor = first;
			result = first->Then<int>([](int value) { return value * 2 + 2; },
				"Transform result", EThreadType::Main)->ToTaskWithResult();
		}
		Require(static_cast<bool>(predecessor), "a continuation must retain unfinished predecessors");
		result->Run();
		scheduler.ProcessTasksOnMainThread();
		Require(result->IsFinished() && result->GetResult() == 42,
			"running the leaf must execute and publish the complete chain");
		Require(!predecessor, "completed continuations must release predecessor ownership");
	}

	void TestQueuedIntermediateStillSchedulesItsSubtree()
	{
		for (bool runFromLeaf : { false, true })
		{
			Tasks::Scheduler scheduler;
			scheduler.AttachCurrentThreadAsMainThread();
			std::array<uint32_t, 5> calls{};
			auto root = Tasks::CreateTask<int>(scheduler, "Root", [&]()
				{
					++calls[0];
					return 40;
				}, EThreadType::Main);
			auto middle = root->Then<int>([&](int value)
				{
					++calls[1];
					return value + 1;
				}, "Queued intermediate", EThreadType::Main);
			auto leaf = middle->Then<int>([&](int value)
				{
					++calls[2];
					return value + 1;
				}, "Leaf", EThreadType::Main);
			auto sibling = middle->Then([&](int) { ++calls[3]; }, "Sibling", EThreadType::Main);
			auto cousin = root->Then([&](int) { ++calls[4]; }, "Cousin", EThreadType::Main);
			scheduler.Run(middle, false);
			if (runFromLeaf)
			{
				leaf->Run();
			}
			else
			{
				root->Run();
			}
			scheduler.ProcessTasksOnMainThread();
			for (uint32_t count : calls)
			{
				Require(count == 1, "a queued intermediate must not hide its parent, children or siblings");
			}
			Require(leaf->IsFinished() && leaf->GetResult() == 42 && sibling->IsFinished() && cousin->IsFinished(),
				"chain traversal must publish every descendant's result exactly once");
		}
	}

	void TestNewContinuationDoesNotAdmitParkedSiblings()
	{
		uint32_t parkedCalls = 0;
		Tasks::Scheduler scheduler;
		scheduler.AttachCurrentThreadAsMainThread();
		auto parent = Tasks::CreateTask<int>(scheduler, "Admitted parent", []() { return 42; }, EThreadType::Main);
		auto parked = parent->Then([&](int) { ++parkedCalls; }, "Parked sibling", EThreadType::Main);
		scheduler.Run(parent, false);

		std::vector<Tasks::TaskPtr<int, int>> children;
		for (int i = 0; i < 128; ++i)
		{
			children.emplace_back(parent->Then<int>([i](int value) { return value + i; },
				"New continuation", EThreadType::Main));
		}
		auto copiedResult = parent->ToTaskWithResult();
		Require(!parked->IsInQueue(), "adding a continuation must not reschedule the parent's other branches");
		scheduler.ProcessTasksOnMainThread();
		for (size_t i = 0; i < children.size(); ++i)
		{
			Require(children[i]->IsFinished() && children[i]->GetResult() == 42 + static_cast<int>(i),
				"a newly registered child must execute without traversing parked sibling branches");
		}
		Require(copiedResult->IsFinished() && copiedResult->GetResult() == 42,
			"ToTaskWithResult must admit its new child independently of sibling branches");
		Require(parkedCalls == 0 && !parked->IsStarted(), "Run without chain traversal must leave existing siblings parked");

		parked->Run();
		scheduler.ProcessTasksOnMainThread();
		Require(parkedCalls == 1 && parked->IsFinished(), "a parked branch must still support explicit Run");
	}

	void TestPinnedQueuesRespectBlockersAndWakeAffinity()
	{
		// Captured state must outlive scheduler shutdown if a bounded wait fails.
		std::atomic<uint32_t> calls = 0;
		std::atomic<uint32_t> checksAtMarker = 0;
		std::atomic<DWORD> executedOn = 0;
		std::atomic<EThreadType> executedType = EThreadType::Main;
		Tasks::Scheduler scheduler;
		scheduler.Initialize();
		Tasks::Scheduler prerequisiteScheduler;
		prerequisiteScheduler.AttachCurrentThreadAsMainThread();
		auto identifyWorker = Tasks::CreateTask<DWORD>(scheduler, "Identify worker", []() { return GetCurrentThreadId(); });
		identifyWorker->Run();
		Require(WaitForTask(scheduler, identifyWorker), "a real Worker queue must execute without the App singleton");
		const std::array<DWORD, 2> targetThreads{ scheduler.GetEditorThreadId(), identifyWorker->GetResult() };

		for (size_t i = 0; i < targetThreads.size(); ++i)
		{
			calls = 0;
			const DWORD targetThread = targetThreads[i];
			auto& prerequisites = i == 0 ? scheduler : prerequisiteScheduler;
			auto first = Tasks::CreateTask<int>(prerequisites, "First prerequisite", []() { return 20; }, EThreadType::Main);
			auto second = Tasks::CreateTask<int>(prerequisites, "Last prerequisite", []() { return 22; }, EThreadType::Main);
			auto dependent = ObservedPinnedTask::Create(scheduler, [&, first, second]()
				{
					++calls;
					executedOn = GetCurrentThreadId();
					executedType = scheduler.GetCurrentThreadType();
					return first->IsFinished() && second->IsFinished()
						? first->GetResult() + second->GetResult() : -1;
				});
			dependent->Join(first);
			dependent->Join(second);
			auto marker = Tasks::CreateTask<>(scheduler, "Ready marker", [&, dependent]()
				{
					checksAtMarker = dependent->GetBlockedCheckCount();
				});
			auto setup = Tasks::CreateTask<>(scheduler, "Queue pinned work", [&, targetThread, marker, dependent]()
				{
					scheduler.Run(marker, targetThread, false);
					// The private queue is LIFO: its last entry must be skipped while blocked.
					scheduler.Run(dependent, targetThread, false);
				});
			scheduler.Run(setup, targetThread, false);
			Require(WaitForTask(scheduler, marker), "a blocked pinned task must not hide ready work in the same queue");
			Require(!dependent->IsStarted() && calls == 0, "pinning must not bypass Join prerequisites");
			Require(dependent->WaitForBlockedCheckAfter(checksAtMarker),
				"the pinned worker must revisit its blocked queue before waiting for completion");

			prerequisites.Run(first, false);
			prerequisites.ProcessTasksOnMainThread();
			auto nextMarker = Tasks::CreateTask<>(scheduler, "One blocker remains", [&, dependent]()
				{
					checksAtMarker = dependent->GetBlockedCheckCount();
				});
			scheduler.Run(nextMarker, targetThread, false);
			Require(WaitForTask(scheduler, nextMarker), "the pinned queue must remain usable while one prerequisite is pending");
			Require(!dependent->IsStarted() && calls == 0, "every Join prerequisite must finish before execution");
			Require(dependent->WaitForBlockedCheckAfter(checksAtMarker), "the target worker must be waiting before the last prerequisite completes");

			// A second admission attempt must not replace the first queue's affinity.
			scheduler.Run(dependent, scheduler.GetAudioThreadId(), false);
			prerequisites.Run(second, false);
			prerequisites.ProcessTasksOnMainThread();
			Require(WaitForTask(scheduler, dependent), "completion must wake the pinned worker, not the nominal Audio pool or prerequisite's scheduler");
			Require(calls == 1 && dependent->GetResult() == 42, "pinned execution must observe all prerequisite results exactly once");
			Require(executedOn == targetThread, "the constructor's worker ID must match its actual execution thread");
			Require(executedType == (i == 0 ? EThreadType::Editor : EThreadType::Worker),
				"a pinned task must run in its selected pool even when its nominal pool differs");
		}
	}

	void TestPinnedContinuationsPublishResultsAndRespectMainQueue()
	{
		const Result expected = MakeResult(117);
		std::atomic<DWORD> executedOn = 0;
		Tasks::Scheduler scheduler;
		scheduler.Initialize();
		auto parent = Tasks::CreateTask<Result>(scheduler, "Publish pinned result", [expected]() { return expected; }, EThreadType::Main);
		auto child = parent->Then<Result>([&](Result value)
			{
				executedOn = GetCurrentThreadId();
				return value;
			}, "Pinned typed continuation", EThreadType::Audio);
		auto marker = Tasks::CreateTask<>(scheduler, "Ready continuation marker", []() {});
		const DWORD editorThread = scheduler.GetEditorThreadId();
		auto setup = Tasks::CreateTask<>(scheduler, "Queue pinned continuation", [&, child, marker, editorThread]()
			{
				scheduler.Run(marker, editorThread, false);
				scheduler.Run(child, editorThread, false);
			});
		scheduler.Run(setup, editorThread, false);
		Require(WaitForTask(scheduler, marker) && !child->IsStarted(), "a pinned continuation must wait for its parent's published arguments");
		scheduler.Run(parent, false);
		scheduler.ProcessTasksOnMainThread();
		Require(WaitForTask(scheduler, child) && child->GetResult() == expected,
			"a real pinned worker must receive the complete continuation result");
		Require(executedOn == editorThread, "the typed continuation must preserve explicit thread affinity");

		auto workerParent = Tasks::CreateTask<Result>(scheduler, "Publish to main", [expected]() { return expected; }, EThreadType::Editor);
		auto mainChild = workerParent->Then<Result>([&](Result value)
			{
				executedOn = GetCurrentThreadId();
				return value;
			}, "Main-pinned continuation", EThreadType::Worker);
		scheduler.Run(mainChild, scheduler.GetMainThreadId(), false);
		scheduler.Run(workerParent, false);
		Require(WaitForTask(scheduler, workerParent), "the real worker prerequisite must finish before main-thread processing");
		Require(!mainChild->IsStarted(), "pinning to Main must not run the task in its nominal Worker pool");
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
		while (!mainChild->IsFinished() && std::chrono::steady_clock::now() < deadline)
		{
			scheduler.ProcessTasksOnMainThread();
			std::this_thread::yield();
		}
		Require(mainChild->IsFinished() && mainChild->GetResult() == expected && executedOn == scheduler.GetMainThreadId(),
			"main-thread processing must execute a Main-pinned continuation with published arguments");
	}

	void TestCachedResultsAndSchedulerLifetime()
	{
		Tasks::TaskPtr<Result> retained;
		const Result expected = MakeResult(91);
		{
			Tasks::Scheduler scheduler;
			scheduler.AttachCurrentThreadAsMainThread();
			auto cached = Tasks::TaskPtr<Result>::Make(expected, &scheduler);
			retained = cached->ToTaskWithResult();
			auto child = cached->Then<uint64_t>([](Result result) { return result.m_values[7]; },
				"Read cached result", EThreadType::Main);
			cached.Clear();
			Tasks::ITaskPtr copyTask;
			Require(scheduler.TryFetchNextAvailiableTask(copyTask, EThreadType::Worker),
				"cached result conversion must schedule its result task");
			copyTask->Execute();
			scheduler.ProcessTasksOnMainThread();
			Require(child->IsFinished() && child->GetResult() == expected.m_values[7],
				"cached results must support continuations without an initialized weak self");
		}
		retained->Wait();
		Require(retained->IsFinished() && retained->GetResult() == expected,
			"waiting on and reading a completed result must not access a destroyed scheduler");
		retained.Clear();
	}

	void TestJoinExpiredAndFinishedTasks()
	{
		Tasks::Scheduler scheduler;
		scheduler.AttachCurrentThreadAsMainThread();
		TWeakPtr<Tasks::ITask> expired;
		{
			auto discarded = Tasks::CreateTask<>(scheduler, "Discarded prerequisite", []() {}, EThreadType::Main);
			expired = discarded;
		}
		auto completed = Tasks::CreateTask<>(scheduler, "Finished prerequisite", []() {}, EThreadType::Main);
		completed->Run();
		scheduler.ProcessTasksOnMainThread();
		uint32_t calls = 0;
		auto dependent = Tasks::CreateTask<>(scheduler, "Independent task", [&]() { ++calls; }, EThreadType::Main);
		dependent->Join(expired);
		dependent->Join(completed);
		dependent->Run();
		scheduler.ProcessTasksOnMainThread();
		Require(calls == 1 && dependent->IsFinished(), "expired or finished prerequisites must not block Join");
	}

	void TestJoinBeyondSixteenBitFanIn()
	{
		constexpr size_t prerequisiteCount = 65536;
		uint32_t calls = 0;
		Tasks::Scheduler scheduler;
		scheduler.AttachCurrentThreadAsMainThread();
		auto dependent = Tasks::CreateTask<>(scheduler, "Large fan-in", [&]() { ++calls; }, EThreadType::Main);
		std::vector<Tasks::TaskPtr<>> prerequisites;
		prerequisites.reserve(prerequisiteCount);
		for (size_t i = 0; i < prerequisiteCount; ++i)
		{
			auto prerequisite = Tasks::CreateTask<>(scheduler, "Prerequisite", []() {}, EThreadType::Main);
			dependent->Join(prerequisite);
			prerequisites.emplace_back(std::move(prerequisite));
		}
		dependent->Run();
		Require(!dependent->IsReadyToStart(), "65536 prerequisites must not wrap the blocker count to zero");
		scheduler.ProcessTasksOnMainThread();
		Require(calls == 0, "large fan-in must not execute before its prerequisites");
		for (size_t i = 0; i + 1 < prerequisites.size(); ++i)
		{
			prerequisites[i]->Run();
			scheduler.ProcessTasksOnMainThread();
		}
		Require(calls == 0 && !dependent->IsReadyToStart(), "the final prerequisite must still block execution");
		prerequisites.back()->Run();
		scheduler.ProcessTasksOnMainThread();
		Require(calls == 1 && dependent->IsFinished(), "the final prerequisite must release the dependent once");
	}
}

int main()
{
	try
	{
		TestContinuationsBeforeDuringAndAfterCompletion();
		std::cout << "[PASS] ContinuationsBeforeDuringAndAfterCompletion\n";
		TestConcurrentRegistrationAndRun();
		std::cout << "[PASS] ConcurrentRegistrationAndRun\n";
		TestRunFromLeafRetainsPredecessors();
		std::cout << "[PASS] RunFromLeafRetainsPredecessors\n";
		TestQueuedIntermediateStillSchedulesItsSubtree();
		std::cout << "[PASS] QueuedIntermediateStillSchedulesItsSubtree\n";
		TestNewContinuationDoesNotAdmitParkedSiblings();
		std::cout << "[PASS] NewContinuationDoesNotAdmitParkedSiblings\n";
		TestPinnedQueuesRespectBlockersAndWakeAffinity();
		std::cout << "[PASS] PinnedQueuesRespectBlockersAndWakeAffinity\n";
		TestPinnedContinuationsPublishResultsAndRespectMainQueue();
		std::cout << "[PASS] PinnedContinuationsPublishResultsAndRespectMainQueue\n";
		TestCachedResultsAndSchedulerLifetime();
		std::cout << "[PASS] CachedResultsAndSchedulerLifetime\n";
		TestJoinExpiredAndFinishedTasks();
		std::cout << "[PASS] JoinExpiredAndFinishedTasks\n";
		TestJoinBeyondSixteenBitFanIn();
		std::cout << "[PASS] JoinBeyondSixteenBitFanIn\n";
		return 0;
	}
	catch (const std::exception& error)
	{
		std::cerr << "[FAIL] " << error.what() << '\n';
		return 1;
	}
}
