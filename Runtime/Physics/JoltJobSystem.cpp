#include "Physics/JoltJobSystem.h"
#include "Tasks/Scheduler.h"
#include "Tasks/Tasks.h"
#include <Jolt/Physics/PhysicsSettings.h>
#include <algorithm>

using namespace Sailor;

Physics::JoltJobSystem::Job::Job(
	const char* name,
	JPH::ColorArg color,
	JPH::JobSystem* jobSystem,
	const JPH::JobSystem::JobFunction& jobFunction,
	JPH::uint32 dependencyCount) :
	JPH::JobSystem::Job(
		name,
		color,
		jobSystem,
		jobFunction,
		dependencyCount)
{}

Physics::JoltJobSystem::JoltJobSystem(Tasks::Scheduler* scheduler) :
	JPH::JobSystemWithBarrier(JPH::cMaxPhysicsBarriers),
	m_scheduler(scheduler)
{}

int Physics::JoltJobSystem::GetMaxConcurrency() const
{
	return m_scheduler
		? static_cast<int>(
			std::max(1u, m_scheduler->GetNumWorkerThreads()) + 1u)
		: 1;
}

JPH::JobHandle Physics::JoltJobSystem::CreateJob(
	const char* name,
	JPH::ColorArg color,
	const JPH::JobSystem::JobFunction& jobFunction,
	JPH::uint32 dependencyCount)
{
	auto* job = new Job(
		name,
		color,
		this,
		jobFunction,
		dependencyCount);
	JPH::JobHandle handle(job);
	if (dependencyCount == 0)
	{
		QueueJob(job);
	}
	return handle;
}

void Physics::JoltJobSystem::QueueJob(JPH::JobSystem::Job* job)
{
	job->AddRef();
	if (!m_scheduler)
	{
		job->Execute();
		job->Release();
		return;
	}

	m_numQueuedTasks.fetch_add(1, std::memory_order_relaxed);
	auto task = Tasks::CreateTask(
		"Jolt Physics",
		[this, job]()
		{
			job->Execute();
			job->Release();
			m_numQueuedTasks.fetch_sub(1, std::memory_order_release);
			m_numQueuedTasks.notify_all();
		},
		EThreadType::Worker);
	m_scheduler->Run(task);
}

void Physics::JoltJobSystem::QueueJobs(
	JPH::JobSystem::Job** jobs,
	JPH::uint jobCount)
{
	for (JPH::uint index = 0; index < jobCount; ++index)
	{
		QueueJob(jobs[index]);
	}
}

void Physics::JoltJobSystem::WaitForJobs(
	JPH::JobSystem::Barrier* barrier)
{
	JPH::JobSystemWithBarrier::WaitForJobs(barrier);

	uint32_t numQueuedTasks =
		m_numQueuedTasks.load(std::memory_order_acquire);
	while (numQueuedTasks != 0)
	{
		m_numQueuedTasks.wait(
			numQueuedTasks,
			std::memory_order_acquire);
		numQueuedTasks =
			m_numQueuedTasks.load(std::memory_order_acquire);
	}
}

void Physics::JoltJobSystem::FreeJob(JPH::JobSystem::Job* job)
{
	delete static_cast<Job*>(job);
}
