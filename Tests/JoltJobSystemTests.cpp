#include "Physics/JoltJobSystem.h"
#include "Memory/WeakPtr.hpp"
#include "Tasks/Tasks.h"
#include <Jolt/Core/Memory.h>

#include <array>
#include <atomic>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace Sailor::Physics
{
	class JoltJobSystemTestAccess
	{
	public:
		static TWeakPtr<std::atomic<uint32_t>> GetCompletion(const JoltJobSystem& jobs)
		{
			return jobs.m_numQueuedTasks;
		}
	};
}

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

	Tasks::ITaskPtr TakeWrapper(Tasks::Scheduler& scheduler)
	{
		Tasks::ITaskPtr task;
		Require(scheduler.TryFetchNextAvailiableTask(task, EThreadType::Worker),
			"Jolt must enqueue a real Sailor Worker task");
		return task;
	}

	uint32_t ReadCount(const TWeakPtr<std::atomic<uint32_t>>& completion)
	{
		auto count = completion.TryLock();
		Require(static_cast<bool>(count), "the completion state must still be owned");
		return count->load(std::memory_order_acquire);
	}

	void TestFinishedWrapperOutlivesJobSystem()
	{
		Tasks::Scheduler scheduler;
		scheduler.AttachCurrentThreadAsMainThread();
		Tasks::ITaskPtr wrapper;
		TWeakPtr<std::atomic<uint32_t>> completion;
		uint32_t calls = 0;
		{
			Physics::JoltJobSystem jobs(&scheduler);
			completion = Physics::JoltJobSystemTestAccess::GetCompletion(jobs);
			auto* barrier = jobs.CreateBarrier();
			auto handle = jobs.CreateJob("Retained wrapper", JPH::Color::sGreen, [&]() { ++calls; });
			barrier->AddJob(handle);
			wrapper = TakeWrapper(scheduler);
			Require(ReadCount(completion) == 1u, "the queued wrapper must hold one completion ticket");
			wrapper->Execute();
			jobs.WaitForJobs(barrier);
			jobs.DestroyBarrier(barrier);
			handle = {};
			Require(calls == 1u && wrapper->IsFinished() && ReadCount(completion) == 0u,
				"the wrapper must execute once and finish before owner teardown");
		}

		Require(ReadCount(completion) == 0u,
			"the retained production lambda must keep completion storage alive after owner destruction");
		wrapper.Clear();
		Require(!completion.TryLock(),
			"completion storage must be released with the last wrapper, not leaked after owner teardown");
	}

	struct JobPayload
	{
		JobPayload(TWeakPtr<std::atomic<uint32_t>> completion, uint32_t& releases, uint32_t& countAtRelease) :
			m_completion(std::move(completion)), m_releases(releases), m_countAtRelease(countAtRelease)
		{}

		~JobPayload()
		{
			if (auto count = m_completion.TryLock())
			{
				m_countAtRelease = count->load(std::memory_order_acquire);
			}
			++m_releases;
		}

		TWeakPtr<std::atomic<uint32_t>> m_completion;
		uint32_t& m_releases;
		uint32_t& m_countAtRelease;
	};

	void TestBarrierWinsAndWrapperReleasesJob()
	{
		Tasks::Scheduler scheduler;
		scheduler.AttachCurrentThreadAsMainThread();
		Physics::JoltJobSystem jobs(&scheduler);
		const auto completion = Physics::JoltJobSystemTestAccess::GetCompletion(jobs);
		uint32_t calls = 0;
		uint32_t releases = 0;
		uint32_t countAtRelease = (std::numeric_limits<uint32_t>::max)();
		auto payload = TSharedPtr<JobPayload>::Make(completion, releases, countAtRelease);
		TWeakPtr<JobPayload> retainedPayload = payload;
		auto* barrier = jobs.CreateBarrier();
		auto handle = jobs.CreateJob("Barrier wins", JPH::Color::sBlue, [payload, &calls]() { ++calls; });
		barrier->AddJob(handle);
		auto wrapper = TakeWrapper(scheduler);
		handle = {};
		payload.Clear();

		// Let the real Jolt barrier execute the job while its Sailor wrapper is still queued work.
		jobs.JPH::JobSystemWithBarrier::WaitForJobs(barrier);
		Require(calls == 1u && !wrapper->IsStarted() && ReadCount(completion) == 1u,
			"Jolt completion must not consume the unexecuted Sailor wrapper's ticket");
		Require(static_cast<bool>(retainedPayload.TryLock()) && releases == 0u,
			"the queued wrapper must retain the job after the barrier releases it");

		wrapper->Execute();
		jobs.WaitForJobs(barrier);
		jobs.DestroyBarrier(barrier);
		Require(calls == 1u && releases == 1u && !retainedPayload.TryLock(),
			"a wrapper whose Jolt job already finished must still release it without repeating the callback");
		Require(countAtRelease == 1u && ReadCount(completion) == 0u,
			"FreeJob and captured payload destruction must finish before the last completion ticket is released");
	}

	void TestDependentAndSpawnedJobs()
	{
		Tasks::Scheduler scheduler;
		scheduler.AttachCurrentThreadAsMainThread();
		Physics::JoltJobSystem jobs(&scheduler);
		const auto completion = Physics::JoltJobSystemTestAccess::GetCompletion(jobs);
		auto* barrier = jobs.CreateBarrier();
		std::array<uint32_t, 4> calls{};
		std::array<uint32_t, 3> values{};
		uint32_t input = 0;
		std::array<JPH::JobHandle, 2> dependents{
			jobs.CreateJob("First dependent", JPH::Color::sRed,
				[&]() { ++calls[1]; values[0] = input + 1u; }, 1u),
			jobs.CreateJob("Second dependent", JPH::Color::sRed,
				[&]() { ++calls[2]; values[1] = input + 2u; }, 1u)
		};
		barrier->AddJobs(dependents.data(), static_cast<JPH::uint>(dependents.size()));
		auto parent = jobs.CreateJob("Publish dependencies", JPH::Color::sGreen, [&, dependents]()
			{
				++calls[0];
				input = 40u;
				JPH::JobHandle::sRemoveDependencies(dependents.data(), static_cast<JPH::uint>(dependents.size()));
				auto spawned = jobs.CreateJob("Spawned during callback", JPH::Color::sBlue,
					[&]() { ++calls[3]; values[2] = input + 3u; });
				barrier->AddJob(spawned);
			});
		barrier->AddJob(parent);
		parent = {};
		dependents = {};

		auto parentWrapper = TakeWrapper(scheduler);
		parentWrapper->Execute();
		Require(ReadCount(completion) == 3u && scheduler.GetNumTasks(EThreadType::Worker) == 3u,
			"callbacks must register dependent and newly created wrappers before releasing the parent ticket");
		for (uint32_t index = 0; index < 3u; ++index)
		{
			auto wrapper = TakeWrapper(scheduler);
			wrapper->Execute();
		}
		jobs.WaitForJobs(barrier);
		jobs.DestroyBarrier(barrier);
		Require(calls == std::array<uint32_t, 4>{ 1u, 1u, 1u, 1u } &&
			values == std::array<uint32_t, 3>{ 41u, 42u, 43u },
			"every dependent callback must execute once and observe its parent's values");
		Require(ReadCount(completion) == 0u && scheduler.GetNumTasks(EThreadType::Worker) == 0u,
			"waiting must leave no queued completion tickets from the dynamic job graph");
	}

	void TestInlineJobs()
	{
		Physics::JoltJobSystem jobs(nullptr);
		const auto completion = Physics::JoltJobSystemTestAccess::GetCompletion(jobs);
		uint32_t calls = 0;
		auto dependent = jobs.CreateJob("Inline dependent", JPH::Color::sBlue, [&]() { ++calls; }, 1u);
		auto parent = jobs.CreateJob("Inline parent", JPH::Color::sGreen, [&, dependent]()
			{
				++calls;
				dependent.RemoveDependency();
			});
		Require(parent.IsDone() && dependent.IsDone() && calls == 2u && jobs.GetMaxConcurrency() == 1,
			"a null scheduler must execute ready jobs and dependencies synchronously");
		auto* barrier = jobs.CreateBarrier();
		barrier->AddJob(parent);
		barrier->AddJob(dependent);
		jobs.WaitForJobs(barrier);
		jobs.DestroyBarrier(barrier);
		parent = {};
		dependent = {};
		Require(ReadCount(completion) == 0u,
			"inline execution must not create or decrement queued-task tickets");
	}
}

int main()
{
	try
	{
		// This executable links Jolt directly; initialize its own core allocator, not the runtime DLL's physics module.
		JPH::RegisterDefaultAllocator();
		TestFinishedWrapperOutlivesJobSystem();
		TestBarrierWinsAndWrapperReleasesJob();
		TestDependentAndSpawnedJobs();
		TestInlineJobs();
		std::cout << "JoltJobSystemTests passed\n";
		return 0;
	}
	catch (const std::exception& error)
	{
		std::cerr << "JoltJobSystemTests failed: " << error.what() << '\n';
		return 1;
	}
}
