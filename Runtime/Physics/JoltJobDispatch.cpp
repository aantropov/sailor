#include "Physics/JoltJobSystem.h"
#include "Tasks/Tasks.h"

using namespace Sailor;

// Keep std::function exception RTTI out of the adapter's no-RTTI translation unit.
void Physics::JoltJobSystem::QueueJob(JPH::JobSystem::Job* job)
{
	job->AddRef();
	if (!m_scheduler)
	{
		job->Execute();
		job->Release();
		return;
	}

	const auto completion = m_numQueuedTasks;
	completion->fetch_add(1, std::memory_order_relaxed);
	auto task = Tasks::CreateTask(
		*m_scheduler,
		"Jolt Physics",
		[completion, job]()
		{
			job->Execute();
			job->Release();
			// The owner may be destroyed as soon as the count reaches zero.
			completion->fetch_sub(1, std::memory_order_release);
			completion->notify_all();
		},
		EThreadType::Worker);
	m_scheduler->Run(task);
}
