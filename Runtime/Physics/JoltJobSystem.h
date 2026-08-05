#pragma once
#include "Core/Defines.h"
#include <Jolt/Jolt.h>
#include <Jolt/Core/JobSystemWithBarrier.h>
#include <atomic>

namespace Sailor::Tasks
{
	class Scheduler;
}

namespace Sailor::Physics
{
	class JoltJobSystem final : public JPH::JobSystemWithBarrier
	{
		class Job final : public JPH::JobSystem::Job
		{
		public:
			Job(
				const char* name,
				JPH::ColorArg color,
				JPH::JobSystem* jobSystem,
				const JPH::JobSystem::JobFunction& jobFunction,
				JPH::uint32 dependencyCount);
		};

	public:
		explicit JoltJobSystem(Tasks::Scheduler* scheduler);

		int GetMaxConcurrency() const override;
		JPH::JobHandle CreateJob(
			const char* name,
			JPH::ColorArg color,
			const JPH::JobSystem::JobFunction& jobFunction,
			JPH::uint32 dependencyCount = 0) override;
		void QueueJob(JPH::JobSystem::Job* job) override;
		void QueueJobs(JPH::JobSystem::Job** jobs, JPH::uint jobCount) override;
		void WaitForJobs(JPH::JobSystem::Barrier* barrier) override;
		void FreeJob(JPH::JobSystem::Job* job) override;

	private:
		Tasks::Scheduler* m_scheduler = nullptr;
		std::atomic<uint32_t> m_numQueuedTasks = 0;
	};
}
